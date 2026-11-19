#include "native/inventory.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace rosh {
namespace {std::string visual_key(const Json&stats){Json normalized=Json::array();for(auto&line:stats)normalized.push_back(Json::array({line[0].get<std::string>(),line[1].get<double>()}));return normalized.dump();}}
Inventory::Inventory(Sqlite &db, Content &content)
    : db_(db), rules_(content.get("data/native_item_rules.json")),
      records_(content.get("server/inventory_record_templates.json")),
      templates_(content.get("server/inventory_templates.json")) {
    Json visual=Json::object();if(content.has("data/native_affix_visual_ids.json")){visual=content.get("data/native_affix_visual_ids.json");for(auto&entry:visual.at("entries"))visual_lookup_[visual_key(entry.at("stats"))]=entry.at("id");}
    if(content.has("server/equipment_rng.json")){auto policy=content.get("server/equipment_rng.json");if(visual.value("rng_policy_sha256",std::string())!=hex(sha256(policy.dump())))throw std::runtime_error("RNG e metadados visuais do client nao correspondem a mesma release");affixes_=std::make_shared<EquipmentRng>(std::move(policy));}
    db_.exec(
        "CREATE TABLE IF NOT EXISTS inventory_slots(player_eid INTEGER NOT NULL,slot INTEGER NOT NULL "
        "CHECK(slot BETWEEN 0 AND 1023),item_id INTEGER NOT NULL,quantity INTEGER NOT NULL "
        "CHECK(quantity>0),instance_uid INTEGER,instance_created_at INTEGER,PRIMARY "
        "KEY(player_eid,slot));CREATE TABLE IF NOT EXISTS equipment_instances(player_eid INTEGER,slot "
        "INTEGER,item_id INTEGER,instance_uid INTEGER,created_at INTEGER,PRIMARY "
        "KEY(player_eid,slot));CREATE TABLE IF NOT EXISTS inventory_uid_seq(id INTEGER PRIMARY KEY "
        "AUTOINCREMENT);CREATE TABLE IF NOT EXISTS inventory_grants(event_key TEXT PRIMARY KEY,player_eid "
        "INTEGER NOT NULL);CREATE TABLE IF NOT EXISTS native_inventory_grants(event_key TEXT PRIMARY KEY,eid "
        "INTEGER NOT NULL,payload TEXT NOT NULL);CREATE TABLE IF NOT EXISTS reward_instances(uid INTEGER "
        "PRIMARY KEY,player_eid INTEGER NOT NULL,item_id INTEGER NOT NULL,expires_at INTEGER NOT NULL,source "
        "TEXT NOT NULL);CREATE TABLE IF NOT EXISTS item_enchantments(uid INTEGER PRIMARY KEY,grade INTEGER "
        "NOT NULL CHECK(grade BETWEEN 0 AND 127))");
    db_.exec("CREATE TABLE IF NOT EXISTS gm_item_origins(uid INTEGER PRIMARY KEY,action_id TEXT NOT NULL,item_id INTEGER NOT NULL,quantity INTEGER NOT NULL,creator_eid INTEGER NOT NULL,creator_name TEXT NOT NULL,recipient_eid INTEGER NOT NULL,recipient_name TEXT NOT NULL,method TEXT NOT NULL,created REAL NOT NULL)");
    db_.exec("CREATE TABLE IF NOT EXISTS gm_item_history(sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
             "uid INTEGER NOT NULL,operation TEXT NOT NULL,holder_eid INTEGER NOT NULL,quantity INTEGER NOT NULL,"
             "source_slot INTEGER NOT NULL,destination_slot INTEGER NOT NULL,occurred REAL NOT NULL);"
             "CREATE INDEX IF NOT EXISTS gm_item_history_uid ON gm_item_history(uid,sequence);"
             "CREATE TABLE IF NOT EXISTS gm_item_lineage(uid INTEGER PRIMARY KEY,parent_uid INTEGER NOT NULL,"
             "item_id INTEGER NOT NULL,created REAL NOT NULL);"
             "CREATE INDEX IF NOT EXISTS gm_item_lineage_parent ON gm_item_lineage(parent_uid)");
    db_.exec("CREATE TABLE IF NOT EXISTS native_expired_items(uid INTEGER PRIMARY KEY,player_eid INTEGER NOT NULL,slot INTEGER NOT NULL,equipped INTEGER NOT NULL,item_id INTEGER NOT NULL,quantity INTEGER NOT NULL,created INTEGER NOT NULL,expires_at INTEGER NOT NULL,archived_at REAL NOT NULL)");
    db_.exec("CREATE TABLE IF NOT EXISTS personal_storage("
             "owner_key TEXT NOT NULL,slot INTEGER NOT NULL CHECK(slot BETWEEN 0 AND 55),"
             "item_id INTEGER NOT NULL,quantity INTEGER NOT NULL CHECK(quantity>0),"
             "uid INTEGER NOT NULL UNIQUE,created INTEGER NOT NULL,deposited_by INTEGER NOT NULL,"
             "PRIMARY KEY(owner_key,slot))");
    db_.exec("CREATE TABLE IF NOT EXISTS native_guilds(id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "name TEXT NOT NULL UNIQUE COLLATE NOCASE);"
             "CREATE TABLE IF NOT EXISTS native_guild_members(eid INTEGER PRIMARY KEY,"
             "account_id TEXT NOT NULL,guild_id INTEGER NOT NULL,rank INTEGER NOT NULL CHECK(rank BETWEEN 0 AND 5));"
             "CREATE INDEX IF NOT EXISTS native_guild_members_guild ON native_guild_members(guild_id);"
             "CREATE UNIQUE INDEX IF NOT EXISTS native_guild_single_master ON native_guild_members(guild_id) WHERE rank=5;"
             "CREATE TABLE IF NOT EXISTS native_guild_invites(token TEXT PRIMARY KEY,guild_id INTEGER NOT NULL,"
             "inviter INTEGER NOT NULL,target INTEGER NOT NULL,target_account TEXT NOT NULL,expires REAL NOT NULL,"
             "UNIQUE(guild_id,target));"
             "CREATE INDEX IF NOT EXISTS native_guild_invites_target ON native_guild_invites(target,expires);"
             "CREATE TABLE IF NOT EXISTS native_guild_profiles(guild_id INTEGER PRIMARY KEY,notice TEXT NOT NULL,"
             "rank_names TEXT NOT NULL);"
             "CREATE TABLE IF NOT EXISTS native_guild_storage_rights(guild_id INTEGER NOT NULL,"
             "rank INTEGER NOT NULL CHECK(rank BETWEEN 0 AND 4),rights INTEGER NOT NULL CHECK(rights BETWEEN 0 AND 7),"
             "PRIMARY KEY(guild_id,rank))");
    db_.exec("CREATE TABLE IF NOT EXISTS native_guild_carats_rights(guild_id INTEGER NOT NULL,"
             "rank INTEGER NOT NULL CHECK(rank BETWEEN 0 AND 4),rights INTEGER NOT NULL CHECK(rights BETWEEN 0 AND 3),"
             "PRIMARY KEY(guild_id,rank));"
             "CREATE TABLE IF NOT EXISTS storage_carats(owner_key TEXT PRIMARY KEY,amount INTEGER NOT NULL CHECK(amount>=0),"
             "slot INTEGER NOT NULL,uid INTEGER NOT NULL UNIQUE,created INTEGER NOT NULL);"
             "CREATE TABLE IF NOT EXISTS storage_carats_events(event_key TEXT PRIMARY KEY,owner_key TEXT NOT NULL,"
             "eid INTEGER NOT NULL,delta INTEGER NOT NULL);"
             "CREATE TABLE IF NOT EXISTS inventory_operations(event_key TEXT PRIMARY KEY,eid INTEGER NOT NULL,"
             "kind TEXT NOT NULL,request TEXT NOT NULL,result TEXT NOT NULL)");
    db_.exec("CREATE TABLE IF NOT EXISTS native_guild_invite_wire(token TEXT PRIMARY KEY);"
             "CREATE TABLE IF NOT EXISTS native_guild_invite_results(id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "event_key TEXT NOT NULL UNIQUE,inviter INTEGER NOT NULL,target INTEGER NOT NULL,"
             "inviter_account TEXT NOT NULL,target_account TEXT NOT NULL,status INTEGER NOT NULL,"
             "guild_id INTEGER NOT NULL,guild_name TEXT NOT NULL,inviter_name TEXT NOT NULL,target_name TEXT NOT NULL,"
             "created REAL NOT NULL);"
             "CREATE INDEX IF NOT EXISTS native_guild_invite_result_time ON native_guild_invite_results(created)");
    db_.exec("CREATE TABLE IF NOT EXISTS native_guild_departures(id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "event_key TEXT NOT NULL UNIQUE,account TEXT NOT NULL,actor INTEGER NOT NULL,target INTEGER NOT NULL,"
             "guild_id INTEGER NOT NULL,created REAL NOT NULL);"
             "CREATE TABLE IF NOT EXISTS native_guild_departure_recipients(event_id INTEGER NOT NULL,"
             "eid INTEGER NOT NULL,account TEXT NOT NULL,PRIMARY KEY(event_id,eid));"
             "CREATE INDEX IF NOT EXISTS native_guild_departure_recipient ON native_guild_departure_recipients(eid,event_id);"
             "CREATE INDEX IF NOT EXISTS native_guild_departure_time ON native_guild_departures(created)");
    db_.exec("CREATE TABLE IF NOT EXISTS native_guild_chat(id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "event_key TEXT NOT NULL UNIQUE,account TEXT NOT NULL,eid INTEGER NOT NULL,guild_id INTEGER NOT NULL,"
             "sender TEXT NOT NULL,message TEXT NOT NULL,created REAL NOT NULL);"
             "CREATE TABLE IF NOT EXISTS native_guild_chat_recipients(message_id INTEGER NOT NULL,"
             "eid INTEGER NOT NULL,account TEXT NOT NULL,PRIMARY KEY(message_id,eid));"
             "CREATE INDEX IF NOT EXISTS native_guild_chat_recipient ON native_guild_chat_recipients(eid,message_id)");
    db_.exec("CREATE INDEX IF NOT EXISTS native_guild_chat_time ON native_guild_chat(created)");
    for (const auto &row : content.get("data/item_catalog_verified.json"))
        names_[row.at("id")] = row;
    auto overrides = content.get("data/reward_tooltips.json");
    for (auto row = overrides.begin(); row != overrides.end(); ++row) {
        auto id = static_cast<std::uint32_t>(std::stoul(row.key()));
        if (names_.count(id)) {
            names_[id]["name"] = row.value()[0];
            names_[id]["description"] = row.value()[1];
        }
    }
}
int Inventory::key(int category, int slot) {
    if (category < 1 || category > 4 || slot < 0 || slot >= (category == 1 ? 20 : 48))
        throw std::runtime_error("Invalid native bag address");
    return (category - 1) * 256 + slot;
}
std::pair<int, int> Inventory::address(int value) {
    if (value < 0)
        throw std::runtime_error("Invalid native inventory address");
    int category = value / 256 + 1, slot = value % 256;
    key(category, slot);
    return {category, slot};
}
std::set<int> Inventory::available(std::uint32_t eid, double timestamp) {
    std::set<int> result;
    for (int i = 0; i < 20; i++)
        result.insert(i);
    for (const auto &row :
         db_.query("SELECT e.slot,e.item_id FROM equipment_instances e LEFT JOIN reward_instances r ON "
                   "r.uid=e.instance_uid WHERE e.player_eid=? AND e.slot BETWEEN 13 AND 15 AND (r.uid IS "
                   "NULL OR (r.player_eid=e.player_eid AND r.expires_at>?))",
                   {std::int64_t(eid), timestamp})) {
        auto id = std::to_string(row.at("item_id").get<std::uint32_t>());
        int capacity = rules_.value(id, Json::object()).value("capacity", 0);
        for (int i = 0; i < std::clamp(capacity, 0, 48); i++)
            result.insert(key(row.at("slot").get<int>() - 11, i));
    }
    return result;
}
std::uint32_t Inventory::next_uid() {
    for (;;) {
        db_.exec("INSERT INTO inventory_uid_seq DEFAULT VALUES");
        auto value =
            db_.query("SELECT last_insert_rowid() AS uid")[0].at("uid").get<std::uint64_t>() + 0x01000000;
        if (value > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("Item instance UID capacity exhausted");
        auto rows = db_.query("SELECT 1 FROM inventory_slots WHERE instance_uid=? UNION ALL SELECT 1 FROM "
                              "equipment_instances WHERE instance_uid=? UNION ALL SELECT 1 FROM "
                              "personal_storage WHERE uid=? UNION ALL SELECT 1 FROM storage_carats WHERE uid=? LIMIT 1",
                              {std::int64_t(value),std::int64_t(value),std::int64_t(value),std::int64_t(value)});
        if (rows.empty())
            return static_cast<std::uint32_t>(value);
    }
}
std::vector<ItemInstance> Inventory::items(std::uint32_t eid, bool equipped) {
    auto sql = equipped ? "SELECT e.slot,e.item_id,1 AS quantity,e.instance_uid,e.created_at AS "
                          "created,r.expires_at,COALESCE(g.grade,0) AS grade FROM equipment_instances e LEFT "
                          "JOIN reward_instances r ON r.uid=e.instance_uid LEFT JOIN item_enchantments g ON "
                          "g.uid=e.instance_uid WHERE e.player_eid=? ORDER BY e.slot"
                        : "SELECT e.slot,e.item_id,e.quantity,e.instance_uid,e.instance_created_at AS "
                          "created,r.expires_at,COALESCE(g.grade,0) AS grade FROM inventory_slots e LEFT "
                          "JOIN reward_instances r ON r.uid=e.instance_uid LEFT JOIN item_enchantments g ON "
                          "g.uid=e.instance_uid WHERE e.player_eid=? ORDER BY e.slot";
    std::vector<ItemInstance> result;
    for (const auto &row : db_.query(sql, {std::int64_t(eid)})) {
        ItemInstance item;
        item.slot = row.at("slot");
        item.item = row.at("item_id");
        item.quantity = row.at("quantity");
        item.uid = row.at("instance_uid");
        item.created = row.at("created");
        item.grade = row.at("grade");
        if (!row.at("expires_at").is_null())
            item.expires = row.at("expires_at");
        result.push_back(item);
    }
    return result;
}
ItemInstance Inventory::owned(std::uint32_t eid, int slot, std::uint32_t item, double timestamp) {
    address(slot);
    if (!available(eid, timestamp).count(slot))
        throw std::runtime_error("Bag is unavailable or expired");
    for (const auto &row : items(eid))
        if (row.slot == slot && row.item == item) {
            if (row.expires && *row.expires <= timestamp)
                throw std::runtime_error("Item instance expired");
            return row;
        }
    throw std::runtime_error("Item does not belong to this character and bag slot");
}
bool Inventory::expire(std::uint32_t eid, double timestamp) {
    Transaction tx(db_);
    bool changed = false;
    for (bool equipped : {false, true})
        for (const auto &item : items(eid, equipped)) {
            if (!item.expires || *item.expires > timestamp) continue;
            // Preserve the expired instance and its original deadline for audit/recovery.
            // Contents of an expired bag are not expired by association.
            db_.exec("INSERT INTO native_expired_items VALUES(?,?,?,?,?,?,?,?,?)",
                     {std::int64_t(item.uid),std::int64_t(eid),std::int64_t(item.slot),std::int64_t(equipped),std::int64_t(item.item),item.quantity,std::int64_t(item.created),std::int64_t(*item.expires),timestamp});
            db_.exec(std::string("DELETE FROM ")+(equipped?"equipment_instances":"inventory_slots")+" WHERE player_eid=? AND slot=? AND instance_uid=?",
                     {std::int64_t(eid),std::int64_t(item.slot),std::int64_t(item.uid)});
            if (db_.changes()!=1) throw std::runtime_error("Expired item changed during archival");
            trace_item(item.uid,equipped?"equipment-expiry":"expiry",eid,item.quantity,item.slot,-1);
            changed=true;
        }
    tx.commit();
    return changed;
}
int Inventory::insert(std::uint32_t eid, std::uint32_t iid, int quantity, std::optional<int> preferred,
                      std::optional<std::uint32_t> uid, std::optional<std::uint32_t> created, bool separate) {
    auto rule = rules_.value(std::to_string(iid), Json::object());
    if (!iid || iid == 999999 || rule.empty() || quantity < 1 || quantity > 10000)
        throw std::runtime_error("Invalid native item grant");
    int limit = std::clamp(rule.value("stack", 1), 1, 10000), remaining = quantity, first = -1;
    auto timestamp = static_cast<std::uint32_t>(now());
    expire(eid, timestamp);
    auto slots = available(eid, timestamp);
    auto rows = items(eid);
    std::set<int> used;
    for (const auto &row : rows)
        used.insert(row.slot);
    if (preferred && (!slots.count(*preferred) || used.count(*preferred)))
        throw std::runtime_error("Destination bag slot is unavailable");
    if (uid) {
        auto existing = db_.query("SELECT 1 FROM inventory_slots WHERE instance_uid=? UNION ALL SELECT 1 "
                                  "FROM equipment_instances WHERE instance_uid=? LIMIT 1",
                                  {std::int64_t(*uid), std::int64_t(*uid)});
        if (!existing.empty())
            throw std::runtime_error("Instance UID is already owned in another slot");
        if (quantity != 1)
            throw std::runtime_error("Instance transfer must contain one item");
    }
    if (limit > 1 && !uid && !preferred && !separate)
        for (const auto &row : rows)
            if (slots.count(row.slot) && row.item == iid && row.quantity < limit && !row.expires &&
                !traced(row.uid)) {
                int amount = std::min(remaining, limit - static_cast<int>(row.quantity));
                db_.exec("UPDATE inventory_slots SET quantity=quantity+? WHERE player_eid=? AND slot=?",
                         {std::int64_t(amount), std::int64_t(eid), std::int64_t(row.slot)});
                if (first < 0)
                    first = row.slot;
                remaining -= amount;
                if (!remaining)
                    return first;
            }
    while (remaining) {
        auto free = std::find_if(slots.begin(), slots.end(), [&](int slot) { return !used.count(slot); });
        int slot = preferred ? *preferred : free == slots.end() ? -1 : *free;
        if (slot < 0)
            throw InventoryFull();
        bool transferred=uid.has_value();
        auto instance = uid ? *uid : next_uid();
        uid.reset();
        int count = std::min(remaining, limit);
        db_.exec("INSERT INTO inventory_slots VALUES(?,?,?,?,?,?)",
                 {std::int64_t(eid), std::int64_t(slot), std::int64_t(iid), std::int64_t(count),
                  std::int64_t(instance), std::int64_t(created.value_or(timestamp))});
        // A newly granted native rental starts now, not at a captured template's date.
        // Transfers keep their UID and existing deadline; they must never renew it.
        auto duration=rule.value("duration",0);
        if(!transferred&&duration>0)db_.exec("INSERT INTO reward_instances VALUES(?,?,?,?,?)",{std::int64_t(instance),std::int64_t(eid),std::int64_t(iid),std::int64_t(timestamp)+duration,std::string("native-rental")});
        if(!transferred)initial_affixes(instance,iid);
        used.insert(slot);
        preferred.reset();
        remaining -= count;
        if (first < 0)
            first = slot;
    }
    return first;
}
bool Inventory::grant_once(std::uint32_t eid, const std::vector<std::pair<std::uint32_t, int>> &entries,
                           const std::string &event, bool separate) {
    if (event.empty() || event.size() > 200 || entries.empty())
        throw std::runtime_error("Invalid inventory grant event");
    auto payload = Json(entries).dump();
    Transaction tx(db_);
    auto prior = db_.query("SELECT eid,payload FROM native_inventory_grants WHERE event_key=?", {event});
    if (!prior.empty()) {
        if (prior[0].at("eid") != eid || prior[0].at("payload") != payload)
            throw std::runtime_error("Inventory grant event reused with different contents");
        tx.commit();
        return false;
    }
    auto old = db_.query("SELECT player_eid FROM inventory_grants WHERE event_key=?", {event});
    if (!old.empty()) {
        if (old[0].at("player_eid") != eid)
            throw std::runtime_error("Inventory grant belongs to a different character");
        tx.commit();
        return false;
    }
    for (const auto &[iid, quantity] : entries)
        insert(eid, iid, quantity, {}, {}, {}, separate);
    db_.exec("INSERT INTO inventory_grants VALUES(?,?)", {event, std::int64_t(eid)});
    db_.exec("INSERT INTO native_inventory_grants VALUES(?,?,?)", {event, std::int64_t(eid), payload});
    tx.commit();
    return true;
}
bool Inventory::grant_timed_once(std::uint32_t eid,std::uint32_t iid,int quantity,int seconds,const std::string& event){
    if(quantity<1||quantity>10000||seconds<=0||seconds>315360000||event.empty()||event.size()>200)throw std::runtime_error("Invalid timed grant");
    Transaction tx(db_);auto payload=Json{{"item",iid},{"quantity",quantity},{"seconds",seconds}}.dump();
    auto prior=db_.query("SELECT eid,payload FROM native_inventory_grants WHERE event_key=?",{event});
    if(!prior.empty()){if(prior[0].at("eid")!=eid||prior[0].at("payload")!=payload)throw std::runtime_error("Timed grant replay changed");tx.commit();return false;}
    auto created=static_cast<std::uint32_t>(now());
    for(int i=0;i<quantity;i++){auto uid=next_uid();insert(eid,iid,1,{},uid,created);db_.exec("INSERT INTO reward_instances VALUES(?,?,?,?,?)",{std::int64_t(uid),std::int64_t(eid),std::int64_t(iid),std::int64_t(created)+seconds,std::string("npc-purchase")});}
    db_.exec("INSERT INTO inventory_grants VALUES(?,?)",{event,std::int64_t(eid)});db_.exec("INSERT INTO native_inventory_grants VALUES(?,?,?)",{event,std::int64_t(eid),payload});tx.commit();return true;
}
void Inventory::trace_item(std::uint32_t uid,const std::string& operation,std::uint32_t holder,
                           std::int64_t quantity,int source_slot,int destination_slot){
    if(!traced(uid))return;
    db_.exec("INSERT INTO gm_item_history(uid,operation,holder_eid,quantity,source_slot,destination_slot,occurred) "
             "VALUES(?,?,?,?,?,?,?)",
             {std::int64_t(uid),operation,std::int64_t(holder),quantity,std::int64_t(source_slot),std::int64_t(destination_slot),now()});
}
bool Inventory::traced(std::uint32_t uid){
    const auto key=std::int64_t(uid);
    return !db_.query("SELECT 1 FROM gm_item_origins WHERE uid=? UNION ALL "
                      "SELECT 1 FROM gm_item_lineage WHERE uid=? LIMIT 1",{key,key}).empty();
}
void Inventory::trace_derivation(std::uint32_t parent,const ItemInstance& child,std::uint32_t owner){
    if(!traced(parent))return;
    if(parent==child.uid||traced(child.uid))throw std::runtime_error("Invalid item trace lineage");
    db_.exec("INSERT INTO gm_item_lineage VALUES(?,?,?,?)",
             {std::int64_t(child.uid),std::int64_t(parent),std::int64_t(child.item),now()});
    trace_item(child.uid,"derived",owner,child.quantity,-1,child.slot);
}
void Inventory::consume(std::uint32_t eid, const ItemInstance &item, std::int64_t count,const std::string& reason) {
    Transaction tx(db_);
    if (count < 1 || count > item.quantity)
        throw std::runtime_error("Invalid consume quantity");
    if (count == item.quantity) {
        db_.exec("DELETE FROM inventory_slots WHERE player_eid=? AND slot=? AND item_id=? AND instance_uid=? "
                 "AND quantity=?",
                 {std::int64_t(eid), std::int64_t(item.slot), std::int64_t(item.item), std::int64_t(item.uid),
                  std::int64_t(count)});
        if (db_.changes() != 1)
            throw std::runtime_error("Consumed item instance changed");
        db_.exec("DELETE FROM reward_instances WHERE uid=? AND player_eid=?",
                 {std::int64_t(item.uid), std::int64_t(eid)});
    } else {
        db_.exec("UPDATE inventory_slots SET quantity=quantity-? WHERE player_eid=? AND slot=? AND item_id=? "
                 "AND instance_uid=? AND quantity>=?",
                 {std::int64_t(count), std::int64_t(eid), std::int64_t(item.slot), std::int64_t(item.item),
                  std::int64_t(item.uid), std::int64_t(count)});
        if (db_.changes() != 1)
            throw std::runtime_error("Consumed item stack changed");
    }
    trace_item(item.uid,reason,eid,count,item.slot,count==item.quantity?-1:item.slot);
    tx.commit();
}
int Inventory::equip(std::uint32_t eid, int slot, std::uint32_t iid, int profession, int level, int requested,
                     double timestamp) {
    Transaction tx(db_);
    auto item = owned(eid, slot, iid, timestamp);
    // Selector items use legacy wearable templates in the client, but are never equipment.
    if ((iid >= 180148 && iid < 180176) || (iid >= 1000900000 && iid < 1000900028))
        throw std::runtime_error("Abra a caixa para escolher a peca do traje Cash");
    auto rule = rules_.value(std::to_string(iid), Json::object());
    int target = rule.value("slot", 0);
    if (!((rule.value("kind", 0) == 1) || (rule.value("kind", 0) == 11 && rule.value("subtype", 0) == 39)) ||
        target < 1 || target > 30 || item.quantity != 1)
        throw std::runtime_error("Item is not native wearable equipment");
    int family = rule.value("class", 0);
    if (family && (family / 100 != profession / 100 ||
                   (family % 100 && family != profession && family / 10 != profession / 10)))
        throw std::runtime_error("Equipment does not belong to this class/profession");
    if (level < rule.value("level", 0) && iid != 113002 && iid != 112002 && iid != 48029)
        throw std::runtime_error("Equipment level requirement");
    auto equipment = items(eid, true);
    if (target == 13 && rule.value("capacity", 0) > 0) {
        if (requested >= 13 && requested <= 15)
            target = requested;
        else
            for (int candidate = 13; candidate <= 15; candidate++)
                if (std::none_of(equipment.begin(), equipment.end(),
                                 [&](const auto &old) { return old.slot == candidate; })) {
                    target = candidate;
                    break;
                }
    }
    if (target == 22 || target == 24) {
        if (requested == target || requested == target + 1)
            target = requested;
        else if (std::any_of(equipment.begin(), equipment.end(),
                             [&](const ItemInstance &old) { return old.slot == target; }))
            target++;
    }
    if (target >= 13 && target <= 15) {
        int capacity = std::clamp(rule.value("capacity", 0), 0, 48);
        for (const auto &row : items(eid)) {
            auto [category, index] = address(row.slot);
            if (row.slot != slot && category == target - 11 && index >= capacity)
                throw std::runtime_error("Changing this bag would hide items; empty the extra slots first");
        }
    }
    db_.exec("DELETE FROM inventory_slots WHERE player_eid=? AND slot=? AND instance_uid=?",
             {std::int64_t(eid), std::int64_t(slot), std::int64_t(item.uid)});
    std::optional<ItemInstance> swapped;
    for (const auto &old : equipment)
        if (old.slot == target) {
            db_.exec("DELETE FROM equipment_instances WHERE player_eid=? AND slot=?",
                     {std::int64_t(eid), std::int64_t(target)});
            swapped = old;
            break;
        }
    db_.exec("INSERT INTO equipment_instances VALUES(?,?,?,?,?)",
             {std::int64_t(eid), std::int64_t(target), std::int64_t(iid), std::int64_t(item.uid),
              std::int64_t(item.created)});
    trace_item(item.uid,"equip",eid,1,slot,target);
    if (swapped) {
        auto slots = available(eid, timestamp);
        const int destination=insert(eid, swapped->item, 1, slots.count(slot) ? std::optional<int>(slot) : std::nullopt,
                                     swapped->uid, swapped->created);
        trace_item(swapped->uid,"unequip",eid,1,target,destination);
    }
    tx.commit();
    return target;
}
int Inventory::unequip(std::uint32_t eid, int slot, std::uint32_t iid, std::optional<int> destination,
                       double timestamp) {
    if (slot < 1 || slot > 30)
        throw std::runtime_error("Invalid native equipment slot");
    Transaction tx(db_);
    auto rows = items(eid, true);
    auto found = std::find_if(rows.begin(), rows.end(),
                              [&](const auto &row) { return row.slot == slot && row.item == iid; });
    if (found == rows.end() || (found->expires && *found->expires <= timestamp))
        throw std::runtime_error("Equipment not owned or expired");
    if (slot >= 13 && slot <= 15)
        for (const auto &row : items(eid))
            if (address(row.slot).first == slot - 11)
                throw std::runtime_error("Empty this bag before removing it");
    db_.exec("DELETE FROM equipment_instances WHERE player_eid=? AND slot=?",
             {std::int64_t(eid), std::int64_t(slot)});
    int placed = insert(eid, iid, 1, destination, found->uid, found->created);
    trace_item(found->uid,"unequip",eid,1,slot,placed);
    tx.commit();
    return placed;
}
void Inventory::visual_attributes(Bytes&bytes,const ItemInstance&item){
 if(visual_lookup_.empty())return;
 auto exists=db_.query("SELECT 1 FROM sqlite_master WHERE type='table' AND name='item_custom_affixes'");if(exists.empty())return;
 auto rows=db_.query("SELECT layer,stat,value FROM item_custom_affixes WHERE uid=? ORDER BY layer,position",{std::int64_t(item.uid)});if(rows.empty())return;
 std::array<Json,2>layers={Json::array(),Json::array()};for(auto&r:rows){auto layer=r.at("layer").get<std::string>();layers[layer=="common"||layer=="starter"?0:1].push_back(Json::array({r.at("stat"),r.at("value")}));}
 auto lookup=[&](const Json&stats)->std::uint16_t{auto found=visual_lookup_.find(visual_key(stats));return found==visual_lookup_.end()?0:found->second;};
 for(int layer=0;layer<2;layer++){
  std::vector<std::uint16_t>ids;Json chunk=Json::array();int effects=0;
  auto flush=[&]{if(chunk.empty())return;auto id=lookup(chunk);if(id)ids.push_back(id);else for(auto&row:chunk){auto one=lookup(Json::array({row}));if(!one)throw std::runtime_error("Atributo sem metadado visual autorizado no client");ids.push_back(one);}chunk=Json::array();effects=0;};
  for(auto&row:layers[layer]){int count=row[0]=="attack_power"?2:1;if(effects+count>5)flush();chunk.push_back(row);effects+=count;}flush();
  if(ids.size()>5){
   const auto&rows=layers[layer];if(rows.size()>10)throw std::runtime_error("Camada de atributos acima do limite visual");
   using Cover=std::optional<std::vector<std::uint16_t>>;std::map<unsigned,Cover>memo;unsigned full=(1u<<rows.size())-1;
   std::function<Cover(unsigned)>cover=[&](unsigned mask)->Cover{
    if(mask==full)return std::vector<std::uint16_t>{};if(memo.count(mask))return memo.at(mask);
    unsigned first=0;while(mask&(1u<<first))++first;Cover best;
    auto admit=[&](std::uint16_t id,unsigned next){if(!id)return;auto tail=cover(next);if(!tail||tail->size()>=5)return;tail->insert(tail->begin(),id);if(!best||tail->size()<best->size())best=std::move(tail);};
    for(unsigned other=first+1;other<rows.size();other++)if(!(mask&(1u<<other))){Json pair=Json::array({rows[first],rows[other]});std::sort(pair.begin(),pair.end(),[](const Json&a,const Json&b){return a[0]==b[0]?a[1].get<double>()<b[1].get<double>():a[0].get<std::string>()<b[0].get<std::string>();});admit(lookup(pair),mask|(1u<<first)|(1u<<other));}
    admit(lookup(Json::array({rows[first]})),mask|(1u<<first));memo[mask]=best;return best;
   };
   auto result=cover(0);if(!result)throw std::runtime_error("Combinacao sem cobertura completa nos metadados visuais autorizados");ids=std::move(*result);
  }
  if(ids.size()>5)throw std::runtime_error("Combinacao de atributos excede as opcoes visuais autorizadas do client");
  for(int i=0;i<5;i++)put16(bytes,130+10*layer+2*i,i<static_cast<int>(ids.size())?ids[i]:0);
 }
}
Bytes Inventory::update(std::uint32_t eid, const ItemInstance &item, bool equipped, bool create) {
    auto bytes = unhex(templates_.at(item.item == 999999 ? "money_0606" : "item_0606"));
    if (bytes.size() != 152)
        throw std::runtime_error("Invalid native inventory carrier");
    auto key = std::to_string(item.item);
    if (item.item != 999999 && records_.contains(key)) {
        auto raw = unhex(records_[key].at("hex"));
        if (raw.size() != 147)
            throw std::runtime_error("Invalid item-specific native record");
        std::copy(raw.begin(), raw.end(), bytes.begin() + 5);
    }
    put32(bytes, 0, eid, true);
    bytes[4] = item.item == 999999 ? 0 : create ? 1 : 0;
    if (equipped) {
        if (item.slot < 1 || item.slot > 30)
            throw std::runtime_error("Invalid equipped address");
        bytes[5] = 0;
        bytes[6] = bytes[11] = static_cast<unsigned char>(item.slot);
    } else {
        auto [category, slot] = address(item.slot);
        bytes[5] = static_cast<unsigned char>(category);
        bytes[6] = static_cast<unsigned char>(slot);
        bytes[11] = 0;
    }
    if (item.grade < 0 || item.grade > 127 || item.uid == 0 || item.quantity < 0 ||
        item.quantity > (item.item == 999999 ? 1000000000000LL : 10000LL))
        throw std::runtime_error("Invalid persistent native item record");
    bytes[12] = static_cast<unsigned char>(item.grade);
    put32(bytes, 7, item.item);
    put32(bytes, 13, item.uid);
    put32(bytes, 17, item.created);
    put32(bytes, 21, static_cast<std::uint32_t>(item.quantity));
    put32(bytes, 25, static_cast<std::uint32_t>(static_cast<std::uint64_t>(item.quantity) >> 32));
    // Permanent items in the supplied native records use 2100-01-01, not Unix zero.
    put32(bytes, 61, item.expires.value_or(4102444800u));
    visual_attributes(bytes,item);
    return bytes;
}
std::vector<Bytes> Inventory::snapshots(std::uint32_t eid, double timestamp) {
    std::vector<std::pair<ItemInstance, bool>> records;
    auto slots=available(eid,timestamp);
    for (const auto &item : items(eid))
        if (slots.count(item.slot) && item.item != 999999 && (!item.expires || *item.expires > timestamp))
            records.emplace_back(item, false);
    for (const auto &item : items(eid, true))
        if (!item.expires || *item.expires > timestamp)
            records.emplace_back(item, true);
    std::vector<Bytes> packets;
    for (std::size_t start = 0; start < records.size() || start == 0; start += 127) {
        auto count = std::min<std::size_t>(127, records.size() - start);
        Bytes packet(6);
        put32(packet, 0, eid, true);
        packet[4] = 5;
        packet[5] = static_cast<unsigned char>(count);
        for (std::size_t i = 0; i < count; i++) {
            auto bytes = update(eid, records[start + i].first, records[start + i].second);
            packet.insert(packet.end(), bytes.begin() + 5, bytes.end());
        }
        packets.push_back(std::move(packet));
    }
    return packets;
}
std::string Inventory::name(std::uint32_t item) const {
    if (item == 999999)
        return "Carats";
    auto found = names_.find(item);
    if (found == names_.end())
        return "Item sem nome";
    auto value = found->second.value("name", "");
    return value.empty() || std::all_of(value.begin(), value.end(),
                                        [](unsigned char c) { return c >= '0' && c <= '9'; })
               ? "Item sem nome"
               : value;
}
} // namespace rosh
