#include "native/runtime.hpp"
#include <iostream>
#include <thread>
#include <algorithm>
using namespace rosh;
// Shorten only this executable's busy timeout to exercise recovery deterministically.
extern "C" int sqlite3_busy_timeout(sqlite3*db,int timeout){
 static auto real=reinterpret_cast<int(*)(sqlite3*,int)>(GetProcAddress(LoadLibraryW(L"winsqlite3.dll"),"sqlite3_busy_timeout"));
 return real(db,timeout>100?100:timeout);
}
int wmain(int argc,wchar_t**argv){
 fs::path fixture=fs::absolute(L"build")/(L"service-contention-"+std::to_wstring(GetCurrentProcessId()));
 try{
  if(argc!=2)throw std::runtime_error("Provide an initialized release Core as the read-only fixture source");
  const fs::path source=fs::weakly_canonical(argv[1]);
  if(fs::exists(fixture)||fixture.parent_path()!=fs::absolute(L"build"))throw std::runtime_error("Unsafe fixture location");
  fs::create_directories(fixture/L"Config/Keys");fs::create_directories(fixture/L"Database");
  fs::copy(source/L"Data",fixture/L"Data",fs::copy_options::recursive);
  fs::copy_file(source/L"Config/Keys/content.key.dpapi",fixture/L"Config/Keys/content.key.dpapi");
  fs::copy_file(source/L"Config/server.json",fixture/L"Config/server.json");
  for(const auto&entry:fs::directory_iterator(source/L"Bootstrap/Database"))
   if(entry.is_regular_file()&&entry.path().extension()==L".sqlite")fs::copy_file(entry.path(),fixture/L"Database"/entry.path().filename());
  {
   NativeService service(fixture,"LobbyServer");service.start();
   Sqlite writer(fixture/L"Database/runtime.sqlite");
   Sqlite reader(fixture/L"Database/runtime.sqlite",true);
   const auto before=now();
   {
    Transaction lock(writer);
    writer.exec("UPDATE native_services SET updated=updated WHERE instance='LobbyServer'");
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));
    lock.commit();
   }
   if(!service.running())throw std::runtime_error("The service worker died after a temporary database lock");
   bool resumed=false;
   for(int attempt=0;attempt<60;++attempt){
    auto rows=reader.query("SELECT updated,payload FROM native_services WHERE instance='LobbyServer'");
    if(rows.size()==1&&rows[0].at("updated").get<double>()>before+1){
     const auto state=Json::parse(rows[0].at("payload").get<std::string>());
     resumed=state.at("running").get<bool>();if(resumed)break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
   }
   if(!resumed)throw std::runtime_error("The worker failed to resume its heartbeat after the writer released its lock");
   const auto log=service.log_tail();
   if(std::none_of(log.begin(),log.end(),[](const std::string&line){return line.find("sqlite-retry")!=std::string::npos;}))
    throw std::runtime_error("The test did not actually exercise the contention recovery path");
   service.stop();
  }
  if(fixture.parent_path()!=fs::absolute(L"build"))throw std::runtime_error("Fixture cleanup refused");
  fs::remove_all(fixture);
  std::cout<<"PASS: injected SQLite contention is observed, the worker resumes its heartbeat and stops normally\n";return 0;
 }catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\nFixture: "<<fixture.u8string()<<'\n';return 1;}
}
