#pragma once
#include "core.hpp"
#include <optional>
#include <random>
#include <set>

namespace rosh {
struct Position {
    double x{}, y{}, z{};
    double distance(const Position &other) const;
};
enum class MobState { Idle, Chase, Attack, Returning, Dead };
struct Entity {
    std::uint32_t eid{}, npc_id{};
    std::size_t spawn_index{};
    std::string name, map, category;
    bool combat{}, alive{true}, guard{};
    int level{1}, hp{1}, maxhp{1};
    Position position, home;
    double angle{}, aggro{}, attack_range{20};
    Json rules=Json::object();
    MobState state{MobState::Idle};
    std::uint32_t target{}, detected_target{};
    double react_at{};
    std::uint32_t attack_sequence{};
    std::uint64_t life_generation{};
    Bytes carrier;
    std::optional<Position> wander;
    double pause_until{}, last_attack{}, impact_at{}, respawn_at{};
    std::map<std::uint32_t, std::uint64_t> damage;
    std::map<std::uint32_t,std::string> damage_names;
    std::map<std::string,std::uint64_t> party_damage;
    std::map<std::string,std::string> party_damage_names;
    std::map<std::string,std::map<std::uint32_t,std::uint64_t>> party_contributors;
    std::uint32_t loot_owner{};
    std::set<std::uint32_t> loot_recipients;
    Json debuffs = Json::object();
    bool stun_stop_sent{}, hp_dirty{}, aggro_notice_sent{};
    bool attackable(bool pk_mode) const { return alive && !rules.value("dungeon_dormant",false) && (guard ? pk_mode : combat); }
};
struct Observer {
    std::uint32_t eid{};
    Position position;
    int hp{}, level{1};
    bool hidden{}, ready{};
    std::int64_t moral{};
};
struct EntityEvent {
    enum Kind { Move, Stop, AttackStart, AttackImpact, Respawn, Reset, Aggro, Disengage, Detection } kind;
    std::uint32_t eid{}, target{};
    Position from;
    int damage{};
    unsigned char move_mode{};
    float move_speed{};
};
class EntityCatalog {
    Content &content_;
    Json spawns_, npc_, observed_, respawns_, spawn_policy_, progression_, hp_overrides_,prototypes_,mob_profiles_;
    std::map<std::uint32_t, Bytes> templates_;
    std::map<std::pair<std::string, std::size_t>, Json> placements_;
    Json checked(const Json &overrides, const Json &base) const;
    void apply_mob_edit(Entity &entity, const Json &edit) const;

  public:
    explicit EntityCatalog(Content &content);
    Json rules(std::uint32_t npc_id, bool combat, const std::string &map) const;
    int health(std::uint32_t npc_id, int level, int observed = 0) const;
    int damage(std::uint32_t npc_id, int level, double roll) const;
    bool event_level_allowed(std::uint32_t npc_id, int mob_level, int player_level) const;
    double simulation_interval() const;
    std::map<std::uint32_t, Entity> load(const std::string &map,
                                         const std::set<std::uint32_t> &reserved = {});
    Bytes appearance(const Entity &entity) const;
    std::uint32_t prototype_reference(const std::string&map,std::uint32_t type) const;
    std::uint32_t prototype_count(const std::string&map) const;
    Entity summoned(std::uint32_t type,std::uint32_t eid,const std::string&map,const Position&position);
    const Json &maps() const {
        return spawns_;
    }
};
class MobSimulation {
    EntityCatalog &catalog_;
    std::mt19937_64 random_;
    double last_{};
    double interval_{.1};
    double uniform(double low, double high);
    std::optional<EntityEvent> step(Entity &entity, const Position &to, double dt, double speed = -1);
    std::optional<EntityEvent> return_home(Entity &entity, double dt);

  public:
    explicit MobSimulation(EntityCatalog &catalog, std::uint64_t seed, double timestamp)
        : catalog_(catalog), random_(seed), last_(timestamp), interval_(catalog.simulation_interval()) {}
    void provoke(Entity &entity, const Observer &observer);
    std::vector<EntityEvent> tick(std::map<std::uint32_t, Entity> &entities,
                                  const std::map<std::uint32_t, Observer> &observers, double timestamp);
};
class SpawnPersistence {
  public:
    static void initialize(Sqlite &db);
    static std::string scope(const std::string &map, int channel, std::uint32_t map_id);
    static void restore(Sqlite &db, const std::string &scope, Entity &entity, double timestamp);
    // Caller owns the reward transaction: a duplicate death cannot pay rewards twice.
    static bool claim(Sqlite &db, const std::string &scope, Entity &entity, const std::string &kill,
                      double timestamp, double jitter_roll);
};
} // namespace rosh
