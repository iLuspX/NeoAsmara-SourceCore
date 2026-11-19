#pragma once
#include "database.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace karos {

struct NearbyMob {
    std::int64_t spawn_id{};
    int npc_id{};
    std::string name;
    int level{};
    int remaining_hp{};
    int max_hp{};
    std::int64_t seconds_to_respawn{};
    double distance{};
};

struct AttackResult {
    std::string monster_name;
    int damage{};
    int remaining_hp{};
    bool killed{};
    std::int64_t gained_exp{};
    int level_after{};
    std::int64_t respawn_at{};
    std::vector<std::int64_t> ground_item_ids;
};

struct NearbyNpc {
    std::int64_t npc_id{};
    std::string name;
    std::string type;
    double distance{};
};

struct ShopOffer {
    int item_id{};
    std::string name;
    std::int64_t price{};
    int stock{};
};

struct PortalOffer {
    std::int64_t portal_id{};
    std::string target_map;
    int min_level{};
    double distance{};
};

class Gameplay {
public:
    explicit Gameplay(Database& db);
    std::vector<NearbyMob> nearby(std::int64_t character_eid, double radius = 100.0,
                                  int limit = 10, std::int64_t now = 0);
    AttackResult attack(std::int64_t character_eid, std::int64_t spawn_id,
                        std::int64_t now = 0);
    void pickup(std::int64_t character_eid, std::int64_t ground_item_id);
    std::vector<NearbyNpc> nearby_npcs(std::int64_t character_eid, double radius = 100.0);
    std::vector<ShopOffer> shop(std::int64_t character_eid, std::int64_t npc_id);
    void buy(std::int64_t character_eid, std::int64_t npc_id, int item_id, int quantity);
    void sell(std::int64_t character_eid, std::int64_t npc_id, int item_id, int quantity);
    std::vector<PortalOffer> nearby_portals(std::int64_t character_eid, double radius = 100.0);
    void travel(std::int64_t character_eid, std::int64_t portal_id);
private:
    Database& db_;
};

} // namespace karos
