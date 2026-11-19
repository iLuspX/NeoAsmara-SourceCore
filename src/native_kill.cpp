#include "native/game.hpp"
#include "native/consumables.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace rosh {
void WorldSession::sync_ground(const Sender&send){
 auto visible=loot_->visible(map_scope(),character_.at("pos"),now());
 for(auto it=ground_visible_.begin();it!=ground_visible_.end();)if(!visible.count(it->first)){send(6,3,GroundLoot::remove(it->first,it->second));it=ground_visible_.erase(it);}else ++it;
 for(auto&[actor,row]:visible)if(!ground_visible_.count(actor)){send(6,4,GroundLoot::spawn(eid(),row));ground_visible_[actor]=row.at("item_id");}
}
void WorldSession::send_quest_kill_items(const QuestKillItems& rewards,const Sender& send){
 if(!rewards.delivered.empty())send_inventory(send,false);
 for(const auto& [id,quantity]:rewards.delivered)
  send(11,1,notice("Item de missao recebido: "+inventory_->name(id)+" x"+std::to_string(quantity)));
 if(rewards.inventory_full)send(11,1,notice("Bolsas cheias. Libere espaco para receber itens da missao."));
}
QuestKillItems WorldSession::commit_kill(Entity&mob){
 auto original=character_;auto originalStats=stats_;auto originalRespawn=mob.respawn_at;
 try{
  Transaction tx(store_.db());auto kill=token(24);auto timestamp=now();auto scope=map_scope();
  auto random=random_bytes(4);double roll=get32(random,0)/4294967295.;
  if(!SpawnPersistence::claim(store_.db(),scope,mob,kill,timestamp,roll))throw std::runtime_error("Esta morte ja foi registrada; recompensa nao foi duplicada");
  if(mob.guard){
   auto moral=character_.value("moral_value",std::int64_t(0));
   character_["moral_value"]=std::max<std::int64_t>(moral,std::int64_t(INT32_MIN)+3500)-3500;
   persist();
   tx.commit();
   return {};
  }
  int level=character_.at("level");int xp=mob.npc_id==1001801?(level<=3?72:63):std::clamp(mob.level*2+4,4,150);
  const int configured_xp = std::clamp(mob.rules.value("dev_xp", xp), 0, 100000000);
  const std::uint64_t base_xp = configured_xp;
  const auto min_xp = (base_xp * 90 + 99) / 100;
  const auto max_xp = base_xp * 110 / 100;
  const auto span = max_xp - min_xp + 1;
  std::uint64_t rolled_xp = min_xp;
  if (span > 1) {
   const auto limit = (0x100000000ULL / span) * span;
   std::uint32_t sample;
   do { sample = get32(random_bytes(4), 0); } while (sample >= limit);
   rolled_xp += sample % span;
  }
  double event_multiplier = 1.;
  if (content_.has("server/devtool_map_rules.json")) {
   const auto configuration = content_.get("server/devtool_map_rules.json");
   for (const auto& event : configuration.value("events", Json::array())) {
    if (event.at("map_id") == map_id_ && timestamp >= event.at("start").get<double>() &&
        timestamp < event.at("end").get<double>()) {
     event_multiplier = std::max(event_multiplier, std::clamp(event.at("xp_multiplier").get<double>(), 1., 10.));
    }
   }
  }
  auto reward=static_cast<std::uint64_t>(std::max(0.,std::nearbyint(rolled_xp*event_multiplier*(1+stats_.value("xp_bonus",0.)/100))));
  auto prior=character_.value("exp",std::uint64_t(0));if(reward>std::numeric_limits<std::uint64_t>::max()-prior)throw std::runtime_error("Experience overflow");character_["exp"]=prior+reward;
  int newLevel=std::max(level,progression_.level(prior+reward));character_["level"]=newLevel;character_["skill_points"]=character_.value("skill_points",0)+newLevel-level;
  auto energy=static_cast<std::int64_t>(std::nearbyint((mob.npc_id==1001801?12:std::clamp(xp/6,1,40))*(1+stats_.value("fletta_bonus",0.)/100)))+character_.value("fletta_energy",std::int64_t(0));
  auto threshold=newLevel<=1?125:125*(newLevel+1);character_["fletta"]=character_.value("fletta",std::int64_t(0))+energy/threshold;character_["fletta_energy"]=energy%threshold;
  character_["moral_value"]=character_.value("moral_value",std::int64_t(0))+(mob.npc_id==1001801?(level<=3?8:7):std::clamp(xp/9,1,20));
  static const std::map<std::string,std::pair<int,int>> bands={{"Spiderling",{9,12}},{"Owlbear Cub",{2,2}},{"Prairie Dog",{2,2}},{"Maulbear Cub",{2,2}},{"Hornboar",{2,2}},{"Rageboar",{8,9}},{"Claybeak",{10,12}},{"Swampy",{17,21}},{"Swampy Sprinter",{17,21}},{"Tyrant Sneekie",{16,27}}};
  auto band=bands.find(mob.name);int low=2,high=std::max(2,std::min(100,xp/3));if(band!=bands.end()){low=band->second.first;high=band->second.second;}auto carats=low+static_cast<int>(get32(random,0)%(high-low+1));
  auto questItems=quests_->route_kill_items(quests_->drops(mob.npc_id,timestamp),kill);
  const auto dropBonus=(!mob.loot_owner||mob.loot_owner==eid())?
      Consumables::bonus(character_,"monster_drops",timestamp):0.;
  loot_->record(kill,mob,eid(),scope,carats,timestamp,questItems.ground,dropBonus);
  quests_->kill(mob.npc_id);
  recalculate();if(newLevel>level){character_["hp"]=character_.at("maxhp");character_["mp"]=character_.at("maxmp");}
  dungeons_->kill(dungeon_group_,mob,timestamp);
  persist();tx.commit();return questItems;
 }catch(...){character_=std::move(original);stats_=std::move(originalStats);mob.respawn_at=originalRespawn;throw;}
}
}
