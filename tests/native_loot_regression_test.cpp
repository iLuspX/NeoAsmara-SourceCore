#include "native/game.hpp"
#include <algorithm>
#include <iostream>
#include <cmath>
using namespace rosh;
static void check(bool ok,const char*message){if(!ok)throw std::runtime_error(message);}
int wmain(int argc,wchar_t**argv){try{
 check(argc==2,"Use isolated fixture");fs::path root=argv[1];check(fs::exists(root/"GM_TEST_FIXTURE"),"Refusing live data");Content content(root);LegacyStore store(root/"Database/karos_gameplay.sqlite");Inventory inventory(store.db(),content);GroundLoot loot(store.db(),content,inventory);EntityCatalog catalog(content);
 auto map=catalog.load("berneo");Entity normal;bool found=false;for(auto&[id,m]:map)if(m.combat&&m.category=="normal"){normal=m;found=true;break;}check(found,"No mob fixture");normal.damage[997001]=1;auto stamp=now();auto scope="loot-regression-"+token(6);Json pos={normal.position.x,normal.position.y,normal.position.z};
 store.db().exec("INSERT OR REPLACE INTO drop_rules(npc_id,item_id,chance_bp,min_qty,max_qty,enabled) VALUES(0,70001,10000,1,1,1)");
 loot.record("quest-inactive-"+token(6),normal,997001,scope,1,stamp);check(store.db().query("SELECT id FROM ground_items WHERE map_name=? AND item_id=70001",{scope}).empty(),"Quest-only item leaked through generic drops");
 loot.record("quest-active-"+token(6),normal,997001,scope,1,stamp,{{70001,1}});auto q=store.db().query("SELECT * FROM ground_items WHERE map_name=? AND item_id=70001",{scope});check(q.size()==1,"Active quest drop missing");Bytes pick(14);put32(pick,0,997002,true);put32(pick,4,q[0].at("id").get<std::uint32_t>(),true);put32(pick,8,70001,true);put16(pick,12,0x005a,true);bool refused=false;try{loot.pickup(997002,scope,pos,pick,stamp+61);}catch(...){refused=true;}check(refused,"Quest drop became free loot for unrelated character");
 store.db().exec("UPDATE sqlite_sequence SET seq=65535 WHERE name='ground_items'");loot.record("ground-reuse-"+token(6),normal,997001,scope,1,stamp);for(auto&[id,row]:loot.visible(scope,pos,stamp))check((id&65535)>0&&(id&65535)<=65535,"Recycled ground actor invalid");check(loot.visible(scope,pos,stamp+121).empty(),"Drops remain forever after TTL");
 {
  auto &db=store.db();Wallet wallet(db,inventory);
  constexpr std::uint32_t owner=997001, actor=65000;
  check(db.query("SELECT id FROM ground_items WHERE id=?",{std::int64_t(actor)}).empty(),"Carat actor fixture already occupied");
  const auto balance=wallet.balance(owner);
  Bytes coins(14);put32(coins,0,owner,true);put32(coins,4,actor,true);put32(coins,8,999999,true);put16(coins,12,0x005a,true);
  for(const auto amount:{17,23}){
   const auto kill="recycled-carats-"+std::to_string(amount);
   db.exec("INSERT INTO mob_kills VALUES(?,?,?,?)",
           {kill,std::int64_t(normal.npc_id),std::int64_t(owner),std::int64_t(stamp)});
   db.exec("INSERT INTO ground_items(id,kill_id,map_name,item_id,quantity,x,y,z,owner_eid,created_at,expires_at) "
           "VALUES(?,?,?,999999,?,?,?,?,?,?,?)",
           {std::int64_t(actor),kill,scope,std::int64_t(amount),
            normal.position.x,normal.position.y,normal.position.z,std::int64_t(owner),stamp,stamp+120});
   loot.pickup(owner,scope,pos,coins,stamp);
   bool rejected=false;
   try{loot.pickup(owner,scope,pos,coins,stamp);}catch(const std::exception&){rejected=true;}
   check(rejected,"Replayed Carat pickup was accepted");
   db.exec("DELETE FROM ground_items WHERE id=?",{std::int64_t(actor)});
  }
  check(wallet.balance(owner)==balance+40,"Recycled Carat actor did not credit both independent piles");
 }
 {
  constexpr std::uint32_t owner=997021,item=54005;
  auto &db=store.db();
  const auto limit=inventory.definition(item).value("stack",1);
  check(limit>2&&inventory.definition(item).value("duration",0)==0,"Missing permanent stack fixture");
  inventory.grant_once(owner,{{item,limit-2}},"ground-partial-stack");
  const auto stack=inventory.items(owner).front();
  std::uint32_t filler=0;
  const auto rules=content.get("data/native_item_rules.json");
  for(auto it=rules.begin();it!=rules.end();++it)if(it.value().value("kind",0)==1&&it.value().value("stack",1)==1){
   filler=static_cast<std::uint32_t>(std::stoul(it.key()));break;
  }
  check(filler!=0,"Missing bag filler");
  const auto free=inventory.available(owner,stamp).size()-1;
  inventory.grant_once(owner,{{filler,static_cast<int>(free)}},"ground-full-bag-stack");
  const auto key="ground-stack-"+token(6);
  loot.record(key,normal,owner,scope,0,stamp,{{item,3}});
  const auto row=db.query("SELECT * FROM ground_items WHERE kill_id=? AND item_id=? AND quantity=3",{key,std::int64_t(item)}).at(0);
  auto appearance=GroundLoot::spawn(owner,row);
  check(row.at("x")==double(getfloat(appearance,167))&&row.at("y")==double(getfloat(appearance,171))&&
        row.at("z")==double(getfloat(appearance,175)),"Pickup location differs from native drop position");
  Bytes pick(14);
  put32(pick,0,owner,true);put32(pick,4,row.at("id").get<std::uint32_t>(),true);
  put32(pick,8,item,true);put16(pick,12,0x005a,true);
  bool refused=false;
  try{loot.pickup(owner,scope,pos,pick,stamp);}catch(const InventoryFull&){refused=true;}
  check(refused&&inventory.owned(owner,stack.slot,item,stamp).quantity==limit-2,"Partial merge was not rolled back");
  db.exec("UPDATE ground_items SET quantity=2 WHERE id=?",{row.at("id").get<std::int64_t>()});
  loot.pickup(owner,scope,pos,pick,stamp);
  const auto merged=inventory.owned(owner,stack.slot,item,stamp);
  check(merged.quantity==limit&&merged.uid==stack.uid,"Full bag did not merge available stack capacity");
 }
 {
  const auto key="ground-spread-"+token(6);
  loot.record(key,normal,997001,scope,0,stamp,{{54005,1},{54005,1},{54005,1},{54005,1}});
  const auto rows=store.db().query("SELECT x,y,z FROM ground_items WHERE kill_id=?",{key});
  check(rows.size()>=4,"Missing multi-drop fixture");
  const auto policy=content.get("server/drop_policy.json");
  const double maximum=std::clamp(policy.value("scatter_radius",45.),0.,80.);
  const double minimum=std::clamp(policy.value("scatter_minimum",10.),0.,maximum);
  constexpr double turn=6.283185307179586;
  std::vector<double> angles;
  for(const auto &row:rows){
   const double dx=row.at("x").get<double>()-normal.position.x,dy=row.at("y").get<double>()-normal.position.y;
   check(std::hypot(dx,dy)>=minimum-.01&&std::hypot(dx,dy)<=maximum+.01,"Multi-drop exceeded scatter bounds");
   angles.push_back(std::atan2(dy,dx));
  }
  std::sort(angles.begin(),angles.end());
  angles.push_back(angles.front()+turn);
  const auto expected=turn/rows.size();
  for(std::size_t i=1;i<angles.size();++i)
   check(std::abs(angles[i]-angles[i-1]-expected)<.01,"Drops clustered instead of occupying separate sectors");
 }
 WorldScene scene(content);auto population=scene.map("berneo");Entity boss;found=false;for(auto&[id,m]:*population)if(m.combat&&m.category=="boss"&&m.maxhp>100){boss=m;found=true;break;}check(found,"No boss fixture");Json bossPos={boss.position.x,boss.position.y,boss.position.z};bool popup=false;Json ranking;auto send=[&](unsigned char c,unsigned char s,const Bytes&body){if(c==2&&s==11&&body.size()>15){auto message=Json::parse(std::string(body.begin()+14,body.end()-1));if(message.value("cmd","")=="asmara.boss.damage"){popup=true;ranking=message;}}};
 Bytes appearance(260),movement(36);std::string first="HighDamage",second="LastHit";appearance.resize(254+first.size()+1);appearance[253]=first.size();std::copy(first.begin(),first.end(),appearance.begin()+254);appearance.resize(254+first.size()+1);
 Json stats={{"hp",100000},{"level",boss.level},{"attack",boss.maxhp*.65*(100+3+1.6*boss.level)/100},{"attack_speed",1.},{"critical",0.}};scene.update_player("top",997011,"berneo",bossPos,appearance,movement,false,send,stats);
 appearance.resize(254+second.size()+1);appearance[253]=second.size();std::copy(second.begin(),second.end(),appearance.begin()+254);stats["attack"]=1e12;scene.update_player("last",997012,"berneo",bossPos,appearance,movement,false,send,stats);
 Bytes hit(32);put32(hit,1,997011,true);put32(hit,28,boss.eid,true);scene.strike("top",1,hit,100,{});check(scene.map("berneo")->at(boss.eid).alive,"First hit unexpectedly killed boss");put32(hit,1,997012,true);bool committed=false;scene.strike("last",1,hit,100,[&](Entity&m){committed=true;check(m.loot_owner==997011&&m.loot_recipients.count(997011)&&!m.loot_recipients.count(997012),"Last hit stole winner's boss loot");});check(committed&&popup,"Boss death did not publish damage popup");check(ranking.at("solo")[0].at("name")==first,"Damage ranking misordered");std::uint64_t total=0;for(auto&r:ranking.at("solo"))total+=r.at("damage").get<std::uint64_t>();check(total==static_cast<std::uint64_t>(boss.maxhp),"Overkill inflated ranking");
 std::cout<<"PASS: quest-only generic exclusion and private pickup; ground ID recycling and TTL; boss actual damage, winner merit, overkill exclusion and popup payload.\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
