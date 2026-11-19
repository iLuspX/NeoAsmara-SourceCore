#include "native/core.hpp"
#include <algorithm>
#include <regex>
#include <set>

namespace rosh {
static std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    return value;
}
static bool username_valid(const std::string &value) {
    return std::regex_match(value, std::regex("[A-Za-z0-9_]{3,32}"));
}
static bool credential_valid(const std::string &value) {
    if (value.find('\0') != std::string::npos)
        return false;
    try {
        auto letters = wide(value);
        std::size_t count = 0;
        for (auto c : letters)
            if (c < 0xdc00 || c > 0xdfff)
                count++;
        return count >= 8 && count <= 128;
    } catch (...) {
        return false;
    }
}
static Identity identity(const Json &row) {
    return {row.at("id").get<std::string>(), row.at("username").get<std::string>(),
            row.at("native_uid").get<std::int64_t>()};
}
AccountStore::AccountStore(const fs::path &path) : db_(path) {
    db_.exec(R"SQL(
CREATE TABLE IF NOT EXISTS login_accounts(id TEXT PRIMARY KEY,username TEXT NOT NULL,username_key TEXT UNIQUE NOT NULL,salt BLOB NOT NULL,password_hash BLOB NOT NULL,iterations INTEGER NOT NULL,created_at REAL NOT NULL,native_uid INTEGER UNIQUE);
CREATE TABLE IF NOT EXISTS login_sessions(token_hash BLOB PRIMARY KEY,account_id TEXT NOT NULL REFERENCES login_accounts(id),expires_at REAL NOT NULL);
CREATE TABLE IF NOT EXISTS login_throttle(bucket TEXT PRIMARY KEY,failures INTEGER NOT NULL,until_at REAL NOT NULL);
CREATE TABLE IF NOT EXISTS game_tickets(token_hash BLOB PRIMARY KEY,account_id TEXT NOT NULL REFERENCES login_accounts(id),expires_at REAL NOT NULL,used INTEGER NOT NULL DEFAULT 0,session_hash BLOB);
CREATE TABLE IF NOT EXISTS game_ticket_locales(token_hash BLOB PRIMARY KEY REFERENCES game_tickets(token_hash) ON DELETE CASCADE,locale TEXT NOT NULL CHECK(locale IN ('pt','es','en')));
)SQL");
}
Identity AccountStore::password(const std::string &username, const std::string &password,
                                const std::string &peer) {
    auto bucket = hex(sha256("login:" + peer));
    double timestamp = now();
    auto blocked = db_.query("SELECT failures,until_at FROM login_throttle WHERE bucket=?", {bucket});
    if (!blocked.empty() && blocked[0]["failures"].get<int>() >= 5 &&
        blocked[0]["until_at"].get<double>() > timestamp)
        throw std::runtime_error("Muitas tentativas. Aguarde alguns minutos.");
    bool input_ok = username_valid(username) && credential_valid(password);
    auto rows = db_.query("SELECT id,username,salt,password_hash,iterations,native_uid FROM login_accounts "
                          "WHERE username_key=?",
                          {username_valid(username) ? lower_ascii(username) : std::string("invalid")});
    Bytes salt(32), expected(32);
    std::uint64_t rounds = 600000;
    if (!rows.empty()) {
        salt = unhex(rows[0]["salt"]);
        expected = unhex(rows[0]["password_hash"]);
        rounds = rows[0]["iterations"].get<std::uint64_t>();
    }
    bool valid =
        secure_equal(pbkdf2(credential_valid(password) ? password : "invalid-password", salt, rounds),
                     expected) &&
        !rows.empty() && input_ok;
    Transaction transaction(db_);
    if (!valid)
        db_.exec("INSERT INTO login_throttle VALUES(?,1,?) ON CONFLICT(bucket) DO UPDATE SET failures=CASE "
                 "WHEN until_at<=? THEN 1 ELSE failures+1 END,until_at=excluded.until_at",
                 {bucket, timestamp + 300, timestamp});
    else
        db_.exec("DELETE FROM login_throttle WHERE bucket=?", {bucket});
    transaction.commit();
    if (!valid)
        throw std::runtime_error("Senha errada");
    return identity(rows[0]);
}
Identity AccountStore::consume_ticket(const std::string &username, const std::string &ticket) {
    if (!username_valid(username) || !std::regex_match(ticket, std::regex("[A-Za-z0-9_-]{43}")))
        throw std::runtime_error("Sessao de jogo invalida.");
    Transaction transaction(db_);
    auto timestamp = now();
    auto rows =
        db_.query("SELECT a.id,a.username,a.native_uid,COALESCE(l.locale,'pt') AS locale FROM game_tickets t JOIN login_accounts a ON "
                  "a.id=t.account_id JOIN login_sessions s ON s.token_hash=t.session_hash "
                  "LEFT JOIN game_ticket_locales l ON l.token_hash=t.token_hash WHERE "
                  "t.token_hash=? AND t.expires_at>? AND t.used=0 AND a.username_key=? AND s.expires_at>?",
                  {sha256(ticket), timestamp, lower_ascii(username), timestamp});
    if (rows.empty())
        throw std::runtime_error("Sessao de jogo expirada ou ja utilizada.");
    db_.exec("UPDATE game_tickets SET used=1 WHERE token_hash=? AND used=0", {sha256(ticket)});
    if (db_.changes() != 1)
        throw std::runtime_error("Sessao ja utilizada.");
    transaction.commit();
    auto result=identity(rows[0]);
    result.locale=rows[0].at("locale").get<std::string>();
    return result;
}
Identity AccountStore::register_account(const std::string &username, const std::string &password) {
    if (!username_valid(username) || !credential_valid(password))
        throw std::runtime_error("Usuario ou senha fora do formato permitido.");
    auto salt = random_bytes(32), hash = pbkdf2(password, salt, 600000);
    auto id = "user:" + hex(random_bytes(16));
    Transaction transaction(db_);
    auto uid = db_.query("SELECT COALESCE(MAX(native_uid),1000)+1 AS uid FROM login_accounts")[0]["uid"]
                   .get<std::int64_t>();
    db_.exec("INSERT INTO "
             "login_accounts(id,username,username_key,salt,password_hash,iterations,created_at,native_uid) "
             "VALUES(?,?,?,?,?,?,?,?)",
             {id, username, lower_ascii(username), salt, hash, std::int64_t(600000), now(), uid});
    transaction.commit();
    return {id, username, uid};
}

