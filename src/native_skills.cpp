#include "native/skills.hpp"
#include <algorithm>
#include <cmath>
namespace rosh {
Json skill_level_with_stone(const Json& skills,const Json& stones,const Json& ch,std::uint32_t id,int rank){
 const auto key=std::to_string(id);auto level=skills.at(key).at("levels").at(std::to_string(rank));
 const auto progress=ch.value("super_skill_progress",Json::object());
 const auto stone=progress.value(key,Json::object()).value("option",0u);
 if(!stone)return level;
 const auto& rule=stones.at(std::to_string(stone));
 const auto& allowed=rule.at("skills");
 if(std::find(allowed.begin(),allowed.end(),id)==allowed.end())throw std::runtime_error("Incompatible saved super-skill stone; data preserved");
 for(const auto& effect:rule.at("effects"))level["effects"].push_back(effect);
 const auto cooldown=level.at("cooldown").get<double>()+rule.at("cooldown_add").get<double>();
 const auto duration=rule.at("duration_override").get<double>();
 const auto multiplier=1.+rule.at("power_multiplier_add").get<double>();
 if(!std::isfinite(cooldown)||!std::isfinite(duration)||!std::isfinite(multiplier)||multiplier<0)
  throw std::runtime_error("Invalid native super-skill stone values");
 level["cooldown"]=std::max(0.,cooldown);if(duration>0)level["duration"]=duration;
 for(const auto* field:{"physical_min","physical_max","magic_min","magic_max","power"})
  level[field]=level.value(field,0.)*multiplier;
 return level;
}
Json skill_level_with_stone(const Json& catalog,const Json& ch,std::uint32_t id,int rank){
 static const Json empty=Json::object();
 return skill_level_with_stone(catalog.at("skills"),catalog.contains("super_stones")?catalog.at("super_stones"):empty,ch,id,rank);
}
SkillBook::SkillBook(Content&content){auto data=content.get("data/skill_catalog.json");catalog_=data.at("skills");trees_=data.at("trees");super_tree_=data.value("super_tree",Json::object());super_stones_=data.value("super_stones",Json::object());attributes_=content.get("data/fletta_attributes.json");initial_=content.get("data/skills_observed_gamexp.json").at("skills");}
Json SkillBook::change_super_stone(Json&ch,const Bytes&body,bool remove)const{
 const auto status_at=remove?8u:14u;
 if(body.size()!=(remove?10u:16u)||get32(body,0,true)!=ch.at("eid")||get16(body,status_at,true)!=(remove?0x2a9:0x2a1))
  throw std::runtime_error("Invalid native super-skill stone request");
 const auto skill=get32(body,4,true);const auto key=std::to_string(skill);
 if(!is_super(ch.at("cls"),skill)||ch.value("skills",Json::object()).value(key,0)<1)
  throw std::runtime_error("Learn this super skill before applying a stone");
 auto progress=ch.value("super_skill_progress",Json::object());
 auto state=progress.value(key,Json{{"points",0},{"option",0}});const auto prior=state.value("option",0u);
 const auto item=remove?0u:get32(body,8,true);
 if(remove){if(!prior)throw std::runtime_error("This super skill has no stone");}
 else{
  const auto item_key=std::to_string(item);
  if(!super_stones_.contains(item_key))throw std::runtime_error("This item is not a playable super-skill stone");
  const auto& targets=super_stones_.at(item_key).at("skills");
  if(std::find(targets.begin(),targets.end(),skill)==targets.end())throw std::runtime_error("Stone incompatible with this super skill");
  if(prior==item)throw std::runtime_error("This stone is already applied");
 }
 auto reply=body;put16(reply,status_at,remove?0x2a9:prior?0x2a2:0x2a1,true);
 state["option"]=item;state.erase("source_uid");progress[key]=state;ch["super_skill_progress"]=progress;
 return {{"reply",hex(reply)},{"fee",remove||!prior?10000:20000},{"item",item},{"skill",key},{"previous",prior}};
}
bool SkillBook::cast_allowed(int cls,std::uint32_t id)const{
 if(allowed(cls,id)||is_super(cls,id))return true;
 // Observed C05/03 casts: these supplemental Paladin skills are absent from
 // the ordinary tree. They still require a saved learned rank; ordinary
 // skill-point learning deliberately continues to use allowed(), not this.
 return (id==4803||id==4004)&&cls/100==2&&catalog_.at(std::to_string(id)).at("class")==200;
}
Bytes SkillBook::change_super_choice(Json&ch,const Bytes&body)const{
 if(body.size()!=12||get16(body,10,true)!=0)throw std::runtime_error("Invalid native Super Skill Change Coupon request");
 const auto from=get32(body,0,true),to=get32(body,4,true);const auto source=std::to_string(from),target=std::to_string(to);
 if(from==to||!is_super(ch.at("cls"),from)||!super_allowed(ch,to)||super_tree_.at(source)!=super_tree_.at(target))
  throw std::runtime_error("Choose the other super skill in the same profession stage");
 auto learned=ch.value("skills",Json::object()),progress=ch.value("super_skill_progress",Json::object());
 const auto rank=learned.value(source,0);const auto state=progress.value(source,Json{{"points",0},{"option",0}});
 if(rank<1||rank>5||learned.value(target,0)!=0||progress.value(target,Json::object()).value("points",0LL)!=0||
    progress.value(target,Json::object()).value("option",0u)!=0)
  throw std::runtime_error("Invalid learned super-skill choice or destination already in use");
 if(state.value("option",0u)!=0)throw std::runtime_error("Remove the super-skill stone before changing the skill");
 const auto points=state.value("points",0LL);const auto& costs=super_tree_.at(source).at("fletta_costs");
 if(points<0||(rank==5?points!=0:points>=costs.at(rank).get<std::int64_t>())||
    ch.at("level").get<int>()<catalog_.at(target).at("levels").at(std::to_string(rank)).at("level").get<int>())
  throw std::runtime_error("Invalid saved progression or insufficient character level; data preserved");
 learned.erase(source);learned[target]=rank;progress.erase(source);progress[target]=state;
 auto buffs=ch.value("active_buffs",Json::object());buffs.erase(source);
 auto cooldowns=ch.value("skill_cooldowns",Json::object());
 const auto cooldown=std::max(cooldowns.value(source,0.),cooldowns.value(target,0.));
 cooldowns.erase(source);if(cooldown>0)cooldowns[target]=cooldown;
 // The owner selected preservation: no Fletta refund or new investment on a coupon swap.
 ch["skills"]=learned;ch["super_skill_progress"]=progress;ch["active_buffs"]=buffs;ch["skill_cooldowns"]=cooldowns;
 return body;
}
bool SkillBook::allowed(int cls,std::uint32_t skill)const{std::vector<int>codes={cls/100*100,cls};if(cls%10)codes.push_back(cls/10*10);for(auto code:codes){auto key=std::to_string(code);if(trees_.contains(key))for(auto id:trees_.at(key))if(id==skill)return true;}return false;}
bool SkillBook::is_super(int cls,std::uint32_t skill)const{
 const auto key=std::to_string(skill);
 return super_tree_.contains(key)&&super_tree_.at(key).at("family")==cls/100*100;
}
bool SkillBook::super_allowed(const Json&ch,std::uint32_t skill)const{
 const int cls=ch.at("cls");if(!is_super(cls,skill)||cls%100==0)return false;
 const auto& node=super_tree_.at(std::to_string(skill));
 const int branch=cls%10?2:1,grade=ch.value("profession_grade",0);
 return grade>=0&&grade<=2&&(node.at("branch").get<int>()<branch||
     (node.at("branch")==branch&&node.at("slot").get<int>()<=grade));
}
Bytes SkillBook::super_list(const Json&ch)const{
 // Captured S05/0c has fifteen packed slots: LE32 ID, rank, option, progress.
 Bytes body(6+15*13);put32(body,0,ch.at("eid"),true);put16(body,4,15,true);
 const auto learned=ch.value("skills",Json::object()),progress=ch.value("super_skill_progress",Json::object());
 if(!learned.is_object()||!progress.is_object())throw std::runtime_error("Estado salvo de super-habilidades inválido; dados preservados");
 std::size_t count=0;
 for(const auto& [key,node]:super_tree_.items()){
  if(node.at("family")!=ch.at("cls").get<int>()/100*100)continue;
  const int rank=learned.value(key,0);const auto state=progress.value(key,Json{{"points",0},{"option",0}});
  const auto points=state.at("points").get<std::int64_t>();const auto option=state.at("option").get<std::uint32_t>();
  if(!rank&&!points&&!option)continue;
  if(rank<0||rank>5||points<0||points>0xffffffffLL||count>=15||
     (rank&&!catalog_.at(key).at("levels").contains(std::to_string(rank))))
   throw std::runtime_error("Progresso salvo de super-habilidade inválido; dados preservados");
  const auto at=6+13*count++;put32(body,at,static_cast<std::uint32_t>(std::stoul(key)));
  body[at+4]=static_cast<unsigned char>(rank);put32(body,at+5,option);put32(body,at+9,static_cast<std::uint32_t>(points));
 }
 return body;
}
Bytes SkillBook::invest_super(Json&ch,const Bytes&body)const{
 if(body.size()!=16||get32(body,0,true)!=ch.at("eid")||get16(body,13,true)!=0x2a6)
  throw std::runtime_error("Pedido nativo de super-habilidade inválido");
 const auto id=get32(body,4,true),amount=get32(body,8,true);const auto key=std::to_string(id);
 if(!amount||!super_allowed(ch,id))throw std::runtime_error("Super-habilidade indisponível para esta profissão ou grau");
 const auto& node=super_tree_.at(key);const auto& levels=catalog_.at(key).at("levels");const auto& costs=node.at("fletta_costs");
 if(costs.size()!=5)throw std::runtime_error("Tabela de custos de super-habilidade inválida");
 auto learned=ch.value("skills",Json::object()),progress=ch.value("super_skill_progress",Json::object());
 if(!learned.is_object()||!progress.is_object())throw std::runtime_error("Estado salvo de habilidades inválido; dados preservados");
 for(const auto& [other,rule]:super_tree_.items())if(other!=key&&rule==node&&
    (learned.value(other,0)>0||progress.value(other,Json::object()).value("points",0LL)>0))
  throw std::runtime_error("A outra super-habilidade desta etapa já foi escolhida");
 int rank=learned.value(key,0);const int previous_rank=rank;
 if(rank<0||rank>=5)throw std::runtime_error("Super-habilidade já está no nível máximo ou possui um estado inválido");
 auto state=progress.value(key,Json{{"points",0},{"option",0}});
 auto points=state.at("points").get<std::int64_t>();
 if(points<0||points>=costs.at(rank).get<std::int64_t>())
  throw std::runtime_error("Progresso salvo inválido; dados preservados");
 std::int64_t remaining=-points;
 for(int next=rank+1;next<=5;++next){
  const auto cost=costs.at(next-1).get<std::int64_t>();
  if(cost<=0||cost>0xffffffffLL)throw std::runtime_error("Custo nativo de Fletta inválido");
  remaining+=cost;
 }
 if(amount>remaining)throw std::runtime_error("Quantidade maior que a necessária para alcançar o nível máximo");
 const auto balance=ch.value("fletta",std::int64_t(0));
 if(balance<amount)throw std::runtime_error("Pontos Fletta insuficientes");
 points+=amount;
 while(rank<5&&points>=costs.at(rank).get<std::int64_t>()){
  const auto& level=levels.at(std::to_string(rank+1));
  if(ch.at("level").get<int>()<level.at("level").get<int>())throw std::runtime_error("Nível insuficiente para evoluir esta super-habilidade");
  points-=costs.at(rank).get<std::int64_t>();++rank;
 }
 if(rank)learned[key]=rank;
 state["points"]=points;progress[key]=state;
 ch["skills"]=learned;ch["super_skill_progress"]=progress;ch["fletta"]=balance-amount;
 auto reply=body;reply[12]=static_cast<unsigned char>(rank);put16(reply,13,0x2a6,true);
 reply[15]=previous_rank==0&&rank>0?1:2;return reply;
}
bool SkillBook::initialize(Json&ch)const{if(ch.contains("skills")){if(!ch.at("skills").is_object())throw std::runtime_error("Invalid saved skill state; preserved for repair");return false;}ch["skills"]=Json::object();int family=ch.at("cls").get<int>()/100*100;for(auto&row:initial_)if(row.at("class")==family&&row.at("initial_rank").get<int>())ch["skills"][std::to_string(row.at("id").get<std::uint32_t>())]=row.at("initial_rank");return true;}
Json SkillBook::fletta(const Json&ch)const{Json result=Json::object();auto paths=ch.value("fletta_paths",Json::object());for(auto it=paths.begin();it!=paths.end();++it){Json best;for(auto&row:attributes_)if(row.at("group")==std::stoi(it.key())&&row.at("path")==it.value().at("path")&&row.at("threshold").get<int>()<=it.value().at("points").get<int>()&&(best.is_null()||row.at("rank").get<int>()>best.at("rank").get<int>()))best=row;if(!best.is_null())result[std::to_string(best.at("skill_id").get<std::uint32_t>())]=best.at("rank");}return result;}
Bytes SkillBook::list(const Json&ch,const Bytes&carrier)const{
 if(carrier.size()<6||carrier.size()!=6+carrier[5]*8u)throw std::runtime_error("Invalid native skill-list carrier");
 std::vector<std::pair<std::uint32_t,std::uint32_t>>known;auto learned=ch.value("skills",Json::object());int cls=ch.at("cls");
 auto valid=[&](std::uint32_t id){auto key=std::to_string(id);return learned.contains(key)&&cast_allowed(cls,id)&&catalog_.contains(key)&&catalog_.at(key).at("levels").contains(std::to_string(learned.at(key).get<int>()));};
 auto add=[&](std::uint32_t id,std::uint32_t rank){auto it=std::find_if(known.begin(),known.end(),[&](auto&r){return r.first==id;});if(it==known.end())known.push_back({id,rank});else it->second=rank;};
 for(int index=0;index<carrier[5];index++){auto id=get32(carrier,6+index*4),rank=get32(carrier,6+carrier[5]*4+index*4);if(id<1000||id>=20000||valid(id))add(id,valid(id)?learned.at(std::to_string(id)).get<std::uint32_t>():rank);}
 for(auto it=learned.begin();it!=learned.end();++it){auto id=static_cast<std::uint32_t>(std::stoul(it.key()));if(valid(id))add(id,it.value());}auto flettaEntries=fletta(ch);for(auto&entry:flettaEntries.items())add(static_cast<std::uint32_t>(std::stoul(entry.key())),entry.value());
 if(known.size()>255)throw std::runtime_error("Native skill-list capacity exceeded");Bytes body(6+known.size()*8);put32(body,0,ch.at("eid"),true);body[5]=static_cast<unsigned char>(known.size());for(std::size_t i=0;i<known.size();i++){put32(body,6+i*4,known[i].first);put32(body,6+known.size()*4+i*4,known[i].second);}return body;
}
Bytes SkillBook::learn(Json&ch,const Bytes&body,bool upgrade)const{
 if(body.size()!=(upgrade?14u:10u)||get32(body,0,true)!=ch.at("eid")||get16(body,body.size()-2,true)!=36)throw std::runtime_error("Invalid native skill learning request");auto id=get32(body,4,true);auto key=std::to_string(id);int cls=ch.at("cls");if(!allowed(cls,id)||!catalog_.contains(key))throw std::runtime_error("Habilidade de outra classe ou profissao");auto&row=catalog_.at(key);int kind=row.at("kind");if(!row.value("passive",false)&&kind!=11&&kind!=13&&kind!=21&&kind!=23&&kind!=25)throw std::runtime_error("Habilidade avancada ainda indisponivel; nenhum ponto gasto");
 for(auto& prerequisite:row.value("prerequisites",Json::array()))if(ch.at("skills").value(std::to_string(prerequisite.at("id").get<int>()),0)<prerequisite.at("rank").get<int>())throw std::runtime_error("Aprenda primeiro a habilidade anterior no rank exigido");
 int rank=ch.at("skills").value(key,0);if(upgrade?(rank==0||get32(body,8,true)!=static_cast<std::uint32_t>(rank)):rank!=0)throw std::runtime_error("Transicao de rank invalida");auto next=std::to_string(rank+1);if(!row.at("levels").contains(next))throw std::runtime_error("Rank maximo atingido");auto level=row.at("levels").at(next);if(ch.at("level").get<int>()<level.at("level").get<int>())throw std::runtime_error("Nivel insuficiente para aprender esta habilidade");int cost=level.at("points");if(cost<0||ch.value("skill_points",0)<cost)throw std::runtime_error("Pontos de habilidade insuficientes");ch["skill_points"]=ch.value("skill_points",0)-cost;ch["skills"][key]=rank+1;auto reply=body;if(upgrade)put32(reply,8,rank+1,true);return reply;
}
Bytes SkillBook::allocate(Json&ch,const Bytes&body)const{
 if(body.size()!=18||get32(body,0,true)!=ch.at("eid")||get16(body,16,true)!=0x120)throw std::runtime_error("Invalid native Fletta request");auto group=get32(body,4,true),path=get32(body,8,true),points=get32(body,12,true);bool found=false;for(auto&row:attributes_)if(row.at("group")==group&&row.at("path")==path)found=true;if(!found||points<1||points>1600)throw std::runtime_error("Caminho ou quantidade Fletta invalida");auto paths=ch.value("fletta_paths",Json::object());auto key=std::to_string(group);auto prior=paths.value(key,Json{{"path",path},{"points",0}});if(prior.at("path")!=path)throw std::runtime_error("Outro caminho Fletta ja foi escolhido");auto current=prior.at("points").get<std::uint32_t>();if(ch.value("fletta",std::int64_t(0))<points||current>1600-points)throw std::runtime_error("Saldo Fletta insuficiente ou caminho no limite");paths[key]={{"path",path},{"points",current+points}};ch["fletta"]=ch.value("fletta",std::int64_t(0))-points;ch["fletta_paths"]=paths;return body;
}
std::vector<Bytes> SkillBook::allocations(const Json&ch)const{std::vector<Bytes>result;auto paths=ch.value("fletta_paths",Json::object());for(auto&entry:paths.items()){Bytes body(18);put32(body,0,ch.at("eid"),true);put32(body,4,static_cast<std::uint32_t>(std::stoul(entry.key())),true);put32(body,8,entry.value().at("path"),true);put32(body,12,entry.value().at("points"),true);put16(body,16,0x120,true);result.push_back(std::move(body));}return result;}
}
