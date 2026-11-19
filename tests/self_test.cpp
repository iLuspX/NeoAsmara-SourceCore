#include "database.hpp"
#include "gm.hpp"
#include "gameplay.hpp"
#include <filesystem>
#include <iostream>
#include <sqlite3.h> // Cabecalho distribuido em include/, API publica do SQLite
#include <stdexcept>

#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x); } while(0)


static void test_reference_import() {
    auto target=std::filesystem::temp_directory_path()/"karos_cpp_reference_target.sqlite";
    auto source=std::filesystem::temp_directory_path()/"karos_cpp_reference_source.sqlite";
    std::error_code ec;
    for(const auto& p : {target,source}) {
        std::filesystem::remove(p,ec); std::filesystem::remove(p.string()+"-wal",ec); std::filesystem::remove(p.string()+"-shm",ec);
    }
    sqlite3* raw=nullptr;
    if(sqlite3_open(source.string().c_str(),&raw)!=SQLITE_OK) throw std::runtime_error("reference source DB open failed");
    const char* sql="CREATE TABLE itemBase(rec INTEGER, name TEXT); INSERT INTO itemBase VALUES(1002,'Test Potion'); CREATE TABLE skillData(rec INTEGER, skill_name TEXT); INSERT INTO skillData VALUES(77,'Test Skill');";
    char* err=nullptr; int rc=sqlite3_exec(raw,sql,nullptr,nullptr,&err);
    if(rc!=SQLITE_OK){std::string m=err?err:"reference SQL failed";sqlite3_free(err);sqlite3_close(raw);throw std::runtime_error(m);}
    sqlite3_close(raw);
    {
        karos::Database db(target.string()); db.migrate();
        CHECK(db.import_reference_content(source.string())==2);
        sqlite3_stmt* q=nullptr;
        CHECK(sqlite3_prepare_v2(db.native_handle(),"SELECT name FROM reference_itemBase WHERE rec=1002",-1,&q,nullptr)==SQLITE_OK);
        CHECK(sqlite3_step(q)==SQLITE_ROW);
        CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(q,0)))=="Test Potion");
        sqlite3_finalize(q);
        CHECK(db.import_reference_content(source.string())==2); // replace reference tables idempotently
    }
    for(const auto& p : {target,source}) {
        std::filesystem::remove(p,ec); std::filesystem::remove(p.string()+"-wal",ec); std::filesystem::remove(p.string()+"-shm",ec);
    }
}

static void test_legacy_migration() {
    auto path=std::filesystem::temp_directory_path()/"karos_cpp_legacy_selftest.sqlite";
    std::error_code ec; std::filesystem::remove(path,ec); std::filesystem::remove(path.string()+"-wal",ec); std::filesystem::remove(path.string()+"-shm",ec);
    sqlite3* raw=nullptr;
    if(sqlite3_open(path.string().c_str(),&raw)!=SQLITE_OK) throw std::runtime_error("legacy db open failed");
    const char* sql=R"SQL(
      CREATE TABLE accounts(account_id TEXT PRIMARY KEY, created_at TEXT DEFAULT CURRENT_TIMESTAMP);
      INSERT INTO accounts(account_id) VALUES('default');
      CREATE TABLE characters(eid INTEGER PRIMARY KEY,account_id TEXT NOT NULL,slot INTEGER NOT NULL,name TEXT NOT NULL,cls INTEGER NOT NULL DEFAULT 600,level INTEGER NOT NULL DEFAULT 1,exp INTEGER NOT NULL DEFAULT 0,hp INTEGER NOT NULL DEFAULT 186,mp INTEGER NOT NULL DEFAULT 100,map_name TEXT,x REAL,y REAL,z REAL,gold INTEGER NOT NULL DEFAULT 0,created_at TEXT DEFAULT CURRENT_TIMESTAMP,updated_at TEXT DEFAULT CURRENT_TIMESTAMP,UNIQUE(account_id,slot));
      INSERT INTO characters(eid,account_id,slot,name,cls,level,exp) VALUES(10389,'default',0,'LegacyMage',600,100,1283884);
    )SQL";
    char* err=nullptr; int rc=sqlite3_exec(raw,sql,nullptr,nullptr,&err);
    if(rc!=SQLITE_OK){std::string m=err?err:"legacy sql failure";sqlite3_free(err);sqlite3_close(raw);throw std::runtime_error(m);}
    sqlite3_close(raw);
    {
      karos::Database db(path.string()); db.migrate();
      auto list=db.characters_for_account("default");
      CHECK(list.size()==1); CHECK(list[0].eid==10389); CHECK(list[0].name=="LegacyMage");
      CHECK(list[0].cls==600); CHECK(list[0].level==100); CHECK(list[0].exp==1283884);
    }
    std::filesystem::remove(path,ec); std::filesystem::remove(path.string()+"-wal",ec); std::filesystem::remove(path.string()+"-shm",ec);
}

