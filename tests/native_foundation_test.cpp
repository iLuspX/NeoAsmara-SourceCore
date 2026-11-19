#include "native/core.hpp"
#include <iostream>
#include <thread>
using namespace rosh;
static void require(bool condition, const char *name) {
    if (!condition)
        throw std::runtime_error(name);
}
template <class Action> static void rejects(Action action, const char *name) {
    bool failed = false;
    try {
        action();
    } catch (const std::exception &) {
        failed = true;
    }
    require(failed, name);
}
int main() {
    auto path = fs::temp_directory_path() / wide("rosh_cpp_test_" + token(8));
    fs::create_directories(path);
    try {
        auto key = random_bytes(32);
        Bytes message{'n', 'a', 't', 'i', 'v', 'e'};
        auto a = encrypt_content(message, key, "items"), b = encrypt_content(message, key, "items");
        require(a != b, "GCM nonce reuse");
        require(decrypt_content(a, key, "items") == message, "GCM roundtrip");
        auto altered = a;
        altered[25] ^= 1;
        rejects([&] { decrypt_content(altered, key, "items"); }, "Tampered content accepted");
        rejects([&] { decrypt_content(a, key, "mobs"); }, "Category swap accepted");
        protect_key(path / "content.key.dpapi", key);
        require(unprotect_key(path / "content.key.dpapi") == key, "DPAPI roundtrip");
        {
            const auto root=path/"content-fixture";
            protect_key(root/"Config/Keys/content.key.dpapi",key);
            Bytes json{'{','"','a','"',':','1','}'};
            const auto approved=encrypt_content(json,key,"fixture");
            const Json catalog={{"resources",{{"fixture",{{"file","Content.dat"},{"sha256",hex(sha256(approved))}}}}}};
            const auto manifest=catalog.dump();
            write_file(root/"Data/catalog.dat",encrypt_content(Bytes(manifest.begin(),manifest.end()),key,"catalog"));
            write_file(root/"Data/Content.dat",approved);
            require(Content(root).get("fixture").at("a")==1,"Approved content was rejected");
            json[5]='2';
            write_file(root/"Data/Content.dat",encrypt_content(json,key,"fixture"));
            rejects([&]{Content(root).get("fixture");},"Unapproved digest was accepted after reread");
        }
        Bytes salt(32);
        for (int i = 0; i < 32; i++)
            salt[i] = static_cast<unsigned char>(i);
        require(hex(pbkdf2("native-test", salt, 600000)) ==
                    "ac93f51627d46f0e9468867f26b7a68b63f69c2a43a818b64700cc1f874c44c4",
                "Python PBKDF2 compatibility");
        {
            AccountStore accounts(path / "login.sqlite");
            auto identity = accounts.register_account("CppOwner", "test-password-42");
            require(accounts.password("cppowner", "test-password-42", "test").id == identity.id,
                    "Password identity");
            rejects([&] { accounts.password("CppOwner", "wrong-password", "test"); },
                    "Bad password accepted");
            rejects([&] { accounts.register_account("CPPOWNER", "different-password"); },
                    "Duplicate casefold account");
            std::string session = token(), ticket = token();
            accounts.db().exec("INSERT INTO login_sessions VALUES(?,?,?)",
                               {sha256(session), identity.id, now() + 100});
            accounts.db().exec("INSERT INTO game_tickets(token_hash,account_id,expires_at,used,session_hash) "
                               "VALUES(?,?,?,0,?)",
                               {sha256(ticket), identity.id, now() + 60, sha256(session)});
            require(accounts.consume_ticket("CppOwner", ticket).id == identity.id, "Ticket identity");
            rejects([&] { accounts.consume_ticket("CppOwner", ticket); }, "Ticket replay accepted");
        }
        {
            LegacyStore world(path / "world.sqlite");
            Json vitals = {{"600", {{"1", {{"maxhp", 186}, {"maxmp", 100}}}}}};
            auto first = world.create_character("one", "Mage", 600, "01007f4050ff0300", vitals);
            auto second = world.create_character("two", "Other", 600, "0200010203040400", vitals);
            require(first["eid"] != second["eid"], "Cross-account entity collision");
            require(world.characters("one").size() == 1, "Account character isolation");
            require(first["appearance"] == "01007f4050ff0300", "Creation appearance changed");
            rejects([&] { world.update_character("two", first["eid"], [](Json &ch) { ch["level"] = 120; }); },
                    "Cross-account mutation");
            world.update_character("one", first["eid"], [](Json &ch) {
                ch["fletta"] = 12345;
                ch["pos"] = {1.2, 3.4, 5.6};
                ch["unknown_future_extension"] = {{"persist", true}};
            });
            auto read = world.character("one", first["eid"]);
            require(read["fletta"] == 12345 && read["unknown_future_extension"]["persist"] == true,
                    "Lossless legacy payload");
            snapshot_database(path / "world.sqlite", path / "copy.sqlite");
            LegacyStore copy(path / "copy.sqlite");
            require(copy.character("one", first["eid"])["fletta"] == 12345, "WAL snapshot lost data");
            require(world.delete_character("one", first["eid"]), "Delete failed");
            auto third = world.create_character("one", "After", 600, "", vitals);
            require(third["eid"].get<int>() > second["eid"].get<int>(), "Deleted entity ID reused");
        }
        {
            Sqlite db(path / "image.sqlite");
            db.exec("CREATE TABLE probe(value TEXT)");
            db.exec("INSERT INTO probe VALUES('memory only')");
            auto image = serialize_database(path / "image.sqlite");
            Sqlite memory(image);
            require(memory.query("SELECT value FROM probe")[0]["value"] == "memory only",
                    "Encrypted SQLite memory reader");
            rejects([&] { memory.exec("INSERT INTO probe VALUES('forbidden')"); },
                    "Readonly image accepted write");
        }
        {
            Sqlite db(path / "nested.sqlite");
            db.exec("CREATE TABLE probe(value INTEGER)");
            {
                Transaction outer(db);
                db.exec("INSERT INTO probe VALUES(1)");
                {
                    Transaction inner(db);
                    db.exec("INSERT INTO probe VALUES(2)");
                    inner.commit();
                }
            }
            require(db.query("SELECT * FROM probe").empty(),
                    "Committed nested equipment mutation escaped its rolled-back outer transaction");
            {
                Transaction outer(db);
                db.exec("INSERT INTO probe VALUES(1)");
                {
                    Transaction inner(db);
                    db.exec("INSERT INTO probe VALUES(2)");
                }
                outer.commit();
            }
            require(db.query("SELECT * FROM probe").size() == 1,
                    "Nested rollback corrupted its surrounding transaction");
        }
        auto frame = make_frame(4, 2, {1, 2, 3}, 257);
        Bytes buffer(frame.begin(), frame.begin() + 5);
        require(extract_frames(buffer).empty(), "Fragmentation parser");
        buffer.insert(buffer.end(), frame.begin() + 5, frame.end());
        auto frames = extract_frames(buffer);
        require(frames.size() == 1 && buffer.empty() && get16(frames[0], 4, true) == 257, "Native framing");
        Bytes invalid{0, 0, 0, 2};
        rejects([&] { extract_frames(invalid); }, "Invalid native length accepted");
        require(get16(notice("Senha errada"), 0, true) == 12, "Native login notice");
        {
            SessionStore sessions(path / "sessions.sqlite");
            Session session;
            session.identity = {"one", "CppOwner", 1001};
            session.id = token();
            session.fingerprint = random_bytes(32);
            session.method = "launcher";
            session.expires = now() + 100;
            sessions.save(session);
            require(sessions.load(session.id).identity.id == "one", "Cross-process session payload");
            sessions.consume(session.id);
            rejects([&] { sessions.load(session.id); }, "Consumed handoff reused");
        }
        std::cout << "PASS: authenticated content/DPAPI; Python PBKDF2; credentials/tickets; isolated "
                     "accounts; WAL snapshots; appearance/persistence; native framing; single-use handoffs\n";
        fs::remove_all(path);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\nEvidence retained at " << path.string() << "\n";
        return 1;
    }
}
