#include "native/gm.hpp"
#include "native/economy.hpp"
#include <cmath>
#include <ctime>
#include <algorithm>
namespace rosh {
static std::string lower(std::string s){ for(auto &c:s) if(c>='A'&&c<='Z')c+=32; return s; }
void gm_schema(Sqlite &db){
 consignment_schema(db);
 db.exec(R"SQL(
 CREATE TABLE IF NOT EXISTS gm_actions(id TEXT PRIMARY KEY,actor_account TEXT NOT NULL,actor_eid INTEGER NOT NULL,generation TEXT NOT NULL,role TEXT NOT NULL,kind TEXT NOT NULL,target_eid INTEGER NOT NULL,payload TEXT NOT NULL,status TEXT NOT NULL,result TEXT NOT NULL,created REAL NOT NULL,updated REAL NOT NULL);
 CREATE INDEX IF NOT EXISTS gm_actions_target ON gm_actions(target_eid,status);
 CREATE TABLE IF NOT EXISTS gm_item_delivery_actors(action_id TEXT PRIMARY KEY,creator_name TEXT NOT NULL,recipient_name TEXT NOT NULL);
 CREATE TABLE IF NOT EXISTS gm_mail(id TEXT PRIMARY KEY,target_eid INTEGER NOT NULL,sender TEXT NOT NULL,title TEXT NOT NULL,body TEXT NOT NULL,attachments TEXT NOT NULL,status TEXT NOT NULL,created REAL NOT NULL);
 CREATE INDEX IF NOT EXISTS gm_mail_target ON gm_mail(target_eid,created);
 CREATE TABLE IF NOT EXISTS gm_mail_native(native_id INTEGER PRIMARY KEY AUTOINCREMENT,mail_id TEXT UNIQUE NOT NULL,read_at REAL);
 CREATE TABLE IF NOT EXISTS gm_spawns(eid INTEGER PRIMARY KEY,action_id TEXT NOT NULL,npc_id INTEGER NOT NULL,map TEXT NOT NULL,channel INTEGER NOT NULL,x REAL NOT NULL,y REAL NOT NULL,z REAL NOT NULL,carrier TEXT NOT NULL,expires REAL NOT NULL,created REAL NOT NULL);
 CREATE TABLE IF NOT EXISTS gm_spawn_sequence(id INTEGER PRIMARY KEY CHECK(id=1),next INTEGER NOT NULL);
 INSERT OR IGNORE INTO gm_spawn_sequence VALUES(1,60000);
 )SQL");
}
Json gm_authority(const fs::path& root,const std::string& account,std::uint32_t eid){
 Sqlite roles(root/"Database/karos_roles.sqlite",true), game(root/"Database/karos_gameplay.sqlite",true);
 auto rows=roles.query("SELECT role,auth_generation FROM staff_roles WHERE account_id=? AND character_eid=?",{account,std::int64_t(eid)});
 if(rows.empty())throw std::runtime_error("Access is restricted to GM and Admin.");
 auto role=lower(rows[0].at("role"));if(role=="dev")role="admin";
 if(role!="gm"&&role!="admin")throw std::runtime_error("Access is restricted to GM and Admin.");
 auto owned=game.query("SELECT payload FROM accounts WHERE id=?",{account});bool found=false;std::string name;
 if(!owned.empty())for(const auto& ch:Json::parse(owned[0].at("payload").get<std::string>()).value("chars",Json::array()))if(ch.value("eid",0u)==eid){found=true;name=ch.value("name","");}
 auto generation=rows[0].value("auth_generation","");if(!found||generation.empty())throw std::runtime_error("Character ownership or staff authorization was revoked.");
 return {{"account",account},{"eid",eid},{"name",name},{"role",role},{"generation",generation}};
}
void gm_deliver(Sqlite &db,Inventory &inventory,const Json &request){
 auto payload=Json::parse(request.at("payload").get<std::string>());
 std::uint32_t eid=request.at("target_eid");
 Transaction tx(db);
 if(request.at("kind")=="equipment"){
  gm_equipment_apply(db,inventory,eid,payload.at("uid"),payload.at("item_id"),payload.at("equipment_config"));db.exec("UPDATE gm_actions SET status='applied',result='Equipment changes saved',updated=? WHERE id=? AND status='pending'",{now(),request.at("id").get<std::string>()});tx.commit();return;
 }
 std::set<std::uint32_t> before;for(auto &i:inventory.items(eid))before.insert(i.uid);
 std::vector<std::pair<std::uint32_t,int>> entries;
 for(const auto& item:payload.at("items"))entries.emplace_back(item.at("id").get<std::uint32_t>(),item.at("quantity").get<int>());
 if(!entries.empty()){
  auto rule=inventory.definition(entries[0].first);if(payload.contains("equipment_config")&&rule.value("duration",0)>0)inventory.grant_timed_once(eid,entries[0].first,entries[0].second,rule.at("duration"),"gm:"+request.at("id").get<std::string>());else inventory.grant_once(eid,entries,"gm:"+request.at("id").get<std::string>(),true);
 }
 auto actors=db.query("SELECT creator_name,recipient_name FROM gm_item_delivery_actors WHERE action_id=?",
                      {request.at("id").get<std::string>()});
 const auto creator=request.at("actor_eid").get<std::int64_t>();
 for(const auto& item:inventory.items(eid))if(!before.count(item.uid))
  db.exec("INSERT INTO gm_item_origins VALUES(?,?,?,?,?,?,?,?,?,?)",
          {std::int64_t(item.uid),request.at("id").get<std::string>(),std::int64_t(item.item),
           item.quantity,creator,actors.empty()?std::string():actors[0].at("creator_name").get<std::string>(),
           std::int64_t(eid),actors.empty()?std::string():actors[0].at("recipient_name").get<std::string>(),
           request.at("kind").get<std::string>(),now()});
 int enchant=payload.value("enchant",0);
 if(enchant)for(auto &i:inventory.items(eid))if(!before.count(i.uid))db.exec("INSERT INTO item_enchantments VALUES(?,?) ON CONFLICT(uid) DO UPDATE SET grade=excluded.grade",{std::int64_t(i.uid),std::int64_t(enchant)});
 if(payload.contains("equipment_config")){int changed=0;for(auto&i:inventory.items(eid))if(!before.count(i.uid)){gm_equipment_apply(db,inventory,eid,i.uid,i.item,payload.at("equipment_config"));++changed;}if(changed!=1)throw std::runtime_error("Equipment delivery did not create exactly one UID");}
 db.exec("UPDATE gm_actions SET status='delivered',result=?,updated=? WHERE id=? AND status='pending'",{std::string("Delivery saved to inventory"),now(),request.at("id").get<std::string>()});
 db.exec("UPDATE gm_mail SET status='delivered' WHERE id=?",{request.at("id").get<std::string>()});
 tx.commit();
}

void gm_world_poll(const fs::path &root,Sqlite &db,Inventory &inventory,std::uint32_t eid,
 const std::string &map,int channel,const Json &position,std::set<std::uint32_t>& visible,
 const std::function<void(unsigned char,unsigned char,const Bytes&)>&send,
 const std::function<void()>&inventory_changed){
 consignment_expire(db,eid,now());
 auto requests=db.query("SELECT * FROM gm_actions WHERE target_eid=? AND status='pending' AND kind IN ('give','equipment') ORDER BY created LIMIT 10",{std::int64_t(eid)});
 for(auto& request:requests){
  try{
   auto staff=gm_authority(root,request.at("actor_account"),request.at("actor_eid"));
   if(staff.at("generation")!=request.at("generation")||staff.at("role")!=request.at("role"))throw std::runtime_error("Autorizacao staff alterada");
  }catch(const std::exception&){db.exec("UPDATE gm_actions SET status='cancelled',result='Cargo ou autenticacao do autor revogados',updated=? WHERE id=?",{now(),request.at("id").get<std::string>()});continue;}
  try{gm_deliver(db,inventory,request);inventory_changed();
   if(request.at("kind")=="equipment"){send(11,1,notice("Equipment updated."));continue;}
   auto payload=Json::parse(request.at("payload").get<std::string>());auto sender=payload.value("sender","Servidor");
   send(11,1,notice(sender+": entrega recebida no inventario."));
  }catch(const std::exception&e){if(request.at("kind")=="equipment")db.exec("UPDATE gm_actions SET status='failed',result=?,updated=? WHERE id=?",{std::string(e.what()),now(),request.at("id").get<std::string>()});else db.exec("UPDATE gm_actions SET result='Waiting for inventory space',updated=? WHERE id=?",{now(),request.at("id").get<std::string>()});}
 }
 std::set<std::uint32_t> current;
 for(auto& spawn:db.query("SELECT eid,x,y,carrier FROM gm_spawns WHERE map=? AND channel=? AND expires>?",{map,std::int64_t(channel),now()})){
  if(std::hypot(position.at(0).get<double>()-spawn.at("x").get<double>(),position.at(1).get<double>()-spawn.at("y").get<double>())>2500)continue;
  std::uint32_t actor=spawn.at("eid");current.insert(actor);
  if(!visible.count(actor))send(4,2,unhex(spawn.at("carrier")));
 }
 for(auto actor:visible)if(!current.count(actor)){Bytes body(5,1);put32(body,1,actor,true);send(4,3,body);}
 visible=std::move(current);
}
namespace {
constexpr int mail_lifetime_days = 30;
constexpr int mail_record_size = 147;
constexpr int mail_slots = 8;
// AMo allocates room for exactly 12 records in the native list handler.
constexpr std::size_t native_mail_capacity = 12;
constexpr double mail_lifetime = mail_lifetime_days * 86400.;

constexpr auto mail_columns =
    "SELECT m.*, n.native_id, n.read_at, CAST(m.created AS INTEGER) AS stamp "
    "FROM gm_mail m JOIN gm_mail_native n ON n.mail_id=m.id ";
const std::string mail_live =
    "(m.created>? OR (m.status='pending' AND m.id LIKE 'consignment-return:%')) "
    "AND m.status IN ('pending','delivered') ";

void mail_text(Bytes &bytes, const std::string &value, std::size_t limit) {
    auto end = std::min(limit, value.find('\0'));
    end = std::min(end, value.size());
    while (end < value.size() && end > 0 && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80)
        --end;
    bytes.insert(bytes.end(), value.begin(), value.begin() + end);
    bytes.push_back(0);
}

void mail_date(Bytes &bytes, double epoch, bool with_time) {
    const auto stamp = static_cast<std::time_t>(epoch);
    std::tm value{};
    if (gmtime_s(&value, &stamp))
        throw std::runtime_error("Invalid mailbox date");
    const auto offset = bytes.size();
    bytes.resize(offset + (with_time ? 20 : 12));
    put32(bytes, offset, value.tm_year + 1900, true);
    put32(bytes, offset + 4, value.tm_mon + 1, true);
    put32(bytes, offset + 8, value.tm_mday, true);
    if (with_time) {
        put32(bytes, offset + 12, value.tm_hour, true);
        put32(bytes, offset + 16, value.tm_min, true);
    }
}

Bytes mail_key(const Json &mail) {
    Bytes bytes(10);
    put32(bytes, 0, mail.at("stamp"), true);
    put32(bytes, 4, mail.at("native_id"), true);
    put16(bytes, 8, 2, true);
    return bytes;
}

bool mail_has_attachments(const Json &mail) {
    return mail.at("status") == "pending" &&
           !Json::parse(mail.at("attachments").get<std::string>()).empty();
}
} // namespace

Bytes native_mail_list(const std::vector<Json> &mails, double time) {
    Bytes bytes(16);
    put32(bytes, 4, 1, true);
    const auto count=std::min(mails.size(),native_mail_capacity);
    put32(bytes,8,static_cast<std::uint32_t>(count),true);
    for(std::size_t i=0;i<count;++i) {
        const auto &mail=mails[i];
        const auto key = mail_key(mail);
        bytes.insert(bytes.end(), key.begin(), key.begin() + 8);
        mail_text(bytes, mail.at("sender"), 40);
        mail_text(bytes, mail.at("title"), 100);
        double expiry = mail.at("created").get<double>() + mail_lifetime;
        if (expiry<=time && mail.at("status")=="pending" &&
            mail.at("id").get<std::string>().rfind("consignment-return:",0)==0)
            expiry=time+mail_lifetime;
        mail_date(bytes, expiry, false);
        const auto offset = bytes.size();
        bytes.resize(offset + 10);
        put16(bytes, offset, 2, true);
        put16(bytes, offset + 2, mail.at("read_at").is_null() ? 0 : 1, true);
        const auto days = std::clamp(std::ceil((expiry - time) / 86400.), 0., double(mail_lifetime_days));
        put16(bytes, offset + 6, static_cast<std::uint16_t>(days), true);
        put16(bytes, offset + 8, mail_has_attachments(mail) ? 1 : 0, true);
    }
    return bytes;
}

Bytes native_mail_detail(Inventory &inventory, std::uint32_t eid, const Json &mail) {
    Bytes bytes = mail_key(mail);
    mail_text(bytes, mail.at("sender"), 40);
    mail_text(bytes, mail.at("title"), 100);
    mail_text(bytes, mail.at("body"), 2000);
    mail_date(bytes, mail.at("created"), true);
    const auto offset = bytes.size();
    bytes.resize(offset + 6 + mail_record_size * mail_slots);
    if (!mail_has_attachments(mail))
        return bytes;
    const auto entries = Json::parse(mail.at("attachments").get<std::string>());
    if (entries.size() > mail_slots)
        throw std::runtime_error("Too many native mail attachments");
    put16(bytes, offset, static_cast<std::uint16_t>(entries.size()), true);
    for (int slot = 0; slot < mail_slots; ++slot) {
        auto record = bytes.begin() + offset + 6 + mail_record_size * slot;
        if (slot < static_cast<int>(entries.size())) {
            const auto &entry = entries.at(slot);
            ItemInstance item{};
            item.item = entry.at("id");
            item.slot = slot;
            item.uid = entry.value("uid",0xff000000u + static_cast<std::uint32_t>(slot));
            item.created = entry.value("created",mail.at("stamp").get<std::uint32_t>());
            item.grade = entry.value("grade",0);
            item.quantity = 1;
            const auto raw = inventory.update(eid, item);
            std::copy(raw.begin() + 5, raw.end(), record);
            put32(bytes, offset + 6 + mail_record_size * slot + 16, entry.at("quantity"));
        }
        record[0] = record[1] = 0xff;
        record[6] = static_cast<unsigned char>(slot);
    }
    return bytes;
}

std::int64_t gm_mail_latest_unread(Sqlite &db,std::uint32_t eid,double time) {
    return db.query("SELECT COALESCE(MAX(m.rowid),0) AS newest FROM gm_mail m "
                    "LEFT JOIN gm_mail_native n ON n.mail_id=m.id "
                    "WHERE m.target_eid=? AND n.read_at IS NULL AND "+mail_live,
                    {std::int64_t(eid),time-mail_lifetime}).front().at("newest").get<std::int64_t>();
}

std::vector<Json> gm_mail_inbox(Sqlite &db, std::uint32_t eid, double time) {
    Transaction tx(db);
    consignment_expire(db,eid,time);
    const std::vector<SqlValue> args{std::int64_t(eid), time - mail_lifetime};
    db.exec("INSERT INTO gm_mail_native(mail_id) SELECT m.id FROM gm_mail m "
            "WHERE m.target_eid=? AND "+mail_live+
            "AND NOT EXISTS(SELECT 1 FROM gm_mail_native n WHERE n.mail_id=m.id)",
            args);
    auto rows = db.query(std::string(mail_columns) +
        "WHERE m.target_eid=? AND "+mail_live+
        "ORDER BY m.created DESC,n.native_id DESC LIMIT "+std::to_string(native_mail_capacity), args);
    tx.commit();
    return rows;
}

Bytes gm_mail_delete(Sqlite &db,std::uint32_t eid,const Bytes &request,double time) {
    constexpr std::size_t entry_size=10;
    if(request.size()<4)throw std::runtime_error("Pedido de exclusao de correio invalido");
    const auto count=get32(request,0,true);
    if(!count||count>native_mail_capacity||request.size()!=4+entry_size*count)
        throw std::runtime_error("Pedido de exclusao de correio invalido");
    for(std::size_t offset=4;offset<request.size();offset+=entry_size)
        if(get16(request,offset+8,true)!=2)
            throw std::runtime_error("Tipo de mensagem invalido");

    Transaction tx(db);
    Bytes reply(4);
    std::uint32_t deleted=0;
    std::set<std::uint32_t> seen;
    for(std::size_t offset=4;offset<request.size();offset+=entry_size){
        const auto nativeId=get32(request,offset+4,true);
        if(!seen.insert(nativeId).second)continue;
        auto rows=db.query(std::string(mail_columns)+
            "WHERE m.target_eid=? AND n.native_id=? AND CAST(m.created AS INTEGER)=? "
            "AND (m.status='deleted' OR ("+mail_live+"))",
            {std::int64_t(eid),std::int64_t(nativeId),std::int64_t(get32(request,offset,true)),time-mail_lifetime});
        if(rows.empty()||mail_has_attachments(rows[0]))continue;
        const auto id=rows[0].at("id").get<std::string>();
        db.exec("UPDATE gm_mail SET status='deleted' WHERE id=? AND status!='deleted'",{id});
        db.exec("UPDATE gm_actions SET status='cancelled',updated=? WHERE id=? AND status='pending'",{time,id});
        reply.insert(reply.end(),request.begin()+offset,request.begin()+offset+entry_size);
        ++deleted;
    }
    put32(reply,0,deleted,true);
    tx.commit();
    return reply;
}

Json gm_mail_access(const fs::path &root, Sqlite &db, Inventory &inventory,
                    std::uint32_t eid, const Bytes &request, bool claim, double time) {
    if (request.size() != (claim ? 18u : 10u) || get16(request, request.size() - 2, true) != 2 ||
        (claim && std::any_of(request.begin() + 8, request.begin() + 16, [](auto byte) { return byte != 0; })))
        throw std::runtime_error("Pedido de correio invalido");
    Transaction tx(db);
    const std::vector<SqlValue> args{std::int64_t(eid), std::int64_t(get32(request,4,true)),
                                    std::int64_t(get32(request,0,true)), time - mail_lifetime};
    const auto select = std::string(mail_columns) +
        "WHERE m.target_eid=? AND n.native_id=? AND CAST(m.created AS INTEGER)=? "
        "AND "+mail_live;
    auto rows = db.query(select, args);
    if (rows.empty())
        throw std::runtime_error("Mensagem indisponivel");
    const auto id = rows[0].at("id").get<std::string>();
    if (claim && rows[0].at("status") == "pending" && id.rfind("consignment-return:",0)==0) {
        consignment_claim_return(db,inventory,eid,id,time);
    } else if (claim && rows[0].at("status") == "pending") {
        auto actions = db.query("SELECT * FROM gm_actions WHERE id=? AND target_eid=? AND kind='mail' AND status='pending'",
                                {id, std::int64_t(eid)});
        if (actions.empty())
            throw std::runtime_error("Anexo indisponivel");
        const auto &action = actions.front();
        const auto staff = gm_authority(root, action.at("actor_account"), action.at("actor_eid"));
        if (staff.at("generation") != action.at("generation") || staff.at("role") != action.at("role"))
            throw std::runtime_error("Autorizacao do remetente revogada");
        gm_deliver(db, inventory, action);
    }
    db.exec("UPDATE gm_mail_native SET read_at=COALESCE(read_at,?) WHERE mail_id=?", {time, id});
    auto result = db.query(select, args).front();
    tx.commit();
    return result;
}
}
