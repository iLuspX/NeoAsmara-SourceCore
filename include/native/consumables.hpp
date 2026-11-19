#pragma once
#include "inventory.hpp"
namespace rosh {
class Consumables {
 Sqlite& db_; Inventory& inventory_; Json effects_,skills_,initial_,super_tree_;
 public:
 Consumables(Sqlite&,Inventory&,Content&);
 bool supports(std::uint32_t id)const;
 bool use(Json& character,int slot,std::uint32_t id,const std::string& event,double stamp,double healing=0,std::uint32_t map_id=0);
 static bool tick(Json& character,double stamp);
 static double bonus(const Json& character,const std::string& resource,double stamp);
};
}