static void test_gameplay_transactions() {
    auto path=std::filesystem::temp_directory_path()/"karos_cpp_gameplay_selftest.sqlite";
    std::error_code ec;
    for(const auto& suffix : {"","-wal","-shm"}) std::filesystem::remove(path.string()+suffix,ec);
    {
        karos::Database db(path.string()); db.migrate();
        db.ensure_account("alpha"); db.ensure_account("beta");
        auto hero=db.create_character("alpha","HeroTest",600);
        auto stranger=db.create_character("beta","StrangerTest",500);
        const char* fixtures=R"SQL(
          INSERT INTO monster_definitions(npc_id,name,level,max_hp,defense,exp_reward)
          VALUES(900101,'Test Mob',1,40,0,30);
          INSERT INTO spawn_points(spawn_id,map_name,npc_id,x,y,z,respawn_seconds)
          VALUES(314159,'berneo',900101,9775.0,8124.8,672.7,10);
          INSERT INTO monster_drops(npc_id,item_id,chance_bp,min_qty,max_qty)
          VALUES(900101,1002,10000,2,2);
        )SQL";
        char* err=nullptr;
        if(sqlite3_exec(db.native_handle(),fixtures,nullptr,nullptr,&err)!=SQLITE_OK) {
            std::string msg=err?err:"fixture failed";sqlite3_free(err);throw std::runtime_error(msg);
        }
        karos::Gameplay gameplay(db);
        const auto near=gameplay.nearby(hero,30,5,1000);
        CHECK(near.size()==1);
        CHECK(near[0].remaining_hp==40);
        auto hit=gameplay.attack(hero,314159,1000);
        CHECK(!hit.killed && hit.damage>0);
        CHECK(db.get_character(hero)->exp==0);
        auto killed=gameplay.attack(hero,314159,1001);
        CHECK(killed.killed && killed.gained_exp==30);
        CHECK(killed.level_after==2);
        CHECK(killed.ground_item_ids.size()==1);
        CHECK(db.get_character(hero)->exp==30);
        const auto respawning=gameplay.nearby(hero,30,5,1002);
        CHECK(respawning.size()==1 && respawning[0].remaining_hp==0);
        bool dead_rejected=false;
        try{(void)gameplay.attack(hero,314159,1002);}catch(const std::runtime_error&){dead_rejected=true;}
        CHECK(dead_rejected && db.get_character(hero)->exp==30);
        bool stranger_rejected=false;
        try{gameplay.pickup(stranger,killed.ground_item_ids[0]);}catch(const std::runtime_error&){stranger_rejected=true;}
        CHECK(stranger_rejected);
        gameplay.pickup(hero,killed.ground_item_ids[0]);
        const auto inv=db.inventory(hero);
        CHECK(inv.size()==1 && inv[0].item_id==1002 && inv[0].quantity==2);
        bool second_pickup_rejected=false;
        try{gameplay.pickup(hero,killed.ground_item_ids[0]);}catch(const std::runtime_error&){second_pickup_rejected=true;}
        CHECK(second_pickup_rejected);
        CHECK(db.get_character(hero)->exp==30);
        const auto back=gameplay.nearby(hero,30,5,1011);
        CHECK(back.size()==1 && back[0].remaining_hp==40);
        auto resurrected=gameplay.attack(hero,314159,1011);
        CHECK(resurrected.damage>0);
        CHECK(db.get_character(hero)->exp==30); // no XP until next kill
        bool too_far_rejected=false;
        db.set_position("HeroTest","berneo",1,2,3);
        try{(void)gameplay.attack(hero,314159,1012);}catch(const std::runtime_error&){too_far_rejected=true;}
        CHECK(too_far_rejected);
    }
    for(const auto& suffix : {"","-wal","-shm"}) std::filesystem::remove(path.string()+suffix,ec);
}

