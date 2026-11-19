#include "native/economy.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace rosh {
bool craft_inventory(Sqlite& db,Inventory& inventory,std::uint32_t eid,const Json& recipe,
                     const std::string& event,double timestamp,double roll){
    if(event.empty()||event.size()>128||!std::isfinite(timestamp)||!std::isfinite(roll)||roll<0||roll>=1)
        throw std::runtime_error("Invalid crafting operation");
    const auto output=recipe.at("item").get<std::uint32_t>();
    const auto price=recipe.at("carats").get<std::int64_t>();
    const auto chance=recipe.at("chance_basis_points").get<int>();
    if(!output||price<0||chance<0||chance>10000||recipe.at("materials").empty()||
       inventory.definition(output).empty())throw std::runtime_error("Invalid native crafting recipe");
    db.exec("CREATE TABLE IF NOT EXISTS native_craft_operations(event_key TEXT PRIMARY KEY,eid INTEGER NOT NULL,recipe TEXT NOT NULL,success INTEGER NOT NULL)");
    Transaction tx(db);
    const auto prior=db.query("SELECT * FROM native_craft_operations WHERE event_key=?",{event});
    if(!prior.empty()){
        if(prior[0].at("eid")!=eid||prior[0].at("recipe")!=recipe.dump())
            throw std::runtime_error("Altered crafting replay");
        const bool success=prior[0].at("success").get<int>()!=0;tx.commit();return success;
    }
    Wallet wallet(db,inventory);
    if(wallet.balance(eid)<price)throw std::runtime_error("Insufficient crafting Carats");
    const auto slots=inventory.available(eid,timestamp);
    const auto items=inventory.items(eid);
    std::map<std::uint32_t,std::int64_t> requirements;
    for(const auto& material:recipe.at("materials")){
        const auto id=material.at("item").get<std::uint32_t>();
        const auto quantity=material.at("quantity").get<std::int64_t>();
        if(!id||id==999999||quantity<1||quantity>1000000)
            throw std::runtime_error("Invalid native crafting material");
        requirements[id]+=quantity;
    }
    std::vector<std::pair<ItemInstance,std::int64_t>> consumed;
    for(const auto& [id,quantity]:requirements){
        auto remaining=quantity;
        for(const auto& item:items)if(item.item==id&&slots.count(item.slot)&&
            (!item.expires||*item.expires>timestamp)){
            const auto count=std::min(remaining,item.quantity);
            if(count>0){consumed.emplace_back(item,count);remaining-=count;}
        }
        if(remaining)throw std::runtime_error("Missing crafting materials");
    }
    const bool success=roll*10000<chance;
    for(const auto& [item,count]:consumed)inventory.consume(eid,item,count,"crafting-material");
    if(price)wallet.apply(eid,-price,"craft-fee:"+event);
    if(success)inventory.grant_once(eid,{{output,1}},"craft-result:"+event);
    db.exec("INSERT INTO native_craft_operations VALUES(?,?,?,?)",{event,std::int64_t(eid),recipe.dump(),std::int64_t(success)});
    tx.commit();return success;
}
Wallet::Wallet(Sqlite &db, Inventory &inventory) : db_(db), inventory_(inventory) {
    db_.exec("CREATE TABLE IF NOT EXISTS wallet(eid INTEGER PRIMARY KEY,carats INTEGER NOT NULL DEFAULT 0 "
             "CHECK(carats>=0));CREATE TABLE IF NOT EXISTS wallet_events(event_key TEXT PRIMARY KEY,eid "
             "INTEGER NOT NULL,delta INTEGER NOT NULL,created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
}
std::int64_t Wallet::balance(std::uint32_t eid) {
    if (!eid)
        throw std::runtime_error("Invalid wallet character");
    auto rows = db_.query("SELECT carats FROM wallet WHERE eid=?", {std::int64_t(eid)});
    return rows.empty() ? 0 : rows[0].at("carats").get<std::int64_t>();
}
std::vector<Bytes> Wallet::updates(std::uint32_t eid) {
    auto current = balance(eid);
    std::vector<Bytes> result;
    for (auto item : inventory_.items(eid))
        if (item.item == 999999) {
            if (!current)
                item.quantity = 0;
            result.push_back(inventory_.update(eid, item));
        }
    return result;
}
void Wallet::synchronize(std::uint32_t eid, std::int64_t balance) {
    constexpr std::int64_t limit = 1000000000000LL;
    auto available = inventory_.available(eid, now());
    auto items = inventory_.items(eid);
    std::vector<ItemInstance> piles;
    std::set<int> occupied;
    for (const auto &item : items)
        if (item.item == 999999)
            piles.push_back(item);
        else
            occupied.insert(item.slot);
    auto count = balance / limit + (balance % limit != 0);
    if (!balance)
        count = piles.empty() ? 0 : 1;
    std::vector<int> slots;
    for (std::size_t i = 0; i < piles.size() && i < static_cast<std::size_t>(count); i++) {
        if (!available.count(piles[i].slot))
            throw std::runtime_error("A Carat pile is in an unavailable bag");
        slots.push_back(piles[i].slot);
    }
    for (int slot : available) {
        if (slots.size() >= static_cast<std::size_t>(count))
            break;
        if (!occupied.count(slot) && std::find(slots.begin(), slots.end(), slot) == slots.end())
            slots.push_back(slot);
    }
    if (slots.size() != static_cast<std::size_t>(count))
        throw std::runtime_error("Mochila cheia: cada pilha de Carat comporta 1 trilhao");
    for (std::size_t i = static_cast<std::size_t>(count); i < piles.size(); i++)
        db_.exec("DELETE FROM inventory_slots WHERE player_eid=? AND slot=?",
                 {std::int64_t(eid), std::int64_t(piles[i].slot)});
    std::int64_t remaining = balance;
    for (int slot : slots) {
        auto quantity = remaining ? std::min(limit, remaining) : 1;
        remaining -= std::min(limit, remaining);
        auto prior =
            std::find_if(piles.begin(), piles.end(), [&](const auto &item) { return item.slot == slot; });
        if (prior != piles.end())
            db_.exec("UPDATE inventory_slots SET quantity=? WHERE player_eid=? AND slot=?",
                     {quantity, std::int64_t(eid), std::int64_t(slot)});
        else
            db_.exec("INSERT INTO inventory_slots VALUES(?,?,?,?,?,?)",
                     {std::int64_t(eid), std::int64_t(slot), std::int64_t(999999), quantity,
                      std::int64_t(inventory_.next_uid()), std::int64_t(now())});
    }
}
std::pair<std::int64_t, bool> Wallet::apply(std::uint32_t eid, std::int64_t delta, const std::string &event) {
    if (!eid || event.empty() || event.size() > 128 || delta == std::numeric_limits<std::int64_t>::min())
        throw std::runtime_error("Invalid wallet event");
    std::lock_guard lock(db_.mutex());
    std::unique_ptr<Transaction> transaction;
    if (sqlite3_get_autocommit(db_.handle()))
        transaction = std::make_unique<Transaction>(db_);
    auto prior = db_.query("SELECT eid,delta FROM wallet_events WHERE event_key=?", {event});
    if (!prior.empty()) {
        if (prior[0].at("eid") != eid || prior[0].at("delta") != delta)
            throw std::runtime_error("Wallet event reused with different values");
        if (transaction)
            transaction->commit();
        return {balance(eid), false};
    }
    auto old = balance(eid);
    if ((delta < 0 && old < -delta) || (delta > 0 && old > std::numeric_limits<std::int64_t>::max() - delta))
        throw std::runtime_error("Insufficient Carats or wallet limit exceeded");
    auto next = old + delta;
    synchronize(eid, next);
    db_.exec("INSERT INTO wallet VALUES(?,?) ON CONFLICT(eid) DO UPDATE SET carats=excluded.carats",
             {std::int64_t(eid), next});
    db_.exec("INSERT INTO wallet_events(event_key,eid,delta) VALUES(?,?,?)",
             {event, std::int64_t(eid), delta});
    if (transaction)
        transaction->commit();
    return {next, true};
}
} // namespace rosh
