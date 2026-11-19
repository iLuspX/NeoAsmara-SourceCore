#pragma once
#include "scene.hpp"
namespace rosh {
class GroundLoot {
 Sqlite&db_;Inventory&inventory_;Json policy_,distribution_,catalog_,gear_,super_;
 std::mt19937_64 random_{std::random_device{}()};
 std::set<std::uint32_t> quest_only_;
 std::int64_t integer(std::int64_t low,std::int64_t high);
 std::vector<std::pair<std::uint32_t,std::int64_t>> distributed(const Entity&mob,const std::string&map,const std::set<std::uint32_t>&blocked,double drop_multiplier);
 public:
 GroundLoot(Sqlite&db,Content&content,Inventory&inventory);
 void record(const std::string&kill,const Entity&mob,std::uint32_t owner,const std::string&scope,std::int64_t carats,double timestamp,const std::vector<std::pair<std::uint32_t,std::int64_t>>&quest_drops={},double drop_bonus=0);
 Json pickup(std::uint32_t eid,const std::string&scope,const Json&position,const Bytes&request,double timestamp);
 std::map<std::uint32_t,Json> visible(const std::string&scope,const Json&position,double timestamp);
 static Bytes spawn(std::uint32_t eid,const Json&row);
 static Bytes remove(std::uint32_t actor,std::uint32_t item);
};
}
