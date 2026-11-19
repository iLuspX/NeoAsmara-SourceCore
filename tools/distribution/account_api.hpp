#pragma once
#include "native/core.hpp"
#include <regex>
#include <set>
#include <algorithm>
namespace rosh {
struct AccountReply {int status{200};Json body;std::string cookie;};
class PublicAccountApi {
 fs::path root_;AccountStore accounts_;Sqlite gameplay_,roles_;Json config_;
 static fs::path required_database(const fs::path&root,const char*name){auto file=root/"Database"/name;if(!fs::is_regular_file(file))throw std::runtime_error("Initialize the complete database package before starting accounts.");return file;}
 static Json public_identity(const Identity&who){return {{"id",who.id},{"username",who.username},{"native_uid",who.native_uid}};}
 Identity session(const std::string&cookie){
  if(!std::regex_match(cookie,std::regex("[A-Za-z0-9_-]{43}")))throw std::runtime_error("Sign in to your account.");
  auto rows=accounts_.db().query("SELECT a.id,a.username,a.native_uid FROM login_sessions s JOIN login_accounts a ON a.id=s.account_id WHERE s.token_hash=? AND s.expires_at>?",{sha256(cookie),now()});
  if(rows.size()!=1)throw std::runtime_error("Your session has expired. Sign in again.");
  return {rows[0].at("id").get<std::string>(),rows[0].at("username").get<std::string>(),rows[0].at("native_uid").get<std::int64_t>()};
 }
 void throttle(const std::string&peer,bool registration){
  auto&db=accounts_.db();Transaction tx(db);auto key=hex(sha256("account-api:"+peer+(registration?":registration":":operation")));
  const auto stamp=now();auto rows=db.query("SELECT failures,until_at FROM login_throttle WHERE bucket=?",{key});
  const int limit=registration?10:60;
  if(!rows.empty()&&rows[0].at("until_at").get<double>()>stamp&&rows[0].at("failures").get<int>()>=limit)throw std::runtime_error("Too many requests. Try again in a minute.");
  db.exec("INSERT INTO login_throttle VALUES(?,1,?) ON CONFLICT(bucket) DO UPDATE SET failures=CASE WHEN until_at<=? THEN 1 ELSE failures+1 END,until_at=CASE WHEN until_at<=? THEN excluded.until_at ELSE until_at END",{key,stamp+60,stamp,stamp});tx.commit();
 }
 public:
 explicit PublicAccountApi(const fs::path&root):root_(fs::weakly_canonical(root)),accounts_(required_database(root_,"karos_login.sqlite")),gameplay_(required_database(root_,"karos_gameplay.sqlite")),roles_(required_database(root_,"karos_roles.sqlite")),config_(Json::parse(read_file(root_/"Config/server.json"))){
  gameplay_.exec("CREATE TABLE IF NOT EXISTS premium_wallet(account_id TEXT PRIMARY KEY,balance INTEGER NOT NULL CHECK(balance BETWEEN 0 AND 4294967295))");
 }
 bool native_ready(){
  try{
   Sqlite runtime(root_/"Database/runtime.sqlite",true);
   auto rows=runtime.query("SELECT pid,updated,payload FROM native_services WHERE instance='LoginServer'");
   if(rows.size()!=1||now()-rows[0].at("updated").get<double>()>5||now()<rows[0].at("updated").get<double>())return false;
   const auto state=Json::parse(rows[0].at("payload").get<std::string>());
   if(!state.value("running",false)||state.value("port",0)!=config_.at("login_port").get<int>())return false;
   auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,rows[0].at("pid").get<DWORD>());
   if(!process)return false;wchar_t executable[32768]{};DWORD size=32768;
   const bool found=QueryFullProcessImageNameW(process,0,executable,&size);CloseHandle(process);
   return found&&_wcsicmp(fs::weakly_canonical(executable).c_str(),fs::weakly_canonical(root_/"Bin/LoginServer.exe").c_str())==0;
  }catch(...){return false;}
 }
 AccountReply appearance(std::string name){
  name=std::regex_replace(name,std::regex("^\\[(ADMIN|GM|MOD|HELPER|ALPHA|BETA)\\]\\s*"),"");
  if(name.empty()||name.size()>140||std::any_of(name.begin(),name.end(),[](unsigned char c){return c<32;}))return {400,{{"error","Invalid character name."}}};
  Json result={{"name",name},{"title",nullptr}};
  auto exists=gameplay_.query("SELECT name FROM sqlite_master WHERE type='table' AND name IN ('accounts','character_titles','character_title_selection')");
  if(exists.size()!=3)return {200,result};
  Json selected;
  if(!gameplay_.query("SELECT name FROM sqlite_master WHERE type='table' AND name='native_public_character_names'").empty())
   selected=gameplay_.query("SELECT chosen.title_id FROM native_public_character_names n JOIN character_title_selection chosen ON chosen.character_eid=n.character_eid JOIN character_titles owned ON owned.character_eid=n.character_eid AND owned.title_id=chosen.title_id AND owned.account_id=n.account_id WHERE n.name=? LIMIT 2",{name});
  else selected=gameplay_.query("SELECT chosen.title_id FROM accounts a,json_each(a.payload,'$.chars') c JOIN character_title_selection chosen ON chosen.character_eid=json_extract(c.value,'$.eid') JOIN character_titles owned ON owned.character_eid=chosen.character_eid AND owned.title_id=chosen.title_id AND owned.account_id=a.id WHERE json_extract(c.value,'$.name')=? LIMIT 2",{name});
  if(selected.size()!=1)return {200,result};
  const auto catalog=root_/"Config/title-styles.json";if(!fs::is_regular_file(catalog))return {200,result};
  const auto styles=Json::parse(read_file(catalog,1024*1024));
  for(const auto&title:styles.at("titles"))if(title.at("id")==selected[0].at("title_id")){
   const std::map<std::string,int> themes={{"common",0},{"rare",1},{"special",2},{"epic",3},{"legendary",4},{"mythic",4},{"staff",5}};
   auto rarity=title.value("rarity","special");
   result["title"]={{"id",title.at("id")},{"label",title.at("name")},{"theme",title.value("staff_reserved",false)?5:(themes.count(rarity)?themes.at(rarity):2)}};break;
  }
  return {200,result};
 }
 AccountReply handle(const std::string&method,const std::string&path,const Json&data,const std::string&cookie,const std::string&peer){
  if(method=="GET"&&path=="/api/status")return {200,{{"service","karos-local-accounts"},{"core_backend","cpp"},{"instance_id",config_.value("instance_id","neoasmara-public")},{"native_game_login",native_ready()},{"launcher_login",true},{"ingame_password_login",false},{"api_only",true}}};
  if(method=="POST"&&(path=="/api/register"||path=="/api/login")){
   throttle(peer,path=="/api/register");const auto username=data.at("username").get<std::string>(),password=data.at("password").get<std::string>();
   if(path=="/api/register")return {201,{{"user",public_identity(accounts_.register_account(username,password))}}};
   auto user=accounts_.password(username,password,peer);auto value=token();auto&db=accounts_.db();Transaction tx(db);
   db.exec("DELETE FROM login_sessions WHERE expires_at<=?",{now()});
   db.exec("INSERT INTO login_sessions VALUES(?,?,?)",{sha256(value),user.id,now()+8*3600});tx.commit();
   return {200,{{"user",public_identity(user)}},"karos_session="+value+"; HttpOnly; SameSite=Strict; Path=/; Max-Age=28800"};
  }
  if(method=="POST"&&path=="/api/logout"){
   if(std::regex_match(cookie,std::regex("[A-Za-z0-9_-]{43}"))){auto&db=accounts_.db();Transaction tx(db);db.exec("DELETE FROM game_tickets WHERE session_hash=?",{sha256(cookie)});db.exec("DELETE FROM login_sessions WHERE token_hash=?",{sha256(cookie)});tx.commit();}
   return {200,{{"ok",true}},"karos_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0"};
  }
  if(path!="/api/me"&&path!="/api/launcher"&&path!="/api/game-session")return {404,{{"error","Unknown account endpoint."}}};
  Identity user;
  try{user=session(cookie);}catch(const std::exception&e){return {401,{{"error",e.what()}}};}
  if(method=="GET"&&path=="/api/me")return {200,{{"user",public_identity(user)}}};
  if(method=="POST"&&path=="/api/game-session"){
   if(!native_ready())return {409,{{"error","Start the native server before entering the game."}}};
   auto locale=data.value("locale",std::string("en"));if(locale!="pt"&&locale!="es"&&locale!="en")throw std::runtime_error("Unsupported client language.");
   auto value=token();auto hash=sha256(value);auto&db=accounts_.db();Transaction tx(db);session(cookie);
   db.exec("DELETE FROM game_tickets WHERE expires_at<=?",{now()});
   db.exec("INSERT INTO game_tickets(token_hash,account_id,expires_at,used,session_hash) VALUES(?,?,?,0,?)",{hash,user.id,now()+60,sha256(cookie)});
   db.exec("INSERT INTO game_ticket_locales VALUES(?,?)",{hash,locale});tx.commit();
   return {200,{{"username",user.username},{"launch_secret",value}}};
  }
  if(method=="GET"&&path=="/api/launcher"){
   Json characters=Json::array(),ranking=Json::array(),entries=Json::array();
   std::map<std::pair<std::string,std::uint32_t>,std::string> staff;
   for(const auto&row:roles_.query("SELECT account_id,character_eid,role FROM staff_roles"))staff[{row.at("account_id").get<std::string>(),row.at("character_eid").get<std::uint32_t>()}]=row.at("role").get<std::string>();
   for(const auto&row:gameplay_.query("SELECT id,payload FROM accounts")){
    const std::string account=row.at("id");auto saved=Json::parse(row.at("payload").get<std::string>());
    for(const auto&ch:saved.value("chars",Json::array())){
     const auto eid=ch.at("eid").get<std::uint32_t>();auto role=staff.count({account,eid})?staff.at({account,eid}):"Player";
     Json entry={{"name",ch.at("name")},{"level",ch.value("level",1)},{"cls",ch.at("cls")}};
     if(account==user.id){auto own=entry;own["eid"]=eid;own["role"]=role;characters.push_back(own);}
     auto lowered=role;std::transform(lowered.begin(),lowered.end(),lowered.begin(),[](unsigned char c){return char(std::tolower(c));});
     if(std::set<std::string>{"helper","ticket","mod","gm","admin","dev"}.count(lowered)==0){entry["exp"]=ch.value("exp",std::uint64_t(0));entries.push_back(entry);}
    }
   }
   std::sort(entries.begin(),entries.end(),[](const Json&a,const Json&b){if(a.at("level")!=b.at("level"))return a.at("level").get<int>()>b.at("level").get<int>();if(a.at("exp")!=b.at("exp"))return a.at("exp").get<std::uint64_t>()>b.at("exp").get<std::uint64_t>();return a.at("name").get<std::string>()<b.at("name").get<std::string>();});
   for(std::size_t i=0;i<std::min<std::size_t>(10,entries.size());++i){auto entry=entries[i];entry.erase("exp");entry["rank"]=i+1;ranking.push_back(entry);}
   auto wallet=gameplay_.query("SELECT balance FROM premium_wallet WHERE account_id=?",{user.id});
   Json news={{"news",Json::array()},{"events",Json::array()},{"announcement","Development preview — ROSH : Neo Asmara"}};
   if(fs::exists(root_/"Config/launcher-news.json"))news=Json::parse(read_file(root_/"Config/launcher-news.json",1024*1024));
   Json bosses={{"available",false},{"reason","World telemetry is not available yet."},{"entries",Json::array()}};
   try{auto state=Json::parse(read_file(root_/"Database/launcher_world_status.json",2*1024*1024));const double age=now()-state.at("updated_at").get<double>();if(state.at("schema")==1&&age>=0&&age<=20)bosses={{"available",true},{"entries",state.at("bosses")},{"maps",state.at("maps")},{"updated_at",state.at("updated_at")}};}catch(...){}
   return {200,{{"characters",characters},{"cash",wallet.empty()?Json(0):wallet[0].at("balance")},{"top10",ranking},{"news",news.value("news",Json::array())},{"events",news.value("events",Json::array())},{"announcement",news.value("announcement",std::string())},{"bosses",bosses}}};
  }
  return {405,{{"error","Method not allowed."}}};
 }
};
}
