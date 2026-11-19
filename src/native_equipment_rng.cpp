#include "native/equipment_rng.hpp"
#include "native/inventory.hpp"
#include <algorithm>
#include <cmath>
namespace rosh {
int EquipmentRng::maximum(std::uint32_t item,const std::string&layer)const{
 if(layer!="common"&&layer!="craft")throw std::runtime_error("Camada de atributos invalida");
 auto key=std::to_string(item);if(!policy_.at("items").contains(key))return 0;
 if(layer=="craft"&&policy_.at("items").at(key).at("slot")==16)return policy_.value("cash_weapon_craft_lines",0);
 auto quality=std::to_string(policy_.at("items").at(key).at("quality").get<int>());
 return policy_.at(layer=="common"?"common_lines":"craft_lines").value(quality,0);
}
Json EquipmentRng::roll(std::uint32_t item,const std::string&layer,std::uint64_t seed,const Json&reserved)const{
 int limit=maximum(item,layer);Json out=Json::array();if(!limit)return out;
 const auto&row=policy_.at("items").at(std::to_string(item));int slot=row.at("slot"),family=row.at("family");
 auto pool=policy_.at("pools").at(slot==7||slot==16?(family==400||family==600?"magic_weapon":"physical_weapon"):"armor");
 if(slot==3)for(auto&entry:policy_.at("pools").at("gloves"))pool.push_back(entry);
 if(slot==4)for(auto&entry:policy_.at("pools").at("boots"))pool.push_back(entry);
 if(family>=100){auto key=family==400||family==600?"magic_attributes":"physical_attributes";if(policy_.at("pools").contains(key))for(auto&entry:policy_.at("pools").at(key))pool.push_back(entry);}
 if(slot==8&&policy_.at("pools").contains("shield"))for(auto&entry:policy_.at("pools").at("shield"))pool.push_back(entry);
 std::mt19937_64 random(seed);auto draw=[&](int max){return std::uniform_int_distribution<int>(0,max-1)(random);};
 // Most items have fewer than their maximum possible lines.
 int lines=1;while(lines<limit&&draw(10000)<policy_.at("extra_line_chance_bp").get<int>())++lines;
 std::map<std::string,int>used;for(auto&line:reserved)++used[line.at("stat").get<std::string>()];
 for(int position=0;position<lines;position++){
  std::vector<Json>choices;bool duplicate=position&&draw(10000)<policy_.at("duplicate_chance_bp").get<int>();
  for(auto&entry:pool){auto stat=entry.at("stat").get<std::string>();if(duplicate?used[stat]==1:used[stat]==0)choices.push_back(entry);}
  if(choices.empty()){for(auto&entry:pool)if(used[entry.at("stat").get<std::string>()]<2)choices.push_back(entry);}
  if(choices.empty())break;
  auto entry=choices[draw(static_cast<int>(choices.size()))];auto stat=entry.at("stat").get<std::string>();++used[stat];
  const auto&weights=policy_.at("value_band_weights");int ticket=draw(10000),band=10;
  for(int i=0;i<10;i++)if((ticket-=weights.at(i).get<int>())<0){band=i+1;break;}
  double maximum=entry.at("maximum"),scale=entry.value("precision",1.);
  double value=std::max(1./scale,std::floor(maximum*scale*band/10+.5)/scale);
  out.push_back({{"stat",stat},{"value",std::min(maximum,value)}});
 }
 return out;
}
std::pair<std::uint32_t,int> EquipmentRng::material(std::uint32_t item,int grade)const{
 const auto&items=policy_.at("items");auto key=std::to_string(item);
 if(!items.contains(key)||grade<0||grade>18)throw std::runtime_error("Equipamento sem regra nativa de atributos adicionais");
 const auto&row=items.at(key);if(!maximum(item,"craft")||row.at("quality")==5)throw std::runtime_error("Este equipamento nao aceita a Pedra do Conhecimento");
 for(auto&cost:row.at("materials")){
  int minimum=cost.at("minimum_grade"),width=cost.at("bucket_width");if(grade<minimum)continue;
  int bucket=width>0?(grade-minimum)/width:4;if(bucket<0||bucket>4)continue;
  auto id=cost.at("items").at(bucket).get<std::uint32_t>();int count=cost.at("quantities").at(bucket);
  if(id&&count>0&&count<=10000)return {id,count};
 }
 throw std::runtime_error("Grau de melhoria insuficiente ou custo nativo indisponivel");
}
void EquipmentRng::initialize(Sqlite&db){
 db.exec("CREATE TABLE IF NOT EXISTS item_custom_affixes(uid INTEGER NOT NULL,layer TEXT NOT NULL,position INTEGER NOT NULL,stat TEXT NOT NULL,value REAL NOT NULL,PRIMARY KEY(uid,layer,position));CREATE TABLE IF NOT EXISTS native_item_affix_origins(uid INTEGER PRIMARY KEY,item_id INTEGER NOT NULL,created_at REAL NOT NULL)");
}
void EquipmentRng::initial(Sqlite&db,std::uint32_t uid,std::uint32_t item)const{
 if(!policy_.at("items").contains(std::to_string(item)))return;
 initialize(db);if(!db.query("SELECT uid FROM native_item_affix_origins WHERE uid=?",{std::int64_t(uid)}).empty())return;
 // Existing transferred/GM attributes must never be replaced by a new roll.
 if(db.query("SELECT uid FROM item_custom_affixes WHERE uid=? LIMIT 1",{std::int64_t(uid)}).empty()){
  auto seed=random_bytes(8);auto rows=roll(item,"common",std::uint64_t(get32(seed,0))|(std::uint64_t(get32(seed,4))<<32));int index=0;
  for(auto&line:rows)db.exec("INSERT INTO item_custom_affixes VALUES(?,?,?,?,?)",{std::int64_t(uid),std::string("common"),index++,line.at("stat").get<std::string>(),line.at("value").get<double>()});
 }
 db.exec("INSERT INTO native_item_affix_origins VALUES(?,?,?)",{std::int64_t(uid),std::int64_t(item),now()});
}
void Inventory::initial_affixes(std::uint32_t uid,std::uint32_t item){if(affixes_)affixes_->initial(db_,uid,item);}
Bytes Inventory::additional_attributes(std::uint32_t eid,const Bytes&body,const std::string&event,double stamp){
 if(!affixes_||body.size()!=22||get32(body,0,true)!=eid||get16(body,20,true)!=0||event.empty()||event.size()>200)throw std::runtime_error("Pedido nativo de atributos adicionais invalido");
 Transaction tx(db_);EquipmentRng::initialize(db_);
 db_.exec("CREATE TABLE IF NOT EXISTS native_knowledge_operations(event_key TEXT PRIMARY KEY,eid INTEGER NOT NULL,request TEXT NOT NULL,response TEXT NOT NULL,uid INTEGER NOT NULL,attributes TEXT NOT NULL)");
 auto prior=db_.query("SELECT eid,request,response FROM native_knowledge_operations WHERE event_key=?",{event});
 if(!prior.empty()){if(prior[0].at("eid")!=eid||prior[0].at("request")!=hex(body))throw std::runtime_error("Pedido repetido de atributos alterado");auto reply=unhex(prior[0].at("response"));tx.commit();return reply;}
 auto stoneSlot=key(get32(body,4,true),get32(body,8,true));std::optional<ItemInstance>material;
 for(auto&i:items(eid))if(i.slot==stoneSlot)material=i;
 if(!material||(material->expires&&*material->expires<=stamp))throw std::runtime_error("Pedra ausente ou expirada");
 auto stone=*material;
 // The native request carries only category/slot. Resolve the item identity
 // inside the same transaction, instead of trusting an ID supplied by a UI.
 auto targetCategory=get32(body,12,true),targetSlot=get32(body,16,true);std::optional<ItemInstance>target;
 if(targetCategory==0){if(targetSlot<1||targetSlot>30)throw std::runtime_error("Posicao de equipamento invalida");for(auto&i:items(eid,true))if(i.slot==static_cast<int>(targetSlot))target=i;}
 else{int slot=key(targetCategory,targetSlot);for(auto&i:items(eid))if(i.slot==slot)target=i;}
 if(!target||target->quantity!=1||(target->expires&&*target->expires<=stamp))throw std::runtime_error("Equipamento ausente ou expirado");
 auto cost=affixes_->material(target->item,target->grade);if(stone.item!=cost.first||stone.quantity<cost.second||stone.uid==target->uid)throw std::runtime_error("Pedra ou quantidade diferente do custo nativo");
 auto reserved=db_.query("SELECT stat,value FROM item_custom_affixes WHERE uid=? AND layer<>'craft' ORDER BY layer,position",{std::int64_t(target->uid)});
 auto seed=random_bytes(8);auto values=affixes_->roll(target->item,"craft",std::uint64_t(get32(seed,0))|(std::uint64_t(get32(seed,4))<<32),reserved);if(values.empty())throw std::runtime_error("Este equipamento nao aceita atributos adicionais");
 consume(eid,stone,cost.second);db_.exec("DELETE FROM item_custom_affixes WHERE uid=? AND layer='craft'",{std::int64_t(target->uid)});int index=0;
 for(auto&line:values)db_.exec("INSERT INTO item_custom_affixes VALUES(?,?,?,?,?)",{std::int64_t(target->uid),std::string("craft"),index++,line.at("stat").get<std::string>(),line.at("value").get<double>()});
 auto reply=body;put16(reply,20,0x175,true);db_.exec("INSERT INTO native_knowledge_operations VALUES(?,?,?,?,?,?)",{event,std::int64_t(eid),hex(body),hex(reply),std::int64_t(target->uid),values.dump()});tx.commit();return reply;
}
}
