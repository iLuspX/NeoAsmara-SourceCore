#include "native/game.hpp"
#include <algorithm>
#include <iostream>
using namespace rosh;
static void check(bool value,const char*message){if(!value)throw std::runtime_error(message);}
int wmain(int argc,wchar_t**argv){try{
 check(argc==2,"Use isolated fixture");fs::path root=argv[1];check(fs::is_regular_file(root/"GM_TEST_FIXTURE"),"Refusing live databases");Content content(root);
 check(std::abs(attack_rate(1.89)-1.89)<1e-9&&std::abs(attack_rate(1.89,5)-9.45)<1e-9,"Captured native attack rate inverted");
 auto damage=damage_range(55,10,1);check(damage.first==30&&damage.second==41,"Python normal damage formula differs");check(attack_interval(1,5)==.2&&std::abs(attack_interval(.35,5)-1./1.75)<1e-9,"Attack rate was treated as duration");
 check(attack_interval(1.5)<attack_interval(1.)&&attack_interval(.8)>attack_interval(1.),"Buff/debuff attack rate direction inverted");
 const Json potency={{"power",1.}};
 for(int profession:{100,200,300,400,500,600,800}){
  const bool caster=profession==400||profession==600;
  check(skill_damage_channels(potency,profession)==std::pair<bool,bool>{!caster,caster},
        "Power-only skill used the wrong class damage channel");
 }
 check(skill_damage_channels({{"magic_min",10.}},200)==std::pair<bool,bool>{false,true},
       "Physical class lost a native magic skill");
 check(skill_damage_channels({{"physical_min",10.},{"magic_max",10.}},600)==std::pair<bool,bool>{true,true},
       "Explicit hybrid skill lost a damage channel");
 auto carriers=content.get("server/combat_templates.json");auto hit=combat_damage(unhex(carriers.at("0502")),12345,123,42,{1,2,3},false,77,true);check(hit.size()==45&&get32(hit,1,true)==12345&&get32(hit,16)==123&&get32(hit,21)==42&&hit[25]==1,"Wrong native damage envelope");
 WorldScene world(content);auto entities=world.map("berneo");const Entity*chosen=nullptr;for(auto&[id,mob]:*entities)if(mob.combat&&mob.alive&&mob.category=="normal"&&mob.level<=15&&mob.position.x>0&&mob.position.y>0&&(!chosen||mob.maxhp<chosen->maxhp))chosen=&mob;check(chosen,"No combat entity");auto mob=*chosen;
 constexpr std::uint32_t eid=72001;Json pos={mob.position.x,mob.position.y,mob.position.z};Bytes appearance(260),movement(34);std::vector<Bytes> hits;
 auto send=[&](unsigned char cat,unsigned char sub,const Bytes&body){if(cat==5&&sub==2)hits.push_back(body);};
 Json stats={{"hp",100000},{"level",mob.level},{"attack",1000000000.},{"attack_speed",.35},{"defense",0.},{"critical",0.}};
 world.update_player("fighter",eid,"berneo",pos,appearance,movement,false,send,stats);
 Bytes request(32);put32(request,1,eid,true);put32(request,6,7,true);put32(request,28,mob.eid,true);bool rejected=false;
 try{world.strike("fighter",1,request,100,[&](Entity&){throw std::runtime_error("Injected reward rollback");});}catch(...){rejected=true;}
 check(rejected&&world.map("berneo")->at(mob.eid).alive&&hits.empty(),"Failed reward killed mob or published success");
 int claims=0;world.strike("fighter",1,request,100,[&](Entity&dead){check(!dead.alive&&dead.hp==0&&dead.damage[eid]==static_cast<unsigned>(mob.hp),"Overkill merit or death state wrong");dead.respawn_at=101;++claims;});check(claims==1&&!hits.empty(),"Kill commit not published");
 rejected=false;try{world.strike("fighter",1,request,101,[&](Entity&){++claims;});}catch(...){rejected=true;}check(rejected&&claims==1,"Repeated death paid twice");
 // Exercise the real visibility publisher across death and respawn.
 std::set<std::uint32_t> visible{mob.eid};int created=0,removed=0;
 auto visibilitySend=[&](unsigned char cat,unsigned char sub,const Bytes&packet){
  if(cat==4&&sub==2)++created;
  if(cat==4&&sub==3&&get32(packet,1,true)==mob.eid)++removed;
 };
 auto deadOnly=SceneEntities{{mob.eid,world.map("berneo")->at(mob.eid)}};
 scene_visibility(world,deadOnly,pos,visible,visibilitySend);
 check(removed==1&&!visible.count(mob.eid),"Dead mob was not removed from visibility");
 world.update_player("fighter",eid,"berneo",pos,appearance,movement,false,visibilitySend,stats);
 world.simulate(100.9);check(!world.map("berneo")->at(mob.eid).alive&&created==0,"Mob respawned before deadline");
 world.simulate(102);check(world.map("berneo")->at(mob.eid).alive,"Shared simulation did not respawn");
 auto liveOnly=SceneEntities{{mob.eid,world.map("berneo")->at(mob.eid)}};
 scene_visibility(world,liveOnly,pos,visible,visibilitySend);
 scene_visibility(world,liveOnly,pos,visible,visibilitySend);
 check(created==1&&visible.size()==1,"Respawn created duplicate client actors");
 world.update_player("fighter",eid,"berneo",pos,appearance,movement,false,send,stats);
 auto identity=request;put32(identity,1,eid+1,true);rejected=false;try{world.strike("fighter",1,identity,103,{});}catch(...){rejected=true;}check(rejected,"Forged attacker accepted");
 stats["attack"]=1.;world.update_player("fighter",eid,"berneo",pos,appearance,movement,false,send,stats);world.strike("fighter",1,request,103,[&](Entity&){throw std::runtime_error("Unexpected lethal fixture strike");});
 for(int tick=0;tick<32;tick++)world.simulate(103.25+tick*.25);
 auto pending=world.depart("fighter");check(pending>0&&world.take_damage("fighter")==0,"Disconnect lost queued mob damage or retained the actor");
 // Exercise the real WorldSession path, including a durable reward commit.
 LegacyStore store(root/"Database/karos_gameplay.sqlite");Progression progression;
 Json character={{"eid",eid},{"name","CombatFixture"},{"cls",100},{"level",115},{"exp",progression.threshold(115)},{"hp",100000},{"mp",100000},{"fletta",0},{"fletta_energy",0},{"moral_value",0},{"skill_points",0}};
 store.db().exec("INSERT INTO accounts(id,payload) VALUES(?,?)",{std::string("combat-fixture"),Json{{"chars",Json::array({character})}}.dump()});
 store.db().exec("CREATE TABLE IF NOT EXISTS character_locations(eid INTEGER PRIMARY KEY,map_name TEXT NOT NULL,x REAL NOT NULL,y REAL NOT NULL,z REAL NOT NULL,angle REAL NOT NULL,updated_at REAL NOT NULL)");
 store.db().exec("INSERT INTO character_locations VALUES(?,?,?,?,?,?,?)",{std::int64_t(eid),std::string("berneo"),mob.position.x,mob.position.y,mob.position.z,0.,now()});
 auto scene=std::make_shared<WorldScene>(content);Session session;session.identity.id="combat-fixture";session.id=token(20);session.expires=now()+600;session.channel=1;
 std::uint64_t initial=character.at("exp");
 {
  WorldSession game(content,store,session,root,eid,scene);Bytes ready(21);put32(ready,1,eid,true);putfloat(ready,5,mob.position.x);putfloat(ready,9,mob.position.y);putfloat(ready,13,mob.position.z);game.handle(4,0,ready,send);
  for(int count=0;count<40&&game.character().at("exp")==initial;count++){game.handle(5,1,request,send,"fixture-hit-"+std::to_string(count));if(game.character().at("exp")==initial)Sleep(400);}
  check(game.character().at("exp").get<std::uint64_t>()>initial,"Real WorldSession did not award kill XP");
  auto saved=store.character("combat-fixture",eid);check(saved.at("exp")==game.character().at("exp")&&saved.at("fletta_energy")==game.character().at("fletta_energy"),"Progress lost at checkpoint");
  auto scope=SpawnPersistence::scope("berneo",1,19003);Inventory inventory(store.db(),content);GroundLoot loot(store.db(),content,inventory);
  auto drops=loot.visible(scope,pos,now());check(!drops.empty(),"Kill generated no visible drops");auto ground=drops.begin()->second;auto actor=drops.begin()->first;
  const auto policy=content.get("server/drop_policy.json");
  const double scatter=std::clamp(policy.value("scatter_radius",45.),0.,80.);
  const double minimum=std::clamp(policy.value("scatter_minimum",10.),0.,scatter);
  for(const auto &[dropActor,row]:drops){
   const auto radius=std::hypot(row.at("x").get<double>()-mob.position.x,row.at("y").get<double>()-mob.position.y);
   check(radius>=minimum-.01&&radius<=scatter+.01,"Drop position outside configured scatter");
  }
  auto spawn=GroundLoot::spawn(eid,ground);
  check(spawn.size()==181&&get32(spawn,4,true)==actor&&get32(spawn,22)==ground.at("item_id")&&
        get32(spawn,12,true)==10&&get32(spawn,16,true)==0&&get16(spawn,20,true)==0&&
        get32(spawn,76)==4102444800u&&spawn[179]==0&&spawn[180]==0,
        "Ground appearance carrier differs from the Genesis monster drop layout");
  Bytes pick(14);put32(pick,0,eid,true);put32(pick,4,actor,true);put32(pick,8,ground.at("item_id"),true);put16(pick,12,0x005a,true);
  rejected=false;try{loot.pickup(eid,SpawnPersistence::scope("berneo",2,19003),pos,pick,now());}catch(...){rejected=true;}check(rejected,"Pickup leaked between channels");
  auto foreign=pick;put32(foreign,0,eid+1,true);rejected=false;try{loot.pickup(eid+1,scope,pos,foreign,now());}catch(...){rejected=true;}check(rejected,"Unrelated player stole merit-locked drop");
  const double range=std::clamp(policy.value("pickup_distance",75.),1.,500.);
  const double x=ground.at("x"),y=ground.at("y"),z=ground.at("z");
  rejected=false;try{loot.pickup(eid,scope,Json::array({x+range+.01,y,z}),pick,now());}catch(...){rejected=true;}check(rejected,"Pickup accepted beyond configured range");
  auto rules=content.get("data/native_item_rules.json");std::uint32_t filler=0;for(auto it=rules.begin();it!=rules.end();++it)if(it.value().value("kind",0)==1&&it.value().value("stack",1)==1){filler=static_cast<std::uint32_t>(std::stoul(it.key()));break;}check(filler,"No equipment fixture");
  auto free=inventory.available(eid,now()).size()-inventory.items(eid).size();if(free)inventory.grant_once(eid,{{filler,static_cast<int>(free)}},"combat-full-bag");
  rejected=false;try{loot.pickup(eid,scope,pos,pick,now());}catch(...){rejected=true;}check(rejected,"Pickup bypassed full inventory");
  auto intact=store.db().query("SELECT picked_at FROM ground_items WHERE id=?",{ground.at("id").get<std::int64_t>()});check(intact[0].at("picked_at").is_null(),"Failed pickup consumed ground item");
  store.db().exec("DELETE FROM inventory_slots WHERE player_eid=? AND item_id=?",{std::int64_t(eid),std::int64_t(filler)});
  loot.pickup(eid,scope,Json::array({x+range-.01,y,z}),pick,now());rejected=false;try{loot.pickup(eid,scope,pos,pick,now());}catch(...){rejected=true;}check(rejected,"Pickup duplicated the item");
  const auto removal=GroundLoot::remove(actor,ground.at("item_id"));
  check(get32(removal,0,true)==0xffffffff&&get16(removal,12,true)==0x01d6,"Wrong observer removal");

  loot.record("wire-pickup-"+token(12),mob,eid,scope,0,now());
  const auto nearby=loot.visible(scope,pos,now());
  check(!nearby.empty(),"No native pickup fixture");
  const auto &offered=nearby.begin()->second;
  put32(pick,4,nearby.begin()->first,true);put32(pick,8,offered.at("item_id"),true);
  std::vector<std::pair<int,Bytes>> wire;
  game.handle(6,3,pick,[&](unsigned char cat,unsigned char sub,const Bytes &packet){
   wire.emplace_back(cat*256+sub,packet);
  });
  check(!wire.empty()&&wire.front().first==0x0603&&wire.front().second==pick,
        "Picker did not receive the native acknowledgement before the inventory update");
  check(loot.visible(scope,pos,now()+121).empty(),"Expired mob drops remain visible");
 }
 {
  auto guards=scene->map("berneo");auto found=std::find_if(guards->begin(),guards->end(),[](const auto&row){return row.second.guard;});
  check(found!=guards->end(),"No city guard fixture");
  const auto guard=found->second;
  store.db().exec("UPDATE character_locations SET x=?,y=?,z=? WHERE eid=?",
                  {guard.position.x,guard.position.y,guard.position.z,std::int64_t(eid)});
  WorldSession game(content,store,session,root,eid,scene);
  Bytes ready(21);put32(ready,1,eid,true);putfloat(ready,5,guard.position.x);putfloat(ready,9,guard.position.y);putfloat(ready,13,guard.position.z);
  std::string feedback;
  bool blocked=false;
  auto receive=[&](unsigned char cat,unsigned char sub,const Bytes&body){
   if(cat==3)feedback.append(body.begin(),body.end());
   if(cat==11&&sub==1&&body==notice("Alvo invalido ou fora de alcance"))blocked=true;
   send(cat,sub,body);
  };
  game.handle(4,0,ready,receive);
  auto guardHit=request;put32(guardHit,28,guard.eid,true);
  auto moral=game.character().at("moral_value").get<std::int64_t>();auto experience=game.character().at("exp");
  auto scope=SpawnPersistence::scope("berneo",1,19003);
  auto lootBefore=store.db().query("SELECT COUNT(*) AS count FROM ground_items")[0].at("count");
  game.handle(5,1,guardHit,receive,"guard-hit-without-pk");
  check(scene->map("berneo")->at(guard.eid).hp==guard.hp&&blocked,
        "Normal mode allowed a city guard attack");
  auto command=[&](std::string text,const std::string&event){Bytes packet(text.begin(),text.end());packet.push_back(0);feedback.clear();game.handle(7,1,packet,receive,event);};
  command("//pk on","guard-enable-pk");check(feedback.find("Modo PK ativo")!=std::string::npos,"PK command did not activate");
  auto stats=Json{{"hp",100000},{"level",115},{"attack",1000000000.},{"attack_speed",1.},{"pk_mode",true}};
  auto position=game.character().at("pos");
  for(int attempt=0;attempt<5&&scene->map("berneo")->at(guard.eid).alive;++attempt){
   game.admin_tick(receive);
   scene->update_player(session.id+":"+std::to_string(eid),eid,"berneo",position,appearance,movement,false,receive,stats);
   game.handle(5,1,guardHit,receive,"guard-hit-"+std::to_string(attempt));
  }
  check(!scene->map("berneo")->at(guard.eid).alive,"PK hit did not kill city guard");
  check(game.character().at("moral_value")==moral-3500&&store.character("combat-fixture",eid).at("moral_value")==moral-3500,
        "Guard kill failed to persist the 3500 moral penalty");
  game.handle(5,1,guardHit,receive,"guard-dead-replay");
  check(game.character().at("moral_value")==moral-3500&&game.character().at("exp")==experience,
        "Repeated guard death charged moral or awarded experience");
  check(store.db().query("SELECT COUNT(*) AS count FROM ground_items")[0].at("count")==lootBefore,"Guard dropped monster loot");
  check(!store.db().query("SELECT 1 FROM spawn_deaths WHERE map_name=? AND spawn_index=? AND npc_id=?",
      {scope,std::int64_t(guard.spawn_index),std::int64_t(guard.npc_id)}).empty(),"Guard respawn was not saved");
 }
 std::cout<<"PASS: native normal combat/rate/identity; authoritative shared death/respawn; reward rollback; overkill merit; WorldSession kill XP/Fletta persistence; drop visibility/TTL; atomic pickup and channel isolation.\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
