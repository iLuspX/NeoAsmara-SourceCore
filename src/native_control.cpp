#include "native/control.hpp"
#include "native/runtime.hpp"
#include <cmath>
namespace rosh {
NativeControl::NativeControl(const fs::path&root):root_(root),db_(root/"Database/runtime.sqlite"){
 db_.exec("CREATE TABLE IF NOT EXISTS native_player_leases(eid INTEGER PRIMARY KEY,session TEXT NOT NULL,account TEXT NOT NULL,channel INTEGER NOT NULL,expires REAL NOT NULL)");
 db_.exec("CREATE TABLE IF NOT EXISTS native_save_requests(group_id TEXT NOT NULL,session TEXT NOT NULL,actor_account TEXT NOT NULL,actor_eid INTEGER NOT NULL,generation TEXT NOT NULL,status TEXT NOT NULL,error TEXT,PRIMARY KEY(group_id,session));CREATE TABLE IF NOT EXISTS native_lifecycle(id INTEGER PRIMARY KEY CHECK(id=1),action TEXT NOT NULL,due REAL NOT NULL,delay INTEGER NOT NULL,announced TEXT NOT NULL,actor_account TEXT NOT NULL,actor_eid INTEGER NOT NULL,generation TEXT NOT NULL,status TEXT NOT NULL,save_group TEXT,save_started REAL)");
}
bool NativeControl::authorized(const Json&row){
 Sqlite roles(root_/"Database/karos_roles.sqlite",true);auto result=roles.query("SELECT role,auth_generation FROM staff_roles WHERE account_id=? AND character_eid=?",{row.at("actor_account").get<std::string>(),row.at("actor_eid").get<std::int64_t>()});return !result.empty()&&(result[0].at("role")=="Admin"||result[0].at("role")=="Dev")&&result[0].at("auth_generation")==row.at("generation");
}
void NativeControl::announce(const std::string&text){JobQueue jobs(root_);for(auto&service:jobs.services())if(service.value("alive",false)&&service.value("role","")=="WorldServer")jobs.enqueue(service.at("instance"),"announce",{{"text",text}});}
int NativeControl::save_all(const std::string&group,const std::string&account,std::uint32_t eid,const std::string&generation,const std::string&except){
 Json actor={{"actor_account",account},{"actor_eid",eid},{"generation",generation}};if(!authorized(actor))throw std::runtime_error("Autorizacao Admin revogada");int count=0;Transaction tx(db_);for(auto&row:db_.query("SELECT session FROM native_player_leases WHERE expires>?",{now()})){auto session=row.at("session").get<std::string>();if(session==except)continue;db_.exec("INSERT OR IGNORE INTO native_save_requests(group_id,session,actor_account,actor_eid,generation,status) VALUES(?,?,?,?,?,'pending')",{group,session,account,std::int64_t(eid),generation});++count;}tx.commit();return count;
}
void NativeControl::poll_save(const std::string&session,const std::function<void()>&save){
 for(auto&row:db_.query("SELECT * FROM native_save_requests WHERE session=? AND status='pending'",{session})){
  try{if(!authorized(row))throw std::runtime_error("Cargo revogado antes do save");save();db_.exec("UPDATE native_save_requests SET status='done',error=NULL WHERE group_id=? AND session=?",{row.at("group_id").get<std::string>(),session});}
  catch(const std::exception&error){db_.exec("UPDATE native_save_requests SET status='failed',error=? WHERE group_id=? AND session=?",{std::string(error.what()),row.at("group_id").get<std::string>(),session});}
 }
}
void NativeControl::schedule(const std::string&action,int delay,const std::string&account,std::uint32_t eid,const std::string&generation){
 if((action!="shutdown"&&action!="restart"&&action!="reset")||delay<0||delay>3600)throw std::runtime_error("Use restart/reset/shutdown com 0..3600 segundos");Json actor={{"actor_account",account},{"actor_eid",eid},{"generation",generation}};if(!authorized(actor))throw std::runtime_error("Autorizacao Admin revogada");db_.exec("INSERT INTO native_lifecycle VALUES(1,?,?,?,'[]',?,?,?,'scheduled',NULL,NULL) ON CONFLICT(id) DO UPDATE SET action=excluded.action,due=excluded.due,delay=excluded.delay,announced='[]',actor_account=excluded.actor_account,actor_eid=excluded.actor_eid,generation=excluded.generation,status='scheduled',save_group=NULL,save_started=NULL",{action,now()+delay,std::int64_t(delay),account,std::int64_t(eid),generation});announce("Servidor: "+action+" em "+std::to_string(delay)+" segundos.");
}
bool NativeControl::cancel(){db_.exec("UPDATE native_lifecycle SET status='cancelled' WHERE id=1 AND status IN ('scheduled','saving')");bool changed=db_.changes()!=0;if(changed)announce("Contagem regressiva cancelada.");return changed;}
std::optional<std::string> NativeControl::poll(double timestamp){
 auto rows=db_.query("SELECT * FROM native_lifecycle WHERE id=1 AND status IN ('scheduled','saving')");if(rows.empty())return {};auto row=rows[0];if(!authorized(row)){cancel();return {};}
 auto action=row.at("action").get<std::string>();
 if(row.at("status")=="scheduled"){
  auto remaining=std::max(0,static_cast<int>(std::ceil(row.at("due").get<double>()-timestamp)));auto announced=Json::parse(row.at("announced").get<std::string>());bool spoken=false;
  for(int threshold:{60,30,10,5,4,3,2,1})if(remaining>0&&remaining<=threshold&&threshold<=row.at("delay").get<int>()&&std::find(announced.begin(),announced.end(),threshold)==announced.end()){announced.push_back(threshold);if(!spoken){announce("Servidor: "+action+" em "+std::to_string(remaining)+" segundos.");spoken=true;}}
  db_.exec("UPDATE native_lifecycle SET announced=? WHERE id=1",{announced.dump()});if(remaining>0)return {};
  auto group="lifecycle:"+token(20);save_all(group,row.at("actor_account"),row.at("actor_eid"),row.at("generation"));db_.exec("UPDATE native_lifecycle SET status='saving',save_group=?,save_started=? WHERE id=1",{group,timestamp});return {};
 }
 auto group=row.at("save_group").get<std::string>();auto failures=db_.query("SELECT session FROM native_save_requests WHERE group_id=? AND status='failed'",{group});auto pending=db_.query("SELECT r.session FROM native_save_requests r JOIN native_player_leases p ON p.session=r.session WHERE r.group_id=? AND r.status='pending' AND p.expires>?",{group,timestamp});
 if(!failures.empty()||(!pending.empty()&&timestamp-row.at("save_started").get<double>()>15)){
  db_.exec("UPDATE native_lifecycle SET status='scheduled',due=?,delay=30,announced='[]',save_group=NULL,save_started=NULL WHERE id=1",{timestamp+30});announce("Operacao adiada: save de personagem falhou ou nao respondeu. Nova tentativa em 30s.");return {};
 }
 if(!pending.empty())return {};
 Sqlite game(root_/"Database/karos_gameplay.sqlite");auto checkpoint=game.query("PRAGMA wal_checkpoint(FULL)");if(!checkpoint.empty()&&checkpoint[0].value("busy",0)!=0)return {};
 db_.exec("UPDATE native_lifecycle SET status='executed' WHERE id=1 AND status='saving'");if(db_.changes()!=1)return {};announce("Servidor: "+action+" agora. Personagens salvos.");return action;
}
}
