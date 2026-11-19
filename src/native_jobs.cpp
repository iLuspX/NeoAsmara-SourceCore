#include "native/runtime.hpp"
#include <algorithm>
#include <sstream>
namespace rosh {
void RuntimeLog::write(const std::string &level, const std::string &message) {
    std::lock_guard lock(mutex_);
    Json row = {{"time", now()}, {"level", level}, {"message", message}};
    auto text = row.dump();
    if (fs::exists(file_) && fs::file_size(file_) > 20 * 1024 * 1024) {
        auto prior = file_;
        prior += L".previous";
        std::error_code error;
        fs::remove(prior, error);
        fs::rename(file_, prior, error);
    }
    std::ofstream file(file_, std::ios::app | std::ios::binary);
    file << text << "\n";
    lines_.push_back(text);
    if (lines_.size() > 200)
        lines_.erase(lines_.begin(), lines_.begin() + 50);
}
std::vector<std::string> RuntimeLog::tail(std::size_t count) {
    std::lock_guard lock(mutex_);
    auto begin = lines_.size() > count ? lines_.size() - count : 0;
    return {lines_.begin() + begin, lines_.end()};
}
JobQueue::JobQueue(const fs::path &root) : db_(root / "Database/runtime.sqlite") {
    db_.exec("CREATE TABLE IF NOT EXISTS native_jobs(id TEXT PRIMARY KEY,target TEXT NOT NULL,kind TEXT NOT "
             "NULL,payload TEXT NOT NULL,created REAL NOT NULL,status TEXT NOT NULL,result TEXT); CREATE "
             "TABLE IF NOT EXISTS native_services(instance TEXT PRIMARY KEY,pid INTEGER NOT NULL,updated "
             "REAL NOT NULL,payload TEXT NOT NULL)");
}
std::string JobQueue::enqueue(const std::string &target, const std::string &kind, const Json &body) {
    auto id = token();
    db_.exec("INSERT INTO native_jobs VALUES(?,?,?,?,?,'pending',NULL)",
             {id, target, kind, body.dump(), now()});
    return id;
}
Json JobQueue::wait(const std::string &id, double seconds) {
    double end = now() + seconds;
    while (now() < end) {
        auto rows = db_.query("SELECT status,result FROM native_jobs WHERE id=?", {id});
        if (!rows.empty() && rows[0]["status"] != "pending") {
            auto result = Json::parse(rows[0]["result"].get<std::string>());
            if (rows[0]["status"] != "done")
                throw std::runtime_error(result.value("error", "Internal service rejected request"));
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("Internal service did not answer before timeout");
}
Json JobQueue::pending(const std::string &target) {
    return db_.query("SELECT id,kind,payload FROM native_jobs WHERE target=? AND status='pending' AND "
                     "created>? ORDER BY created LIMIT 32",
                     {target, now() - 10});
}
void JobQueue::finish(const std::string &id, const Json &value, bool ok) {
    db_.exec("UPDATE native_jobs SET status=?,result=? WHERE id=? AND status='pending'",
             {std::string(ok ? "done" : "failed"), value.dump(), id});
}
void JobQueue::heartbeat(const std::string &name, const Json &state) {
    db_.exec("INSERT INTO native_services VALUES(?,?,?,?) ON CONFLICT(instance) DO UPDATE SET "
             "pid=excluded.pid,updated=excluded.updated,payload=excluded.payload",
             {name, std::int64_t(GetCurrentProcessId()), now(), state.dump()});
}
Json JobQueue::services() {
    auto rows = db_.query("SELECT instance,pid,updated,payload FROM native_services ORDER BY instance");
    Json result = Json::array();
    for (const auto &row : rows) {
        auto status = Json::parse(row["payload"].get<std::string>());
        status["instance"] = row["instance"];
        status["pid"] = row["pid"];
        status["alive"] = row["updated"].get<double>() > now() - 4;
        result.push_back(status);
    }
    return result;
}

}
