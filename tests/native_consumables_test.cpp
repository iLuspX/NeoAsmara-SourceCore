#include "native/consumables.hpp"
#include <iostream>

using namespace rosh;
static void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "Expected an isolated consumables fixture");
        const fs::path root = argv[1];
        check(fs::is_regular_file(root / "GM_TEST_FIXTURE"), "Refusing active database");
        Content content(root);
        Sqlite db(root / "Database/consumables-test.sqlite");
        Inventory inventory(db, content);
        Consumables consumables(db, inventory, content);
        constexpr std::uint32_t player = 76001;
        inventory.grant_once(player, {{54001, 3}, {54216, 1}, {113001, 2}, {140004, 1}, {140002, 1}, {400140002, 1}}, "consumables-fixture");
        auto slot = [&](std::uint32_t id) {
            for (const auto& item : inventory.items(player))
                if (item.item == id)
                    return item.slot;
            throw std::runtime_error("Fixture potion missing");
        };
        const int healing = slot(54001);
        const auto stamp = now();
        Json character = {{"eid", player}, {"hp", 100}, {"maxhp", 1000}, {"mp", 100}, {"maxmp", 500}};
        check(consumables.use(character, healing, 54001, "hp-potion-1", stamp), "HP potion failed");
        check(character.at("hp") == 340, "HP potion did not restore its declared 240 HP");
        check(!consumables.use(character, healing, 54001, "hp-potion-1", stamp), "Replay consumed a second potion");
        check(inventory.owned(player, healing, 54001, stamp).quantity == 2, "Replay changed inventory");
        bool rejected = false;
        try { consumables.use(character, healing, 54001, "hp-potion-2", stamp); }
        catch (const std::exception&) { rejected = true; }
        check(rejected && inventory.owned(player, healing, 54001, stamp).quantity == 2,
              "Cooldown bypass consumed another potion");

        db.exec("CREATE TRIGGER fail_consumable_receipt BEFORE INSERT ON native_consumable_uses "
                "BEGIN SELECT RAISE(ABORT, 'fixture rollback'); END");
        rejected = false;
        try { consumables.use(character, healing, 54001, "hp-potion-rollback", stamp + 2); }
        catch (const std::exception&) { rejected = true; }
        check(rejected && character.at("hp") == 340 &&
              inventory.owned(player, healing, 54001, stamp).quantity == 2, "Failed receipt left partial use");
        db.exec("DROP TRIGGER fail_consumable_receipt");

        check(consumables.use(character, slot(54216), 54216, "regeneration-potion", stamp), "Regen potion failed");
        check(!Consumables::tick(character, stamp + 4), "Regen triggered before its five-second period");
        check(Consumables::tick(character, stamp + 5) && character.at("hp") == 360, "Regen did not restore 2% HP");
        check(consumables.use(character, slot(113001), 113001, "xp-potion", stamp), "XP potion failed");
        check(Consumables::bonus(character, "xp", stamp + 1) == 30 &&
              Consumables::bonus(character, "fletta", stamp + 1) == 0, "Wrong progression rate or resource");
        auto &effects=character.at("consumable_effects");
        effects["progression:xp:86400"]=effects.at("progression:item:113001");
        effects.erase("progression:item:113001");
        rejected=false;
        try{consumables.use(character,slot(113001),113001,"legacy-same-potion",stamp+10);}
        catch(const std::exception &){rejected=true;}
        check(rejected&&inventory.owned(player,slot(113001),113001,stamp+10).quantity==1,
              "Old saved effect allowed the same potion to be reapplied");
        check(consumables.use(character,slot(140004),140004,"xp-potion-other-id",stamp),"Distinct XP potion was blocked");
        check(consumables.use(character,slot(140002),140002,"fletta-potion",stamp),"Fletta potion was blocked");
        check(consumables.use(character,slot(400140002),400140002,"fletta-potion-other-id",stamp),"Distinct Fletta potion was blocked");
        auto saved=Json::parse(character.dump());
        saved["hp"]=0;
        Consumables::tick(saved,stamp+6);
        check(Consumables::bonus(saved,"xp",stamp+6)==40&&Consumables::bonus(saved,"fletta",stamp+6)==80,
              "Reload/death did not preserve distinct potion effects");
        check(Consumables::bonus(saved,"fletta",stamp+10800)==0&&Consumables::bonus(saved,"xp",stamp+10800)==40,
              "Potion deadlines changed or crossed resources");
        check(Consumables::tick(character, stamp + 86400) &&
              Consumables::bonus(character, "xp", stamp + 86400) == 0, "Expired XP potion retained its bonus");
        constexpr std::uint32_t arena_potion = 999010055;
        const auto arena_time = stamp + 86402;
        inventory.grant_once(player, {{arena_potion, 2}}, "arena-potions");
        const auto arena_slot = slot(arena_potion);
        const auto before = character;
        for (const auto outside : {0u, 19003u, 9000000u}) {
            rejected = false;
            try { consumables.use(character, arena_slot, arena_potion, "arena-rejected", arena_time, 0, outside); }
            catch (const std::exception& error) { rejected = std::string(error.what()).find("Arena") != std::string::npos; }
            check(rejected && character == before &&
                  inventory.owned(player, arena_slot, arena_potion, arena_time).quantity == 2,
                  "Arena potion used outside Arena or changed state during rejection");
        }
        check(db.query("SELECT 1 FROM native_consumable_uses WHERE event_key='arena-rejected'").empty(),
              "Rejected arena use created a receipt");
        check(consumables.use(character, arena_slot, arena_potion, "arena-first", arena_time, 0, 1900329),
              "Potion rejected in the first Arena");
        check(character.at("hp").get<int>() == before.at("hp").get<int>() + 200,
              "Arena potion used wrong amount");
        check(consumables.use(character, arena_slot, arena_potion, "arena-second", arena_time + 2, 0, 1900349),
              "Potion rejected in the second Arena");
        constexpr std::uint32_t rapid_recovery = 999900003;
        const auto recovery_time = arena_time + 10;
        inventory.grant_once(player, {{rapid_recovery, 2}}, "rapid-recovery");
        const auto recovery_slot = slot(rapid_recovery);
        character["hp"] = 100;
        character["mp"] = 100;
        check(consumables.use(character, recovery_slot, rapid_recovery, "rapid-use", recovery_time),
              "Quick Recovery failed");
        check(!Consumables::tick(character, recovery_time + .9) && character.at("hp")==100 &&
                  character.at("mp")==100, "Quick Recovery fired before one second");
        check(Consumables::tick(character, recovery_time + 1) && character.at("hp")==150 &&
                  character.at("mp")==125, "Quick Recovery did not restore 5% of both resources");
        check(!Consumables::tick(character, recovery_time + 1), "Duplicate tick restored resources twice");
        rejected = false;
        try { consumables.use(character, recovery_slot, rapid_recovery, "rapid-use-duplicate", recovery_time+2); }
        catch (const std::exception&) { rejected = true; }
        check(rejected && inventory.owned(player, recovery_slot, rapid_recovery, recovery_time+2).quantity==1,
              "A second Quick Recovery was consumed while the effect was active");
        check(Consumables::tick(character, recovery_time + 298.25) && character.at("hp")==200 &&
                  character.at("mp")==150, "Quick Recovery caught up missed pulses");
        character["hp"]=990;
        character["mp"]=490;
        check(Consumables::tick(character, recovery_time + 299) && character.at("hp")==1000 &&
                  character.at("mp")==500, "Quick Recovery did not cap both resources");
        character["hp"]=900;
        character["mp"]=400;
        check(Consumables::tick(character, recovery_time + 300) && character.at("hp")==900 &&
                  character.at("mp")==400 && !character.at("consumable_effects").contains("regen:quick-recovery"),
              "Quick Recovery continued past five minutes");
        std::cout << "PASS: documented HP restoration, cooldown, replay, transactional rollback, regen and XP expiry.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
