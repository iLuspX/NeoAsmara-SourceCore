#pragma once
#include "core.hpp"
namespace rosh {
class QuickSlots {
 static Json validate(const Json&row);
 public:
 static constexpr std::size_t count=36,entry_offset=0x1d1;
 static bool initialize(Json&character,const Bytes&enter_carrier);
 static Bytes change(Json&character,unsigned char sub,const Bytes&body);
 static void patch_entry(const Json&character,Bytes&body);
};
}
