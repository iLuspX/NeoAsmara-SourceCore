#pragma once
#include "entities.hpp"
namespace rosh {
std::pair<bool,bool> skill_damage_channels(const Json& skill,int profession);
std::pair<std::uint32_t,std::uint32_t> damage_range(double attack,int target_level,int attacker_level,double defense=-1);
double attack_rate(double speed,double multiplier=1);
double attack_interval(double attack_rate,double multiplier=1);
double physical_hit_chance(double accuracy,double evasion);
std::uint32_t attack_target(const Bytes&body,unsigned char sub,std::uint32_t actor);
Bytes combat_damage(const Bytes&carrier,std::uint32_t attacker,std::uint32_t target,std::uint32_t damage,
                    const Position&position,bool mob_attacker,std::uint32_t token,bool alive,int type=1);
Bytes combat_hp(const Bytes&carrier,std::uint32_t eid,std::uint32_t hp);
Bytes combat_start(std::uint32_t attacker,std::uint32_t target,std::uint32_t token,
                   const Position&from,const Position&to,double angle=0);
}
