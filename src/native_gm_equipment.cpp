#include "native/gm.hpp"
#include <cmath>
namespace rosh {
Json gm_equipment_options(const Json&rule){
 Json stats=Json::array();auto add=[&](const char*id,const char*label,double maximum,const char*unit){stats.push_back({{"id",id},{"label",label},{"minimum",0},{"maximum",maximum},{"unit",unit}});};
 add("physical_attack_flat","Physical attack",150,"points");add("magic_attack_flat","Magic attack",150,"points");add("physical_defense_flat","Physical defense",180,"points");add("magic_defense_flat","Magic defense",180,"points");add("critical_chance_flat","Critical chance",15,"points");add("critical_multiplier","Critical multiplier",.25,"multiplier");add("physical_attack","Physical attack power",15,"%");add("magic_attack","Magic attack power",15,"%");add("attack_power","Physical and magic attack power",15,"%");add("attack_speed","Attack speed",rule.value("slot",0)==3||rule.value("slot",0)==28?8:10,"%");add("cast_speed","Casting speed",25,"%");add("mana_absorption","Mana absorption",3,"%");add("physical_defense","Physical defense bonus",15,"%");add("magic_defense","Magic defense bonus",15,"%");add("max_hp_flat","Maximum HP",350,"points");add("max_hp","Maximum HP bonus",15,"%");
 if(rule.empty()||rule.value("slot",0)==4||rule.value("slot",0)==29)add("move_speed","Movement speed",10,"%");
 add("strength","Strength",5,"points");add("dexterity","Dexterity",5,"points");add("vitality","Vitality",5,"points");add("intelligence","Intelligence",5,"points");add("wisdom","Wisdom",5,"points");add("hp_regen_flat","HP regeneration",20,"points/pulse");add("mp_regen_flat","MP regeneration",20,"points/pulse");add("max_mp_flat","Maximum MP",350,"points");add("max_mp","Maximum MP bonus",15,"%");add("accuracy","Attack rating",15,"points");add("defense_rating","Protection rating",15,"points");add("cooldown_reduction","Skill cooldown reduction",25,"%");add("hp_absorption","HP absorption",3,"%");add("evasion","Evasion",10,"%");add("critical_reduction","Critical damage reduction",.25,"multiplier");add("physical_reflection","Physical damage reflection",2,"%");add("magic_reflection","Magic damage reflection",2,"%");
 if(rule.empty()||rule.value("slot",0)==8)add("block_chance","Shield block chance",10,"%");
 return {{"stats",stats},{"maximum_lines",7},{"maximum_matching_lines",2},{"maximum_enhancement",18},{"layers",Json::array({"common","craft"})},{"notice","Server effects are saved per item UID. Native custom tooltips remain incomplete. Limits apply per line; unchanged layers are preserved."}};
}
Json gm_equipment_validate(const Json&config,const Json&rule){
 if(!config.is_object()||config.empty()||config.size()>3)throw std::runtime_error("Select at least one equipment change");
 if((rule.value("kind",0)!=1&&rule.value("kind",0)!=11)||rule.value("slot",0)<=0||rule.value("stack",1)!=1)throw std::runtime_error("Select a non-stackable equipment item");
 auto options=gm_equipment_options(rule);Json limits=Json::object();for(auto&s:options.at("stats"))limits[s.at("id").get<std::string>()]=s.at("maximum");
 for(auto entry=config.begin();entry!=config.end();++entry){
  if(entry.key()=="grade"){if(!entry.value().is_number_integer()||entry.value().get<std::int64_t>()<0||entry.value().get<std::int64_t>()>18)throw std::runtime_error("Enhancement must be between +0 and +18");continue;}
  if(entry.key()!="common"&&entry.key()!="craft")throw std::runtime_error("Unknown equipment change");
  if(!entry.value().is_array()||entry.value().size()>7)throw std::runtime_error("Use up to seven lines per attribute layer");std::map<std::string,int>counts;
  for(auto&line:entry.value()){
   if(!line.is_object()||line.size()!=2||!line.contains("stat")||!line.at("stat").is_string()||!line.contains("value")||!line.at("value").is_number())throw std::runtime_error("Invalid attribute line");
   auto stat=line.at("stat").get<std::string>();double value=line.at("value");if(!limits.contains(stat)||!std::isfinite(value)||value<=0||value>limits.at(stat).get<double>())throw std::runtime_error("Attribute is unsupported or exceeds its per-line limit");if(++counts[stat]>2)throw std::runtime_error("At most two matching attributes per layer");
  }
 }
 return config;
}
void gm_equipment_apply(Sqlite&db,Inventory&inventory,std::uint32_t eid,std::uint32_t uid,std::uint32_t expected,const Json&config){
 ItemInstance selected;int matches=0;for(bool equipped:{false,true})for(auto&i:inventory.items(eid,equipped))if(i.uid==uid){selected=i;++matches;}
 if(matches!=1||selected.item!=expected||!uid)throw std::runtime_error("Equipment UID no longer belongs to this character");if(selected.expires&&*selected.expires<=now())throw std::runtime_error("Expired equipment cannot be modified");gm_equipment_validate(config,inventory.definition(selected.item));
 Transaction tx(db);db.exec("CREATE TABLE IF NOT EXISTS item_custom_affixes(uid INTEGER NOT NULL,layer TEXT NOT NULL,position INTEGER NOT NULL,stat TEXT NOT NULL,value REAL NOT NULL,PRIMARY KEY(uid,layer,position))");
 if(config.contains("grade"))db.exec("INSERT INTO item_enchantments VALUES(?,?) ON CONFLICT(uid) DO UPDATE SET grade=excluded.grade",{std::int64_t(uid),config.at("grade").get<std::int64_t>()});
 for(auto layer:{"common","craft"})if(config.contains(layer)){
  db.exec("DELETE FROM item_custom_affixes WHERE uid=? AND layer=?",{std::int64_t(uid),std::string(layer)});int position=0;
  for(auto&line:config.at(layer))db.exec("INSERT INTO item_custom_affixes VALUES(?,?,?,?,?)",{std::int64_t(uid),std::string(layer),std::int64_t(position++),line.at("stat").get<std::string>(),line.at("value").get<double>()});
 }inventory.update(eid,selected);tx.commit();
}
}
