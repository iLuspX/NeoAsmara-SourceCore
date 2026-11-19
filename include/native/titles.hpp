#pragma once
#include "core.hpp"

namespace rosh {
inline void initialize_titles(Sqlite& db) {
    db.exec("CREATE TABLE IF NOT EXISTS character_titles(account_id TEXT NOT NULL,character_eid INTEGER NOT NULL,title_id INTEGER NOT NULL,granted_by TEXT NOT NULL,granted_at INTEGER NOT NULL,reason TEXT NOT NULL,PRIMARY KEY(character_eid,title_id));"
            "CREATE TABLE IF NOT EXISTS character_title_selection(character_eid INTEGER PRIMARY KEY,title_id INTEGER NOT NULL);"
            "CREATE TABLE IF NOT EXISTS title_audit(event_key TEXT PRIMARY KEY,actor_account TEXT NOT NULL,actor_eid INTEGER NOT NULL,target_account TEXT NOT NULL,target_eid INTEGER NOT NULL,title_id INTEGER NOT NULL,operation TEXT NOT NULL,created_at INTEGER NOT NULL,reason TEXT NOT NULL)");
}

inline bool title_owner(const fs::path& root, const std::string& account) {
    const auto authority = root / "Config/Keys/owner_authority.private.json.dat";
    if (!fs::is_regular_file(authority)) return false;
    const auto key = unprotect_key(root / "Config/Keys/content.key.dpapi");
    const auto owner = Json::parse(decrypt_content(read_file(authority), key, "owner_authority.private.json"));
    return owner.value("staff_titles_owner_bound", false) && owner.value("account_id", "") == account;
}

inline bool change_title(Sqlite& db, const std::string& event, const std::string& actor,
                         std::int64_t actor_eid, const std::string& account, std::int64_t eid,
                         std::int64_t title, bool revoke, const std::string& reason) {
    initialize_titles(db);
    const std::string operation = revoke ? "revoke" : "grant";
    Transaction tx(db);
    const auto prior = db.query("SELECT * FROM title_audit WHERE event_key=?", {event});
    if (!prior.empty()) {
        const auto& row = prior.front();
        if (row.at("actor_account") != actor || row.at("actor_eid") != actor_eid ||
            row.at("target_account") != account || row.at("target_eid") != eid ||
            row.at("title_id") != title || row.at("operation") != operation || row.at("reason") != reason)
            throw std::runtime_error("Title operation ID reused with different contents");
        tx.commit();
        return false;
    }
    const auto stamp = std::int64_t(now());
    if (revoke) {
        db.exec("DELETE FROM character_titles WHERE account_id=? AND character_eid=? AND title_id=?", {account,eid,title});
        db.exec("DELETE FROM character_title_selection WHERE character_eid=? AND title_id=?", {eid,title});
    } else {
        db.exec("INSERT INTO character_titles VALUES(?,?,?,?,?,?) ON CONFLICT(character_eid,title_id) DO NOTHING",
                {account,eid,title,actor,stamp,reason});
    }
    db.exec("INSERT INTO title_audit VALUES(?,?,?,?,?,?,?,?,?)", {event,actor,actor_eid,account,eid,title,operation,stamp,reason});
    tx.commit();
    return true;
}
}
