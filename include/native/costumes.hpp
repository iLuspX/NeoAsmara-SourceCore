#pragma once
#include "inventory.hpp"
namespace rosh {
class CashChoices {
 Sqlite&db_;Inventory&inventory_;Json policy_,rules_;
 ItemInstance instance(std::uint32_t eid,std::uint32_t uid);
 int part(int profession,const ItemInstance&item,bool box)const;
 Json attributes(int profession,int part,int option)const;
 std::optional<std::uint32_t> catalog_target(const ItemInstance&item);
 public:
 CashChoices(Sqlite&db,Content&content,Inventory&inventory);
 Json preview(std::uint32_t eid,std::uint32_t uid,int profession);
 std::vector<std::uint32_t> open(std::uint32_t eid,int slot,std::uint32_t item,int profession);
 Json choose(std::uint32_t eid,std::uint32_t uid,int profession,int option);
 std::uint32_t grant_catalog_box(std::uint32_t eid,std::uint32_t target,const std::string&event);
};
}
