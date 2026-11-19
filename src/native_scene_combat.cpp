#include "native/scene.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace rosh {
namespace {
void apply_effect(Entity&mob,std::uint32_t caster,const Json&skill,double timestamp){
 if(skill.value("duration",0.)<=0||skill.value("effects",Json::array()).empty())return;
 for(auto it=mob.debuffs.begin();it!=mob.debuffs.end();)if(it.value().value("expires",0.)<=timestamp)it=mob.debuffs.erase(it);else ++it;
 mob.debuffs[std::to_string(skill.at("id").get<int>())]={{"caster_eid",caster},{"rank",skill.at("rank")},{"effects",skill.at("effects")},{"expires",timestamp+skill.at("duration").get<double>()}};
}
double defense(const Entity&mob,bool magic,double stamp){
 double flat=0,percent=0;for(auto&buff:mob.debuffs)if(buff.value("expires",0.)>stamp)for(auto&fx:buff.at("effects")){int op=fx.at("operation");if((magic&&(op==19||op==73))||(!magic&&(op==16||op==17))){flat+=fx.value("flat",0.);percent+=fx.value("percent",0.);}}
 return std::max(0.,(3+1.6*mob.level+flat)*std::max(0.,1+percent/100));
}
}
void WorldScene::skill_effects(const std::string&key,std::uint32_t target,const Json&profile,double timestamp){
 std::lock_guard lock(mutex_);auto p=players_.find(key);if(p==players_.end())return;auto found=maps_.find(p->second.map);if(found==maps_.end()||!found->second->count(target))return;
 auto entities=std::make_shared<SceneEntities>(*found->second);auto&mob=entities->at(target);if(!mob.attackable(p->second.combat.value("pk_mode",false))||mob.position.distance(p->second.position)>350)return;apply_effect(mob,p->second.eid,profile,timestamp);found->second=entities;
}
int WorldScene::depart(const std::string&key){
 int damage=0;{std::lock_guard lock(mutex_);auto it=players_.find(key);if(it!=players_.end()){it->second.hidden=true;it->second.hp=0;it->second.combat=Json::object();damage=it->second.pending_damage;it->second.pending_damage=0;}}
 remove_player(key);return damage;
}
void WorldScene::restore(const std::string&name,Sqlite&db,const std::string&scope,double timestamp){
 map(name);SpawnPersistence::initialize(db);std::lock_guard lock(mutex_);auto entities=std::make_shared<SceneEntities>(*maps_.at(name));for(auto&[id,mob]:*entities)SpawnPersistence::restore(db,scope,mob,timestamp);maps_[name]=std::move(entities);
}
int WorldScene::take_damage(const std::string&key){std::lock_guard lock(mutex_);auto it=players_.find(key);if(it==players_.end())return 0;auto damage=it->second.pending_damage;it->second.pending_damage=0;return damage;}
void WorldScene::simulate(double timestamp){
 if(!std::isfinite(timestamp))throw std::runtime_error("Invalid world simulation clock");
 struct Message{std::function<void(unsigned char,unsigned char,const Bytes&)> send;int cat,sub;Bytes body;};std::vector<Message> messages;
 {
  std::lock_guard lock(mutex_);
  tick_radars(timestamp);
  for(auto&entry:maps_){auto map=entry.first;auto&snapshot=entry.second;
   if(timestamp<next_simulation_[map])continue;
   std::map<std::uint32_t,Observer> observers;
   for(auto&[key,p]:players_)if(p.map==map&&!p.combat.empty())observers[p.eid]={p.eid,p.position,p.hp,p.combat.value("level",1),p.hidden,true,p.combat.value("moral_value",std::int64_t(0))};
   if(observers.empty())continue;
   next_simulation_[map]=timestamp+catalog_.simulation_interval();
   auto&simulation=simulations_[map];if(!simulation)simulation=std::make_unique<MobSimulation>(catalog_,combat_random_(),timestamp-.25);
   auto entities=std::make_shared<SceneEntities>(*snapshot);
   auto events=simulation->tick(*entities,observers,timestamp);
   for(const auto&event:events){
    auto&mob=entities->at(event.eid);
    auto publish=[&](int cat,int sub,const Bytes&body){for(auto&[key,p]:players_)if(p.map==map&&p.position.distance(mob.position)<=3000)messages.push_back({p.send,cat,sub,body});};
    if(event.kind==EntityEvent::Move||event.kind==EntityEvent::Stop||event.kind==EntityEvent::Reset){
     for(auto&[key,p]:players_)if(p.map==map&&p.position.distance(mob.position)<=3000){Bytes body(36);body[0]=1;body[1]=event.kind==EntityEvent::Move?event.move_mode:2;put32(body,2,mob.eid,true);putfloat(body,6,event.from.x);putfloat(body,10,event.from.y);putfloat(body,14,event.from.z);putfloat(body,18,mob.position.x);putfloat(body,22,mob.position.y);putfloat(body,26,mob.position.z);putfloat(body,30,event.kind==EntityEvent::Move?event.move_speed:static_cast<float>(mob.angle));messages.push_back({p.send,4,1,body});}
    } else if (event.kind == EntityEvent::Detection || event.kind == EntityEvent::Aggro ||
               event.kind == EntityEvent::Disengage) {
     // Genesis S05/0b carries the actor ID followed by the reaction state.
     std::uint32_t state = 0;
     if (event.kind == EntityEvent::Detection) state = 1;
     if (event.kind == EntityEvent::Aggro) state = 10;
     Bytes body(8);
     put32(body, 0, mob.eid, true);
     put32(body, 4, state, true);
     publish(5, 11, body);
    }else if(event.kind==EntityEvent::Respawn){
     // Per-session visibility owns creation so respawn cannot create a second actor.
    }
    else if(event.kind==EntityEvent::AttackStart||event.kind==EntityEvent::AttackImpact){
     auto target=std::find_if(players_.begin(),players_.end(),[&](auto&entry){return entry.second.map==map&&entry.second.eid==event.target;});
     if(target==players_.end()||target->second.hidden||target->second.hp<=0)continue;auto&p=target->second;
     bool start=event.kind==EntityEvent::AttackStart||mob.rules.at("attack_windup_seconds").get<double>()<=0;
     if(start){++mob.attack_sequence;publish(5,0,combat_start(mob.eid,p.eid,mob.attack_sequence,mob.position,p.position,mob.angle));}
     if(event.kind==EntityEvent::AttackImpact){
     bool magic=mob.rules.value("damage_channel",std::string("physical"))=="magic";
     auto defense=std::max(0.,p.combat.value(magic?"magic_defense":"defense",0.));auto reduction=std::clamp(p.combat.value("damage_reduction",0.),0.,70.);
      int damage=std::max(1,static_cast<int>(std::nearbyint(event.damage*100/(100+defense)*(1-reduction/100))));
      auto evasion=std::clamp(std::max(0.,p.combat.value("evasion",0.))+std::max(0.,p.combat.value("defense_rating",0.))*.5,0.,75.);
      if(std::uniform_real_distribution<double>(0,100)(combat_random_)<evasion)damage=0;
      if(!magic&&damage&&p.combat.value("shield_active",false)&&std::uniform_real_distribution<double>(0,100)(combat_random_)<std::clamp(p.combat.value("block_chance",0.),0.,75.))damage=std::max(1,damage/2);
      if(p.combat.value("god_mode",false))damage=0;
      auto applied=std::min(damage,p.hp);p.hp-=applied;p.pending_damage+=applied;
      auto reflected=static_cast<std::uint32_t>(std::floor(applied*std::clamp(p.combat.value(magic?"magic_reflection":"physical_reflection",0.),0.,100.)/100));
      if(reflected&&mob.combat){auto&queued=p.reflected[mob.eid];if(queued.first!=mob.life_generation)queued={mob.life_generation,0};queued.second=std::min<std::uint64_t>(std::uint64_t(queued.second)+reflected,std::numeric_limits<std::uint32_t>::max());}
      publish(5,2,combat_damage(damage_carrier_,mob.eid,p.eid,damage,p.position,true,mob.attack_sequence,p.hp>0));
      // Only the owning session mutates/persists character HP; other threads
      // queue damage here and never access that session's SQLite connection.
     }
    }
    if(mob.hp_dirty){publish(8,6,combat_hp(hp_carrier_,mob.eid,mob.hp));mob.hp_dirty=false;}
   }
   snapshot=std::move(entities);
  }
 }
 for(auto&message:messages)try{message.send(message.cat,message.sub,message.body);}catch(const std::exception&){}
}
std::uint32_t WorldScene::strike(const std::string&key,unsigned char sub,const Bytes&body,double timestamp,const std::function<void(Entity&)>&commit_death,const Json&skill){
 return strike_impl(key,sub,body,timestamp,commit_death,skill,{});
}
int WorldScene::resolve_reflections(const std::string&key,double timestamp,const std::function<void(Entity&)>&commit_death){
 std::map<std::uint32_t,std::pair<std::uint64_t,std::uint32_t>>queued;std::uint32_t eid=0;std::string map;
 {std::lock_guard lock(mutex_);auto player=players_.find(key);if(player==players_.end())return 0;eid=player->second.eid;map=player->second.map;queued.swap(player->second.reflected);}
 int kills=0;
 for(auto it=queued.begin();it!=queued.end();++it){
  Bytes hit(32);put32(hit,1,eid,true);put32(hit,28,it->first,true);
  try{strike_impl(key,1,hit,timestamp,[&](Entity&mob){if(!commit_death)throw std::runtime_error("Reflection reward handler missing");commit_death(mob);++kills;},Json::object(),it->second);}
  catch(...){
   // A failed reward/save leaves the immutable scene unchanged. Retry only
   // uncommitted impacts; never silently lose them or replay successful kills.
   std::lock_guard lock(mutex_);auto player=players_.find(key);
   if(player!=players_.end()&&player->second.map==map)
    for(auto pending=it;pending!=queued.end();++pending){auto&dst=player->second.reflected[pending->first];if(dst.first!=pending->second.first)dst=pending->second;else dst.second=std::min<std::uint64_t>(std::uint64_t(dst.second)+pending->second.second,std::numeric_limits<std::uint32_t>::max());}
   throw;
  }
 }
 return kills;
}
std::uint32_t WorldScene::strike_impl(const std::string&key,unsigned char sub,const Bytes&body,double timestamp,const std::function<void(Entity&)>&commit_death,const Json&skill,const std::optional<std::pair<std::uint64_t,std::uint32_t>>&reflected){
 std::uint32_t applied_damage=0;
 struct Message{std::function<void(unsigned char,unsigned char,const Bytes&)>send;int cat,sub;Bytes body;};std::vector<Message>messages;
 {
  std::lock_guard lock(mutex_);auto player=players_.find(key);if(player==players_.end()||player->second.combat.empty())throw std::runtime_error("O mapa ainda nao terminou de carregar");auto&p=player->second;
  if(reflected&&(p.hidden||p.hp<=0))return 0;
  if(p.hidden||p.hp<=0)throw std::runtime_error("Desative vanish e mantenha o personagem vivo para atacar");
  auto target=attack_target(body,sub,p.eid);auto found=maps_.find(p.map);if(found==maps_.end())throw std::runtime_error("Mapa nao carregado");auto original=found->second->find(target);
  if(reflected&&(original==found->second->end()||!original->second.combat||!original->second.alive||original->second.position.distance(p.position)>350))return 0;
  if(original==found->second->end()||!original->second.attackable(p.combat.value("pk_mode",false))||
     (original->second.position.distance(p.position)>350||std::abs(original->second.position.z-p.position.z)>original->second.rules.value("vertical_range",1e15)))throw std::runtime_error("Alvo invalido ou fora de alcance");
  if(reflected&&reflected->first!=original->second.life_generation)return 0;
  bool special=body[5]==1;
  if(body[5]>1||(special&&(sub!=1||skill.empty()||skill.at("id")!=get32(body,6,true)||skill.at("target")!=target)))throw std::runtime_error("Ataque especial exige o fluxo de skill autorizado");
  if(reflected&&!catalog_.event_level_allowed(original->second.npc_id,original->second.level,p.combat.value("level",1)))return 0;
  if(!catalog_.event_level_allowed(original->second.npc_id,original->second.level,p.combat.value("level",1)))throw std::runtime_error("Seu nivel nao e permitido para este semi-chefe");
  if(sub==0){messages.push_back({p.send,5,0,body});}
  else{
   if(!std::isfinite(timestamp))throw std::runtime_error("Relogio de combate invalido");
   if(!special&&!reflected&&timestamp-p.last_hit<attack_interval(p.combat.value("attack_speed",1.),p.combat.value("attack_speed_multiplier",1.)))return 0;
   auto entities=std::make_shared<SceneEntities>(*found->second);auto&mob=entities->at(target);
   if(special&&(skill.at("kind")==11||skill.value("area",false)))apply_effect(mob,p.eid,skill,timestamp);
   auto roll=[&](double power,bool magic){auto bounds=damage_range(power,mob.level,p.combat.value("level",1),defense(mob,magic,timestamp));return std::uniform_int_distribution<std::uint32_t>(bounds.first,bounds.second)(combat_random_);};
   std::uint32_t damage=0;
   if(reflected)damage=reflected->second;
   else if(!special)damage=roll(p.combat.value("attack",0.),false);
   else if(skill.value("damaging",false)){
    auto [physical,magic]=skill_damage_channels(skill,p.combat.value("profession",100));
    double sum=0;for(bool channel:{false,true})if(channel?magic:physical){auto low=skill.value(channel?"magic_min":"physical_min",0.),high=skill.value(channel?"magic_max":"physical_max",0.);auto power=p.combat.value(channel?"magic_attack":"attack",0.)*skill.value("power",0.)+std::uniform_real_distribution<double>(std::min(low,high),std::max(low,high))(combat_random_);sum+=roll(std::max(0.,power),channel);}damage=static_cast<std::uint32_t>(std::min(sum,4294967295.));
   }
   if(!reflected&&!special&&damage&&std::uniform_real_distribution<double>(0,1)(combat_random_)>=physical_hit_chance(p.combat.value("accuracy",0.),mob.rules.value("evasion_percent",0.)))damage=0;
   int type=!reflected&&damage&&std::uniform_real_distribution<double>(0,100)(combat_random_)<std::clamp(p.combat.value("critical",0.),0.,50.)?2:1;
   if(type==2&&damage)damage=static_cast<std::uint32_t>(std::clamp(std::nearbyint(damage*p.combat.value("critical_multiplier",1.5)),1.,4294967295.));
   auto applied=std::min<std::uint32_t>(damage,mob.hp);applied_damage=applied;mob.damage[p.eid]+=applied;mob.damage_names[p.eid]=native_actor_name(p.appearance);
   auto party=party_members_.find(p.eid);auto group=party==party_members_.end()?"solo:"+std::to_string(p.eid):"party:"+party->second;mob.party_damage[group]+=applied;mob.party_contributors[group][p.eid]+=applied;mob.party_damage_names[group]=party==party_members_.end()?mob.damage_names[p.eid]:party_labels_.at(party->second);
   mob.hp=damage>=static_cast<std::uint32_t>(mob.hp)?0:mob.hp-damage;p.last_damage_target=mob.eid;
   if(!mob.hp){if(!commit_death)throw std::runtime_error("Fluxo de recompensa nao conectado: morte cancelada sem perder o mob");mob.alive=false;mob.state=MobState::Dead;
    if(mob.category=="boss"&&!mob.party_damage.empty()){
     auto winner=mob.party_damage.begin();for(auto it=mob.party_damage.begin();it!=mob.party_damage.end();++it)if(it->second>winner->second)winner=it;
     std::uint64_t highest=0;for(auto&[member,damage]:mob.party_contributors.at(winner->first))if(damage){mob.loot_recipients.insert(member);if(damage>highest){highest=damage;mob.loot_owner=member;}}
    }
    commit_death(mob);
   }
   else{auto&simulation=simulations_[p.map];if(!simulation)simulation=std::make_unique<MobSimulation>(catalog_,combat_random_(),timestamp-.25);simulation->provoke(mob,{p.eid,p.position,p.hp,p.combat.value("level",1),p.hidden,true});}
   auto hit=combat_damage(damage_carrier_,p.eid,mob.eid,damage,mob.position,false,get32(body,6,true),mob.alive,type);if(special){hit[5]=1;put32(hit,11,skill.at("kind"),true);}auto hp=combat_hp(hp_carrier_,mob.eid,mob.hp);
   for(auto&[otherKey,other]:players_)if(other.map==p.map&&other.position.distance(mob.position)<=3000){messages.push_back({other.send,5,2,hit});messages.push_back({other.send,8,6,hp});}
   if(mob.category=="boss"&&!mob.alive){
    auto rank=[](const auto&scores,const auto&names){std::vector<std::pair<std::uint64_t,std::string>>ordered;for(auto&[id,damage]:scores){auto n=names.find(id);ordered.push_back({damage,n==names.end()?"Unknown":n->second});}std::sort(ordered.begin(),ordered.end(),[](auto&a,auto&b){return a.first==b.first?a.second<b.second:a.first>b.first;});Json rows=Json::array();for(std::size_t i=0;i<std::min<std::size_t>(10,ordered.size());i++)rows.push_back({{"name",ordered[i].second},{"damage",ordered[i].first}});return rows;};
    Json ranking={{"cmd","asmara.boss.damage"},{"boss",mob.name},{"npc",mob.npc_id},{"solo",rank(mob.damage,mob.damage_names)},{"party",rank(mob.party_damage,mob.party_damage_names)}};auto text=ranking.dump();auto overlay=overlay_carrier_;overlay.insert(overlay.end(),text.begin(),text.end());overlay.push_back(0);
    for(auto&[otherKey,other]:players_)if(other.map==p.map&&other.position.distance(mob.position)<=3000)messages.push_back({other.send,2,11,overlay});
   }
   found->second=std::move(entities);if(!reflected)p.last_hit=timestamp;
  }
 }
 for(auto&message:messages)try{message.send(message.cat,message.sub,message.body);}catch(const std::exception&){}
 return applied_damage;
}
}
