#pragma once
#include "entities.hpp"
namespace rosh {
class DungeonStore {
    Sqlite& db_;
    Json rules_;
    void emit(const std::string& run, const std::string& key, std::uint32_t map, const Json& events);
public:
    DungeonStore(Sqlite& db, Content& content);
    Json rule(std::uint32_t map) const;
    Json resume(std::uint32_t eid, std::uint32_t map, int channel, double stamp);
    Json join(std::uint32_t eid, const std::string& owner, std::uint32_t map, int channel,
              const Json& entrance, double stamp);
    void leave(const std::string& run, std::uint32_t eid, double stamp);
    void kill(const std::string& run, const Entity& mob, double stamp);
    void trigger(const std::string& run, std::uint32_t eid, std::uint32_t map,
                 std::uint32_t trigger, const Position& position, double stamp);
    Json events(const std::string& run, std::uint32_t eid, std::int64_t after, double stamp);
    Json entrance(const std::string& run, std::uint32_t eid);
    Json last_membership(std::uint32_t eid,std::uint32_t map);
    std::set<std::uint32_t> active_references(const std::string& run);
    std::set<std::uint32_t> inactive_references(const std::string& run);
};
}
