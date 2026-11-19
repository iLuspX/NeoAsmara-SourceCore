#include "native/skills.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
using namespace rosh;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>static void unchanged(Json& ch,F&& action,const char* message){auto before=ch;bool failed=false;try{action();}catch(const std::exception&){failed=true;}check(failed&&ch==before,message);}
static Bytes invest(std::uint32_t eid,std::uint32_t skill,std::uint32_t points){Bytes body(16);put32(body,0,eid,true);put32(body,4,skill,true);put32(body,8,points,true);body[12]=0x20;put16(body,13,0x2a6,true);return body;}
int wmain(int argc,wchar_t**argv){try{
 check(argc==3,"Expected isolated encrypted content and captured super-skill fixture");Content content(argv[1]);SkillBook book(content);
 const auto catalog=content.get("data/skill_catalog.json");const auto& tree=catalog.at("super_tree");check(tree.size()==84,"Native tree incomplete");
 const auto capture=Json::parse(read_file(argv[2]));const auto initial=unhex(capture.at("initial_list").at("body"));
 Json ch={{"eid",get32(initial,0,true)},{"cls",211},{"profession_grade",2},{"level",115},{"skill_points",987},{"fletta",10000},{"skills",Json::object()},{"super_skill_progress",Json::object()}};
 for(int index=0;index<get16(initial,4,true);++index){auto at=6+13*index;auto id=get32(initial,at);if(!id)continue;auto key=std::to_string(id);ch["skills"][key]=initial[at+4];ch["super_skill_progress"][key]={{"option",get32(initial,at+5)},{"points",get32(initial,at+9)}};}
 check(book.super_list(ch)==initial,"Initial authoritative list differs from captured wire");
 for(const auto& step:capture.at("investments")){
  const auto request=unhex(step.at("request"));check(book.invest_super(ch,request)==unhex(step.at("reply")),"Native investment reply differs from capture");
  check(book.super_list(ch)==unhex(step.at("list")),"Rank/progress carry differs from captured list");
 }
 check(ch.at("fletta")==100&&ch.at("skill_points")==987,"Super skills spent ordinary points or wrong Fletta amount");
 int tested=0;
 for(const auto& [key,node]:tree.items()){
  const auto id=static_cast<std::uint32_t>(std::stoul(key));const int family=node.at("family");
  Json actor={{"eid",90001},{"cls",family+11},{"profession_grade",2},{"level",115},{"skill_points",543},{"fletta",100000},{"skills",Json::object()}};
  check(book.is_super(family,id)&&book.super_allowed(actor,id)&&book.cast_allowed(family+11,id),"Native super skill missing from class permission");
  check(!book.allowed(family+11,id),"Super skill entered ordinary skill-point tree");
  auto base=actor;base["cls"]=family;unchanged(base,[&]{book.invest_super(base,invest(90001,id,1));},"Base class learned before first profession");
  auto wrong=actor;wrong["cls"]=family==100?211:111;unchanged(wrong,[&]{book.invest_super(wrong,invest(90001,id,1));},"Foreign class super skill accepted");
  if(node.at("slot").get<int>()>0){auto early=actor;early["cls"]=family+10;early["profession_grade"]=0;unchanged(early,[&]{book.invest_super(early,invest(90001,id,1));},"Profession grade prerequisite ignored");}
  std::int64_t total=0;
  for(int rank=1;rank<=5;++rank){auto cost=node.at("fletta_costs").at(rank-1).get<std::uint32_t>();total+=cost;auto reply=book.invest_super(actor,invest(90001,id,cost));check(reply[12]==rank&&actor.at("skills").at(key)==rank,"Native rank progression failed");}
  check(actor.at("fletta")==100000-total&&actor.at("skill_points")==543,"Native costs mixed skill points and Fletta");
  unchanged(actor,[&]{book.invest_super(actor,invest(90001,id,1));},"Max-rank investment spent Fletta");++tested;
 }
 Json actor={{"eid",90002},{"cls",211},{"profession_grade",2},{"level",115},{"fletta",100000},{"skills",Json::object()}};
 book.invest_super(actor,invest(90002,4000,1));
 unchanged(actor,[&]{book.invest_super(actor,invest(90002,4001,1));},"Both alternatives in one stage were learned");
 auto no_money=actor;no_money["fletta"]=0;unchanged(no_money,[&]{book.invest_super(no_money,invest(90002,4000,1));},"Missing Fletta mutated progress");
 unchanged(actor,[&]{book.invest_super(actor,invest(90002,4000,0));},"Zero investment accepted");
 unchanged(actor,[&]{book.invest_super(actor,invest(90002,4000,0xffffffff));},"Oversized investment spent or overflowed Fletta");
 auto malformed=invest(90002,4000,1);malformed.resize(15);unchanged(actor,[&]{book.invest_super(actor,malformed);},"Truncated investment mutated character");
 Json fresh={{"eid",90003},{"cls",100},{"skills",Json::object()}};const auto list=book.super_list(fresh);
 check(get32(list,0,true)==90003&&get16(list,4,true)==15&&std::all_of(list.begin()+6,list.end(),[](auto byte){return byte==0;}),"New character inherited reference-player super skills");
 check(catalog.at("super_stones").size()==165,"Playable native stone catalog incomplete");
 Json stone_actor={{"eid",90004},{"cls",211},{"level",115},{"skills",{{"4000",1}}},{"super_skill_progress",{{"4000",{{"points",123},{"option",0}}}}}};
 Bytes apply(16);put32(apply,0,90004,true);put32(apply,4,4000,true);put32(apply,8,600101,true);apply[12]=1;put16(apply,14,0x2a1,true);
 auto base_level=skill_level_with_stone(catalog,stone_actor,4000,1);auto plan=book.change_super_stone(stone_actor,apply,false);
 check(plan.at("fee")==10000&&get16(unhex(plan.at("reply")),14,true)==0x2a1,"Wrong native apply cost/status");
 const auto resolved=skill_level_with_stone(catalog,stone_actor,4000,1);
 check(resolved.at("effects").size()==base_level.at("effects").size()+1&&resolved.at("effects").back().at("flat")==2.,"Passive stone failed to add native Strength +2");
 check(stone_actor.at("super_skill_progress").at("4000").at("points")==123&&stone_actor.at("skills").at("4000")==1,"Applying stone changed rank/Fletta progress");
 unchanged(stone_actor,[&]{book.change_super_stone(stone_actor,apply,false);},"Same stone was consumed twice");
 auto bad=apply;put32(bad,8,631001,true);unchanged(stone_actor,[&]{book.change_super_stone(stone_actor,bad,false);},"Attack stone accepted on passive skill");
 put32(apply,8,600102,true);plan=book.change_super_stone(stone_actor,apply,false);
 check(plan.at("fee")==20000&&get16(unhex(plan.at("reply")),14,true)==0x2a2,"Wrong native replacement cost/status");
 Bytes remove(10);put32(remove,0,90004,true);put32(remove,4,4000,true);put16(remove,8,0x2a9,true);plan=book.change_super_stone(stone_actor,remove,true);
 check(plan.at("fee")==10000&&skill_level_with_stone(catalog,stone_actor,4000,1)==base_level,"Removing stone retained modifiers");
 unchanged(stone_actor,[&]{book.change_super_stone(stone_actor,remove,true);},"Removing an empty stone slot was accepted");
 Bytes swap(12);put32(swap,0,4000,true);put32(swap,4,4001,true);swap[8]=1;
 auto bad_swap=swap;put32(bad_swap,4,4002,true);
 unchanged(stone_actor,[&]{book.change_super_choice(stone_actor,bad_swap);},"Coupon changed a different super-skill stage");
 stone_actor["super_skill_progress"]["4000"]["option"]=600101;
 unchanged(stone_actor,[&]{book.change_super_choice(stone_actor,swap);},"Coupon erased an equipped stone");
 stone_actor["super_skill_progress"]["4000"]["option"]=0;
 stone_actor["fletta"]=321;stone_actor["skill_points"]=1234;
 stone_actor["active_buffs"]={{"4000",{{"rank",1},{"expires",99999.}}}};
 stone_actor["skill_cooldowns"]={{"4000",99999.},{"global",555.}};
 check(book.change_super_choice(stone_actor,swap)==swap,"Native coupon response changed unexpectedly");
 check(stone_actor.at("skills").value("4000",0)==0&&stone_actor.at("skills").at("4001")==1&&
       stone_actor.at("super_skill_progress").at("4001").at("points")==123&&stone_actor.at("fletta")==321&&stone_actor.at("skill_points")==1234,"Coupon did not preserve rank/progress or altered currency");
 check(stone_actor.at("active_buffs").empty()&&stone_actor.at("skill_cooldowns").at("4001")==99999.&&stone_actor.at("skill_cooldowns").at("global")==555.,"Coupon retained old buff or bypassed cooldown");
 for(const auto& [stone,definition]:catalog.at("super_stones").items())for(const auto& id:definition.at("skills")){
  const auto skill=id.get<std::uint32_t>();const auto key=std::to_string(skill);const auto family=tree.at(key).at("family").get<int>();
  Json test={{"eid",90005},{"cls",family+11},{"skills",{{key,1}}},{"super_skill_progress",{{key,{{"points",0},{"option",std::stoul(stone)}}}}}};
  const auto actual=skill_level_with_stone(catalog,test,skill,1);const auto native=catalog.at("skills").at(key).at("levels").at("1");
  check(actual.at("effects").size()==native.at("effects").size()+definition.at("effects").size(),"Stone native effects lost");
  check(actual.at("cooldown").get<double>()==std::max(0.,native.at("cooldown").get<double>()+definition.at("cooldown_add").get<double>()),"Stone cooldown adjustment lost");
  if(std::stoul(stone)==631001)check(std::abs(actual.at("power").get<double>()-native.at("power").get<double>()*1.2)<1e-5,"Attack stone power +20% lost");
 }
 std::cout<<"PASS: "<<tested<<" native super skills across seven classes; all five ranks; exact captured investments; 165 stone compatibility/effect rules; native apply/replace/delete; coupon rank/progress preservation, pair/stone restrictions and cooldown continuity; fresh character isolation\n";return 0;
}catch(const std::exception&error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}}
