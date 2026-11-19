#pragma once
#include "core.hpp"
namespace rosh {
Json skill_level_with_stone(const Json& catalog,const Json& character,std::uint32_t skill,int rank);
Json skill_level_with_stone(const Json& skills,const Json& stones,const Json& character,std::uint32_t skill,int rank);
class SkillBook {
 Json catalog_,trees_,attributes_,initial_,super_tree_,super_stones_;
 public:
 explicit SkillBook(Content&content);
 bool allowed(int profession,std::uint32_t skill)const;
 bool cast_allowed(int profession,std::uint32_t skill)const;
 bool is_super(int profession,std::uint32_t skill)const;
 bool super_allowed(const Json&character,std::uint32_t skill)const;
 Bytes super_list(const Json&character)const;
 Bytes invest_super(Json&character,const Bytes&request)const;
 Json change_super_stone(Json&character,const Bytes&request,bool remove)const;
 Bytes change_super_choice(Json&character,const Bytes&request)const;
 bool initialize(Json&character)const;
 Json fletta(const Json&character)const;
 Bytes list(const Json&character,const Bytes&carrier)const;
 Bytes learn(Json&character,const Bytes&request,bool upgrade)const;
 Bytes allocate(Json&character,const Bytes&request)const;
 std::vector<Bytes> allocations(const Json&character)const;
};
}
