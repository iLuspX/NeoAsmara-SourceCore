#include "native/game.hpp"
#include <iostream>
#include <algorithm>
using namespace rosh;
static void check(bool value,const char*message){if(!value)throw std::runtime_error(message);}
int wmain(int argc,wchar_t**argv){const char*stage="initialization";try{
 check(argc==2,"Use isolated fixture");fs::path root=argv[1];check(fs::is_regular_file(root/"GM_TEST_FIXTURE"),"Refusing live inventory");Content content(root);Sqlite db(root/"Database/karos_gameplay.sqlite");Inventory inventory(db,content);CashChoices choices(db,content,inventory);constexpr std::uint32_t eid=74001;
 stage="outer box";inventory.grant_timed_once(eid,999000303,1,3600,"costume-fixture-outer");ItemInstance outer=inventory.items(eid).at(0);auto boxes=choices.open(eid,outer.slot,outer.item,600);check(boxes.size()==4,"Cash outfit did not give four piece selectors");check(inventory.items(eid).size()==4,"Cash selectors delivered duplicate sets");stage="piece choice";
 auto pieces=inventory.items(eid);auto expected=content.get("data/starter_costume_choices.json").at("parts").at("600");int selected=0;
 for(auto&box:pieces){auto preview=choices.preview(eid,box.uid,600);check(preview.at("options").size()==2,"Piece does not offer exactly two options");int part=(box.item-180148)%4,option=part%2+1;auto result=choices.choose(eid,box.uid,600,option);check(result.at("item")==expected.at(part),"Wrong class or part delivered");auto replay=choices.choose(eid,box.uid,600,option);check(replay==result,"Choice replay changed result");bool denied=false;try{choices.choose(eid,box.uid,600,3-option);}catch(...){denied=true;}check(denied,"Selector delivered both attribute configurations");selected++;}
 check(selected==4&&inventory.items(eid).size()==4,"Outfit not exactly torso/pants/gloves/boots");for(auto&item:inventory.items(eid))check(item.expires&&*item.expires<=*outer.expires,"Selector reset or removed expiration");
 stage="native costume catalog";auto rules=content.get("data/native_item_rules.json");std::uint32_t native=0;std::size_t coverage=0;
 for(auto it=rules.begin();it!=rules.end();++it){auto&r=it.value();if(r.value("kind",0)==1&&r.value("slot",0)>=26&&r.value("slot",0)<=29){coverage++;if(!native&&r.value("class",0)==600&&r.value("slot",0)==26&&r.value("level",0)==1&&r.value("duration",0)==0)native=static_cast<std::uint32_t>(std::stoul(it.key()));}}
 check(native&&coverage>1000,"General native costume catalog was reduced to Santa only");
 constexpr std::uint32_t actor=74002;LegacyStore store(root/"Database/karos_gameplay.sqlite");Progression progression;Json ch={{"eid",actor},{"name","CostumeFixture"},{"cls",600},{"level",115},{"exp",progression.threshold(115)},{"hp",100000},{"mp",100000}};store.db().exec("INSERT INTO accounts(id,payload) VALUES(?,?)",{std::string("costume-fixture"),Json{{"chars",Json::array({ch})}}.dump()});
 Inventory owned(store.db(),content);owned.grant_once(actor,{{native,1}},"costume-native-fixture");auto original=owned.items(actor).at(0);Session session;session.identity.id="costume-fixture";session.id=token(20);session.channel=1;session.expires=now()+600;
 struct Packet{int cat,sub;Bytes body;};std::vector<Packet>packets;auto send=[&](unsigned char cat,unsigned char sub,const Bytes&body){packets.push_back({cat,sub,body});};
 {
  stage="world selection";WorldSession game(content,store,session,root,actor);Bytes ready(21);put32(ready,1,actor,true);putfloat(ready,5,9020);putfloat(ready,9,6590);putfloat(ready,13,627);game.handle(4,0,ready,send);packets.clear();
  Bytes wear(12);put32(wear,0,actor,true);auto address=Inventory::address(original.slot);wear[4]=address.first;wear[5]=address.second;wear[6]=26;put32(wear,7,native,true);game.handle(6,0,wear,send,"costume-wear-fixture");
  auto equipped=owned.items(actor,true);
  check(std::any_of(equipped.begin(),equipped.end(),[&](auto& item){return item.slot==26&&item.uid==original.uid&&item.item==native;}),"Direct cash piece did not equip into Cash torso slot");
  // The captured retail Cash wing acknowledgement uses slot 30 and the
  // stored grade byte. An 11-byte legacy request still receives 12 bytes.
  constexpr std::uint32_t wings=1000000323;
  owned.grant_timed_once(actor,wings,1,2592000,"retail-wing-fixture");
  const auto items=owned.items(actor);
  const auto wing=std::find_if(items.begin(),items.end(),[](const auto& item){return item.item==wings;});
  check(wing!=items.end(),"Captured Cash wing is missing from inventory");
  const auto wingAddress=Inventory::address(wing->slot);
  Bytes wingWear(11);put32(wingWear,0,actor,true);wingWear[4]=wingAddress.first;wingWear[5]=wingAddress.second;wingWear[6]=255;put32(wingWear,7,wings,true);
  packets.clear();game.handle(6,0,wingWear,send,"retail-wing-equip");
  const auto wingAck=std::find_if(packets.begin(),packets.end(),[](const auto& packet){return packet.cat==6&&packet.sub==0;});
  check(wingAck!=packets.end()&&wingAck->body.size()==12&&wingAck->body[6]==30&&wingAck->body[11]==0,"Cash wing acknowledgement differs from the retail capture");
  Bytes wingRemove(12);put32(wingRemove,0,actor,true);wingRemove[6]=30;put32(wingRemove,7,wings,true);
  packets.clear();game.handle(6,1,wingRemove,send,"retail-wing-unequip");
  const auto removeAck=std::find_if(packets.begin(),packets.end(),[](const auto& packet){return packet.cat==6&&packet.sub==1;});
  check(removeAck!=packets.end()&&removeAck->body.size()==11&&removeAck->body[6]==30,"Cash wing removal acknowledgement differs from the retail capture");
  CashChoices shopChoices(store.db(),content,owned);
  auto boxUid=shopChoices.grant_catalog_box(actor,native,"costume-wear-box-fixture");
  auto bag=owned.items(actor);auto selector=std::find_if(bag.begin(),bag.end(),[&](auto& item){return item.uid==boxUid;});
  check(selector!=bag.end(),"Selector missing");auto selectorAddress=Inventory::address(selector->slot);
  wear[4]=selectorAddress.first;wear[5]=selectorAddress.second;wear[6]=1;put32(wear,7,selector->item,true);packets.clear();
  game.handle(6,0,wear,send,"selector-equip-fixture");
  bool preview=false;for(auto&p:packets)if(p.cat==2&&p.sub==11&&p.body.size()>15&&get32(p.body,0,true)==103){auto message=Json::parse(std::string(p.body.begin()+14,p.body.end()-1));if(message.value("cmd","")=="asmara.choice"&&message.at("uid")==boxUid&&message.at("options").size()==2)preview=true;}
  check(preview,"Client equip request did not open selector");
  auto after=owned.items(actor,true);check(std::none_of(after.begin(),after.end(),[&](auto& item){return item.uid==boxUid;}),"Selector was equipped as armor");
  auto text="npcshop starterchoose "+std::to_string(boxUid)+" 2";Bytes command(text.begin(),text.end());command.push_back(0);game.handle(7,1,command,send,"costume-choice-fixture");
  auto selected=owned.items(actor);check(std::any_of(selected.begin(),selected.end(),[&](auto& item){return item.item==native&&item.uid!=original.uid;}),"Selector did not deliver the native Cash piece");
 }
 std::cout<<"PASS: four class-native cash parts; two exclusive options; no duplicated sets; expiration preserved; native costume catalog coverage; in-game selector and equip continuation; UID/selected affixes persisted. Staged only.\n";return 0;
}catch(const std::exception&e){std::cerr<<stage<<": "<<e.what()<<"\n";return 1;}}