static void test_npc_shop_portals() {
    auto path=std::filesystem::temp_directory_path()/"karos_cpp_shop_portals_test.sqlite";
    std::error_code ec;
    for(auto ext:{"","-wal","-shm"})std::filesystem::remove(path.string()+ext,ec);
    {
        karos::Database db(path.string());db.migrate();
        auto owner=db.create_character("shop-tester","StoreHero",100);
        auto other=db.create_character("stranger","StoreStranger",600);
        db.add_gold(owner,1000);
        const char* sql=R"SQL(
            INSERT INTO map_definitions(map_id,map_name,display_name) VALUES(999001,'lab_second','Second');
            INSERT INTO npc_definitions(npc_id,name,map_name,x,y,z,npc_type)
                 VALUES(9301234,'Merchant','berneo',9775.0,8124.8,672.7,'merchant');
            INSERT INTO shop_definitions(shop_id,npc_id,name) VALUES(1234,9301234,'Lab Store');
            INSERT INTO item_definitions(item_id,name,stack_limit,sell_price)
                 VALUES(70001,'Lab Potion',10,7);
            INSERT INTO shop_items(shop_id,item_id,price,stock) VALUES(1234,70001,40,3);
            INSERT INTO portals(portal_id,source_map,target_map,x,y,z,target_x,target_y,target_z,min_level)
                VALUES(77,'berneo','lab_second',9775.0,8124.8,672.7,50,51,52,2);
        )SQL";
        char* err=nullptr;
        if(sqlite3_exec(db.native_handle(),sql,nullptr,nullptr,&err)!=SQLITE_OK) {
            std::string m=err?err:"fixture error";sqlite3_free(err);throw std::runtime_error(m);
        }
        karos::Gameplay g(db);
        CHECK(g.nearby_npcs(owner,30).size()==1);
        CHECK(g.shop(owner,9301234).size()==1);
        CHECK(g.nearby_portals(owner,30).size()==1);
        bool denied=false;
        try{g.buy(owner,9301234,70001,4);}catch(const std::runtime_error&){denied=true;}
        CHECK(denied && db.get_character(owner)->gold==1000);
        g.buy(owner,9301234,70001,2);
        CHECK(db.get_character(owner)->gold==920);
        CHECK(g.shop(owner,9301234)[0].stock==1);
        CHECK(db.inventory(owner).size()==1 && db.inventory(owner)[0].quantity==2);
        denied=false;
        try{g.sell(other,9301234,70001,1);}catch(const std::runtime_error&){denied=true;}
        CHECK(denied);
        g.sell(owner,9301234,70001,1);
        CHECK(db.get_character(owner)->gold==927);
        CHECK(db.inventory(owner).size()==1 && db.inventory(owner)[0].quantity==1);
        g.sell(owner,9301234,70001,1); // remove entire slot without violating quantity>0 CHECK
        CHECK(db.inventory(owner).empty());
        CHECK(db.get_character(owner)->gold==934);
        denied=false;
        try{g.travel(owner,77);}catch(const std::runtime_error&){denied=true;}
        CHECK(denied && db.get_character(owner)->map_name=="berneo");
        db.set_level("StoreHero",2);
        g.travel(owner,77);
        CHECK(db.get_character(owner)->map_name=="lab_second");
        CHECK(db.get_character(owner)->x==50);
        denied=false;
        try{g.buy(owner,9301234,70001,1);}catch(const std::runtime_error&){denied=true;}
        CHECK(denied && db.get_character(owner)->gold==934);
        CHECK(db.inventory(other).empty());
    }
    for(auto ext:{"","-wal","-shm"})std::filesystem::remove(path.string()+ext,ec);
}

