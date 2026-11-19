#pragma once
#include "scene.hpp"
#include "skills.hpp"
namespace rosh {
class ActiveSkills {
 Json catalog_,stones_,encounters_,pending_=Json::object();
 public:
 explicit ActiveSkills(Content&content);
 Json effects(std::uint32_t id,int rank)const;
 Json admit(Json&character,const Json&stats,const SceneEntities&entities,const SkillBook&learned,const Bytes&body,double timestamp,bool pk_mode=false,bool friendly_target=false);
 Json impact(const Bytes&body,std::uint32_t eid,double timestamp);
 static bool targeted_healing(std::uint32_t id);
 static bool sacrifice_healing(std::uint32_t id);
 static bool healing_radar(std::uint32_t id);
 Json state()const{return pending_;}
 void restore(const Json&state){pending_=state;}
 void cancel(){pending_=Json::object();}
};
}
