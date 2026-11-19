#include "native/gm.hpp"
#include "native/economy.hpp"
#include <algorithm>
#include <iostream>
using namespace rosh;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int wmain(int argc,wchar_t**argv){try{
 if(argc!=2)throw std::runtime_error("Use an isolated GM fixture");fs::path root=argv[1];check(fs::is_regular_file(root/"GM_TEST_FIXTURE"),"Refusing non-fixture database");Content content(root);Sqlite db(root/"Database/karos_gameplay.sqlite");Inventory inventory(db,content);
 gm_schema(db);
 check(consignment_category(inventory.definition(48001))==ConsignmentCategory::MountShoulders,"Mount missing from Mount / Shoulders");
 check(consignment_category(inventory.definition(310001))==ConsignmentCategory::MountShoulders,"Wings missing from Mount / Shoulders");
 check(consignment_category(inventory.definition(53001))==ConsignmentCategory::WeaponOther,"Fletta weapon missing from Weapons / Other");
 check(consignment_category(inventory.definition(53501))==ConsignmentCategory::ArmorTorso,"Fletta armor missing from Armor / Torso");
 check(!consignment_category(inventory.definition(34813)),"Ordinary chest armor received the Fletta override");
 auto rows=db.query("SELECT * FROM gm_spawns LIMIT 1");check(!rows.empty(),"No fixture spawn");auto spawn=rows[0];
 db.exec("UPDATE gm_spawns SET expires=?",{now()+600});
 Json position={spawn["x"],spawn["y"],spawn["z"]};std::set<std::uint32_t> visible;
 struct Packet{int cat,sub;Bytes body;};std::vector<Packet> packets;int refresh=0;
 auto send=[&](unsigned char cat,unsigned char sub,const Bytes&body){packets.push_back({cat,sub,body});};
 auto poll=[&](int channel){gm_world_poll(root,db,inventory,50002,spawn.at("map"),channel,position,visible,send,[&]{refresh++;});};
 auto iid=inventory.items(50002).at(0).item;auto total=[&]{int n=0;for(auto&i:inventory.items(50002))if(i.item==iid)n+=i.quantity;return n;};int before=total();
 Json payload={{"items",Json::array({{{"id",iid},{"quantity",1}}})},{"enchant",0}};
 db.exec("INSERT INTO gm_actions VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",{std::string("bridge-grant-admin"),std::string("gmfixture0"),std::int64_t(50001),std::string("fixture-generation-0"),std::string("admin"),std::string("give"),std::int64_t(50002),payload.dump(),std::string("pending"),std::string("test"),now(),now()});
 poll(1);check(refresh==1&&total()==before+1,"Online grant missing");
 bool native=false;for(auto&p:packets)if(p.cat==4&&p.sub==2){check(get32(p.body,64)==spawn.at("npc_id"),"Wrong summoned native ID");native=true;}check(native,"No native spawn packet");
 packets.clear();poll(1);check(refresh==1&&total()==before+1&&packets.empty(),"Duplicate delivery or spawn");
 poll(2);check(packets.size()==1&&packets[0].cat==4&&packets[0].sub==3,"Channel visibility isolation missing");
 packets.clear();poll(1);check(!packets.empty(),"Visibility restoration missing");db.exec("UPDATE gm_spawns SET expires=?",{now()-1});packets.clear();poll(1);check(packets.size()==1&&packets[0].sub==3,"Expired spawn not removed");
 db.exec("INSERT INTO gm_actions VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",{std::string("bridge-revoked-gm"),std::string("gmfixture1"),std::int64_t(50002),std::string("fixture-generation-1"),std::string("gm"),std::string("give"),std::int64_t(50002),payload.dump(),std::string("pending"),std::string("test"),now(),now()});
 poll(1);check(total()==before+1,"Revoked staff delivered items");check(db.query("SELECT status FROM gm_actions WHERE id='bridge-revoked-gm'")[0].at("status")=="cancelled","Revoked staff request not cancelled");
 const std::string mail_id="bridge-native-mail";
 const auto stamp=now();
 db.exec("INSERT INTO gm_actions VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",{mail_id,std::string("gmfixture0"),std::int64_t(50001),std::string("fixture-generation-0"),std::string("admin"),std::string("mail"),std::int64_t(50002),payload.dump(),std::string("pending"),std::string("test"),stamp,stamp});
 db.exec("INSERT INTO gm_mail VALUES(?,?,?,?,?,?,?,?)",{mail_id,std::int64_t(50002),std::string("GM Gift"),std::string("Mail fixture"),std::string("Claim me"),payload.at("items").dump(),std::string("pending"),stamp});
 poll(1);check(total()==before+1,"Mail bypassed the NPC and granted items during polling");
 auto inbox=gm_mail_inbox(db,50002,stamp);
 auto mail=std::find_if(inbox.begin(),inbox.end(),[&](const auto &row){return row.at("id")==mail_id;});
 check(mail!=inbox.end()&&mail->at("read_at").is_null(),"New mail missing or already read");
 auto listed=native_mail_list({*mail},stamp);
 check(get32(listed,4,true)==1&&get32(listed,8,true)==1&&get16(listed,listed.size()-2,true)==1,"Incorrect native list");
 Bytes read(10);put32(read,0,mail->at("stamp"),true);put32(read,4,mail->at("native_id"),true);put16(read,8,2,true);
 auto opened=gm_mail_access(root,db,inventory,50002,read,false,stamp);
 check(!opened.at("read_at").is_null()&&total()==before+1,"Opening mail consumed attachments");
 auto detail=native_mail_detail(inventory,50002,opened);
 const auto attachment=detail.size()-8*147;
 check(get16(detail,attachment-6,true)==1&&detail[attachment]==0xff&&get32(detail,attachment+2)==iid,"Incorrect native attachment detail");
 auto claim=read;claim.resize(18);claim[9]=0;put16(claim,16,2,true);
 bool denied=false;
 try{gm_mail_access(root,db,inventory,50001,claim,true,stamp);}catch(const std::exception&){denied=true;}
 check(denied,"Another character claimed mail");
 auto claimed=gm_mail_access(root,db,inventory,50002,claim,true,stamp);
 check(claimed.at("status")=="delivered"&&total()==before+2,"Mailbox claim failed");
 auto reread=gm_mail_access(root,db,inventory,50002,claim,true,stamp);
 check(reread.at("status")=="delivered"&&total()==before+2,"Mailbox claim replay duplicated the attachment");
 check(db.query("SELECT uid FROM gm_item_origins WHERE action_id=?",{mail_id}).size()==1,"Mail attachment lost its trace UID");
 const auto listedItem=inventory.items(50002).back();
 const std::uint32_t stackItem=53901;
 check(inventory.grant_once(50001,{{stackItem,8}},"consignment-split-test",true),"Could not create test stack");
 const auto sellerItems=inventory.items(50001);
 const auto stack=std::find_if(sellerItems.begin(),sellerItems.end(),[&](const auto &i){return i.item==stackItem&&i.quantity==8;});
 check(stack!=sellerItems.end(),"Missing test stack");
 bool badQuantity=false;
 try{consignment_list(db,inventory,50001,stack->slot,stack->item,stack->uid,20,stamp,9);}
 catch(const std::exception&){badQuantity=true;}
 check(badQuantity&&inventory.owned(50001,stack->slot,stack->item,stamp).quantity==8,"Invalid quantity changed original stack");
 const auto splitListing=consignment_list(db,inventory,50001,stack->slot,stack->item,stack->uid,21,stamp,3);
 const auto listedStack=db.query("SELECT uid,quantity,item_created FROM consignment WHERE id=?",{splitListing}).at(0);
 ConsignmentSearch search;
 search.item=stackItem;
 auto offers=consignment_search(db,inventory,search,stamp,1);
 check(offers.at("total")==1&&offers.at("rows").at(0).at("id")==splitListing&&
       offers.at("rows").at(0).at("price")==21,"Search did not preserve quoted price");
 auto query=unhex("00000001000000010000019200000000000000010000006e000000000000000d0202000019a800000000000000000000000000cb");
 put32(query,34,stackItem,true);
 auto nativePage=native_consignment_page(db,inventory,query,offers);
 check(native_consignment_search(db,inventory,query,stamp)==nativePage,"Native search adapter changed exact result");
 auto secondPage=query;put32(secondPage,38,2,true);
 auto emptyPage=native_consignment_search(db,inventory,secondPage,stamp);
 check(emptyPage.size()==52&&get32(emptyPage,38,true)==2&&get32(emptyPage,42,true)==0&&get32(emptyPage,46,true)==1,
       "Native search adapter lost count on empty page");
 const auto sellerTerminator=std::find(nativePage.begin()+112,nativePage.end(),0);
 check(get32(nativePage,38,true)==1&&get32(nativePage,42,true)==1&&get32(nativePage,46,true)==1&&
       get32(nativePage,52,true)==splitListing&&get32(nativePage,56,true)==stackItem&&
       get32(nativePage,68,true)==7&&get32(nativePage,76,true)==3&&get32(nativePage,108,true)==21&&
       nativePage.end()-sellerTerminator==96&&get32(nativePage,sellerTerminator-nativePage.begin()+1,true)==
       static_cast<std::uint32_t>(stamp+86400),"Search response does not match native page framing");
 search.category=ConsignmentCategory::MountShoulders;
 check(consignment_search(db,inventory,search,stamp,1).at("total")==0,"Category search returned a potion");
 search.item=0;
 for(const auto itemId:{48001,310001,53001,53501}) {
   db.exec("INSERT INTO consignment(seller,item_id,uid,quantity,item_created,price,expires,status) "
           "VALUES(50001,?,?,1,0,1,?,'listed')",{std::int64_t(itemId),std::int64_t(900000+itemId),stamp+5});
 }
 check(consignment_search(db,inventory,search,stamp,1).at("total")==2,"Mounts and wings did not share a category");
 search.category=ConsignmentCategory::WeaponOther;
 check(consignment_search(db,inventory,search,stamp,1).at("rows").at(0).at("item_id")==53001,"Fletta weapon search mismatch");
 search.category=ConsignmentCategory::ArmorTorso;
 check(consignment_search(db,inventory,search,stamp,1).at("rows").at(0).at("item_id")==53501,"Fletta armor search mismatch");
 auto browse=query;
 put32(browse,4,0,true);
 put32(browse,34,0,true);
 for(const auto [category,child,count]:{std::array{110,0,1},std::array{200,202,1},std::array{205,0,2}}){
   put32(browse,8,category,true);
   put32(browse,12,child,true);
   check(get32(native_consignment_search(db,inventory,browse,stamp),42,true)==count,
         "Native subcategory search did not return expected listings");
 }
 search.item=stackItem;
 search.category.reset();
 check(consignment_search(db,inventory,search,stamp+86400,1).at("total")==0,"Search returned an expired offer");
 const auto remainder=inventory.owned(50001,stack->slot,stack->item,stamp);
 check(remainder.quantity==5&&remainder.uid==stack->uid&&listedStack.at("quantity")==3&&
       listedStack.at("uid")!=stack->uid&&listedStack.at("item_created")==stack->created,
       "Partial listing moved the wrong quantity or identity");
 const auto listing=consignment_list(db,inventory,50002,listedItem.slot,listedItem.item,listedItem.uid,1700,stamp);
 consignment_expire(db,50002,stamp+86399);
 const auto returnedId="consignment-return:"+std::to_string(listing);
 check(db.query("SELECT 1 FROM gm_mail WHERE id=?",{returnedId}).empty(),"Consignment expired before 24 hours");
 auto returned=gm_mail_inbox(db,50002,stamp+86400);
 auto message=std::find_if(returned.begin(),returned.end(),[&](const auto &row){return row.at("id")==returnedId;});
 check(message!=returned.end(),"Expired consignment did not reach mailbox");
 put32(claim,0,message->at("stamp"),true);put32(claim,4,message->at("native_id"),true);
 gm_mail_access(root,db,inventory,50002,claim,true,stamp+86400);
 auto owned=inventory.items(50002);
 check(std::any_of(owned.begin(),owned.end(),[&](const auto &i){return i.uid==listedItem.uid&&i.quantity==listedItem.quantity&&i.grade==listedItem.grade&&i.created==listedItem.created;}),"Consignment return replaced original item");
 const auto restored=std::find_if(owned.begin(),owned.end(),[&](const auto &i){return i.uid==listedItem.uid;});
 const auto sold=consignment_list(db,inventory,50002,restored->slot,restored->item,restored->uid,1700,stamp+86400);
 Wallet wallet(db,inventory);wallet.apply(50003,2000,"bridge-consignment-funds");const auto sellerBalance=wallet.balance(50002);
 bool wrongPrice=false;try{consignment_buy(db,inventory,50003,sold,1699,stamp+86401);}catch(const std::exception&){wrongPrice=true;}
 check(wrongPrice&&wallet.balance(50002)==sellerBalance,"Consignment accepted a different price");
 consignment_buy(db,inventory,50003,sold,1700,stamp+86401);
 check(wallet.balance(50002)==sellerBalance+1700,"Consignment did not credit seller");
 auto purchased=inventory.items(50003);
 check(std::any_of(purchased.begin(),purchased.end(),[&](const auto &i){return i.uid==listedItem.uid;}),"Purchase replaced original item");
 auto origin=db.query("SELECT uid FROM gm_item_origins WHERE action_id='bridge-grant-admin'");
 check(origin.size()==1,"GM delivery lacked a trace UID");
 const auto uid=origin[0].at("uid").get<std::uint32_t>();
 auto items=inventory.items(50002);
 auto item=std::find_if(items.begin(),items.end(),[&](const auto& candidate){return candidate.uid==uid;});
 check(item!=items.end(),"Trace UID not present in delivered inventory");
 auto slots=inventory.available(50002,now());
 for(const auto& candidate:items)slots.erase(candidate.slot);
 check(!slots.empty(),"Trace fixture has no free slot");
 const int destination=*slots.begin();
 auto from=Inventory::address(item->slot),to=Inventory::address(destination);
 Bytes move(16);put32(move,0,50002,true);move[4]=from.first;move[5]=from.second;
 put32(move,6,item->item,true);move[10]=to.first;move[11]=to.second;
 inventory.move(50002,move);
 auto moved=inventory.owned(50002,destination,item->item,now());
 check(moved.uid==uid,"Inventory move changed trace UID");
 inventory.consume(50002,moved,1,"npc-sale");
 auto trail=db.query("SELECT operation,holder_eid,source_slot,destination_slot FROM gm_item_history "
                     "WHERE uid=? ORDER BY sequence",{std::int64_t(uid)});
 check(trail.size()==2&&trail[0].at("operation")=="move"&&trail[0].at("destination_slot")==destination&&
       trail[1].at("operation")=="npc-sale"&&trail[1].at("holder_eid")==50002,
       "Move and disposition did not leave an ordered trace");
 std::set<std::uint32_t> prior;
 for(const auto& existing:inventory.items(50002))prior.insert(existing.uid);
 inventory.grant_once(50002,{{moved.item,1}},"bridge-derived-fixture",true);
 auto after=inventory.items(50002);
 auto child=std::find_if(after.begin(),after.end(),[&](const auto& candidate){return !prior.count(candidate.uid);});
 check(child!=after.end()&&!inventory.traced(child->uid),"Ordinary grant unexpectedly inherited GM provenance");
 inventory.trace_derivation(uid,*child,50002);
 check(inventory.traced(child->uid)&&db.query("SELECT parent_uid FROM gm_item_lineage WHERE uid=?",
       {std::int64_t(child->uid)})[0].at("parent_uid")==uid,"Derived item lost its parent UID");
 std::cout<<"PASS: WorldServer bridge; live inventory refresh; duplicate prevention; native 04/02 summon; channel visibility; expiry 04/03 removal; queued author revocation.\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