int main() {
    try {
        test_reference_import();
        test_gameplay_transactions();
        test_npc_shop_portals();
        test_legacy_migration();
        auto path=std::filesystem::temp_directory_path()/"karos_cpp_core_selftest.sqlite";
        std::error_code ec; std::filesystem::remove(path,ec); std::filesystem::remove(path.string()+"-wal",ec); std::filesystem::remove(path.string()+"-shm",ec);
        {
            karos::Database db(path.string()); db.migrate();
            db.ensure_account("alpha"); db.ensure_account("beta");
            auto mage=db.create_character("alpha","MageTest",600);
            auto archer=db.create_character("alpha","ArcherTest",500);
            auto fighter=db.create_character("beta","FighterTest",100);
            CHECK(db.get_character(mage).has_value());
            CHECK(db.get_character(mage)->cls==600);
            CHECK(db.get_character(archer)->cls==500);
            CHECK(db.characters_for_account("alpha").size()==2);
            CHECK(db.characters_for_account("beta").size()==1);
            bool rejected=false; try{(void)db.create_character("alpha","NoClass",999);}catch(const std::invalid_argument&){rejected=true;}
            CHECK(rejected);
            db.set_gm_role("alpha","owner","console");
            karos::GMService gm(db);
            auto denied=gm.execute("beta","//gm gold MageTest 25"); CHECK(!denied.ok);
            auto gold=gm.execute("alpha","//gm gold MageTest 25"); CHECK(gold.ok);
            CHECK(db.get_character(mage)->gold==25);
            auto xp=gm.execute("alpha","//gm xp MageTest 16"); CHECK(xp.ok);
            CHECK(db.get_character(mage)->level==2);
            CHECK(db.get_character(mage)->skill_points==1);
            auto item=gm.execute("alpha","//gm item MageTest 1002 120"); CHECK(item.ok);
            auto items=db.inventory(mage); CHECK(items.size()==2); CHECK(items[0].quantity==99); CHECK(items[1].quantity==21);
            auto tp=gm.execute("alpha","//gm tp MageTest berneo 1 2 3"); CHECK(tp.ok);
            CHECK(db.get_character(mage)->x==1.0 && db.get_character(mage)->z==3.0);
            auto spawn=gm.execute("alpha","//gm spawn 9001 berneo 1 2 3 2"); CHECK(spawn.ok);
            auto announce=gm.execute("alpha","//gm announce Karos core test"); CHECK(announce.ok);
            auto role=gm.execute("alpha","//gm setgm beta gm"); CHECK(role.ok); CHECK(db.gm_role("beta")=="gm");
            db.set_gm_role("alpha","admin","console"); // new public spelling; stored as legacy owner
            CHECK(db.gm_role("alpha")=="owner");
            auto role_info=gm.execute("alpha","//gm whoami");
            CHECK(role_info.ok && role_info.message.find("Admin")!=std::string::npos);
            auto promoted=gm.execute("alpha","//gm setgm beta admin");
            CHECK(promoted.ok && db.gm_role("beta")=="owner");
            auto help=gm.execute("alpha","//gm help");
            CHECK(help.ok && help.message.find("<player|gm|admin>")!=std::string::npos);
            CHECK(db.audit_tail(50).size()>=8);
            CHECK(db.delete_character("alpha",archer));
            CHECK(db.characters_for_account("alpha").size()==1);
        }
        std::filesystem::remove(path,ec); std::filesystem::remove(path.string()+"-wal",ec); std::filesystem::remove(path.string()+"-shm",ec);
        std::cout << "PASS: legacy migration preserves character ID/class/level/XP; reference content import is idempotent; account isolation, class validation, character slots, XP/level-up, inventory stacks, GM permission checks, GM commands and audit log; offline combat, kill-to-XP atomicity, respawn, configured drop, pickup and ownership checks.\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << "FAIL: " << e.what() << "\n"; return 1; }
}
