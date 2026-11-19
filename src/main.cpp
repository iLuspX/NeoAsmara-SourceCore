#include "database.hpp"
#include "gm.hpp"
#include "gameplay.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void usage() {
    std::cout << R"TXT(Karos C++ Core - offline database/GM utility
Usage:
  karos_admin --db <database.sqlite> init
  karos_admin --db <database.sqlite> content import <content_typed.sqlite>
  karos_admin --db <database.sqlite> account ensure <account_id>
  karos_admin --db <database.sqlite> gm set <account_id> <player|gm|admin>
  karos_admin --db <database.sqlite> gm list
  karos_admin --db <database.sqlite> character list <account_id>
  karos_admin --db <database.sqlite> character create <account_id> <class_id> <name>
  karos_admin --db <database.sqlite> character delete <account_id> <eid>
  karos_admin --db <database.sqlite> gm run <account_id> <quoted-command>
  karos_admin --db <database.sqlite> audit [count]
  karos_admin --db <database.sqlite> world nearby <character_eid> [radius]
  karos_admin --db <database.sqlite> world attack <character_eid> <spawn_id>
  karos_admin --db <database.sqlite> world pickup <character_eid> <ground_item_id>
  karos_admin --db <database.sqlite> npc nearby <character_eid> [radius]
  karos_admin --db <database.sqlite> shop list <character_eid> <npc_id>
  karos_admin --db <database.sqlite> shop buy <character_eid> <npc_id> <item_id> <quantity>
  karos_admin --db <database.sqlite> shop sell <character_eid> <npc_id> <item_id> <quantity>
  karos_admin --db <database.sqlite> portal nearby <character_eid> [radius]
  karos_admin --db <database.sqlite> portal travel <character_eid> <portal_id>

