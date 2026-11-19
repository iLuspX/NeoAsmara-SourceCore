#pragma once
#include "entities.hpp"
#include "economy.hpp"
#include "combat.hpp"
namespace rosh {
using SceneEntities = std::map<std::uint32_t,Entity>;
std::string native_actor_name(const Bytes&appearance);
// Authoritative map state per WorldServer; immutable snapshots let session
// readers inspect NPCs without racing the shared simulation.
class WorldScene {
 EntityCatalog catalog_;
 std::mutex mutex_;
 std::map<std::string,std::shared_ptr<const SceneEntities>> maps_;
 std::map<std::string,std::unique_ptr<MobSimulation>> simulations_;
 std::map<std::string,double> next_simulation_;
 Bytes damage_carrier_,hp_carrier_,overlay_carrier_;
 std::mt19937_64 combat_random_{std::random_device{}()};
 struct PlayerView {
  std::uint32_t eid{};std::string map;Position position;Bytes appearance,movement;bool hidden{};
  std::function<void(unsigned char,unsigned char,const Bytes&)> send;
  std::map<std::uint32_t,std::pair<Bytes,Position>> observed;
  Json combat=Json::object();int hp{},pending_damage{},pending_healing{},pending_mana{};
  std::map<std::uint32_t,std::pair<std::uint64_t,std::uint32_t>>reflected;
  double last_hit{-1e30};
  std::uint32_t last_damage_target{};
 };
 std::map<std::string,PlayerView> players_;
 struct HealingRadar{std::string map;Position position;double expires,next,health_percent,mana;};
 std::map<std::string,HealingRadar> healing_radars_;
 struct DefenseRadar{std::string map;Position position;Json aura;};
 std::map<std::pair<std::string,int>,DefenseRadar> defense_radars_;
 void tick_radars(double timestamp);
 static bool healing_reachable(const PlayerView& source,const PlayerView& target);
 bool same_party(std::uint32_t source,std::uint32_t target)const;
 static void queue_healing(PlayerView& recipient,double flat,double percent,double multiplier);
 std::map<std::uint32_t,std::string> party_members_;
 std::map<std::string,std::string> party_labels_;
 struct Invitation{std::string group;double expires;};
 std::map<std::uint32_t,Invitation> invitations_;
 std::uint32_t strike_impl(const std::string&key,unsigned char sub,const Bytes&body,double timestamp,const std::function<void(Entity&)>&commit_death,const Json&skill,const std::optional<std::pair<std::uint64_t,std::uint32_t>>&reflected);
 public:
 explicit WorldScene(Content &content);
 std::shared_ptr<const SceneEntities> map(const std::string &name);
 Bytes appearance(const Entity &entity) const {return catalog_.appearance(entity);}
 Json online(bool hidden=false);
 void reset_observer(const std::string&key);
 void activate_dungeon_actors(const std::string&map,const std::set<std::uint32_t>& references);
 void apply_dungeon_actors(const std::string&map,const std::set<std::uint32_t>& active,const std::set<std::uint32_t>& inactive);
 int respawn(const std::string&map,Sqlite&db,const std::string&scope,double timestamp);
 void summon(const std::string&map,std::uint32_t type,int count,const Position&position);
 int recover_npcs(const std::string&map);
 std::vector<std::string> party(const std::string&key,const std::vector<std::string>&args);
 void leave_party(std::uint32_t eid);
 Json party_auras(const std::string& key,double timestamp);
 std::string instance_group(std::uint32_t eid);
 std::vector<std::string> damage_ranking(const std::string&key,const std::string&kind);
 void update_player(const std::string&key,std::uint32_t eid,const std::string&map,const Json&position,
                    const Bytes&appearance,const Bytes&movement,bool hidden,
                    const std::function<void(unsigned char,unsigned char,const Bytes&)>&send,const Json&combat=Json::object());
 void simulate(double timestamp);
 int take_damage(const std::string&key);
 bool can_heal_player(const std::string& key,std::uint32_t target,bool party_only=false);
 void heal_player(const std::string& key,std::uint32_t target,double flat,double percent,double multiplier,bool party_only=false);
 void heal_party(const std::string& key,double flat,double percent,double multiplier);
 std::pair<int,int> take_recovery(const std::string& key);
 void restore_recovery(const std::string& key,int hp,int mp);
 void place_defense_radar(const std::string& key,int aura_skill,double stamp,double duration,Json effects);
 void place_healing_radar(const std::string& key,double stamp,double duration,double health_percent,double mana);
 int resolve_reflections(const std::string&key,double timestamp,const std::function<void(Entity&)>&commit_death);
 int depart(const std::string&key);
 void restore(const std::string&map,Sqlite&db,const std::string&scope,double timestamp);
 std::uint32_t strike(const std::string&key,unsigned char sub,const Bytes&body,double timestamp,
             const std::function<void(Entity&)>&commit_death,const Json&skill=Json::object());
 void skill_effects(const std::string&key,std::uint32_t target,const Json&profile,double timestamp);
 void remove_player(const std::string&key);
 void chat(const std::string&key,const std::string&name,const std::string&message,std::uint32_t channel);
 void action(const std::string&key,unsigned char cat,unsigned char sub,const Bytes&body);
};
std::string character_role(Sqlite &roles,const std::string &account,std::uint32_t eid);
std::string displayed_name(const std::string &name,const std::string &role,std::size_t limit=35);
void scene_visibility(WorldScene &scene,const SceneEntities &entities,const Json &position,
 std::set<std::uint32_t>&visible,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
class NpcShops {
 Sqlite &db_;
 Content &content_;
 std::unique_ptr<Sqlite> typed_;
 Json rules_;
 Inventory &inventory_;
 Wallet wallet_;
 public:
 NpcShops(Sqlite &db,Content &content,Inventory &inventory);
 Bytes transact(const SceneEntities &entities,const Json &character,int sub,const Bytes &body,const std::string &event);
};
}
