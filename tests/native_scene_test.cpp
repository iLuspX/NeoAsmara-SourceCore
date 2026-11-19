#include "native/scene.hpp"
#include "native/staff.hpp"
#include <iostream>
using namespace rosh;
static void check(bool value,const char*message){if(!value)throw std::runtime_error(message);}
static void qty(Bytes&body,std::size_t offset,std::uint64_t amount){for(int i=7;i>=0;i--){body[offset+i]=amount&255;amount>>=8;}}
int wmain(int argc,wchar_t**argv){try{
 check(argc==2,"Use a copied GM fixture");fs::path root=argv[1];check(fs::is_regular_file(root/"GM_TEST_FIXTURE"),"Refusing non-fixture database");Content content(root);WorldScene scene(content);auto actors=scene.map("berneo");check(!actors->empty(),"Berneo has no actors");check(scene.map("berneo")==actors,"Map state not shared");
 for(auto &entry:*actors)check(entry.first<10000,"NPC actor overlaps player identity range");
 auto typed=content.database("database/content_typed.sqlite");auto rules=content.get("data/native_item_rules.json");
 const Entity* merchant=nullptr;std::uint32_t item_id=0;Json shop;
 for(auto &[id,actor]:*actors){if(actor.combat)continue;auto rows=typed->query("SELECT o8_i,o12_f64,o20_f64 FROM npcShop WHERE o0_i=?",{std::int64_t(actor.npc_id)});if(rows.empty())continue;
  for(auto& item:typed->query("SELECT o4_i FROM npcShopItem WHERE o0_i=?",{rows[0].at("o8_i").get<std::int64_t>()})){
   auto iid=item.at("o4_i").get<std::uint32_t>();auto rule=rules.value(std::to_string(iid),Json::object());
   if(rule.value("buy",0.)>0&&rule.value("sell",0.)>0&&rule.value("duration",0)==0&&rule.value("stack",1)>2){merchant=&actor;item_id=iid;shop=rows[0];break;}
  }if(merchant)break;
 }
 check(merchant&&item_id,"No native merchant with normal stackable goods");
 Json position={merchant->position.x,merchant->position.y,merchant->position.z};std::set<std::uint32_t> visible,seen;int npc_packets=0,removals=0;
 auto send=[&](unsigned char cat,unsigned char sub,const Bytes&body){check(cat==4,"Unexpected visibility opcode");if(sub==2){auto eid=get32(body,2);check(seen.insert(eid).second,"Duplicate entity published");check(body.size()==254&&body[1]==1&&get32(body,64)==actors->at(eid).npc_id,"Wrong native NPC identity");++npc_packets;}else{check(sub==3&&body[0]==1,"Wrong NPC removal packet");++removals;}};
 for(int i=0;i<20;i++)scene_visibility(scene,*actors,position,visible,send);check(visible.count(merchant->eid)&&npc_packets>0,"Merchant not visible");auto count=visible.size();
 std::size_t pinned=0;
 for(auto id:visible)if(actors->at(id).rules.value("dev_always_visible",false))++pinned;
 scene_visibility(scene,*actors,Json::array({1.e9,1.e9,0.}),visible,send);
 check(visible.size()==pinned&&removals==static_cast<int>(count-pinned),"Visibility out-of-range did not remove ordinary actors");
 for(auto id:visible)check(actors->at(id).rules.value("dev_always_visible",false),"Out-of-range ordinary actor remained visible");
 Sqlite db(root/"Database/karos_gameplay.sqlite");Inventory inventory(db,content);NpcShops shops(db,content,inventory);Wallet wallet(db,inventory);constexpr std::uint32_t player=50001;
 wallet.apply(player,1000000,"scene-fund");Json character={{"eid",player},{"hp",100},{"pos",position}};
 Bytes buy(22);put32(buy,0,player,true);put32(buy,4,merchant->eid,true);put32(buy,8,item_id,true);put16(buy,12,9,true);qty(buy,14,2);
 auto before=wallet.balance(player);auto reply=shops.transact(*actors,character,13,buy,"scene-buy");check(get16(reply,12,true)==44,"Wrong native purchase reply");auto paid=wallet.balance(player);check(paid<before,"Purchase did not charge");
 auto sum=[&]{int total=0;for(auto&i:inventory.items(player))if(i.item==item_id)total+=i.quantity;return total;};check(sum()==2,"Purchase quantity incorrect");
 check(shops.transact(*actors,character,13,buy,"scene-buy")==reply&&sum()==2&&wallet.balance(player)==paid,"Purchase replay duplicated reward/charge");
 auto changed=buy;qty(changed,14,3);bool rejected=false;try{shops.transact(*actors,character,13,changed,"scene-buy");}catch(...){rejected=true;}check(rejected,"Mutated request replay accepted");
 auto distant_character=character;distant_character["pos"]={99999.,99999.,0.};rejected=false;try{shops.transact(*actors,distant_character,13,buy,"scene-far");}catch(...){rejected=true;}check(rejected,"Remote NPC purchase accepted");
 ItemInstance purchased;for(auto&i:inventory.items(player))if(i.item==item_id)purchased=i;
 auto address=Inventory::address(purchased.slot);Bytes sell(30);put32(sell,0,player,true);put32(sell,4,address.first,true);put32(sell,8,address.second,true);put32(sell,12,item_id,true);put32(sell,16,merchant->eid,true);put16(sell,20,45,true);qty(sell,22,2);
 shops.transact(*actors,character,14,sell,"scene-sell");auto credited=wallet.balance(player);check(sum()==0&&credited>paid,"Sale not committed");shops.transact(*actors,character,14,sell,"scene-sell");check(wallet.balance(player)==credited,"Sale replay paid twice");
 inventory.grant_timed_once(player,item_id,2,3600,"scene-timed");auto timed=inventory.items(player);int times=0;for(auto&i:timed)if(i.item==item_id){check(i.quantity==1&&i.expires&&*i.expires>now(),"Timed item not unique or permanent");++times;}check(times==2,"Timed units merged");inventory.grant_timed_once(player,item_id,2,3600,"scene-timed");check(sum()==2,"Timed grant duplicated");
 {
  auto instances=inventory.items(player);ItemInstance one,two;
  for(auto&i:instances)if(i.item==item_id){if(!one.uid)one=i;else two=i;}
  check(one.uid&&two.uid&&one.uid!=two.uid,"Timed fixture instances missing");
  auto from=Inventory::address(one.slot),to=Inventory::address(two.slot);Bytes move(16);put32(move,0,player,true);move[4]=from.first;move[5]=from.second;put32(move,6,item_id,true);move[10]=to.first;move[11]=to.second;put32(move,12,item_id,true);
  inventory.move(player,move);check(inventory.owned(player,two.slot,item_id,now()).uid==one.uid,"Bag swap changed instance identity");
  auto wrong=move;put32(wrong,12,0,true);rejected=false;try{inventory.move(player,wrong);}catch(...){rejected=true;}check(rejected,"Bag move accepted changed destination");
  Bytes sort(10);put32(sort,0,player,true);put16(sort,8,0x22a,true);inventory.organize(player,sort);
  int unique=0;for(auto&i:inventory.items(player))if(i.item==item_id){check(i.expires&&i.quantity==1&&(i.uid==one.uid||i.uid==two.uid),"Sorting merged timed instances");++unique;}check(unique==2,"Sorting lost timed item");
  auto current=inventory.items(player);ItemInstance selected;for(auto&i:current)if(i.uid==one.uid)selected=i;auto slot=Inventory::address(selected.slot);
  inventory.grant_once(player,{{69009,1}},"scene-trash-bag");
  for(const auto &i:inventory.items(player))if(i.item==69009)inventory.equip(player,i.slot,69009,600,1,14,now());
  Bytes to_bag(16);put32(to_bag,0,player,true);to_bag[4]=slot.first;to_bag[5]=slot.second;put32(to_bag,6,item_id,true);to_bag[10]=3;to_bag[11]=9;
  inventory.move(player,to_bag);
  Bytes trash(26);put32(trash,0,player,true);put32(trash,4,3,true);put32(trash,8,9,true);put32(trash,12,item_id,true);qty(trash,16,~std::uint64_t(0));put16(trash,24,181,true);
  trash.insert(trash.end(),{'1','2','2','1','1',0});
  inventory.trash(player,trash,"scene-trash");inventory.trash(player,trash,"scene-trash");check(sum()==1,"Trash replay removed a second instance");
  check(db.query("SELECT uid FROM reward_instances WHERE uid=?",{std::int64_t(one.uid)}).empty(),"Trash left timed instance metadata");
  auto altered=trash;qty(altered,16,1);rejected=false;try{inventory.trash(player,altered,"scene-trash");}catch(...){rejected=true;}check(rejected,"Trash replay accepted changed quantity");
  inventory.grant_once(player,{{item_id,5}},"scene-regular");inventory.organize(player,sort);int permanent=0;for(auto&i:inventory.items(player))if(i.item==item_id&&!i.expires)permanent+=i.quantity;check(permanent==5&&sum()==6,"Sorting lost permanent stack quantity");
  auto balance=wallet.balance(player);ItemInstance pile;for(auto&i:inventory.items(player))if(i.item==999999)pile=i;slot=Inventory::address(pile.slot);put32(trash,4,slot.first,true);put32(trash,8,slot.second,true);put32(trash,12,999999,true);qty(trash,16,10);
  trash.resize(26);trash.resize(30,0);
  inventory.trash(player,trash,"scene-trash-carats");inventory.trash(player,trash,"scene-trash-carats");check(wallet.balance(player)==balance-10,"Currency removal not atomic or replayed twice");
 }
 auto empty_command=command_tokens("\"\"");check(empty_command.empty()||empty_command.front().empty(),"Empty command token rejected unsafely");
 check(displayed_name("Mage","GM")=="[GM] Mage"&&displayed_name("Mage","Admin")=="[ADMIN] Mage"&&displayed_name("Mage","")=="Mage","Role display differs from Python");
 check(displayed_name("0123456789012345678901234567890123456789","Admin").size()==35,"Role display does not preserve limit");
 {
  Sqlite roles(root/"Database/karos_roles.sqlite");roles.exec("INSERT INTO staff_roles(account_id,character_eid,character_name,role,auth_generation) VALUES('native-staff-fixture',71001,'NativeStaff','Admin','native-staff-generation')");
  StaffSession auth(root,"native-staff-fixture",71001);auto password=token(20);check(!auth.valid(),"Staff role bypasses second login");
  auth.authentication({"staffregister",password,password});check(!auth.valid(),"Registration authenticates staff implicitly");auth.authentication({"stafflogin",password});check(auth.valid(),"Native staff login failed");
  roles.exec("UPDATE staff_roles SET auth_generation='new-generation' WHERE account_id='native-staff-fixture'");check(!auth.valid(),"Old staff credential generation remains valid");bool denied=false;try{auth.authentication({"stafflogin",password});}catch(...){denied=true;}check(denied,"Old staff password survived regrant");
  auto replacement=token(20);auth.authentication({"staffregister",replacement,replacement});auth.authentication({"stafflogin",replacement});check(auth.valid(),"Staff cannot reregister after role change");auth.authentication({"stafflogout"});check(!auth.valid(),"Staff logout not revoked");
 }
 {
  struct Message{int cat,sub;Bytes body;};std::vector<Message> first,second,foreign;
  auto listener=[](std::vector<Message>&out){return [&out](unsigned char cat,unsigned char sub,const Bytes&body){out.push_back({cat,sub,body});};};
  Bytes a(260),b(260),c(260),movement(34);a[0]=b[0]=c[0]=1;put32(a,2,51001);put32(b,2,51002);put32(c,2,51003);
  scene.update_player("one",51001,"berneo",position,a,movement,false,listener(first));
  scene.update_player("two",51002,"berneo",position,b,movement,false,listener(second));
  check(first.size()==2&&first[0].sub==2&&get32(first[0].body,2)==51002,"Other player not published");check(second.size()==2&&get32(second[0].body,2)==51001,"Reciprocal visibility missing");
  check(first[1].cat==6&&first[1].sub==0x46&&first[1].body.size()==6&&get32(first[1].body,0,true)==51002&&first[1].body[4]==0&&first[1].body[5]==0,"Late observer did not receive the native cash appearance flags");
  check(second[1].cat==6&&second[1].sub==0x46&&get32(second[1].body,0,true)==51001,"Reciprocal native cash appearance flags missing");
  first.clear();second.clear();
  scene.reset_observer("one");
  scene.update_player("one",51001,"berneo",position,a,movement,false,listener(first));
  check(first.size()==2&&first[0].cat==4&&first[0].sub==2&&get32(first[0].body,2)==51002,
        "Observer rebuild did not restore the other player's appearance");
  check(first[1].cat==6&&first[1].sub==0x46&&second.empty(),
        "Observer rebuild affected another player's view or lost cash flags");
  scene.update_player("two",51002,"berneo",position,b,movement,false,listener(second),{{"sitting",true}});
  first.clear();second.clear();scene.reset_observer("one");
  scene.update_player("one",51001,"berneo",position,a,movement,false,listener(first));
  check(first.size()==3&&first[2].cat==3&&first[2].sub==2&&first[2].body.size()==11&&first[2].body[4]==1&&get32(first[2].body,0,true)==51002,
        "Late observer lost the other player's native seated pose");
  scene.update_player("two",51002,"berneo",position,b,movement,false,listener(second),{{"sitting",false}});
  scene.update_player("foreign",51003,"peltrok",position,c,movement,false,listener(foreign));check(foreign.empty(),"Player leaked between maps");
  first.clear();auto moved=position;moved[0]=position[0].get<double>()+30;
  scene.update_player("two",51002,"berneo",moved,b,movement,false,listener(second));check(first.size()==1&&first[0].sub==1&&get32(first[0].body,2,true)==51002,"Validated movement not broadcast");
  first.clear();scene.update_player("two",51002,"berneo",moved,b,movement,true,listener(second));check(first.size()==1&&first[0].sub==3,"Vanish did not hide other actor");
  first.clear();second.clear();Bytes emotion(36);put32(emotion,1,51001,true);put32(emotion,5,11,true);put32(emotion,9,1,true);put32(emotion,30,51001,true);put16(emotion,34,0x24,true);
  scene.action("one",5,3,emotion);
  check(first.empty()&&second.size()==1&&second[0].cat==5&&second[0].sub==3&&second[0].body==emotion&&foreign.empty(),"Emotion did not reach only nearby observers in the same map");
  first.clear();second.clear();scene.chat("one","[ADMIN] Staff","Hello",1);check(first.size()==1&&second.size()==1&&foreign.empty()&&first[0].cat==3&&get32(first[0].body,0,true)==51001,"Chat identity or map scope lost");
  scene.remove_player("one");scene.remove_player("two");scene.remove_player("foreign");
 }
 std::cout<<"PASS: shared native map; exact NPC models; proximity streaming/removal; NPC purchases/sales; request replay validation; timed instances; inventory swap/sort/trash; atomic currency removal; render-only prefixes; staff second login/revocation; multiplayer visibility/movement/vanish and map-scoped chat.\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
