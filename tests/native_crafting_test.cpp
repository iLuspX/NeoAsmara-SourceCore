#include "native/game.hpp"
#include <algorithm>
#include <iostream>
#include <thread>
using namespace rosh;
static void check(bool pass,const char* message){if(!pass)throw std::runtime_error(message);}
template<class F>static void fails(F action,const char* message){bool failed=false;try{action();}catch(const std::exception&){failed=true;}check(failed,message);}
int wmain(int argc,wchar_t**argv){try{
 check(argc==2&&fs::exists(fs::path(argv[1])/"GM_TEST_FIXTURE"),"Isolated fixture required");
 const fs::path root=argv[1];Content content(root);LegacyStore store(root/"Database/karos_gameplay.sqlite");
 const auto catalog=content.get("data/skill_catalog.json").at("crafting");
 check(catalog.at("recipes").size()==1243&&catalog.at("recipes").at("100001").at("item")==54603,"Native recipe/output namespace lost");
 int capped=0;for(const auto& r:catalog.at("recipes")){
  check(r.at("chance_basis_points").get<int>()<=10000,"Crafting chance exceeds 100 percent");
  if(r.at("source_chance_basis_points").get<int>()>10000)++capped;
 }check(capped==181,"Native chance audit changed");
 Inventory inventory(store.db(),content);Wallet wallet(store.db(),inventory);const std::uint32_t eid=955501;
 wallet.apply(eid,10000,"craft-test-money");
 auto quantity=[&](std::uint32_t id){std::int64_t total=0;for(const auto& item:inventory.items(eid))if(item.item==id)total+=item.quantity;return total;};
 const auto recipe=catalog.at("recipes").at("1002");
 inventory.grant_once(eid,{{63004,16},{63020,6},{95004,2}},"craft-test-materials");
 check(craft_inventory(store.db(),inventory,eid,recipe,"craft-test-success",now(),.9999),"Native 100-percent recipe failed");
 check(quantity(1002)==1&&quantity(63004)==8&&quantity(63020)==3&&quantity(95004)==1&&wallet.balance(eid)==9960,"Crafting output/materials/fee wrong");
 check(craft_inventory(store.db(),inventory,eid,recipe,"craft-test-success",now(),0),"Successful replay changed result");
 check(quantity(1002)==1&&quantity(63004)==8&&wallet.balance(eid)==9960,"Crafting replay consumed or granted twice");
 auto altered=recipe;altered["carats"]=1;
 fails([&]{craft_inventory(store.db(),inventory,eid,altered,"craft-test-success",now(),0);},"Altered replay accepted");
 store.db().exec("CREATE TRIGGER craft_fail BEFORE INSERT ON native_craft_operations BEGIN SELECT RAISE(ABORT,'isolated craft failure'); END");
 fails([&]{craft_inventory(store.db(),inventory,eid,recipe,"craft-test-rollback",now(),0);},"Forced crafting save failure ignored");
 store.db().exec("DROP TRIGGER craft_fail");
 check(quantity(1002)==1&&quantity(63004)==8&&wallet.balance(eid)==9960,"Failed craft did not roll back inventory/wallet");
 auto failure=recipe;failure["chance_basis_points"]=1000;
 check(!craft_inventory(store.db(),inventory,eid,failure,"craft-test-rng-failure",now(),.1),"10-percent roll boundary incorrect");
 check(quantity(1002)==1&&quantity(63004)==0&&wallet.balance(eid)==9920,"Failed chance did not consume materials/fee exactly once");
 fails([&]{craft_inventory(store.db(),inventory,eid,recipe,"craft-test-empty",now(),0);},"Craft succeeded without materials");
 check(wallet.balance(eid)==9920,"Rejected craft charged fee");
 fails([&]{craft_inventory(store.db(),inventory,eid,recipe,"craft-test-roll",now(),1);},"Out-of-range RNG accepted");
 auto scene=std::make_shared<WorldScene>(content);std::optional<Entity> npc;
 // Session entry can replace the scene snapshot; retain the selected actor value.
 for(const auto& [actor,row]:*scene->map("berneo"))if(row.npc_id==9300011){npc=row;break;}
 check(npc.has_value(),"Native Berneo crafting NPC missing");
 const auto account="craft-flow-"+token(12);const std::uint32_t actor=955502;
 Json character={{"eid",actor},{"name","CraftFixture"},{"cls",810},{"profession_grade",2},{"level",115},{"exp",Progression().threshold(115)},
  {"hp",100},{"mp",100},{"fletta",0},{"skill_points",2},{"moral_value",0},{"map_id",19003},{"skills",Json::object()}};
 store.db().exec("INSERT INTO accounts VALUES(?,?)",{account,Json{{"chars",Json::array({character})}}.dump()});
 store.db().exec("CREATE TABLE IF NOT EXISTS character_locations(eid INTEGER PRIMARY KEY,map_name TEXT NOT NULL,x REAL NOT NULL,y REAL NOT NULL,z REAL NOT NULL,angle REAL NOT NULL,updated_at REAL NOT NULL)");
 store.db().exec("INSERT INTO character_locations VALUES(?,'berneo',?,?,?,0,?)",{std::int64_t(actor),npc->position.x,npc->position.y,npc->position.z,now()});
 Session identity;identity.id=token(20);identity.identity.id=account;identity.identity.locale="en";identity.channel=1;identity.expires=now()+600;
 struct Packet{int cat,sub;Bytes body;};std::vector<Packet> packets;
 const auto send=[&](unsigned char cat,unsigned char sub,const Bytes&body){packets.push_back({cat,sub,body});};
 WorldSession game(content,store,identity,root,actor,scene);Bytes ready(21);put32(ready,1,actor,true);
 for(int axis=0;axis<3;++axis)putfloat(ready,5+axis*4,game.character().at("pos").at(axis));
 game.handle(4,0,ready,send);
 Bytes learn(10);put32(learn,0,actor,true);put32(learn,4,15201,true);put16(learn,8,36,true);
 game.handle(5,6,learn,send,"craft-native-acquisition");
 check(game.character().at("skills").value("15201",0)==1&&game.character().at("skill_points")==1,"Native Gunner crafting skill learning failed");
 wallet.apply(actor,10000,"craft-flow-money");
 inventory.grant_once(actor,{{63004,8},{63020,3},{95004,1}},"craft-flow-materials");
 auto seen=[&](int sub,int status){return std::any_of(packets.begin(),packets.end(),[&](const auto& packet){return packet.cat==6&&packet.sub==sub&&get16(packet.body,packet.body.size()-2,true)==status;});};
 Bytes request(14);put32(request,0,actor,true);put32(request,4,npc->eid,true);put32(request,8,1002,true);put16(request,12,0x6f,true);
 packets.clear();game.handle(6,0x16,request,send,"craft-flow-npc");
 check(seen(0x16,0x6f)&&wallet.balance(actor)==9960,"Native NPC craft did not execute");
 game.handle(6,0x16,request,send,"craft-flow-npc");check(wallet.balance(actor)==9960,"Native NPC replay charged twice");
 auto wrong=request;put32(wrong,4,0,true);packets.clear();game.handle(6,0x16,wrong,send,"craft-flow-wrong-npc");
 check(seen(0x16,0x73)&&wallet.balance(actor)==9960,"Wrong NPC craft was accepted");
 const auto pc=catalog.at("recipes").at("100001");std::vector<std::pair<std::uint32_t,int>> materials;
 for(const auto& material:pc.at("materials"))materials.emplace_back(material.at("item"),material.at("quantity").get<int>()*2);
 inventory.grant_once(actor,materials,"craft-flow-pc-materials");
 Bytes action(18);put32(action,0,actor,true);put32(action,4,15201,true);put32(action,8,1,true);put32(action,12,100001,true);put16(action,16,0x177,true);
 auto invalid=action;put32(invalid,4,15104,true);packets.clear();game.handle(6,0x3f,invalid,send,"craft-flow-pc-invalid");
 check(seen(0x3f,0x73),"Unlearned native crafting skill accepted");
 packets.clear();game.handle(6,0x3f,action,send,"craft-flow-pc-cancel");check(seen(0x3f,0x177),"Native PC start missing");
 auto cancel=action;put16(cancel,16,0x176,true);game.handle(6,0x3f,cancel,send,"craft-flow-pc-stop");
 check(seen(0x3f,0x179)&&wallet.balance(actor)==9960,"Native cancellation consumed materials or fee");
 packets.clear();game.handle(6,0x3f,action,send,"craft-flow-pc-complete");
 check(seen(0x3f,0x177),"Second native PC start missing");
 std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(pc.at("seconds").get<double>()*1000)+100));
 game.admin_tick(send);check(seen(0x3f,0x178),"Server-timed native PC crafting did not complete");
 check(wallet.balance(actor)==9960-pc.at("carats").get<std::int64_t>(),"PC crafting charged wrong fee");
 const auto after=wallet.balance(actor);game.handle(6,0x3f,action,send,"craft-flow-pc-complete");check(wallet.balance(actor)==after,"PC completion replay charged twice");
 check(std::none_of(packets.begin(),packets.end(),[](const auto& packet){return packet.cat==4&&packet.sub==4;}),"Crafting reloaded the map");
 std::cout<<"PASS: native recipes, capped rates, exact fee/materials/output, RNG/replay/rollback; NPC range/link and PC learned skill/start/cancel/server-timed completion; no map reload\n";
 return 0;
}catch(const std::exception&error){std::cerr<<error.what()<<"\n";return 1;}}
