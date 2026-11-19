#include "native/core.hpp"
#include <algorithm>
#include <iostream>
#include <set>
using namespace rosh;
static std::string category(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.find("template") != std::string::npos || name.find("world_core") != std::string::npos ||
        name.find("world_enter_seq") != std::string::npos || name.find("capture") != std::string::npos ||
        name.find("observed") != std::string::npos)
        return "Protocol";
    if (name.find("quest") != std::string::npos)
        return "Quests";
    if (name.find("drop") != std::string::npos || name.find("loot") != std::string::npos)
        return "Drops";
    if (name.find("event") != std::string::npos || name.find("attendance") != std::string::npos ||
        name.find("reward") != std::string::npos || name.find("gacha") != std::string::npos ||
        name.find("starter_costume") != std::string::npos)
        return "Events";
    if (name.find("npc") != std::string::npos)
        return "NPCs";
    if (name.find("spawn") != std::string::npos || name.find("mob") != std::string::npos ||
        name.find("boss") != std::string::npos)
        return "Mobs";
    if (name.find("item") != std::string::npos || name.find("equip") != std::string::npos ||
        name.find("enchant") != std::string::npos || name.find("mount") != std::string::npos)
        return "Items";
    return "Rules";
}
static void pack(const fs::path &source, const fs::path &root,const fs::path &overrides={}) {
    fs::create_directories(root / "Data");
    auto keypath = root / "Config/Keys/content.key.dpapi";
    Bytes key = fs::exists(keypath) ? unprotect_key(keypath) : random_bytes(32);
    if (!fs::exists(keypath))
        protect_key(keypath, key);
    auto owner=source/"data/owner_authority.private.json";
    if(fs::is_regular_file(owner))write_file(root/"Config/Keys/owner_authority.private.json.dat",encrypt_content(read_file(owner),key,"owner_authority.private.json"));
    std::map<std::string, Json> bundles;
    std::map<std::string, std::string> groups;
    Json manifest = {
        {"schema", 1}, {"format", "AES-256-GCM/DPAPI-current-user"}, {"resources", Json::object()}};
    std::vector<fs::path> layers={source};if(!overrides.empty())layers.push_back(overrides);
    for(const auto&layer:layers)for (const auto &folder : {"data", "server"}) {
        if(!fs::is_directory(layer/folder))continue;
        for (const auto &entry : fs::directory_iterator(layer / folder)) {
            if (!entry.is_regular_file() || entry.path().extension() != L".json")
                continue;
            auto filename = entry.path().filename().string();
            if (filename.find(".private.") != std::string::npos || filename == "launcher_world_status.json")
                continue;
            std::string name = std::string(folder) + "/" + filename;
            auto bytes = read_file(entry.path());
            auto data = Json::parse(bytes);
            auto group = category(filename);
            bundles[group][name] = std::move(data);
            groups[name] = group;
            if(layer!=source)manifest["sourceOverrides"][name]=hex(sha256(bytes));
        }
    }
    for (const auto &[group, content] : bundles) {
        auto text = content.dump();
        auto encrypted = encrypt_content(Bytes(text.begin(), text.end()), key, group);
        write_file(root / "Data" / (group + ".dat"), encrypted);
        auto hash = hex(sha256(encrypted));
        for (const auto &[name, assigned] : groups)
            if (assigned == group)
                manifest["resources"][name] = {
                    {"file", group + ".dat"}, {"category", group}, {"member", name}, {"sha256", hash}};
        std::cout << group << ".dat: " << content.size() << " resources, " << encrypted.size()
                  << " encrypted bytes\n";
    }
    for (const auto &filename : {"content.sqlite", "content_typed.sqlite"}) {
        auto source_db = source / "data" / filename;
        if (!fs::exists(source_db))
            continue;
        std::string name = "database/" + std::string(filename);
        auto plain = serialize_database(source_db);
        auto encrypted = encrypt_content(plain, key, name);
        std::string target = std::string(filename) == "content.sqlite" ? "Content.dat" : "ContentTyped.dat";
        write_file(root / "Data" / target, encrypted);
        manifest["resources"][name] = {
            {"file", target}, {"format", "sqlite-image"}, {"sha256", hex(sha256(encrypted))}};
        std::cout << target << ": encrypted SQLite content, memory-only reader\n";
    }
    auto text = manifest.dump();
    write_file(root / "Data/catalog.dat", encrypt_content(Bytes(text.begin(), text.end()), key, "catalog"));
    Content verify(root);
    for (const auto &[name, group] : groups)
        if (verify.get(name) != bundles[group][name])
            throw std::runtime_error("Encrypted content readback mismatch");
    std::cout << "PASS: " << groups.size() << " native content resources authenticated and read back\n";
}
static void migrate(const fs::path &source, const fs::path &root) {
    auto destination = root / "Database";
    fs::create_directories(destination);
    Json report = Json::array();
    for (const auto &entry : fs::directory_iterator(source / "data")) {
        if (!entry.is_regular_file() || entry.path().extension() != L".sqlite")
            continue;
        auto filename = entry.path().filename().string();
        if (filename == "content.sqlite" || filename == "content_typed.sqlite")
            continue;
        auto target = destination / entry.path().filename();
        snapshot_database(entry.path(), target);
        report.push_back({{"name", filename}, {"quick_check", "ok"}});
    }
    for (const auto &name : {"karos_login.sqlite", "moderation.sqlite", "asmara_website.sqlite"}) {
        auto input = source / "SITE/data" / name;
        if (fs::exists(input)) {
            snapshot_database(input, destination / name);
            report.push_back({{"name", name}, {"quick_check", "ok"}});
        }
    }
    for (const auto &name : {"owner_authority.private.json"}) {
        auto input = source / "data" / name;
        if (fs::exists(input)) {
            auto key = unprotect_key(root / "Config/Keys/content.key.dpapi");
            write_file(root / "Config/Keys" / (std::string(name) + ".dat"),
                       encrypt_content(read_file(input), key, name));
        }
    }
    auto text = Json{{"source", source.u8string()},
                     {"source_mode", "read-only SQLite backup"},
                     {"databases", report},
                     {"content_databases",
                      "Content.dat and ContentTyped.dat: encrypted SQLite images; memory-only queries"}}
                    .dump(2);
    write_file(root / "Logs/migration.json", Bytes(text.begin(), text.end()));
    std::cout << "PASS: " << report.size()
              << " consistent SQLite database snapshots; source databases unchanged\n";
}
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc < 3)
            throw std::runtime_error("Usage: KarosDataTool pack|migrate <readonly Python server source> "
                                     "<native destination>; verify <native root>");
        auto command = utf8(argv[1]);
        if(command=="copy-db"){
            if(argc!=4)throw std::runtime_error("Usage: copy-db <source database> <new snapshot>");
            auto source=fs::weakly_canonical(argv[2]),destination=fs::weakly_canonical(argv[3]);
            if(source==destination||fs::exists(destination))throw std::runtime_error("Snapshot destination must be a new file");
            snapshot_database(source,destination);std::cout<<"PASS: consistent SQLite snapshot\n";return 0;
        }
        if(command=="status"){
            Sqlite db(fs::path(argv[2])/"Database/runtime.sqlite",true);
            std::cout<<db.query("SELECT instance,pid,updated,payload FROM native_services ORDER BY instance").dump()<<"\n";return 0;
        }
        if(command=="probe-cluster"){
            Sqlite db(fs::path(argv[2])/"Database/runtime.sqlite");
            auto rows=db.query("SELECT instance FROM native_services WHERE updated>?",{now()-5});
            if(rows.size()!=7)throw std::runtime_error("Expected seven independent native processes");
            std::vector<std::string> ids;for(const auto& row:rows){auto id=token();ids.push_back(id);db.exec("INSERT INTO native_jobs VALUES(?,?,?,'{}',?,'pending',NULL)",{id,row.at("instance").get<std::string>(),std::string("checkpoint"),now()});}
            auto deadline=now()+10;for(const auto& id:ids){bool done=false;while(now()<deadline){auto result=db.query("SELECT status,result FROM native_jobs WHERE id=?",{id});if(!result.empty()&&result[0]["status"]=="done"){done=true;break;}if(!result.empty()&&result[0]["status"]=="failed")throw std::runtime_error("Native process rejected checkpoint probe");Sleep(25);}if(!done)throw std::runtime_error("Native process did not execute its probe");}
            std::cout<<"PASS: all seven EXE processes executed SQLite IPC checkpoints\n";return 0;
        }
        if (command == "verify") {
            Content content(argv[2]);
            auto manifest = content.status();
            for (auto it = manifest["resources"].begin(); it != manifest["resources"].end(); ++it) {
                if (it.value().value("format", "") == "sqlite-image") {
                    auto db = content.database(it.key());
                    if (db->query("PRAGMA quick_check")[0].begin().value() != "ok")
                        throw std::runtime_error("Encrypted content database integrity failed");
                } else
                    content.get(it.key());
            }
            std::cout << "PASS: all encrypted native data files verified\n";
            return 0;
        }
        if (argc != 4 && !(command=="pack"&&argc==5))
            throw std::runtime_error("Missing source or destination");
        auto source = fs::weakly_canonical(argv[2]), destination = fs::weakly_canonical(argv[3]);
        auto relative = destination.lexically_relative(source);
        if (relative.empty() || relative == "." || *relative.begin() != "..")
            throw std::runtime_error("Destination must be outside the read-only Python source tree");
        auto parent = source.lexically_relative(destination);
        if (parent.empty() || parent == "." || *parent.begin() != "..")
            throw std::runtime_error("Destination cannot contain the read-only Python source tree");
        if (command == "pack")
            pack(source, destination,argc==5?fs::path(argv[4]):fs::path());
        else if (command == "migrate") {
            if (fs::exists(destination / "Database") && !fs::is_empty(destination / "Database"))
                throw std::runtime_error("Refusing to overwrite existing native player databases");
            migrate(source, destination);
        } else
            throw std::runtime_error("Unknown command");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
