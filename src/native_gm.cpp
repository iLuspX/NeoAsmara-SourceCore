#include "native/premium.hpp"
#include "native/titles.hpp"
#include "native/gm.hpp"
#include "native/runtime.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace rosh {
static std::string lower(std::string s){ for(auto &c:s) if(c>='A'&&c<='Z')c+=32; return s; }
static std::int64_t number(const Json& j,const char* key,std::int64_t min,std::int64_t max){
 if(!j.contains(key)||!j.at(key).is_number_integer())throw std::runtime_error(std::string("Invalid number: ")+key);
 auto n=j.at(key).get<std::int64_t>();if(n<min||n>max)throw std::runtime_error(std::string("Value outside the allowed range: ")+key);return n;
}
static std::string text(const Json& j,const char* key,std::size_t min,std::size_t max){
 auto s=j.at(key).get<std::string>();if(s.size()<min||s.size()>max||s.find('\0')!=std::string::npos)throw std::runtime_error(std::string("Invalid text: ")+key);return s;
}
static bool local_account_login(const fs::path&root){auto path=root/"Config/server.json";return fs::is_regular_file(path)&&Json::parse(read_file(path)).value("gm_local_account_login_only",false);}
static std::string class_label(int id){switch(id/100*100){case 100:return "Swordsman";case 200:return "Paladin";case 300:return "Rogue";case 400:return "Witch";case 500:return "Archer";case 600:return "Mystic";case 800:return "Gunner";default:return "Shared";}}
GmController::GmController(const fs::path &root):root_(root),content_(root),entities_(content_),accounts_(root/"Database/karos_login.sqlite"),world_(root/"Database/karos_gameplay.sqlite"),roles_(root/"Database/karos_roles.sqlite",true),credentials_(root/"Database/staff_second_login.sqlite",true),runtime_(root/"Database/runtime.sqlite"),moderation_(root/"Database/moderation.sqlite"),inventory_(world_.db(),content_){
 gm_schema(world_.db());
 JobQueue initialize_runtime(root_);
 runtime_.exec("CREATE TABLE IF NOT EXISTS native_player_leases(eid INTEGER PRIMARY KEY,session TEXT NOT NULL,account TEXT NOT NULL,channel INTEGER NOT NULL,expires REAL NOT NULL)");
 moderation_.exec("CREATE TABLE IF NOT EXISTS sanctions(id INTEGER PRIMARY KEY,kind TEXT NOT NULL,scope TEXT NOT NULL,subject TEXT NOT NULL,actor_account TEXT NOT NULL,actor_eid INTEGER NOT NULL,reason TEXT NOT NULL,created REAL NOT NULL,expires REAL,revoked REAL)");
 item_rows_=content_.get("data/item_catalog_verified.json");auto rules=content_.get("data/native_item_rules.json");
 for(auto& item:item_rows_){auto rule=rules.value(std::to_string(item.at("id").get<std::uint32_t>()),Json::object());item["level"]=rule.value("level",0);item["slot"]=rule.value("slot",0);item["kind"]=rule.value("kind",0);item["class"]=rule.value("class",0);item["class_name"]=class_label(rule.value("class",0));item["duration"]=rule.value("duration",0);item["equipment"]=(rule.value("kind",0)==1||rule.value("kind",0)==11)&&rule.value("slot",0)>0;}
 auto bosses=content_.get("server/respawn_catalog.json");
 for(auto it=entities_.maps().begin();it!=entities_.maps().end();++it)for(auto& pair:entities_.load(it.key())){
  auto e=pair.second;auto key=e.map+":"+std::to_string(e.spawn_index);placements_[key]=e;
  auto meta=bosses.value(std::to_string(e.npc_id),Json::object());
  std::string category=!e.combat?"npc":meta.value("neo",false)?"neo":e.category;
  entity_rows_.push_back({{"key",key},{"id",e.npc_id},{"name",e.name},{"category",category},{"map",e.map},{"index",e.spawn_index},{"level",e.level},{"hp",e.maxhp},{"x",e.home.x},{"y",e.home.y},{"z",e.home.z},{"respawn_seconds",e.rules.value("respawn_seconds",0.)}});
 }
}
Json GmController::authorize(const std::string& secret){
 auto it=sessions_.find(hex(sha256(secret)));if(it==sessions_.end()||it->second.until<=now())throw std::runtime_error("Session expired. Sign in again.");
 auto authority=gm_authority(root_,it->second.account,it->second.eid);
 if(authority.at("generation")!=it->second.generation){sessions_.erase(it);throw std::runtime_error("Staff credentials or role changed. Sign in again.");}
 authority["expires"]=it->second.until;return authority;
}
Json GmController::players(){
 Json out=Json::array();auto online=runtime_.query("SELECT eid,channel FROM native_player_leases WHERE expires>?",{now()});
 auto locationTable=world_.db().query("SELECT name FROM sqlite_master WHERE type='table' AND name='character_locations'");
 for(const auto& account:world_.db().query("SELECT id,payload FROM accounts"))for(const auto& ch:Json::parse(account.at("payload").get<std::string>()).value("chars",Json::array())){
  auto eid=ch.value("eid",0u);auto p=ch.value("pos",Json::array({0,0,0}));std::string map=ch.value("map","berneo");
  if(!locationTable.empty()){auto loc=world_.db().query("SELECT * FROM character_locations WHERE eid=?",{std::int64_t(eid)});if(!loc.empty()){p={loc[0]["x"],loc[0]["y"],loc[0]["z"]};map=loc[0].at("map_name");}}
  bool live=false;int channel=0;for(auto& lease:online)if(lease.at("eid")==eid){live=true;channel=lease.at("channel");}
  out.push_back({{"eid",eid},{"name",ch.value("name","")},{"level",ch.value("level",1)},{"class",ch.value("cls",0)},{"map",map},{"x",p.at(0)},{"y",p.at(1)},{"z",p.at(2)},{"online",live},{"channel",channel}});
 }
 return out;
}
static Json filtered(const Json& rows,const Json& body){
 std::string q=lower(body.value("query","")),category=body.value("category","");
 auto offset=body.value("offset",0);if(offset<0||offset>100000)throw std::runtime_error("Invalid page");
 Json selected=Json::array();int total=0;
 int family=body.value("class_family",0),slot=body.value("slot",0),low=body.value("min_level",0),high=body.value("max_level",1000);bool equipment=body.value("equipment_only",false);if(family!=0&&family!=100&&family!=200&&family!=300&&family!=400&&family!=500&&family!=600&&family!=800)throw std::runtime_error("Invalid class filter");if(slot<0||slot>40||low<0||high<low||high>1000)throw std::runtime_error("Invalid equipment filter");
 for(const auto &row:rows){if(equipment&&!row.value("equipment",false))continue;int cls=row.value("class",0);if(family&&cls/100*100!=family&&!(body.value("include_shared",true)&&cls==0))continue;if(slot&&row.value("slot",0)!=slot)continue;if(row.value("level",0)<low||row.value("level",0)>high)continue;if(!category.empty()&&row.value("category","")!=category)continue;if(!q.empty()&&lower(row.dump()).find(q)==std::string::npos)continue;if(total++>=offset&&selected.size()<200)selected.push_back(row);}
 return {{"rows",selected},{"total",total},{"offset",offset},{"page_size",200}};
}
Json GmController::request(const std::string& method,const std::string& path,const Json& body,const std::string& bearer){
 if(path=="/health"&&method=="GET")return {{"service","rosh-gm-native"},{"version",2},{"loopback_only",true},{"staff_password_required",!local_account_login(root_)}};
 if(path=="/login"&&method=="POST"){
  auto username=text(body,"username",3,32);auto &failure=failures_[lower(username)];if(failure.first>=5&&failure.second>now())throw std::runtime_error("Too many attempts. Try again in five minutes.");
  try{
   auto identity=[&](){try{return accounts_.password(username,text(body,"password",8,256),"gm-console");}catch(const std::exception&){throw std::runtime_error("Incorrect account name or password");}}();
   std::string wanted=lower(body.value("character",""));Json staff;int matches=0;
   for(const auto& ch:world_.characters(identity.id)){
    if(!wanted.empty()&&lower(ch.value("name",""))!=wanted)continue;
    try{staff=gm_authority(root_,identity.id,ch.at("eid"));++matches;}catch(const std::exception&){}
   }
   if(matches!=1)throw std::runtime_error("Enter a GM or Admin character belonging to your account.");
   if(!local_account_login(root_)){
   auto rows=credentials_.query("SELECT generation,salt,digest FROM staff_credentials WHERE account=? AND eid=?",{identity.id,staff.at("eid").get<std::int64_t>()});
   auto password=text(body,"staff_password",12,256);
   auto salt=rows.empty()?Bytes(32):unhex(rows[0].at("salt"));auto digest=pbkdf2(password,salt,600000);
   if(rows.empty()||rows[0].at("generation")!=staff.at("generation")||!secure_equal(digest,unhex(rows[0].at("digest"))))throw std::runtime_error("The staff password is incorrect or not registered in-game.");
   }
   auto secret=token();double until=now()+900;
   for(auto it=sessions_.begin();it!=sessions_.end();)if(it->second.until<=now())it=sessions_.erase(it);else ++it;
   sessions_[hex(sha256(secret))]={identity.id,staff.at("generation"),staff.at("eid"),until};failures_.erase(lower(username));
   staff.erase("generation");staff["token"]=secret;staff["expires"]=until;return staff;
  }catch(...){if(failure.second<=now())failure.first=0;failure.first++;failure.second=now()+300;throw;}
 }
 auto staff=authorize(bearer);
 if(path=="/logout"){sessions_.erase(hex(sha256(bearer)));return {{"ok",true}};}
 if(path=="/status")return {{"staff",{{"name",staff.at("name")},{"role",staff.at("role")}}},{"items",item_rows_.size()},{"placements",entity_rows_.size()},{"services",runtime_.query("SELECT instance,updated,payload FROM native_services")},{"pending",world_.db().query("SELECT count(*) AS count FROM gm_actions WHERE status='pending'")[0].at("count")},{"native_mail_ui",true},{"spawn_combat_migration_complete",false}};
 if(path=="/items")return filtered(item_rows_,body);
 if(path=="/equipment"){auto query=body;query["equipment_only"]=true;return filtered(item_rows_,query);}
 if(path=="/equipment/options"){if(body.contains("item_id"))return gm_equipment_options(inventory_.definition(static_cast<std::uint32_t>(number(body,"item_id",1,0xffffffff))));return gm_equipment_options();}
 if(path=="/equipment/details"){
  auto eid=number(body,"eid",1,0x7fffffff),uid=number(body,"uid",1,0xffffffff);int matches=0;Json result;
  for(bool worn:{false,true})for(auto&i:inventory_.items(eid,worn))if(i.uid==uid){auto rule=inventory_.definition(i.item);result={{"uid",i.uid},{"id",i.item},{"name",inventory_.name(i.item)},{"equipped",worn},{"grade",i.grade},{"class",rule.value("class",0)},{"slot",rule.value("slot",0)},{"expires",i.expires?Json(*i.expires):Json(nullptr)},{"schema",gm_equipment_options(rule)},{"affixes",Json::array()}};++matches;}
  if(matches!=1)throw std::runtime_error("Equipment UID does not belong to this character");
  auto exists=world_.db().query("SELECT name FROM sqlite_master WHERE type='table' AND name='item_custom_affixes'");if(!exists.empty())result["affixes"]=world_.db().query("SELECT layer,position,stat,value FROM item_custom_affixes WHERE uid=? ORDER BY layer,position",{uid});return result;
 }
 if(path=="/titles"||path=="/titles/change"){
  auto& db=world_.db();
  const auto eid=number(body,"eid",1,0x7fffffff);
  const auto owners=db.query("SELECT a.id FROM accounts a,json_each(a.payload,'$.chars') c WHERE json_extract(c.value,'$.eid')=? LIMIT 2",{eid});
  if(owners.size()!=1)throw std::runtime_error("Character not found or ambiguous");
  const auto account=owners[0].at("id").get<std::string>();
  const auto actor=staff.at("account").get<std::string>();
  const bool may_change=staff.at("role")=="admin"&&title_owner(root_,actor);
  auto catalog=content_.get("data/title_catalog.json").at("titles");
  if(path=="/titles/change"){
   if(method!="POST"||!may_change)throw std::runtime_error("Only the authenticated owner may grant or revoke titles");
   const auto title=number(body,"title_id",1,0x7fffffff);
   if(std::none_of(catalog.begin(),catalog.end(),[&](const auto& row){return row.at("id")==title;}))throw std::runtime_error("Title not found");
   const auto operation=text(body,"operation",5,6);
   if(operation!="grant"&&operation!="revoke")throw std::runtime_error("Invalid title operation");
   const auto event="gm-title:"+text(body,"request_id",16,80);
   const auto reason=text(body,"reason",0,300);
   change_title(db,event,actor,staff.at("eid"),account,eid,title,operation=="revoke",reason);
  }
  initialize_titles(db);
  auto owned=db.query("SELECT title_id FROM character_titles WHERE account_id=? AND character_eid=?",{account,eid});
  for(auto& row:catalog)row["owned"]=std::any_of(owned.begin(),owned.end(),[&](const auto& entry){return entry.at("title_id")==row.at("id");});
  const auto selection=db.query("SELECT title_id FROM character_title_selection WHERE character_eid=?",{eid});
  return {{"rows",catalog},{"may_change",may_change},{"selected",selection.empty()?Json(nullptr):selection[0].at("title_id")}};
 }
 if(path=="/catalog/cash"||path=="/catalog/gacha"){
  if(staff.at("role")!="admin")throw std::runtime_error("Catalog editing requires Admin");
  auto& db=world_.db();initialize_catalog_edits(db);
  const auto actor=staff.at("account").get<std::string>();
  const auto actor_eid=staff.at("eid").get<std::int64_t>();
  auto audit=[&](const std::string& kind,const Json& before,const Json& after){
   db.exec("INSERT INTO premium_catalog_audit(actor_account,actor_eid,kind,old_value,new_value,created_at) VALUES(?,?,?,?,?,?)",{actor,actor_eid,kind,before.dump(),after.dump(),std::int64_t(now())});
  };
  const auto shop=content_.get("data/cash_shop_local.json");
  if(path=="/catalog/cash"){
   const auto edits=db.query("SELECT offer,price FROM cash_price_edits");
   std::map<int,std::int64_t> prices;for(auto& row:edits)prices[row.at("offer")]=row.at("price");
   if(body.contains("new_price")){
    const auto id=number(body,"offer",1,0x7fffffff),price=number(body,"new_price",1,1000000);
    const auto& offers=shop.at("offers");
    const auto offer=std::find_if(offers.begin(),offers.end(),[&](const auto& row){return row.at("id")==id&&!row.value("unavailable",false)&&row.value("purchase_mode",std::string("direct"))!="boxed";});
    if(offer==offers.end())throw std::runtime_error("Offer unavailable");
    Transaction tx(db);
    auto current=db.query("SELECT price FROM cash_price_edits WHERE offer=?",{id});
    const auto old=current.empty()?offer->at("cash").get<std::int64_t>():current[0].at("price").get<std::int64_t>();
    if(old!=number(body,"old_price",1,1000000))throw std::runtime_error("Price changed. Reload the offer.");
    if(old!=price){
     for(auto sku:{id,offer->value("counterpart",std::int64_t(0))})if(sku)db.exec("INSERT INTO cash_price_edits VALUES(?,?) ON CONFLICT(offer) DO UPDATE SET price=excluded.price",{sku,price});
     audit("cash_price",{{"offer",id},{"price",old}},{{"offer",id},{"price",price}});
    }
    tx.commit();prices[static_cast<int>(id)]=price;
   }
   Json rows=Json::array();for(const auto& offer:shop.at("offers")){
    if(offer.value("unavailable",false)||offer.value("purchase_mode",std::string("direct"))=="boxed")continue;
    int id=offer.at("id");rows.push_back({{"offer",id},{"name",offer.at("title")},{"class",offer.value("class",0)},{"level",offer.value("level",0)},{"price",prices.count(id)?prices.at(id):offer.at("cash").get<std::int64_t>()}});
   }
   return filtered(rows,body);
  }
  const int tab=static_cast<int>(number(body,"tab",1,5));
  const auto gacha=content_.get("data/gacha_local.json");
  if(body.contains("new_item")){
   Transaction tx(db);auto pool=current_gacha_pool(db,gacha,tab);
   const auto position=number(body,"position",0,static_cast<std::int64_t>(pool.size())-1);
   const auto previous=pool.at(position);
   if(previous.at("item")!=body.at("old_item")||previous.at("quantity")!=body.at("old_quantity")||previous.at("weight")!=body.at("old_weight")||previous.at("grade")!=body.at("old_grade"))throw std::runtime_error("Prize changed. Reload the tab.");
   const auto item=number(body,"new_item",1,0xffffffff),quantity=number(body,"quantity",1,10000),weight=number(body,"weight",1,65535),grade=number(body,"grade",1,3);
   if(item==999999||inventory_.definition(static_cast<std::uint32_t>(item)).value("kind",0)==0)throw std::runtime_error("Prize item is not a usable catalog item");
   for(const auto& offer:shop.at("offers"))if(offer.value("unavailable",false))for(const auto& entry:offer.at("items"))if(entry[0]==item)throw std::runtime_error("Prize item is unavailable in the client");
   db.exec("INSERT INTO gacha_prize_edits VALUES(?,?,?,?,?,?) ON CONFLICT(tab,position) DO UPDATE SET item=excluded.item,quantity=excluded.quantity,weight=excluded.weight,grade=excluded.grade",{std::int64_t(tab),position,item,quantity,weight,grade});
   audit("gacha_prize",{{"tab",tab},{"position",position},{"prize",previous}},{{"tab",tab},{"position",position},{"item",item},{"quantity",quantity},{"weight",weight},{"grade",grade}});
   tx.commit();
  }
  auto pool=current_gacha_pool(db,gacha,tab);Json rows=Json::array();
  for(std::size_t i=0;i<pool.size();++i){auto prize=pool[i];prize.erase("native_row");prize["position"]=i;prize["name"]=inventory_.name(prize.at("item"));rows.push_back(std::move(prize));}
  return {{"rows",rows}};
 }
 if(path=="/entities")return filtered(entity_rows_,body);
 if(path=="/players")return {{"rows",players()}};
 if(path=="/history")return {{"rows",world_.db().query("SELECT id,actor_eid,role,kind,target_eid,status,result,created,updated FROM gm_actions ORDER BY created DESC LIMIT 250")}};
 if(path=="/mail")return {{"rows",world_.db().query("SELECT * FROM gm_mail ORDER BY created DESC LIMIT 250")}};
 if(path=="/inventory"){
  auto eid=number(body,"eid",1,0x7fffffff);bool found=false;for(auto& p:players())if(p.at("eid")==eid)found=true;if(!found)throw std::runtime_error("Character not found.");
  Json rows=Json::array();for(bool equipped:{false,true})for(auto& item:inventory_.items(eid,equipped)){auto rule=inventory_.definition(item.item);bool gear=(rule.value("kind",0)==1||rule.value("kind",0)==11)&&rule.value("slot",0)>0;if(body.value("equipment_only",false)&&!gear)continue;rows.push_back({{"id",item.item},{"name",inventory_.name(item.item)},{"quantity",item.quantity},{"slot",item.slot},{"uid",item.uid},{"equipped",equipped},{"grade",item.grade},{"equipment",gear},{"equipment_slot",rule.value("slot",0)},{"class",rule.value("class",0)},{"level",rule.value("level",0)}});}return {{"rows",rows}};
 }
 if(path=="/action"&&method=="POST")return execute(staff,body);
 throw std::runtime_error("Unknown operation.");
}
Json GmController::execute(const Json &staff,const Json& body){
 const auto id=text(body,"request_id",16,80);
 if(id.rfind("consignment-return:",0)==0)throw std::runtime_error("Reserved operation ID.");
 const auto kind=text(body,"kind",3,30);
 const bool delivery=kind=="give"||kind=="mail"||kind=="mail_all";
 const auto reason=delivery&&!body.contains("reason") ? std::string() : text(body,"reason",delivery?0:5,400);
 bool admin=staff.at("role")=="admin";
 if(kind=="mail_all"){
  if(!admin)throw std::runtime_error("Mass mail is restricted to Admin.");
  auto list=players();if(list.size()>10000)throw std::runtime_error("The batch exceeds 10,000 characters.");
  Json results=Json::array();int succeeded=0;
  for(auto &player:list){auto child=body;child["kind"]="mail";child["target_eid"]=player.at("eid");child["request_id"]=id+"-"+std::to_string(player.at("eid").get<std::uint32_t>());
   try{auto result=execute(staff,child);result["target_eid"]=player.at("eid");results.push_back(result);++succeeded;}
   catch(const std::exception&e){results.push_back({{"target_eid",player.at("eid")},{"status","rejected"},{"error",e.what()}});}
  }
  return {{"status","batch_processed"},{"result",std::to_string(succeeded)+" mail records saved out of "+std::to_string(list.size())},{"rows",results}};
 }
 if(kind!="mail"&&kind!="spawn"&&kind!="give"&&kind!="equipment"&&kind!="announce"&&kind!="ban"&&kind!="unban"&&kind!="cash")throw std::runtime_error("Unsupported action.");
 if(!admin&&kind!="mail"&&kind!="spawn")throw std::runtime_error("Admin permission required.");
 auto payload=body;payload["reason"]=reason;
 auto target=kind=="announce"?0:number(body,"target_eid",1,0x7fffffff);
 Json player;
 if(target){for(auto& p:players())if(p.at("eid")==target)player=p;if(player.is_null())throw std::runtime_error("Target character not found.");}
 auto previous=world_.db().query("SELECT * FROM gm_actions WHERE id=?",{id});
 if(!previous.empty()){
  if(previous[0].at("actor_account")!=staff.at("account")||previous[0].at("payload")!=payload.dump())throw std::runtime_error("Operation ID reused with different contents.");
  return {{"id",id},{"status",previous[0].at("status")},{"result",previous[0].at("result")},{"replayed",true}};
 }
 if(kind=="give"||kind=="mail"){
  auto entries=body.value("items",Json::array());if(!entries.is_array()||entries.size()>5||(kind=="give"&&entries.empty()))throw std::runtime_error("Use one to five attachments.");
  for(auto& item:entries){auto iid=number(item,"id",1,0xffffffff),qty=number(item,"quantity",1,admin?100000:10);(void)qty;bool exists=false;for(auto& row:item_rows_)if(row.at("id")==iid)exists=true;if(!exists||iid==999999)throw std::runtime_error("Item is not cataloged or is currency. Use the currency action.");}
  int enchant=body.value("enchant",0);if(enchant<0||enchant>18||(!admin&&enchant))throw std::runtime_error("Enhancement is Admin-only and must be between +0 and +18.");
  if(body.contains("equipment_config")){if(!admin||entries.size()!=1||entries[0].at("quantity")!=1||enchant)throw std::runtime_error("Equipment configuration requires one Admin equipment delivery");gm_equipment_validate(body.at("equipment_config"),inventory_.definition(entries[0].at("id")));}
  if(kind=="mail"){
   auto sender=text(body,"sender",3,40);if(sender!="Servidor"&&sender!="Presente de GM"&&sender!="Eventos"&&sender!="Sistema"&&sender!="Server"&&sender!="GM Gift"&&sender!="Events"&&sender!="System")throw std::runtime_error("Invalid mail sender.");
   if(!admin&&sender!="Presente de GM"&&sender!="GM Gift")throw std::runtime_error("GM may only use the GM Gift sender.");
   text(body,"title",1,100);text(body,"message",1,2000);
  }
 }
 if(kind=="equipment"){
  auto uid=number(body,"uid",1,0xffffffff),iid=number(body,"item_id",1,0xffffffff);int matches=0;for(bool worn:{false,true})for(auto&i:inventory_.items(target,worn))if(i.uid==uid&&i.item==iid&&(!i.expires||*i.expires>now()))++matches;if(matches!=1)throw std::runtime_error("Equipment UID does not belong to the target character");gm_equipment_validate(body.at("equipment_config"),inventory_.definition(static_cast<std::uint32_t>(iid)));
 }
 Entity chosen;
 if(kind=="spawn"){
  auto key=text(body,"placement",1,100);auto found=placements_.find(key);if(found==placements_.end())throw std::runtime_error("Original placement not found.");chosen=found->second;
  auto mode=body.value("mode","near");if(mode!="near"&&mode!="original")throw std::runtime_error("Invalid summon mode.");
  auto count=number(body,"count",1,admin?20:3);(void)count;number(body,"duration",60,3600);number(body,"channel",1,4);
  if(!admin&&(mode!="near"||chosen.category!="normal"||!chosen.combat))throw std::runtime_error("GM may only summon ordinary mobs near players.");
  if(mode=="near"){
   if(!player.at("online").get<bool>()||player.at("channel")!=body.at("channel"))throw std::runtime_error("The target must be online in the selected channel.");
   if(player.at("map")!=chosen.map)throw std::runtime_error("Choose a creature from the target map.");
   chosen.position={player.at("x").get<double>()+60,player.at("y"),player.at("z")};
  }
 }
 if(kind=="announce")text(body,"message",1,110);
 if(kind=="ban")number(body,"minutes",0,525600);
 if(kind=="cash")number(body,"amount",1,10000000);
 Transaction tx(world_.db());
 world_.db().exec("INSERT INTO gm_actions VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",{id,staff.at("account").get<std::string>(),staff.at("eid").get<std::int64_t>(),staff.at("generation").get<std::string>(),staff.at("role").get<std::string>(),kind,target,payload.dump(),std::string("pending"),std::string("Waiting for processing"),now(),now()});
 if(kind=="give"||kind=="mail")
  world_.db().exec("INSERT INTO gm_item_delivery_actors VALUES(?,?,?)",
                   {id,staff.at("name").get<std::string>(),player.at("name").get<std::string>()});
 if(kind=="mail")world_.db().exec("INSERT INTO gm_mail VALUES(?,?,?,?,?,?,?,?)",{id,target,body.at("sender").get<std::string>(),body.at("title").get<std::string>(),body.at("message").get<std::string>(),body.value("items",Json::array()).dump(),std::string("pending"),now()});
 // Commit mailbox and request together before inventory delivery; retries remain idempotent.
 if(kind=="spawn"){
  auto next=world_.db().query("SELECT next FROM gm_spawn_sequence WHERE id=1")[0].at("next").get<int>();auto count=body.at("count").get<int>();
  if(next+count>65000)throw std::runtime_error("The GM actor ID range is exhausted.");
  for(int i=0;i<count;i++){auto e=chosen;e.eid=next+i;e.position.x+=i*30;auto carrier=entities_.appearance(e);
   world_.db().exec("INSERT INTO gm_spawns VALUES(?,?,?,?,?,?,?,?,?,?,?)",{std::int64_t(e.eid),id,std::int64_t(e.npc_id),e.map,body.at("channel").get<std::int64_t>(),e.position.x,e.position.y,e.position.z,hex(carrier),now()+body.at("duration").get<double>(),now()});}
  world_.db().exec("UPDATE gm_spawn_sequence SET next=? WHERE id=1",{std::int64_t(next+count)});
  world_.db().exec("UPDATE gm_actions SET status='spawn_registered',result=? WHERE id=?",{std::string("Spawn registered; WorldServer distributes its appearance; complete combat is still being migrated"),id});
 }
 if(kind=="announce"){
  JobQueue jobs(root_);Json ids=Json::array();for(int c=1;c<=4;c++)ids.push_back(jobs.enqueue("WorldServer:"+std::to_string(c),"announce",{{"text",body.at("message")}}));
  world_.db().exec("UPDATE gm_actions SET status='queued',result=? WHERE id=?",{ids.dump(),id});
 }
 if(kind=="ban"||kind=="unban"){
  auto subject=std::to_string(target);
  if(kind=="ban"){
   auto minutes=body.at("minutes").get<std::int64_t>();
   moderation_.exec("INSERT INTO sanctions(kind,scope,subject,actor_account,actor_eid,reason,created,expires) VALUES('ban','character',?,?,?,?,?,?)",{subject,staff.at("account").get<std::string>(),staff.at("eid").get<std::int64_t>(),reason,now(),minutes?SqlValue(now()+minutes*60):SqlValue(nullptr)});
  }else moderation_.exec("UPDATE sanctions SET revoked=? WHERE kind='ban' AND scope='character' AND subject=? AND revoked IS NULL",{now(),subject});
  world_.db().exec("UPDATE gm_actions SET status='applied',result='Access sanction updated' WHERE id=?",{id});
 }
 if(kind=="cash"){
  world_.db().exec("CREATE TABLE IF NOT EXISTS premium_wallet(account_id TEXT PRIMARY KEY,balance INTEGER NOT NULL CHECK(balance BETWEEN 0 AND 4294967295))");
  std::string targetAccount;for(auto& row:world_.db().query("SELECT id,payload FROM accounts"))for(auto& ch:Json::parse(row.at("payload").get<std::string>()).value("chars",Json::array()))if(ch.value("eid",0u)==target)targetAccount=row.at("id");
  if(targetAccount.empty())throw std::runtime_error("Target account not found.");
  auto amount=body.at("amount").get<std::int64_t>();auto balance=world_.db().query("SELECT balance FROM premium_wallet WHERE account_id=?",{targetAccount});if(!balance.empty()&&balance[0].at("balance").get<std::int64_t>()>1000000000-amount)throw std::runtime_error("Cash balance exceeds the tool limit.");
  world_.db().exec("INSERT INTO premium_wallet VALUES(?,?) ON CONFLICT(account_id) DO UPDATE SET balance=balance+excluded.balance",{targetAccount,amount});
  world_.db().exec("UPDATE gm_actions SET status='applied',result='Cash credited to the persistent wallet' WHERE id=?",{id});
 }
 tx.commit();
 if(kind=="give"||kind=="equipment"){
  // Serialize against WorldSession.claim; never change an online character from this process.
  Transaction lease(runtime_);auto active=runtime_.query("SELECT eid FROM native_player_leases WHERE eid=? AND expires>?",{target,now()});
  if(active.empty()){
   try{auto row=world_.db().query("SELECT * FROM gm_actions WHERE id=?",{id})[0];gm_deliver(world_.db(),inventory_,row);}
   catch(const std::exception&e){if(kind=="equipment")world_.db().exec("UPDATE gm_actions SET status='failed',result=? WHERE id=?",{std::string(e.what()),id});else world_.db().exec("UPDATE gm_actions SET result='Pending: inventory full or delivery unavailable; retry at login' WHERE id=?",{id});}
  }
  lease.commit();
 }
 auto result=world_.db().query("SELECT status,result FROM gm_actions WHERE id=?",{id})[0];result["id"]=id;return result;
}
}
