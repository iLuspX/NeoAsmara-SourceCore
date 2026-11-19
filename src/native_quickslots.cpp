#include "native/quickslots.hpp"
#include <set>
namespace rosh {
static Json empty_quickslot(){return Json::array({0,0xffffffffu,0xffffffffu});}
Json QuickSlots::validate(const Json&row){
 if(!row.is_array()||row.size()!=3)throw std::runtime_error("Invalid quickslot record");for(auto&value:row)if(!value.is_number_integer()||value.get<std::int64_t>()<0||value.get<std::uint64_t>()>0xffffffffULL)throw std::runtime_error("Invalid quickslot value");
 if(row[0]==0)return empty_quickslot();static const std::set<std::uint32_t>types={1,2,3,4,9,10,11,49};if(!types.count(row[1].get<std::uint32_t>())||row[2].get<std::uint32_t>()>0x7fffffff)throw std::runtime_error("Unsupported native quickslot type/index");return row;
}
bool QuickSlots::initialize(Json&ch,const Bytes&carrier){
 if(ch.contains("quickslots")){if(!ch.at("quickslots").is_array()||ch.at("quickslots").size()!=count)throw std::runtime_error("Saved quickslot bar is invalid; not overwritten");for(auto&row:ch.at("quickslots"))validate(row);return false;}
 if(carrier.size()<entry_offset+count*12)throw std::runtime_error("Native entry lacks quickslot storage");auto known=ch.value("skills",Json::object());Json slots=Json::array();for(std::size_t i=0;i<count;i++){auto off=entry_offset+i*12;Json row=Json::array({get32(carrier,off),get32(carrier,off+4),get32(carrier,off+8)});auto id=row[0].get<std::uint32_t>(),type=row[1].get<std::uint32_t>();if(type>=1&&type<=4)row=empty_quickslot();else if(type==9&&id>=1000&&id<2000000000&&!known.contains(std::to_string(id)))row=empty_quickslot();slots.push_back(validate(row));}ch["quickslots"]=std::move(slots);return true;
}
Bytes QuickSlots::change(Json&ch,unsigned char sub,const Bytes&body){
 auto expected=sub==11?20u:sub==12?8u:sub==82?436u:0u;if(!expected||body.size()!=expected||get32(body,0,true)!=ch.at("eid"))throw std::runtime_error("Invalid quickslot packet identity/length");if(!ch.contains("quickslots")||ch.at("quickslots").size()!=count)throw std::runtime_error("Quickslot state not initialized");auto slots=ch.at("quickslots");
 if(sub==82){for(std::size_t i=0;i<count;i++)slots[i]=validate(Json::array({get32(body,4+i*12),get32(body,8+i*12),get32(body,12+i*12)}));}
 else{auto slot=get32(body,4,true);if(slot>=count)throw std::runtime_error("Quickslot fora do limite");slots[slot]=sub==12?empty_quickslot():validate(Json::array({get32(body,8),get32(body,12),get32(body,16)}));}
 ch["quickslots"]=std::move(slots);return body;
}
void QuickSlots::patch_entry(const Json&ch,Bytes&body){if(body.size()<entry_offset+count*12)throw std::runtime_error("Invalid native entry quickslot capacity");auto&slots=ch.at("quickslots");if(slots.size()!=count)throw std::runtime_error("Invalid saved quickslot count");for(std::size_t i=0;i<count;i++){auto row=validate(slots[i]);for(std::size_t column=0;column<3;column++)put32(body,entry_offset+i*12+column*4,row[column]);}}
}
