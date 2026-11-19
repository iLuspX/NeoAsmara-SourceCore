#include "native/combat.hpp"
#include <algorithm>
#include <cmath>
namespace rosh {
std::pair<bool,bool> skill_damage_channels(const Json& skill,int profession){
 const bool physical=skill.value("physical_min",0.)>0||skill.value("physical_max",0.)>0;
 const bool magic=skill.value("magic_min",0.)>0||skill.value("magic_max",0.)>0;
 if(physical||magic)return {physical,magic};
 const int family=profession/100;
 const bool caster=family==4||family==6;
 return {!caster,caster};
}
double physical_hit_chance(double accuracy,double evasion){
 if(!std::isfinite(accuracy)||!std::isfinite(evasion))throw std::runtime_error("Invalid hit/evasion rating");
 return std::clamp((100+std::max(0.,accuracy)-std::max(0.,evasion))/100,.05,1.);
}
std::pair<std::uint32_t,std::uint32_t> damage_range(double attack,int level,int attacker_level,double defense){
 if(!std::isfinite(attack)||attack<0||!std::isfinite(defense))throw std::runtime_error("Invalid attack power");
 if(defense<0)defense=3+1.6*level;
 auto base=attack*100/(100+std::max(0.,defense))*std::clamp(1+.025*(attacker_level-level),.65,1.35);
 auto rounded=[](double value){return static_cast<std::uint32_t>(std::clamp(std::nearbyint(value),1.,4294967295.));};
 return {rounded(base*.85),rounded(base*1.15)};
}
double attack_rate(double speed,double multiplier){
 if(!std::isfinite(speed)||!std::isfinite(multiplier)||multiplier<1||multiplier>5)throw std::runtime_error("Use //attackspeed <1..5>");
 return std::clamp(std::max(.1,speed)*multiplier,.1,50.);
}
double attack_interval(double speed,double multiplier){
 // Native character parameters carry a rate. Only the server hit timer uses seconds.
 return 1./attack_rate(speed,multiplier);
}
std::uint32_t attack_target(const Bytes&body,unsigned char sub,std::uint32_t eid){
 auto offset=sub==1?28u:27u;
 if(sub>1||body.size()<offset+4||body.size()>512||body[0]!=0||get32(body,1,true)!=eid)throw std::runtime_error("Invalid native attack identity");
 if(sub==0&&(body.size()!=49||body[47]!=0||body[48]!=36))throw std::runtime_error("Invalid native attack animation");
 return get32(body,offset,true);
}
Bytes combat_damage(const Bytes&carrier,std::uint32_t attacker,std::uint32_t target,std::uint32_t damage,const Position&pos,bool mob,std::uint32_t token,bool alive,int type){
 if(carrier.size()!=45)throw std::runtime_error("Native combat carrier must have45bytes");
 auto body=carrier;body[0]=mob?1:0;put32(body,1,attacker,true);body[5]=0;put32(body,6,token,true);body[10]=1;put32(body,11,0xffffffff,true);body[15]=mob?0:1;put32(body,16,target);body[20]=type;put32(body,21,damage);body[25]=!mob&&alive&&(type==1||type==2)?type:0;putfloat(body,29,pos.x);putfloat(body,33,pos.y);putfloat(body,37,pos.z);return body;
}
Bytes combat_hp(const Bytes&carrier,std::uint32_t eid,std::uint32_t hp){if(carrier.size()<13)throw std::runtime_error("Invalid native HP carrier");auto body=carrier;put32(body,1,eid,true);put32(body,9,hp,true);return body;}
Bytes combat_start(std::uint32_t attacker,std::uint32_t target,std::uint32_t token,const Position&from,const Position&to,double angle){
 Bytes body(49);body[0]=1;put32(body,1,attacker,true);put32(body,6,token,true);putfloat(body,10,from.x);putfloat(body,14,from.y);putfloat(body,18,from.z);putfloat(body,22,angle);put32(body,27,target,true);putfloat(body,31,to.x);putfloat(body,35,to.y);putfloat(body,39,to.z);putfloat(body,43,from.distance(to));body[48]=36;return body;
}
}
