#include "native/runtime.hpp"
#include "native/inventory.hpp"
#include "native/entities.hpp"
#include "native/consumables.hpp"
#include "native/scene.hpp"
#include "native/gm.hpp"
#include "native/game.hpp"
#include "native/economy.hpp"
#include "native/loot.hpp"
#include <algorithm>
#include <iostream>
#include <ws2tcpip.h>
using namespace rosh;
static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Client {
    SOCKET fd{INVALID_SOCKET};
    explicit Client(int port) {
        fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<u_short>(port));
        DWORD timeout = 6000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)))
            throw std::runtime_error("Native test connection failed");
    }
    ~Client() {
        if (fd != INVALID_SOCKET)
            closesocket(fd);
    }
    void send(unsigned char cat, unsigned char sub, const Bytes &body, std::uint16_t sequence = 1) {
        auto bytes = make_frame(cat, sub, body, sequence);
        std::size_t done = 0;
        while (done < bytes.size()) {
            auto n = ::send(fd, reinterpret_cast<const char *>(bytes.data() + done),
                            static_cast<int>(bytes.size() - done), 0);
            if (n <= 0)
                throw std::runtime_error("Native test send failed");
            done += n;
        }
    }
    Bytes read(std::size_t size) {
        Bytes bytes(size);
        std::size_t done = 0;
        while (done < size) {
            auto n =
                recv(fd, reinterpret_cast<char *>(bytes.data() + done), static_cast<int>(size - done), 0);
            if (n <= 0)
                throw std::runtime_error("Native test receive failed");
            done += n;
        }
        return bytes;
    }
    Bytes packet(int cat, int sub) {
        for (;;) {
            auto header = read(4);
            auto size = get32(header, 0, true);
            require(size >= 6 && size <= 1048576, "Invalid server framing");
            auto frame = read(size);
            if (frame[0] == cat && frame[1] == sub && !(cat==2&&sub==11&&frame.size()>=10&&get32(frame,6,true)==103))
                return Bytes(frame.begin() + 6, frame.end());
            if (frame[0] == 6 && frame[1] == 5) {
                require(frame.size() >= 12 && frame.size() == 12 + frame[11] * 147,
                        "Invalid inventory snapshot");
                continue;
            }
            if (frame[0] == 6 && frame[1] == 6) {
                require(frame.size() == 158, "Invalid item update record");
                continue;
            }
            if (frame[0] == 6 && frame[1] == 0x46) {
                require(frame.size() == 12 && get32(frame,6,true)>0 && frame[10]<=1 && frame[11]<=1,
                        "Invalid native cash appearance state");
                continue;
            }
            if(frame[0]==4&&frame[1]==2&&frame.size()>=260&&frame[7]==1){
                require(get32(frame,70)>0,"NPC stream has no native type");
                continue;
            }
            if(frame[0]==4&&frame[1]==1){
                require(frame.size()>=36&&frame[6]==1&&get32(frame,8,true)>0,"Invalid asynchronous NPC movement");
                require(std::isfinite(getfloat(frame,24))&&std::isfinite(getfloat(frame,28))&&std::isfinite(getfloat(frame,32)),"NPC movement is not finite");continue;
            }
            if(frame[0]==4&&frame[1]==3){require(frame.size()==11&&frame[6]==1,"Invalid NPC removal");continue;}
            if(frame[0]==5&&frame[1]==0){require(frame.size()==55&&frame[6]==1&&frame[53]==0&&frame[54]==36,"Invalid asynchronous mob attack start");continue;}
            if(frame[0]==5&&frame[1]==11){require(frame.size()==14&&get32(frame,6,true)>0,"Invalid NPC reaction");continue;}
            if(frame[0]==5&&frame[1]==2){require(frame.size()==51&&frame[6]==1&&frame[21]==0,"Invalid asynchronous mob hit");continue;}
            if(frame[0]==8&&frame[1]==6){require(frame.size()==27&&get32(frame,7,true)>0,"Invalid entity HP stream");continue;}
            if(frame[0]==8&&frame[1]==0){require(frame.size()>=31&&get32(frame,7,true)>0,"Invalid player HP stream");continue;}
            if(frame[0]==8&&frame[1]==1){
                require(frame.size()>=49&&get32(frame,7,true)>0&&get32(frame,23)<=get32(frame,27),
                        "Invalid asynchronous player MP stream");
                continue;
            }
            if(frame[0]==5&&frame[1]==5){require(frame.size()>=12&&frame.size()==12+frame[11]*8u,"Invalid skill-list stream");continue;}
            if(frame[0]==5&&frame[1]==9){
                require(frame.size()>=14&&frame.size()==14+18u*get16(frame,12,true)&&frame[11]<=1&&get32(frame,7,true)>0,
                        "Invalid native buff delta framing");
                for(std::size_t offset=14;offset<frame.size();offset+=18){
                    require(get32(frame,offset,true)>0,"Native buff delta has no skill ID");
                    if(frame[11])require(frame[offset+4]>0&&get32(frame,offset+10,true)>0,"Native buff has no rank or remaining duration");
                }
                continue;
            }
            if(frame[0]==8&&frame[1]==4){require(frame.size()==24&&get16(frame,22,true)==0x120,"Invalid saved Fletta path");continue;}
            if(frame[0]==2&&frame[1]==11&&frame.size()>=21&&get32(frame,6,true)==103){auto message=Json::parse(std::string(frame.begin()+20,frame.end()-1));require(message.at("cmd")=="quests"||message.at("cmd")=="quests.held","Unexpected overlay command");require(message.at("active").size()<=20,"Quest HUD exceeds native capacity");continue;}
            if(frame[0]==9&&frame[1]==3){require(frame.size()==28&&get16(frame,26,true)==0x51,"Invalid quest progress update");continue;}
            if (frame[0] != cat || frame[1] != sub)
                throw std::runtime_error("Unexpected native opcode: " + std::to_string(frame[0]) + "/" +
                                         std::to_string(frame[1])+" while waiting "+std::to_string(cat)+"/"+std::to_string(sub));
            return Bytes(frame.begin() + 6, frame.end());
        }
    }
    void login(const std::string &user, const std::string &credential) {
        Bytes body(271);
        for (const auto &field : {user, credential, std::string("fingerprint-1"),
                                  std::string("fingerprint-2"), std::string("fingerprint-3")}) {
            body.insert(body.end(), field.begin(), field.end());
            body.push_back(0);
        }
        send(2, 0, body);
    }
};
static int free_port() {
    auto fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    int size = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size);
    int port = ntohs(address.sin_port);
    closesocket(fd);
    return port;
}
static void verify_descada_brackets(const fs::path &root) {
    Content content(root);
    LegacyStore store(root / "Database/descada-test.sqlite");
    auto &db = store.db();
    db.exec("CREATE TABLE character_locations(eid INTEGER PRIMARY KEY,map_name TEXT NOT NULL,"
            "x REAL NOT NULL,y REAL NOT NULL,z REAL NOT NULL,angle REAL NOT NULL,updated_at REAL NOT NULL)");
    auto scene = std::make_shared<WorldScene>(content);
    const auto actors = scene->map("berneo");
    const auto admin = std::find_if(actors->begin(), actors->end(), [](const auto &actor) {
        return actor.second.npc_id == 90000001;
    });
    require(admin != actors->end(), "Missing Descada administrator");
    const auto &npc = admin->second;
    const auto position = npc.position;
    struct Route {
        std::uint32_t target, map;
        int minimum, maximum, fee;
    };
    const Route routes[] = {{90035551, 9003555, 36, 55, 20000}, {90056751, 9005675, 56, 75, 40000},
                            {90076951, 9007695, 76, 95, 60000}, {90096001, 9009600, 96, 110, 70000}};
    std::uint32_t eid = 996900;
    for (const auto &route : routes) {
        for (const int level : {route.minimum - 1, route.minimum, route.maximum, route.maximum + 1}) {
            ++eid;
            const auto account = "descada-entry-" + std::to_string(eid);
            Json character = {{"eid", eid}, {"name", "DescadaFixture"}, {"cls", 600}, {"level", level},
                              {"hp", 100}, {"mp", 100}, {"exp", Progression().threshold(level)}, {"map_id", 19003}};
            db.exec("INSERT INTO accounts VALUES(?,?)", {account, Json{{"chars", Json::array({character})}}.dump()});
            db.exec("INSERT INTO character_locations VALUES(?,?,?,?,?,?,?)",
                    {std::int64_t(eid), std::string("berneo"), position.x, position.y, position.z, 0., now()});
            Session session;
            session.id = token();
            session.identity.id = account;
            const bool spanish = route.minimum == 56;
            const bool english = route.minimum == 76;
            session.identity.locale = spanish ? "es" : english ? "en" : "pt";
            session.expires = now() + 600;
            WorldSession world(content, store, session, root, eid, scene);
            Inventory inventory(db, content);
            Wallet wallet(db, inventory);
            const auto balance = wallet.apply(eid, 100000, "descada-wallet-" + account).first;
            Bytes ready(21);
            put32(ready, 1, eid, true);
            putfloat(ready, 5, position.x);
            putfloat(ready, 9, position.y);
            putfloat(ready, 13, position.z);
            world.handle(4, 0, ready, [](unsigned char, unsigned char, const Bytes &) {});
            Bytes warp(14);
            const auto route_group=content.database("database/content_typed.sqlite")->query("SELECT o0_i AS route_group FROM mapWarp WHERE o8_i=? AND o16_i=?",{std::int64_t(npc.npc_id),std::int64_t(route.target)});
            require(!route_group.empty(), "Missing native Descada route group");
            put32(warp, 0, route_group.front().at("route_group").get<std::uint32_t>(), true);
            put32(warp, 4, route.target, true);
            put32(warp, 8, npc.eid, true);
            put16(warp, 12, 0x123, true);
            const auto warning = notice((spanish ? "Descada requiere nivel " : english ? "Descada requires level " :
                                         "Descada exige nível ") + std::to_string(route.minimum) + (english ? " to " : " a ") +
                                        std::to_string(route.maximum) + ".");
            bool denied = false, changed = false;
            world.handle(4, 7, warp, [&](unsigned char category, unsigned char sub, const Bytes &reply) {
                if (category == 11 && sub == 1 && reply == warning)
                    denied = true;
                if (category == 4 && sub == 4 && get32(reply, 4, true) == route.map)
                    changed = true;
            }, "descada-warp-" + account);
            const bool eligible = level >= route.minimum && level <= route.maximum;
            require(denied == !eligible && changed == eligible, "Descada level boundary returned the wrong result");
            require(wallet.balance(eid) == balance - (eligible ? route.fee : 0), "Descada entry fee mismatch");
        }
    }
}
static void verify_mine_registration(const fs::path &root) {
    Content content(root);
    LegacyStore store(root / "Database/mine-registration-test.sqlite");
    auto &db = store.db();
    auto scene = std::make_shared<WorldScene>(content);
    const auto actors = scene->map("asmara");
    const auto caretaker = std::find_if(actors->begin(), actors->end(), [](const auto &actor) {
        return actor.second.npc_id == 9419201;
    });
    require(caretaker != actors->end(), "Lupinel caretaker missing");
    const auto position = caretaker->second.position;
    constexpr std::uint32_t eid = 997001;
    const std::string account = "mine-leader-fixture";
    const Json character = {{"eid", eid}, {"name", "MineFixture"}, {"cls", 600}, {"level", 40},
                            {"hp", 100}, {"mp", 100}, {"exp", Progression().threshold(40)}, {"map_id", 19002}};
    db.exec("INSERT INTO accounts VALUES(?,?)", {account, Json{{"chars", Json::array({character})}}.dump()});
    db.exec("CREATE TABLE character_locations(eid INTEGER PRIMARY KEY,map_name TEXT NOT NULL,"
            "x REAL NOT NULL,y REAL NOT NULL,z REAL NOT NULL,angle REAL NOT NULL,updated_at REAL NOT NULL)");
    db.exec("INSERT INTO character_locations VALUES(?,?,?,?,?,?,?)",
            {std::int64_t(eid), std::string("asmara"), position.x, position.y, position.z, 0., now()});
    Inventory inventory(db, content);
    Wallet wallet(db, inventory);
    wallet.apply(eid, 250000, "mine-fixture-funds");
    db.exec("INSERT INTO native_guilds(id,name) VALUES(1,'Mine Fixture')");
    db.exec("INSERT INTO native_guild_members VALUES(?,?,1,5)", {std::int64_t(eid), account});
    auto save_member = [&](int member) {
        const auto member_id = eid + member;
        const auto member_account = "mine-member-" + std::to_string(member);
        const Json saved = {{"eid", member_id}, {"name", member_account}, {"level", 40}};
        db.exec("INSERT INTO accounts VALUES(?,?)",
                {member_account, Json{{"chars", Json::array({saved})}}.dump()});
    };
    for (int member = 1; member <= 20; ++member) {
        const auto member_account = "mine-member-" + std::to_string(member);
        db.exec("INSERT INTO native_guild_members VALUES(?,?,1,0)", {std::int64_t(eid + member), member_account});
        if (member < 20)
            save_member(member);
    }
    Bytes ready(21), proposal(14);
    put32(ready, 1, eid, true);
    putfloat(ready, 5, position.x);
    putfloat(ready, 9, position.y);
    putfloat(ready, 13, position.z);
    put32(proposal, 0, eid, true);
    put32(proposal, 4, 1900201, true);
    put16(proposal, 8, 0x12b, true);
    auto request = [&](int channel, int attempts, unsigned expected_list_count = 0) {
        Session session;
        session.id = token();
        session.identity.id = account;
        session.channel = channel;
        session.expires = now() + 600;
        WorldSession world(content, store, session, root, eid, scene);
        world.handle(4, 0, ready, [](unsigned char, unsigned char, const Bytes &) {});
        int successes = 0;
        for (int attempt = 0; attempt < attempts; ++attempt)
            world.handle(0x0d, 1, proposal, [&](unsigned char cat, unsigned char sub, const Bytes &reply) {
                if (cat == 0x0d && sub == 1 && reply.size() == 6 && get32(reply, 0, true) == 1900201 &&
                    get16(reply, 4, true) == 0x12b)
                    ++successes;
            });
        Bytes list_request(12), list_reply;
        put32(list_request, 0, eid, true);
        put32(list_request, 4, 1900201, true);
        world.handle(0x0d, 2, list_request, [&](unsigned char cat, unsigned char sub, const Bytes &reply) {
            if (cat == 0x0d && sub == 2)
                list_reply = reply;
        });
        require(list_reply.size() >= 8 && get32(list_reply, 0, true) == 1900201 &&
                    get32(list_reply, 4, true) == expected_list_count,
                "Mine list count or occupation ID mismatch");
        if (expected_list_count) {
            const std::string name = "Mine Fixture";
            const auto offset = 8 + name.size() + 1;
            require(list_reply.size() == offset + 12 && list_reply[offset - 1] == 0 &&
                        std::equal(name.begin(), name.end(), list_reply.begin() + 8) &&
                        get32(list_reply, offset + 8, true) == 1,
                    "Mine list entry malformed");
            const auto registered = (std::uint64_t(get32(list_reply, offset, true)) << 32) |
                                    get32(list_reply, offset + 4, true);
            require(registered + 60 >= now() && registered <= now(), "Mine list registration time mismatch");
        }
        return successes;
    };
    require(request(1, 1) == 0 && wallet.balance(eid) == 250000, "Small clan was charged for mine entry");
    save_member(20);
    require(request(1, 2, 1) == 2 && wallet.balance(eid) == 150000, "Mine registration must charge once");
    require(request(2, 1) == 0 && wallet.balance(eid) == 150000, "Mine registration crossed channels");
    require(db.query("SELECT COUNT(*) AS count FROM native_mine_registrations").front().at("count") == 1,
            "Mine registration duplicated in SQLite");
    const auto config_path = root / "Config/server.json";
    const auto original = read_file(config_path);
    auto closed_config = Json::parse(original);
    closed_config["lupinel_mine_start_unix"] = std::int64_t(now()) + 72000;
    const auto closed_text = closed_config.dump();
    write_file(config_path, Bytes(closed_text.begin(), closed_text.end()));
    const auto closed = request(1, 1);
    write_file(config_path, original);
    require(closed == 0 && wallet.balance(eid) == 150000, "Mine registration accepted at or after the cutoff");
}
static void verify_drop_rule_precedence(const fs::path &root) {
    Content content(root);
    LegacyStore store(root / "Database/drop-precedence-test.sqlite");
    auto &db = store.db();
    db.exec("CREATE TABLE drop_rules(npc_id INTEGER,item_id INTEGER,chance_bp INTEGER,min_qty INTEGER,"
            "max_qty INTEGER,enabled INTEGER,PRIMARY KEY(npc_id,item_id))");
    Inventory inventory(db, content);
    GroundLoot loot(db, content, inventory);
    const auto junk = content.get("server/loot_distribution.json").at("junk_items");
    require(!junk.empty(), "Drop precedence fixture needs fallback items");
    Entity mob;
    mob.npc_id = 199999998;
    mob.map = "berneo";
    mob.level = 0; // Isolate the generic junk fallback from levelled equipment.
    for (const auto &item : junk)
        db.exec("INSERT INTO drop_rules VALUES(?,?,0,1,1,1)",
                {std::int64_t(mob.npc_id), item.get<std::int64_t>()});
    const auto item = junk.front().get<std::int64_t>();
    db.exec("INSERT INTO drop_rules VALUES(0,?,10000,1,1,1)", {item});
    loot.record("explicit-zero", mob, 990100, "drop-precedence", 1, now());
    const std::string tested_items = " AND item_id IN (SELECT item_id FROM drop_rules WHERE npc_id=199999998)";
    require(db.query("SELECT item_id FROM ground_items WHERE kill_id='explicit-zero'" + tested_items).empty(),
            "The fallback pool bypassed the explicit zero-percent drop rule");
    db.exec("UPDATE drop_rules SET chance_bp=10000,min_qty=2,max_qty=2 WHERE npc_id=? AND item_id=?",
            {std::int64_t(mob.npc_id), item});
    loot.record("explicit-full", mob, 990100, "drop-precedence", 1, now());
    const auto rows = db.query("SELECT item_id,quantity FROM ground_items WHERE kill_id='explicit-full'" + tested_items);
    require(rows.size() == 1 && rows.front().at("item_id") == item && rows.front().at("quantity") == 2,
            "The explicit item did not use the configured chance and quantity");
}
int wmain(int argc, wchar_t **argv) {
    auto fixture = fs::temp_directory_path() / wide("rosh_native_services_" + token(8));
    try {
        require(argc == 2 || (argc == 3 && std::wstring(argv[2]) == L"--spanish-mail"),
                "Usage: native service tests <encrypted content root> [--spanish-mail]");
        const bool spanishMail = argc == 3;
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
        fs::create_directories(fixture / "Config/Keys");
        fs::copy(fs::path(argv[1]) / "Data", fixture / "Data", fs::copy_options::recursive);
        fs::copy_file(fs::path(argv[1]) / "Config/Keys/content.key.dpapi",
                      fixture / "Config/Keys/content.key.dpapi");
        int port = free_port();
        auto text = Json{{"login_port", port}, {"world_hint_port", 10101}, {"gm_local_account_login_only", true},
                         {"lupinel_mine_start_unix", std::int64_t(now()) + 86400}}.dump();
        write_file(fixture / "Config/server.json", Bytes(text.begin(), text.end()));
        Identity account,otherAccount;
        {
            AccountStore users(fixture / "Database/karos_login.sqlite");
            account = users.register_account("CppFixture", "test-password-42");
            otherAccount = users.register_account("OtherFixture", "test-password-43");
            LegacyStore world(fixture / "Database/karos_gameplay.sqlite");
            Sqlite moderation(fixture / "Database/moderation.sqlite");
            Sqlite roles(fixture/"Database/karos_roles.sqlite");
            roles.exec("CREATE TABLE staff_roles(account_id TEXT,character_eid INTEGER,role TEXT,auth_generation TEXT,PRIMARY KEY(account_id,character_eid))");
        }
        verify_descada_brackets(fixture);
        verify_mine_registration(fixture);
        verify_drop_rule_precedence(fixture);
        {
            NativeService main(fixture, "MainServer"), lobby(fixture, "LobbyServer"),
                gate(fixture, "LoginServer");
            std::vector<std::unique_ptr<NativeService>> worlds;
            main.start();
            lobby.start();
            for (int i = 1; i <= 4; i++) {
                worlds.push_back(std::make_unique<NativeService>(fixture, "WorldServer", i));
                worlds.back()->start();
            }
            gate.start();
            {
                NativeService duplicate(fixture, "WorldServer", 1);
                bool rejected = false;
                try {
                    duplicate.start();
                } catch (...) {
                    rejected = true;
                }
                require(rejected, "Duplicate world consumer allowed");
            }
            {
                Client bad(port);
                bad.login("CppFixture", "test-password-42");
                require(bad.packet(11, 1) == notice("Entre pelo NeoAsmara Dashboard."),
                        "Game login still accepted the correct account password without a Dashboard ticket");
            }
            std::uint32_t eid = 0;
            std::uint32_t primaryEid = 0;
            std::uint32_t storedAccountUid = 0;
            std::uint32_t storedPersonalUid = 0;
            Bytes switchedChannel;
            Json savedPotionEffects;
            for (int channel : {1,2,5,3,4}) {
                const bool testOtherAccount=channel==5;
                if(testOtherAccount){
                    require(storedAccountUid!=0&&storedPersonalUid!=0,"Missing warehouse data before worker restart");
                    for(const int workerChannel:{2,3}){
                        auto &world=worlds.at(workerChannel-1);
                        world.reset();
                        world=std::make_unique<NativeService>(fixture,"WorldServer",workerChannel);
                        world->start();
                    }
                }
                const auto &loginAccount=testOtherAccount?otherAccount:account;
                const auto loginName=testOtherAccount?"OtherFixture":"CppFixture";
                std::string credential;
                {
                    AccountStore users(fixture/"Database/karos_login.sqlite");
                    const auto webSession=sha256(token());
                    const auto ticket=token();
                    users.db().exec("INSERT INTO login_sessions VALUES(?,?,?)",{webSession,loginAccount.id,now()+100});
                    users.db().exec("INSERT INTO game_tickets(token_hash,account_id,expires_at,used,session_hash) "
                                    "VALUES(?,?,?,0,?)",{sha256(ticket),loginAccount.id,now()+60,webSession});
                    if(testOtherAccount||(spanishMail&&channel==4))
                        users.db().exec("INSERT INTO game_ticket_locales VALUES(?,'es')",{sha256(ticket)});
                    credential="L1:"+ticket;
                }
                Client login(port);
                login.login(loginName, credential);
                login.packet(11, 0);
                login.packet(2, 0);
                auto address = login.packet(2, 6);
                int lobby_port = 0;
                for (std::size_t i = 0; i + 5 < address.size(); i++)
                    if (address[i] == 127 && address[i + 1] == 0 && address[i + 2] == 0 &&
                        address[i + 3] == 1)
                        lobby_port = get16(address, i + 4);
                require(lobby_port > 0, "Gate missing authenticated lobby endpoint");
                {
                    AccountStore users(fixture/"Database/karos_login.sqlite");
                    require(users.db().query("SELECT used FROM game_tickets WHERE token_hash=?",
                                             {sha256(credential.substr(3))}).at(0).at("used")==1,
                            "Dashboard ticket was not consumed");
                }
                Client mid(lobby_port);
                mid.login(loginName, "");
                mid.packet(11, 0);
                auto ack = mid.packet(2, 0);
                auto listing = mid.packet(2, 6);
                require(listing.size() == 76 && get16(listing, 4, true) == 4,
                        "Four independent public channels missing");
                for (int i = 0; i < 4; i++)
                    require(get16(listing, 14 + i * 16) == i + 1, "Incorrect public channel row");
                Bytes selection(18);
                put16(selection, 0, 1, true);
                put16(selection, 2, static_cast<std::uint16_t>(testOtherAccount?2:channel), true);
                Bytes transfer;
                if(channel==2){
                    require(!switchedChannel.empty()&&get16(switchedChannel,2,true)==2,
                            "World session did not transfer to channel 2");
                    transfer=switchedChannel;
                }else{
                    mid.send(2, 7, selection);
                    transfer=mid.packet(2, 7);
                }
                Client game(get16(transfer, 12, true));
                game.login(std::to_string(loginAccount.native_uid), std::to_string(get32(transfer, 14, true)));
                game.packet(11, 0);
                game.packet(2, 0);
                Content content(fixture);
                if (content.get("server/stage_templates.json")["game"].contains("020b"))
                    game.packet(2, 11);
                auto card = game.packet(2, 1);
                if (channel == 1 || channel == 3 || testOtherAccount) {
                    if(channel==1||testOtherAccount)
                        require(card.size() == 389 && card[0] == 1, "Empty account character state");
                    else
                        require(get32(card,1,true)==primaryEid,"The primary character card was missing");
                    const std::string name=testOtherAccount?"OtherMage":channel==1?"Mage":"StashMage";
                    Bytes create(15 + name.size() * 4);
                    put16(create, 0, 600);
                    auto appearance = unhex("01007f4050ff0300");
                    std::copy(appearance.begin(), appearance.end(), create.begin() + 2);
                    create[13] = static_cast<unsigned char>(name.size());
                    for (std::size_t i=0;i<name.size();++i)
                        put32(create, 15 + i * 4, name[i]);
                    game.send(2, 2, create);
                    card = game.packet(2, 1);
                    if(channel==3&&!card[0])card=game.packet(2,1);
                    eid = get32(card, 1, true);
                    if(channel==1)primaryEid=eid;
                    else require(eid!=primaryEid&&card[0],"The second character did not get a distinct card");
                    {Sqlite roles(fixture/"Database/karos_roles.sqlite");roles.exec("INSERT INTO staff_roles(account_id,character_eid,role) VALUES(?,?,'Beta')",{loginAccount.id,std::int64_t(eid)});}
                    require(get16(card, 11, true) == 256 && get16(card, 17) == 3,
                            "Creation appearance packet lost face or hair");
                } else
                    require(get32(card, 1, true) == eid, "Cross-channel character identity lost");
                std::uint32_t serviceActor=0;
                Position servicePosition;
                const std::string serviceMap=channel==3?"Tempteroon":"berneo";
                const std::uint32_t serviceMapId=channel==3?19004:19003;
                if(channel>=2){
                    EntityCatalog catalog(content);
                    const auto entities=catalog.load(serviceMap);
                    const auto type=channel==4?9300281u:channel==3?9400121u:9300131u;
                    const auto npc=std::find_if(entities.begin(),entities.end(),[&](const auto &row){
                        return row.second.npc_id==type&&row.second.alive;
                    });
                    require(npc!=entities.end(),"Native service NPC missing from its map");
                    serviceActor=npc->first;
                    servicePosition=npc->second.position;
                    Sqlite db(fixture/"Database/karos_gameplay.sqlite");
                    if(channel==2){
                        const auto old=db.query("SELECT x,y FROM character_locations WHERE eid=?",{std::int64_t(eid)}).at(0);
                        require(old.at("x")==9120&&old.at("y")==6600,"World channel switch lost the saved position");
                    }
                    db.exec("INSERT INTO character_locations VALUES(?,?,?,?,?,0,?) "
                            "ON CONFLICT(eid) DO UPDATE SET map_name=excluded.map_name,x=excluded.x,y=excluded.y,z=excluded.z",
                            {std::int64_t(eid),serviceMap,servicePosition.x,servicePosition.y,servicePosition.z,now()});
                    if(channel==3){
                        LegacyStore state(fixture/"Database/karos_gameplay.sqlite");
                        state.update_character(account.id,eid,[&](Json &ch){ch["map_id"]=serviceMapId;});
                    }
                }
                Bytes entry(4);
                put32(entry, 0, eid, true);
                game.send(2, 4, entry);
                auto enter = game.packet(2, 5);
                require(get32(enter, 0, true) == eid && get16(enter, 6, true) == 256 && get16(enter, 12) == 3,
                        "World entry lost selected character or appearance");
                auto map = game.packet(4, 4);
                require(map.size() == 17 && get32(map, 4, true) == serviceMapId,
                        "Initial or saved map did not match the selected character");
                Bytes ready(25);
                put32(ready, 0, eid, true);
                game.send(8, 9, ready);
                require(game.packet(8, 9) == ready, "Map ready acknowledgement mismatch");
                Bytes position(21);
                put32(position, 1, eid, true);
                putfloat(position, 5, 9000);
                putfloat(position, 9, 6500);
                putfloat(position, 13, 627);
                game.send(4, 0, position);
                auto own = game.packet(4, 2);
                require(get32(own, 2) == eid && get16(own, 248) == 3, "Own actor appearance lost hair");
                require(own.size()>261&&std::string(own.begin()+254,own.end()-1).rfind("[BETA] ",0)==0,"Role prefix missing from real world appearance");
                const float expectedX=channel==1?9000.f:static_cast<float>(servicePosition.x);
                require(getfloat(own, 6) == expectedX,
                        "New native spawn ignored or saved position replaced by client map spawn");
                // The restored Python bootstrap precedes the authoritative
                // gauges. Consume and validate each carrier explicitly.
                for (int opcode : {0x0a0f, 0x0f11, 0x1601}) {
                    for (const auto &record : content.get("server/world_core.json")) {
                        auto raw=unhex(record);
                        if(raw.size()>=6 && raw[0]*256+raw[1]==opcode) {
                            auto carrier=game.packet(opcode>>8,opcode&255);
                            require(carrier.size()==raw.size()-6,"Bootstrap carrier length changed");
                            break;
                        }
                    }
                }
                game.packet(8, 7);
                game.packet(8, 8);
                game.packet(8, 2);
                auto gauge = game.packet(8, 1);
                require(get16(gauge, 37) == 1 && get32(gauge, 9) == 150,
                        "Fresh character progression or HP incorrect");
                game.packet(8, 0);
                game.packet(5, 5);
                for (const auto &record : content.get("server/world_core.json")) {
                    auto raw=unhex(record);
                    if(raw.size()>=6 && raw[0]==5 && raw[1]==12) {
                        require(game.packet(5,12).size()==raw.size()-6,"Skill bootstrap length changed");
                        break;
                    }
                }
                auto questList=game.packet(9,2);require(questList.size()==6&&get16(questList,4,true)==0,"Fresh character inherited quests");auto questHistory=game.packet(9,4);require(questHistory.size()==8&&get16(questHistory,4,true)==0&&get16(questHistory,6,true)==0,"Fresh character inherited completions");
                if(testOtherAccount){
                    Bytes opening(22);
                    put32(opening,0,eid,true);put32(opening,4,serviceActor,true);put16(opening,20,0xc6,true);
                    game.send(6,0x2b,opening,4180);
                    require(get32(game.packet(6,0x2b),16,true)==0,"A different login could see the stored item");
                    auto withdrawal=opening;
                    put32(withdrawal,8,2,true);put32(withdrawal,12,0xffffffff,true);put32(withdrawal,16,0xffffffff,true);
                    game.send(6,0x2a,withdrawal,4181);
                    put16(withdrawal,20,0xc7,true);
                    require(game.packet(6,0x2a)==withdrawal,"A different login could withdraw the stored item");
                    require(game.packet(11,1)==notice("El objeto no está disponible en el depósito."),
                            "The Dashboard ticket locale did not reach the world notice");
                    eid=primaryEid;
                    continue;
                }
                {
                    Bytes invalid_equipment(11);
                    put32(invalid_equipment,0,eid,true);
                    invalid_equipment[4]=1;
                    invalid_equipment[6]=7;
                    put32(invalid_equipment,7,0xfffffffe,true);
                    game.send(6,0,invalid_equipment);
                    require(!game.packet(11,1).empty(),"Equipment refusal disconnected the world session");
                }
                for(int transport:{0,8,7}){
                    Bytes request(transport==0?9:7);if(transport==0)put32(request,0,eid,true);else request[5]=25;
                    std::string sender="UntrustedName",command=transport==0?"//gmstatus":"/gmstatus";request.insert(request.end(),sender.begin(),sender.end());request.push_back(0);request.insert(request.end(),command.begin(),command.end());request.push_back(0);if(transport==7){std::string native="gmstatus";request.assign(native.begin(),native.end());request.push_back(0);game.send(7,1,request);}else game.send(3,transport,request);
                    auto response=game.packet(3,0);require(response.size()>17&&get32(response,0,true)==eid&&get32(response,4,true)==1,"Command feedback is not native recipient chat");auto end=std::find(response.begin()+9,response.end(),0);require(end!=response.end()&&std::string(response.begin()+9,end)=="Server","Command response sender malformed");std::string text(end+1,response.end()-1);require(text.find("Cargo: Beta")!=std::string::npos&&text.find("nao autenticado")!=std::string::npos,"Native command failed to reply over real world connection");
                }
                {
                    Bytes request(8);put16(request,0,1,true);put16(request,2,1,true);
                    game.send(21,2,request);auto machines=game.packet(21,2);require(machines.size()==88&&get16(machines,4,true)==5,"Native Gacha machines absent on world channel");
                    game.send(21,3,request);auto prizes=game.packet(21,3);require(prizes.size()==8+12*get16(prizes,4,true)&&get16(prizes,4,true)>0,"Native Gacha prize layout invalid on world channel");
                }
                if (channel == 1) {
                    Bytes petList(14);
                    put32(petList,0,eid,true);put16(petList,8,0x01c9,true);
                    game.send(17,0,petList,4290);
                    put32(petList,10,1,true);
                    require(game.packet(17,0)==petList,"Empty native pet panel reply differs from capture");
                    {
                        Sqlite db(fixture / "Database/karos_gameplay.sqlite");
                        Inventory inventory(db, content);
                        const auto before = inventory.items(eid).size();
                        db.exec("INSERT INTO gm_mail VALUES(?,?,?,?,?,?,?,?)",
                                {std::string("transport-mail"),std::int64_t(eid),std::string("System"),
                                 std::string("Delivery test"),std::string("Collect at the mailbox"),
                                 Json::array({{{"id",54005},{"quantity",1}}}).dump(),
                                 std::string("pending"),now()});
                        require(game.packet(16,4)==Bytes({0,0,0,1}),
                                "New mail notice did not reach the real world connection");
                        require(inventory.items(eid).size()==before,
                                "New mail notice auto-granted the attachment");
                        db.exec("UPDATE gm_mail SET status='deleted' WHERE id='transport-mail'");
                    }
                    int source_slot = 0;
                    {
                        Sqlite db(fixture / "Database/karos_gameplay.sqlite");
                        Inventory inventory(db, content);
                        inventory.grant_once(eid, {{69009, 1}}, "wire-bag-test");
                        for (const auto &item : inventory.items(eid))
                            if (item.item == 69009)
                                source_slot = item.slot;
                    }
                    auto [category, slot] = Inventory::address(source_slot);
                    Bytes equip(11);
                    put32(equip, 0, eid, true);
                    equip[4] = static_cast<unsigned char>(category);
                    equip[5] = static_cast<unsigned char>(slot);
                    equip[6] = 13;
                    put32(equip, 7, 69009, true);
                    game.send(6, 0, equip);
                    auto equipped = game.packet(6, 0);
                    require(equipped[6] == 13, "Native equipment acknowledgement lost bag slot");
                    game.packet(8, 2);
                    game.packet(8, 0);
                    game.packet(8, 1);
                    {
                        Sqlite db(fixture / "Database/karos_gameplay.sqlite");
                        Inventory inventory(db, content);
                        constexpr std::uint32_t potion = 54005;
                        inventory.grant_once(eid, {{potion, 3}}, "wire-discard-stack", true);
                        const auto items = inventory.items(eid);
                        const auto selected = std::find_if(items.begin(), items.end(), [](const auto &item) {
                            return item.item == potion && item.quantity == 3;
                        });
                        require(selected != items.end(), "Missing discard fixture stack");
                        const auto [bag, cell] = Inventory::address(selected->slot);
                        Bytes move(16);
                        put32(move, 0, eid, true);
                        move[4] = bag;
                        move[5] = cell;
                        put32(move, 6, potion, true);
                        move[10] = 2;
                        move[11] = 9;
                        game.send(6, 8, move);
                        require(game.packet(6, 8) == move, "Native move to extra bag was not acknowledged");

                        Bytes discard(26);
                        put32(discard, 0, eid, true);
                        put32(discard, 4, 2, true);
                        put32(discard, 8, 9, true);
                        put32(discard, 12, potion, true);
                        put32(discard, 20, 1, true);
                        put16(discard, 24, 181, true);
                        const auto partialAck = discard;
                        discard.insert(discard.end(), {'1', '2', '2', '1', '1', 0});
                        // Genesis capture frames 78249 and 78643 use consecutive nonzero sequences.
                        game.send(6, 0x41, discard, 3788);
                        require(game.packet(6, 0x41) == partialAck, "Wrong discard acknowledgement");
                        const auto rest = inventory.owned(eid, Inventory::key(2, 9), potion, now());
                        require(rest.quantity == 2 && rest.uid == selected->uid,
                                "Partial discard changed the remaining stack identity or quantity");

                        game.send(6, 0x41, discard, 3788);
                        require(game.packet(6, 0x41) == partialAck &&
                                inventory.owned(eid, Inventory::key(2, 9), potion, now()).quantity == 2,
                                "Replayed discard removed another item");

                        auto tooMany=discard;
                        put32(tooMany,20,3,true);
                        game.send(6,0x41,tooMany,3790);
                        require(game.packet(11,1)==notice("A quantidade é maior do que você tem."),
                                "Oversized discard did not report the localized quantity error");
                        const auto afterRejected=inventory.owned(eid,Inventory::key(2,9),potion,now());
                        require(afterRejected.quantity==2&&afterRejected.uid==rest.uid,
                                "Rejected discard changed the remaining stack");

                        std::fill(discard.begin() + 16, discard.begin() + 24, 0xff);
                        game.send(6, 0x41, discard, 3789);
                        require(game.packet(6, 0x41) == Bytes(discard.begin(), discard.begin() + 26),
                                "Missing whole-stack discard acknowledgement");
                        const auto after = inventory.items(eid);
                        require(std::none_of(after.begin(), after.end(), [&](const auto &item) {
                            return item.uid == selected->uid;
                        }), "Discarded stack remained in persistent inventory");
                        require(db.query("SELECT id FROM ground_items WHERE instance_uid=?",{std::int64_t(rest.uid)}).empty(),
                                "Inventory discard exposed the item as a ground drop");
                    }
                    Bytes remove(11);
                    put32(remove, 0, eid, true);
                    remove[4] = 1;
                    remove[5] = 0;
                    remove[6] = 13;
                    put32(remove, 7, 69009, true);
                    game.send(6, 1, remove);
                    require(game.packet(6, 1)[6] == 13, "Native unequip acknowledgement lost slot");
                    game.packet(8, 2);
                    game.packet(8, 0);
                    game.packet(8, 1);
                }
                if(channel==1||channel==2||channel==4){
                    LegacyStore state(fixture/"Database/karos_gameplay.sqlite");
                    Inventory inventory(state.db(),content);
                    auto usePotion=[&](std::uint32_t id,std::uint16_t sequence){
                        const auto items=inventory.items(eid);
                        const auto item=std::find_if(items.begin(),items.end(),[&](const auto &entry){return entry.item==id;});
                        require(item!=items.end(),"Progression potion fixture missing");
                        const auto [bag,slot]=Inventory::address(item->slot);
                        Bytes use(16);
                        put32(use,0,eid,true);use[4]=bag;use[5]=slot;
                        put32(use,6,id,true);put32(use,11,eid,true);
                        game.send(6,2,use,sequence);
                        return use;
                    };
                    if(channel==1){
                        inventory.grant_once(eid,{{113001,2},{140004,1},{140002,1},{400140002,1},{140012,2}},"wire-progression");
                        std::uint16_t sequence=4310;
                        for(const auto id:{113001u,140004u,140002u,400140002u,140012u}){
                            const auto use=usePotion(id,sequence++);
                            require(game.packet(6,2)==use,"Progression potion use was not acknowledged");
                            game.packet(8,2);game.packet(8,0);game.packet(8,1);game.packet(4,2);
                            const auto icons=game.packet(5,9);
                            require(icons.size()==8+18u*get16(icons,6,true)&&get32(icons,1,true)==eid&&icons[5]==1,
                                    "Potion did not publish its native buff icon");
                            const auto saved=state.character(account.id,eid);
                            double deadline=0.;
                            for(const auto& effect:saved.at("consumable_effects"))if(effect.value("item_id",0u)==id)deadline=effect.at("expires");
                            bool foundIcon=false;
                            for(std::size_t offset=8;offset<icons.size();offset+=18)
                                if(get32(icons,offset+6,true)==id){
                                    foundIcon=true;
                                    require(std::abs(double(get32(icons,offset+10,true))-(deadline-now()))<=5.,
                                            "Potion icon changed the original expiration deadline");
                                }
                            require(foundIcon,"Potion source item is missing from the native buff bar");
                        }
                        const auto character=state.character(account.id,eid);
                        savedPotionEffects=character.at("consumable_effects");
                        require(Consumables::bonus(character,"xp",now())==40&&Consumables::bonus(character,"fletta",now())==80&&
                                Consumables::bonus(character,"monster_drops",now())==30,
                                "Distinct progression potion IDs did not stack in the world");
                    }else{
                        auto character=state.character(account.id,eid);
                        require(character.at("consumable_effects")==savedPotionEffects,
                                "World reconnect discarded or renewed the original potion deadlines");
                        usePotion(113001,4316);
                        require(game.packet(11,1)==notice("Este efeito ja esta ativo"),"The destination world did not load the active XP potion");
                        usePotion(140012,4317);
                        require(game.packet(11,1)==notice("Este efeito ja esta ativo"),"The destination world did not load the active drop potion");
                        character=state.character(account.id,eid);
                        require(character.at("consumable_effects")==savedPotionEffects,
                                "Rejected duplicate modified saved potion effects");
                    }
                }
                if(channel==2||channel==3){
                    Sqlite db(fixture/"Database/karos_gameplay.sqlite");
                    Inventory inventory(db,content);
                    constexpr std::uint32_t itemId=54005;
                    for(const unsigned listOpcode:{0x1cu,0x2bu}){
                        const bool personal=listOpcode==0x1c;
                        Bytes opening(22);
                        put32(opening,0,eid,true);put32(opening,4,serviceActor,true);put16(opening,20,0xc6,true);
                        game.send(6,listOpcode,opening);
                        const auto snapshot=game.packet(6,listOpcode);
                        require(get32(snapshot,12,true)==(personal?56u:16u),"Native stash capacity mismatch");
                        if(personal&&channel==3){
                            require(get32(snapshot,16,true)==0,"Sibling character could see the personal deposit");
                            auto withdrawal=opening;
                            put32(withdrawal,8,2,true);put32(withdrawal,12,0xffffffff,true);put32(withdrawal,16,0xffffffff,true);
                            game.send(6,listOpcode-1,withdrawal,4190);
                            put16(withdrawal,20,0xc7,true);
                            require(game.packet(6,listOpcode-1)==withdrawal,"Sibling character could withdraw from the personal deposit");
                            game.packet(11,1);
                            const auto owner=db.query("SELECT deposited_by FROM personal_storage WHERE uid=?",{std::int64_t(storedPersonalUid)});
                            require(owner.size()==1&&owner[0].at("deposited_by")==primaryEid,"Refused withdrawal changed the personal deposit");
                            continue;
                        }
                        std::uint32_t uid=storedAccountUid;
                        if(channel==2){
                            require(get32(snapshot,16,true)==0,"New deposit was not empty");
                            inventory.grant_once(eid,{{itemId,1}},personal?"wire-personal-stash":"wire-account-stash",true);
                            const auto items=inventory.items(eid);
                            const auto item=std::find_if(items.rbegin(),items.rend(),[](const auto &entry){return entry.item==itemId;});
                            require(item!=items.rend(),"Missing item for native deposit");
                            uid=item->uid;
                            const auto [bag,cell]=Inventory::address(item->slot);
                            Bytes deposit(34);
                            put32(deposit,0,eid,true);put32(deposit,4,serviceActor,true);
                            put32(deposit,8,bag,true);put32(deposit,12,cell,true);put32(deposit,16,itemId,true);
                            put32(deposit,24,1,true);put32(deposit,28,2,true);put16(deposit,32,0xc6,true);
                            game.send(6,listOpcode-2,deposit,4100+listOpcode);
                            require(game.packet(6,listOpcode-2)==deposit,"Native deposit ACK mismatch");
                            const auto deposited=game.packet(6,listOpcode);
                            require(get32(deposited,16,true)==1&&get32(deposited,29)==uid,"Native deposit changed item identity");
                            if(!personal){storedAccountUid=uid;continue;}
                        }else{
                            require(get32(snapshot,16,true)==1&&get32(snapshot,29)==uid,
                                    "Account deposit was lost after restarting the world workers");
                        }
                        Bytes withdrawal=opening;
                        put32(withdrawal,8,2,true);put32(withdrawal,12,0xffffffff,true);put32(withdrawal,16,0xffffffff,true);
                        game.send(6,listOpcode-1,withdrawal,4150+listOpcode);
                        require(game.packet(6,listOpcode-1)==withdrawal,"Native withdrawal ACK mismatch");
                        require(get32(game.packet(6,listOpcode),16,true)==0,"Withdrawn item remained in the stash");
                        const auto after=inventory.items(eid);
                        const auto restored=std::find_if(after.begin(),after.end(),[&](const auto &entry){return entry.uid==uid;});
                        require(restored!=after.end(),
                                "Withdrawal did not restore the original item UID");
                        if(personal){
                            const auto [bag,cell]=Inventory::address(restored->slot);
                            Bytes deposit(34);
                            put32(deposit,0,eid,true);put32(deposit,4,serviceActor,true);
                            put32(deposit,8,bag,true);put32(deposit,12,cell,true);put32(deposit,16,itemId,true);
                            put32(deposit,24,1,true);put32(deposit,28,2,true);put16(deposit,32,0xc6,true);
                            game.send(6,listOpcode-2,deposit,4191);
                            require(game.packet(6,listOpcode-2)==deposit,"Personal redeposit ACK mismatch");
                            require(get32(game.packet(6,listOpcode),29)==uid,"Personal redeposit changed item identity");
                            storedPersonalUid=uid;
                        }
                    }
                }
                if(channel==3){
                    Sqlite db(fixture/"Database/karos_gameplay.sqlite");
                    {
                        Transaction tx(db);
                        db.exec("INSERT INTO native_guilds(id,name) VALUES(?,?)",
                                {std::int64_t(23),std::string("Transport Guild")});
                        db.exec("INSERT INTO native_guild_members VALUES(?,?,?,5)",
                                {std::int64_t(eid),account.id,std::int64_t(23)});
                        tx.commit();
                    }
                    require(get32(game.packet(12,0),0)==23,"Live guild membership did not reach the client");
                    require(get32(game.packet(12,1),20)==5,"Guild leader rank missing from native roster");
                    Bytes settings(58);
                    put32(settings,0,eid,true);put32(settings,4,23,true);put16(settings,8,0xee,true);
                    put32(settings,10,1,true);put32(settings,10+9*4,1,true);
                    for(const std::string field:{"Transport notice","","One","Two","Three","Four","Five"}){
                        settings.insert(settings.end(),field.begin(),field.end());settings.push_back(0);
                    }
                    game.send(12,0x10,settings,4210);
                    const auto guild=game.packet(12,0);
                    require(get32(guild,0x34)==1&&get32(guild,0x40)==0&&get32(guild,0x58)==1,
                            "Native guild permission response changed the chosen values");
                    game.packet(12,1);
                    game.packet(11,1);
                    const auto rights=db.query("SELECT r.rights,c.rights AS coins FROM native_guild_storage_rights r "
                                               "JOIN native_guild_carats_rights c ON c.guild_id=r.guild_id AND c.rank=r.rank "
                                               "WHERE r.guild_id=23 AND r.rank=3").at(0);
                    require(rights.at("rights")==3&&rights.at("coins")==2,"Guild rights did not persist in SQLite");
                    Bytes opening(22);
                    put32(opening,0,eid,true);put32(opening,4,serviceActor,true);put16(opening,20,0xc6,true);
                    game.send(12,0x0e,opening,4211);
                    const auto stash=game.packet(12,0x0e);
                    require(stash.size()==22&&get32(stash,12,true)==56,"Guild NPC did not open native storage");
                    Inventory inventory(db,content);
                    const auto items=inventory.items(eid);
                    const auto memberItem=std::find_if(items.begin(),items.end(),[&](const auto &entry){
                        return entry.uid==storedAccountUid;
                    });
                    require(memberItem!=items.end(),"Guild transfer fixture item missing");
                    const auto [bag,cell]=Inventory::address(memberItem->slot);
                    Bytes deposit(34);
                    put32(deposit,0,eid,true);put32(deposit,4,serviceActor,true);
                    put32(deposit,8,bag,true);put32(deposit,12,cell,true);put32(deposit,16,memberItem->item,true);
                    put32(deposit,24,1,true);put32(deposit,28,2,true);put16(deposit,32,0xc6,true);
                    game.send(12,0x0c,deposit,4212);
                    require(game.packet(12,0x0c)==deposit,"Guild deposit ACK mismatch");
                    require(get32(game.packet(12,0x0e),29)==memberItem->uid,"Guild stash changed item identity");
                    Bytes withdrawal=opening;
                    put32(withdrawal,8,2,true);put32(withdrawal,12,0xffffffff,true);put32(withdrawal,16,0xffffffff,true);
                    db.exec("UPDATE native_guild_members SET rank=3 WHERE eid=?",{std::int64_t(eid)});
                    game.send(12,0x0d,withdrawal,4214);
                    auto refused=withdrawal;
                    put16(refused,20,0xc7,true);
                    require(game.packet(12,0x0d)==refused,"Guild withdrawal ignored a live rank restriction");
                    require(game.packet(11,1)==notice("Seu cargo não permite esta operação no depósito do clã."),
                            "Restricted withdrawal did not explain the permission error");
                    game.send(12,0x0e,opening,4215);
                    require(get32(game.packet(12,0x0e),29)==memberItem->uid,
                            "A refused withdrawal changed the stored item");
                    db.exec("UPDATE native_guild_members SET rank=5 WHERE eid=?",{std::int64_t(eid)});
                    game.send(12,0x0d,withdrawal,4213);
                    require(game.packet(12,0x0d)==withdrawal,"Guild withdrawal ACK mismatch");
                    require(get32(game.packet(12,0x0e),16,true)==0,"Guild stash retained withdrawn item");
                    const auto returned=inventory.items(eid);
                    require(std::any_of(returned.begin(),returned.end(),[&](const auto &entry){return entry.uid==memberItem->uid;}),
                            "Guild withdrawal did not restore the original item UID");
                    Wallet wallet(db,inventory);
                    const auto startingCarats=wallet.apply(eid,1000,"wire-guild-wallet-funds").first;
                    const auto moneyItems=inventory.items(eid);
                    const auto coins=std::find_if(moneyItems.begin(),moneyItems.end(),[](const auto &item){return item.item==999999;});
                    require(coins!=moneyItems.end(),"Missing native Carats stack");
                    const auto [moneyBag,moneySlot]=Inventory::address(coins->slot);
                    auto coinDeposit=deposit;
                    put32(coinDeposit,8,moneyBag,true);put32(coinDeposit,12,moneySlot,true);
                    put32(coinDeposit,16,999999,true);put32(coinDeposit,24,100,true);
                    db.exec("UPDATE native_guild_members SET rank=3 WHERE eid=?",{std::int64_t(eid)});
                    game.send(12,0x0c,coinDeposit,4220);
                    require(game.packet(11,1)==notice("Seu cargo não permite esta operação no depósito do clã."),
                            "Item deposit permission also granted Carats deposit");
                    require(wallet.balance(eid)==startingCarats,"Refused Carats deposit changed the wallet");
                    db.exec("UPDATE native_guild_members SET rank=5 WHERE eid=?",{std::int64_t(eid)});
                    game.send(12,0x0c,coinDeposit,4221);
                    require(game.packet(12,0x0c)==coinDeposit,"Leader Carats deposit ACK mismatch");
                    require(get32(game.packet(12,0x0e),16,true)==1&&wallet.balance(eid)==startingCarats-100,
                            "Leader Carats deposit did not update the wallet and warehouse");
                    db.exec("UPDATE native_guild_members SET rank=3 WHERE eid=?",{std::int64_t(eid)});
                    auto coinWithdrawal=withdrawal;
                    put32(coinWithdrawal,12,0,true);put32(coinWithdrawal,16,40,true);
                    game.send(12,0x0d,coinWithdrawal,4222);
                    require(game.packet(12,0x0d)==coinWithdrawal,"Independent Carats withdrawal permission was ignored");
                    require(get32(game.packet(12,0x0e),16,true)==1&&wallet.balance(eid)==startingCarats-60,
                            "Partial Carats withdrawal did not preserve the remaining balance");
                    db.exec("DELETE FROM native_guild_members WHERE eid=?",{std::int64_t(eid)});
                    constexpr std::uint32_t nearActor=64001,farActor=64002,lootItem=54005;
                    constexpr std::uint32_t nearUid=4000000001u;
                    const auto stamp=now();
                    const auto scope=SpawnPersistence::scope(serviceMap,channel,serviceMapId);
                    const auto distance=content.get("server/drop_policy.json").value("pickup_distance",75.);
                    constexpr double boundaryMargin=0.25;
                    const auto dropX=[&](std::uint32_t actor){
                        return static_cast<float>(static_cast<float>(servicePosition.x)+distance+
                                                  (actor==nearActor?-boundaryMargin:boundaryMargin));
                    };
                    {
                        Transaction tx(db);
                        for(const auto actor:{nearActor,farActor}){
                            db.exec("INSERT INTO ground_items(id,kill_id,map_name,item_id,quantity,x,y,z,owner_eid,"
                                    "created_at,expires_at,instance_uid,instance_created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                                    {std::int64_t(actor),std::string("wire-drop-distance"),scope,std::int64_t(lootItem),
                                     std::int64_t(1),double(dropX(actor)),servicePosition.y,servicePosition.z,std::int64_t(eid),stamp,
                                     stamp+120,std::int64_t(nearUid+actor-nearActor),stamp});
                        }
                        tx.commit();
                    }
                    for(const auto actor:{nearActor,farActor}){
                        const auto spawn=game.packet(6,4);
                        require(spawn.size()==181&&get32(spawn,4,true)==actor&&get32(spawn,22)==lootItem&&
                                getfloat(spawn,167)==dropX(actor),
                                "Ground appearance did not reach the world connection");
                    }
                    Bytes pick(14);
                    put32(pick,0,eid,true);put32(pick,4,farActor,true);
                    put32(pick,8,lootItem,true);put16(pick,12,0x005a,true);
                    game.send(6,3,pick,4214);
                    require(game.packet(11,1)==notice("Você está longe demais do item."),"Out-of-range item was not refused");
                    put32(pick,4,nearActor,true);
                    game.send(6,3,pick,4215);
                    require(game.packet(6,3)==pick,"Nearby pickup did not get the native ACK");
                    game.packet(11,1);
                    const auto bagAfterPickup=inventory.items(eid);
                    require(std::any_of(bagAfterPickup.begin(),bagAfterPickup.end(),[](const auto &entry){return entry.uid==nearUid;}),
                            "Ground pickup did not preserve item identity");
                    db.exec("UPDATE ground_items SET expires_at=? WHERE id=?",{now()-1,std::int64_t(farActor)});
                    const auto removed=game.packet(6,3);
                    require(get32(removed,0,true)==0xffffffff&&get32(removed,4,true)==farActor&&get16(removed,12,true)==0x01d6,
                            "Expired drop did not leave the client's ground scene");
                    constexpr std::uint32_t recycledUid=4000000003u;
                    db.exec("UPDATE ground_items SET kill_id='wire-drop-recycled',instance_uid=?,picked_at=NULL,"
                            "expires_at=? WHERE id=?",{std::int64_t(recycledUid),now()+120,std::int64_t(nearActor)});
                    require(get32(game.packet(6,4),4,true)==nearActor,"Recycled actor did not reappear");
                    game.send(6,3,pick,4215);
                    require(game.packet(11,1)==notice("Pedido de coleta ja processado"),
                            "Delayed replay consumed a different drop with the same actor");
                    require(db.query("SELECT picked_at FROM ground_items WHERE id=?",{std::int64_t(nearActor)}).at(0).at("picked_at").is_null(),
                            "Delayed replay changed the replacement drop");
                    game.send(6,3,pick,4216);
                    require(game.packet(6,3)==pick,"Fresh request could not collect the replacement drop");
                    game.packet(11,1);
                }
                if(channel==4){
                    Sqlite db(fixture/"Database/karos_gameplay.sqlite");
                    Inventory inventory(db,content);
                    auto quantity=[&](){
                        std::int64_t total=0;
                        for(const auto &item:inventory.items(eid))if(item.item==54005)total+=item.quantity;
                        return total;
                    };
                    const auto before=quantity();
                    constexpr std::int64_t sender=999900;
                    const auto items=Json::array({{{"id",54005},{"quantity",1}}});
                    AccountStore users(fixture/"Database/karos_login.sqlite");
                    const auto staff=users.register_account("TestStaffAccount","fixture-staff-pass");
                    db.exec("INSERT INTO accounts VALUES(?,?)",
                            {staff.id,Json{{"chars",Json::array({{{"eid",sender},{"name","TestStaff"}}})}}.dump()});
                    Sqlite roles(fixture/"Database/karos_roles.sqlite");
                    roles.exec("INSERT INTO staff_roles VALUES(?,?,'admin',?)",
                               {staff.id,sender,std::string("transport-generation")});
                    Sqlite staffCredentials(fixture/"Database/staff_second_login.sqlite");
                    GmController gm(fixture);
                    const auto authorization=gm.request("POST","/login",
                        {{"username","TestStaffAccount"},{"password","fixture-staff-pass"},{"character","TestStaff"}});
                    require(gm.request("GET","/status",Json::object(),authorization.at("token")).at("native_mail_ui")==true,
                            "GM Tool did not advertise native mailbox support");
                    const auto action=gm.request("POST","/action",
                        {{"request_id","transport-claim-0001"},{"kind","mail"},{"target_eid",eid},{"items",items},
                         {"sender","Sistema"},{"title","Attachment"},{"message","Collect at the mailbox"},{"enchant",0}},
                        authorization.at("token"));
                    require(action.at("status")=="pending"&&quantity()==before,
                            "GM Tool did not queue the attachment for NPC retrieval");
                    const auto mailId=action.at("id").get<std::string>();
                    require(game.packet(16,4)==Bytes({0,0,0,1}),"Mailbox claim fixture notice missing");
                    game.send(16,2,Bytes(15));
                    require(game.packet(11,1)==notice(spanishMail?"Solicitud de buzón no válida.":"Pedido de correio inválido."),
                            "Malformed mailbox request did not return the localized warning");
                    Bytes opening(16);
                    put32(opening,12,serviceActor,true);
                    game.send(16,2,opening);
                    const auto list=game.packet(16,2);
                    require(list.size()>24&&get32(list,8,true)==1,"NPC did not return the queued letter");
                    Bytes key(list.begin()+16,list.begin()+24);
                    Bytes read=key;
                    read.resize(10);
                    put16(read,8,2,true);
                    game.send(16,1,read);
                    game.packet(16,2);
                    const auto detail=game.packet(16,1);
                    constexpr std::size_t attachments=8*147+6;
                    require(detail.size()>=attachments&&get16(detail,detail.size()-attachments,true)==1,
                            "NPC did not show the attachment");
                    Bytes deleteLetter(4);
                    put32(deleteLetter,0,1,true);
                    deleteLetter.insert(deleteLetter.end(),read.begin(),read.end());
                    game.send(16,3,deleteLetter,4202);
                    require(game.packet(16,3)==Bytes(4),"Mailbox deleted an unclaimed attachment");
                    const auto warning=spanishMail?"Algunas cartas no se eliminaron. Recoge los adjuntos antes de borrarlas.":
                        "Algumas cartas não foram apagadas. Retire os anexos antes de apagá-las.";
                    require(game.packet(11,1)==notice(warning),
                            "Mailbox did not explain why the letter could not be deleted");
                    Bytes claim=key;
                    claim.resize(18);
                    put16(claim,16,2,true);
                    auto freeSlots=inventory.available(eid,now());
                    for(const auto &owned:inventory.items(eid))freeSlots.erase(owned.slot);
                    require(!freeSlots.empty(),"Mail capacity fixture already full");
                    std::uint32_t filler=0;
                    const auto rules=content.get("data/native_item_rules.json");
                    for(auto it=rules.begin();it!=rules.end();++it){
                        if(it.value().value("kind",0)==1&&it.value().value("stack",1)==1&&it.value().value("duration",0)==0){
                            filler=static_cast<std::uint32_t>(std::stoul(it.key()));
                            break;
                        }
                    }
                    require(filler!=0,"No permanent equipment to fill the fixture bags");
                    inventory.grant_once(eid,{{filler,static_cast<int>(freeSlots.size())}},"wire-full-mail-bags",true);
                    game.send(16,5,claim,4204);
                    const auto fullBagWarning=spanishMail?"Todas las bolsas están llenas.":"Todas as bolsas estão cheias.";
                    require(game.packet(11,1)==notice(fullBagWarning),"Full bag claim did not return the localized warning");
                    const auto queued=db.query("SELECT status FROM gm_mail WHERE id=?",{mailId});
                    require(queued.size()==1&&queued[0].at("status")=="pending"&&quantity()==before&&
                            db.query("SELECT uid FROM gm_item_origins WHERE action_id=?",{mailId}).empty(),
                            "Failed claim changed the letter, item quantity or origin audit");
                    db.exec("DELETE FROM inventory_slots WHERE player_eid=? AND slot=? AND item_id=?",
                            {std::int64_t(eid),std::int64_t(*freeSlots.begin()),std::int64_t(filler)});
                    for(int attempt=0;attempt<2;++attempt){
                        game.send(16,5,claim,4200+attempt);
                        require(game.packet(16,8)==read,"NPC did not acknowledge the claim");
                        game.packet(16,2);
                        const auto claimed=game.packet(16,1);
                        require(claimed.size()>=attachments&&get16(claimed,claimed.size()-attachments,true)==0,
                                "Claimed attachment was still available");
                        require(quantity()==before+1,"Attachment was not granted exactly once");
                        require(db.query("SELECT uid FROM gm_item_origins WHERE action_id=?",{mailId}).size()==1,
                                "Mail claim did not retain one item origin audit record");
                    }
                    game.send(16,3,deleteLetter,4203);
                    require(game.packet(16,3)==deleteLetter,"Mailbox could not delete the claimed letter");
                    require(get32(game.packet(16,2),8,true)==0,"Deleted letter remained in the inbox");
                    require(quantity()==before+1&&db.query("SELECT uid FROM gm_item_origins WHERE action_id=?",{mailId}).size()==1,
                            "Deleting the letter changed the claimed item or its audit record");
                }
                putfloat(position, 5, 9120);
                putfloat(position, 9, 6600);
                game.send(4, 0, position);
                if(channel==1){
                    Bytes switchRequest(18);
                    put16(switchRequest,0,1,true);put16(switchRequest,2,99,true);
                    game.send(2,7,switchRequest,4300);
                    require(game.packet(11,1)==notice("Canal indisponivel."),"Invalid channel did not report an error");
                    game.send(7,1,Bytes{'c','h','p','i','c','k',' ','2',0},4301);
                    switchedChannel=game.packet(2,7);
                    continue;
                }
                game.send(2, 1, {});
                if (content.get("server/stage_templates.json")["game"].contains("020b"))
                    game.packet(2, 11);
                auto returned=game.packet(2,1);
                if(channel==3&&!returned[0])returned=game.packet(2,1);
                require(get32(returned,1,true)==eid,"Return to character selection lost cards");
                if(channel==3){
                    Bytes remove(4);
                    put32(remove,0,eid,true);
                    game.send(2,3,remove);
                    require(game.packet(2,3)==remove,"Second character delete ACK missing");
                    require(get32(game.packet(2,1),1,true)==primaryEid,"Deleting the second character lost the first");
                    eid=primaryEid;
                }
                if (channel == 4) {
                    Bytes remove(4);
                    put32(remove, 0, eid, true);
                    game.send(2, 3, remove);
                    require(game.packet(2, 3) == remove, "Character delete ack");
                    require(game.packet(2, 1).size() == 389, "Deleted card remained");
                }
            }
            gate.stop();
            lobby.stop();
            for (auto &world : worlds)
                world->stop();
            main.stop();
            {
                LegacyStore state(fixture / "Database/karos_gameplay.sqlite");
                require(state.characters(account.id).empty(), "Character deletion not persisted");
            }
            require(main.cluster().size() == 7, "Missing independent native service heartbeat");
        }
        std::cout << "PASS: Dashboard-only login -> lobby -> all four world channels, live channel transfer and potion persistence, account deposit isolation, "
                     "independent workers, duplicate-instance lock, extra-bag discard, pickup range and expiry, personal/account/guild deposits, NPC mailbox claim, character appearance/create/delete and "
                     "SQLite persistence\n";
        fs::remove_all(fixture);
        WSACleanup();
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "FAIL: " << ex.what() << "\nEvidence: " << fixture.string() << "\n";
        return 1;
    }
}

