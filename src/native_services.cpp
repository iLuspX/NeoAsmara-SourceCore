#include "native/game.hpp"
#include "native/runtime.hpp"
#include "native/control.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <ws2tcpip.h>

namespace rosh {
Peer::~Peer() {
    if (socket_ != INVALID_SOCKET)
        closesocket(socket_);
}
static void send_all(SOCKET socket, const unsigned char *bytes, std::size_t count) {
    while (count) {
        int n = ::send(socket, reinterpret_cast<const char *>(bytes),
                       static_cast<int>(std::min<std::size_t>(count, 32768)), 0);
        if (n <= 0)
            throw std::runtime_error("Client connection closed while sending");
        bytes += n;
        count -= n;
    }
}
void Peer::send(unsigned char category, unsigned char sub, const Bytes &body) {
    std::lock_guard lock(send_mutex_);
    auto bytes = make_frame(category, sub, body, sequence_++);
    send_all(socket_, bytes.data(), bytes.size());
}
void Peer::send_raw(const std::string &text) {
    std::lock_guard lock(send_mutex_);
    send_all(socket_, reinterpret_cast<const unsigned char *>(text.data()), text.size());
}
void Peer::stop() {
    ::shutdown(socket_, SD_BOTH);
}
static fs::path required_database(const fs::path &root, const char *name) {
    auto file = root / "Database" / name;
    if (!fs::is_regular_file(file))
        throw std::runtime_error("Native database is missing: " + std::string(name) +
                                 ". Run migration before starting; empty accounts will not be created.");
    return file;
}
NativeService::NativeService(fs::path root, std::string role, int channel)
    : root_(std::move(root)), role_(std::move(role)),
      instance_(role_ + (role_ == "WorldServer" ? ":" + std::to_string(channel) : "")), channel_(channel),
      config_(Json::parse(read_file(root_ / "Config/server.json"))),
      log_(root_ / "Logs" /
           (instance_.find(':') == std::string::npos ? instance_ + ".jsonl"
                                                     : "WorldServer-" + std::to_string(channel_) + ".jsonl")),
      jobs_(root_), content_(root_), accounts_(required_database(root_, "karos_login.sqlite")),
      world_(required_database(root_, "karos_gameplay.sqlite")), sessions_(root_ / "Database/runtime.sqlite"),
      moderation_(required_database(root_, "moderation.sqlite")) {
    if (channel_ < 1 || channel_ > 4)
        throw std::runtime_error("Public channel must be1..4");
    if(role_=="WorldServer")scene_=std::make_shared<WorldScene>(content_);
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data))
        throw std::runtime_error("Winsock startup failed");
    moderation_.exec("CREATE TABLE IF NOT EXISTS sanctions(id INTEGER PRIMARY KEY,kind TEXT NOT NULL,scope "
                     "TEXT NOT NULL,subject TEXT NOT NULL,actor_account TEXT NOT NULL,actor_eid INTEGER NOT "
                     "NULL,reason TEXT NOT NULL,created REAL NOT NULL,expires REAL,revoked REAL); CREATE "
                     "TABLE IF NOT EXISTS device_observations(account_id TEXT NOT NULL,fingerprint TEXT NOT "
                     "NULL,last_seen REAL NOT NULL,PRIMARY KEY(account_id,fingerprint))");
    log_.write("info", "Native C++ service initialized; no Python subprocess");
}
NativeService::~NativeService() {
    try {
        stop();
    } catch (const std::exception &error) {
        log_.write("error", error.what());
    }
    WSACleanup();
}
SOCKET NativeService::open_listener(int port, int *actual) {
    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET)
        throw std::runtime_error("Cannot create server socket");
    BOOL exclusive = TRUE;
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive),
               sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (role_ == "LoginServer") {
        const auto host = config_.value("bind", std::string("127.0.0.1"));
        if (InetPtonA(AF_INET, host.c_str(), &address.sin_addr) != 1 ||
            (ntohl(address.sin_addr.s_addr) >> 24) != 127) {
            closesocket(fd);
            throw std::runtime_error("LoginServer requires an IPv4 loopback bind");
        }
    }
    address.sin_port = htons(static_cast<u_short>(port));
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(fd, 32)) {
        closesocket(fd);
        throw std::runtime_error("Cannot bind loopback port " + std::to_string(port) +
                                 "; another server may already be active");
    }
    if (actual) {
        int size = sizeof(address);
        getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size);
        *actual = ntohs(address.sin_port);
    }
    u_long nonblocking = 1;
    ioctlsocket(fd, FIONBIO, &nonblocking);
    return fd;
}
void NativeService::start() {
    if (running_.exchange(true))
        return;
    try {
        auto identity = hex(sha256(root_.u8string() + "/" + instance_));
        instance_mutex_ = CreateMutexW(nullptr, FALSE, wide("Local\\RoshNative_" + identity).c_str());
        if (!instance_mutex_ || GetLastError() == ERROR_ALREADY_EXISTS)
            throw std::runtime_error("This native service instance is already running");
        if (role_ == "LoginServer")
            listener_ = open_listener(config_.value("login_port", 10001));
        worker_ = std::thread([this] { run(); });
        log_.write("info", "Service started");
    } catch (...) {
        running_ = false;
        if (instance_mutex_) {
            CloseHandle(instance_mutex_);
            instance_mutex_ = nullptr;
        }
        throw;
    }
}
void NativeService::stop() {
    if (!running_.exchange(false) && !worker_.joinable())
        return;
    if (listener_ != INVALID_SOCKET) {
        closesocket(listener_);
        listener_ = INVALID_SOCKET;
    }
    std::vector<Connection> threads;
    {
        std::lock_guard lock(mutex_);
        for (auto &peer : peers_)
            peer->stop();
        for (auto fd : handoffs_)
            closesocket(fd);
        handoffs_.clear();
    }
    if (worker_.joinable())
        worker_.join();
    {
        std::lock_guard lock(mutex_);
        threads.swap(connections_);
    }
    for (auto &item : threads)
        if (item.thread.joinable())
            item.thread.join();
    if (instance_mutex_) {
        CloseHandle(instance_mutex_);
        instance_mutex_ = nullptr;
    }
    world_.db().exec("PRAGMA wal_checkpoint(PASSIVE)");
    jobs_.heartbeat(instance_, status());
    log_.write("info", "Service stopped; connections closed and SQLite checkpoint completed");
}
Json NativeService::status() {
    DWORD handles=0; GetProcessHandleCount(GetCurrentProcess(), &handles);
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    auto value=[](FILETIME t) { return (std::uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime; };
    IO_COUNTERS io{}; GetProcessIoCounters(GetCurrentProcess(), &io);
    return {{"role", role_},
            {"instance_id", config_.value("instance_id",std::string("neoasmara"))},
            {"process_cpu_ms", (value(kernel)+value(user))/10000},
            {"process_handles", handles},
            {"io_read_bytes", io.ReadTransferCount},
            {"io_write_bytes", io.WriteTransferCount},
            {"channel", channel_},
            {"running", running_.load()},
            {"users", users_.load()},
            {"ticks", ticks_.load()},
            {"bind", role_ == "LoginServer" ? config_.value("bind", std::string("127.0.0.1")) : "127.0.0.1"},
            {"port", role_ == "LoginServer" ? config_.value("login_port", 10001) : 0}};
}
void NativeService::command(const std::string &name, const Json &payload) {
    if (name == "announce") {
        auto text = payload.at("text").get<std::string>();
        if (wide(text).size() > 110)
            throw std::runtime_error("Announcement limit110characters");
        std::vector<std::shared_ptr<Peer>> peers;
        {
            std::lock_guard lock(mutex_);
            peers = peers_;
        }
        for (auto &peer : peers)
            if (peer->authenticated)
                try {
                    peer->send(11, 1, notice(text));
                } catch (...) {
                }
        log_.write("operator", "Announcement delivered to authenticated local sessions");
    } else if (name == "disconnect") {
        std::lock_guard lock(mutex_);
        for (auto &peer : peers_)
            peer->stop();
    } else if (name == "checkpoint") {
        world_.db().exec("PRAGMA wal_checkpoint(PASSIVE)");
        log_.write("operator", "SQLite WAL checkpoint completed");
    } else
        throw std::runtime_error("Operator command is not implemented in the native service");
}
void NativeService::operator_console(const std::string&text){
 if(role_!="MainServer")throw std::runtime_error("Operator console is restricted to MainServer");
 auto args=command_tokens(text);if(args.empty())return;
 if(args[0]=="help"&&args.size()==1){log_.write("console","help | status | checkpoint | setadmin <character> | announce <text>");return;}
 if(args[0]=="status"&&args.size()==1){log_.write("console",status().dump());return;}
 if(args[0]=="checkpoint"&&args.size()==1){command("checkpoint");return;}
 if(args[0]=="setadmin"&&args.size()==2){
  std::vector<std::pair<std::string,Json>>matches;
  for(auto&r:world_.db().query("SELECT id,payload FROM accounts")){auto account=Json::parse(r.at("payload").get<std::string>());for(auto&ch:account.value("chars",Json::array()))if(_stricmp(ch.value("name","").c_str(),args[1].c_str())==0)matches.push_back({r.at("id"),ch});}
  if(matches.size()!=1)throw std::runtime_error("Character missing or ambiguous; no role changed");
  auto&target=matches[0];auto eid=target.second.at("eid").get<std::int64_t>();auto name=target.second.at("name").get<std::string>();
  Sqlite roles(root_/"Database/karos_roles.sqlite");Transaction tx(roles);
  roles.exec("INSERT INTO staff_roles(account_id,character_eid,character_name,role,auth_generation) VALUES(?,?,?,?,?) ON CONFLICT(account_id,character_eid) DO UPDATE SET character_name=excluded.character_name,role=excluded.role,auth_generation=excluded.auth_generation",{target.first,eid,name,std::string("Admin"),token(20)});
  roles.exec("CREATE TABLE IF NOT EXISTS local_operator_audit(id INTEGER PRIMARY KEY,action TEXT,account_id TEXT,character_eid INTEGER,created_at REAL)");
  roles.exec("INSERT INTO local_operator_audit(action,account_id,character_eid,created_at) VALUES('setadmin',?,?,?)",{target.first,eid,now()});tx.commit();
  log_.write("console","Admin assigned to "+name+" (EID "+std::to_string(eid)+"). Account password preserved; staff authentication remains required in game.");return;
 }
 if(args[0]=="announce"&&args.size()>1){std::string message;for(std::size_t i=1;i<args.size();i++){if(i>1)message+=' ';message+=args[i];}for(auto&item:jobs_.services())if(item.value("alive",false)&&item.value("role","")=="WorldServer")jobs_.enqueue(item.at("instance"),"announce",{{"text",message}});log_.write("console","Announcement queued");return;}
 throw std::runtime_error("Unknown console command. Use help");
}
void NativeService::connection(std::function<void()> action) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    connections_.push_back({std::thread([this, done, action = std::move(action)] {
                                try {
                                    action();
                                } catch (const std::exception &error) {
                                    log_.write("error", error.what());
                                } catch (...) {
                                    log_.write("error", "Unhandled connection failure");
                                }
                                done->store(true);
                            }),
                            done});
}
void NativeService::reap() {
    std::vector<std::thread> completed;
    {
        std::lock_guard lock(mutex_);
        auto it = connections_.begin();
        while (it != connections_.end()) {
            if (it->done->load()) {
                completed.push_back(std::move(it->thread));
                it = connections_.erase(it);
            } else
                ++it;
        }
    }
    for (auto &thread : completed)
        thread.join();
}
void NativeService::run() {
    double heartbeat = 0,next_contention_log=0;
    try {
        while (running_) {
          try {
            ticks_++;
            if (listener_ != INVALID_SOCKET) {
                auto fd = accept(listener_, nullptr, nullptr);
                if (fd != INVALID_SOCKET) {
                    u_long blocking = 0;
                    ioctlsocket(fd, FIONBIO, &blocking);
                    DWORD timeout = 1000;
                    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
                               sizeof(timeout));
                    auto peer = std::make_shared<Peer>(fd);
                    std::lock_guard lock(mutex_);
                    if (peers_.size() >= 64) {
                        peer->stop();
                    } else {
                        peers_.push_back(peer);
                        connection([this, peer] { serve(peer, 10001); });
                    }
                }
            }
            process_jobs();
            reap();
            if (now() >= heartbeat) {
                const auto timestamp = now();
                if(role_=="WorldServer"){
                    try{
                        const auto start=lupinel_round_start(root_);
                        if(start>0&&last_lupinel_selection_!=start&&timestamp>=start-20*60*60){
                            select_lupinel_challengers(world_.db(),start,channel_,static_cast<std::int64_t>(timestamp));
                            last_lupinel_selection_=start;
                        }
                    }catch(const std::exception &error){log_.write("lupinel-selection",error.what());}
                }
                jobs_.heartbeat(instance_, status());
                heartbeat = timestamp + 1;
            }
          }catch(const SqliteError&error){
            if(!error.contention())throw;
            // A competing writer delays control jobs; it must not kill the login/world worker.
            if(now()>=next_contention_log){log_.write("sqlite-retry",error.what());next_contention_log=now()+5;}
          }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    } catch (const std::exception &error) {
        running_ = false;
        log_.write("error", error.what());
    }
}
void NativeService::process_jobs() {
    if(role_=="MainServer"&&now()>=next_control_tick_){
        next_control_tick_=now()+1;
        NativeControl controls(root_);auto action=controls.poll(now());
        if(action){
            if(*action!="shutdown"){
                auto executable=root_/"Bin/MainServer.exe";
                auto line=L"\""+executable.wstring()+L"\" --root \""+root_.wstring()+L"\" --after-process "+std::to_wstring(GetCurrentProcessId())+L" --cluster";
                STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_SHOWNORMAL;PROCESS_INFORMATION process{};
                if(!CreateProcessW(executable.c_str(),line.data(),nullptr,nullptr,FALSE,0,nullptr,root_.c_str(),&startup,&process))throw std::runtime_error("Nao foi possivel preparar o reinicio nativo");
                CloseHandle(process.hThread);CloseHandle(process.hProcess);
            }
            auto name=L"Local\\RoshNativeStop_"+std::to_wstring(GetCurrentProcessId());auto event=OpenEventW(EVENT_MODIFY_STATE,FALSE,name.c_str());
            if(!event)throw std::runtime_error("Evento de encerramento normal indisponivel");SetEvent(event);CloseHandle(event);
        }
    }
    for (const auto &job : jobs_.pending(instance_)) {
        auto id = job.at("id").get<std::string>();
        try {
            auto kind = job.at("kind").get<std::string>();
            auto payload = Json::parse(job.at("payload").get<std::string>());
            if (kind == "handoff") {
                int stage = role_ == "LobbyServer" ? 10110 : role_ == "WorldServer" ? 10101 : 0;
                if (!stage)
                    throw std::runtime_error("Invalid handoff target");
                auto session = sessions_.load(payload.at("session"));
                if (stage == 10101 && session.channel != channel_)
                    throw std::runtime_error("Wrong channel handoff");
                int port;
                auto fd = open_listener(0, &port);
                {
                    std::lock_guard lock(mutex_);
                    handoffs_.insert(fd);
                    connection([this, session, stage, fd] { accept_handoff(session, stage, fd); });
                }
                jobs_.finish(id, {{"port", port}});
            } else {
                command(kind, payload);
                jobs_.finish(id, {{"ok", true}});
            }
        } catch (const std::exception &error) {
            jobs_.finish(id, {{"error", error.what()}}, false);
            log_.write("warning", "Internal command rejected");
        }
    }
}
void NativeService::accept_handoff(const Session &session, int stage, SOCKET listener) {
    SOCKET fd = INVALID_SOCKET;
    while (running_ && now() < session.expires) {
        fd = accept(listener, nullptr, nullptr);
        if (fd != INVALID_SOCKET)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    {
        std::lock_guard lock(mutex_);
        if (handoffs_.erase(listener))
            closesocket(listener);
    }
    if (fd == INVALID_SOCKET)
        return;
    u_long blocking = 0;
    ioctlsocket(fd, FIONBIO, &blocking);
    DWORD timeout = 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    auto peer = std::make_shared<Peer>(fd);
    {
        std::lock_guard lock(mutex_);
        if (peers_.size() >= 64) {
            peer->stop();
            return;
        }
        peers_.push_back(peer);
    }
    serve(peer, stage, session);
}
int NativeService::request_handoff(Session session, int stage) {
    session.id = token();
    session.expires = now() + 600;
    sessions_.save(session);
    auto target = stage == 10110 ? "LobbyServer" : "WorldServer:" + std::to_string(session.channel);
    auto id = jobs_.enqueue(target, "handoff", {{"session", session.id}});
    return jobs_.wait(id).at("port");
}
bool NativeService::banned(const Session &session, std::uint32_t eid) {
    moderation_.exec("CREATE TABLE IF NOT EXISTS native_kicks(session TEXT PRIMARY KEY,until_at REAL NOT NULL,reason TEXT NOT NULL)");
    if(!moderation_.query("SELECT session FROM native_kicks WHERE session=? AND until_at>?",{session.id,now()}).empty())return true;
    auto subjects = std::vector<std::pair<std::string, std::string>>{{"account", session.identity.id},
                                                                     {"hardware", hex(session.fingerprint)}};
    if (eid)
        subjects.emplace_back("character", std::to_string(eid));
    for (const auto &[scope, subject] : subjects)
        if (!moderation_
                 .query("SELECT id FROM sanctions WHERE kind='ban' AND scope=? AND subject=? AND revoked IS "
                        "NULL AND (expires IS NULL OR expires>?) LIMIT 1",
                        {scope, subject, now()})
                 .empty())
            return true;
    return false;
}
static Bytes acknowledged(const Json &stage, const Identity &identity) {
    auto bytes = unhex(stage.at("0200"));
    if (bytes.size() < 272)
        throw std::runtime_error("Invalid login acknowledgement carrier");
    auto end = std::find(bytes.begin() + 271, bytes.end(), 0);
    if (end == bytes.end())
        throw std::runtime_error("Invalid acknowledgement identity");
    Bytes result(bytes.begin(), bytes.begin() + 271);
    auto uid = std::to_string(identity.native_uid);
    result.insert(result.end(), uid.begin(), uid.end());
    result.insert(result.end(), end, bytes.end());
    return result;
}
static Bytes gate_address(Bytes body, int port) {
    for (std::size_t i = 0; i + 5 < body.size(); i++) {
        if ((body[i] == 0x55 && body[i + 1] == 0x77 && body[i + 2] == 0x95) ||
            (body[i] == 127 && body[i + 1] == 0 && body[i + 2] == 0 && body[i + 3] == 1)) {
            body[i] = 127;
            body[i + 1] = body[i + 2] = 0;
            body[i + 3] = 1;
            put16(body, i + 4, static_cast<std::uint16_t>(port));
            i += 5;
        }
    }
    return body;
}
static Bytes channel_list(Bytes body, int port) {
    if (body.size() != 28)
        throw std::runtime_error("Invalid native channel carrier");
    Bytes result(body.begin(), body.begin() + 12);
    put16(result, 2, 1, true);
    put16(result, 4, 4, true);
    for (int channel = 1; channel <= 4; channel++) {
        Bytes row(body.begin() + 12, body.end());
        put16(row, 0, 1);
        put16(row, 2, static_cast<std::uint16_t>(channel));
        row[6] = 127;
        row[7] = row[8] = 0;
        row[9] = 1;
        put16(row, 10, static_cast<std::uint16_t>(port));
        result.insert(result.end(), row.begin(), row.end());
    }
    return result;
}
void NativeService::send_cards(Peer &peer, const Identity &identity, std::uint32_t only_eid) {
    Sqlite staff_roles(root_/"Database/karos_roles.sqlite",true);
    auto cards = content_.get("server/class_cards.json"), starters = content_.get("server/starter_gear.json");
    auto characters = world_.characters(identity.id);
    if (only_eid) {
        Json selected = Json::array();
        for (const auto &ch : characters)
            if (ch.at("eid") == only_eid)
                selected.push_back(ch);
        characters = std::move(selected);
    }
    if (characters.empty()) {
        Bytes empty(389);
        empty[0] = 1;
        peer.send(2, 1, empty);
        return;
    }
    for (std::size_t i = 0; i < characters.size() && i < 4; i++) {
        auto ch = characters[i];
        int cls = ch.at("cls"), family = cls / 100 * 100;
        auto body = unhex(cards.at(std::to_string(family)));
        if (body.size() < 385)
            throw std::runtime_error("Invalid native character card carrier");
        auto old_name = body[383];
        if (385 + old_name * 4 > body.size())
            throw std::runtime_error("Invalid card name tail");
        Bytes tail(body.begin() + 385 + old_name * 4, body.end());
        body[0] = i + 1 == characters.size();
        put32(body, 1, ch.at("eid"), true);
        put32(body, 9, static_cast<std::uint32_t>(cls));
        put32(body, 49, ch.value("level", 1));
        put32(body, 21, ch.value("hp", 150));
        put32(body, 29, ch.value("mp", 90));
        auto gear =
            ch.contains("gear") && !ch["gear"].empty() ? ch["gear"] : starters.at(std::to_string(family));
        if (ch.contains("equipment_slots")) {
            for (int slot = 1; slot <= 30; slot++)
                put32(body, 128 + slot * 4, ch["equipment_slots"].value(std::to_string(slot), 0));
        } else {
            for (int slot = 0; slot < 4; slot++)
                put32(body, 132 + slot * 4, gear["armor"][slot]);
            put32(body, 156, gear.value("weapon", 0));
            put32(body, 160, gear.value("shield", 0));
        }
        if (ch.contains("appearance")) {
            auto appearance = unhex(ch["appearance"]);
            if (appearance.size() != 6 && appearance.size() != 8)
                throw std::runtime_error("Invalid saved character appearance");
            body[11] = appearance[0];
            body[12] = appearance[1];
            std::copy(appearance.begin() + 2, appearance.begin() + 6, body.begin() + 13);
            if (appearance.size() == 8)
                std::copy(appearance.begin() + 6, appearance.end(), body.begin() + 17);
        }
        auto letters = wide(displayed_name(ch.at("name"),character_role(staff_roles,identity.id,ch.at("eid")),20));
        std::vector<std::uint32_t> codes;
        for (std::size_t n = 0; n < letters.size(); n++) {
            std::uint32_t code = letters[n];
            if (code >= 0xd800 && code <= 0xdbff && n + 1 < letters.size())
                code = 0x10000 + ((code - 0xd800) << 10) + (letters[++n] - 0xdc00);
            codes.push_back(code);
        }
        if (codes.size() > 20)
            throw std::runtime_error("Saved character name exceeds native limit");
        body.resize(385 + codes.size() * 4);
        body[383] = static_cast<unsigned char>(codes.size());
        body[384] = 0;
        for (std::size_t n = 0; n < codes.size(); n++)
            put32(body, 385 + n * 4, codes[n]);
        body.insert(body.end(), tail.begin(), tail.end());
        peer.send(2, 1, body);
    }
}
void NativeService::serve(std::shared_ptr<Peer> peer, int stage, Session chain) {
    Bytes buffer;
    bool admitted = false;
    double last = now();
    std::unique_ptr<WorldSession> game;
    try {
        auto templates = content_.get("server/stage_templates.json");
        auto stage_name = stage == 10001 ? "gate" : stage == 10110 ? "mid" : "game";
        const auto protocol = templates.at(stage_name);
        while (running_ && now() - last < 600) {
            if (game) {
                if(banned(chain,game->eid()))throw std::runtime_error("Sessao encerrada pela moderacao");
                game->tick();
                game->admin_tick([peer](unsigned char cat,unsigned char sub,const Bytes &body){peer->send(cat,sub,body);});
            }
            char data[65536];
            int count = recv(peer->socket(), data, sizeof(data), 0);
            if (count == 0)
                break;
            if (count < 0) {
                if (WSAGetLastError() == WSAETIMEDOUT)
                    continue;
                break;
            }
            last = now();
            buffer.insert(buffer.end(), data, data + count);
            const std::string health = "KAROS-LOCAL-STATUS\n";
            if (stage == 10001 && !admitted) {
                std::string input(buffer.begin(), buffer.end());
                if (input == health) {
                    peer->send_raw("KAROS-TEST-FUNCTIONS-AUTH/1\n");
                    break;
                }
                if (input.size() < health.size() && health.compare(0, input.size(), input) == 0)
                    continue;
            }
            for (const auto &frame : extract_frames(buffer)) {
                auto cat = frame[0], sub = frame[1];
                Bytes body(frame.begin() + 6, frame.end());
                if (cat == 2 && sub == 0) {
                    if (admitted)
                        throw std::runtime_error("Repeated login request");
                    auto fields = parse_login(frame);
                    if (stage == 10001) {
                        if (fields.credential.rfind("L1:", 0) != 0)
                            throw std::runtime_error("Entre pelo NeoAsmara Dashboard.");
                        chain.identity = accounts_.consume_ticket(fields.username, fields.credential.substr(3));
                        chain.method = "launcher";
                        chain.fingerprint = fields.fingerprint;
                        chain.expires = now() + 600;
                    } else {
                        auto expected = stage == 10101 ? std::to_string(chain.identity.native_uid)
                                                       : chain.identity.username;
                        auto expected_code =
                            stage == 10101 ? std::to_string(chain.channel_code) : std::string();
                        if (chain.method != "launcher" || now() > chain.expires ||
                            _stricmp(fields.username.c_str(), expected.c_str()) != 0 || fields.credential != expected_code ||
                            !secure_equal(fields.fingerprint, chain.fingerprint))
                            throw std::runtime_error("Invalid native session handoff");
                        sessions_.consume(chain.id);
                    }
                    if (banned(chain))
                        throw std::runtime_error("Conta ou dispositivo bloqueado.");
                    moderation_.exec(
                        "INSERT INTO device_observations VALUES(?,?,?) ON CONFLICT(account_id,fingerprint) "
                        "DO UPDATE SET last_seen=excluded.last_seen",
                        {chain.identity.id, hex(chain.fingerprint), now()});
                    admitted = true;
                    peer->authenticated = true;
                    users_++;
                    peer->send(11, 0, unhex(protocol.at("0b00")));
                    peer->send(2, 0,
                               stage == 10001 ? unhex(protocol.at("0200"))
                                              : acknowledged(protocol, chain.identity));
                    if (stage == 10001)
                        peer->send(2, 6,
                                   gate_address(unhex(protocol.at("0206")), request_handoff(chain, 10110)));
                    else if (stage == 10110)
                        peer->send(2, 6,
                                   channel_list(unhex(protocol.at("0206")),
                                                config_.value("world_hint_port", 10101)));
                    else {
                        if (protocol.contains("020b"))
                            peer->send(2, 11, unhex(protocol.at("020b")));
                        send_cards(*peer, chain.identity);
                    }
                    log_.write("info", "Authenticated native session admitted; credentials omitted");
                    continue;
                }
                if (!admitted)
                    throw std::runtime_error("Authentication required");
                if (banned(chain))
                    throw std::runtime_error("Conta bloqueada.");
                // The installed channel picker bridge sends C07/01 "chpick N" instead of C02/07.
                static const Bytes channelPick{'c','h','p','i','c','k',' '};
                if (stage==10101&&cat==7&&sub==1&&body.size()==9&&body.back()==0&&
                    std::equal(channelPick.begin(),channelPick.end(),body.begin())&&body[7]>='1'&&body[7]<='9') {
                    if(!game||!game->ready()||game->character().value("hp",0)<=0){
                        peer->send(11,1,notice("Troca de canal indisponivel."));
                        continue;
                    }
                    const auto requested=body[7]-'0';
                    cat=2;
                    sub=7;
                    body.assign(18,0);
                    put16(body,0,1,true);
                    put16(body,2,requested,true);
                }
                if (stage == 10101 && cat == 2 && sub == 4) {
                    if (body.size() < 4)
                        throw std::runtime_error("Invalid world entry");
                    auto eid = get32(body, 0, true);
                    if (banned(chain, eid))
                        throw std::runtime_error("Personagem bloqueado.");
                    game.reset();
                    game = std::make_unique<WorldSession>(content_, world_, chain, root_, eid,scene_);
                    game->enter([peer](auto cat, auto sub, const Bytes &bytes) { peer->send(cat, sub, bytes); });
                } else if (stage == 10101 && cat == 2 && sub == 1) {
                    game.reset();
                    if (protocol.contains("020b"))
                        peer->send(2, 11, unhex(protocol.at("020b")));
                    send_cards(*peer, chain.identity);
                } else if (stage != 10001 && cat == 2 && sub == 6) {
                    game.reset();
                    peer->send(2, 6,
                               channel_list(unhex(templates.at("mid").at("0206")),
                                            config_.value("world_hint_port", 10101)));
                } else if (game && game->handle(cat, sub, body, [peer](auto c, auto s, const Bytes &bytes) {
                               peer->send(c, s, bytes);
                           },"npc:"+chain.id+":"+std::to_string(cat)+"/"+std::to_string(sub)+":"+hex(Bytes(frame.begin()+2,frame.begin()+6)))) {
                } else if (cat == 2 && sub == 7 && stage != 10001) {
                    if (body.size() != 18 || get16(body, 0, true) != 1 ||
                        std::any_of(body.begin() + 4, body.end(), [](unsigned char c) { return c != 0; })) {
                        peer->send(11, 1, notice("Pedido de troca de canal invalido."));
                        continue;
                    }
                    const int channel = get16(body, 2, true);
                    if (channel < 1 || channel > 4) {
                        peer->send(11, 1, notice("Canal indisponivel."));
                        continue;
                    }
                    auto transfer = unhex(templates["mid"]["0207"]);
                    if (transfer.size() != 18)
                        throw std::runtime_error("Invalid channel transfer carrier");
                    auto next = chain;
                    next.channel = channel;
                    next.channel_code = (get32(random_bytes(4), 0) % 0x7ffffffe) + 1;
                    int port;
                    try {
                        port = request_handoff(next, 10101);
                    } catch (const std::exception &error) {
                        log_.write("warning", "Channel handoff unavailable: " + std::string(error.what()));
                        peer->send(11, 1, notice("Canal indisponivel."));
                        continue;
                    }
                    put16(transfer, 0, 1, true);
                    put16(transfer, 2, static_cast<std::uint16_t>(channel), true);
                    transfer[8] = 1;
                    transfer[9] = transfer[10] = 0;
                    transfer[11] = 127;
                    put16(transfer, 12, static_cast<std::uint16_t>(port), true);
                    put32(transfer, 14, next.channel_code, true);
                    game.reset();
                    peer->send(2, 7, transfer);
                } else if (cat == 2 && sub == 2 && stage == 10101) {
                    if (body.size() < 19)
                        throw std::runtime_error("Truncated character creation");
                    int cls = get16(body, 0), length = body[13];
                    if (length < 1 || length > 20 || body.size() < 15 + static_cast<std::size_t>(length) * 4)
                        throw std::runtime_error("Invalid character name");
                    std::wstring name;
                    for (int n = 0; n < length; n++) {
                        auto code = get32(body, 15 + n * 4);
                        if (code == 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
                            throw std::runtime_error("Invalid character name codepoint");
                        if (code < 0x10000)
                            name += static_cast<wchar_t>(code);
                        else {
                            code -= 0x10000;
                            name += static_cast<wchar_t>(0xd800 + (code >> 10));
                            name += static_cast<wchar_t>(0xdc00 + (code & 1023));
                        }
                    }
                    Json vitals = Json::object();
                    const std::map<int, std::pair<int, int>> base = {
                        {100, {190, 60}}, {200, {190, 91}}, {300, {150, 80}}, {400, {160, 90}},
                        {500, {160, 70}}, {600, {150, 90}}, {800, {189, 117}}};
                    for (const auto &[family, hpmp] : base)
                        vitals[std::to_string(family)]["1"] = {{"maxhp", hpmp.first}, {"maxmp", hpmp.second}};
                    auto character =
                        world_.create_character(chain.identity.id, utf8(name), cls,
                                                hex(Bytes(body.begin() + 2, body.begin() + 10)), vitals);
                    auto starters = content_.get("server/starter_gear.json");
                    world_.update_character(chain.identity.id, character["eid"],
                                            [&](Json &ch) { ch["gear"] = starters.at(std::to_string(cls)); });
                    send_cards(*peer, chain.identity, character.at("eid"));
                } else if (cat == 2 && sub == 3 && stage == 10101) {
                    if (body.size() != 4)
                        throw std::runtime_error("Invalid character deletion");
                    world_.delete_character(chain.identity.id, get32(body, 0, true));
                    peer->send(2, 3, body);
                    send_cards(*peer, chain.identity);
                } else if (cat == 11 && sub == 0) {
                } else {
                    log_.write("pending", "Native opcode requires gameplay migration: " +
                                              std::to_string(cat) + "/" + std::to_string(sub));
                }
            }
        }
    } catch (const std::exception &error) {
        try {
            peer->send(11, 1, notice(error.what()));
        } catch (...) {
        }
        log_.write("warning", "Native connection ended: " + std::string(error.what()));
    }
    game.reset();
    if (admitted)
        users_--;
    peer->authenticated = false;
    peer->stop();
    {
        std::lock_guard lock(mutex_);
        peers_.erase(std::remove(peers_.begin(), peers_.end(), peer), peers_.end());
    }
}
} // namespace rosh
