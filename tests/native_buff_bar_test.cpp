#include "native/game.hpp"
#include <cmath>
#include <iostream>
using namespace rosh;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int wmain(int argc,wchar_t**argv){try{
 require(argc==2,"Expected captured buff list fixture");
 const auto fixture=Json::parse(read_file(argv[1]));
 const auto captured=unhex(fixture.at("body"));
 const auto count=get16(captured,6,true);
 require(captured.size()==8+18u*count&&captured[5]==1,"Unexpected retail buff list");
 Json entries=Json::object();
 for(unsigned i=0;i<count;++i){
  auto offset=8+i*18;
  require(captured[offset+5]==0&&get32(captured,offset+14,true)==0,"Fixture has unreviewed effect flags");
  entries[std::to_string(get32(captured,offset,true))]={{"rank",captured[offset+4]},
   {"item",get32(captured,offset+6,true)},{"expires",1000.+get32(captured,offset+10,true)}};
 }
 const auto rebuilt=native_buff_packet(get32(captured,1,true),entries,true,1000.);
 // JSON sorts numeric-looking keys lexically; compare each exact native record.
 for(unsigned i=0;i<count;++i){
  auto offset=8+i*18;bool found=false;
  for(unsigned j=0;j<count;++j)if(std::equal(captured.begin()+offset,captured.begin()+offset+18,rebuilt.begin()+8+j*18))found=true;
  require(found,"Native buff record differs from captured bytes");
 }
 const auto removed=native_buff_packet(62001,entries,false,1000.);
 require(removed[5]==0&&get16(removed,6,true)==count,"Expired buffs were not removed by ID");
 Json character={{"active_buffs",{{"300501",{{"rank",5},{"expires",1004.}}}}},
  {"consumable_effects",{{"first",{{"item_id",100140004},{"expires",8200.}}},
                         {"second",{{"item_id",200140004},{"expires",8300.}}},
                         {"expired",{{"item_id",54217},{"expires",999.}}}}}};
 Json links={{"100140004",{{"skill_id",16000003}}},{"200140004",{{"skill_id",16000003}}}};
 Json borrowed={{"aura",{{"skill",390801},{"rank",3},{"expires",1020.}}}};
 auto state=native_buff_state(character,borrowed,links,1000.);
 require(state.size()==3&&state.at("16000003").at("item")==200140004,"Potion source mapping or shared-skill deduplication failed");
 require(state.at("390801").at("rank")==3,"Party aura lost its learned rank");
 auto later=native_buff_state(character,borrowed,links,1010.);
 require(!later.contains("300501")&&later.at("16000003").at("expires")==8300.,"Buff expiration extended the original potion deadline");
 auto packet=native_buff_packet(62001,later,true,1010.);
 require(get32(packet,18,true)==7290,"Restored potion duration is not the remaining seconds");
 bool rejected=false;
 try{native_buff_packet(62001,Json{{"4294967296",{{"rank",1},{"item",0},{"expires",1100.}}}},true,1000.);}catch(const std::exception&){rejected=true;}
 require(rejected,"Out-of-range buff ID was accepted");
 std::cout<<"PASS: captured native buff records; add/remove deltas; potion icons and source IDs; original remaining duration; party aura rank; expired effects and ID bounds\n";
 return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}}
