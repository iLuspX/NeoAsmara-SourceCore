#include "native/game.hpp"
#include "native/inventory.hpp"
#include <cmath>
#include <iostream>
using namespace rosh;
static void check(bool ok,const char*message){if(!ok)throw std::runtime_error(message);}
int wmain(int argc,wchar_t**argv){try{
 check(argc==2,"Use isolated content fixture");fs::path root=argv[1];check(fs::exists(root/"GM_TEST_FIXTURE"),"Refusing live database");Content content(root);auto policy=content.get("server/equipment_rng.json");EquipmentRng rng(policy);
 std::vector<std::uint32_t>examples;std::uint32_t rare=0;std::set<std::tuple<int,int,int>>seen;
 for(auto it=policy.at("items").begin();it!=policy.at("items").end();++it){auto&r=it.value();int quality=r.at("quality"),slot=r.at("slot"),family=r.at("family");if(quality>=2&&quality!=5&&seen.insert({quality,slot,family}).second)examples.push_back(std::stoul(it.key()));if(quality==2&&slot==7&&family==200&&!r.at("materials").empty())rare=std::stoul(it.key());}
 check(rare!=0,"No native rare physical weapon fixture");std::size_t lines=0,duplicates=0,maximums=0;
 for(auto item:examples)for(int iteration=0;iteration<1000;iteration++){
  auto attrs=rng.roll(item,"common",item+iteration*std::uint64_t(104729));check(!attrs.empty()&&attrs.size()<=static_cast<std::size_t>(rng.maximum(item,"common")),"RNG exceeded native-category line policy");std::map<std::string,int>counts;
  const auto&definition=policy.at("items").at(std::to_string(item));int slot=definition.at("slot"),family=definition.at("family");auto pool=policy.at("pools").at(slot==7?(family==400||family==600?"magic_weapon":"physical_weapon"):"armor");if(slot==3)for(auto&s:policy.at("pools").at("gloves"))pool.push_back(s);if(slot==4)for(auto&s:policy.at("pools").at("boots"))pool.push_back(s);
  if(family>=100)for(auto&s:policy.at("pools").at(family==400||family==600?"magic_attributes":"physical_attributes"))pool.push_back(s);if(slot==8)for(auto&s:policy.at("pools").at("shield"))pool.push_back(s);
  for(auto&line:attrs){auto stat=line.at("stat").get<std::string>();check(++counts[stat]<=2,"More than two matching lines");if(counts[stat]==2)++duplicates;bool admitted=false;for(auto&s:pool)if(s.at("stat")==stat){double value=line.at("value"),cap=s.at("maximum");check(value>0&&value<=cap,"Per-line cap exceeded");admitted=true;if(value==cap)++maximums;}check(admitted,"Attribute incompatible with class/armor slot");check(stat!="block_chance"||slot==8,"Shield blocking rolled on another equipment slot");check((family==400||family==600)?stat!="strength"&&stat!="dexterity":stat!="intelligence"&&stat!="wisdom","Primary attribute belongs to a different combat family");++lines;}
 }
 check(duplicates>0&&duplicates<lines/20,"Duplicate policy is absent or too common");check(maximums>0&&maximums<lines/40,"Maximum-value rolls are not rare");
 Sqlite db(":memory:");Inventory bag(db,content);constexpr std::uint32_t eid=996201;auto cost=rng.material(rare,18);bag.grant_once(eid,{{rare,1},{cost.first,cost.second*3}},"rng-seed");ItemInstance target,stone;for(auto&i:bag.items(eid))if(i.item==rare)target=i;else if(i.item==cost.first)stone=i;
 auto common=db.query("SELECT stat,value FROM item_custom_affixes WHERE uid=? AND layer='common' ORDER BY position",{std::int64_t(target.uid)});check(!common.empty(),"New rare item has no initial RNG attributes");db.exec("INSERT INTO item_enchantments VALUES(?,18)",{std::int64_t(target.uid)});
 auto a=Inventory::address(stone.slot),b=Inventory::address(target.slot);Bytes request(22);put32(request,0,eid,true);put32(request,4,a.first,true);put32(request,8,a.second,true);put32(request,12,b.first,true);put32(request,16,b.second,true);auto reply=bag.additional_attributes(eid,request,"knowledge-1",now());check(get16(reply,20,true)==0x175,"Native Knowledge success code missing");check(bag.owned(eid,stone.slot,stone.item,now()).quantity==cost.second*2,"Native material quantity not consumed");auto craft=db.query("SELECT stat,value FROM item_custom_affixes WHERE uid=? AND layer='craft' ORDER BY position",{std::int64_t(target.uid)});check(!craft.empty(),"Additional RNG attributes missing");check(common==db.query("SELECT stat,value FROM item_custom_affixes WHERE uid=? AND layer='common' ORDER BY position",{std::int64_t(target.uid)}),"Knowledge replaced common attributes");bag.additional_attributes(eid,request,"knowledge-1",now());check(bag.owned(eid,stone.slot,stone.item,now()).quantity==cost.second*2,"Replay spent Knowledge Stones twice");
 try{Transaction tx(db);bag.additional_attributes(eid,request,"knowledge-rollback",now());throw std::runtime_error("simulate failed save");}catch(...){}
 check(craft==db.query("SELECT stat,value FROM item_custom_affixes WHERE uid=? AND layer='craft' ORDER BY position",{std::int64_t(target.uid)})&&bag.owned(eid,stone.slot,stone.item,now()).quantity==cost.second*2,"Failed save did not roll back stones and attributes together");
 auto changed=request;put32(changed,0,eid+1,true);bool rejected=false;try{bag.additional_attributes(eid,changed,"knowledge-forged",now());}catch(...){rejected=true;}check(rejected,"Knowledge accepted foreign actor");
 for(auto&row:db.query("SELECT stat,COUNT(*) AS count FROM item_custom_affixes WHERE uid=? GROUP BY stat",{std::int64_t(target.uid)}))check(row.at("count").get<int>()<=2,"Common and additional RNG exceeded two matching attributes per item");
 {
  constexpr std::uint32_t cash=999000072;auto cashCost=rng.material(cash,0);check(cashCost.first==90058&&cashCost.second==25,"Cash weapon differs from native Knowledge Stone cost");check(rng.maximum(cash,"common")==0&&rng.maximum(cash,"craft")==4,"Cash weapon received ordinary random drop affixes");
  bag.grant_once(eid,{{cash,1},{cashCost.first,cashCost.second}},"cash-knowledge-seed");ItemInstance skin,stones;for(auto&i:bag.items(eid)){if(i.item==cash)skin=i;if(i.item==cashCost.first)stones=i;}auto before=stones.quantity;auto materialAddress=Inventory::address(stones.slot),skinAddress=Inventory::address(skin.slot);Bytes q(22);put32(q,0,eid,true);put32(q,4,materialAddress.first,true);put32(q,8,materialAddress.second,true);put32(q,12,skinAddress.first,true);put32(q,16,skinAddress.second,true);check(get16(bag.additional_attributes(eid,q,"cash-knowledge",now()),20,true)==0x175,"Cash weapon native Knowledge request rejected");check(bag.owned(eid,stones.slot,stones.item,now()).quantity==before-25,"Cash weapon Knowledge consumption incorrect");check(!db.query("SELECT stat FROM item_custom_affixes WHERE uid=? AND layer='craft'",{std::int64_t(skin.uid)}).empty(),"Cash appearance cannot carry real attributes");
 }
 // Decode the actual option IDs from native inventory records and ensure no
 // common/additional line is silently discarded, including seven-line rolls.
 auto visuals=content.get("data/native_affix_visual_ids.json").at("entries");std::map<int,Json>options;for(auto&entry:visuals)options[entry.at("id")]=entry.at("stats");
 for(auto item:examples)for(int iteration=0;iteration<8;iteration++){
  auto attrs=rng.roll(item,"common",item+iteration*std::uint64_t(173));db.exec("DELETE FROM item_custom_affixes WHERE uid=?",{std::int64_t(target.uid)});int index=0;std::multiset<std::pair<std::string,double>>expected,decoded;
  for(auto&line:attrs){auto stat=line.at("stat").get<std::string>();double value=line.at("value");db.exec("INSERT INTO item_custom_affixes VALUES(?,?,?,?,?)",{std::int64_t(target.uid),std::string("common"),index++,stat,value});expected.insert({stat,value});}
  auto packet=bag.update(eid,target);for(int i=0;i<5;i++){int id=get16(packet,130+2*i);if(id)for(auto&line:options.at(id))decoded.insert({line[0].get<std::string>(),line[1].get<double>()});}check(decoded==expected,"Native tooltip lost RNG lines");
 }
 Json stats={{"lifesteal",3.},{"mana_absorption",3.}},character={{"hp",100},{"maxhp",120},{"mp",10},{"maxmp",50}};check(attack_recovery(stats,character,1000)==std::pair<int,int>(20,30),"HP/MP drain did not respect missing-resource caps");check(attack_recovery(stats,character,0)==std::pair<int,int>(0,0),"Miss/replay restored resources");character["hp"]=0;check(attack_recovery(stats,character,1000)==std::pair<int,int>(0,0),"Lifesteal resurrected dead character");
 // Exercise queued reflection through the real shared scene. An isolated
 // summon avoids ambient monsters affecting the damage/reward assertions.
 {
  WorldScene scene(content);auto base=scene.map("berneo");const Entity*type=nullptr;
  for(auto&[id,mob]:*base)if(mob.combat&&mob.category=="normal"&&mob.level<=15&&(!type||mob.maxhp<type->maxhp))type=&mob;
  check(type,"No reflection fixture species");auto species=type->npc_id;scene.summon("berneo",species,1,{99000,99000,0});auto target=scene.map("berneo")->at(9999);
  Json combat={{"hp",1000000},{"level",target.level},{"attack",1.},{"attack_speed",1.},{"defense",0.},{"critical",0.},{"physical_reflection",100.},{"magic_reflection",0.}};
  Bytes avatar(260),movement(34),hit(32);put32(hit,1,eid,true);put32(hit,28,target.eid,true);Json pos={target.position.x,target.position.y,target.position.z};
  scene.update_player("reflector",eid,"berneo",pos,avatar,movement,false,[](auto,auto,const auto&){},combat);
  scene.strike("reflector",1,hit,1000,{});for(int n=0;n<240;n++)scene.simulate(1000.1+n*.25);
  check(scene.take_damage("reflector")>target.hp,"Reflection fixture received insufficient damage");auto before=scene.map("berneo")->at(target.eid).hp;bool failed=false;
  try{scene.resolve_reflections("reflector",1061,[&](Entity&){throw std::runtime_error("Injected reward failure");});}catch(...){failed=true;}
  check(failed&&scene.map("berneo")->at(target.eid).hp==before,"Failed reflection reward mutated mob state");int claims=0;
  check(scene.resolve_reflections("reflector",1062,[&](Entity&mob){++claims;mob.respawn_at=1063;})==1&&claims==1,"Reflection retry lost or duplicated reward");
  check(scene.resolve_reflections("reflector",1062,[&](Entity&){++claims;})==0&&claims==1,"Resolved reflection replay paid twice");scene.simulate(1064);check(scene.map("berneo")->at(target.eid).life_generation==target.life_generation+1,"Respawn did not invalidate queued life identity");
  // Physical hits must not use magic-only reflection.
  combat["physical_reflection"]=0.;combat["magic_reflection"]=100.;scene.update_player("reflector",eid,"berneo",pos,avatar,movement,false,[](auto,auto,const auto&){},combat);
  scene.strike("reflector",1,hit,1065,{});for(int n=0;n<40;n++)scene.simulate(1065.1+n*.25);before=scene.map("berneo")->at(target.eid).hp;scene.resolve_reflections("reflector",1076,[&](Entity&){++claims;});check(scene.map("berneo")->at(target.eid).hp==before,"Physical hit used magical reflection");
 }
 std::cout<<"PASS: "<<examples.size()<<" class/slot/quality combinations; bounded rare duplicates and maxima; native Knowledge material costs and success reply; common/craft isolation; replay and rollback; complete native tooltip encoding; HP/MP recovery caps.\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
