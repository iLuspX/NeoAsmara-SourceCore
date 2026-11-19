#include "native/game.hpp"
#include "native/control.hpp"
#include "native/runtime.hpp"
#include "native/economy.hpp"
#include <iostream>
#include <cmath>
using namespace rosh;
static void check(bool value,const char*message){if(!value)throw std::runtime_error(message);}
struct Packet{int cat,sub;Bytes body;};
int wmain(int argc,wchar_t**argv){try{
 check(argc==2,"Use an isolated fixture");fs::path root=argv[1];check(fs::is_regular_file(root/"GM_TEST_FIXTURE"),"Refusing live database");
 Content content(root);LegacyStore store(root/"Database/karos_gameplay.sqlite");Sqlite roles(root/"Database/karos_roles.sqlite");
 constexpr std::uint32_t eid=74101,targetid=74102;std::string actor="command-fixture-"+token(8),other="target-fixture-"+token(8),generation=token(20),password=token(20);
 Json ch={{"eid",eid},{"name","NativeCommandAdmin"},{"cls",100},{"level",1},{"exp",0},{"hp",100},{"mp",100},{"skill_points",0},{"fletta",0},{"fletta_energy",0},{"moral_value",0}};auto target=ch;target["eid"]=targetid;target["name"]="NativeCommandTarget";
 store.db().exec("INSERT INTO accounts(id,payload) VALUES(?,?)",{actor,Json{{"chars",Json::array({ch})}}.dump()});store.db().exec("INSERT INTO accounts(id,payload) VALUES(?,?)",{other,Json{{"chars",Json::array({target})}}.dump()});
 roles.exec("INSERT INTO staff_roles(account_id,character_eid,character_name,role,auth_generation) VALUES(?,?,?,?,?)",{actor,std::int64_t(eid),std::string("NativeCommandAdmin"),std::string("Admin"),generation});
 Session identity;identity.identity.id=actor;identity.id=token(20);identity.channel=1;identity.expires=now()+600;auto scene=std::make_shared<WorldScene>(content);
 check(command_tokens("//speedattack 5")==std::vector<std::string>({"attackspeed","5"}),"Double-slash alias failed");check(command_tokens("!give 54005")[0]=="!give","Old prefix still normalized as command");check(command_tokens("/where")[0]=="/where","Single slash still normalized");
 check(normalized_native_command(8,"/stafflogin test")==std::optional<std::string>("//stafflogin test"),"Native transport consumed slash not recovered");check(normalized_native_command(0,"/stafflogin test")==std::optional<std::string>("//stafflogin test"),"Consumed slash credential command leaked into chat");check(!normalized_native_command(0,"I can't chat"),"Ordinary apostrophe broke chat");
 {
  StaffChannel staffchat(store.db());Sqlite second(root/"Database/karos_gameplay.sqlite");StaffChannel otherChannel(second);auto transport=actor+":transport";
  auto start=staffchat.latest();bool denied=false;try{staffchat.post("Player",actor,eid,"ForgedStaff","private","staff-chat-denied",now());}catch(...){denied=true;}check(denied&&staffchat.latest()==start,"Player posted in staff-only channel");
  staffchat.post("Admin",transport,eid,"NativeCommandAdmin","cross-channel secret","staff-chat-one",now());auto rows=otherChannel.poll(start);check(rows.size()==1&&rows[0].at("message")=="cross-channel secret","Separate WorldServer DB connection cannot see staff message");
  staffchat.post("Admin",transport,eid,"NativeCommandAdmin","cross-channel secret","staff-chat-one",now());check(otherChannel.poll(start).size()==1,"Staff message replay duplicated delivery");
  denied=false;try{staffchat.post("Admin",transport,eid,"NativeCommandAdmin","changed replay","staff-chat-one",now());}catch(...){denied=true;}check(denied,"Staff message replay payload changed");
 }
 for(auto&[name,policy]:staff_command_policies()){bool denied=false;try{authorize_staff_command("Player",name,policy.min_args);}catch(...){denied=true;}check(denied==(policy.level>0),"Public/staff policy boundary incorrect");authorize_staff_command("Admin",name,policy.min_args);}
 std::vector<Packet> packets;auto send=[&](unsigned char cat,unsigned char sub,const Bytes&body){packets.push_back({cat,sub,body});};
 auto ready=[&](WorldSession&game){Bytes body(21);put32(body,1,eid,true);putfloat(body,5,9020);putfloat(body,9,6590);putfloat(body,13,627);game.handle(4,0,body,send);};
 {
  WorldSession game(content,store,identity,root,eid,scene);ready(game);
  auto command=[&](const std::string&text,const std::string&event="command-fixture",bool native=false){Bytes body(native?7:9);if(native){body[5]=25;}else put32(body,0,eid,true);std::string name="ForgedAdminName";body.insert(body.end(),name.begin(),name.end());body.push_back(0);body.insert(body.end(),text.begin(),text.end());body.push_back(0);packets.clear();check(game.handle(3,native?8:0,body,send,event),"Command packet not consumed");};
  auto reply=[&](){std::string text;for(auto&p:packets)if(p.cat==3&&p.sub==0){check(p.body.size()>17&&get32(p.body,0,true)==eid&&get32(p.body,4,true)==1,"Command reply not addressed to its owner");auto sender=std::find(p.body.begin()+9,p.body.end(),0);check(std::string(p.body.begin()+9,sender)=="Server"&&sender!=p.body.end(),"Command feedback has wrong native sender");auto end=std::find(sender+1,p.body.end(),0);check(end!=p.body.end(),"Command feedback lacks terminator");text+=std::string(sender+1,end)+"\n";}return text;};
  command("//");check(reply().find("//staffhelp")!=std::string::npos,"Bare prefix gave no help");
  command("//unknown-command");check(!reply().empty(),"Unknown command silently ignored");
  command("//stafflogin TooShort");check(reply().find("Senha staff")!=std::string::npos,"Authentication error invisible in world chat");

  command("//setlevel 20");check(game.character().at("level")==1,"Role bypassed second login");
  command("/staffregister "+password+" "+password);check(reply().find("cadastrada")!=std::string::npos,"Registration reply missing");command("stafflogin "+password);check(reply().find("autenticado")!=std::string::npos,"Staff authentication failed");
  {
   auto regularIdentity=identity;regularIdentity.id=token(20);regularIdentity.identity.id=other;regularIdentity.channel=2;
   WorldSession regular(content,store,regularIdentity,root,targetid,std::make_shared<WorldScene>(content));std::string received;
   auto receive=[&](unsigned char c,unsigned char s,const Bytes&b){if(c==3&&s==0)received.append(b.begin(),b.end());};
   Bytes load(21);put32(load,1,targetid,true);putfloat(load,5,9020);putfloat(load,9,6590);putfloat(load,13,627);regular.handle(4,0,load,receive);
   command("//sc mensagem exclusiva","staff-chat-world");check(reply().find("[STAFF]")!=std::string::npos&&reply().find("mensagem exclusiva")!=std::string::npos,"Staff command had no private visible delivery");
   received.clear();regular.admin_tick(receive);check(received.find("mensagem exclusiva")==std::string::npos,"Staff channel leaked to regular player");
   roles.exec("INSERT INTO staff_roles(account_id,character_eid,character_name,role,auth_generation) VALUES(?,?,?,?,?)",{other,std::int64_t(targetid),std::string("NativeCommandTarget"),std::string("Helper"),token(20)});
   StaffChannel channel(store.db());channel.post("Helper",actor+":helper",eid,"HelperFixture","helper-visible","helper-visible",now());Sleep(1100);received.clear();regular.admin_tick(receive);check(received.find("helper-visible")!=std::string::npos,"Promoted helper cannot receive across world channels");
   roles.exec("DELETE FROM staff_roles WHERE account_id=? AND character_eid=?",{other,std::int64_t(targetid)});channel.post("Admin",actor+":revoke",eid,"AdminFixture","revocation-secret","revocation-secret",now());Sleep(1100);received.clear();regular.admin_tick(receive);check(received.find("revocation-secret")==std::string::npos,"Revoked recipient retained staff channel access");
  }
  command("//setlevel 20");check(game.character().at("level")==20&&store.character(actor,eid).at("level")==20,"Setlevel not persisted");
  Inventory advancementInventory(store.db(),content);
  Wallet advancementWallet(store.db(),advancementInventory);
  advancementWallet.apply(eid,1000000,"profession-fixture-funds");
  auto requestProfession=[&](int destination,const std::string& event){
   Bytes request(14);put32(request,0,eid,true);put32(request,4,destination,true);put16(request,12,0x11a,true);
   packets.clear();game.handle(8,3,request,send,event);
   for(auto& packet:packets)if(packet.cat==8&&packet.sub==3)return get16(packet.body,12,true);
   throw std::runtime_error("Native profession request returned no result");
  };
  check(requestProfession(110,"profession-missing")==0x11e,"Native profession ignored missing certificate");
  advancementInventory.grant_once(eid,{{63007,1},{70211,1}},"profession-fixture-certs");
  auto professionFunds=advancementWallet.balance(eid);
  check(requestProfession(110,"profession-native-first")==0x11a&&store.character(actor,eid).at("cls")==110&&
        advancementWallet.balance(eid)==professionFunds-10000,"First native profession was not atomic");
  check(requestProfession(110,"profession-native-first")==0x11a&&advancementWallet.balance(eid)==professionFunds-10000,
        "Profession retry debited twice");
  check(requestProfession(111,"profession-early")==0x11d&&game.character().at("cls")==110,"Native profession ignored level requirement");
  command("//setlevel 45");
  check(requestProfession(111,"profession-native-second")==0x11a&&game.character().at("cls")==111&&
        advancementWallet.balance(eid)==professionFunds-110000,"Second native profession was not atomic");
  for(const auto& item:advancementInventory.items(eid))
   check(item.item!=63007&&item.item!=70211,"Successful advancement left certificate in inventory");
  command("//fletta 12345");check(store.character(actor,eid).at("fletta")==12345,"Fletta command not saved");
  command("attackspeed 5", "speed-five",true);check(game.character().at("attack_speed_multiplier")==5.,"Stripped native command transport missing");float fast=0;for(auto&p:packets)if(p.cat==8&&p.sub==2)fast=getfloat(p.body,88);check(fast>0,"Attack speed rate not published");command("//attackspeed 1");float normal=0;for(auto&p:packets)if(p.cat==8&&p.sub==2)normal=getfloat(p.body,88);check(std::abs(fast/normal-5)<.001,"Positive command multiplier lowered the native attack rate");
  command("//speedattack 2");check(game.character().at("attack_speed_multiplier")==2.,"Speedattack alias not wired");command("//attackspeed 6");check(game.character().at("attack_speed_multiplier")==2.,"Attack speed cap not enforced");
  command("//movespeed 3");check(game.character().at("move_speed_multiplier")==3.,"Movement speed command missing");command("//god on");check(game.character().at("god_mode")==true,"God toggle missing");
  command("//teleport 9100 6600 627");command("//passwall 100");check(std::abs(game.character().at("pos")[0].get<double>()-9200)<.01,"Passwall did not use heading");check(store.character(actor,eid).at("pos")==game.character().at("pos"),"Teleport position not saved");
  auto before=game.character();store.db().exec("CREATE TRIGGER command_fail_save BEFORE UPDATE ON accounts BEGIN SELECT RAISE(ABORT,'fixture save failed'); END");command("//setlevel 60");check(game.character().at("level")==before.at("level"),"Failed command save changed live level");store.db().exec("DROP TRIGGER command_fail_save");
  command("//addcash NativeCommandTarget 123 fixture", "cash-replay");command("//addcash NativeCommandTarget 123 fixture", "cash-replay");check(store.db().query("SELECT balance FROM premium_wallet WHERE account_id=?",{other})[0].at("balance")==123,"Cash replay credited twice");command("//addcash NativeCommandTarget 124 fixture", "cash-replay");check(store.db().query("SELECT balance FROM premium_wallet WHERE account_id=?",{other})[0].at("balance")==123,"Changed Cash replay accepted");
  command("//mute NativeCommandTarget character 1h fixture");Sqlite moderation(root/"Database/moderation.sqlite");check(!moderation.query("SELECT id FROM sanctions WHERE kind='mute' AND subject=? AND revoked IS NULL",{std::to_string(targetid)}).empty(),"Mute not applied");command("//unmute NativeCommandTarget character");check(moderation.query("SELECT id FROM sanctions WHERE kind='mute' AND subject=? AND revoked IS NULL",{std::to_string(targetid)}).empty(),"Unmute not applied");
  command("//ban NativeCommandAdmin account 1h fixture");check(moderation.query("SELECT id FROM sanctions WHERE kind='ban' AND subject=? AND revoked IS NULL",{actor}).empty(),"Equal-rank staff account sanction accepted");
  command("//setrole NativeCommandTarget Mod");check(character_role(roles,other,targetid)=="Mod","Cross-account role grant failed");command("//setrole NativeCommandTarget Player");check(character_role(roles,other,targetid).empty(),"Role removal failed");
  command("//item 54005");check(reply().find("ainda nao") ==std::string::npos&&reply().find("54005")!=std::string::npos,"Item lookup failed");command("//maps");check(reply().find("Mapas")!=std::string::npos,"Verified map list failed");
  command("//map Lupinel");check(!game.ready()&&game.character().at("map")!="berneo","Map command did not start transfer");bool changed=false,early=false;for(auto&p:packets){changed|=p.cat==4&&p.sub==4&&get32(p.body,4,true)==19002;early|=p.cat==4&&p.sub==2;}check(changed&&!early,"Warp spawned actor before native map-ready");Bytes ack(25);put32(ack,0,eid,true);game.handle(8,9,ack,send);ready(game);check(game.ready()&&std::abs(game.character().at("pos")[0].get<double>()-2440)<.01,"Verified captured map arrival lost");
  auto entities=scene->map(game.character().at("map"));std::uint32_t mob=0;for(auto&[id,e]:*entities)if(e.combat){mob=e.npc_id;break;}check(mob!=0,"Fixture map has no combat mobs");auto size=entities->size();command("//spawn "+std::to_string(mob)+" 2");check(scene->map(game.character().at("map"))->size()==size+2,"Summons absent from authoritative combat simulation");
  auto summoned=scene->map(game.character().at("map"));
  auto target=std::find_if(summoned->begin(),summoned->end(),[&](const auto& row){return row.second.npc_id==mob;});
  check(target!=summoned->end(),"GM inspection fixture missing");
  command("//mobinfo "+std::to_string(target->first));check(reply().find("HP ")!=std::string::npos,"GM mob info omitted current HP");
  command("//findmob "+target->second.name);check(reply().find("ID "+std::to_string(mob))!=std::string::npos,"GM mob search omitted verified ID");
  command("//bosstimers");check(!reply().empty(),"GM boss timers gave no feedback");
  bool modDenied=false;try{authorize_staff_command("Mod","mobinfo",1);}catch(...){modDenied=true;}
  check(modDenied,"GM-only inspection was granted to Mod");
  command("//respawn");command("//npc activate");command("//queststatus");command("//starter");check(reply().find("Starter")!=std::string::npos,"Starter seed command failed");
  Bytes targetAppear(254),targetMovement(34);std::string targetName="NativeCommandTarget";targetAppear[253]=static_cast<unsigned char>(targetName.size());targetAppear.insert(targetAppear.end(),targetName.begin(),targetName.end());targetAppear.push_back(0);scene->update_player("party-target",targetid,game.character().at("map"),game.character().at("pos"),targetAppear,targetMovement,false,send);
  command("//party invite NativeCommandTarget");scene->party("party-target",{"accept"});command("//party status");check(reply().find("Party de ")!=std::string::npos,"Party grouping missing");
  auto active=scene->map(game.character().at("map"));std::uint32_t victim=0;auto pos=game.character().at("pos");for(auto&[id,e]:*active)if(e.combat&&e.alive&&e.position.distance({pos[0],pos[1],pos[2]})<100)victim=id;check(victim!=0,"No summoned damage target nearby");Bytes attack(32);put32(attack,1,eid,true);put32(attack,28,victim,true);game.handle(5,1,attack,send,"ranking-hit");command("//damage solo");check(reply().find("dano solo")!=std::string::npos,"Solo damage ranking missing");command("//dano party");check(reply().find("dano party")!=std::string::npos,"Party damage ranking missing");
  auto ownerText=Json{{"staff_titles_owner_bound",true},{"account_id",actor}}.dump();auto ownerBytes=Bytes(ownerText.begin(),ownerText.end());write_file(root/"Config/Keys/owner_authority.private.json.dat",encrypt_content(ownerBytes,unprotect_key(root/"Config/Keys/content.key.dpapi"),"owner_authority.private.json"));
  command("//titlegrant NativeCommandTarget 1001 fixture","title-grant");check(!store.db().query("SELECT title_id FROM character_titles WHERE account_id=? AND character_eid=? AND title_id=1001",{other,std::int64_t(targetid)}).empty(),"Owner title grant not saved");command("//titlegrant NativeCommandAdmin 1001 fixture","self-title-grant");command("//titleuse 1001");check(store.db().query("SELECT title_id FROM character_title_selection WHERE character_eid=?",{std::int64_t(eid)})[0].at("title_id")==1001,"Title selection not saved");command("//titlerevoke NativeCommandAdmin 1001","self-title-revoke");check(store.db().query("SELECT title_id FROM character_title_selection WHERE character_eid=?",{std::int64_t(eid)}).empty(),"Revoked title remained selected");command("//itemstats arma");check(!reply().empty(),"Item attributes command missing");
  command("/help","native-slash",true);check(reply().find("//party")!=std::string::npos,"Native double-slash UI transport rejected");
  command("//map berneo");ready(game);auto travelDb=content.database("database/content_typed.sqlite");const Entity* gate=nullptr;Json route;
  auto berneoActors=scene->map("berneo");for(auto&[id,e]:*berneoActors){if(e.combat||!e.alive)continue;auto rows=travelDb->query("SELECT w.o16_i AS target,w.o20_i AS cost FROM mapWarp w JOIN mapWarpTargetPos p ON p.o0_i=w.o16_i WHERE w.o8_i=? AND p.o4_i!=19003",{std::int64_t(e.npc_id)});if(!rows.empty()){gate=&e;route=rows[0];break;}}
  check(gate!=nullptr,"No native travel NPC for test");command("//teleport "+std::to_string(gate->position.x)+" "+std::to_string(gate->position.y)+" "+std::to_string(gate->position.z));Inventory travelInv(store.db(),content);Wallet travelWallet(store.db(),travelInv);travelWallet.apply(eid,10000000,"travel-funds");auto travelBalance=travelWallet.balance(eid);Bytes warp(14);put32(warp,0,1,true);put32(warp,4,route.at("target"),true);put32(warp,8,gate->eid,true);put16(warp,12,0x123,true);packets.clear();check(game.handle(4,7,warp,send,"native-npc-warp"),"NPC warp not handled");check(!game.ready(),"NPC warp did not start map handshake");check(travelWallet.balance(eid)==travelBalance-std::max<std::int64_t>(0,route.at("cost")),"NPC warp fee incorrect");check(store.db().query("SELECT target_id FROM map_travel_events WHERE event_key='native-npc-warp'").size()==1,"NPC warp receipt missing");ready(game);
  std::uint32_t scroll=0;for(int id=57105;id<=57114;id++)if(!travelDb->query("SELECT o16_i FROM mapWarp WHERE o8_i=?",{std::int64_t(id)}).empty()){scroll=id;break;}check(scroll!=0,"No native return scroll");travelInv.grant_once(eid,{{scroll,2}},"travel-scroll");ItemInstance scrollInstance;for(auto&i:travelInv.items(eid))if(i.item==scroll)scrollInstance=i;auto address=Inventory::address(scrollInstance.slot);Bytes use(16);put32(use,0,eid,true);use[4]=address.first;use[5]=address.second;put32(use,6,scroll,true);put32(use,11,eid,true);game.handle(6,2,use,send,"native-scroll-warp");check(!game.ready(),"Return scroll did not start native transfer");check(travelInv.owned(eid,scrollInstance.slot,scroll,now()).quantity==1,"Return scroll not consumed exactly once");ready(game);
  command("//give 54005 1","prefix-seed");Inventory inv(store.db(),content);auto count=[&](){std::int64_t n=0;for(auto&i:inv.items(eid))if(i.item==54005)n+=i.quantity;return n;};auto qty=count();command("!give 54005 1","bang",true);check(count()==qty,"Old bang prefix activated mutation");command("/give 54005 1","single",false);check(count()==qty+1,"Consumed slash command was lost on normal chat transport");qty=count();
  roles.exec("UPDATE staff_roles SET role='Helper',auth_generation='revoked' WHERE account_id=?",{actor});command("//give 54005 1","revoked");check(count()==qty,"Role demotion kept write permissions");command("//stats");check(reply().find("Lv ")!=std::string::npos,"Helper read-only status unavailable");
 }
 // Exercise the scheduler/save handshake without stopping a real server.
 roles.exec("UPDATE staff_roles SET role='Admin',auth_generation=? WHERE account_id=?",{generation,actor});NativeControl controls(root);Sqlite runtime(root/"Database/runtime.sqlite");runtime.exec("INSERT INTO native_player_leases VALUES(?,?,?,?,?)",{std::int64_t(eid),std::string("scheduler-fixture"),actor,std::int64_t(1),now()+600});
 controls.schedule("shutdown",0,actor,eid,generation);check(!controls.poll(now()),"Lifecycle did not wait for saves");int saves=0;controls.poll_save("scheduler-fixture",[&]{++saves;});check(saves==1,"Save did not execute on owning session");check(controls.poll(now())==std::optional<std::string>("shutdown"),"Saved lifecycle never completed");
 controls.schedule("restart",0,actor,eid,generation);controls.poll(now());controls.poll_save("scheduler-fixture",[]{throw std::runtime_error("injected save failure");});check(!controls.poll(now()),"Restart executed despite failed save");check(runtime.query("SELECT status FROM native_lifecycle")[0].at("status")=="scheduled","Failed save did not postpone restart");check(controls.cancel()&&!controls.cancel(),"Countdown cancellation failed");
 controls.schedule("reset",10,actor,eid,generation);roles.exec("UPDATE staff_roles SET auth_generation='different' WHERE account_id=?",{actor});check(!controls.poll(now()+20),"Revoked actor's scheduled action executed");
 std::cout<<"PASS: // prefix in normal/native chat; legacy bang rejected; consumed slash compatibility and private visible chat replies; role matrix/second login/revocation; level/profession/Fletta persistence; failed-save rollback; attack duration division; movement/god/passwall; account Cash replay; moderation hierarchy; role grant/removal; verified map handshake; real combat summons; starter; save-gated lifecycle and cancellation.\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