Accepted class IDs: 100, 200, 300, 400, 500, 600, 800.
)TXT";
}
std::string join(int argc, char** argv, int start) {
    std::string s;
    for (int i=start;i<argc;++i) { if(i>start) s+=' '; s+=argv[i]; }
    return s;
}
std::int64_t to_i64(const char* s) {
    std::size_t used=0; long long v=std::stoll(s,&used,10);
    if (used!=std::string(s).size()) throw std::invalid_argument("expected integer");
    return static_cast<std::int64_t>(v);
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 4 || std::string(argv[1]) != "--db") { usage(); return 2; }
        const std::string db_path=argv[2];
        karos::Database db(db_path); db.migrate();
        const std::string group=argv[3];
        if(group=="init") {
            std::cout << "Schema migrated without deleting characters. DB: " << db.path() << "\n"; return 0;
        }
        if(group=="content" && argc==6 && std::string(argv[4])=="import") {
            const auto count=db.import_reference_content(argv[5]);
            std::cout << "Reference tables imported: " << count << "\n";
            std::cout << "Imported data is stored in reference_* tables, separate from active game tables.\n";
            return 0;
        }
        if(group=="account" && argc>=5 && std::string(argv[4])=="ensure" && argc==6) {
            db.ensure_account(argv[5]); std::cout << "Account ready: " << argv[5] << "\n"; return 0;
        }
        if(group=="gm" && argc>=5 && std::string(argv[4])=="set" && argc==7) {
            db.set_gm_role(argv[5],argv[6],"console");
            std::cout << "Role updated: " << argv[5] << " -> " << (std::string(argv[6])=="owner" ? "admin" : argv[6]) << "\n"; return 0;
        }
        if(group=="gm" && argc==5 && std::string(argv[4])=="list") {
            auto rows=db.gm_accounts();
            if(rows.empty()) std::cout << "No GM accounts configured.\n";
            for(const auto& row:rows) std::cout << row.first << "\t" << (row.second=="owner" ? "admin" : row.second) << "\n";
            return 0;
        }
        if(group=="character" && argc==6 && std::string(argv[4])=="list") {
            auto cs=db.characters_for_account(argv[5]);
            if(cs.empty()) std::cout << "No characters for account " << argv[5] << ".\n";
            for(const auto& c:cs) std::cout << "eid="<<c.eid<<" slot="<<c.slot<<" name="<<c.name<<" class="<<c.cls<<" level="<<c.level<<" exp="<<c.exp<<" gold="<<c.gold<<" map="<<c.map_name<<"\n";
            return 0;
        }
        if(group=="character" && argc>=8 && std::string(argv[4])=="create") {
            int cls=static_cast<int>(to_i64(argv[6])); std::string name=join(argc,argv,7);
            auto eid=db.create_character(argv[5],name,cls);
            std::cout << "Character created: eid="<<eid<<" account="<<argv[5]<<" class="<<cls<<" name="<<name<<"\n"; return 0;
        }
        if(group=="character" && argc==7 && std::string(argv[4])=="delete") {
            bool deleted=db.delete_character(argv[5],to_i64(argv[6]));
            std::cout << (deleted?"Character deleted.\n":"Character not found in this account.\n"); return deleted?0:1;
        }
        if(group=="gm" && argc>=7 && std::string(argv[4])=="run") {
            karos::GMService gm(db); auto result=gm.execute(argv[5],join(argc,argv,6));
            std::cout << (result.ok?"OK: ":"ERROR: ") << result.message << "\n";
            return result.ok?0:1;
        }
        if(group=="world" && argc>=6) {
            karos::Gameplay game(db);
            const std::string op=argv[4];
            if(op=="nearby" && (argc==6 || argc==7)) {
                const auto eid=to_i64(argv[5]);
                const double radius=argc==7?std::stod(argv[6]):150.0;
                const auto mobs=game.nearby(eid,radius,15);
                if(mobs.empty()) std::cout<<"No nearby spawns (try another map location).\n";
                for(const auto& m:mobs) {
                    std::cout << "spawn="<<m.spawn_id<<" mob="<<m.name<<" lvl="<<m.level
                              <<" HP="<<m.remaining_hp<<"/"<<m.max_hp
                              <<" distance="<<m.distance<<" respawn_in="<<m.seconds_to_respawn<<"s\n";
                }
                return 0;
            }
            if(op=="attack" && argc==7) {
                auto r=game.attack(to_i64(argv[5]),to_i64(argv[6]));
                std::cout<<"Attack on "<<r.monster_name<<" damage="<<r.damage<<" remaining_HP="<<r.remaining_hp;
                if(r.killed) {
                    std::cout<<" | MOB DEFEATED XP +"<<r.gained_exp<<" level="<<r.level_after;
                    if(r.ground_item_ids.empty()) std::cout<<" | no drop configured";
                    for(auto id:r.ground_item_ids) std::cout<<" | ground_item="<<id;
                }
                std::cout<<"\n";
                return 0;
            }
            if(op=="pickup" && argc==7) {
                game.pickup(to_i64(argv[5]),to_i64(argv[6]));
                std::cout<<"Item picked up and saved to inventory.\n";return 0;
            }
        }
        if(group=="npc" && argc>=6 && std::string(argv[4])=="nearby" && argc<=7) {
            karos::Gameplay game(db);
            double radius=argc==7?std::stod(argv[6]):100.;
            for(const auto& n:game.nearby_npcs(to_i64(argv[5]),radius))
                std::cout<<"npc="<<n.npc_id<<" name="<<n.name<<" type="<<n.type<<" distance="<<n.distance<<"\n";
            return 0;
        }
        if(group=="shop" && argc>=7) {
            karos::Gameplay game(db);
            std::string op=argv[4];
            if(op=="list" && argc==7) {
                for(const auto& x:game.shop(to_i64(argv[5]),to_i64(argv[6])))
                    std::cout<<"item="<<x.item_id<<" name="<<x.name<<" preco="<<x.price<<" estoque="<<x.stock<<"\n";
                return 0;
            }
            if((op=="buy" || op=="sell") && argc==9) {
                auto eid=to_i64(argv[5]);auto npc=to_i64(argv[6]);
                int item=static_cast<int>(to_i64(argv[7]));int qty=static_cast<int>(to_i64(argv[8]));
                if(op=="buy") game.buy(eid,npc,item,qty);
                else game.sell(eid,npc,item,qty);
                std::cout<<"Operacao confirmada e persistida no SQLite.\n";return 0;
            }
        }
        if(group=="portal" && argc>=6) {
            karos::Gameplay game(db);
            const std::string op=argv[4];
            if(op=="nearby" && argc<=7) {
                double radius=argc==7?std::stod(argv[6]):100.;
                for(const auto& p:game.nearby_portals(to_i64(argv[5]),radius))
                    std::cout<<"portal="<<p.portal_id<<" destino="<<p.target_map<<" nivel_min="<<p.min_level<<" distance="<<p.distance<<"\n";
                return 0;
            }
            if(op=="travel" && argc==7) {
                game.travel(to_i64(argv[5]),to_i64(argv[6]));
                std::cout<<"Teleporte realizado e posicao salva.\n";return 0;
            }
        }
        if(group=="audit" && argc<=5) {
            int n=argc==5?static_cast<int>(to_i64(argv[4])):25;
            for(const auto& row:db.audit_tail(n)) std::cout<<row<<"\n";
            return 0;
        }
        usage(); return 2;
    } catch(const std::exception& e) {
        std::cerr << "Erro: " << e.what() << "\n"; return 1;
    }
}
