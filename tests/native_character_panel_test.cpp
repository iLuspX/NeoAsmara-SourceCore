#include "native/game.hpp"
#include <cmath>
#include <iostream>
using namespace rosh;
static void check(bool value,const char*message){if(!value)throw std::runtime_error(message);}
static void check_near(double actual,double expected,const char*message){check(std::abs(actual-expected)<.0002,message);}
int wmain(int argc,wchar_t**argv){try{
 check(argc==2,"Use an isolated encrypted content fixture");fs::path root=argv[1];check(fs::exists(root/"GM_TEST_FIXTURE"),"Refusing live database");Content content(root);auto profiles=content.get("data/balance_profiles.json");
 auto base=calculated_stats(profiles,400,50);
 auto intelligence=calculated_stats(profiles,400,50,{{"5",{5.,0.}}});
 auto wisdom=calculated_stats(profiles,400,50,{{"4",{5.,0.}}});
 check_near(intelligence["attributes"][3],base["attributes"][3].get<double>()+5,"Native operation5 did not increase Intelligence");check_near(intelligence["attributes"][4],base["attributes"][4],"Intelligence changed Wisdom");
 check_near(wisdom["attributes"][4],base["attributes"][4].get<double>()+5,"Native operation4 did not increase Wisdom");check_near(wisdom["attributes"][3],base["attributes"][3],"Wisdom changed Intelligence");check(wisdom["max_mp"].get<double>()>base["max_mp"].get<double>()&&intelligence["magic_attack"].get<double>()>base["magic_attack"].get<double>(),"Primary attributes are cosmetic only");
 auto regen=calculated_stats(profiles,400,50,{{"30",{20.,50.}},{"31",{20.,25.}},{"47",{350.,15.}},{"10",{15.,0.}},{"16",{15.,0.}},{"22",{0.,10.}},{"64",{0.,25.}}});
 check_near(regen["hp_regen"],(base["hp_regen"].get<double>()+20)*1.5,"HP regeneration modifier ignored");check_near(regen["mp_regen"],(base["mp_regen"].get<double>()+20)*1.25,"MP regeneration modifier ignored");check_near(regen["accuracy"],base["accuracy"].get<double>()+15,"Attack rating modifier ignored");check_near(regen["defense_rating"],15,"Protection rating modifier ignored");check_near(regen["block_chance"],10,"Shield block modifier ignored");check_near(regen["cooldown_reduction"],25,"Cooldown reduction ignored");check(regen["max_mp"]>base["max_mp"],"MP equipment has no effect");
 Json wounded={{"hp",99},{"maxhp",100},{"mp",1},{"maxmp",10}};check(regeneration_recovery(regen,wounded)==std::pair<int,int>{1,9},"Regeneration exceeds missing HP/MP");wounded["hp"]=0;check(regeneration_recovery(regen,wounded)==std::pair<int,int>{0,0},"Regeneration resurrects dead character");
 check_near(physical_hit_chance(0,50),.5,"Hit/evasion contract incorrect");check(physical_hit_chance(15,50)>physical_hit_chance(0,50),"Attack rating lowers hit chance");check_near(physical_hit_chance(100,50),1.,"Hit chance exceeds100percent");
 auto total=regen;total["attributes"]=base["attributes"];total["attributes"][3]=base["attributes"][3].get<int>()+5;total["attack"]=9999.;total["magic_attack"]=9999.;total["physical_intrinsic"]=40.;total["magic_intrinsic"]=50.;total["physical_factor"]=1.15;total["magic_factor"]=1.25;total["physical_reflection"]=2.;total["lifesteal"]=3.;total["mana_absorption"]=3.;total["attack_speed"]=1.5;total["cast_speed"]=1.4;total["defense"]=370.;total["magic_defense"]=120.;total["critical"]=15.;total["critical_multiplier"]=1.75;total["evasion"]=10.;
 Bytes carrier(788,0);auto encoded=native_character_stats_packet(carrier,base,total,5,1.2);
 // These positions come from the supplied AMo getters, not the old Python
 // packet builder. Attack/defense and total/baseline were previously mixed.
 check_near(getfloat(encoded,96),370,"Physical defense written into another native field");check_near(getfloat(encoded,120),120,"Magical defense written into another native field");check_near(getfloat(encoded,164),15,"Critical chance native field missing");check_near(getfloat(encoded,132),1.75,"Critical damage native field missing");check_near(getfloat(encoded,128),140,"Casting rate encoded as duration instead of native percent");check_near(getfloat(encoded,292),2,"Reflection native field missing");check_near(getfloat(encoded,304),3,"HP absorption native field missing");check_near(getfloat(encoded,316),3,"MP absorption native field missing");check_near(getfloat(encoded,60),10,"Evasion native field missing");check_near(getfloat(encoded,64),10,"Block chance native field missing");
 check_near(getfloat(encoded,476),base["defense"],"Equipment total overwrote intrinsic defense baseline");check(getfloat(encoded,476)!=getfloat(encoded,96),"Total and intrinsic buffers are mirrored incorrectly");check_near(getfloat(encoded,88),7.5,"Staff attack speed rate inverted");check_near(getfloat(encoded,468),attack_rate(base["attack_speed"]),"Staff/buff speed overwrote intrinsic baseline");
 check(get16(encoded,12)==base["attributes"][3].get<int>()&&get16(encoded,24)==5,"Native Intelligence base/bonus words do not agree with panel getter");
 // Native UI adds weapon damage locally, then applies the supplied factor.
 // The server's TOTAL9999 is intentionally not emitted as another weapon.
 check_near((20+getfloat(encoded,84))*(1+getfloat(encoded,280)),69.,"Physical weapon counted twice in the native panel");check_near((30+getfloat(encoded,112))*(1+getfloat(encoded,284)),100.,"Magical weapon counted twice in the native panel");
 // Every split of seven lines between old/new visual groups must round-trip
 // through the native five ushort option IDs without truncating attributes.
 Sqlite db(":memory:");Inventory bag(db,content);constexpr std::uint32_t eid=997301;bag.grant_once(eid,{{15001,1}},"native-panel-mixed-options");auto item=bag.items(eid).front();auto visuals=content.get("data/native_affix_visual_ids.json").at("entries");std::map<int,Json>options;for(auto&e:visuals)options[e.at("id")]=e.at("stats");
 std::vector<std::pair<std::string,double>>old={{"physical_attack_flat",15},{"magic_attack_flat",15},{"physical_defense_flat",18},{"magic_defense_flat",18},{"critical_chance_flat",2},{"attack_speed",1},{"move_speed",1}},fresh={{"strength",1},{"dexterity",1},{"vitality",1},{"intelligence",1},{"wisdom",1},{"hp_regen_flat",2},{"mp_regen_flat",2}};
 for(int count=0;count<=7;count++){
  db.exec("DELETE FROM item_custom_affixes WHERE uid=?",{std::int64_t(item.uid)});std::multiset<std::pair<std::string,double>>expected,actual;
  for(int index=0;index<7;index++){auto value=index<count?fresh[index]:old[index];expected.insert(value);db.exec("INSERT INTO item_custom_affixes VALUES(?,?,?,?,?)",{std::int64_t(item.uid),std::string("common"),index,value.first,value.second});}
  auto record=bag.update(eid,item);for(int i=0;i<5;i++){int id=get16(record,130+2*i);if(id)for(auto&v:options.at(id))actual.insert({v[0].get<std::string>(),v[1].get<double>()});}check(actual==expected,"Mixed old/new seven-line tooltip lost an attribute");
 }
 std::cout<<"PASS: native primary INT/WIS operations; real primary/MP/regen/hit/block/cooldown calculations; recovery caps; native27-row panel field contract; intrinsic versus total; weapon damage not duplicated; seven-line mixed tooltip cover.\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
