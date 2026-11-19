#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include "nlohmann/json.hpp"
namespace fs=std::filesystem;
using Json=nlohmann::json;
struct Process {DWORD id;fs::path executable;};
std::wstring image(DWORD pid){
 auto handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!handle)return {};
 wchar_t path[32768];DWORD length=32768;bool ok=QueryFullProcessImageNameW(handle,0,path,&length);CloseHandle(handle);
 return ok?std::wstring(path,length):std::wstring();
}
bool same(const fs::path&a,const fs::path&b){return _wcsicmp(a.lexically_normal().c_str(),b.lexically_normal().c_str())==0;}
std::vector<Process> owned(const fs::path&root){
 std::vector<Process> result;auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
 if(snapshot==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot inspect running processes");
 PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
 if(Process32FirstW(snapshot,&entry))do{
  std::wstring name=entry.szExeFile;
  if(name!=L"MainServer.exe"&&name!=L"LoginServer.exe"&&name!=L"LobbyServer.exe"&&name!=L"WorldServer.exe"&&name!=L"RoshAccountService.exe")continue;
  auto path=image(entry.th32ProcessID);if(!path.empty()&&same(fs::path(path).parent_path(),root/L"Bin"))result.push_back({entry.th32ProcessID,path});
 }while(Process32NextW(snapshot,&entry));
 CloseHandle(snapshot);return result;
}
void check_port(int port,const fs::path&expected){
 ULONG bytes=0;auto code=GetExtendedTcpTable(nullptr,&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_LISTENER,0);
 if(code!=ERROR_INSUFFICIENT_BUFFER)throw std::runtime_error("Cannot inspect TCP listeners");
 std::vector<unsigned char> buffer(bytes);
 if(GetExtendedTcpTable(buffer.data(),&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_LISTENER,0)!=NO_ERROR)throw std::runtime_error("Cannot inspect TCP listeners");
 auto table=reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
 for(DWORD i=0;i<table->dwNumEntries;++i)if(ntohs(static_cast<u_short>(table->table[i].dwLocalPort))==port){
  auto path=image(table->table[i].dwOwningPid);
  if(path.empty()||!same(path,expected))throw std::runtime_error("Configured port belongs to another process. Nothing was stopped.");
 }
}
DWORD launch(const fs::path&exe,const fs::path&root,const std::wstring&args,bool hidden,bool wait=false){
 if(!fs::is_regular_file(exe))throw std::runtime_error("Required release executable is missing");
 auto command=L"\""+exe.wstring()+L"\" "+args;
 STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=hidden?SW_HIDE:SW_SHOWNORMAL;
 PROCESS_INFORMATION process{};
 if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,hidden?CREATE_NO_WINDOW:0,nullptr,root.c_str(),&startup,&process))
  throw std::runtime_error("Cannot start the release executable");
 CloseHandle(process.hThread);const auto pid=process.dwProcessId;
 if(wait){auto waited=WaitForSingleObject(process.hProcess,30000);DWORD code=1;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);if(waited!=WAIT_OBJECT_0||code)throw std::runtime_error("Initialization failed. Run INITIALIZE.cmd for details.");}
 else CloseHandle(process.hProcess);
 return pid;
}
bool ready(int port,const std::string&instance){
 auto session=WinHttpOpen(L"NeoAsmaraSetup/1",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
 if(!session)return false;WinHttpSetTimeouts(session,500,500,500,500);
 auto connection=WinHttpConnect(session,L"127.0.0.1",static_cast<INTERNET_PORT>(port),0);
 auto request=connection?WinHttpOpenRequest(connection,L"GET",L"/api/status",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0):nullptr;bool result=false;
 try{
  if(request&&WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(request,nullptr)){
   DWORD status=0,size=sizeof(status);WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
   if(status==200){std::string body;char buffer[1024];DWORD count=0;while(WinHttpReadData(request,buffer,sizeof(buffer),&count)&&count){body.append(buffer,count);if(body.size()>4096)throw std::runtime_error("Invalid account status");}
    const auto data=Json::parse(body);result=data.value("service","")=="karos-local-accounts"&&data.value("instance_id","")==instance&&data.value("core_backend","")=="cpp"&&data.value("native_game_login",false);}
  }
 }catch(...){}
 if(request)WinHttpCloseHandle(request);if(connection)WinHttpCloseHandle(connection);WinHttpCloseHandle(session);return result;
}
int wmain(int argc,wchar_t**argv){try{
 wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
 fs::path root=fs::weakly_canonical(fs::path(executable).parent_path().parent_path());
 std::wstring action=L"status";
 for(int i=1;i<argc;++i){std::wstring option=argv[i];if(option==L"start"||option==L"stop"||option==L"status")action=option;else throw std::runtime_error("Use ServerControl.exe start | stop | status");}
 auto config=Json::parse(std::ifstream(root/L"Config/server.json"));
 const int nativePort=config.at("login_port"),accountPort=config.at("account_port");
 if(nativePort<1024||nativePort>65535||accountPort<1024||accountPort>65535||nativePort==accountPort)throw std::runtime_error("Invalid server ports");
 auto processes=owned(root);
 if(action==L"start"){
  check_port(nativePort,root/L"Bin/LoginServer.exe");check_port(accountPort,root/L"Bin/RoshAccountService.exe");
  launch(root/L"Bin/Initialize.exe",root,L"",true,true);
  bool main=false,accounts=false,other=false;
  for(const auto&p:processes){const auto name=p.executable.filename();main|=name==L"MainServer.exe";accounts|=name==L"RoshAccountService.exe";other|=name!=L"MainServer.exe"&&name!=L"RoshAccountService.exe";}
  if(!main&&other)throw std::runtime_error("A partial cluster is already running in this folder. Stop it normally before restarting.");
  const auto args=L"--root \""+root.wstring()+L"\"";
  if(!accounts)launch(root/L"Bin/RoshAccountService.exe",root,args,true);
  if(!main)launch(root/L"Bin/MainServer.exe",root,args+L" --cluster",false);
  bool complete=false;
  for(int attempt=0;attempt<60;++attempt){
   const auto current=owned(root);int worlds=0;for(const auto&p:current)if(p.executable.filename()==L"WorldServer.exe")++worlds;
   if(current.size()==8&&worlds==4&&ready(accountPort,config.value("instance_id","neoasmara-public"))){complete=true;break;}
   Sleep(250);
  }
  if(!complete)throw std::runtime_error("Startup is incomplete. Inspect the visible MainServer panel and this installation's Logs folder.");
  std::cout<<"READY: account API, LoginServer, LobbyServer, MainServer and four world channels. MainServer is visible.\n";
 }else if(action==L"stop"){
  // Named events request checkpoints and normal socket shutdown; never force-kill.
  for(const auto&p:processes){
   auto prefix=p.executable.filename()==L"RoshAccountService.exe"?L"Local\\RoshAccountsStop_":L"Local\\RoshNativeStop_";
   auto event=OpenEventW(EVENT_MODIFY_STATE,FALSE,(prefix+std::to_wstring(p.id)).c_str());
   if(event){SetEvent(event);CloseHandle(event);}
  }
  const auto until=GetTickCount64()+20000;
  while(!owned(root).empty()&&GetTickCount64()<until)Sleep(100);
  if(!owned(root).empty())throw std::runtime_error("Some services are still shutting down. Nothing was force-killed.");
  std::cout<<"This installation stopped normally.\n";
 }else{
  Json rows=Json::array();for(const auto&p:processes)rows.push_back({{"pid",p.id},{"service",p.executable.filename().u8string()}});
  std::cout<<Json({{"running",rows},{"login_port",nativePort},{"account_port",accountPort}}).dump(2)<<'\n';
 }
 return 0;
}catch(const std::exception&e){std::cerr<<"Server control: "<<e.what()<<'\n';return 1;}}
