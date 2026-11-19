#pragma once
#include "core.hpp"
#include <set>
#include <thread>

namespace rosh {
class WorldScene;
class RuntimeLog {
    fs::path file_;
    std::mutex mutex_;
    std::vector<std::string> lines_;

  public:
    explicit RuntimeLog(const fs::path &file) : file_(file) {
        fs::create_directories(file.parent_path());
    }
    void write(const std::string &level, const std::string &message);
    std::vector<std::string> tail(std::size_t count = 80);
};
class JobQueue {
    Sqlite db_;

  public:
    explicit JobQueue(const fs::path &root);
    std::string enqueue(const std::string &target, const std::string &kind, const Json &body);
    Json wait(const std::string &id, double seconds = 5);
    Json pending(const std::string &target);
    void finish(const std::string &id, const Json &result, bool ok = true);
    void heartbeat(const std::string &name, const Json &state);
    Json services();
};
class Peer {
    SOCKET socket_{INVALID_SOCKET};
    std::mutex send_mutex_;
    std::uint16_t sequence_{1};

  public:
    std::atomic<bool> authenticated{false};
    explicit Peer(SOCKET socket) : socket_(socket) {}
    ~Peer();
    SOCKET socket() const {
        return socket_;
    }
    void send(unsigned char category, unsigned char subcategory, const Bytes &body);
    void send_raw(const std::string &text);
    void stop();
};
std::int64_t lupinel_round_start(const fs::path &root);
void select_lupinel_challengers(Sqlite &db,std::int64_t start,int channel,std::int64_t timestamp);
class NativeService {
    fs::path root_;
    std::string role_, instance_;
    int channel_;
    Json config_;
    std::int64_t last_lupinel_selection_{};
    RuntimeLog log_;
    JobQueue jobs_;
    Content content_;
    std::shared_ptr<WorldScene> scene_;
    AccountStore accounts_;
    LegacyStore world_;
    SessionStore sessions_;
    Sqlite moderation_;
    std::atomic<bool> running_{false};
    std::atomic<int> users_{0};
    std::thread worker_;
    mutable std::mutex mutex_;
    SOCKET listener_{INVALID_SOCKET};
    std::set<SOCKET> handoffs_;
    struct Connection {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Connection> connections_;
    std::vector<std::shared_ptr<Peer>> peers_;
    HANDLE instance_mutex_{nullptr};
    void connection(std::function<void()> action);
    void reap();
    std::atomic<std::uint64_t> ticks_{0};
    double next_control_tick_{};
    SOCKET open_listener(int port, int *actual = nullptr);
    void run();
    void process_jobs();
    void accept_handoff(const Session &session, int stage, SOCKET listener);
    void serve(std::shared_ptr<Peer> peer, int stage, Session chain = {});
    void send_cards(Peer &peer, const Identity &identity, std::uint32_t only_eid = 0);
    int request_handoff(Session session, int stage);
    bool banned(const Session &session, std::uint32_t eid = 0);

  public:
    NativeService(fs::path root, std::string role, int channel = 1);
    ~NativeService();
    void start();
    void stop();
    bool running() const {
        return running_;
    }
    Json status();
    Json cluster() {
        return jobs_.services();
    }
    void command(const std::string &name, const Json &payload = Json::object());
    void operator_console(const std::string &text);
    std::vector<std::string> log_tail() {
        return log_.tail();
    }
    const fs::path &root() const {
        return root_;
    }
    const std::string &role() const {
        return role_;
    }
};
int server_application(HINSTANCE instance, int show, const std::string &role,
                       const std::wstring &command_line);
} // namespace rosh
