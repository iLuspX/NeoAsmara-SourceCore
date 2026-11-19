#include "native/scene.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace rosh {
std::string native_actor_name(const Bytes&appearance){
 if(appearance.size()<254)return "";auto length=static_cast<std::size_t>(appearance[253]);if(254+length>appearance.size())return "";std::string name(appearance.begin()+254,appearance.begin()+254+length);
 for(auto prefix:{"[ALPHA] ","[BETA] ","[HELPER] ","[MOD] ","[GM] ","[ADMIN] "})if(name.rfind(prefix,0)==0)return name.substr(std::char_traits<char>::length(prefix));return name;
}
WorldScene::WorldScene(Content&content):catalog_(content){
 overlay_carrier_=unhex(content.get("server/stage_templates.json").at("game").at("020b"));if(overlay_carrier_.size()<14)throw std::runtime_error("Missing encounter overlay carrier");overlay_carrier_.resize(14);put32(overlay_carrier_,0,103,true);
 auto carriers=content.get("server/combat_templates.json");damage_carrier_=unhex(carriers.at("0502"));hp_carrier_=unhex(carriers.at("0806"));
 if(damage_carrier_.size()!=45||hp_carrier_.size()<13)throw std::runtime_error("Captured combat carriers invalid");
}
std::string WorldScene::instance_group(std::uint32_t eid){std::lock_guard lock(mutex_);auto party=party_members_.find(eid);return party==party_members_.end()?"solo:"+std::to_string(eid):"party:"+party->second;}
void WorldScene::reset_observer(const std::string&key){
 std::lock_guard lock(mutex_);
 auto observer=players_.find(key);
 if(observer!=players_.end())observer->second.observed.clear();
}
std::shared_ptr<const SceneEntities> WorldScene::map(const std::string &name){
 std::lock_guard lock(mutex_);auto &cached=maps_[name];
 if(!cached){
  // Player IDs start at10000. Captured actor IDs must not replace a player or
  // the separately allocated GM summons in the client's shared actor table.
  std::set<std::uint32_t> reserved;for(std::uint32_t id=10000;id<=65535;id++)reserved.insert(id);
  cached=std::make_shared<const SceneEntities>(catalog_.load(name,reserved));
 }
 return cached;
}
void WorldScene::activate_dungeon_actors(const std::string& name,const std::set<std::uint32_t>& references){
 apply_dungeon_actors(name,references,{});
}
void WorldScene::apply_dungeon_actors(const std::string& name,const std::set<std::uint32_t>& active,const std::set<std::uint32_t>& inactive){
 if(active.empty()&&inactive.empty())return;
 map(name);std::lock_guard lock(mutex_);
 auto changed=[&](const auto& entry){const auto reference=entry.second.rules.value("native_reference",0u);const bool dormant=entry.second.rules.value("dungeon_dormant",false);return (active.count(reference)&&dormant)||(inactive.count(reference)&&!dormant);};
 if(std::none_of(maps_.at(name)->begin(),maps_.at(name)->end(),changed))return;
 auto entities=std::make_shared<SceneEntities>(*maps_.at(name));
 for(auto& entry:*entities){
  auto& actor=entry.second;const auto reference=actor.rules.value("native_reference",0u);
  if(active.count(reference))actor.rules.erase("dungeon_dormant");
  if(inactive.count(reference)){
   actor.rules["dungeon_dormant"]=true;actor.target=actor.detected_target=0;actor.impact_at=actor.react_at=0;
   actor.wander.reset();actor.state=actor.alive?MobState::Idle:MobState::Dead;
  }
 }
 maps_[name]=std::move(entities);
}
std::string character_role(Sqlite &roles,const std::string&account,std::uint32_t eid){
 auto found=roles.query("SELECT role FROM staff_roles WHERE account_id=? AND character_eid=?",{account,std::int64_t(eid)});
 if(found.empty())return "";auto role=found[0].at("role").get<std::string>();if(role=="Dev")return "Admin";if(role=="Ticket")return "Helper";return role;
}
Json WorldScene::online(bool hidden){
 std::lock_guard lock(mutex_);Json result=Json::array();for(auto&[key,p]:players_)if(hidden||!p.hidden){auto name=native_actor_name(p.appearance);result.push_back({{"eid",p.eid},{"name",name},{"map",p.map},{"position",{p.position.x,p.position.y,p.position.z}},{"hidden",p.hidden}});}return result;
}
int WorldScene::recover_npcs(const std::string&name){
 map(name);std::lock_guard lock(mutex_);auto entities=std::make_shared<SceneEntities>(*maps_.at(name));int restored=0;for(auto&[id,e]:*entities)if(!e.combat&&!e.guard&&!e.alive){e.alive=true;e.hp=e.maxhp;e.state=MobState::Idle;e.target=0;e.impact_at=0;++restored;}maps_[name]=std::move(entities);return restored;
}
int WorldScene::respawn(const std::string&name,Sqlite&db,const std::string&scope,double timestamp){
 map(name);std::lock_guard lock(mutex_);auto entities=std::make_shared<SceneEntities>(*maps_.at(name));int count=0;Transaction tx(db);for(auto&[id,e]:*entities)if(e.combat&&!e.alive){e.respawn_at=timestamp;db.exec("UPDATE spawn_deaths SET respawn_at=? WHERE map_name=? AND spawn_index=? AND npc_id=?",{timestamp,scope,std::int64_t(e.spawn_index),std::int64_t(e.npc_id)});++count;}tx.commit();maps_[name]=std::move(entities);return count;
}
void WorldScene::summon(const std::string&name,std::uint32_t type,int count,const Position&position){
 if(count<1||count>10)throw std::runtime_error("Quantidade deve ser 1..10");map(name);std::lock_guard lock(mutex_);auto entities=std::make_shared<SceneEntities>(*maps_.at(name));std::uint32_t candidate=9999;for(int i=0;i<count;i++){while(candidate&&entities->count(candidate))--candidate;if(!candidate)throw std::runtime_error("Capacidade de entidades atingida");Position point{position.x+30+i*8,position.y+30,position.z};if(point.x>=100000||point.y>=100000)throw std::runtime_error("Posicao de invocacao fora do mapa");auto e=catalog_.summoned(type,candidate,name,point);entities->emplace(candidate,std::move(e));--candidate;}maps_[name]=std::move(entities);
}
std::string displayed_name(const std::string&name,const std::string&role,std::size_t limit){
 static const std::map<std::string,std::string> prefixes={{"Alpha","[ALPHA]"},{"Beta","[BETA]"},{"Helper","[HELPER]"},{"Mod","[MOD]"},{"GM","[GM]"},{"Admin","[ADMIN]"}};
 auto found=prefixes.find(role);auto prefix=found==prefixes.end()?std::string():found->second+" ";auto value=wide(name);std::size_t used=std::min(value.size(),limit>prefix.size()?limit-prefix.size():0);
 // Never cut a UTF16 surrogate pair while applying Python's render-only limit.
 if(used&&used<value.size()&&value[used-1]>=0xd800&&value[used-1]<=0xdbff)--used;
 return prefix+utf8(value.substr(0,used));
}
void WorldScene::update_player(const std::string&key,std::uint32_t eid,const std::string&map,const Json&position,const Bytes&appearance,const Bytes&movement,bool hidden,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send,const Json&combat){
 if(appearance.size()<254||movement.size()<30)throw std::runtime_error("Invalid native player visibility carrier");
 struct Notification{std::function<void(unsigned char,unsigned char,const Bytes&)> send;int cat,sub;Bytes body;};std::vector<Notification> messages;
 {
  std::lock_guard lock(mutex_);auto& current=players_[key];if(current.map!=map)current.reflected.clear();current.eid=eid;current.map=map;current.position={position[0],position[1],position[2]};current.appearance=appearance;current.movement=movement;current.hidden=hidden;current.send=send;
  if(!combat.empty()){current.combat=combat;current.hp=std::max(0,combat.value("hp",0)-current.pending_damage);}
  for(auto& [observerKey,observer]:players_){
   std::set<std::uint32_t> nearby;
   for(auto& [otherKey,other]:players_){
    if(observerKey==otherKey||observer.map!=other.map||other.hidden||observer.position.distance(other.position)>3000)continue;nearby.insert(other.eid);
    Bytes signature(other.appearance.begin()+68,other.appearance.end());auto prior=observer.observed.find(other.eid);
    if(prior==observer.observed.end()||prior->second.first!=signature){
     messages.push_back({observer.send,4,2,other.appearance});
     auto flags=other.combat.value("cash_visibility",Json::array({0,0}));if(flags.size()==2){Bytes visibility(6);put32(visibility,0,other.eid,true);visibility[4]=flags[0].get<unsigned char>();visibility[5]=flags[1].get<unsigned char>();messages.push_back({observer.send,6,0x46,visibility});}
     if(other.combat.value("mounted",false)&&other.combat.contains("mount_action")){
      auto ride=unhex(other.combat.at("mount_action"));if(ride.size()==36){messages.push_back({observer.send,5,3,ride});Bytes complete(25);std::copy(ride.begin(),ride.begin()+9,complete.begin());std::copy(ride.begin()+13,ride.begin()+29,complete.begin()+9);messages.push_back({observer.send,5,4,std::move(complete)});Bytes mode(11);put32(mode,0,other.eid,true);mode[4]=2;put32(mode,6,other.eid,true);messages.push_back({observer.send,3,2,std::move(mode)});}
     }
     if(other.combat.value("sitting",false)&&!other.combat.value("mounted",false)){
      Bytes pose(11);put32(pose,0,other.eid,true);pose[4]=1;messages.push_back({observer.send,3,2,std::move(pose)});
     }
    }
    else if(prior->second.second.distance(other.position)>0||prior->second.second.z!=other.position.z){auto body=other.movement;if(body.size()<30)throw std::runtime_error("Missing native movement template");body[0]=0;body[1]=0;put32(body,2,other.eid,true);const auto&from=prior->second.second;for(int i=0;i<3;i++){double before=i==0?from.x:i==1?from.y:from.z;double after=i==0?other.position.x:i==1?other.position.y:other.position.z;putfloat(body,6+i*4,before);putfloat(body,18+i*4,after);}messages.push_back({observer.send,4,1,std::move(body)});}
    observer.observed[other.eid]={std::move(signature),other.position};
   }
   for(auto it=observer.observed.begin();it!=observer.observed.end();)if(!nearby.count(it->first)){Bytes body(5);put32(body,1,it->first,true);messages.push_back({observer.send,4,3,std::move(body)});it=observer.observed.erase(it);}else ++it;
  }
 }
 // Match staff_runtime.Visibility: never hold the registry lock during socket IO.
 for(auto&message:messages)try{message.send(message.cat,message.sub,message.body);}catch(const std::exception&){}
}
void WorldScene::action(const std::string&key,unsigned char cat,unsigned char sub,const Bytes&body){
 std::vector<std::function<void(unsigned char,unsigned char,const Bytes&)>> listeners;
 {std::lock_guard lock(mutex_);auto actor=players_.find(key);if(actor==players_.end()||actor->second.hidden)return;for(auto&[otherKey,other]:players_)if(otherKey!=key&&other.map==actor->second.map&&other.observed.count(actor->second.eid))listeners.push_back(other.send);}
 for(auto&send:listeners)try{send(cat,sub,body);}catch(const std::exception&){}
}
void WorldScene::remove_player(const std::string&key){
 std::vector<std::function<void(unsigned char,unsigned char,const Bytes&)>> listeners;std::uint32_t eid{};
 {std::lock_guard lock(mutex_);auto it=players_.find(key);if(it==players_.end())return;eid=it->second.eid;players_.erase(it);for(auto&[otherKey,other]:players_)if(other.observed.erase(eid))listeners.push_back(other.send);}
 Bytes body(5);put32(body,1,eid,true);for(auto&listener:listeners)try{listener(4,3,body);}catch(const std::exception&){}
}
void WorldScene::chat(const std::string&key,const std::string&name,const std::string&message,std::uint32_t channel){
 std::vector<std::function<void(unsigned char,unsigned char,const Bytes&)>> listeners;std::uint32_t eid{};
 {std::lock_guard lock(mutex_);auto it=players_.find(key);if(it==players_.end())return;eid=it->second.eid;
  if(channel==3)throw std::runtime_error("Guild chat requires authenticated membership routing");
  const auto party=party_members_.find(eid);
  if(channel==2&&party==party_members_.end())throw std::runtime_error("You are not in a party");
  for(auto &[otherKey,other]:players_){
   if(channel==2){auto member=party_members_.find(other.eid);if(member!=party_members_.end()&&member->second==party->second)listeners.push_back(other.send);}
   else if(other.map==it->second.map)listeners.push_back(other.send);
  }
 }
 Bytes body(9);put32(body,0,eid,true);put32(body,4,channel,true);body.insert(body.end(),name.begin(),name.end());body.push_back(0);body.insert(body.end(),message.begin(),message.end());body.push_back(0);
 for(auto&listener:listeners)try{listener(3,0,body);}catch(const std::exception&){}
}
void scene_visibility(WorldScene&scene,const SceneEntities&entities,const Json&position,std::set<std::uint32_t>&visible,
 const std::function<void(unsigned char,unsigned char,const Bytes&)>&send){
 Position player{position.at(0),position.at(1),position.at(2)};
 for(auto it=visible.begin();it!=visible.end();){auto found=entities.find(*it);if(found==entities.end()||!found->second.alive||found->second.rules.value("dungeon_dormant",false)||(!found->second.rules.value("dev_always_visible",false)&&found->second.position.distance(player)>3500)){Bytes b(5,1);put32(b,1,*it,true);send(4,3,b);it=visible.erase(it);}else ++it;}
 std::vector<const Entity*> candidates;for(auto &[id,e]:entities)if(e.alive&&!e.rules.value("dungeon_dormant",false)&&!visible.count(id)&&(e.rules.value("dev_always_visible",false)||e.position.distance(player)<=3000))candidates.push_back(&e);
 std::sort(candidates.begin(),candidates.end(),[&](auto a,auto b){if(a->combat!=b->combat)return !a->combat;return a->position.distance(player)<b->position.distance(player);});
 int count=0;for(auto e:candidates){send(4,2,scene.appearance(*e));visible.insert(e->eid);if(++count>=40)break;}
}
static std::uint64_t get64be(const Bytes&bytes,std::size_t at){if(at+8>bytes.size())throw std::runtime_error("Truncated quantity");std::uint64_t n=0;for(int i=0;i<8;i++)n=(n<<8)|bytes[at+i];return n;}
NpcShops::NpcShops(Sqlite &db,Content&content,Inventory &inventory):db_(db),content_(content),typed_(content.database("database/content_typed.sqlite")),rules_(content.get("data/native_item_rules.json")),inventory_(inventory),wallet_(db,inventory){
 db_.exec("CREATE TABLE IF NOT EXISTS npc_shop_operations(event_key TEXT PRIMARY KEY,eid INTEGER NOT NULL,kind TEXT NOT NULL,request BLOB NOT NULL,response BLOB NOT NULL)");
}
Bytes NpcShops::transact(const SceneEntities&entities,const Json &character,int sub,const Bytes&body,const std::string&event){
 auto eid=character.at("eid").get<std::uint32_t>();bool buy=sub==13;
 if((sub!=13&&sub!=14)||body.size()!=(buy?22:30)||get32(body,0,true)!=eid||get16(body,buy?12:20,true)!=(buy?9:45)||event.empty()||event.size()>128)throw std::runtime_error("Invalid NPC shop request");
 auto npc_id=get32(body,buy?4:16,true),iid=get32(body,buy?8:12,true);auto npc=entities.find(npc_id);
 auto p=character.at("pos");Position position{p[0],p[1],p[2]};
 if(character.value("hp",0)<=0||npc==entities.end()||!npc->second.alive||npc->second.position.distance(position)>350)throw std::runtime_error("NPC indisponivel ou distante demais");
 auto shops=typed_->query("SELECT o8_i,o12_f64,o20_f64 FROM npcShop WHERE o0_i=?",{std::int64_t(npc->second.npc_id)});
 Json authored;
 if (content_.has("server/devtool_map_rules.json")) {
  const auto config = content_.get("server/devtool_map_rules.json");
  authored = config.value("shops", Json::object()).value(std::to_string(npc->second.npc_id), Json());
  if (!authored.is_null()) shops = Json::array({Json{{"o8_i",1},{"o12_f64",authored.at("buy_percent")},{"o20_f64",authored.at("sell_percent")}}});
 }
 if(shops.empty()||shops[0].value("o8_i",0)==0)throw std::runtime_error("Este NPC nao oferece loja de itens");
 auto rule=rules_.value(std::to_string(iid),Json::object());if(rule.empty()||iid==999999)throw std::runtime_error("Item nao pode ser negociado");
 auto kind=buy?std::string("buy"):std::string("sell");Transaction tx(db_);
 auto prior=db_.query("SELECT eid,kind,request,response FROM npc_shop_operations WHERE event_key=?",{event});
 if(!prior.empty()){
  if(prior[0].at("eid")!=eid||prior[0].at("kind")!=kind||unhex(prior[0].at("request"))!=body)throw std::runtime_error("NPC replay changed its request");
  auto reply=unhex(prior[0].at("response"));tx.commit();return reply;
 }
 if(!db_.query("SELECT event_key FROM wallet_events WHERE event_key=?",{event}).empty())throw std::runtime_error("NPC event already processed without detailed receipt");
 Bytes reply=body;std::uint64_t quantity=get64be(body,buy?14:22);
 if(buy){
  if(quantity==std::numeric_limits<std::uint64_t>::max())quantity=1;
  if(quantity<1||quantity>10000)throw std::runtime_error("Quantidade de compra invalida");
  if(authored.is_null() && typed_->query("SELECT o0_i FROM npcShopItem WHERE o0_i=? AND o4_i=? LIMIT 1",{shops[0].at("o8_i").get<std::int64_t>(),std::int64_t(iid)}).empty())throw std::runtime_error("Item nao vendido por este NPC");
  if (!authored.is_null()) {
   const auto& goods = authored.at("items");
   if (std::find(goods.begin(),goods.end(),Json(iid))==goods.end())
    throw std::runtime_error("Item nao vendido neste catalogo de NPC");
  }
  double price=std::ceil(rule.value("buy",0.)*shops[0].at("o12_f64").get<double>()/100.);
  if(!std::isfinite(price)||price<0||price>1.e12)throw std::runtime_error("Preco de compra invalido");
  wallet_.apply(eid,-static_cast<std::int64_t>(price)*static_cast<std::int64_t>(quantity),event);
  int duration=rule.value("duration",0);
  if(duration){
   // Match Python timed_grant: each timed unit owns its own UID/expiration.
   inventory_.grant_timed_once(eid,iid,quantity,duration,event+":item");
  }else inventory_.grant_once(eid,{{iid,static_cast<int>(quantity)}},event+":item");
  put16(reply,12,44,true);
 }else{
  auto category=get32(body,4,true),slot=get32(body,8,true);auto item=inventory_.owned(eid,Inventory::key(category,slot),iid,now());
  if(quantity==std::numeric_limits<std::uint64_t>::max())quantity=item.quantity;
  if(!quantity||quantity>static_cast<std::uint64_t>(item.quantity)||rule.value("sell",0.)<=0)throw std::runtime_error("Quantidade ou item invalido para venda");
  double price=std::floor(rule.value("sell",0.)*shops[0].at("o20_f64").get<double>()/100.);
  if(!std::isfinite(price)||price<0||price>1.e12||quantity>10000)throw std::runtime_error("Preco ou quantidade de venda invalida");
  inventory_.consume(eid,item,quantity,"npc-sale");wallet_.apply(eid,static_cast<std::int64_t>(price)*quantity,event);
 }
 db_.exec("INSERT INTO npc_shop_operations VALUES(?,?,?,?,?)",{event,std::int64_t(eid),kind,body,reply});tx.commit();return reply;
}
}
