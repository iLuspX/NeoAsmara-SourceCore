#include "native/game.hpp"
#include "native/consumables.hpp"
#include "native/economy.hpp"
#include <algorithm>
#include <iostream>
using namespace rosh;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Packet{int cat,sub;Bytes body;};
int wmain(int argc,wchar_t**argv){try{
 check(argc==2,"Expected isolated super-skill fixture");const fs::path root=argv[1];
 check(fs::exists(root/"GM_TEST_FIXTURE"),"Refusing live player databases");
 Content content(root);LegacyStore store(root/"Database/karos_gameplay.sqlite");SkillBook book(content);
 auto scene=std::make_shared<WorldScene>(content);const auto catalog=content.get("data/skill_catalog.json");
 Position mentor_position{};bool mentor_found=false;
 for(const auto& [actor,npc]:*scene->map("berneo"))if(npc.npc_id==9300111){mentor_position=npc.position;mentor_found=true;break;}
 check(mentor_found,"Native Berneo instructor missing");
 store.db().exec("CREATE TABLE IF NOT EXISTS character_locations(eid INTEGER PRIMARY KEY,map_name TEXT NOT NULL,x REAL NOT NULL,y REAL NOT NULL,z REAL NOT NULL,angle REAL NOT NULL,updated_at REAL NOT NULL)");
 for(int family:{100,200,300,400,500,600,800}){
  const auto eid=static_cast<std::uint32_t>(940000+family);const auto account="super-flow-"+token(12);
  Json character={{"eid",eid},{"name","SuperFixture"},{"cls",family+10},{"profession_grade",2},{"level",115},{"exp",Progression().threshold(115)},{"hp",100},{"mp",100},{"fletta",100000},{"fletta_energy",0},{"skill_points",1000},{"moral_value",0},{"map_id",19003}};
  store.db().exec("INSERT INTO accounts VALUES(?,?)",{account,Json{{"chars",Json::array({character})}}.dump()});
  store.db().exec("INSERT INTO character_locations VALUES(?,'berneo',?,?,?,0,?)",{std::int64_t(eid),mentor_position.x,mentor_position.y,mentor_position.z,now()});
  Session identity;identity.id=token(20);identity.identity.id=account;identity.identity.locale="en";identity.channel=1;identity.expires=now()+600;
  std::uint32_t skill=0;Json rule;
  for(const auto& [key,node]:catalog.at("super_tree").items())if(node.at("family")==family&&node.at("branch")==1&&node.at("slot")==0){skill=static_cast<std::uint32_t>(std::stoul(key));rule=node;break;}
  check(skill&&catalog.at("skills").at(std::to_string(skill)).at("passive").get<bool>(),"Missing first native passive super-skill choice");
  std::vector<Packet> packets;auto send=[&](unsigned char cat,unsigned char sub,const Bytes& body){packets.push_back({cat,sub,body});};
  auto ready=[&](WorldSession& game){Bytes body(21);put32(body,1,eid,true);for(int axis=0;axis<3;++axis)putfloat(body,5+4*axis,game.character().at("pos").at(axis));game.handle(4,0,body,send);};
  Bytes request(16);put32(request,0,eid,true);put32(request,4,skill,true);put32(request,8,rule.at("fletta_costs").at(0),true);request[12]=0x20;put16(request,13,0x2a6,true);
  Json saved_progress;
  {
   WorldSession game(content,store,identity,root,eid,scene);ready(game);
   auto initial=std::find_if(packets.begin(),packets.end(),[](const auto& p){return p.cat==5&&p.sub==12;});
   check(initial!=packets.end()&&std::all_of(initial->body.begin()+6,initial->body.end(),[](auto b){return b==0;}),"Fresh session inherited another character's super skills");
   const auto before_modifiers=character_modifiers(content,game.character(),now());packets.clear();
   const auto operation="super-flow-learn-"+std::to_string(family);game.handle(5,14,request,send,operation);
   check(game.ready()&&game.character().at("skills").value(std::to_string(skill),0)==1,"Native investment did not learn the skill");
   const auto balance=100000-rule.at("fletta_costs").at(0).get<int>();
   check(game.character().at("fletta")==balance&&game.character().at("skill_points")==1000,"Native request spent the wrong currency");
   check(character_modifiers(content,game.character(),now())!=before_modifiers,"Learned native passive had no server modifier");
   check(std::any_of(packets.begin(),packets.end(),[](const auto& p){return p.cat==5&&p.sub==14&&p.body[12]==1&&get16(p.body,13,true)==0x2a6;}),"Native success reply missing");
   check(std::none_of(packets.begin(),packets.end(),[](const auto& p){return p.cat==4&&p.sub==4;}),"Learning a super skill reloaded the map");
   game.handle(5,14,request,send,operation);check(game.character().at("fletta")==balance,"Replay spent Fletta twice");
   auto changed=request;put32(changed,8,10,true);game.handle(5,14,changed,send,operation);
   check(game.character().at("fletta")==balance,"Altered replay spent Fletta");
   const auto before=game.character();store.db().exec("CREATE TRIGGER super_flow_fail BEFORE UPDATE ON accounts BEGIN SELECT RAISE(ABORT,'isolated super checkpoint failure'); END");
   game.handle(5,14,changed,send,"super-flow-rollback-"+std::to_string(family));
   check(game.character()==before,"Failed account save changed super-skill state or Fletta");store.db().exec("DROP TRIGGER super_flow_fail");
   Inventory stone_inventory(store.db(),content);Wallet stone_wallet(store.db(),stone_inventory);
   stone_inventory.grant_once(eid,{{600101,2},{600102,1}},"stone-fixture-"+std::to_string(family),true);
   auto locate=[&](std::uint32_t id){for(const auto& item:stone_inventory.items(eid))if(item.item==id)return item;throw std::runtime_error("Stone fixture item missing");};
   auto stone_request=[&](std::uint32_t id){const auto item=locate(id);const auto [bag,slot]=Inventory::address(item.slot);Bytes b(16);put32(b,0,eid,true);put32(b,4,skill,true);put32(b,8,id,true);b[12]=bag;b[13]=slot;put16(b,14,0x2a1,true);return b;};
   auto apply=stone_request(600101);const auto unfunded=game.character();
   game.handle(5,13,apply,send,"stone-unfunded-"+std::to_string(family));
   check(game.character()==unfunded&&locate(600101).quantity==2,"Failed stone fee consumed item or changed character");
   stone_wallet.apply(eid,100000,"stone-money-"+std::to_string(family));
   const auto stone_op="stone-apply-"+std::to_string(family);
   const auto mods_before_stone=character_modifiers(content,game.character(),now());
   game.handle(5,13,apply,send,stone_op);
   check(game.character().at("super_skill_progress").at(std::to_string(skill)).at("option")==600101,"Native stone apply failed");
   check(stone_wallet.balance(eid)==90000,"Native stone apply charged wrong fee");
   const auto mods_after_stone=character_modifiers(content,game.character(),now());
   check(mods_after_stone.at("1").at(0).get<double>()==mods_before_stone.value("1",Json::array({0.,0.})).at(0).get<double>()+2.,"Applied stone did not change server Strength");
   const auto stone_uid=game.character().at("super_skill_progress").at(std::to_string(skill)).at("source_uid");
   check(stone_uid==locate(600101).uid&&locate(600101).quantity==1,"Stone source UID or exact consumption lost");
   game.handle(5,13,apply,send,stone_op);
   check(stone_wallet.balance(eid)==90000&&locate(600101).quantity==1,"Replayed stone request charged or consumed twice");
   auto replacement=stone_request(600102);const auto before_replace=game.character();
   store.db().exec("CREATE TRIGGER stone_flow_fail BEFORE UPDATE ON accounts BEGIN SELECT RAISE(ABORT,'isolated stone checkpoint failure'); END");
   game.handle(5,13,replacement,send,"stone-failed-save-"+std::to_string(family));store.db().exec("DROP TRIGGER stone_flow_fail");
   check(game.character()==before_replace&&stone_wallet.balance(eid)==90000&&locate(600102).quantity==1,"Stone save failure did not roll back item, fee and character");
   game.handle(5,13,replacement,send,"stone-replace-"+std::to_string(family));
   check(stone_wallet.balance(eid)==70000&&game.character().at("super_skill_progress").at(std::to_string(skill)).at("option")==600102,"Stone replacement cost/state wrong");
   saved_progress=game.character().at("super_skill_progress");
   Inventory inventory(store.db(),content);Consumables consumables(store.db(),inventory,content);const auto definitions=content.get("data/consumable_effects.json");
   std::uint32_t reset=0;for(const auto& [key,effect]:definitions.items())if(effect.at("kind")=="skill_reset"){reset=static_cast<std::uint32_t>(std::stoul(key));break;}
   check(reset,"Missing native ordinary skill reset item");
   auto copy=game.character();std::uint32_t ordinary=0;
   for(const auto& [key,definition]:catalog.at("skills").items()){
    const auto id=static_cast<std::uint32_t>(std::stoul(key));
    if(book.allowed(family+10,id)&&!copy.at("skills").contains(key)&&definition.at("levels").contains("1")&&definition.at("levels").at("1").value("points",0)>0){ordinary=id;break;}
   }
   check(ordinary,"No ordinary skill available for reset regression");copy["skills"][std::to_string(ordinary)]=1;const auto ordinary_points=catalog.at("skills").at(std::to_string(ordinary)).at("levels").at("1").at("points").get<int>();
   inventory.grant_once(eid,{{reset,1}},"super-reset-fixture-"+std::to_string(family));auto items=inventory.items(eid);auto item=std::find_if(items.begin(),items.end(),[&](const auto& i){return i.item==reset;});check(item!=items.end(),"Reset item missing");
   check(consumables.use(copy,item->slot,reset,"super-reset-use-"+std::to_string(family),now()),"Ordinary skill reset failed");
   check(copy.at("skills").value(std::to_string(skill),0)==1&&copy.at("super_skill_progress")==saved_progress&&copy.at("fletta")==balance,"Ordinary reset erased or refunded Fletta super skills");
   check(copy.at("skill_points")==1000+ordinary_points,"Super-skill Fletta was refunded as ordinary skill points");
  }
  identity.id=token(20);packets.clear();WorldSession reconnected(content,store,identity,root,eid,scene);ready(reconnected);
  check(reconnected.character().at("skills").value(std::to_string(skill),0)==1&&reconnected.character().at("super_skill_progress")==saved_progress,"Reconnection lost native super-skill state");
  std::uint32_t alternative=0;
  for(const auto& [key,node]:catalog.at("super_tree").items())if(node==rule&&std::stoul(key)!=skill){alternative=static_cast<std::uint32_t>(std::stoul(key));break;}
  check(alternative,"Other native choice missing");
  Inventory final_inventory(store.db(),content);Wallet final_wallet(store.db(),final_inventory);
  final_inventory.grant_once(eid,{{140024,2}},"coupon-fixture-"+std::to_string(family),true);
  auto coupon=[&](){for(const auto& item:final_inventory.items(eid))if(item.item==140024)return item;throw std::runtime_error("Coupon fixture missing");};
  auto coupon_count=[&](){std::int64_t count=0;for(const auto& item:final_inventory.items(eid))if(item.item==140024)count+=item.quantity;return count;};
  const auto [coupon_bag,coupon_slot]=Inventory::address(coupon().slot);
  Bytes swap(12);put32(swap,0,skill,true);put32(swap,4,alternative,true);swap[8]=coupon_bag;swap[9]=coupon_slot;
  const auto blocked= reconnected.character();reconnected.handle(6,0x5d,swap,send,"coupon-blocked-stone-"+std::to_string(family));
  check(reconnected.character()==blocked&&coupon_count()==2,"Coupon consumed or skill changed with a stone equipped");
  Bytes remove(10);put32(remove,0,eid,true);put32(remove,4,skill,true);put16(remove,8,0x2a9,true);
  reconnected.handle(5,15,remove,send,"stone-delete-"+std::to_string(family));
  check(final_wallet.balance(eid)==60000&&reconnected.character().at("super_skill_progress").at(std::to_string(skill)).at("option")==0,"Native stone delete did not clear stone or charged wrong fee");
  reconnected.handle(5,15,remove,send,"stone-delete-"+std::to_string(family));
  check(final_wallet.balance(eid)==60000,"Repeated delete charged twice");
  auto partial=request;put32(partial,8,50,true);reconnected.handle(5,14,partial,send,"coupon-partial-"+std::to_string(family));
  const auto before_swap=reconnected.character();packets.clear();const auto swap_event="coupon-swap-"+std::to_string(family);
  reconnected.handle(6,0x5d,swap,send,swap_event);
  check(reconnected.character().at("skills").value(std::to_string(skill),0)==0&&reconnected.character().at("skills").value(std::to_string(alternative),0)==1,"Native coupon failed to switch choice");
  check(reconnected.character().at("super_skill_progress").at(std::to_string(alternative)).at("points")==50&&reconnected.character().at("fletta")==before_swap.at("fletta")&&reconnected.character().at("skill_points")==before_swap.at("skill_points"),"Coupon lost partial progress or changed Fletta/skill points");
  check(coupon_count()==1&&final_wallet.balance(eid)==60000,"Coupon consumed the wrong quantity or charged Carats");
  check(std::none_of(packets.begin(),packets.end(),[](const auto& p){return p.cat==4&&p.sub==4;}),"Coupon reloaded the map");
  reconnected.handle(6,0x5d,swap,send,swap_event);check(coupon_count()==1,"Replayed coupon consumed twice");
  auto reverse=swap;put32(reverse,0,alternative,true);put32(reverse,4,skill,true);
  const auto [remaining_bag,remaining_slot]=Inventory::address(coupon().slot);reverse[8]=remaining_bag;reverse[9]=remaining_slot;
  const auto after_swap=reconnected.character();store.db().exec("CREATE TRIGGER coupon_flow_fail BEFORE UPDATE ON accounts BEGIN SELECT RAISE(ABORT,'isolated coupon checkpoint failure'); END");
  reconnected.handle(6,0x5d,reverse,send,"coupon-failed-save-"+std::to_string(family));store.db().exec("DROP TRIGGER coupon_flow_fail");
  check(reconnected.character()==after_swap&&coupon_count()==1,"Failed coupon save did not roll back state and item");
  check(store.character(account,eid).at("super_skill_progress")==reconnected.character().at("super_skill_progress"),"Coupon choice/progress not saved");
 }
 std::cout<<"PASS: seven classes; native learning, stone apply/replace/delete and coupon choice; passive modifiers; exact fees/items; replay and forced-save rollback; coupon rank/partial-Fletta preservation; ordinary-reset separation; no map reload; reconnect persistence\n";return 0;
}catch(const std::exception&error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}}
