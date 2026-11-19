#include "native/quests.hpp"
#include "native/economy.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstring>
#include <limits>
namespace rosh {
QuestBook::QuestBook(Sqlite&db,Inventory&inventory,Content&content,std::uint32_t eid)
 :db_(db),inventory_(inventory),catalog_(content.get("data/quest_catalog.json")),policy_(content.get("server/quest_policy.json")),eid_(eid){
 db_.exec("CREATE TABLE IF NOT EXISTS quest_state(player_eid INTEGER,quest_id INTEGER,progress INTEGER NOT NULL DEFAULT 0,completed INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(player_eid,quest_id));"
 "CREATE TABLE IF NOT EXISTS quest_objectives(player_eid INTEGER,quest_id INTEGER,objective INTEGER,value INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(player_eid,quest_id,objective));"
 "CREATE TABLE IF NOT EXISTS quest_cycles(player_eid INTEGER NOT NULL,quest_id INTEGER NOT NULL,cycle INTEGER NOT NULL,PRIMARY KEY(player_eid,quest_id));"
 "CREATE TABLE IF NOT EXISTS quest_completions(player_eid INTEGER NOT NULL,quest_id INTEGER NOT NULL,cycle INTEGER NOT NULL,completed_at REAL NOT NULL,local_window TEXT NOT NULL,PRIMARY KEY(player_eid,quest_id,cycle));"
 "CREATE TABLE IF NOT EXISTS quest_wire_operations(event_key TEXT PRIMARY KEY,player_eid INTEGER NOT NULL,request BLOB NOT NULL);"
 "CREATE TABLE IF NOT EXISTS native_quest_rewards(player_eid INTEGER NOT NULL,quest_id INTEGER NOT NULL,cycle INTEGER NOT NULL,event_key TEXT NOT NULL UNIQUE,PRIMARY KEY(player_eid,quest_id,cycle))");
 Transaction tx(db_);db_.exec("INSERT OR IGNORE INTO quest_cycles SELECT player_eid,quest_id,0 FROM quest_state");
 db_.exec("INSERT OR IGNORE INTO quest_completions SELECT q.player_eid,q.quest_id,c.cycle,?,? FROM quest_state q JOIN quest_cycles c ON c.player_eid=q.player_eid AND c.quest_id=q.quest_id WHERE q.completed=1",{now(),local_window(now(),policy_.at("reset_local_hour"))});tx.commit();
}
std::string QuestBook::local_window(double timestamp,int reset_hour){
 if(!std::isfinite(timestamp)||reset_hour<0||reset_hour>23)throw std::runtime_error("Invalid quest reset clock");
 auto stamp=static_cast<std::time_t>(timestamp);std::tm local{};if(localtime_s(&local,&stamp))throw std::runtime_error("Cannot read host local date");
 if(local.tm_hour<reset_hour){local.tm_mday--;local.tm_hour=12;local.tm_isdst=-1;if(std::mktime(&local)==-1)throw std::runtime_error("Invalid host quest date");}
 char result[16];if(!std::strftime(result,sizeof(result),"%Y-%m-%d",&local))throw std::runtime_error("Quest date overflow");return result;
}
const Json&QuestBook::quest(int id)const{auto key=std::to_string(id);if(!catalog_.contains(key)||!catalog_[key].value("supported",false))throw std::runtime_error("Missao sem objetivos ou recompensas suportados nos dados nativos");return catalog_[key];}
std::string QuestBook::mode(int id)const{
 auto key=std::to_string(id);auto q=catalog_.value(key,Json::object());auto types=policy_.at("native_type_modes");auto result=q.value("recurrence",types.value(std::to_string(q.value("native_type",0)),std::string("unique")));
 result=policy_.value("quest_overrides",Json::object()).value(key,result);
 if(result!="unique"&&result!="repeatable"&&result!="daily")throw std::runtime_error("Invalid quest recurrence");return result;
}
bool QuestBook::ever_completed(int qid)const{return !db_.query("SELECT 1 FROM quest_completions WHERE player_eid=? AND quest_id=? UNION ALL SELECT 1 FROM quest_state WHERE player_eid=? AND quest_id=? AND completed=1 LIMIT 1",{std::int64_t(eid_),std::int64_t(qid),std::int64_t(eid_),std::int64_t(qid)}).empty();}
bool QuestBook::active(int qid)const{return !db_.query("SELECT 1 FROM quest_state WHERE player_eid=? AND quest_id=? AND completed=0",{std::int64_t(eid_),std::int64_t(qid)}).empty();}
bool QuestBook::blocked(int qid,double stamp)const{
 auto recurrence=mode(qid);if(recurrence=="repeatable")return false;if(recurrence=="unique")return ever_completed(qid);
 auto rows=db_.query("SELECT MAX(local_window) AS window FROM quest_completions WHERE player_eid=? AND quest_id=?",{std::int64_t(eid_),std::int64_t(qid)});
 return !rows[0].at("window").is_null()&&local_window(stamp,policy_.at("reset_local_hour"))<=rows[0].at("window").get<std::string>();
}
int QuestBook::cycle(int qid)const{auto r=db_.query("SELECT cycle FROM quest_cycles WHERE player_eid=? AND quest_id=?",{std::int64_t(eid_),std::int64_t(qid)});return r.empty()?0:r[0].at("cycle").get<int>();}
std::vector<QuestBook::Goal>QuestBook::groups(const Json&q)const{
 std::vector<Goal> result;int index=0;for(const auto&o:q.at("objectives")){
  auto raw=o.at("raw");if(raw.size()!=13||index>=20)throw std::runtime_error("Invalid native quest objective layout");int kind=o.at("kind"),group=raw[8],required=o.at("required");if(required<1)throw std::runtime_error("Invalid quest goal quantity");
  auto it=std::find_if(result.begin(),result.end(),[&](auto&g){return g.kind==kind&&g.group==group;});if(it==result.end()){result.push_back({kind,group,required,{index}});}else{it->required=std::max(it->required,required);it->indexes.push_back(index);}index++;
 }return result;
}
std::map<int,int>QuestBook::values(int qid)const{std::map<int,int>out;for(auto&r:db_.query("SELECT objective,value FROM quest_objectives WHERE player_eid=? AND quest_id=?",{std::int64_t(eid_),std::int64_t(qid)}))out[r.at("objective")]=r.at("value");return out;}
std::int64_t QuestBook::owned(int item,double stamp)const{return db_.query("SELECT COALESCE(SUM(i.quantity),0) AS count FROM inventory_slots i LEFT JOIN reward_instances r ON r.uid=i.instance_uid WHERE i.player_eid=? AND i.item_id=? AND (r.uid IS NULL OR (r.player_eid=i.player_eid AND r.item_id=i.item_id AND r.expires_at>?))",{std::int64_t(eid_),std::int64_t(item),stamp})[0].at("count");}
void QuestBook::aggregate(int qid){db_.exec("UPDATE quest_state SET progress=(SELECT COALESCE(SUM(value),0) FROM quest_objectives WHERE player_eid=? AND quest_id=?) WHERE player_eid=? AND quest_id=?",{std::int64_t(eid_),std::int64_t(qid),std::int64_t(eid_),std::int64_t(qid)});}
int QuestBook::validate(const Bytes&body,std::size_t length)const{if(body.size()!=length||get32(body,0,true)!=eid_||get16(body,length-2,true)!=0x51)throw std::runtime_error("Pedido de missao invalido");return get32(body,4,true);}
void QuestBook::npc(const SceneEntities&entities,const Json&character,std::uint32_t actor,int type)const{
 auto it=entities.find(actor);auto p=character.at("pos");if(character.value("hp",0)<=0||it==entities.end()||!it->second.alive||it->second.npc_id!=static_cast<unsigned>(type)||it->second.position.distance({p[0],p[1],p[2]})>350)throw std::runtime_error("NPC da missao indisponivel ou distante");
}
bool QuestBook::seen(const std::string&event,const Bytes&body){if(event.empty()||event.size()>200)throw std::runtime_error("Identificador de missao invalido");auto r=db_.query("SELECT player_eid,request FROM quest_wire_operations WHERE event_key=?",{event});if(r.empty())return false;if(r[0].at("player_eid")!=eid_||r[0].at("request")!=hex(body))throw std::runtime_error("Repeticao da missao alterou o pedido");return true;}
void QuestBook::remember(const std::string&event,const Bytes&body){db_.exec("INSERT INTO quest_wire_operations VALUES(?,?,?)",{event,std::int64_t(eid_),body});}
bool QuestBook::accept(const Bytes&body,const Json&character,const SceneEntities&entities,const std::string&event,double stamp){
 auto id=validate(body,14);auto&q=quest(id);npc(entities,character,get32(body,8,true),q.at("start_npc"));groups(q);int level=character.at("level");if(level<q.at("min_level").get<int>()||level>q.at("max_level").get<int>())throw std::runtime_error("Nivel insuficiente para esta missao");
 Transaction tx(db_);if(seen(event,body)){tx.commit();return false;}if(active(id)){remember(event,body);tx.commit();return false;}
 if(q.at("prerequisite")!=0&&!ever_completed(q.at("prerequisite")))throw std::runtime_error("Conclua primeiro a missao anterior");if(blocked(id,stamp))throw std::runtime_error(mode(id)=="daily"?"Missao diaria reinicia as 04:00 do servidor":"Missao unica ja concluida");
 if(db_.query("SELECT COUNT(*) AS count FROM quest_state WHERE player_eid=? AND completed=0",{std::int64_t(eid_)})[0].at("count").get<int>()>=20)throw std::runtime_error("Limite de 20 missoes ativas");
 auto previous=db_.query("SELECT cycle FROM quest_cycles WHERE player_eid=? AND quest_id=?",{std::int64_t(eid_),std::int64_t(id)});int next=previous.empty()?0:previous[0].at("cycle").get<int>()+1;
 db_.exec("INSERT INTO quest_cycles VALUES(?,?,?) ON CONFLICT(player_eid,quest_id) DO UPDATE SET cycle=excluded.cycle",{std::int64_t(eid_),std::int64_t(id),std::int64_t(next)});
 db_.exec("INSERT INTO quest_state VALUES(?,?,0,0) ON CONFLICT(player_eid,quest_id) DO UPDATE SET progress=0,completed=0",{std::int64_t(eid_),std::int64_t(id)});db_.exec("DELETE FROM quest_objectives WHERE player_eid=? AND quest_id=?",{std::int64_t(eid_),std::int64_t(id)});
 for(std::size_t i=0;i<q.at("objectives").size();i++)db_.exec("INSERT INTO quest_objectives VALUES(?,?,?,0)",{std::int64_t(eid_),std::int64_t(id),std::int64_t(i)});
 if(q.value("delivery_on_accept",false)){std::vector<std::pair<std::uint32_t,int>>letters;for(auto&g:groups(q))if(g.kind==9)letters.push_back({q.at("objectives")[g.indexes[0]].at("target"),g.required});if(!letters.empty())inventory_.grant_once(eid_,letters,"quest-delivery:"+std::to_string(eid_)+":"+std::to_string(id)+":"+std::to_string(next));}
 synchronize(stamp);remember(event,body);tx.commit();return true;
}
bool QuestBook::talk(const Bytes&body,const Json&character,const SceneEntities&entities){
 auto id=validate(body,22);int kind=get32(body,8,true),target=get32(body,12,true),claimed=get32(body,16,true);if(kind==1||kind==3||kind==9||(kind==8&&claimed==0))return false;
 auto&q=quest(id);if(kind!=8)throw std::runtime_error("Objetivo nao pertence a missao");bool nearby=false;for(auto&[actor,n]:entities)if(n.npc_id==static_cast<unsigned>(target)&&n.alive){try{npc(entities,character,actor,target);nearby=true;break;}catch(const std::exception&){}}
 if(!nearby)throw std::runtime_error("NPC do objetivo indisponivel ou distante");Transaction tx(db_);if(!active(id)){tx.commit();return false;}bool found=false;
 for(std::size_t i=0;i<q.at("objectives").size();i++){auto o=q.at("objectives")[i];if(o.at("kind")==8&&o.at("target")==target){db_.exec("UPDATE quest_objectives SET value=? WHERE player_eid=? AND quest_id=? AND objective=?",{o.at("required").get<std::int64_t>(),std::int64_t(eid_),std::int64_t(id),std::int64_t(i)});found=true;}}
 if(!found)throw std::runtime_error("Conversa nao pertence a missao");aggregate(id);tx.commit();return true;
}
void QuestBook::synchronize(double stamp){
 for(auto&r:db_.query("SELECT quest_id FROM quest_state WHERE player_eid=? AND completed=0",{std::int64_t(eid_)})){
  int id=r.at("quest_id");auto key=std::to_string(id);if(!catalog_.contains(key)||!catalog_[key].value("supported",false))continue;auto&q=catalog_[key];std::map<int,std::vector<std::pair<int,int>>>requirements;
  for(auto&g:groups(q))if(g.kind==3||g.kind==9){int item=q.at("objectives")[g.indexes[0]].at("target");requirements[item].push_back({g.group,g.required});}
  for(auto&[item,goals]:requirements){auto remaining=owned(item,stamp);std::map<int,int>allocation;for(auto&[group,required]:goals){allocation[group]=static_cast<int>(std::min<std::int64_t>(remaining,required));remaining-=allocation[group];}
   for(std::size_t i=0;i<q.at("objectives").size();i++){auto o=q.at("objectives")[i];if((o.at("kind")!=3&&o.at("kind")!=9)||o.at("target")!=item)continue;int value=std::min(allocation[o.at("raw")[8]],o.at("required").get<int>());db_.exec("INSERT INTO quest_objectives VALUES(?,?,?,?) ON CONFLICT(player_eid,quest_id,objective) DO UPDATE SET value=excluded.value",{std::int64_t(eid_),std::int64_t(id),std::int64_t(i),std::int64_t(value)});}
  }aggregate(id);
 }
}
void QuestBook::kill(std::uint32_t type){
 for(auto&r:db_.query("SELECT quest_id FROM quest_state WHERE player_eid=? AND completed=0",{std::int64_t(eid_)})){
  int id=r.at("quest_id");auto key=std::to_string(id);if(!catalog_.contains(key)||!catalog_[key].value("supported",false))continue;auto&q=catalog_[key];auto v=values(id);
  for(auto&g:groups(q))if(g.kind==1){int total=0;for(int index:g.indexes)total+=v[index];if(total>=g.required)continue;
   for(int index:g.indexes){auto o=q.at("objectives")[index];if(o.at("target")==type&&v[index]<o.at("required").get<int>()){v[index]++;db_.exec("UPDATE quest_objectives SET value=? WHERE player_eid=? AND quest_id=? AND objective=?",{std::int64_t(v[index]),std::int64_t(eid_),std::int64_t(id),std::int64_t(index)});break;}}
  }aggregate(id);
 }
}
std::vector<std::pair<std::uint32_t,std::int64_t>>QuestBook::drops(std::uint32_t type,double stamp){
 std::map<std::uint32_t,double>chances;
 const auto groundRules=policy_.value("observed_ground_drops",Json::object());
 for(auto&r:db_.query("SELECT quest_id FROM quest_state WHERE player_eid=? AND completed=0",{std::int64_t(eid_)})){
  auto key=std::to_string(r.at("quest_id").get<int>());if(!catalog_.contains(key)||!catalog_[key].value("supported",false))continue;auto&q=catalog_[key];std::map<int,int>required;
 for(auto&g:groups(q))if(g.kind==3)required[q.at("objectives")[g.indexes[0]].at("target").get<int>()]+=g.required;
  for(auto&[item,count]:required){auto pending=db_.query("SELECT COALESCE(SUM(quantity),0) AS count FROM ground_items WHERE owner_eid=? AND item_id=? AND picked_at IS NULL AND expires_at>?",{std::int64_t(eid_),std::int64_t(item),stamp})[0].at("count").get<std::int64_t>();if(owned(item,stamp)+pending>=count)continue;
   const auto groundRule=groundRules.find(key);
   if(groundRule!=groundRules.end()&&groundRule->at("item")==item){
    const auto &sources=groundRule->at("npc_ids");
    if(std::find(sources.begin(),sources.end(),type)!=sources.end())
     chances[item]=std::max(chances[item],std::clamp(groundRule->at("chance_bp").get<double>(),0.,10000.));
   }
   for(auto&o:q.at("objectives"))if(o.at("kind")==3&&o.at("target")==item&&o.at("raw")[4]==type){std::uint64_t bits=o.at("raw")[5].get<std::uint32_t>()|(std::uint64_t(o.at("raw")[6].get<std::uint32_t>())<<32);double rate;std::memcpy(&rate,&bits,8);if(std::isfinite(rate)&&rate>=0&&rate<=10000)chances[item]=std::max(chances[item],rate);}
  }
 }
 std::vector<std::pair<std::uint32_t,std::int64_t>>out;for(auto&[item,chance]:chances){auto random=random_bytes(4);if(get32(random,0)/4294967296.*10000<chance)out.push_back({item,1});}return out;
}
QuestKillItems QuestBook::route_kill_items(
 const std::vector<std::pair<std::uint32_t,std::int64_t>>& rolled,const std::string& kill){
 QuestKillItems result;
 const auto groundRules=policy_.value("observed_ground_drops",Json::object());
 for(const auto& [id,quantity]:rolled){
  const bool observedOnGround=std::any_of(groundRules.begin(),groundRules.end(),[id](const auto &rule){
   return rule.at("item")==id;
  });
  if(observedOnGround||inventory_.definition(id).value("kind",0)!=4){
   result.ground.emplace_back(id,quantity);
   continue;
  }
  if(quantity<1||quantity>10000)throw std::runtime_error("Quantidade de item de missao invalida");
  try{
   if(inventory_.grant_once(eid_,{{id,static_cast<int>(quantity)}},"quest-kill:"+kill+":"+std::to_string(id)))
    result.delivered.emplace_back(id,quantity);
  }catch(const InventoryFull&){result.inventory_full=true;}
 }
 return result;
}
Json QuestBook::complete(const Bytes&body,const Json&character,const SceneEntities&entities,const std::string&event,double stamp){
 int id=validate(body,18);auto&q=quest(id);npc(entities,character,get32(body,8,true),q.at("end_npc"));std::uint32_t choice=get32(body,12,true);bool has_choice=false,valid_choice=false;
 for(auto&r:q.at("rewards"))if(r.value("choice",false)){has_choice=true;if(r.at("index")==choice)valid_choice=true;}
 if((has_choice&&!valid_choice)||(!has_choice&&choice!=0xffffffffu))throw std::runtime_error("Selecione uma recompensa valida da missao");
 Transaction tx(db_);if(seen(event,body)){tx.commit();return {{"replay",true},{"xp",0}};}
 if(!active(id)){if(ever_completed(id)){remember(event,body);tx.commit();return {{"replay",true},{"xp",0}};}throw std::runtime_error("Missao nao esta ativa");}
 auto reward="quest-reward-v2:"+std::to_string(eid_)+":"+std::to_string(id)+":"+std::to_string(cycle(id));
 bool rewarded=!db_.query("SELECT 1 FROM native_quest_rewards WHERE player_eid=? AND quest_id=? AND cycle=?",{std::int64_t(eid_),std::int64_t(id),std::int64_t(cycle(id))}).empty();
 if(!rewarded&&!db_.query("SELECT name FROM sqlite_master WHERE type='table' AND name='inventory_grants'").empty()){
  auto legacy=cycle(id)==0?"quest-reward-v1:"+std::to_string(eid_)+":"+std::to_string(id):reward;
  rewarded=!db_.query("SELECT 1 FROM inventory_grants WHERE event_key=?",{legacy}).empty();
 }
 if(rewarded){db_.exec("UPDATE quest_state SET completed=1 WHERE player_eid=? AND quest_id=?",{std::int64_t(eid_),std::int64_t(id)});db_.exec("INSERT OR IGNORE INTO quest_completions VALUES(?,?,?,?,?)",{std::int64_t(eid_),std::int64_t(id),std::int64_t(cycle(id)),stamp,local_window(stamp,policy_.at("reset_local_hour"))});remember(event,body);tx.commit();return {{"replay",true},{"xp",0}};}
 synchronize(stamp);auto v=values(id);for(auto&g:groups(q)){int have=0;for(int index:g.indexes)have=g.kind==1?have+v[index]:std::max(have,v[index]);if(have<g.required)throw std::runtime_error("Objetivos da missao incompletos");}
 std::map<int,int>required;for(auto&g:groups(q))if(g.kind==3||g.kind==9)required[q.at("objectives")[g.indexes[0]].at("target").get<int>()]+=g.required;
 for(auto&[item,need]:required){if(owned(item,stamp)<need)throw std::runtime_error("Itens do objetivo ausentes");for(auto&i:inventory_.items(eid_))if(i.item==static_cast<unsigned>(item)&&(!i.expires||*i.expires>stamp)&&need>0){auto use=std::min<std::int64_t>(need,i.quantity);inventory_.consume(eid_,i,use,"quest-turn-in");need-=static_cast<int>(use);}if(need)throw std::runtime_error("Quantidade do objetivo mudou");}
 std::uint64_t xp=0;std::int64_t carats=0;std::vector<std::pair<std::uint32_t,int>>items;
 for(auto&r:q.at("rewards")){if(r.value("choice",false)&&r.at("index")!=choice)continue;int kind=r.at("kind");auto amount=r.at("item").get<std::int64_t>();if(amount<0)throw std::runtime_error("Invalid native quest reward");if(kind==1)xp+=amount;else if(kind==2)carats+=amount;else if(kind==3)items.push_back({static_cast<std::uint32_t>(amount),r.at("quantity")});else throw std::runtime_error("Recompensa sem suporte");}
 if(!items.empty()&&!inventory_.grant_once(eid_,items,reward))throw std::runtime_error("Recompensa da missao ja registrada");if(carats)Wallet(db_,inventory_).apply(eid_,carats,reward+":carats");
 db_.exec("INSERT INTO native_quest_rewards VALUES(?,?,?,?)",{std::int64_t(eid_),std::int64_t(id),std::int64_t(cycle(id)),reward});
 db_.exec("UPDATE quest_state SET completed=1 WHERE player_eid=? AND quest_id=?",{std::int64_t(eid_),std::int64_t(id)});
 db_.exec("INSERT OR IGNORE INTO quest_completions VALUES(?,?,?,?,?)",{std::int64_t(eid_),std::int64_t(id),std::int64_t(cycle(id)),stamp,local_window(stamp,policy_.at("reset_local_hour"))});remember(event,body);tx.commit();return {{"replay",false},{"xp",xp}};
}
bool QuestBook::abandon(int id){Transaction tx(db_);if(!active(id)){tx.commit();return false;}db_.exec("DELETE FROM quest_state WHERE player_eid=? AND quest_id=? AND completed=0",{std::int64_t(eid_),std::int64_t(id)});db_.exec("DELETE FROM quest_objectives WHERE player_eid=? AND quest_id=?",{std::int64_t(eid_),std::int64_t(id)});tx.commit();return true;}
Bytes QuestBook::list_packet()const{
 Bytes out(6);put32(out,0,eid_,true);std::uint16_t count=0;
 for(auto&r:db_.query("SELECT quest_id FROM quest_state WHERE player_eid=? AND completed=0 ORDER BY quest_id",{std::int64_t(eid_)})){int id=r.at("quest_id");auto key=std::to_string(id);if(!catalog_.contains(key))continue;auto&q=catalog_[key];groups(q);auto v=values(id);Bytes record(0x414);put32(record,0,id);int i=0;for(auto&o:q.at("objectives")){auto raw=o.at("raw");raw[2]=v[i];for(int field=0;field<13;field++)put32(record,4+i*52+field*4,raw[field]);i++;}out.insert(out.end(),record.begin(),record.end());count++;}
 put16(out,4,count,true);return out;
}
Bytes QuestBook::completed_packet(double stamp)const{
 std::set<int>ids;for(auto&r:db_.query("SELECT DISTINCT quest_id FROM quest_completions WHERE player_eid=? ORDER BY quest_id",{std::int64_t(eid_)}))if(blocked(r.at("quest_id"),stamp))ids.insert(r.at("quest_id").get<int>());if(ids.size()>65535)throw std::runtime_error("Quest history exceeds native format");Bytes out(8+ids.size()*4);put32(out,0,eid_,true);put16(out,6,static_cast<std::uint16_t>(ids.size()),true);int offset=8;for(int id:ids){put32(out,offset,id);offset+=4;}return out;
}
std::vector<Bytes>QuestBook::progress_updates(){
 std::vector<Bytes>out;std::map<std::pair<int,int>,int>current;
 for(auto&r:db_.query("SELECT quest_id,objective,value FROM quest_objectives WHERE player_eid=? AND EXISTS(SELECT 1 FROM quest_state q WHERE q.player_eid=quest_objectives.player_eid AND q.quest_id=quest_objectives.quest_id AND q.completed=0)",{std::int64_t(eid_)})){
  int id=r.at("quest_id"),index=r.at("objective"),value=r.at("value");auto key=std::make_pair(id,index);auto q=catalog_.find(std::to_string(id));if(q==catalog_.end()||index<0||index>=static_cast<int>(q->at("objectives").size()))continue;current[key]=value;
  if(!sent_.count(key)||sent_[key]!=value){auto o=q->at("objectives")[index];Bytes b(22);put32(b,0,eid_,true);put32(b,4,id,true);put32(b,8,o.at("kind"),true);put32(b,12,o.at("target"),true);put32(b,16,value,true);put16(b,20,0x51,true);out.push_back(std::move(b));}
 }sent_=std::move(current);return out;
}
Json QuestBook::ui_state(int level,bool full,double stamp)const{
 Json out={{"cmd",full?"quests":"quests.held"},{"level",level},{"active",Json::array()}};
 for(auto&r:db_.query("SELECT quest_id FROM quest_state WHERE player_eid=? AND completed=0 ORDER BY quest_id",{std::int64_t(eid_)})){int id=r.at("quest_id");auto key=std::to_string(id);if(!catalog_.contains(key))continue;auto v=values(id);Json goals=Json::array();bool ready=true;for(auto&g:groups(catalog_[key])){int have=0;for(int i:g.indexes)have=g.kind==1?have+v[i]:std::max(have,v[i]);have=std::min(have,g.required);goals.push_back(have);ready&=have>=g.required;}out["active"].push_back({{"id",id},{"ready",ready},{"goals",goals}});}
 if(full){std::set<int>available;for(auto q=catalog_.begin();q!=catalog_.end();++q){int id=std::stoi(q.key());if(!q->value("supported",false)||active(id)||level<q->at("min_level").get<int>()||level>q->at("max_level").get<int>())continue;int prior=q->at("prerequisite");if(prior&&!ever_completed(prior))continue;if(!blocked(id,stamp))available.insert(id);}out["available"]=Json::array();for(int id:available)out["available"].push_back(id);}
 return out;
}
}
