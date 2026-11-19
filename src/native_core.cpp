#include "native/core.hpp"
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>

namespace rosh {
double now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string utf8(const std::wstring &value) {
    if (value.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                nullptr, 0, nullptr, nullptr);
    if (!n)
        throw std::runtime_error("Invalid Unicode");
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring wide(const std::string &value) {
    if (value.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        out.data(), n);
    return out;
}
Bytes read_file(const fs::path &path, std::size_t limit) {
    auto size = fs::file_size(path);
    if (size > limit)
        throw std::runtime_error("File limit: " + path.filename().string());
    Bytes data(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input || !input.read(reinterpret_cast<char *>(data.data()), data.size()))
        throw std::runtime_error("Cannot read: " + path.filename().string());
    return data;
}
void write_file(const fs::path &path, const Bytes &bytes) {
    fs::create_directories(path.parent_path());
    auto temp = path;
    temp += L".writing";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()))
            throw std::runtime_error("Write failed");
        output.flush();
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Atomic install failed");
}
std::string hex(const Bytes &data) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (auto c : data) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}
Bytes unhex(const std::string &value) {
    if (value.size() % 2)
        throw std::runtime_error("Invalid hex");
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        throw std::runtime_error("Invalid hex");
    };
    Bytes out;
    for (std::size_t i = 0; i < value.size(); i += 2)
        out.push_back(static_cast<unsigned char>((digit(value[i]) << 4) | digit(value[i + 1])));
    return out;
}
static void checked(NTSTATUS result, const char *what) {
    if (result < 0)
        throw std::runtime_error(std::string(what) +
                                 " failed: " + std::to_string(static_cast<unsigned long>(result)));
}
Bytes random_bytes(std::size_t size) {
    if (size > 1024 * 1024)
        throw std::runtime_error("Random request limit");
    Bytes out(size);
    checked(BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG),
            "Random");
    return out;
}
Bytes sha256(const Bytes &bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    checked(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0), "SHA256 provider");
    Bytes result(32);
    try {
        checked(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0), "SHA256 create");
        checked(BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0),
                "SHA256 data");
        checked(BCryptFinishHash(hash, result.data(), 32, 0), "SHA256 finish");
    } catch (...) {
        if (hash)
            BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}
