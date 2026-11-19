#include "native/core.hpp"
#include "release_content_key.hpp"
#include <iostream>
using namespace rosh;
int wmain(int argc,wchar_t**argv){try{
 fs::path root=fs::absolute(argv[0]).parent_path().parent_path(),databasePackage;
 bool verifyOnly=false;
 for(int i=1;i<argc;++i){std::wstring option=argv[i];if(option==L"--root"&&i+1<argc)root=fs::absolute(argv[++i]);else if(option==L"--database-package"&&i+1<argc)databasePackage=fs::absolute(argv[++i]);else if(option==L"--verify")verifyOnly=true;else throw std::runtime_error("Unknown setup option");}
 if(databasePackage.empty())databasePackage=root/"Bootstrap/Database";
 if(!fs::is_regular_file(root/"Config/server.json")||!fs::is_regular_file(root/"Data/catalog.dat"))throw std::runtime_error("Select the complete server folder with --root");
 const auto databaseManifest=Json::parse(read_file(root/"Config/database-manifest.json"));
 if(databaseManifest.at("backend")!="sqlite"||!databaseManifest.at("freshDatabases").get<bool>())throw std::runtime_error("Invalid clean database package");
 if(!verifyOnly){
  Bytes key(std::begin(releaseContentKey),std::end(releaseContentKey));
  // Authenticate edited catalogs with the release key before touching setup state.
  // This also permits a DevTool-authored catalog on another Windows account.
  const auto releaseManifest=Json::parse(decrypt_content(read_file(root/"Data/catalog.dat"),key,"catalog"));
  for(const auto&row:releaseManifest.at("resources")){
   const auto file=fs::u8path(row.at("file").get<std::string>());
   if(file.empty()||file!=file.filename()||hex(sha256(read_file(root/"Data"/file)))!=row.at("sha256"))
    throw std::runtime_error("Release content authentication failed. Existing databases and keys were preserved.");
  }
  const auto keyPath=root/"Config/Keys/content.key.dpapi";bool current=false;
  if(fs::exists(keyPath)){
   Bytes existing;bool readable=false;
   try{existing=unprotect_key(keyPath);readable=true;}catch(const std::exception&){}
   if(readable){
    if(!secure_equal(existing,key))throw std::runtime_error("This folder uses another content key. Existing installations are preserved.");
    current=true;
   }
  }
  for(const auto& entry:databaseManifest.at("databases")){
   const fs::path name=fs::u8path(entry.at("database").get<std::string>());
   if(name!=name.filename()||name.extension()!=L".sqlite")throw std::runtime_error("Invalid database package filename");
   const auto source=databasePackage/name,target=root/"Database"/name;
   if(!fs::exists(target)){
    if(databasePackage.empty())throw std::runtime_error("Missing runtime database. Use --database-package to restore missing files.");
    if(hex(sha256(read_file(source)))!=entry.at("sha256"))throw std::runtime_error("Clean database package hash mismatch");
    fs::create_directories(target.parent_path());fs::copy_file(source,target);std::cout<<"Created "<<name.string()<<'\n';
   }
   else std::cout<<"Preserved existing "<<name.string()<<'\n';
  }
  if(!current){
   protect_key(keyPath,key);
  }
  fs::create_directories(root/"Logs");fs::create_directories(root/"Dumps");
 }
 Content content(root);const auto resources=content.status().at("resources");
 for(const auto& [name,row]:resources.items()){
  if(row.value("format","")=="sqlite-image"){
   auto db=content.database(name);if(db->query("PRAGMA quick_check")[0].begin().value()!="ok")throw std::runtime_error("Encrypted reference database verification failed");
  }else content.get(name);
 }
 for(const auto& entry:databaseManifest.at("databases")){
  Sqlite db(root/"Database"/fs::u8path(entry.at("database").get<std::string>()),true);
  if(db.query("PRAGMA quick_check")[0].begin().value()!="ok")throw std::runtime_error("Runtime database verification failed");
 }
 std::cout<<"PASS: content and SQLite databases verified. Setup never resets existing accounts.\n";return 0;
}catch(const std::exception&e){std::cerr<<"Setup: "<<e.what()<<'\n';return 1;}}
