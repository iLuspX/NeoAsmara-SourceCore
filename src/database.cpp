#include "database.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace karos {
namespace {

class Statement {
public:
    Statement(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("SQLite prepare: ") + sqlite3_errmsg(db_));
    }
    ~Statement() { if (stmt_) sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const { return stmt_; }
    void text(int i, const std::string& value) {
        if (sqlite3_bind_text(stmt_, i, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
    }
    void i64(int i, std::int64_t value) {
        if (sqlite3_bind_int64(stmt_, i, value) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
    }
    void integer(int i, int value) {
        if (sqlite3_bind_int(stmt_, i, value) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
    }
    void real(int i, double value) {
        if (sqlite3_bind_double(stmt_, i, value) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
    }
    bool row() {
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("SQLite step: ") + sqlite3_errmsg(db_));
    }
    void done() {
        int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_DONE) throw std::runtime_error(std::string("SQLite step: ") + sqlite3_errmsg(db_));
    }
private:
    sqlite3* db_{};
    sqlite3_stmt* stmt_{};
};

std::string column_text(sqlite3_stmt* s, int col) {
    const auto* p = sqlite3_column_text(s, col);
    return p ? reinterpret_cast<const char*>(p) : std::string{};
}

Character row_character(sqlite3_stmt* s) {
    Character c;
    c.eid = sqlite3_column_int64(s, 0);
    c.account_id = column_text(s, 1);
    c.slot = sqlite3_column_int(s, 2);
    c.name = column_text(s, 3);
    c.cls = sqlite3_column_int(s, 4);
    c.level = sqlite3_column_int(s, 5);
    c.exp = sqlite3_column_int64(s, 6);
    c.skill_points = sqlite3_column_int(s, 7);
    c.fleta_points = sqlite3_column_int(s, 8);
    c.fleta_progress = sqlite3_column_int(s, 9);
    c.hp = sqlite3_column_int(s, 10);
    c.max_hp = sqlite3_column_int(s, 11);
    c.mp = sqlite3_column_int(s, 12);
    c.max_mp = sqlite3_column_int(s, 13);
    c.map_name = column_text(s, 14);
    c.x = sqlite3_column_double(s, 15);
    c.y = sqlite3_column_double(s, 16);
    c.z = sqlite3_column_double(s, 17);
    c.gold = sqlite3_column_int64(s, 18);
    return c;
}

constexpr const char* kCharacterSelect =
    "SELECT eid, account_id, slot, name, cls, level, exp, skill_points, fleta_points, "
    "fleta_progress, hp, max_hp, mp, max_mp, COALESCE(map_name,'berneo'), "
    "COALESCE(x,9775.0), COALESCE(y,8124.8), COALESCE(z,672.7), gold FROM characters ";

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

} // namespace

Database::Database(std::string path) : path_(std::move(path)) {
    const std::filesystem::path p(path_);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    if (sqlite3_open_v2(path_.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "could not allocate SQLite handle";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Cannot open database: " + msg);
    }
    sqlite3_busy_timeout(db_, 10000);
    exec("PRAGMA foreign_keys=ON;");
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=FULL;");
}

Database::~Database() { if (db_) sqlite3_close(db_); }

void Database::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : sqlite3_errmsg(db_);
        sqlite3_free(err);
        throw std::runtime_error("SQLite: " + msg);
    }
}

bool Database::has_column(const std::string& table, const std::string& column) {
    Statement q(db_, "PRAGMA table_info(\"" + table + "\")");
    while (q.row()) if (column_text(q.get(), 1) == column) return true;
    return false;
}

void Database::add_column_if_missing(const std::string& table, const std::string& column,
                                     const std::string& definition) {
    if (!has_column(table, column)) exec("ALTER TABLE \"" + table + "\" ADD COLUMN \"" + column + "\" " + definition);
}

std::size_t Database::import_reference_content(const std::string& source_path) {
    if (source_path.empty()) throw std::invalid_argument("reference database path is empty");
    const auto target_abs = std::filesystem::absolute(path_).lexically_normal();
    const auto source_abs = std::filesystem::absolute(source_path).lexically_normal();
    if (target_abs == source_abs) throw std::invalid_argument("source content database must differ from target database");
    if (!std::filesystem::exists(source_abs)) throw std::invalid_argument("reference database does not exist: " + source_path);

    char* attach_sql = sqlite3_mprintf("ATTACH DATABASE %Q AS source_content", source_abs.string().c_str());
    if (!attach_sql) throw std::bad_alloc();
    try {
        exec(attach_sql);
    } catch (...) {
        sqlite3_free(attach_sql);
        throw;
    }
    sqlite3_free(attach_sql);

    auto detach_noexcept = [this]() noexcept {
        char* err = nullptr;
        sqlite3_exec(db_, "DETACH DATABASE source_content", nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
    };

    try {
        // Read metadata into vectors before DDL so no schema-reading statement remains active.
        std::vector<std::string> source_tables;
        {
            Statement q(db_, "SELECT name FROM source_content.sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
            while (q.row()) source_tables.push_back(column_text(q.get(), 0));
        }
        if (source_tables.empty()) throw std::runtime_error("reference database contains no tables");

        std::vector<std::string> old_tables;
        {
            Statement q(db_, "SELECT name FROM main.sqlite_master WHERE type='table' AND name LIKE 'reference_%' ORDER BY name");
            while (q.row()) old_tables.push_back(column_text(q.get(), 0));
        }

        auto quote_identifier = [](const std::string& name) {
            if (name.empty()) throw std::runtime_error("empty SQL identifier in reference database");
            for (unsigned char c : name) {
                if (!(std::isalnum(c) || c == '_'))
                    throw std::runtime_error("unsupported SQL identifier in reference database: " + name);
            }
            return "\"" + name + "\"";
        };

        exec("BEGIN IMMEDIATE;");
        try {
            for (const auto& name : old_tables) exec("DROP TABLE main." + quote_identifier(name));
            for (const auto& name : source_tables) {
                const std::string dest = "reference_" + name;
                const std::string dest_q = quote_identifier(dest);
                const std::string source_q = quote_identifier(name);
                exec("CREATE TABLE main." + dest_q + " AS SELECT * FROM source_content." + source_q);
                // Most decoded client tables have a 'rec' key. Index it for quick editor/admin lookups.
                if (has_column(dest, "rec")) {
                    const std::string index = quote_identifier("idx_" + dest + "_rec");
                    exec("CREATE INDEX main." + index + " ON " + dest_q + "(rec)");
                }
            }
            exec("COMMIT;");
        } catch (...) {
            try { exec("ROLLBACK;"); } catch (...) {}
            throw;
        }
        detach_noexcept();
        return source_tables.size();
    } catch (...) {
        detach_noexcept();
        throw;
    }
}

void Database::migrate() {
    // Create additive schema. Existing characters/accounts/inventory rows are never dropped.
    exec(R"SQL(
    BEGIN IMMEDIATE;
    CREATE TABLE IF NOT EXISTS accounts(
      account_id TEXT PRIMARY KEY,
      password_hash TEXT NOT NULL DEFAULT '',
      status TEXT NOT NULL DEFAULT 'active' CHECK(status IN ('active','locked')),
      created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
      last_login_at TEXT
    );
    CREATE TABLE IF NOT EXISTS characters(
      eid INTEGER PRIMARY KEY,
      account_id TEXT NOT NULL,
      slot INTEGER NOT NULL DEFAULT 0,
      name TEXT NOT NULL,
      cls INTEGER NOT NULL DEFAULT 100,
      level INTEGER NOT NULL DEFAULT 1 CHECK(level BETWEEN 1 AND 110),
      exp INTEGER NOT NULL DEFAULT 0 CHECK(exp >= 0),
      skill_points INTEGER NOT NULL DEFAULT 0 CHECK(skill_points >= 0),
      fleta_points INTEGER NOT NULL DEFAULT 0 CHECK(fleta_points >= 0),
      fleta_progress INTEGER NOT NULL DEFAULT 0 CHECK(fleta_progress >= 0),
      skill_levels_json TEXT NOT NULL DEFAULT '{}',
      fleta_allocations_json TEXT NOT NULL DEFAULT '{}',
      hp INTEGER NOT NULL DEFAULT 186,
      max_hp INTEGER NOT NULL DEFAULT 186,
      mp INTEGER NOT NULL DEFAULT 100,
      max_mp INTEGER NOT NULL DEFAULT 100,
      map_name TEXT NOT NULL DEFAULT 'berneo',
      x REAL NOT NULL DEFAULT 9775.0,
      y REAL NOT NULL DEFAULT 8124.8,
      z REAL NOT NULL DEFAULT 672.7,
      gold INTEGER NOT NULL DEFAULT 0 CHECK(gold >= 0),
      created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
      UNIQUE(account_id, slot),
      FOREIGN KEY(account_id) REFERENCES accounts(account_id) ON DELETE CASCADE
    );
    CREATE TABLE IF NOT EXISTS inventory(
      character_eid INTEGER NOT NULL, slot INTEGER NOT NULL, item_id INTEGER NOT NULL,
      quantity INTEGER NOT NULL DEFAULT 1 CHECK(quantity > 0), enhancement INTEGER NOT NULL DEFAULT 0,
      PRIMARY KEY(character_eid, slot),
      FOREIGN KEY(character_eid) REFERENCES characters(eid) ON DELETE CASCADE
    );
    CREATE TABLE IF NOT EXISTS equipment(
      character_eid INTEGER NOT NULL, slot INTEGER NOT NULL, item_id INTEGER NOT NULL,
      PRIMARY KEY(character_eid, slot),
      FOREIGN KEY(character_eid) REFERENCES characters(eid) ON DELETE CASCADE
    );
    CREATE TABLE IF NOT EXISTS character_wallet(
      character_eid INTEGER PRIMARY KEY, gold INTEGER NOT NULL DEFAULT 0 CHECK(gold >= 0),
      flota INTEGER NOT NULL DEFAULT 0 CHECK(flota >= 0),
      FOREIGN KEY(character_eid) REFERENCES characters(eid) ON DELETE CASCADE
    );
    CREATE TABLE IF NOT EXISTS wallet_ledger(
      ledger_id INTEGER PRIMARY KEY AUTOINCREMENT, character_eid INTEGER NOT NULL,
      currency TEXT NOT NULL, delta INTEGER NOT NULL, balance_after INTEGER NOT NULL,
      reason TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
      FOREIGN KEY(character_eid) REFERENCES characters(eid) ON DELETE CASCADE
    );
    CREATE TABLE IF NOT EXISTS gm_accounts(
      account_id TEXT PRIMARY KEY, role TEXT NOT NULL CHECK(role IN ('player','gm','owner')),
      granted_by TEXT NOT NULL DEFAULT 'console', granted_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
      FOREIGN KEY(account_id) REFERENCES accounts(account_id) ON DELETE CASCADE
    );
    CREATE TABLE IF NOT EXISTS gm_audit(
      audit_id INTEGER PRIMARY KEY AUTOINCREMENT, actor_account TEXT NOT NULL,
      action TEXT NOT NULL, target TEXT NOT NULL DEFAULT '', details TEXT NOT NULL DEFAULT '',
      created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    );
    CREATE TABLE IF NOT EXISTS item_definitions(
      item_id INTEGER PRIMARY KEY, name TEXT NOT NULL DEFAULT '', item_type TEXT NOT NULL DEFAULT 'unknown',
      stack_limit INTEGER NOT NULL DEFAULT 1, required_level INTEGER NOT NULL DEFAULT 0,
      attack REAL NOT NULL DEFAULT 0, defense REAL NOT NULL DEFAULT 0,
      buy_price INTEGER NOT NULL DEFAULT 0, sell_price INTEGER NOT NULL DEFAULT 0,
      enabled INTEGER NOT NULL DEFAULT 1, data_json TEXT NOT NULL DEFAULT '{}'
    );
    CREATE TABLE IF NOT EXISTS class_definitions(
      class_id INTEGER PRIMARY KEY, code TEXT NOT NULL UNIQUE, display_name TEXT NOT NULL DEFAULT '', enabled INTEGER NOT NULL DEFAULT 1
    );
    CREATE TABLE IF NOT EXISTS skill_definitions(
      skill_id INTEGER PRIMARY KEY, class_id INTEGER NOT NULL DEFAULT 0, name TEXT NOT NULL DEFAULT '',
      required_level INTEGER NOT NULL DEFAULT 1, max_level INTEGER NOT NULL DEFAULT 10,
      mp_cost INTEGER NOT NULL DEFAULT 0, range_value REAL NOT NULL DEFAULT 0,
      cooldown_ms INTEGER NOT NULL DEFAULT 0, power REAL NOT NULL DEFAULT 1,
      enabled INTEGER NOT NULL DEFAULT 1, data_json TEXT NOT NULL DEFAULT '{}'
    );
    CREATE TABLE IF NOT EXISTS character_skills(
      character_eid INTEGER NOT NULL, skill_id INTEGER NOT NULL, skill_level INTEGER NOT NULL DEFAULT 0,
      PRIMARY KEY(character_eid,skill_id),
      FOREIGN KEY(character_eid) REFERENCES characters(eid) ON DELETE CASCADE,
      FOREIGN KEY(skill_id) REFERENCES skill_definitions(skill_id)
    );
    CREATE TABLE IF NOT EXISTS level_curve(
      target_level INTEGER PRIMARY KEY, cumulative_exp INTEGER NOT NULL, skill_points_reward INTEGER NOT NULL DEFAULT 1
    );
    CREATE TABLE IF NOT EXISTS map_definitions(
      map_id INTEGER PRIMARY KEY, map_name TEXT NOT NULL UNIQUE, display_name TEXT NOT NULL DEFAULT '', enabled INTEGER NOT NULL DEFAULT 1
    );
    CREATE TABLE IF NOT EXISTS monster_definitions(
      npc_id INTEGER PRIMARY KEY, name TEXT NOT NULL DEFAULT '', level INTEGER NOT NULL DEFAULT 1,
      max_hp INTEGER NOT NULL DEFAULT 100, attack REAL NOT NULL DEFAULT 1, defense REAL NOT NULL DEFAULT 0,
      aggro_range REAL NOT NULL DEFAULT 0, exp_reward INTEGER NOT NULL DEFAULT 1,
      respawn_seconds INTEGER NOT NULL DEFAULT 30, enabled INTEGER NOT NULL DEFAULT 1,
      data_json TEXT NOT NULL DEFAULT '{}'
    );
    CREATE TABLE IF NOT EXISTS spawn_points(
      spawn_id INTEGER PRIMARY KEY AUTOINCREMENT, map_name TEXT NOT NULL, npc_id INTEGER NOT NULL,
      x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL, heading REAL NOT NULL DEFAULT 0,
      respawn_seconds INTEGER NOT NULL DEFAULT 30, max_alive INTEGER NOT NULL DEFAULT 1, enabled INTEGER NOT NULL DEFAULT 1,
      FOREIGN KEY(npc_id) REFERENCES monster_definitions(npc_id)
    );
    CREATE INDEX IF NOT EXISTS idx_spawn_map ON spawn_points(map_name,enabled);
    CREATE TABLE IF NOT EXISTS monster_drops(
      npc_id INTEGER NOT NULL, item_id INTEGER NOT NULL, chance_bp INTEGER NOT NULL DEFAULT 0 CHECK(chance_bp BETWEEN 0 AND 10000),
      min_qty INTEGER NOT NULL DEFAULT 1, max_qty INTEGER NOT NULL DEFAULT 1,
      PRIMARY KEY(npc_id,item_id),
      FOREIGN KEY(npc_id) REFERENCES monster_definitions(npc_id),
      FOREIGN KEY(item_id) REFERENCES item_definitions(item_id)
    );
    CREATE TABLE IF NOT EXISTS ground_items(
      ground_item_id INTEGER PRIMARY KEY AUTOINCREMENT, map_name TEXT NOT NULL, item_id INTEGER NOT NULL,
      quantity INTEGER NOT NULL DEFAULT 1, x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL,
      owner_eid INTEGER, spawned_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
      expires_at TEXT, picked_up INTEGER NOT NULL DEFAULT 0,
      FOREIGN KEY(item_id) REFERENCES item_definitions(item_id)
    );
    CREATE TABLE IF NOT EXISTS npc_definitions(
      npc_id INTEGER PRIMARY KEY, name TEXT NOT NULL DEFAULT '', map_name TEXT NOT NULL DEFAULT '',
      x REAL NOT NULL DEFAULT 0, y REAL NOT NULL DEFAULT 0, z REAL NOT NULL DEFAULT 0,
      npc_type TEXT NOT NULL DEFAULT 'generic', data_json TEXT NOT NULL DEFAULT '{}'
    );
    CREATE TABLE IF NOT EXISTS npc_dialogue(
      npc_id INTEGER NOT NULL, dialogue_id INTEGER NOT NULL, text_key TEXT NOT NULL DEFAULT '',
      action_type TEXT NOT NULL DEFAULT 'none', action_data TEXT NOT NULL DEFAULT '{}',
      PRIMARY KEY(npc_id,dialogue_id), FOREIGN KEY(npc_id) REFERENCES npc_definitions(npc_id) ON DELETE CASCADE
    );
    CREATE TABLE IF NOT EXISTS shop_definitions(
      shop_id INTEGER PRIMARY KEY, npc_id INTEGER NOT NULL, name TEXT NOT NULL DEFAULT '', enabled INTEGER NOT NULL DEFAULT 1,
      FOREIGN KEY(npc_id) REFERENCES npc_definitions(npc_id)
    );
    CREATE TABLE IF NOT EXISTS shop_items(
      shop_id INTEGER NOT NULL, item_id INTEGER NOT NULL, price INTEGER NOT NULL DEFAULT 0,
      stock INTEGER NOT NULL DEFAULT -1, PRIMARY KEY(shop_id,item_id),
      FOREIGN KEY(shop_id) REFERENCES shop_definitions(shop_id) ON DELETE CASCADE,
      FOREIGN KEY(item_id) REFERENCES item_definitions(item_id)
    );
    CREATE TABLE IF NOT EXISTS quest_definitions(
      quest_id INTEGER PRIMARY KEY, name TEXT NOT NULL DEFAULT '', min_level INTEGER NOT NULL DEFAULT 1,
      max_level INTEGER NOT NULL DEFAULT 110, repeatable INTEGER NOT NULL DEFAULT 0,
      exp_reward INTEGER NOT NULL DEFAULT 0, gold_reward INTEGER NOT NULL DEFAULT 0,
      enabled INTEGER NOT NULL DEFAULT 1, data_json TEXT NOT NULL DEFAULT '{}'
    );
    CREATE TABLE IF NOT EXISTS quest_objectives(
      quest_id INTEGER NOT NULL, objective_no INTEGER NOT NULL, objective_type TEXT NOT NULL,
      target_id INTEGER NOT NULL DEFAULT 0, required_count INTEGER NOT NULL DEFAULT 1,
      PRIMARY KEY(quest_id,objective_no), FOREIGN KEY(quest_id) REFERENCES quest_definitions(quest_id) ON DELETE CASCADE
    );
    CREATE TABLE IF NOT EXISTS character_quests(
      character_eid INTEGER NOT NULL, quest_id INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'active',
      progress INTEGER NOT NULL DEFAULT 0, accepted_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, completed_at TEXT,
      PRIMARY KEY(character_eid,quest_id),
      FOREIGN KEY(character_eid) REFERENCES characters(eid) ON DELETE CASCADE,
      FOREIGN KEY(quest_id) REFERENCES quest_definitions(quest_id)
    );
    CREATE TABLE IF NOT EXISTS portals(
      portal_id INTEGER PRIMARY KEY, source_map TEXT NOT NULL, target_map TEXT NOT NULL,
      x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL, target_x REAL NOT NULL,
      target_y REAL NOT NULL, target_z REAL NOT NULL, min_level INTEGER NOT NULL DEFAULT 1,
      enabled INTEGER NOT NULL DEFAULT 1, data_json TEXT NOT NULL DEFAULT '{}'
    );
    CREATE TABLE IF NOT EXISTS gm_spawn_requests(
      request_id INTEGER PRIMARY KEY AUTOINCREMENT, requested_by TEXT NOT NULL, npc_id INTEGER NOT NULL,
      map_name TEXT NOT NULL, x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL,
      spawn_count INTEGER NOT NULL DEFAULT 1, status TEXT NOT NULL DEFAULT 'pending',
      created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    );
    CREATE TABLE IF NOT EXISTS server_announcements(
      announcement_id INTEGER PRIMARY KEY AUTOINCREMENT, created_by TEXT NOT NULL,
      message TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, delivered INTEGER NOT NULL DEFAULT 0
    );
    CREATE TABLE IF NOT EXISTS server_settings(
      setting_key TEXT PRIMARY KEY, setting_value TEXT NOT NULL, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    );
    COMMIT;
    )SQL");

    // Migrate old prototype schemas in place. SQLite's CREATE IF NOT EXISTS does not
    // add columns, so columns are checked individually. This preserves existing saves.
    add_column_if_missing("accounts", "password_hash", "TEXT NOT NULL DEFAULT ''");
    add_column_if_missing("accounts", "status", "TEXT NOT NULL DEFAULT 'active'");
    add_column_if_missing("accounts", "last_login_at", "TEXT");
    add_column_if_missing("character_wallet", "flota", "INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing("characters", "skill_points", "INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing("characters", "fleta_points", "INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing("characters", "fleta_progress", "INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing("characters", "skill_levels_json", "TEXT NOT NULL DEFAULT '{}'");
    add_column_if_missing("characters", "fleta_allocations_json", "TEXT NOT NULL DEFAULT '{}'");
    add_column_if_missing("characters", "max_hp", "INTEGER NOT NULL DEFAULT 186");
    add_column_if_missing("characters", "max_mp", "INTEGER NOT NULL DEFAULT 100");
    add_column_if_missing("characters", "map_name", "TEXT NOT NULL DEFAULT 'berneo'");
    add_column_if_missing("characters", "x", "REAL NOT NULL DEFAULT 9775.0");
    add_column_if_missing("characters", "y", "REAL NOT NULL DEFAULT 8124.8");
    add_column_if_missing("characters", "z", "REAL NOT NULL DEFAULT 672.7");
    add_column_if_missing("inventory", "enhancement", "INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing("inventory", "quantity", "INTEGER NOT NULL DEFAULT 1");

    // Compatibility import: existing saves often contain accounts but older branches
    // didn't create the accounts parent row correctly. Recreate it without overwriting.
    exec("INSERT OR IGNORE INTO accounts(account_id) SELECT DISTINCT account_id FROM characters WHERE account_id IS NOT NULL AND account_id<>'';");
    exec("INSERT OR IGNORE INTO character_wallet(character_eid,gold) SELECT eid,gold FROM characters;");
    exec("UPDATE characters SET max_hp=CASE WHEN max_hp<1 THEN 186 ELSE max_hp END, max_mp=CASE WHEN max_mp<1 THEN 100 ELSE max_mp END;");
    exec("CREATE INDEX IF NOT EXISTS idx_characters_account_slot ON characters(account_id,slot);");
    exec("CREATE INDEX IF NOT EXISTS idx_inventory_char ON inventory(character_eid,slot);");
    exec("CREATE INDEX IF NOT EXISTS idx_gm_audit_time ON gm_audit(created_at);");

    // Stable class catalog: class IDs are protocol IDs. We never silently replace an
    // unknown ID with a mage/warrior; unknown create packet layouts must be diagnosed.
    exec(R"SQL(
      INSERT OR IGNORE INTO class_definitions(class_id,code,display_name) VALUES
        (100,'class_100','Karos class 100'),(200,'class_200','Karos class 200'),
        (300,'class_300','Karos class 300'),(400,'class_400','Karos class 400'),
        (500,'class_500','Karos class 500'),(600,'class_600','Karos class 600'),
        (800,'class_800','Karos class 800');
      INSERT OR IGNORE INTO level_curve(target_level,cumulative_exp,skill_points_reward) VALUES
        (2,16,1),(3,88,1),(4,200,1),(5,380,1),(6,640,1),(7,1000,1),(8,1500,1),(9,2200,1),(10,3200,1);
      INSERT OR IGNORE INTO server_settings(setting_key,setting_value) VALUES
        ('schema_version','1'),('max_character_slots','4'),('max_level','110'),('exp_rate','1.0'),('drop_rate','1.0'),('exp_curve_state','verified_prototype_values_only_through_level_10');
      INSERT OR IGNORE INTO item_definitions(item_id,name,item_type,stack_limit) VALUES (1002,'Experimental item','misc',99);
    )SQL");
}

void Database::ensure_account(const std::string& account_id) {
    const auto id = trim(account_id);
    if (id.empty() || id.size() > 64) throw std::invalid_argument("account ID must be 1..64 characters");
    Statement q(db_, "INSERT OR IGNORE INTO accounts(account_id) VALUES(?)"); q.text(1, id); q.done();
}

bool Database::account_exists(const std::string& account_id) {
    Statement q(db_, "SELECT 1 FROM accounts WHERE account_id=?"); q.text(1, account_id); return q.row();
}

bool Database::character_exists(std::int64_t eid) {
    Statement q(db_, "SELECT 1 FROM characters WHERE eid=?"); q.i64(1, eid); return q.row();
}

std::vector<Character> Database::characters_for_account(const std::string& account_id) {
    std::vector<Character> out;
    Statement q(db_, std::string(kCharacterSelect) + " WHERE account_id=? ORDER BY slot");
    q.text(1, account_id);
    while (q.row()) out.push_back(row_character(q.get()));
    return out;
}

std::optional<Character> Database::get_character(std::int64_t eid) {
    Statement q(db_, std::string(kCharacterSelect) + " WHERE eid=?"); q.i64(1, eid);
    if (!q.row()) return std::nullopt;
    return row_character(q.get());
}

std::optional<Character> Database::get_character_by_name(const std::string& name) {
    Statement q(db_, std::string(kCharacterSelect) + " WHERE name=? COLLATE NOCASE LIMIT 1"); q.text(1, name);
    if (!q.row()) return std::nullopt;
    return row_character(q.get());
}

std::int64_t Database::create_character(const std::string& account_id, const std::string& name, int class_id) {
    const std::string clean_name = trim(name);
    static const int allowed[] = {100,200,300,400,500,600,800};
    if (std::find(std::begin(allowed), std::end(allowed), class_id) == std::end(allowed))
        throw std::invalid_argument("unsupported class ID " + std::to_string(class_id) + "; refusing to silently substitute a different class");
    if (clean_name.empty() || clean_name.size() > 20) throw std::invalid_argument("character name must be 1..20 bytes/characters");
    if (clean_name.find('\0') != std::string::npos) throw std::invalid_argument("character name contains NUL");
    ensure_account(account_id);

    exec("BEGIN IMMEDIATE;");
    try {
        Statement account(db_, "SELECT status FROM accounts WHERE account_id=?"); account.text(1, account_id);
        if (!account.row() || column_text(account.get(), 0) != "active") throw std::runtime_error("account missing or locked");
        Statement count(db_, "SELECT COUNT(*) FROM characters WHERE account_id=?"); count.text(1, account_id); count.row();
        int n = sqlite3_column_int(count.get(), 0);
        Statement max_slots(db_, "SELECT setting_value FROM server_settings WHERE setting_key='max_character_slots'");
        int slot_limit = 4; if (max_slots.row()) slot_limit = std::stoi(column_text(max_slots.get(), 0));
        if (n >= slot_limit) throw std::runtime_error("account already has maximum character slots");
        Statement duplicate(db_, "SELECT 1 FROM characters WHERE name=? COLLATE NOCASE"); duplicate.text(1, clean_name);
        if (duplicate.row()) throw std::runtime_error("character name is already in use");
        Statement slot(db_, "SELECT COALESCE(MAX(slot),-1)+1 FROM characters WHERE account_id=?"); slot.text(1, account_id); slot.row();
        const int next_slot = sqlite3_column_int(slot.get(), 0);
        Statement max_eid(db_, "SELECT COALESCE(MAX(eid),10385)+1 FROM characters"); max_eid.row();
        const std::int64_t eid = sqlite3_column_int64(max_eid.get(), 0);
        Statement ins(db_, "INSERT INTO characters(eid,account_id,slot,name,cls,level,exp,hp,max_hp,mp,max_mp,map_name,x,y,z,gold) VALUES(?,?,?,?,?,1,0,186,186,100,100,'berneo',9775.0,8124.8,672.7,0)");
        ins.i64(1, eid); ins.text(2, account_id); ins.integer(3, next_slot); ins.text(4, clean_name); ins.integer(5, class_id); ins.done();
        Statement wallet(db_, "INSERT OR IGNORE INTO character_wallet(character_eid,gold) VALUES(?,0)"); wallet.i64(1, eid); wallet.done();
        exec("COMMIT;");
        return eid;
    } catch (...) { exec("ROLLBACK;"); throw; }
}

bool Database::delete_character(const std::string& account_id, std::int64_t eid) {
    exec("BEGIN IMMEDIATE;");
    try {
        Statement d(db_, "DELETE FROM characters WHERE account_id=? AND eid=?"); d.text(1, account_id); d.i64(2, eid); d.done();
        const bool deleted = sqlite3_changes(db_) > 0;
        Statement reorder(db_, "SELECT eid FROM characters WHERE account_id=? ORDER BY slot"); reorder.text(1, account_id);
        std::vector<std::int64_t> ids; while (reorder.row()) ids.push_back(sqlite3_column_int64(reorder.get(),0));
        for (std::size_t i=0;i<ids.size();++i) { Statement u(db_, "UPDATE characters SET slot=?,updated_at=CURRENT_TIMESTAMP WHERE eid=?"); u.integer(1, static_cast<int>(i)); u.i64(2,ids[i]); u.done(); }
        exec("COMMIT;"); return deleted;
    } catch (...) { exec("ROLLBACK;"); throw; }
}

void Database::set_level(const std::string& character_name, int level) {
    if (level < 1 || level > 110) throw std::invalid_argument("level must be 1..110");
    Statement old(db_, "SELECT level FROM characters WHERE name=? COLLATE NOCASE LIMIT 1"); old.text(1,character_name);
    if (!old.row()) throw std::runtime_error("character not found");
    const int previous=sqlite3_column_int(old.get(),0);
    const int points=std::max(0,level-previous);
    Statement q(db_, "UPDATE characters SET level=?, skill_points=skill_points+?, updated_at=CURRENT_TIMESTAMP WHERE name=? COLLATE NOCASE");
    q.integer(1, level); q.integer(2, points); q.text(3, character_name); q.done();
    if (sqlite3_changes(db_) == 0) throw std::runtime_error("character not found");
}

void Database::grant_exp(std::int64_t eid, std::int64_t amount) {
    if (amount <= 0) throw std::invalid_argument("XP amount must be positive");
    exec("BEGIN IMMEDIATE;");
    try {
        auto ch = get_character(eid); if (!ch) throw std::runtime_error("character not found");
        const std::int64_t new_exp = ch->exp + amount;
        int level = ch->level;
        int gained = 0;
        while (level < 110) {
            // Threshold values are cumulative EXP for the target level. Missing
            // levels deliberately stop progression until real client/content data
            // is imported; guessing the retail curve silently corrupts progression.
            Statement threshold(db_, "SELECT cumulative_exp,skill_points_reward FROM level_curve WHERE target_level=?");
            threshold.integer(1,level+1);
            if (!threshold.row()) break;
            const auto required = sqlite3_column_int64(threshold.get(),0);
            const int reward = sqlite3_column_int(threshold.get(),1);
            if (new_exp < required) break;
            ++level;
            gained += std::max(0,reward);
        }
        Statement u(db_, "UPDATE characters SET exp=?,level=?,skill_points=skill_points+?,updated_at=CURRENT_TIMESTAMP WHERE eid=?");
        u.i64(1,new_exp); u.integer(2,level); u.integer(3,gained); u.i64(4,eid); u.done();
        exec("COMMIT;");
    } catch (...) { exec("ROLLBACK;"); throw; }
}

void Database::add_gold(std::int64_t eid, std::int64_t amount) {
    if (amount == 0) throw std::invalid_argument("gold amount cannot be zero");
    exec("BEGIN IMMEDIATE;");
    try {
        auto ch = get_character(eid); if (!ch) throw std::runtime_error("character not found");
        const auto updated = ch->gold + amount;
        if (updated < 0) throw std::runtime_error("gold cannot become negative");
        Statement u(db_, "UPDATE characters SET gold=?,updated_at=CURRENT_TIMESTAMP WHERE eid=?"); u.i64(1,updated); u.i64(2,eid); u.done();
        Statement w(db_, "INSERT INTO character_wallet(character_eid,gold) VALUES(?,?) ON CONFLICT(character_eid) DO UPDATE SET gold=excluded.gold"); w.i64(1,eid); w.i64(2,updated); w.done();
        Statement l(db_, "INSERT INTO wallet_ledger(character_eid,currency,delta,balance_after,reason) VALUES(?,'gold',?,?,?)"); l.i64(1,eid); l.i64(2,amount); l.i64(3,updated); l.text(4,"GM/admin adjustment"); l.done();
        exec("COMMIT;");
    } catch (...) { exec("ROLLBACK;"); throw; }
}

int Database::add_item(std::int64_t eid, int item_id, int quantity) {
    if (item_id <= 0 || quantity <= 0 || quantity > 1000000) throw std::invalid_argument("invalid item ID/quantity");
    exec("BEGIN IMMEDIATE;");
    try {
        if (!character_exists(eid)) throw std::runtime_error("character not found");
        Statement def(db_, "SELECT stack_limit FROM item_definitions WHERE item_id=? AND enabled=1"); def.integer(1,item_id);
        int stack_limit=1;
        if (def.row()) stack_limit = std::max(1,sqlite3_column_int(def.get(),0));
        else {
            Statement adddef(db_, "INSERT OR IGNORE INTO item_definitions(item_id,name,item_type,stack_limit) VALUES(?,?,'gm_test',99)");
            adddef.integer(1,item_id); adddef.text(2,"GM item " + std::to_string(item_id)); adddef.done(); stack_limit=99;
        }
        int result_slot=-1;
        std::vector<std::pair<int,int>> stacks;
        {
            Statement stacksq(db_, "SELECT slot,quantity FROM inventory WHERE character_eid=? AND item_id=? ORDER BY slot");
            stacksq.i64(1,eid); stacksq.integer(2,item_id);
            while (stacksq.row()) stacks.emplace_back(sqlite3_column_int(stacksq.get(),0),sqlite3_column_int(stacksq.get(),1));
        }
        for (const auto& st : stacks) {
            if (quantity<=0) break;
            int slot=st.first, old=st.second;
            int moved=std::min(quantity,std::max(0,stack_limit-old));
            if (moved>0) {
                Statement u(db_, "UPDATE inventory SET quantity=quantity+? WHERE character_eid=? AND slot=?");
                u.integer(1,moved); u.i64(2,eid); u.integer(3,slot); u.done();
                if(result_slot<0) result_slot=slot;
                quantity-=moved;
            }
        }
        while (quantity>0) {
            Statement used(db_, "SELECT slot FROM inventory WHERE character_eid=? ORDER BY slot"); used.i64(1,eid);
            int candidate=0; while (used.row()) { int s=sqlite3_column_int(used.get(),0); if (s==candidate) ++candidate; else if (s>candidate) break; }
            if (candidate>=120) throw std::runtime_error("inventory full");
            int moved=std::min(quantity,stack_limit);
            Statement ins(db_, "INSERT INTO inventory(character_eid,slot,item_id,quantity) VALUES(?,?,?,?)");
            ins.i64(1,eid); ins.integer(2,candidate); ins.integer(3,item_id); ins.integer(4,moved); ins.done();
            if(result_slot<0) result_slot=candidate;
            quantity-=moved;
        }
        exec("COMMIT;");
        return result_slot;
    } catch (...) { exec("ROLLBACK;"); throw; }
}

void Database::set_position(const std::string& character_name, const std::string& map_name,
                            double x, double y, double z) {
    if (map_name.empty() || map_name.size() > 64 || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        throw std::invalid_argument("invalid map/coordinates");
    Statement q(db_, "UPDATE characters SET map_name=?,x=?,y=?,z=?,updated_at=CURRENT_TIMESTAMP WHERE name=? COLLATE NOCASE");
    q.text(1,map_name); q.real(2,x); q.real(3,y); q.real(4,z); q.text(5,character_name); q.done();
    if (sqlite3_changes(db_) == 0) throw std::runtime_error("character not found");
}

void Database::add_announcement(const std::string& actor, const std::string& message) {
    if (message.empty() || message.size() > 400) throw std::invalid_argument("announcement must be 1..400 bytes");
    Statement q(db_, "INSERT INTO server_announcements(created_by,message) VALUES(?,?)");
    q.text(1,actor); q.text(2,message); q.done();
}

void Database::queue_spawn(const std::string& actor, int npc_id, const std::string& map_name,
                           double x, double y, double z, int count) {
    if (npc_id <= 0 || count < 1 || count > 100 || map_name.empty() || map_name.size() > 64 ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        throw std::invalid_argument("invalid spawn request");
    Statement q(db_, "INSERT INTO gm_spawn_requests(requested_by,npc_id,map_name,x,y,z,spawn_count) VALUES(?,?,?,?,?,?,?)");
    q.text(1,actor); q.integer(2,npc_id); q.text(3,map_name); q.real(4,x); q.real(5,y); q.real(6,z); q.integer(7,count); q.done();
}

std::vector<ItemStack> Database::inventory(std::int64_t eid) {
    std::vector<ItemStack> out;
    Statement q(db_, "SELECT slot,item_id,quantity,enhancement FROM inventory WHERE character_eid=? ORDER BY slot"); q.i64(1,eid);
    while (q.row()) out.push_back({sqlite3_column_int(q.get(),0),sqlite3_column_int(q.get(),1),sqlite3_column_int(q.get(),2),sqlite3_column_int(q.get(),3)});
    return out;
}

void Database::set_gm_role(const std::string& account_id, const std::string& requested_role, const std::string& granted_by) {
    // Keep older DBs readable: legacy "owner" is the same role as "admin".
    const std::string role = requested_role == "admin" ? "owner" : requested_role;
    if (role != "player" && role != "gm" && role != "owner") throw std::invalid_argument("role must be player, gm or admin");
    ensure_account(account_id);
    if (role == "player") {
        Statement d(db_, "DELETE FROM gm_accounts WHERE account_id=?"); d.text(1,account_id); d.done();
    } else {
        Statement q(db_, "INSERT INTO gm_accounts(account_id,role,granted_by) VALUES(?,?,?) ON CONFLICT(account_id) DO UPDATE SET role=excluded.role,granted_by=excluded.granted_by,granted_at=CURRENT_TIMESTAMP");
        q.text(1,account_id); q.text(2,role); q.text(3,granted_by); q.done();
    }
    audit(granted_by, "set_gm_role", account_id, role);
}

std::string Database::gm_role(const std::string& account_id) {
    Statement q(db_, "SELECT role FROM gm_accounts WHERE account_id=?"); q.text(1,account_id);
    return q.row() ? column_text(q.get(),0) : "player";
}

std::vector<std::pair<std::string, std::string>> Database::gm_accounts() {
    std::vector<std::pair<std::string,std::string>> out;
    Statement q(db_, "SELECT account_id,role FROM gm_accounts ORDER BY role DESC,account_id");
    while(q.row()) out.emplace_back(column_text(q.get(),0),column_text(q.get(),1));
    return out;
}

void Database::audit(const std::string& actor, const std::string& action,
                     const std::string& target, const std::string& details) {
    Statement q(db_, "INSERT INTO gm_audit(actor_account,action,target,details) VALUES(?,?,?,?)");
    q.text(1,actor); q.text(2,action); q.text(3,target); q.text(4,details); q.done();
}

std::vector<std::string> Database::audit_tail(int count) {
    std::vector<std::string> out;
    Statement q(db_, "SELECT created_at,actor_account,action,target,details FROM gm_audit ORDER BY audit_id DESC LIMIT ?"); q.integer(1,std::max(1,std::min(500,count)));
    while(q.row()) out.push_back(column_text(q.get(),0)+" | "+column_text(q.get(),1)+" | "+column_text(q.get(),2)+" | "+column_text(q.get(),3)+" | "+column_text(q.get(),4));
    return out;
}

} // namespace karos