LegacyStore::LegacyStore(const fs::path &path) : db_(path) {
    db_.exec("CREATE TABLE IF NOT EXISTS accounts(id TEXT PRIMARY KEY,payload TEXT NOT NULL)");
}
Json LegacyStore::account(const std::string &id) {
    auto rows = db_.query("SELECT payload FROM accounts WHERE id=?", {id});
    if (rows.empty())
        return Json{{"chars", Json::array()}, {"next_eid", 0x2892}};
    auto state = Json::parse(rows[0]["payload"].get<std::string>());
    if (!state.at("chars").is_array())
        throw std::runtime_error("Invalid existing character payload");
    return state;
}
Json LegacyStore::characters(const std::string &id) {
    return account(id).at("chars");
}
Json LegacyStore::character(const std::string &account_id, std::uint32_t eid) {
    for (const auto &character : characters(account_id))
        if (character.at("eid").get<std::uint32_t>() == eid)
            return character;
    throw std::runtime_error("Personagem nao pertence a esta conta.");
}
static std::wstring folded(const std::string &text) {
    auto input = wide(text);
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, input.data(),
                          static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr, 0);
    std::wstring output(n, 0);
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, input.data(), static_cast<int>(input.size()),
                  output.data(), n, nullptr, nullptr, 0);
    return output;
}
Json LegacyStore::create_character(const std::string &id, const std::string &name, int cls,
                                   const std::string &appearance, const Json &vitals) {
    static const std::set<int> valid = {100, 200, 300, 400, 500, 600, 800};
    if (!valid.count(cls))
        throw std::runtime_error("Classe invalida.");
    auto letters = wide(name);
    std::size_t count = 0;
    for (wchar_t letter : letters) {
        if (letter < 32 || letter == L'/' || letter == L'\\' || letter == 127)
            throw std::runtime_error("Nome invalido.");
        if (letter < 0xdc00 || letter > 0xdfff)
            count++;
    }
    if (count < 1 || count > 20)
        throw std::runtime_error("Nome invalido.");
    if (!appearance.empty() && unhex(appearance).size() != 8)
        throw std::runtime_error("Aparencia invalida.");
    Transaction transaction(db_);
    auto state = account(id);
    if (state["chars"].size() >= 4)
        throw std::runtime_error("Limite de personagens atingido.");
    std::uint32_t eid = state.value("next_eid", 0x2892u);
    for (const auto &row : db_.query("SELECT payload FROM accounts")) {
        auto saved = Json::parse(row["payload"].get<std::string>());
        eid = std::max(eid, saved.value("next_eid", 0x2892u));
        for (const auto &ch : saved["chars"]) {
            if (folded(ch["name"].get<std::string>()) == folded(name))
                throw std::runtime_error("Nome de personagem ja utilizado.");
            auto current = ch["eid"].get<std::uint32_t>();
            if (current == 0xffffffff)
                throw std::runtime_error("Entity capacity exceeded");
            eid = std::max(eid, current + 1);
        }
    }
    if (eid == 0xffffffff)
        throw std::runtime_error("Entity capacity exceeded");
    auto row = vitals.at(std::to_string(cls)).at("1");
    int hp = row.at("maxhp"), mp = row.at("maxmp");
    Json ch = {{"slot", state["chars"].size()},
               {"eid", eid},
               {"name", name},
               {"cls", cls},
               {"level", 1},
               {"exp", 0},
               {"hp", hp},
               {"mp", mp},
               {"maxhp", hp},
               {"maxmp", mp},
               {"progression_version", 2},
               {"starter_eligible", true},
               {"fletta_schema_version", 2},
               {"fletta", 0},
               {"fletta_energy", 0},
               {"moral_value", 0},
               {"gear", Json::object()}};
    if (!appearance.empty())
        ch["appearance"] = appearance;
    state["chars"].push_back(ch);
    state["next_eid"] = eid + 1;
    db_.exec(
        "INSERT INTO accounts(id,payload) VALUES(?,?) ON CONFLICT(id) DO UPDATE SET payload=excluded.payload",
        {id, state.dump()});
    transaction.commit();
    return ch;
}
void LegacyStore::update_character(const std::string &id, std::uint32_t eid,
                                   const std::function<void(Json &)> &mutation) {
    Transaction transaction(db_);
    auto state = account(id);
    bool found = false;
    for (auto &character : state["chars"]) {
        if (character.at("eid").get<std::uint32_t>() != eid)
            continue;
        auto old_id = character.at("eid");
        mutation(character);
        if (character.at("eid") != old_id)
            throw std::runtime_error("Immutable character identity");
        found = true;
        break;
    }
    if (!found)
        throw std::runtime_error("Personagem nao pertence a esta conta.");
    db_.exec("UPDATE accounts SET payload=? WHERE id=?", {state.dump(), id});
    transaction.commit();
}
void LegacyStore::checkpoint(const std::string &id, const Json &value) {
    auto eid = value.at("eid").get<std::uint32_t>();
    update_character(id, eid, [&](Json &character) {
        for (auto it = value.begin(); it != value.end(); ++it)
            character[it.key()] = it.value();
    });
}
bool LegacyStore::delete_character(const std::string &id, std::uint32_t eid) {
    Transaction transaction(db_);
    auto state = account(id);
    auto &rows = state["chars"];
    auto before = rows.size();
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [eid](const Json &ch) { return ch.at("eid").get<std::uint32_t>() == eid; }),
               rows.end());
    if (rows.size() == before) {
        transaction.commit();
        return false;
    }
    for (std::size_t i = 0; i < rows.size(); i++)
        rows[i]["slot"] = i;
    db_.exec("UPDATE accounts SET payload=? WHERE id=?", {state.dump(), id});
    transaction.commit();
    return true;
}
SessionStore::SessionStore(const fs::path &path) : db_(path) {
    db_.exec("CREATE TABLE IF NOT EXISTS native_sessions(id TEXT PRIMARY KEY,payload TEXT NOT NULL,expires "
             "REAL NOT NULL,consumed INTEGER NOT NULL DEFAULT 0)");
}
void SessionStore::save(const Session &value) {
    Json data = {{"account", value.identity.id},       {"username", value.identity.username},
                 {"uid", value.identity.native_uid},   {"fingerprint", hex(value.fingerprint)},
                 {"proof", hex(value.password_proof)}, {"method", value.method},
                 {"channel", value.channel},           {"code", value.channel_code},
                 {"locale", value.identity.locale}};
    Transaction tx(db_);
    db_.exec("DELETE FROM native_sessions WHERE expires<?", {now()});
    db_.exec("INSERT INTO native_sessions(id,payload,expires,consumed) VALUES(?,?,?,0) ON CONFLICT(id) DO "
             "UPDATE SET payload=excluded.payload,expires=excluded.expires,consumed=0",
             {value.id, data.dump(), value.expires});
    tx.commit();
}
Session SessionStore::load(const std::string &id) {
    auto rows = db_.query(
        "SELECT payload,expires FROM native_sessions WHERE id=? AND expires>? AND consumed=0", {id, now()});
    if (rows.empty())
        throw std::runtime_error("Sessao expirada.");
    auto data = Json::parse(rows[0]["payload"].get<std::string>());
    Session result;
    result.id = id;
    result.identity = {data["account"], data["username"], data["uid"]};
    result.identity.locale=data.value("locale",std::string("pt"));
    result.fingerprint = unhex(data["fingerprint"]);
    result.password_proof = unhex(data["proof"]);
    result.method = data["method"];
    result.expires = rows[0]["expires"];
    result.channel = data["channel"];
    result.channel_code = data["code"];
    return result;
}
void SessionStore::consume(const std::string &id) {
    Transaction tx(db_);
    db_.exec("UPDATE native_sessions SET consumed=1 WHERE id=? AND consumed=0 AND expires>?", {id, now()});
    if (db_.changes() != 1)
        throw std::runtime_error("Sessao ja utilizada.");
    tx.commit();
}
} // namespace rosh
