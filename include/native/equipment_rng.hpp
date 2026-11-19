#pragma once
#include "core.hpp"
#include <random>
namespace rosh {
// Native metadata selects eligible items and materials. Roll odds and per-line
// limits are an explicit local policy, not recovered retail server formulas.
class EquipmentRng {
    Json policy_;
  public:
    explicit EquipmentRng(Json policy):policy_(std::move(policy)){}
    const Json& policy()const{return policy_;}
    int maximum(std::uint32_t item,const std::string&layer)const;
    Json roll(std::uint32_t item,const std::string&layer,std::uint64_t seed,const Json&reserved=Json::array())const;
    std::pair<std::uint32_t,int> material(std::uint32_t item,int grade)const;
    static void initialize(Sqlite&db);
    void initial(Sqlite&db,std::uint32_t uid,std::uint32_t item)const;
};
}
