#include "native/consumables.hpp"
#include <algorithm>
#include <cmath>
namespace rosh {
Consumables::Consumables(Sqlite& db,Inventory& inventory,Content& content):db_(db),inventory_(inventory),effects_(content.get("data/consumable_effects.json")),skills_(content.get("data/skill_catalog.json").at("skills")),initial_(content.get("data/skills_observed_gamexp.json").at("skills")),super_tree_(content.get("data/skill_catalog.json").value("super_tree",Json::object())){
 db_.exec("CREATE TABLE IF NOT EXISTS native_consumable_uses(event_key TEXT PRIMARY KEY,eid INTEGER NOT NULL,slot INTEGER NOT NULL,item INTEGER NOT NULL)");
}
bool Consumables::supports(std::uint32_t id)const{return effects_.contains(std::to_string(id));}
bool Consumables::use(Json& ch,int slot,std::uint32_t id,const std::string& event,double stamp,double healing,std::uint32_t map_id){
 if(!supports(id))throw std::runtime_error("Consumivel sem efeito documentado; item preservado");
 if(event.empty()||event.size()>118||!std::isfinite(stamp))throw std::runtime_error("Pedido de consumivel invalido");
 auto before=ch;try{
 Transaction tx(db_);auto eid=ch.at("eid").get<std::uint32_t>();auto previous=db_.query("SELECT eid,slot,item FROM native_consumable_uses WHERE event_key=?",{event});
 if(!previous.empty()){if(previous[0].at("eid")!=eid||previous[0].at("slot")!=slot||previous[0].at("item")!=id)throw std::runtime_error("Pedido repetido alterado");tx.commit();return false;}
 if(ch.value("hp",0)<=0)throw std::runtime_error("Personagem morto nao pode usar consumiveis");
 auto effect=effects_.at(std::to_string(id));
 if(effect.contains("allowed_maps")){
  const auto& maps=effect.at("allowed_maps");
  if(std::find(maps.begin(),maps.end(),map_id)==maps.end())
   throw std::runtime_error("Este consumivel so pode ser usado na Arena");
 }
 auto item=inventory_.owned(eid,slot,id,stamp);auto kind=effect.at("kind").get<std::string>();
 auto timers=ch.value("potion_cooldowns",Json::object());auto itemKey="item:"+std::to_string(id);
 if(timers.value(itemKey,0.)>stamp)throw std::runtime_error("Aguarde a recarga do item");
 if(kind=="restore"){
  bool needed=false;for(auto& row:effect.at("restore").items()){if(timers.value("resource:"+row.key(),0.)>stamp)throw std::runtime_error("Aguarde a recarga da pocao");needed|=ch.at(row.key()).get<double>()<ch.at("max"+row.key()).get<double>();}
  if(!needed)throw std::runtime_error("HP/MP ja completos");
  for(auto& row:effect.at("restore").items()){auto maximum=ch.at("max"+row.key()).get<double>();double amount=row.value().get<double>()+maximum*effect.value("percent_restore",Json::object()).value(row.key(),0.)/100.;if(row.key()=="hp")amount*=1+std::max(0.,healing)/100.;ch[row.key()]=static_cast<int>(std::min(maximum,ch.at(row.key()).get<double>()+std::nearbyint(amount)));timers["resource:"+row.key()]=stamp+1;}
 }else if(kind=="buff"||kind=="native_buff"||kind=="regen"||kind=="progression"){
  auto active=ch.value("consumable_effects",Json::object());
  auto group=effect.at("group").get<std::string>();
  const auto resource=effect.value("resource",std::string());
  if(kind=="progression"&&(resource=="xp"||resource=="fletta")){
   // Existing characters may still carry the old resource/duration key.
   for(const auto &entry:active)
    if(entry.value("item_id",0u)==id&&entry.value("expires",0.)>stamp)
     throw std::runtime_error("Este efeito ja esta ativo");
   group="progression:item:"+std::to_string(id);
  }
  if(active.contains(group)&&active.at(group).value("expires",0.)>stamp)
   throw std::runtime_error("Este efeito ja esta ativo");
  effect["item_id"]=id;effect["expires"]=stamp+effect.at("duration").get<double>();effect["next_tick"]=stamp+effect.value("period",0.);active[group]=effect;ch["consumable_effects"]=active;
 }else if(kind=="fletta_convert"){
  auto paths=ch.value("fletta_paths",Json::object());auto group=std::to_string(effect.at("group").get<int>());if(!paths.contains(group)||paths.at(group).value("points",0)<=0||paths.at(group).at("path")==effect.at("path"))throw std::runtime_error("Sem pontos Fletta para converter");paths[group]["path"]=effect.at("path");ch["fletta_paths"]=paths;
 }else if(kind=="fletta_reset"){
  bool spent=false;for(auto& p:ch.value("fletta_paths",Json::object()))spent|=p.value("points",0)>0;if(!spent)throw std::runtime_error("Sem atributos Fletta para redefinir");ch["fletta_paths"]=Json::object();
 }else if(kind=="skill_reset"){
  Json baseline=Json::object();for(auto& r:initial_)if(r.at("class").get<int>()==ch.at("cls").get<int>()/100*100&&r.at("initial_rank").get<int>())baseline[std::to_string(r.at("id").get<int>())]=r.at("initial_rank");
  auto learnedSkills=ch.value("skills",Json::object());std::int64_t refund=0;for(auto& learned:learnedSkills.items()){if(super_tree_.contains(learned.key())){baseline[learned.key()]=learned.value();continue;}if(!skills_.contains(learned.key()))continue;auto levels=skills_.at(learned.key()).at("levels");for(int n=baseline.value(learned.key(),0)+1;n<=learned.value().get<int>();n++)if(levels.contains(std::to_string(n)))refund+=levels.at(std::to_string(n)).value("points",0);}
  if(!refund)throw std::runtime_error("Sem pontos de habilidade investidos");ch["skill_points"]=ch.value("skill_points",std::int64_t(0))+refund;ch["skills"]=baseline;auto buffs=ch.value("active_buffs",Json::object());for(auto it=buffs.begin();it!=buffs.end();)if(!super_tree_.contains(it.key()))it=buffs.erase(it);else ++it;ch["active_buffs"]=buffs;
 }else if(kind!="box")throw std::runtime_error("Efeito sem suporte; item preservado");
 inventory_.consume(eid,item,1,kind=="box"?"open-box":"use");
 if(kind=="box"){
  const bool trace=inventory_.traced(item.uid);
  std::set<std::uint32_t> prior;
  if(trace)for(const auto& existing:inventory_.items(eid))prior.insert(existing.uid);
  std::vector<std::pair<std::uint32_t,int>> goods;
  for(const auto& row:effect.at("items"))goods.emplace_back(row[0],row[1]);
  if(inventory_.grant_once(eid,goods,event+":contents",trace)&&trace)
   for(const auto& child:inventory_.items(eid))
    if(!prior.count(child.uid))inventory_.trace_derivation(item.uid,child,eid);
 }
 if(effect.value("cooldown",0.)>0)timers[itemKey]=stamp+effect.at("cooldown").get<double>();ch["potion_cooldowns"]=timers;
 db_.exec("INSERT INTO native_consumable_uses VALUES(?,?,?,?)",{event,std::int64_t(eid),std::int64_t(slot),std::int64_t(id)});tx.commit();return true;
 }catch(...){ch=std::move(before);throw;}
}
bool Consumables::tick(Json& ch,double stamp){
 auto active=ch.value("consumable_effects",Json::object());
 bool changed=false;
 for(auto it=active.begin();it!=active.end();){
  auto& effect=it.value();
  if(effect.at("expires").get<double>()<=stamp){
   it=active.erase(it);
   changed=true;
   continue;
  }
  const auto due=effect.value("next_tick",stamp);
  if(effect.at("kind")=="regen"&&due<=stamp){
   const auto period=effect.at("period").get<double>();
   if(ch.value("hp",0)>0){
    auto restore=[&](const std::string& resource){
     const auto maximum=ch.at("max"+resource).get<double>();
     auto amount=effect.at("amount").get<double>();
     if(effect.value("percent",false))
      amount=std::max(1.,std::nearbyint(maximum*amount/100.));
     ch[resource]=static_cast<int>(std::min(maximum,ch.at(resource).get<double>()+amount));
    };
    if(effect.contains("resources")){
     for(const auto& resource:effect.at("resources"))
      restore(resource.get<std::string>());
    }else{
     restore(effect.at("resource").get<std::string>());
    }
   }
   // Keep the dual-resource pulse on its original cadence. Missed pulses
   // are skipped, including time spent disconnected.
   effect["next_tick"]=effect.contains("resources")?
       due+(std::floor((stamp-due)/period)+1)*period:stamp+period;
   changed=true;
  }
  ++it;
 }
 if(changed)ch["consumable_effects"]=active;
 return changed;
}
double Consumables::bonus(const Json& ch,const std::string& resource,double stamp){double value=0;for(auto& e:ch.value("consumable_effects",Json::object()))if(e.value("kind",std::string())=="progression"&&e.value("expires",0.)>stamp&&e.value("resource",std::string())==resource)value+=e.at("percent").get<double>();return value;}
}
