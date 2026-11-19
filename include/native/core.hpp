#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Winsock must precede windows.h; BCrypt and DPAPI require Windows types.
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace rosh {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Bytes = std::vector<unsigned char>;
using SqlValue = std::variant<std::nullptr_t, std::int64_t, double, std::string, Bytes>;
double now();
std::string utf8(const std::wstring &value);
std::wstring wide(const std::string &value);
Bytes read_file(const fs::path &path, std::size_t limit = 256 * 1024 * 1024);
void write_file(const fs::path &path, const Bytes &data);
std::string hex(const Bytes &data);
Bytes unhex(const std::string &value);
Bytes random_bytes(std::size_t size);
Bytes sha256(const Bytes &bytes);
Bytes sha256(const std::string &text);
Bytes pbkdf2(const std::string &password, const Bytes &salt, std::uint64_t iterations);
bool secure_equal(const Bytes &a, const Bytes &b);
std::string token(std::size_t bytes = 32);
void protect_key(const fs::path &path, const Bytes &key);
Bytes unprotect_key(const fs::path &path);
Bytes encrypt_content(const Bytes &plain, const Bytes &key, const std::string &category);
Bytes decrypt_content(const Bytes &envelope, const Bytes &key, const std::string &category);
void snapshot_database(const fs::path &source, const fs::path &destination);
Bytes serialize_database(const fs::path &source);

class SqliteError : public std::runtime_error {
    int code_;
  public:
    SqliteError(int code,const std::string& message):std::runtime_error(message),code_(code){}
    bool contention() const {return (code_&255)==SQLITE_BUSY||(code_&255)==SQLITE_LOCKED;}
};
class Sqlite {
    sqlite3 *db_{};
    std::recursive_mutex mutex_;

  public:
    explicit Sqlite(const fs::path &path, bool readonly = false);
    explicit Sqlite(const Bytes &readonly_image);
    ~Sqlite();
    Sqlite(const Sqlite &) = delete;
    Sqlite &operator=(const Sqlite &) = delete;
    sqlite3 *handle() {
        return db_;
    }
    std::recursive_mutex &mutex() {
        return mutex_;
    }
    void exec(const std::string &sql, const std::vector<SqlValue> &parameters = {});
    Json query(const std::string &sql, const std::vector<SqlValue> &parameters = {});
    std::int64_t changes();
};
class Transaction {
    Sqlite &db_;
    std::unique_lock<std::recursive_mutex> lock_;
    bool committed_{};
    std::string savepoint_;

  public:
    explicit Transaction(Sqlite &db) : db_(db), lock_(db.mutex()) {
        if (sqlite3_get_autocommit(db_.handle()))
            db_.exec("BEGIN IMMEDIATE");
        else {
            static std::atomic<std::uint64_t> sequence{0};
            savepoint_ = "rosh_tx_" + std::to_string(++sequence);
            db_.exec("SAVEPOINT " + savepoint_);
        }
    }
    ~Transaction() {
        if (!committed_)
            try {
                if (savepoint_.empty())
                    db_.exec("ROLLBACK");
                else {
                    db_.exec("ROLLBACK TO SAVEPOINT " + savepoint_);
                    db_.exec("RELEASE SAVEPOINT " + savepoint_);
                }
            } catch (...) {
            }
    }
    void commit() {
        db_.exec(savepoint_.empty() ? "COMMIT" : "RELEASE SAVEPOINT " + savepoint_);
        committed_ = true;
    }
};

struct Identity {
    std::string id, username;
    std::int64_t native_uid{};
    std::string locale{"pt"};
};
struct LoginFields {
    std::string username, credential;
    Bytes fingerprint;
};
struct Session {
    Identity identity;
    Bytes fingerprint, password_proof;
    std::string method, id;
    double expires{};
    int channel{1};
    std::uint32_t channel_code{};
};
class AccountStore {
    Sqlite db_;

  public:
    explicit AccountStore(const fs::path &path);
    Identity password(const std::string &username, const std::string &password, const std::string &peer);
    Identity consume_ticket(const std::string &username, const std::string &ticket);
    Identity register_account(const std::string &username, const std::string &password);
    Sqlite &db() {
        return db_;
    }
};
class Content {
    fs::path root_;
    Bytes key_;
    Json manifest_;
    std::mutex mutex_;
    std::map<std::string, Json> cache_, bundles_;

  public:
    explicit Content(const fs::path &root);
    Json get(const std::string &name);
    bool has(const std::string &name);
    std::unique_ptr<Sqlite> database(const std::string &name);
    Json status() const {
        return manifest_;
    }
};
class LegacyStore {
    Sqlite db_;

  public:
    explicit LegacyStore(const fs::path &path);
    Json account(const std::string &id);
    Json character(const std::string &account, std::uint32_t eid);
    Json create_character(const std::string &account, const std::string &name, int cls,
                          const std::string &appearance, const Json &class_vitals);
    void update_character(const std::string &account, std::uint32_t eid,
                          const std::function<void(Json &)> &mutation);
    bool delete_character(const std::string &account, std::uint32_t eid);
    Json characters(const std::string &account);
    void checkpoint(const std::string &account, const Json &character);
    Sqlite &db() {
        return db_;
    }
};
class SessionStore {
    Sqlite db_;

  public:
    explicit SessionStore(const fs::path &path);
    void save(const Session &value);
    Session load(const std::string &id);
    void consume(const std::string &id);
};
LoginFields parse_login(const Bytes &frame);
Bytes make_frame(unsigned char category, unsigned char subcategory, const Bytes &body,
                 std::uint16_t sequence);
std::vector<Bytes> extract_frames(Bytes &buffer);
Bytes notice(const std::string &text);
void put16(Bytes &bytes, std::size_t offset, std::uint16_t value, bool big = false);
void put32(Bytes &bytes, std::size_t offset, std::uint32_t value, bool big = false);
void putfloat(Bytes &bytes, std::size_t offset, float value);
std::uint16_t get16(const Bytes &bytes, std::size_t offset, bool big = false);
std::uint32_t get32(const Bytes &bytes, std::size_t offset, bool big = false);
float getfloat(const Bytes &bytes, std::size_t offset);
} // namespace rosh
