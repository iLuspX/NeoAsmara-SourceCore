#include "native/game.hpp"
#include "native/gm.hpp"
#include <iostream>
#include <cmath>
using namespace rosh;
static void check(bool ok,const char*message){if(!ok)throw std::runtime_error(message);}
int wmain(int argc,wchar_t**argv){try{
 check(argc==2,"Use isolated GM fixture");fs::path root=argv[1];check(fs::is_regular_file(root/"GM_TEST_FIXTURE"),"Refusing live database");Content content(root);LegacyStore store(root/"Database/karos_gameplay.sqlite");gm_schema(store.db());Inventory bag(store.db(),content);
 auto profiles=content.get("data/balance_profiles.json");
 for(int level:{30,90,115})for(int physical:{100,300,500,800})for(int caster:{400,600}){
  auto fighter=calculated_stats(profiles,physical,level),mage=calculated_stats(profiles,caster,level);
  check(fighter.at("attack")>mage.at("attack")&&mage.at("magic_attack")>fighter.at("magic_attack"),
        "Equal-level physical and magic profiles collapsed into the same damage specialization");
 }
 for(int cls:{100,200,300,400,500,600,800}){
  auto base=calculated_stats(profiles,cls,30);auto boosted=calculated_stats(profiles,cls,30,{{"13",{0.,40.}}});auto slow=calculated_stats(profiles,cls,30,{{"13",{0.,-20.}}});
  double normal=attack_interval(base.at("attack_speed")),fast=attack_interval(boosted.at("attack_speed"));check(std::abs(normal/fast-1.4)<1e-6,"Positive item/skill modifiers slow attacks");check(attack_interval(slow.at("attack_speed"))>normal,"Negative modifier accelerates attacks");check(std::abs(attack_interval(boosted.at("attack_speed"),5)*5-fast)<1e-6,"Staff multiplier not composed with item buffs");
 }

 {
  WorldScene world(content);auto entities=world.map("berneo");Entity chosen;bool found=false;for(auto&[id,mob]:*entities)if(mob.alive&&mob.combat&&mob.category=="normal"&&mob.hp>10){chosen=mob;found=true;break;}check(found,"No authoritative combat fixture");
  constexpr std::uint32_t attacker=987002;Json stats={{"hp",100000},{"level",chosen.level},{"attack",1.},{"attack_speed",1.},{"defense",0.},{"critical",0.}};Json pos={chosen.position.x,chosen.position.y,chosen.position.z};auto discard=[](unsigned char,unsigned char,const Bytes&){};
  world.update_player("speed-timing",attacker,"berneo",pos,Bytes(260),Bytes(34),false,discard,stats);Bytes hit(32);put32(hit,1,attacker,true);put32(hit,28,chosen.eid,true);world.strike("speed-timing",1,hit,100,{});auto hp=world.map("berneo")->at(chosen.eid).hp;
  world.strike("speed-timing",1,hit,100.8,{});check(world.map("berneo")->at(chosen.eid).hp==hp,"Normal-rate anti-spam bypassed");stats["attack_speed"]=1.5;world.update_player("speed-timing",attacker,"berneo",pos,Bytes(260),Bytes(34),false,discard,stats);world.strike("speed-timing",1,hit,100.8,{});check(world.map("berneo")->at(chosen.eid).hp<hp,"Real combat rejects faster attack after positive buff");
 }
 constexpr std::uint32_t eid=987001;std::string account="attack-speed-"+token(8);Json ch={{"eid",eid},{"name","SpeedFixture"},{"cls",110},{"level",120},{"exp",Progression().threshold(120)},{"hp",100},{"mp",100},{"fletta",0},{"fletta_energy",0},{"moral_value",0},{"skill_points",0}};store.db().exec("INSERT INTO accounts(id,payload) VALUES(?,?)",{account,Json{{"chars",Json::array({ch})}}.dump()});
 bag.grant_once(eid,{{1001,1},{54209,2}},"speed-test-inventory");ItemInstance weapon{},potion{};for(auto&i:bag.items(eid)){if(i.item==1001)weapon=i;if(i.item==54209)potion=i;}bag.equip(eid,weapon.slot,1001,110,120,7,now());
 Session identity;identity.identity.id=account;identity.id=token(20);identity.channel=1;identity.expires=now()+600;auto scene=std::make_shared<WorldScene>(content);
 double rate=0;std::vector<double> appearance;auto send=[&](unsigned char cat,unsigned char sub,const Bytes&b){if(cat==8&&sub==2){rate=getfloat(b,88);if(b.size()>=472)check(std::abs(getfloat(b,468)-attack_rate(calculated_stats(profiles,110,120).at("attack_speed")))<1e-6,"Intrinsic baseline was replaced by the equipped/buffed attack rate");}if(cat==4&&sub==2&&b.size()>=50)appearance.push_back(getfloat(b,46));};
 auto ready=[&](WorldSession&game){game.enter(send);Bytes body(21);put32(body,1,eid,true);putfloat(body,5,9020);putfloat(body,9,6590);putfloat(body,13,627);game.handle(4,0,body,send);check(rate>0,"Native speed stat absent");};double normal=0,gear=0,withPotion=0;
 {WorldSession game(content,store,identity,root,eid,scene);ready(game);normal=rate;}
 gm_equipment_apply(store.db(),bag,eid,weapon.uid,1001,{{"common",Json::array({{{"stat","attack_speed"},{"value",10}}})},{"craft",Json::array({{{"stat","attack_speed"},{"value",10}}})}});
 {WorldSession game(content,store,identity,root,eid,scene);ready(game);gear=rate;check(std::abs(gear/normal-1.2)<1e-5,"Real worn enchantments slow attacks");
  Bytes use(16);put32(use,0,eid,true);auto address=Inventory::address(potion.slot);use[4]=address.first;use[5]=address.second;put32(use,6,54209,true);put32(use,11,eid,true);game.handle(6,2,use,send,"speed-potion-use");withPotion=rate;check(std::abs(withPotion/normal-1.5)<1e-5,"Real potion and enchantments do not combine");check(!appearance.empty()&&std::abs(appearance.back()-withPotion)<1e-6,"Appearance and combat stats disagree");
  auto quantity=[&]{std::int64_t n=0;for(auto&i:bag.items(eid))if(i.item==54209)n+=i.quantity;return n;};check(quantity()==1,"Potion not consumed once");game.handle(6,2,use,send,"speed-potion-use");check(quantity()==1,"Potion replay consumed twice");game.handle(6,2,use,send,"speed-potion-cooldown");check(quantity()==1,"Potion cooldown ignored");
 }
 {WorldSession game(content,store,identity,root,eid,scene);ready(game);check(std::abs(rate-withPotion)<1e-6,"Potion speed lost on reconnect");}
 store.update_character(account,eid,[](Json&c){for(auto&effect:c["consumable_effects"])effect["expires"]=now()+.2;});
 {WorldSession game(content,store,identity,root,eid,scene);ready(game);Sleep(1300);game.admin_tick(send);check(std::abs(rate-gear)<1e-6,"Expired potion keeps attack speed");check(game.character().at("consumable_effects").empty(),"Expired potion not removed");}
 std::cout<<"PASS: seven classes; positive/negative speed modifiers; actual common/craft gear; potion use, stacking, cooldown, replay, reconnect and expiry; native stat/appearance agreement; 5x command composition.\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
