#include "native/game.hpp"
#include "native/economy.hpp"
#include <algorithm>
#include <cmath>
namespace rosh {
static const char *descada_scene(std::uint32_t id){
 switch(id){
 case 9000000:return "RVR";
 case 9003555:return "RVR_35_B";
 case 9005675:return "RVR_56_B";
 case 9007695:return "RVR_76_B";
 case 9009600:return "RVR_96_B";
 default:return nullptr;
 }
}
std::string WorldSession::scene_map_name(const std::string&name,std::uint32_t id)const{
 if(const auto *descada=descada_scene(id))return descada;
 bool instanced=id>=29000&&id<30000&&id!=29501&&id!=29502;
 if(dungeons_)instanced=dungeons_->rule(id).value("instanced",instanced);
 else if(content_.has("server/dungeon_rules.json")){
  const auto definitions=content_.get("server/dungeon_rules.json").at("maps");
  instanced=definitions.value(std::to_string(id),Json::object()).value("instanced",instanced);
 }
 if(instanced)return name+"#"+std::to_string(id)+"#"+(dungeon_group_.empty()?"solo:"+std::to_string(eid()):dungeon_group_);
 return name;
}
std::string WorldSession::map_scope()const{return SpawnPersistence::scope(scene_map_name(),identity_.channel,map_id_);}
static void dungeon_arrival_fallback(Json& destination,const Json& rule,bool observed){
 if(observed||!rule.contains("arrival"))return;
 const auto& fallback=rule.at("arrival");
 const double x=destination.at("x"),y=destination.at("y"),z=destination.at("z");
 const bool valid_xy=std::isfinite(x)&&std::isfinite(y)&&x>0&&y>0&&x<100000&&y<100000;
 // A scene's default region can be inside a room; it must not replace a valid NPC entrance.
 if(valid_xy&&(z!=0||std::hypot(x-fallback.at("x").get<double>(),y-fallback.at("y").get<double>())>=40))return;
 for(auto axis:{"x","y","z"})destination[axis]=fallback.at(axis);
 destination["resolve_native"]=false;
}
static Json native_target(Content&content,std::uint32_t target){
 auto typed=content.database("database/content_typed.sqlite");auto rows=typed->query("SELECT o4_i AS map_id,o8_f64 AS x,o16_f64 AS y,o24_f64 AS z FROM mapWarpTargetPos WHERE o0_i=?",{std::int64_t(target)});
 if(rows.empty())throw std::runtime_error("Destino ausente da tabela nativa");auto d=rows[0];d["target"]=target;bool known=false;
 for(auto&m:content.get("data/maplist.json"))if(m.at("head")[1]==d.at("map_id")){d["world_name"]=m.at("name");known=true;break;}
 if(!known)throw std::runtime_error("Mapa nativo desconhecido");auto arrivals=content.get("server/native_map_arrivals.json").at("targets");auto key=std::to_string(target);
 bool observed=arrivals.contains(key)&&arrivals[key].at("map_id")==d.at("map_id");
 if(observed)for(auto axis:{"x","y","z"})d[axis]=arrivals[key].at(axis);
 d["resolve_native"]=!observed&&d.at("z")==0;
 if(target==d.at("map_id")&&content.has("server/dungeon_rules.json")){
  const auto definitions=content.get("server/dungeon_rules.json").at("maps");const auto rule=definitions.value(std::to_string(target),Json::object());
  dungeon_arrival_fallback(d,rule,observed);
 }
 for(auto axis:{"x","y","z"}){double v=d.at(axis);if(!std::isfinite(v)||(std::string(axis)!="z"&&(v<=0||v>=100000))||std::abs(v)>100000)throw std::runtime_error("Coordenadas nativas invalidas");}
 return d;
}
bool WorldSession::travel_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const Sender&send){
 bool portal=cat==4&&sub==6,npc=cat==4&&sub==7,scroll=cat==6&&sub==2&&body.size()>=10&&get32(body,6,true)>=57105&&get32(body,6,true)<=57114;
 if(!portal&&!npc&&!scroll)return false;
 try{
  if(!loaded_||character_.at("hp").get<int>()<=0)throw std::runtime_error("Aguarde o mapa carregar; personagem deve estar vivo");
  if(event.empty()||event.size()>128)throw std::runtime_error("Identidade de viagem invalida");
  auto typed=content_.database("database/content_typed.sqlite");std::uint32_t target=0,source=0,item=0,ack=0,requirement=0,route_group=0;std::int64_t cost=0;std::optional<ItemInstance>consumed;
  if(portal){
   if(body.size()!=6||get16(body,4,true)!=0x123)throw std::runtime_error("Pedido de portal invalido");source=get32(body,0,true);ack=source;
   auto gates=typed->query("SELECT o4_i AS map_id,o8_f64 AS x,o16_f64 AS y,o24_f64 AS x2,o32_f64 AS y2,o48_i AS shape,o52_f64 AS radius,o60_i AS target FROM mapWarpGate WHERE o0_i=?",{std::int64_t(source)});
   if(gates.empty()||gates[0].at("map_id")!=map_id_)throw std::runtime_error("Portal nao pertence a este mapa");auto g=gates[0];double x=character_["pos"][0],y=character_["pos"][1],gx=g.at("x"),gy=g.at("y"),x2=g.at("x2"),y2=g.at("y2");
   bool within_gate=g.at("shape")==1?std::hypot(x-gx,y-gy)<=std::max(50.,g.at("radius").get<double>())+100:x>=std::min(gx,x2)-150&&x<=std::max(gx,x2)+150&&y>=std::min(gy,y2)-150&&y<=std::max(gy,y2)+150;
   if(!within_gate)throw std::runtime_error("Personagem fora da area do portal");target=g.at("target");
   if(content_.has("server/devtool_map_rules.json")){
    const auto config=content_.get("server/devtool_map_rules.json");
    const auto gates=config.value("portals",Json::object());
    const auto key=std::to_string(source);
    if(gates.contains(key)){
     const auto edited=gates.at(key).get<std::uint32_t>();
     const auto arrivals=content_.get("server/native_map_arrivals.json").at("targets");
     if(!arrivals.contains(std::to_string(edited)))throw std::runtime_error("Destino sem chegada nativa verificada");
     target=edited;
    }
   }
  }else{
   if(npc){
    std::size_t offset;if(body.size()==14&&get32(body,0,true)>0&&get16(body,12,true)==0x123)offset=4;else if(body.size()==13&&body[0]==0&&body[1]==0&&body[2]==1&&get16(body,11,true)==0x123)offset=3;else throw std::runtime_error("Pedido nativo de teleporte invalido");
    target=get32(body,offset,true);auto actor=get32(body,offset+4,true);auto entities=scene_->map(scene_map_name());auto found=entities->find(actor);auto p=character_.at("pos");
    if(found==entities->end()||!found->second.alive||found->second.combat||found->second.position.distance({p[0],p[1],p[2]})>350)throw std::runtime_error("NPC de teleporte indisponivel ou distante");source=found->second.npc_id;
   }else{
    if(body.size()!=16||get32(body,0,true)!=eid()||body[10]!=0||get32(body,11,true)!=eid()||body[15]!=0)throw std::runtime_error("Pedido de pergaminho invalido");item=get32(body,6,true);source=item;consumed=inventory_->owned(eid(),Inventory::key(body[4],body[5]),item,now());
   }
   auto routes=typed->query("SELECT o0_i AS route_group,o16_i AS target,o20_i AS cost,o24_i AS requirements FROM mapWarp WHERE o8_i=? ORDER BY rowid",{std::int64_t(source)});Json route;
   for(auto&r:routes)if(!npc||r.at("target")==target){route=r;break;}
   if(route.is_null())throw std::runtime_error("Este NPC ou pergaminho nao oferece esse destino");target=route.at("target");cost=std::max<std::int64_t>(0,route.at("cost").get<std::int64_t>());ack=target;requirement=route.at("requirements");route_group=route.at("route_group");if(npc&&body.size()==14&&get32(body,0,true)!=route_group)throw std::runtime_error("Invalid native route group");
  }
  auto destination=native_target(content_,target);
  if(descada_scene(destination.at("map_id").get<std::uint32_t>())){
   const auto limits=typed->query("SELECT o12_i AS min_level,o28_i AS max_level FROM mapWarpStat WHERE o0_i=? AND o4_i=4",{std::int64_t(target)});
   const int min_level=limits.empty()?36:limits.front().at("min_level").get<int>();
   const int max_level=limits.empty()?0:limits.front().at("max_level").get<int>();
   const int level=character_.at("level").get<int>();
   if(level<min_level||(max_level>0&&level>max_level)){
    const auto &locale=identity_.identity.locale;
    const char *prefix=locale=="es"?"Descada requiere nivel ":locale=="en"?"Descada requires level ":"Descada exige nível ";
    auto message=prefix+std::to_string(min_level);
    if(max_level>0)message+=(locale=="en"?" to ":" a ")+std::to_string(max_level);
    throw std::runtime_error(message+".");
   }
  }
  const auto destination_id=destination.at("map_id").get<std::uint32_t>();
  const auto definition=dungeons_->rule(destination_id);
  if(definition.value("instanced",false)){
   const auto spawns=content_.get("data/spawns.json");
   const auto spawn_key=definition.at("spawn_key").get<std::string>();
   if(!spawns.contains(spawn_key)||spawns.at(spawn_key).empty())throw std::runtime_error("Dungeon content is unavailable; entry was not charged");
  }
  std::optional<ItemInstance> entrance_item;bool consume_entrance=false;std::uint32_t required_entrance_item=0;
  if(requirement){
   const auto limits=typed->query("SELECT o12_i AS min_level,o16_i AS required_item,o24_i AS consume_item FROM mapWarpStat WHERE o0_i=? AND o4_i=0",{std::int64_t(requirement)});
   if(!limits.empty()){
    if(character_.at("level").get<int>()<limits.front().at("min_level").get<int>())throw std::runtime_error("Character level is too low for this dungeon");
    const auto required=limits.front().at("required_item").get<std::uint32_t>();required_entrance_item=required;
    if(required){
     for(const auto& owned:inventory_->items(eid()))if(owned.item==required&&(!owned.expires||*owned.expires>now())){entrance_item=owned;break;}
     consume_entrance=limits.front().at("consume_item")==1;
    }
   }
  }
  // Validate map resources without holding SQLite's writer lock against world combat.
  scene_->map(scene_map_name(destination.at("world_name"),destination_id));
  auto&db=store_.db();db.exec("CREATE TABLE IF NOT EXISTS map_travel_events(event_key TEXT PRIMARY KEY,player_eid INTEGER NOT NULL,target_id INTEGER NOT NULL,item_id INTEGER NOT NULL,cost INTEGER NOT NULL,created_at INTEGER NOT NULL)");
  auto before=character_;auto previous_name=map_name_;auto previous_id=map_id_;auto previous_group=dungeon_group_;bool internal=destination.at("map_id")==map_id_;
  {Transaction tx(db);auto prior=db.query("SELECT player_eid,target_id,item_id,cost FROM map_travel_events WHERE event_key=?",{event});
   if(!prior.empty()){if(prior[0].at("player_eid")!=eid()||prior[0].at("target_id")!=target||prior[0].at("item_id")!=item||prior[0].at("cost")!=cost)throw std::runtime_error("Repeticao de viagem alterada");tx.commit();return true;}
   auto name=destination.at("world_name").get<std::string>();auto id=destination.at("map_id").get<std::uint32_t>();
   bool new_admission=true;
   try{
   if(!internal){
    dungeon_group_=scene_->instance_group(eid());
    if(definition.value("instanced",false)){
     auto p=before.at("pos");Json entrance={{"world_name",previous_name},{"map_id",previous_id},{"x",p[0]},{"y",p[1]},{"z",p[2]},{"angle",before.at("angle")}};
     auto run=dungeons_->join(eid(),dungeon_group_,id,identity_.channel,entrance,now());
     dungeon_group_="run:"+run.at("id").get<std::string>();new_admission=run.at("charge_entry");
    }
   }
   auto scene_name=scene_map_name(name,id);
    if(new_admission&&required_entrance_item&&!entrance_item)throw std::runtime_error("Required dungeon entrance item: "+inventory_->name(required_entrance_item));
    if(cost&&new_admission)Wallet(db,*inventory_).apply(eid(),-cost,event);
    if(entrance_item&&consume_entrance&&new_admission)inventory_->consume(eid(),*entrance_item,1,"dungeon-entry");
    if(consumed)inventory_->consume(eid(),*consumed,1,"travel-scroll");
    if(!internal&&previous_group!=dungeon_group_)dungeons_->leave(previous_group,eid(),now());
    character_["pos"]={destination.at("x"),destination.at("y"),destination.at("z")};character_["angle"]=0.;character_["map"]=name;map_name_=name;map_id_=id;
    persist();db.exec("INSERT INTO map_travel_events VALUES(?,?,?,?,?,?)",{event,std::int64_t(eid()),std::int64_t(target),std::int64_t(item),cost,std::int64_t(now())});tx.commit();
   }catch(...){character_=before;map_name_=previous_name;map_id_=previous_id;dungeon_group_=previous_group;throw;}
  }
  if(!internal){dungeon_event_after_=0;next_dungeon_poll_=0;}
  scene_->restore(scene_map_name(),db,map_scope(),now());
  if(entrance_item&&consume_entrance)send_inventory(send,false);
  clear_mount();casting_->cancel();if(!internal)received_party_auras_=Json::object();recalculate();scene_->depart(scene_key());scene_entities_=scene_->map(scene_map_name());scene_visible_.clear();ground_visible_.clear();gm_visible_.clear();loaded_=internal;position_guard_=true;resolve_native_arrival_=destination.value("resolve_native",false)&&!internal;
  if(!internal){Bytes change(17);put32(change,0,eid(),true);put32(change,4,map_id_,true);if(definition.value("instanced",false)&&npc){change[8]=1;put32(change,9,route_group,true);put32(change,13,target,true);}send(4,4,change);}
  Bytes reply(6);put32(reply,0,ack,true);put16(reply,4,0x123,true);if(!scroll)send(4,6,reply);
  if(internal){if(scroll)send_inventory(send,true);send(4,2,appearance());publish_player(send);scene_visibility(*scene_,*scene_entities_,character_.at("pos"),scene_visible_,send);}
 }catch(const std::exception&e){send(11,1,notice(e.what()));}
 return true;
}
static void ensure_mine_registrations(Sqlite &db){
   db.exec("CREATE TABLE IF NOT EXISTS native_mine_registrations(start_at INTEGER NOT NULL,guild_id INTEGER NOT NULL,"
           "channel INTEGER NOT NULL,registered_by INTEGER NOT NULL,registered_at INTEGER NOT NULL DEFAULT 0,"
           "PRIMARY KEY(start_at,guild_id))");
   if(db.query("SELECT 1 FROM pragma_table_info('native_mine_registrations') WHERE name='registered_at'").empty())
    db.exec("ALTER TABLE native_mine_registrations ADD COLUMN registered_at INTEGER NOT NULL DEFAULT 0");
}
std::int64_t lupinel_round_start(const fs::path &root){
 return Json::parse(read_file(root/"Config/server.json")).value("lupinel_mine_start_unix",std::int64_t{0});
}
void select_lupinel_challengers(Sqlite &db,std::int64_t start,int channel,std::int64_t timestamp){
 constexpr std::int64_t cutoff=20*60*60, duration=60*60;
 if(start<=0||timestamp<start-cutoff||(timestamp>=start&&timestamp-start>=duration))return;
 Transaction tx(db);
 db.exec("CREATE TABLE IF NOT EXISTS native_mine_selections(start_at INTEGER NOT NULL,channel INTEGER NOT NULL,"
         "selected_at INTEGER NOT NULL,PRIMARY KEY(start_at,channel));"
         "CREATE TABLE IF NOT EXISTS native_mine_challengers(start_at INTEGER NOT NULL,channel INTEGER NOT NULL,"
         "guild_id INTEGER NOT NULL,seed INTEGER NOT NULL,level_sum INTEGER NOT NULL,"
         "PRIMARY KEY(start_at,channel,guild_id),UNIQUE(start_at,channel,seed))");
 const auto prior=db.query("SELECT 1 FROM native_mine_selections WHERE start_at=? AND channel=?",
                           {start,std::int64_t(channel)});
 if(prior.empty()){
  ensure_mine_registrations(db);
  const auto registrations=db.query("SELECT 1 FROM native_mine_registrations WHERE start_at=? AND channel=? LIMIT 1",
                                     {start,std::int64_t(channel)});
  if(!registrations.empty()){
    const auto ranked=db.query(
        "SELECT r.guild_id,SUM(CAST(json_extract(c.value,'$.level') AS INTEGER)) AS score "
        "FROM native_mine_registrations r JOIN native_guilds g ON g.id=r.guild_id "
        "JOIN native_guild_members m ON m.guild_id=r.guild_id JOIN accounts a ON a.id=m.account_id "
        "JOIN json_each(a.payload,'$.chars') c ON json_extract(c.value,'$.eid')=m.eid "
        "WHERE r.start_at=? AND r.channel=? GROUP BY r.guild_id "
        "HAVING COUNT(DISTINCT m.eid)>20 ORDER BY score DESC,r.registered_at,r.guild_id LIMIT 5",
        {start,std::int64_t(channel)});
    std::int64_t seed=0;
    for(const auto &row:ranked)db.exec("INSERT INTO native_mine_challengers VALUES(?,?,?,?,?)",
        {start,std::int64_t(channel),row.at("guild_id").get<std::int64_t>(),++seed,row.at("score").get<std::int64_t>()});
  }
  db.exec("INSERT INTO native_mine_selections VALUES(?,?,?)",{start,std::int64_t(channel),timestamp});
 }
 tx.commit();
}
bool WorldSession::mine_registration_request(unsigned char cat,unsigned char sub,const Bytes&body,const Sender&send){
 constexpr std::uint32_t occupation=1900201, caretaker=9419201;
 constexpr std::int64_t fee=100000, registration_cutoff=20*60*60;
 const bool list=sub==2;
 if(cat!=0x0d||(sub!=1&&!list)||body.size()!=(list?12u:14u)||get32(body,4,true)!=occupation)return false;
 try{
  if(!loaded_||map_id_!=19002||character_.value("hp",0)<=0||get32(body,0,true)!=eid()||
     (list?get32(body,8,true)!=0:(get16(body,8,true)!=0x12b||get32(body,10,true)!=0)))
   throw std::runtime_error("Pedido de inscrição inválido.");
  const auto &p=character_.at("pos");
  const Position position{p[0],p[1],p[2]};
  const auto entities=scene_->map(scene_map_name());
  const auto nearby=std::any_of(entities->begin(),entities->end(),[&](const auto &actor){
   const auto &entity=actor.second;
   return entity.npc_id==caretaker&&entity.alive&&!entity.combat&&entity.position.distance(position)<=350;
  });
  if(!nearby)throw std::runtime_error("Fale com Cortes para inscrever o clã.");
  auto &db=store_.db();
  Bytes reply;
  {
   Transaction tx(db);
   // A pending registration can wait on the same writer lock as challenger selection.
   const auto start=lupinel_round_start(root_);
   const auto timestamp=static_cast<std::int64_t>(now());
   if(!list&&start<=timestamp+registration_cutoff)
    throw std::runtime_error("As inscrições da Conquista da Mina estão fechadas.");
   ensure_mine_registrations(db);
   if(list){
    const auto registrations=db.query("SELECT g.id,g.name,r.registered_at FROM native_mine_registrations r "
                                      "JOIN native_guilds g ON g.id=r.guild_id WHERE r.start_at=? AND r.channel=? "
                                      "ORDER BY r.registered_at,g.id",{start,std::int64_t(identity_.channel)});
    reply.resize(8);
    put32(reply,0,occupation,true);
    put32(reply,4,static_cast<std::uint32_t>(registrations.size()),true);
    for(const auto &row:registrations){
     const auto name=row.at("name").get<std::string>();
     reply.insert(reply.end(),name.begin(),name.end());
     reply.push_back(0);
     const auto offset=reply.size();
     reply.resize(offset+12);
     const auto registered=row.at("registered_at").get<std::uint64_t>();
     put32(reply,offset,static_cast<std::uint32_t>(registered>>32),true);
     put32(reply,offset+4,static_cast<std::uint32_t>(registered),true);
     put32(reply,offset+8,row.at("id").get<std::uint32_t>(),true);
    }
   }else{
    const auto leader=db.query("SELECT guild_id FROM native_guild_members WHERE eid=? AND account_id=? AND rank=5",
                               {std::int64_t(eid()),identity_.identity.id});
    if(leader.empty())throw std::runtime_error("Somente o líder do clã pode inscrevê-lo.");
    const auto guild=leader.front().at("guild_id").get<std::int64_t>();
    const auto existing=db.query("SELECT channel FROM native_mine_registrations WHERE start_at=? AND guild_id=?",{start,guild});
    if(!existing.empty()&&existing.front().at("channel")!=identity_.channel)
     throw std::runtime_error("O clã já está inscrito em outro canal.");
    if(existing.empty()){
     const auto size=db.query("SELECT COUNT(DISTINCT m.eid) AS members FROM native_guild_members m "
                             "JOIN accounts a ON a.id=m.account_id JOIN json_each(a.payload,'$.chars') c "
                             "ON json_extract(c.value,'$.eid')=m.eid WHERE m.guild_id=?",{guild});
     if(size.front().at("members").get<int>()<=20)throw std::runtime_error("O clã precisa ter mais de 20 membros.");
     const auto key="mine-registration:"+std::to_string(start)+":"+std::to_string(guild);
     Wallet(db,*inventory_).apply(eid(),-fee,key);
     db.exec("INSERT INTO native_mine_registrations(start_at,guild_id,channel,registered_by,registered_at) VALUES(?,?,?,?,?)",
             {start,guild,std::int64_t(identity_.channel),std::int64_t(eid()),timestamp});
    }
    reply.resize(6);
    put32(reply,0,occupation,true);
    put16(reply,4,0x12b,true);
   }
   tx.commit();
  }
  send(cat,sub,reply);
  if(!list)send_inventory(send,true);
 }catch(const std::exception &error){send(11,1,notice(error.what()));}
 return true;
}
bool WorldSession::revival_request(unsigned char cat,unsigned char sub,const Bytes& body,const std::string& event,const Sender& send){
 const std::string action="npcshop playerrevive";
 const bool button=cat==7&&sub==1&&body.size()==action.size()+1&&body.back()==0&&
     std::equal(action.begin(),action.end(),body.begin());
 const bool native=cat==4&&sub==0&&character_.value("hp",0)<=0;
 if(!button&&!native)return false;
 try{
  if(character_.value("hp",0)>0){quest_overlay({{"cmd","asmara.death"},{"dead",false}},send);return true;}
  if(native&&(body.size()!=21||get32(body,1,true)!=eid()))throw std::runtime_error("Invalid revival actor");
  Json destination;
  try{destination=resolve_native_map(content_,map_name_);}catch(const std::exception&){destination=resolve_native_map(content_,"berneo");}
  if(dungeons_&&dungeons_->rule(map_id_).value("instanced",false)){
   auto entrance=dungeons_->entrance(dungeon_group_,eid());
   destination=entrance.is_object()?entrance:resolve_native_map(content_,"berneo");
  }
  auto before=character_;auto old_name=map_name_;auto old_id=map_id_;auto old_group=dungeon_group_;
  auto& db=store_.db();
  db.exec("CREATE TABLE IF NOT EXISTS native_player_revivals(event_key TEXT PRIMARY KEY,eid INTEGER NOT NULL,created REAL NOT NULL)");
  if(!event.empty()&&!db.query("SELECT 1 FROM native_player_revivals WHERE event_key=?",{event}).empty())return true;
  const auto destination_name=destination.at("world_name").get<std::string>();
  const auto destination_id=destination.at("map_id").get<std::uint32_t>();
  const auto target_scene=scene_map_name(destination_name,destination_id);
  // World combat commits deaths under the scene lock. Acquire no SQLite write lock while waiting for it.
  scene_->map(target_scene);
  scene_->restore(target_scene,db,SpawnPersistence::scope(target_scene,identity_.channel,destination_id),now());
  try{
   Transaction tx(db);
   dungeons_->leave(dungeon_group_,eid(),now());
   map_name_=destination_name;map_id_=destination_id;dungeon_group_.clear();
   character_["hp"]=character_.at("maxhp");character_["mp"]=character_.at("maxmp");
   character_["pos"]={destination.at("x"),destination.at("y"),destination.at("z")};
   character_["angle"]=0.;character_["map"]=map_name_;
   persist();
   if(!event.empty())db.exec("INSERT INTO native_player_revivals VALUES(?,?,?)",{event,std::int64_t(eid()),now()});
   tx.commit();
  }catch(...){character_=before;map_name_=old_name;map_id_=old_id;dungeon_group_=old_group;throw;}
  clear_mount();casting_->cancel();death_notice_sent_=false;received_party_auras_=Json::object();
  recalculate();scene_->depart(scene_key());scene_entities_=scene_->map(scene_map_name());
  scene_visible_.clear();ground_visible_.clear();gm_visible_.clear();loaded_=false;position_guard_=true;
  resolve_native_arrival_=destination.value("resolve_native",false);
  quest_overlay({{"cmd","asmara.death"},{"dead",false}},send);
  send(8,0,gauge(0x0800));send(8,1,gauge(0x0801));
  Bytes change(17);put32(change,0,eid(),true);put32(change,4,map_id_,true);send(4,4,change);
 }catch(const std::exception&e){send(11,1,notice(e.what()));}
 return true;
}
Json native_map_destinations(Content&content){
 auto typed=content.database("database/content_typed.sqlite");auto maps=content.get("data/maplist.json");auto arrivals=content.get("server/native_map_arrivals.json").at("targets");Json targets=Json::array();
 const auto dungeons=content.has("server/dungeon_rules.json")?content.get("server/dungeon_rules.json").at("maps"):Json::object();
 static const std::map<int,int> preferred={{19002,190021},{19003,190031},{19004,190041},{19005,190051},{19006,190061},{19007,190072},{19008,1900801},{19009,1900901},{19010,1901001},{19014,1901406}};
 static const std::map<int,std::string> overrides={{9000000,"RVR"},{9003555,"RVR_35_B"},{9005675,"RVR_56_B"},{9007695,"RVR_76_B"},{9009600,"RVR_96_B"},{1900308,"Siege_"}};
 for(auto map:maps){
  int map_id=map.at("head")[1];auto name=map.at("name").get<std::string>();if(overrides.count(map_id))name=overrides.at(map_id);
  auto rows=typed->query("SELECT o0_i AS target,o8_f64 AS x,o16_f64 AS y,o24_f64 AS z,strings AS label FROM mapWarpTargetPos WHERE o4_i=? ORDER BY o0_i",{std::int64_t(map_id)});
  Json chosen;int priority=-1;
  for(auto row:rows){
   auto target=row.at("target").get<int>();auto key=std::to_string(target);if(arrivals.contains(key)&&arrivals[key].at("map_id")==map_id)for(auto axis:{"x","y","z"})row[axis]=arrivals[key].at(axis);
   if(target==map_id&&dungeons.contains(std::to_string(map_id)))dungeon_arrival_fallback(row,dungeons.at(std::to_string(map_id)),arrivals.contains(key)&&arrivals[key].at("map_id")==map_id);
   double x=row.at("x"),y=row.at("y"),z=row.at("z");if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||x<=0||x>=100000||y<=0||y>=100000||std::abs(z)>10000)continue;
   int score=0;std::string label=row.value("label","");for(auto&c:label)c=std::tolower(static_cast<unsigned char>(c));for(auto word:{"city","return stone","oasis","border village","dock","town"})if(label.find(word)!=std::string::npos)score=1;if(target==map_id)score=2;if(preferred.count(map_id)&&preferred.at(map_id)==target)score=3;
   if(score>priority){chosen=row;priority=score;}
  }
  if(!chosen.is_null()){
   // An unset Z on a default entrance is not a valid terrain height. Use a
   // complete observed position nearby, never a height copied onto other X/Y.
   const auto key=std::to_string(chosen.at("target").get<int>());
   if(chosen.at("z")==0&&(!arrivals.contains(key)||arrivals.at(key).at("map_id")!=map_id)){
    const auto x=chosen.at("x").get<double>(),y=chosen.at("y").get<double>();
    const Json *nearest=nullptr;
    double distance=40.;
    for(const auto &arrival:arrivals){
     if(arrival.at("map_id")!=map_id)continue;
     const auto ax=arrival.at("x").get<double>(),ay=arrival.at("y").get<double>(),az=arrival.at("z").get<double>();
     if(!std::isfinite(az)||std::abs(az)>10000)continue;
     const auto candidate=std::hypot(ax-x,ay-y);
     if(candidate<distance){nearest=&arrival;distance=candidate;}
    }
    if(nearest)for(const char *axis:{"x","y","z"})chosen[axis]=nearest->at(axis);
   }
   chosen["map_id"]=map_id;chosen["world_name"]=name;targets.push_back(chosen);
  }
 }
 std::sort(targets.begin(),targets.end(),[](auto&a,auto&b){return a.at("map_id")<b.at("map_id");});return targets;
}
Json resolve_native_map(Content&content,const std::string&query){
 static const std::map<std::string,int> aliases={{"lupinel",19002},{"asmara",19002},{"berneo",19003},{"tempteroon",19004},{"tempteroom",19004},{"tempteron",19004},{"peltrok",19005},{"peltrock",19005},{"balasong",19006},{"bhalasong",19006},{"seron",19007},{"seron island",19007},{"bloodstone",19008},{"blood stone",19008},{"posetunia",19009},{"posetunya",19009},{"focetunia",19009},{"heristum",19010},{"eristum",19010},{"zastava",19014},{"outpost",19014}};
 auto key=query;for(auto&c:key)c=std::tolower(static_cast<unsigned char>(c));int id=-1;if(aliases.count(key))id=aliases.at(key);else try{std::size_t used;id=std::stoi(key,&used);if(used!=key.size())id=-1;}catch(...){}
 for(auto&row:native_map_destinations(content))if(row.at("map_id")==id||_stricmp(row.at("world_name").get<std::string>().c_str(),query.c_str())==0)return row;
 throw std::runtime_error("Mapa sem destino nativo verificado. Use //maps.");
}
void WorldSession::transition_map(const Json&destination,const Sender&send,bool internal){
 if(!loaded_||character_.at("hp").get<int>()<=0)throw std::runtime_error("Aguarde o mapa carregar; personagem deve estar vivo");
 auto name=destination.at("world_name").get<std::string>();auto map_id=destination.at("map_id").get<std::uint32_t>();auto previous_group=dungeon_group_;dungeon_group_=scene_->instance_group(eid());
 if(dungeons_->rule(map_id).value("instanced",false)){
  auto p=character_.at("pos");auto run=dungeons_->join(eid(),dungeon_group_,map_id,identity_.channel,{{"world_name",map_name_},{"map_id",map_id_},{"x",p[0]},{"y",p[1]},{"z",p[2]}},now());dungeon_group_="run:"+run.at("id").get<std::string>();
 }
 auto scene_name=scene_map_name(name,map_id);
 try{scene_->map(scene_name);scene_->restore(scene_name,store_.db(),SpawnPersistence::scope(scene_name,identity_.channel,map_id),now());}catch(...){dungeon_group_=previous_group;throw;}
 auto previous=character_;auto previous_name=map_name_;auto previous_id=map_id_;
 character_["pos"]={destination.at("x"),destination.at("y"),destination.at("z")};character_["angle"]=0.;character_["map"]=name;map_name_=name;map_id_=map_id;
 try{persist();}catch(...){character_=previous;map_name_=previous_name;map_id_=previous_id;dungeon_group_=previous_group;throw;}
 if(previous_group!=dungeon_group_)dungeons_->leave(previous_group,eid(),now());dungeon_event_after_=0;next_dungeon_poll_=0;
 clear_mount();casting_->cancel();if(!internal)received_party_auras_=Json::object();recalculate();scene_->depart(scene_key());scene_entities_=scene_->map(scene_name);scene_visible_.clear();ground_visible_.clear();gm_visible_.clear();loaded_=internal;position_guard_=true;resolve_native_arrival_=destination.value("resolve_native",false);
 Bytes body(17);put32(body,0,eid(),true);put32(body,4,map_id_,true);send(4,4,body);
}
bool WorldSession::dungeon_request(unsigned char cat,unsigned char sub,const Bytes& body,const Sender& send){
 if(cat!=10||sub!=1)return false;
 try{
  if(!loaded_||character_.value("hp",0)<=0||body.size()!=8||get32(body,0,true)!=map_id_)throw std::runtime_error("Invalid dungeon event request");
  const auto& p=character_.at("pos");
  dungeons_->trigger(dungeon_group_,eid(),map_id_,get32(body,4,true),{p[0],p[1],p[2]},now());
  next_dungeon_poll_=0;sync_dungeon(send);
 }catch(const std::exception& error){send(11,1,notice(error.what()));}
 return true;
}
void WorldSession::sync_dungeon(const Sender& send){
 if(!dungeons_||dungeon_group_.rfind("run:",0)!=0||!loaded_||now()<next_dungeon_poll_)return;
 next_dungeon_poll_=now()+1;
 for(const auto& row:dungeons_->events(dungeon_group_,eid(),dungeon_event_after_,now())){
  send(10,row.value("subcode",0),unhex(row.at("body")));dungeon_event_after_=row.at("sequence");
 }
 scene_->apply_dungeon_actors(scene_map_name(),dungeons_->active_references(dungeon_group_),dungeons_->inactive_references(dungeon_group_));
 const auto updated=scene_->map(scene_map_name());
 if(updated!=scene_entities_){scene_entities_=updated;scene_visibility(*scene_,*scene_entities_,character_.at("pos"),scene_visible_,send);}
}

}
