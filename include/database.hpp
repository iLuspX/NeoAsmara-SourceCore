#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <sqlite3.h> // Cabecalho distribuido em include/, API publica do SQLite
#include <string>
#include <vector>

namespace karos {

struct Character {
    std::int64_t eid{};
    std::string account_id;
    int slot{};
    std::string name;
    int cls{100};
    int level{1};
    std::int64_t exp{};
    int skill_points{};
    int fleta_points{};
    int fleta_progress{};
    int hp{186}, max_hp{186}, mp{100}, max_mp{100};
    std::string map_name{"berneo"};
    double x{9775.0}, y{8124.8}, z{672.7};
    std::int64_t gold{};
};

struct ItemStack {
    int slot{};
    int item_id{};
    int quantity{};
    int enhancement{};
};

class Database {
public:
    explicit Database(std::string path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void migrate();
    // Copy every ordinary table from the supplied reference SQLite database into
    // this database as reference_<table>. Returns the number of tables imported.
    std::size_t import_reference_content(const std::string& source_path);
    const std::string& path() const noexcept { return path_; }

    void ensure_account(const std::string& account_id);
    bool account_exists(const std::string& account_id);
    bool character_exists(std::int64_t eid);
    std::vector<Character> characters_for_account(const std::string& account_id);
    std::optional<Character> get_character(std::int64_t eid);
    std::optional<Character> get_character_by_name(const std::string& name);
    std::int64_t create_character(const std::string& account_id, const std::string& name, int class_id);
    bool delete_character(const std::string& account_id, std::int64_t eid);
    void set_level(const std::string& character_name, int level);
    void grant_exp(std::int64_t eid, std::int64_t amount);
    void add_gold(std::int64_t eid, std::int64_t amount);
    int add_item(std::int64_t eid, int item_id, int quantity);
    std::vector<ItemStack> inventory(std::int64_t eid);
    void set_position(const std::string& character_name, const std::string& map_name,
                      double x, double y, double z);
    void add_announcement(const std::string& actor, const std::string& message);
    void queue_spawn(const std::string& actor, int npc_id, const std::string& map_name,
                     double x, double y, double z, int count);

    void set_gm_role(const std::string& account_id, const std::string& role,
                     const std::string& granted_by = "console");
    std::string gm_role(const std::string& account_id);
    std::vector<std::pair<std::string, std::string>> gm_accounts();
    void audit(const std::string& actor, const std::string& action,
               const std::string& target, const std::string& details);
    std::vector<std::string> audit_tail(int count = 25);

    // Exposed for focused tests and read-only admin diagnostics. Use prepared
    // statements for user input; this handle is not intended for protocol code.
    sqlite3* native_handle() noexcept { return db_; }

private:
    std::string path_;
    sqlite3* db_{};
    void exec(const std::string& sql);
    bool has_column(const std::string& table, const std::string& column);
    void add_column_if_missing(const std::string& table, const std::string& column,
                               const std::string& definition);
    static int class_id_from_name_or_number(const std::string& value);
};

} // namespace karos
