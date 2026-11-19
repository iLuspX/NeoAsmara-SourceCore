#include "native/scene.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace rosh {
Json WorldScene::party_auras(const std::string& key,double timestamp){
 std::lock_guard lock(mutex_);
 Json result=Json::object();
 auto player=players_.find(key);
 if(player==players_.end()||player->second.hp<=0||player->second.hidden)return result;
 auto member=party_members_.find(player->second.eid);
 for(const auto& [owner,radar]:defense_radars_){
  const auto source=players_.find(owner.first);
  if(source==players_.end()||source->second.hidden||source->second.hp<=0||
     source->second.map!=radar.map||radar.map!=player->second.map||
     radar.position.distance(player->second.position)>80||radar.aura.at("expires").get<double>()<=timestamp||
     (owner.first!=key&&!same_party(source->second.eid,player->second.eid)))continue;
  const auto group=std::to_string(owner.second);
  if(!result.contains(group)||radar.aura.at("power").get<double>()>result.at(group).at("power").get<double>())
   result[group]=radar.aura;
 }
 if(member==party_members_.end())return result;
 for(const auto& [otherKey,source]:players_){
  if(otherKey==key||source.map!=player->second.map||source.hp<=0||source.hidden)continue;
  auto sourceGroup=party_members_.find(source.eid);
  if(sourceGroup==party_members_.end()||sourceGroup->second!=member->second)continue;
  auto active=source.combat.value("party_auras",Json::object());
  for(const auto& [group,aura]:active.items())
   if(aura.at("expires").get<double>()>timestamp&&
      (!result.contains(group)||aura.at("power").get<double>()>result.at(group).at("power").get<double>()))
    result[group]=aura;
 }
 return result;
}
bool WorldScene::healing_reachable(const PlayerView& source,const PlayerView& target){
 return !source.hidden&&source.hp>0&&!target.hidden&&target.hp>0&&
        !target.combat.empty()&&source.map==target.map&&source.position.distance(target.position)<=350;
}
void WorldScene::queue_healing(PlayerView& recipient,double flat,double percent,double multiplier){
 const auto maximum=std::clamp(recipient.combat.value("max_hp",0.),0.,double(std::numeric_limits<int>::max()));
 const auto missing=std::max(0.,maximum-recipient.hp-recipient.pending_healing);
 const auto amount=std::clamp(std::nearbyint((flat+maximum*percent/100)*multiplier),0.,missing);
 recipient.pending_healing+=static_cast<int>(amount);
}
bool WorldScene::same_party(std::uint32_t source,std::uint32_t target)const{
 const auto group=party_members_.find(source),member=party_members_.find(target);
 return group!=party_members_.end()&&member!=party_members_.end()&&group->second==member->second;
}
bool WorldScene::can_heal_player(const std::string& key,std::uint32_t target,bool party_only){
 std::lock_guard lock(mutex_);
 const auto caster=players_.find(key);
 if(caster==players_.end()||target==caster->second.eid||
    (party_only&&!same_party(caster->second.eid,target)))return false;
 return std::any_of(players_.begin(),players_.end(),[&](const auto& entry){
  return entry.second.eid==target&&healing_reachable(caster->second,entry.second);
 });
}
void WorldScene::heal_player(const std::string& key,std::uint32_t target,double flat,double percent,double multiplier,bool party_only){
 std::lock_guard lock(mutex_);
 const auto caster=players_.find(key);
 if(caster==players_.end()||target==caster->second.eid||
    (party_only&&!same_party(caster->second.eid,target)))return;
 for(auto& [other,recipient]:players_)if(recipient.eid==target&&healing_reachable(caster->second,recipient)){
  queue_healing(recipient,flat,percent,multiplier);
  return;
 }
}
void WorldScene::heal_party(const std::string& key,double flat,double percent,double multiplier){
 std::lock_guard lock(mutex_);
 const auto caster=players_.find(key);
 if(caster==players_.end()||caster->second.hidden||caster->second.hp<=0)return;
 const auto group=party_members_.find(caster->second.eid);
 if(group==party_members_.end())return;
 for(auto& [other,recipient]:players_){
  if(other==key||!healing_reachable(caster->second,recipient))continue;
  const auto member=party_members_.find(recipient.eid);
  if(member==party_members_.end()||member->second!=group->second)continue;
  queue_healing(recipient,flat,percent,multiplier);
 }
}
void WorldScene::place_healing_radar(const std::string& key,double stamp,double duration,double health_percent,double mana){
 std::lock_guard lock(mutex_);
 auto source=players_.find(key);
 if(source==players_.end()||source->second.hp<=0||source->second.hidden||duration<=0)return;
 healing_radars_[key]={source->second.map,source->second.position,stamp+duration,stamp+5.,health_percent,mana};
}
void WorldScene::place_defense_radar(const std::string& key,int aura_skill,double stamp,double duration,Json effects){
 std::lock_guard lock(mutex_);
 const auto source=players_.find(key);
 if(source==players_.end()||source->second.hidden||source->second.hp<=0||duration<=0||effects.empty())return;
 double power=0;
 for(const auto& effect:effects)power+=effect.at("percent").get<double>();
 defense_radars_[{key,aura_skill}]={source->second.map,source->second.position,
   {{"skill",aura_skill},{"power",power},{"expires",stamp+duration},{"effects",std::move(effects)}}};
}
void WorldScene::tick_radars(double timestamp){
 for(auto it=defense_radars_.begin();it!=defense_radars_.end();){
  const auto source=players_.find(it->first.first);
  if(source==players_.end()||source->second.map!=it->second.map||source->second.hp<=0||
     it->second.aura.at("expires").get<double>()<=timestamp)
   it=defense_radars_.erase(it);
  else ++it;
 }
 for(auto it=healing_radars_.begin();it!=healing_radars_.end();){
  auto source=players_.find(it->first);
  auto& radar=it->second;
  if(source==players_.end()||source->second.map!=radar.map||source->second.hp<=0||radar.expires<=timestamp){
   it=healing_radars_.erase(it);
   continue;
  }
  if(radar.next<=timestamp){
   for(auto& [key,recipient]:players_){
    if(recipient.map!=radar.map||recipient.hp<=0||recipient.hidden||recipient.combat.empty()||
       recipient.position.distance(radar.position)>80||
       (key!=it->first&&!same_party(source->second.eid,recipient.eid)))continue;
    queue_healing(recipient,0,radar.health_percent,1.);
    const auto maximum=std::clamp(recipient.combat.value("max_mp",0.),0.,double(std::numeric_limits<int>::max()));
    const auto current=recipient.combat.value("mp",maximum);
    const auto amount=std::clamp(std::nearbyint(radar.mana),0.,std::max(0.,maximum-current-recipient.pending_mana));
    recipient.pending_mana+=static_cast<int>(amount);
   }
   radar.next+=5.*(std::floor((timestamp-radar.next)/5.)+1);
  }
  ++it;
 }
}
std::pair<int,int> WorldScene::take_recovery(const std::string& key){
 std::lock_guard lock(mutex_);
 auto player=players_.find(key);
 if(player==players_.end())return {0,0};
 auto& p=player->second;
 const auto result=std::make_pair(p.pending_healing,p.pending_mana);
 p.pending_healing=p.pending_mana=0;
 return result;
}
void WorldScene::restore_recovery(const std::string& key,int hp,int mp){
 std::lock_guard lock(mutex_);
 auto player=players_.find(key);
 if(player==players_.end())return;
 auto& p=player->second;
 p.pending_healing+=std::min(hp,std::numeric_limits<int>::max()-p.pending_healing);
 p.pending_mana+=std::min(mp,std::numeric_limits<int>::max()-p.pending_mana);
}
void WorldScene::leave_party(std::uint32_t eid){
 std::lock_guard lock(mutex_);auto found=party_members_.find(eid);if(found!=party_members_.end()){auto group=found->second;party_members_.erase(found);if(std::none_of(party_members_.begin(),party_members_.end(),[&](auto&p){return p.second==group;}))party_labels_.erase(group);}invitations_.erase(eid);
}
std::vector<std::string> WorldScene::party(const std::string&key,const std::vector<std::string>&args){
 std::function<void(unsigned char,unsigned char,const Bytes&)> notify;std::string notification;std::vector<std::string> result;
 {
  std::lock_guard lock(mutex_);auto found=players_.find(key);if(found==players_.end())throw std::runtime_error("Entre no mapa primeiro");auto&player=found->second;auto action=args.empty()?std::string("status"):args[0];for(auto&c:action)c=std::tolower(static_cast<unsigned char>(c));auto member=party_members_.find(player.eid);
  if(action=="status"&&args.size()<=1){result.push_back(member==party_members_.end()?"Solo: "+native_actor_name(player.appearance):"Party de "+party_labels_.at(member->second));}
  else if(action=="invite"&&args.size()==2){
   auto target=std::find_if(players_.begin(),players_.end(),[&](auto&p){return p.second.map==player.map&&!p.second.hidden&&_stricmp(native_actor_name(p.second.appearance).c_str(),args[1].c_str())==0;});
   if(target==players_.end())throw std::runtime_error("Personagem precisa estar no mesmo mapa e canal");if(target->second.eid==player.eid)throw std::runtime_error("Nao pode convidar o proprio personagem");if(party_members_.count(target->second.eid))throw std::runtime_error("Personagem ja esta em uma party");
   std::string group;if(member==party_members_.end()){group=token(20);party_members_[player.eid]=group;party_labels_[group]=native_actor_name(player.appearance);}else group=member->second;
   if(std::count_if(party_members_.begin(),party_members_.end(),[&](auto&p){return p.second==group;})>=8)throw std::runtime_error("Party cheia (8)");invitations_[target->second.eid]={group,GetTickCount64()/1000.+60};notify=target->second.send;notification=native_actor_name(player.appearance)+" convidou voce. Use //party accept (60s).";result.push_back("Convite enviado");
  }else if(action=="accept"&&args.size()==1){
   auto invite=invitations_.find(player.eid);if(invite==invitations_.end())throw std::runtime_error("Convite ausente ou expirado");auto value=invite->second;invitations_.erase(invite);if(value.expires<GetTickCount64()/1000.||!party_labels_.count(value.group))throw std::runtime_error("Convite ausente ou expirado");if(member!=party_members_.end()||std::count_if(party_members_.begin(),party_members_.end(),[&](auto&p){return p.second==value.group;})>=8)throw std::runtime_error("Party indisponivel");party_members_[player.eid]=value.group;result.push_back("Entrada na party confirmada");
  }else if(action=="leave"&&args.size()==1){
   if(member!=party_members_.end()){auto group=member->second;party_members_.erase(member);if(std::none_of(party_members_.begin(),party_members_.end(),[&](auto&p){return p.second==group;}))party_labels_.erase(group);}invitations_.erase(player.eid);result.push_back("Voce saiu da party");
  }else throw std::runtime_error("Use //party invite nome / accept / leave / status");
 }
 if(notify)notify(11,1,notice(notification));return result;
}
std::vector<std::string> WorldScene::damage_ranking(const std::string&key,const std::string&kind){
 if(kind!="solo"&&kind!="party")throw std::runtime_error("Use //damage solo ou //damage party");std::lock_guard lock(mutex_);auto player=players_.find(key);if(player==players_.end()||!player->second.last_damage_target)throw std::runtime_error("Ataque um alvo primeiro");auto map=maps_.find(player->second.map);if(map==maps_.end())throw std::runtime_error("Mapa ausente");auto entity=map->second->find(player->second.last_damage_target);if(entity==map->second->end())throw std::runtime_error("Alvo ausente");auto&mob=entity->second;
 std::vector<std::pair<std::uint64_t,std::string>> rows;if(kind=="solo")for(auto&[id,damage]:mob.damage)rows.push_back({damage,mob.damage_names.count(id)?mob.damage_names.at(id):std::to_string(id)});else for(auto&[id,damage]:mob.party_damage)rows.push_back({damage,mob.party_damage_names.at(id)});
 std::sort(rows.begin(),rows.end(),[](auto&a,auto&b){if(a.first!=b.first)return a.first>b.first;return _stricmp(a.second.c_str(),b.second.c_str())<0;});std::vector<std::string> result={mob.name+" / dano "+kind};for(std::size_t i=0;i<std::min(rows.size(),std::size_t(5));i++)result.push_back(std::to_string(i+1)+". "+rows[i].second+": "+std::to_string(rows[i].first));if(rows.empty())result.push_back("Sem dano registrado neste encontro");return result;
}
}