Bytes sha256(const std::string &text) {
    return sha256(Bytes(text.begin(), text.end()));
}
Bytes pbkdf2(const std::string &password, const Bytes &salt, std::uint64_t iterations) {
    if (iterations < 10000 || iterations > 10000000 || salt.size() > 1024)
        throw std::runtime_error("Invalid password parameters");
    BCRYPT_ALG_HANDLE algorithm{};
    checked(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                        BCRYPT_ALG_HANDLE_HMAC_FLAG),
            "PBKDF2 provider");
    Bytes out(32);
    auto status =
        BCryptDeriveKeyPBKDF2(algorithm, reinterpret_cast<PUCHAR>(const_cast<char *>(password.data())),
                              static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt.data()),
                              static_cast<ULONG>(salt.size()), iterations, out.data(), 32, 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    checked(status, "PBKDF2");
    return out;
}
bool secure_equal(const Bytes &a, const Bytes &b) {
    if (a.size() != b.size())
        return false;
    unsigned char difference = 0;
    for (std::size_t i = 0; i < a.size(); i++)
        difference |= a[i] ^ b[i];
    return difference == 0;
}
std::string token(std::size_t count) {
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    auto bytes = random_bytes(count);
    std::string result;
    unsigned bits = 0, acc = 0;
    for (auto byte : bytes) {
        acc = (acc << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            result += alphabet[(acc >> bits) & 63];
        }
    }
    if (bits)
        result += alphabet[(acc << (6 - bits)) & 63];
    return result;
}
void protect_key(const fs::path &path, const Bytes &key) {
    DATA_BLOB input{static_cast<DWORD>(key.size()), const_cast<BYTE *>(key.data())}, out{};
    if (!CryptProtectData(&input, L"ROSH encrypted server content", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        throw std::runtime_error("DPAPI protect failed");
    Bytes bytes(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    write_file(path, bytes);
}
Bytes unprotect_key(const fs::path &path) {
    auto bytes = read_file(path, 8192);
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), bytes.data()}, out{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        throw std::runtime_error("This Windows account cannot open the content key");
    Bytes result(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (result.size() != 32)
        throw std::runtime_error("Invalid content key");
    return result;
}
static Bytes crypt(bool encrypt, const Bytes &input, const Bytes &key, Bytes &nonce, Bytes &tag,
                   const std::string &aad) {
    if (key.size() != 32 || nonce.size() != 12 || tag.size() != 16)
        throw std::runtime_error("Invalid AES-GCM parameters");
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_KEY_HANDLE handle{};
    checked(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0), "AES provider");
    Bytes output(input.size());
    try {
        checked(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                                  reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
                                  sizeof(BCRYPT_CHAIN_MODE_GCM), 0),
                "AES-GCM mode");
        checked(
            BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0, const_cast<PUCHAR>(key.data()), 32, 0),
            "AES key");
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce.data();
        info.cbNonce = 12;
        info.pbTag = tag.data();
        info.cbTag = 16;
        info.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char *>(aad.data()));
        info.cbAuthData = static_cast<ULONG>(aad.size());
        ULONG length{};
        auto status = encrypt ? BCryptEncrypt(handle, const_cast<PUCHAR>(input.data()),
                                              static_cast<ULONG>(input.size()), &info, nullptr, 0,
                                              output.data(), static_cast<ULONG>(output.size()), &length, 0)
                              : BCryptDecrypt(handle, const_cast<PUCHAR>(input.data()),
                                              static_cast<ULONG>(input.size()), &info, nullptr, 0,
                                              output.data(), static_cast<ULONG>(output.size()), &length, 0);
        checked(status, encrypt ? "AES-GCM encrypt" : "Content authentication");
        output.resize(length);
    } catch (...) {
        if (handle)
            BCryptDestroyKey(handle);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
    BCryptDestroyKey(handle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return output;
}
Bytes encrypt_content(const Bytes &plain, const Bytes &key, const std::string &category) {
    auto nonce = random_bytes(12);
    Bytes tag(16);
    std::string aad = "ROSHDAT1:" + category;
    auto cipher = crypt(true, plain, key, nonce, tag, aad);
    Bytes out{'R', 'O', 'S', 'H', 'D', 'A', 'T', '1'};
    out.insert(out.end(), nonce.begin(), nonce.end());
    out.insert(out.end(), tag.begin(), tag.end());
    out.insert(out.end(), cipher.begin(), cipher.end());
    return out;
}
Bytes decrypt_content(const Bytes &bytes, const Bytes &key, const std::string &category) {
    if (bytes.size() < 36 || std::string(bytes.begin(), bytes.begin() + 8) != "ROSHDAT1")
        throw std::runtime_error("Invalid content envelope");
    Bytes nonce(bytes.begin() + 8, bytes.begin() + 20), tag(bytes.begin() + 20, bytes.begin() + 36),
        cipher(bytes.begin() + 36, bytes.end());
    return crypt(false, cipher, key, nonce, tag, "ROSHDAT1:" + category);
}

namespace {
class Statement {
    sqlite3_stmt *value_{};

  public:
    Statement(sqlite3 *db, const std::string &sql, const std::vector<SqlValue> &params) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &value_, nullptr) != SQLITE_OK)
            throw SqliteError(sqlite3_extended_errcode(db),sqlite3_errmsg(db));
        for (std::size_t i = 0; i < params.size(); i++) {
            int n = static_cast<int>(i + 1), result = SQLITE_ERROR;
            std::visit(
                [&](auto &&p) {
                    using T = std::decay_t<decltype(p)>;
                    if constexpr (std::is_same_v<T, std::nullptr_t>)
                        result = sqlite3_bind_null(value_, n);
                    else if constexpr (std::is_same_v<T, std::int64_t>)
                        result = sqlite3_bind_int64(value_, n, p);
                    else if constexpr (std::is_same_v<T, double>)
                        result = sqlite3_bind_double(value_, n, p);
                    else if constexpr (std::is_same_v<T, std::string>)
                        result = sqlite3_bind_text(value_, n, p.data(), static_cast<int>(p.size()),
                                                   SQLITE_TRANSIENT);
                    else
                        result = sqlite3_bind_blob(value_, n, p.data(), static_cast<int>(p.size()),
                                                   SQLITE_TRANSIENT);
                },
                params[i]);
            if (result != SQLITE_OK) {
                sqlite3_finalize(value_);
                value_ = nullptr;
                throw SqliteError(sqlite3_extended_errcode(db),sqlite3_errmsg(db));
            }
        }
    }
    ~Statement() {
        if (value_)
            sqlite3_finalize(value_);
    }
    sqlite3_stmt *get() {
        return value_;
    }
};
} // namespace
Sqlite::Sqlite(const fs::path &path, bool readonly) {
    if (!readonly && path.has_parent_path())
        fs::create_directories(path.parent_path());
    auto flags = (readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) |
                 SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(path.u8string().c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string error = db_ ? sqlite3_errmsg(db_) : "SQLite open failed";
        if (db_)
            sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(error);
    }
    sqlite3_busy_timeout(db_, 15000);
    try {
        if (!readonly) {
            exec("PRAGMA foreign_keys=ON");
            exec("PRAGMA journal_mode=WAL");
            exec("PRAGMA synchronous=FULL");
        }
    } catch (...) {
        sqlite3_close(db_);db_=nullptr;
        throw;
    }
}
Sqlite::Sqlite(const Bytes &image) : Sqlite(fs::path(":memory:")) {
    if (image.size() < 100 || image.size() > 256 * 1024 * 1024 ||
        std::memcmp(image.data(), "SQLite format 3\0", 16) != 0)
        throw std::runtime_error("Invalid encrypted SQLite image");
    auto memory = static_cast<unsigned char *>(sqlite3_malloc64(image.size()));
    if (!memory)
        throw std::bad_alloc();
    std::memcpy(memory, image.data(), image.size());
    memory[18] = memory[19] = 1;
    int result = sqlite3_deserialize(db_, "main", memory, static_cast<sqlite3_int64>(image.size()),
                                     static_cast<sqlite3_int64>(image.size()),
                                     SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_READONLY);
    if (result != SQLITE_OK) {
        // FREEONCLOSE transfers ownership even when deserialization fails.
        throw std::runtime_error("Cannot load readonly SQLite content into memory");
    }
    exec("PRAGMA query_only=ON");
}
Sqlite::~Sqlite() {
    if (db_)
        sqlite3_close(db_);
}
void Sqlite::exec(const std::string &sql, const std::vector<SqlValue> &params) {
    std::lock_guard lock(mutex_);
    if (params.empty()) {
        char *message{};
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
            std::string error = message ? message : sqlite3_errmsg(db_);
            sqlite3_free(message);
            throw SqliteError(sqlite3_extended_errcode(db_),error);
        }
        return;
    }
    Statement statement(db_, sql, params);
    if (sqlite3_step(statement.get()) != SQLITE_DONE)
        throw SqliteError(sqlite3_extended_errcode(db_),sqlite3_errmsg(db_));
}
Json Sqlite::query(const std::string &sql, const std::vector<SqlValue> &params) {
    std::lock_guard lock(mutex_);
    Statement statement(db_, sql, params);
    Json rows = Json::array();
    int status;
    while ((status = sqlite3_step(statement.get())) == SQLITE_ROW) {
        Json row = Json::object();
        for (int col = 0; col < sqlite3_column_count(statement.get()); col++) {
            auto name = sqlite3_column_name(statement.get(), col);
            switch (sqlite3_column_type(statement.get(), col)) {
            case SQLITE_INTEGER:
                row[name] = sqlite3_column_int64(statement.get(), col);
                break;
            case SQLITE_FLOAT:
                row[name] = sqlite3_column_double(statement.get(), col);
                break;
            case SQLITE_TEXT: {
                auto p = reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), col));
                row[name] = std::string(p, p + sqlite3_column_bytes(statement.get(), col));
                break;
            }
            case SQLITE_BLOB: {
                auto p = static_cast<const unsigned char *>(sqlite3_column_blob(statement.get(), col));
                row[name] = hex(Bytes(p, p + sqlite3_column_bytes(statement.get(), col)));
                break;
            }
            default:
                row[name] = nullptr;
            }
        }
        rows.push_back(std::move(row));
    }
    if (status != SQLITE_DONE)
        throw SqliteError(sqlite3_extended_errcode(db_),sqlite3_errmsg(db_));
    return rows;
}
std::int64_t Sqlite::changes() {
    std::lock_guard lock(mutex_);
    return sqlite3_changes(db_);
}
void snapshot_database(const fs::path &source, const fs::path &target) {
    if (fs::exists(target))
        throw std::runtime_error("Snapshot destination already exists");
    Sqlite reader(source, true), writer(target);
    auto backup = sqlite3_backup_init(writer.handle(), "main", reader.handle(), "main");
    if (!backup)
        throw std::runtime_error(sqlite3_errmsg(writer.handle()));
    int status;
    do {
        status = sqlite3_backup_step(backup, 128);
        if (status == SQLITE_BUSY || status == SQLITE_LOCKED)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (status == SQLITE_OK || status == SQLITE_BUSY || status == SQLITE_LOCKED);
    int finish = sqlite3_backup_finish(backup);
    if (status != SQLITE_DONE || finish != SQLITE_OK)
        throw std::runtime_error("Consistent SQLite snapshot failed");
    if (writer.query("PRAGMA quick_check")[0].begin().value() != "ok")
        throw std::runtime_error("SQLite snapshot integrity failed");
}
Content::Content(const fs::path &root)
    : root_(root), key_(unprotect_key(root / "Config/Keys/content.key.dpapi")) {
    auto bytes = read_file(root / "Data/catalog.dat");
    auto plain = decrypt_content(bytes, key_, "catalog");
    manifest_ = Json::parse(plain);
}
Bytes serialize_database(const fs::path &source) {
    Sqlite reader(source, true);
    reader.exec("BEGIN");
    sqlite3_int64 length{};
    auto memory = sqlite3_serialize(reader.handle(), "main", &length, 0);
    if (!memory || length < 100 || length > 256 * 1024 * 1024) {
        if (memory)
            sqlite3_free(memory);
        throw std::runtime_error("SQLite content serialization failed");
    }
    Bytes result(memory, memory + length);
    sqlite3_free(memory);
    reader.exec("COMMIT");
    Sqlite verify(result);
    if (verify.query("PRAGMA quick_check")[0].begin().value() != "ok")
        throw std::runtime_error("Serialized SQLite content integrity failure");
    return result;
}
static Bytes verified_content_file(const fs::path &path,const std::string &expected) {
    auto bytes=read_file(path);
    auto digest=hex(sha256(bytes));
    if(digest==expected)return bytes;
    const auto first=digest;
    unsigned consecutive=0;
    // If a read disagrees with the authenticated catalog, reopen the file and
    // require two consecutive catalog matches before using the recovered bytes.
    for(unsigned attempt=0;attempt<3;++attempt){
        bytes=read_file(path);
        digest=hex(sha256(bytes));
        consecutive=digest==expected?consecutive+1:0;
        if(consecutive==2)return bytes;
    }
    throw std::runtime_error("Content digest mismatch: "+path.filename().string()+" expected="+expected+
                             " first="+first+" last="+digest+" bytes="+std::to_string(bytes.size()));
}

std::unique_ptr<Sqlite> Content::database(const std::string &name) {
    std::lock_guard lock(mutex_);
    auto row = manifest_.at("resources").at(name);
    if (row.value("format", "") != "sqlite-image")
        throw std::runtime_error("Resource is not a SQLite image");
    auto filename = row.at("file").get<std::string>();
    if (filename.find("..") != std::string::npos || filename.find(':') != std::string::npos ||
        filename.find('\\') != std::string::npos)
        throw std::runtime_error("Invalid encrypted database path");
    auto encrypted=verified_content_file(root_/"Data"/filename,row.at("sha256"));
    return std::make_unique<Sqlite>(decrypt_content(encrypted, key_, name));
}
bool Content::has(const std::string&name){std::lock_guard lock(mutex_);return manifest_.at("resources").contains(name);}
Json Content::get(const std::string &name) {
    std::lock_guard lock(mutex_);
    if (cache_.count(name))
        return cache_[name];
    if (!manifest_["resources"].contains(name))
        throw std::runtime_error("Content resource missing: " + name);
    auto row = manifest_["resources"][name];
    auto path = row["file"].get<std::string>();
    if (path.find("..") != std::string::npos || path.find(':') != std::string::npos ||
        path.find('\\') != std::string::npos)
        throw std::runtime_error("Invalid content path");
    if (!bundles_.count(path)) {
        auto bytes=verified_content_file(root_/"Data"/path,row.at("sha256"));
        auto plain = decrypt_content(bytes, key_, row.value("category", name));
        bundles_[path] = Json::parse(plain);
    }
    cache_[name] =
        row.contains("member") ? bundles_[path].at(row["member"].get<std::string>()) : bundles_[path];
    return cache_[name];
}
} // namespace rosh
