#include "native/game.hpp"
#include "native/runtime.hpp"
#include <iostream>
using namespace rosh;
static void check(bool ok,const char*message){if(!ok)throw std::runtime_error(message);}
int wmain(int argc,wchar_t**argv){try{
 check(argc==2,"Use isolated fixture");fs::path root=argv[1];check(fs::exists(root/"GM_TEST_FIXTURE"),"Refusing live data");Content content(root);LegacyStore store(root/"Database/karos_gameplay.sqlite");Inventory bag(store.db(),content);
 for(int family:{200,600,400}){
  auto eid=std::uint32_t(998000+family);auto account="enchant-"+token(8);auto weapon=family==200?5105u:family==600?16001u:15001u;
  Json ch={{"eid",eid},{"name","EnchantFixture"+std::to_string(family)},{"cls",family},{"level",120},{"hp",100},{"mp",100},{"skill_points",0},{"fletta",0},{"fletta_energy",0},{"moral_value",0}};store.db().exec("INSERT INTO accounts VALUES(?,?)",{account,Json{{"chars",Json::array({ch})}}.dump()});
  bag.grant_once(eid,{{weapon,1},{60901,4}},account+":seed");ItemInstance item,scroll;for(auto&i:bag.items(eid)){if(i.item==weapon)item=i;else if(i.item==60901)scroll=i;}
  auto address=Inventory::address(scroll.slot);Session session;session.identity.id=account;session.id=token(20);session.channel=1;session.expires=now()+600;
  WorldSession game(content,store,session,root,eid);Bytes lastReply;int clears=0;auto send=[&](unsigned char c,unsigned char s,const Bytes&b){if(c==6&&s==23)lastReply=b;if(c==6&&s==6&&get32(b,7)==weapon&&get32(b,21)==0)++clears;};
  Bytes ready(21);put32(ready,1,eid,true);putfloat(ready,5,9020);putfloat(ready,9,6590);putfloat(ready,13,627);game.handle(4,0,ready,send);
  auto target=Inventory::address(item.slot);Bytes request(43);put32(request,0,eid,true);put32(request,4,address.first,true);put32(request,8,address.second,true);put32(request,12,60901,true);put32(request,16,target.first,true);put32(request,20,target.second,true);put32(request,24,weapon,true);put16(request,28,0x74,true);for(int i=30;i<42;i+=4)put32(request,i,0xffffffffu,true);request[42]=2;
  for(int grade=1;grade<=3;grade++){lastReply.clear();game.handle(6,23,request,send,account+":"+std::to_string(grade));check(lastReply==request,"Captured enchant format not acknowledged");check(store.db().query("SELECT grade FROM item_enchantments WHERE uid=?",{std::int64_t(item.uid)})[0].at("grade")==grade,"Native enchant did not increase grade");}
  check(clears>=3,"Changed equipment sent without native clear/replace cache update");game.handle(6,23,request,send,account+":3");check(bag.owned(eid,scroll.slot,60901,now()).quantity==1,"Repeated enhancement consumed scroll twice");check(bag.owned(eid,item.slot,weapon,now()).uid==item.uid,"Enhancement changed item identity");
  auto attrs=content.get("data/item_attrs.json").at(std::to_string(weapon));auto low=enchant_row(attrs,0),high=enchant_row(attrs,3);
  auto gearBefore=game.character().at("equipment_slots");Bytes visibility(10);put32(visibility,0,eid,true);put32(visibility,4,eid,true);visibility[8]=0;visibility[9]=1;game.handle(6,0x46,visibility,send,account+":cash-hide");check(game.character().at("cash_visibility")==Json::array({0,1})&&store.character(account,eid).at("cash_visibility")==Json::array({0,1}),"Cash appearance setting rejected or not persisted");check(game.character().at("equipment_slots")==gearBefore,"Cash visibility changed authoritative equipment");
  if(family==200)check(high.at("vals")[0]>low.at("vals")[0],"Physical weapon lacks native physical progression");else check(high.at("magic_attack")>low.at("magic_attack"),"Magic weapon lacks native magic progression");
  bag.grant_once(eid,{{48017,1}},account+":rental");ItemInstance rental;for(auto&i:bag.items(eid))if(i.item==48017)rental=i;check(rental.expires&&*rental.expires>now(),"New native rental has no fresh duration");auto expiry=rental.expires;bag.equip(eid,rental.slot,rental.item,family,120,0,now());for(auto&i:bag.items(eid,true))if(i.uid==rental.uid)check(i.expires==expiry,"Equipping rental renewed its deadline");
 }
 if(content.has("data/native_affix_visual_ids.json")){
  auto registry=content.get("data/native_affix_visual_ids.json").at("entries");
  store.db().exec("CREATE TABLE IF NOT EXISTS item_custom_affixes(uid INTEGER NOT NULL,layer TEXT NOT NULL,position INTEGER NOT NULL,stat TEXT NOT NULL,value REAL NOT NULL,PRIMARY KEY(uid,layer,position))");
  ItemInstance visual;visual.item=5105;visual.uid=1998000;visual.quantity=1;visual.slot=1;
  for(auto&entry:registry){
   store.db().exec("DELETE FROM item_custom_affixes WHERE uid=?",{std::int64_t(visual.uid)});int position=0;
   for(auto&stat:entry.at("stats"))store.db().exec("INSERT INTO item_custom_affixes VALUES(?,?,?,?,?)",{std::int64_t(visual.uid),std::string("common"),position++,stat[0].get<std::string>(),stat[1].get<double>()});
   auto packet=bag.update(998200,visual);check(get16(packet,130)==entry.at("id").get<unsigned>(),"Native tooltip option does not match exact saved affix values");check(get16(packet,140)==0,"Common tooltip option leaked into additional layer");
  }
  store.db().exec("DELETE FROM item_custom_affixes WHERE uid=?",{std::int64_t(visual.uid)});
 }
 NativeService main(root,"MainServer");auto before=store.db().query("SELECT id,payload FROM accounts WHERE payload LIKE '%EnchantFixture200%'");main.operator_console("setadmin EnchantFixture200");Sqlite roles(root/"Database/karos_roles.sqlite");check(roles.query("SELECT role FROM staff_roles WHERE character_name='EnchantFixture200'")[0].at("role")=="Admin","Local console did not set Admin");check(before==store.db().query("SELECT id,payload FROM accounts WHERE payload LIKE '%EnchantFixture200%'"),"Operator role assignment changed character/account data");
 bool refused=false;try{main.operator_console("setadmin NotARealCharacter");}catch(...){refused=true;}check(refused,"Console accepted missing character");
 std::cout<<"PASS: captured 43-byte weapon enhancement; physical and both magical families; native grades/chances; consume/replay/UID/cache refresh; fresh rental and unchanged transfer expiry; local MainServer Admin console.\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
