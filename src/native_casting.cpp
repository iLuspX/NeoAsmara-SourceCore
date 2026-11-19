#include "native/casting.hpp"
#include <cmath>
#include <array>
#include <algorithm>
#include <set>
namespace rosh {
ActiveSkills::ActiveSkills(Content&content):encounters_(content.get("server/respawn_catalog.json")){
 auto source=content.get("data/skill_catalog.json");
 catalog_=std::move(source.at("skills"));stones_=std::move(source["super_stones"]);
 if(stones_.is_null())stones_=Json::object();
}
Json ActiveSkills::effects(std::uint32_t id,int rank)const{
 Json result=Json::array();auto key=std::to_string(id),level=std::to_string(rank);if(!catalog_.contains(key))return result;std::vector<std::string>sources={key};int aura=catalog_.at(key).value("aura_skill",0);if(aura&&aura!=static_cast<int>(id))sources.push_back(std::to_string(aura));
 for(auto&source:sources)if(catalog_.contains(source)&&catalog_.at(source).at("levels").contains(level))for(auto&e:catalog_.at(source).at("levels").at(level).at("effects"))result.push_back(e);return result;
}
bool ActiveSkills::targeted_healing(std::uint32_t id){return id==11901||id==12502||id==12702;}
bool ActiveSkills::sacrifice_healing(std::uint32_t id){return id==4506||id==4702||id==4706;}
bool ActiveSkills::healing_radar(std::uint32_t id){return id==15109||id==15902;}
static int sacrifice_hp_cost(std::uint32_t id,std::uint32_t rank){
 // SkillLevel native LE64 at record +24, distinct from mana at +40.
 static constexpr std::array offering{30,35,40,45,50};
 static constexpr std::array grail{480,600,720,1140,1190,1380};
 static constexpr std::array high_offering{300,600,1200,1800,2400};
 if(id==4506)return offering.at(rank-1);
 if(id==4702)return grail.at(rank-1);
 return high_offering.at(rank-1);
}
Json ActiveSkills::admit(Json&ch,const Json&stats,const SceneEntities&entities,const SkillBook&learned,const Bytes&body,double stamp,bool pk_mode,bool friendly_target){
 auto eid=ch.at("eid").get<std::uint32_t>();if(body.size()!=36||body[0]!=0||get32(body,1,true)!=eid||get16(body,34,true)!=0x24)throw std::runtime_error("Pedido de habilidade invalido");
 for(int i=0;i<4;i++)if(!std::isfinite(getfloat(body,13+i*4)))throw std::runtime_error("Posicao de habilidade invalida");
 auto id=get32(body,5,true),rank=get32(body,9,true),target=get32(body,30,true);auto key=std::to_string(id),r=std::to_string(rank);
 if(!catalog_.contains(key)||!catalog_[key].at("levels").contains(r)||!learned.cast_allowed(ch.at("cls"),id)||catalog_[key].value("passive",false))throw std::runtime_error("Habilidade indisponivel para esta classe");
 auto row=catalog_[key],level=skill_level_with_stone(catalog_,stones_,ch,id,rank);int kind=row.at("kind");const bool radar=healing_radar(id)||id==15908||id==16705;if(kind!=11&&kind!=13&&kind!=21&&kind!=23&&kind!=25&&!radar)throw std::runtime_error("Esta habilidade depende do fluxo de area, invocacao ou ressurreicao");
 if(ch.value("hp",0)<=0||ch.value("skills",Json::object()).value(key,0u)!=rank)throw std::runtime_error("Habilidade nao aprendida neste nivel");
 const bool sacrifice=sacrifice_healing(id);
 const bool other_heal=(targeted_healing(id)||sacrifice)&&target!=0&&target!=eid;
 if(sacrifice&&!other_heal)throw std::runtime_error("Selecione um aliado da party para esta cura");
 bool area=(id==3907||id==4503||id==4004),ground=area&&body[29]==0&&(target==0||target==eid);
 auto mob=entities.find(target);if((kind==11||kind==13)&&!ground){auto p=ch.at("pos");if(body[29]!=1||mob==entities.end()||!mob->second.attackable(pk_mode)||mob->second.position.distance({p[0],p[1],p[2]})>350)throw std::runtime_error("Alvo invalido ou fora do alcance da habilidade");
  if(encounters_.value(std::to_string(mob->second.npc_id),Json::object()).value("native_effect",0)==97&&(ch.at("level").get<int>()<std::max(1,mob->second.level-5)||ch.at("level").get<int>()>mob->second.level+15))throw std::runtime_error("Nivel nao permitido para este semi-chefe");
 }else if(other_heal){
  if(body[29]!=0||!friendly_target)throw std::runtime_error("Jogador invalido ou fora do alcance da cura");
 }else if(body[29]!=0||(target&&target!=eid))throw std::runtime_error("Esta habilidade requer alvo proprio");
 if((kind==21&&!other_heal)||kind==23||kind==25||radar)target=eid;
 auto timers=ch.value("skill_cooldowns",Json::object());if(std::max(timers.value(key,0.),timers.value("global",0.))>stamp)throw std::runtime_error("Habilidade em recarga");
 double mana=level.at("mana");if(!std::isfinite(mana)||mana<0||mana>100000000)throw std::runtime_error("Custo nativo de mana invalido");int cost=static_cast<int>(std::ceil(mana));if(ch.value("mp",0)<cost)throw std::runtime_error("Mana insuficiente");
 if(sacrifice){
  const auto hp_cost=sacrifice_hp_cost(id,rank);
  const auto hp=ch.at("hp").get<int>();
  if(hp<=hp_cost)throw std::runtime_error("HP insuficiente para a cura de sacrificio");
  ch["hp"]=hp-hp_cost;
 }
 ch["mp"]=ch.value("mp",0)-cost;timers[key]=stamp+std::max(.25,level.at("cooldown").get<double>()*(1-std::clamp(stats.value("cooldown_reduction",0.),0.,75.)/100));timers["global"]=stamp+.5/std::max(.1,stats.value("cast_speed",1.));
 for(auto it=timers.begin();it!=timers.end();)if(it.value().get<double>()<=stamp)it=timers.erase(it);else ++it;ch["skill_cooldowns"]=timers;
 double duration=level.at("duration");if(kind==23||kind==25){auto buffs=ch.value("active_buffs",Json::object());buffs[key]={{"rank",rank},{"expires",stamp+std::max(1.,duration>0?duration:3600.)}};ch["active_buffs"]=buffs;}
 const bool group_heal=id==11908||id==12505;
 const bool native_heal=group_heal||targeted_healing(id)||sacrifice;
 double heal_flat=0.,heal_percent=0.;
 if(native_heal){
  for(const auto& effect:level.at("effects"))if(effect.at("operation")==28){
   heal_flat+=effect.at("flat").get<double>();
   heal_percent+=effect.at("percent").get<double>();
  }
 }
 const double heal_multiplier=1+std::max(0.,stats.value("healing",0.))/100;
 if(kind==21&&!other_heal){
  const double maximum=ch.at("maxhp");
  const double heal=native_heal?std::max(0.,std::nearbyint((heal_flat+maximum*heal_percent/100)*heal_multiplier)):
      std::max(1.,std::nearbyint(stats.value("magic_attack",21.)*1.5*heal_multiplier));
  ch["hp"]=static_cast<int>(std::min(maximum,ch.at("hp").get<int>()+heal));
 }
 pending_={{"id",id},{"rank",rank},{"target",target},{"area",area},{"expires",stamp+std::max(6.,level.value("cast_time",0.)+3.)},{"kind",kind},{"remaining",std::clamp(level.value("hits",id==9001||id==3901?2:1),1,10)}};
 auto result=pending_;
 if(group_heal||other_heal)result["healing"]={heal_flat,heal_percent,heal_multiplier};
 if(healing_radar(id)){
  double hp=0.,mp=0.;
  for(const auto& effect:effects(15150,rank)){
   if(effect.at("operation")==28)hp+=effect.at("percent").get<double>();
   if(effect.at("operation")==29)mp+=effect.at("flat").get<double>();
  }
  result["radar"]={hp,mp};
 }
 if(id==15908||id==16705){
  result["defense_aura"]=id==15908?15955:16757;
  result["defense_effects"]=effects(id==15908?15954:16756,rank);
 }
 auto resolved_effects=level.at("effects");
 const int aura=row.value("aura_skill",0);if(aura&&aura!=static_cast<int>(id))for(const auto& effect:effects(aura,rank))resolved_effects.push_back(effect);
 result["effects"]=resolved_effects;result["duration"]=kind==13&&duration<=0?10.:duration;
 pending_["resolved_level"]=level;pending_["resolved_effects"]=resolved_effects;return result;
}
Json ActiveSkills::impact(const Bytes&body,std::uint32_t eid,double stamp){
 if(body.size()<32||body.size()>512||body[0]!=0||get32(body,1,true)!=eid||body[5]!=1||pending_.empty()||pending_.at("id")!=get32(body,6,true)||(!pending_.value("area",false)&&pending_.at("target")!=get32(body,28,true))||pending_.at("expires").get<double>()<stamp)throw std::runtime_error("Impacto de habilidade sem cast autorizado");
 if(body[26]<1||body[26]>36||body.size()!=27+5u*body[26]||(!pending_.value("area",false)&&body[26]!=1))throw std::runtime_error("Lista de alvos invalida");
 Json targets=Json::array();std::set<std::uint32_t>unique;int kind=pending_.at("kind");for(unsigned i=0;i<body[26];i++){auto target=get32(body,28+i*5,true);if(body[27+i*5]!=(kind==21||kind==23||kind==25||kind==28?0:1)||!unique.insert(target).second)throw std::runtime_error("Alvo de habilidade repetido ou invalido");targets.push_back(target);}
 auto result=pending_;result["targets"]=targets;auto id=result.at("id").get<std::uint32_t>();int rank=result.at("rank");auto level=pending_.value("resolved_level",catalog_.at(std::to_string(id)).at("levels").at(std::to_string(rank)));bool damaging=false;
 for(auto field:{"physical_min","physical_max","magic_min","magic_max","power"}){double value=std::max(0.,level.value(field,0.));result[field]=value;damaging|=value>0;}
 result["damaging"]=damaging&&(result.at("kind")==11||result.at("kind")==13);result["duration"]=level.at("duration");result["effects"]=pending_.value("resolved_effects",effects(id,rank));
 int remaining=pending_.at("remaining");if(remaining>1)pending_["remaining"]=remaining-1;else cancel();return result;
}
}
