#include "native/game.hpp"
#include "native/inventory.hpp"
#include <iostream>
#include <cmath>
using namespace rosh;
static void check(bool ok,const char*message){if(!ok)throw std::runtime_error(message);}
struct Packet{int cat,sub;Bytes body;};
int wmain(int argc,wchar_t**argv){try{
 check(argc==3,"Use isolated fixture and captured mount examples");fs::path root=argv[1];check(fs::is_regular_file(root/"GM_TEST_FIXTURE"),"Refusing live databases");
 Content content(root);LegacyStore store(root/"Database/karos_gameplay.sqlite");auto scene=std::make_shared<WorldScene>(content);auto evidence=Json::parse(read_file(argv[2]));
 auto seed=[&](std::uint32_t eid,const std::string&map,int mapid,const Json&pos){
  std::string account="capture-"+std::to_string(eid)+token(6);Json ch={{"eid",eid},{"name","CaptureReplay"},{"cls",100},{"level",107},{"exp",Progression().threshold(107)},{"hp",100},{"mp",100},{"fletta",0},{"fletta_energy",0},{"skill_points",0},{"moral_value",0},{"map_id",mapid}};
  store.db().exec("INSERT INTO accounts VALUES(?,?)",{account,Json{{"chars",Json::array({ch})}}.dump()});
  store.db().exec("CREATE TABLE IF NOT EXISTS character_locations(eid INTEGER PRIMARY KEY,map_name TEXT NOT NULL,x REAL NOT NULL,y REAL NOT NULL,z REAL NOT NULL,angle REAL NOT NULL,updated_at REAL NOT NULL)");
  store.db().exec("INSERT INTO character_locations VALUES(?,?,?,?,?,?,?)",{std::int64_t(eid),map,pos[0].get<double>(),pos[1].get<double>(),pos[2].get<double>(),0.,now()});
  Session session;session.id=token(20);session.identity.id=account;session.channel=1;session.expires=now()+600;return session;
 };
 auto ready=[&](WorldSession&game,const WorldSession::Sender&send){Bytes b(21);put32(b,1,game.eid(),true);for(int i=0;i<3;i++)putfloat(b,5+i*4,game.character()["pos"][i]);game.handle(4,0,b,send);};
 std::vector<Packet>packets;auto send=[&](unsigned char c,unsigned char s,const Bytes&b){packets.push_back({c,s,b});};
 int captured=0;
 for(const auto&row:evidence.at("mounts"))if(row.at("direction")=="C"){
  auto b=unhex(row.at("body"));check(b.size()==36&&get32(b,5,true)==5&&get32(b,9,true)==1&&get16(b,34,true)==0x24,"Captured mount action contract changed");++captured;
 }
 check(captured==80,"Missing captured mount exchanges");
 auto session=seed(82001,"berneo",19003,Json::array({9020.,6590.,627.}));
 {
  WorldSession game(content,store,session,root,82001,scene);ready(game,send);Inventory bag(store.db(),content);
  auto request=unhex(evidence.at("mounts")[0].at("body"));put32(request,1,82001,true);put32(request,30,82001,true);
  packets.clear();game.handle(5,3,request,send,"mount-unowned");check(packets.size()==1&&packets[0].cat==11,"Unowned mount accepted");
  Bytes earlyMode(11);put32(earlyMode,0,82001,true);earlyMode[4]=2;packets.clear();game.handle(3,2,earlyMode,send,"unowned-mode");check(packets.size()==1&&packets[0].cat==11,"Mount visual mode bypassed ownership/cast");
  bag.grant_once(82001,{{48001,1}},"capture-mount");ItemInstance mount;for(auto&i:bag.items(82001))if(i.item==48001)mount=i;
  auto address=Inventory::address(mount.slot);Bytes equip(11);put32(equip,0,82001,true);equip[4]=address.first;equip[5]=address.second;equip[6]=6;put32(equip,7,48001,true);game.handle(6,0,equip,send,"capture-equip");
  float base=0;for(auto&p:packets)if(p.cat==8&&p.sub==1)base=getfloat(p.body,25);check(base>0,"Baseline movement gauge absent");
  put32(request,9,0,true); // Additional Genesis capture sends 0; server must reply 1.
  packets.clear();game.handle(5,3,request,send,"mount-owned");check(packets.size()>=2&&packets[0].cat==8&&packets[0].sub==1&&packets[1].cat==5&&packets[1].sub==3&&get32(packets[1].body,9,true)==1,"Mount must publish gauge before canonical native action");
  check(std::abs(getfloat(packets[0].body,25)-base)<.01,"Mount movement changed before the native transition completed");for(auto&p:packets)check(!(p.cat==4&&p.sub==2),"Mount recreated own actor and interrupted animation");
  auto completion=unhex(evidence.at("completions")[0].at("body"));put32(completion,1,82001,true);putfloat(completion,9,99999.);packets.clear();game.handle(5,4,completion,send,"mount-finish");check(packets.size()==3&&packets[0].cat==5&&packets[0].sub==4&&get32(packets[0].body,5,true)==5,"Mount completion must precede native parameter and gauge updates");check(packets[1].cat==8&&packets[1].sub==2&&packets[1].body.size()==788&&packets[2].cat==8&&packets[2].sub==1,"Mount completion omitted captured parameter/gauge refresh");check(std::abs(getfloat(packets[1].body,76)-base-75)<.01&&std::abs(getfloat(packets[2].body,25)-base-75)<.01,"Mount completion did not publish equipped mount speed");check(getfloat(packets[0].body,9)==game.character()["pos"][0].get<float>(),"Mount completion relayed a forged position");check(std::abs(getfloat(packets[1].body,72)-base*.695)<.01&&std::abs(getfloat(packets[2].body,29)-base*.695)<.01,"Mount transition changed native walking speed");
  packets.clear();game.handle(5,4,completion,send,"mount-finish-replay");check(packets.empty(),"Mount completion replay published twice");
  // Retail 03/02 is an actor action, not chat. It alone enables mounted visuals.
  auto mode=unhex(evidence.at("native_mode_evidence_20261117").at("packets")[0].at("body"));put32(mode,0,82001,true);
  packets.clear();game.handle(3,2,mode,send,"mount-native-mode");
  check(!packets.empty()&&packets[0].cat==3&&packets[0].sub==2&&packets[0].body.size()==11&&packets[0].body[4]==2&&get32(packets[0].body,6,true)==82001,"Missing native mount visual mode acknowledgment");
  auto spoof=mode;put32(spoof,0,82002,true);packets.clear();game.handle(3,2,spoof,send,"mount-foreign-mode");check(packets.size()==1&&packets[0].cat==11,"Foreign mount actor accepted");

  bag.grant_once(82001,{{54209,1}},"capture-mounted-potion");ItemInstance potion;bool potionOwned=false;for(auto&i:bag.items(82001))if(i.item==54209){potion=i;potionOwned=true;break;}check(potionOwned,"Mounted potion fixture missing");
  auto potionAddress=Inventory::address(potion.slot);Bytes use(16);put32(use,0,82001,true);use[4]=potionAddress.first;use[5]=potionAddress.second;put32(use,6,54209,true);put32(use,11,82001,true);
  packets.clear();game.handle(6,2,use,send,"capture-mounted-potion");bool potionAck=false,potionStats=false;for(auto&p:packets){if(p.cat==6&&p.sub==2)potionAck=true;if(p.cat==8&&p.sub==2)potionStats=true;check(!(p.cat==4&&p.sub==2&&get32(p.body,2)==82001),"Mounted potion recreated own actor and interrupted riding");}check(potionAck&&potionStats,"Mounted potion failed to apply immediate effect");
  auto observerSession=seed(82002,"berneo",19003,Json::array({9020.,6590.,627.}));
  {WorldSession observer(content,store,observerSession,root,82002,scene);packets.clear();ready(observer,send);bool appearance=false,action=false,complete=false,mode=false;for(auto&p:packets){if(p.cat==4&&p.sub==2&&get32(p.body,2)==82001)appearance=true;if(p.cat==5&&p.sub==3&&get32(p.body,1,true)==82001){check(appearance,"Rider action before actor appearance");action=true;}if(p.cat==5&&p.sub==4&&get32(p.body,1,true)==82001)complete=true;if(p.cat==3&&p.sub==2&&get32(p.body,0,true)==82001&&p.body[4]==2){check(appearance,"Mount mode before actor appearance");mode=true;}}check(appearance&&action&&complete&&mode,"New observer cannot see an already mounted player");}
  store.db().exec("DELETE FROM equipment_instances WHERE player_eid=82001 AND slot=6");packets.clear();game.admin_tick(send);bool reset=false;for(auto&p:packets)if(p.cat==8&&p.sub==1)reset=std::abs(getfloat(p.body,25)-base)<.01;check(reset,"Removing mount leaves its speed bonus active");
  bool refreshed=false;for(auto&p:packets)if(p.cat==4&&p.sub==2&&get32(p.body,2)==82001)refreshed=true;check(refreshed,"Deferred mounted appearance never refreshed after dismount");
 }
 constexpr std::uint32_t rider = 82006, sacred_tiger = 1000000336;
 auto tiger_session = seed(rider, "berneo", 19003, Json::array({9020.,6590.,627.}));
 {
  WorldSession game(content, store, tiger_session, root, rider, scene);
  ready(game, send);
  Inventory inventory(store.db(), content);
  inventory.grant_once(rider, {{sacred_tiger, 1}}, "sacred-tiger-fixture");
  const auto items = inventory.items(rider);
  const auto tiger = std::find_if(items.begin(), items.end(), [](const auto& item){ return item.item == sacred_tiger; });
  check(tiger != items.end(), "Sacred Tiger did not reach the bag");
  const auto address = Inventory::address(tiger->slot);
  Bytes equip(11);
  put32(equip, 0, rider, true);
  equip[4] = address.first; equip[5] = address.second; equip[6] = 6;
  put32(equip, 7, sacred_tiger, true);
  game.handle(6, 0, equip, send, "equip-sacred-tiger");
  auto request = unhex(evidence.at("mounts")[0].at("body"));
  put32(request, 1, rider, true); put32(request, 30, rider, true);
  packets.clear();
  game.handle(5, 3, request, send, "ride-sacred-tiger");
  check(packets.size() >= 2 && packets[1].cat == 5 && packets[1].sub == 3,
        "Sacred Tiger did not begin its riding action");
  store.db().exec("DELETE FROM equipment_instances WHERE player_eid=? AND slot=6", {std::int64_t(rider)});
  packets.clear();
  game.admin_tick(send);
  check(std::any_of(packets.begin(), packets.end(), [=](const auto& packet){
   return packet.cat == 4 && packet.sub == 2 && get32(packet.body, 2) == rider && get32(packet.body, 92) == 0;
  }), "Forced dismount without a deferred refresh kept the old model");
 }
 auto typed=content.database("database/content_typed.sqlite");
 auto gate=typed->query("SELECT o8_f64 AS x,o16_f64 AS y FROM mapWarpGate WHERE o0_i=1900306").at(0);
 auto portalSession=seed(82003,"berneo",19003,Json::array({gate.at("x"),gate.at("y"),627.}));
 {WorldSession game(content,store,portalSession,root,82003,scene);ready(game,send);Bytes b(6);put32(b,0,1900306,true);put16(b,4,0x123,true);packets.clear();game.handle(4,6,b,send,"capture-border-portal");check(packets.size()>=2&&packets[0].cat==4&&packets[0].sub==4&&get32(packets[0].body,4,true)==19002&&packets[1].sub==6,"Captured inter-map portal handshake missing");check(store.character(portalSession.identity.id,82003).at("map_id")==19002,"Map ID not saved with portal position");ready(game,send);auto before=game.character().at("pos");packets.clear();game.handle(4,6,b,send,"foreign-portal");check(game.character().at("pos")==before&&packets.size()==1&&packets[0].cat==11,"Foreign-map portal accepted");}
 auto internalGate=typed->query("SELECT o8_f64 AS x,o16_f64 AS y FROM mapWarpGate WHERE o0_i=295104").at(0);
 auto internalSession=seed(82004,"Antcave",29501,Json::array({internalGate.at("x"),internalGate.at("y"),0.}));
 {WorldSession game(content,store,internalSession,root,82004,scene);ready(game,send);Bytes b(6);put32(b,0,295104,true);put16(b,4,0x123,true);packets.clear();game.handle(4,6,b,send,"capture-ant-floor");bool ack=false,own=false;for(auto&p:packets){check(!(p.cat==4&&p.sub==4),"Internal floor portal reloaded the map");if(p.cat==4&&p.sub==6)ack=true;if(p.cat==4&&p.sub==2&&get32(p.body,2)==82004)own=true;}check(ack&&own&&game.ready()&&game.character()["pos"][0].get<double>()>13000,"Antcave floor transition missing");}
 for(auto name:{"catacomb02","sewer_01","Labyrinth_02","catacomb02event#29614","catacombBelltower#29416"}){auto actors=scene->map(std::string(name)+"#solo:test");check(!actors->empty(),"Captured dungeon is empty");for(auto&[id,a]:*actors)check(get32(scene->appearance(a),68)>0,"Dungeon mob lacks a verified model reference");}
 auto a=scene->map("catacomb02#29003#run:one"),b=scene->map("catacomb02#29003#run:two");check(a!=b&&!a->empty()&&a->size()==b->size(),"Private dungeons share mutable actors");check(scene->map("Antcave")==scene->map("Antcave"),"Open Antcave world must be shared in its channel");
 auto variant=seed(82005,"catacomb02event",29614,Json::array({1452.,1037.,470.}));
 {WorldSession game(content,store,variant,root,82005,scene);packets.clear();game.enter(send);check(get32(packets[1].body,4,true)==29614,"Dungeon difficulty changed during login");}
 std::cout<<"PASS: 80 captured mount contracts; owned equipment, gauge/action/completion, mounted potion without own actor recreation, late observer, removed-mount cleanup and deferred appearance; border/floor portals; dungeon models, independent scenes and saved difficulty\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
