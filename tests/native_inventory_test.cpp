#include "native/inventory.hpp"
#include "native/economy.hpp"
#include <iostream>
using namespace rosh;
static void require(bool ok, const char *text) {
    if (!ok)
        throw std::runtime_error(text);
}
template <class Action> static void reject(Action action, const char *text) {
    bool failed = false;
    try {
        action();
    } catch (const std::exception &) {
        failed = true;
    }
    require(failed, text);
}
int wmain(int argc, wchar_t **argv) {
    auto fixture = fs::temp_directory_path() / wide("rosh_inventory_" + token(8));
    try {
        require(argc == 2, "Usage: inventory tests <encrypted root>");
        fs::create_directories(fixture);
        Content content(argv[1]);
        {
            Sqlite db(fixture / "gameplay.sqlite");
            Inventory inventory(db, content);
            constexpr std::uint32_t eid = 0x2892;
            auto timestamp = now();
            require(inventory.available(eid, timestamp).size() == 20,
                    "New character received unearned extra bags");
            require(inventory.grant_once(eid, {{54005, 150}}, "potion-grant"), "Item grant failed");
            require(!inventory.grant_once(eid, {{54005, 150}}, "potion-grant"), "Duplicate grant paid twice");
            reject([&] { inventory.grant_once(eid, {{54005, 151}}, "potion-grant"); },
                   "Grant replay payload changed");
            reject([&] { inventory.owned(eid + 1, 0, 54005, timestamp); },
                   "Foreign inventory owned by another character");
            auto potions = inventory.items(eid);
            require(potions.size() == 2 && potions[0].quantity == 100 && potions[1].quantity == 50 &&
                        potions[0].uid != potions[1].uid,
                    "Native potion stack size or identity lost");
            inventory.grant_once(eid, {{69009, 3}, {16001, 1}}, "bags-and-staff");
            for (int target = 13; target <= 15; target++) {
                auto rows = inventory.items(eid);
                auto bag = std::find_if(rows.begin(), rows.end(),
                                        [](const auto &item) { return item.item == 69009; });
                require(bag != rows.end(), "44 slot bag missing");
                require(inventory.equip(eid, bag->slot, 69009, 600, 1, target, timestamp) == target,
                        "Multiple bag slots not independent");
            }
            require(inventory.available(eid, timestamp).size() == 20 + 44 * 3,
                    "Three 44 slot bags did not expand inventory");
            auto rows = inventory.items(eid);
            auto weapon =
                std::find_if(rows.begin(), rows.end(), [](const auto &item) { return item.item == 16001; });
            require(weapon != rows.end(), "Staff missing");
            auto weapon_uid = weapon->uid;
            reject([&] { inventory.equip(eid, weapon->slot, 16001, 100, 120, 7, timestamp); },
                   "Wrong class equipped the mage weapon");
            require(inventory.equip(eid, weapon->slot, 16001, 600, 1, 7, timestamp) == 7,
                    "Weapon equipment failed");
            auto equipped = inventory.items(eid, true);
            auto staff = std::find_if(equipped.begin(), equipped.end(),
                                      [](const auto &item) { return item.slot == 7; });
            require(staff != equipped.end() && staff->uid == weapon_uid,
                    "Equipment identity changed on wear");
            db.exec("INSERT INTO item_enchantments VALUES(?,18)", {std::int64_t(weapon_uid)});
            staff->grade = 18;
            auto record = inventory.update(eid, *staff, true);
            require(record[5] == 0 && record[6] == 7 && record[11] == 7 && record[12] == 18 &&
                        get32(record, 13) == weapon_uid && get32(record, 61) == 4102444800u,
                    "Native equipped cache, grade or permanent lifetime incorrect");
            auto destination = inventory.unequip(eid, 7, 16001, std::nullopt, timestamp);
            require(inventory.owned(eid, destination, 16001, timestamp).uid == weapon_uid,
                    "Unequip lost instance UID");
            Wallet wallet(db, inventory);
            constexpr std::uint32_t rich = eid + 1;
            require(wallet.apply(rich, 2500000000000LL, "carat-credit").second, "Wallet credit failed");
            require(!wallet.apply(rich, 2500000000000LL, "carat-credit").second &&
                        wallet.balance(rich) == 2500000000000LL,
                    "Wallet replay paid twice");
            reject([&] { wallet.apply(rich, 1, "carat-credit"); }, "Wallet replay changed amount");
            auto money = wallet.updates(rich);
            require(money.size() == 3 &&
                        get32(money[0], 21) == static_cast<std::uint32_t>(1000000000000ULL) &&
                        get32(money[0], 25) == static_cast<std::uint32_t>(1000000000000ULL >> 32),
                    "64 bit Carat piles were truncated");
            wallet.apply(rich, -2500000000000LL, "carat-debit");
            require(wallet.balance(rich) == 0 && wallet.updates(rich).size() == 1 &&
                        get32(wallet.updates(rich)[0], 21) == 0,
                    "Zero wallet displayed a placeholder as one Carat");
            reject([&] { wallet.apply(rich, -1, "overdraft"); }, "Wallet overdraft allowed");
            require(wallet.balance(rich) == 0, "Failed wallet transaction changed balance");
            inventory.grant_once(eid, {{16001, 90}}, "fill-bags");
            reject([&] { inventory.unequip(eid, 13, 69009, std::nullopt, timestamp); },
                   "Nonempty bag was removed");
            auto before = inventory.items(eid).size();
            reject([&] { inventory.grant_once(eid, {{16001, 100}}, "overflow-grant"); },
                   "Overfull inventory accepted");
            require(inventory.items(eid).size() == before &&
                        db.query("SELECT 1 FROM inventory_grants WHERE event_key='overflow-grant'").empty(),
                    "Inventory overflow did not roll back the whole grant");
            auto snapshots = inventory.snapshots(eid, timestamp);
            std::size_t count = 0;
            for (const auto &packet : snapshots) {
                require(packet.size() == 6 + packet[5] * 147 && get32(packet, 0, true) == eid,
                        "Native inventory snapshot framing invalid");
                count += packet[5];
            }
            require(count == inventory.items(eid).size() + inventory.items(eid, true).size(),
                    "Snapshot omitted items or Cash equipment");
            require(inventory.name(16001) != "16001" && inventory.name(0xffffffff) == "Item sem nome",
                    "Item chat names expose raw IDs");
            constexpr auto rental_eid=eid+20;
            inventory.grant_timed_once(rental_eid,69009,1,60,"expiring-bag");
            auto rental_bag=inventory.items(rental_eid).at(0);
            inventory.equip(rental_eid,rental_bag.slot,rental_bag.item,600,120,13,now());
            inventory.grant_once(rental_eid,{{54005,20}},"hidden-potions");
            auto hidden=inventory.items(rental_eid).at(0);
            db.exec("UPDATE inventory_slots SET slot=256 WHERE instance_uid=?",{std::int64_t(hidden.uid)});
            db.exec("UPDATE reward_instances SET expires_at=? WHERE uid=?",{std::int64_t(now()-1),std::int64_t(rental_bag.uid)});
            reject([&]{inventory.owned(rental_eid,256,54005,now());},"Expired bag contents remained usable");
            inventory.grant_once(rental_eid,{{54005,5}},"fresh-potions-after-bag-expiry");
            auto preserved=db.query("SELECT quantity FROM inventory_slots WHERE instance_uid=?",{std::int64_t(hidden.uid)});
            require(preserved.size()==1&&preserved[0]["quantity"]==20,"Expired bag contents were lost or stacked invisibly");
            require(inventory.items(rental_eid,true).empty()&&db.query("SELECT 1 FROM native_expired_items WHERE uid=?",{std::int64_t(rental_bag.uid)}).size()==1,"Expired bag was not archived");
            auto hidden_snap=inventory.snapshots(rental_eid,now());
            require(hidden_snap.size()==1&&hidden_snap[0][5]==1,"Unavailable bag contents leaked into visible snapshot");
            Bytes sort_request(10);put32(sort_request,0,rental_eid,true);put16(sort_request,8,0x22a,true);
            inventory.organize(rental_eid,sort_request);
            auto after_sort=db.query("SELECT slot,quantity FROM inventory_slots WHERE instance_uid=?",{std::int64_t(hidden.uid)});
            require(after_sort.size()==1&&after_sort[0]["slot"]==256&&after_sort[0]["quantity"]==20,"Sorting moved or deleted expired bag contents");
            inventory.grant_once(rental_eid,{{69009,1}},"replacement-bag");
            ItemInstance replacement;for(auto&i:inventory.items(rental_eid))if(i.item==69009)replacement=i;
            inventory.equip(rental_eid,replacement.slot,replacement.item,600,120,13,now());
            require(inventory.owned(rental_eid,256,54005,now()).uid==hidden.uid,"Replacing expired bag did not restore preserved contents");
            inventory.grant_timed_once(eid+21,48017,20,60,"expired-slots");
            db.exec("UPDATE reward_instances SET expires_at=? WHERE player_eid=?",{std::int64_t(now()-1),std::int64_t(eid+21)});
            inventory.grant_once(eid+21,{{16001,1}},"reuse-expired-slot");
            require(inventory.items(eid+21).size()==1&&inventory.items(eid+21)[0].slot==0,"Expired invisible items kept occupying full inventory");
            require(!inventory.expire(eid+21,now()),"Expiration archival was not idempotent");
            constexpr std::uint32_t first=61001, sibling=61002, outsider=61003;
            db.exec("CREATE TABLE accounts(id TEXT PRIMARY KEY,payload TEXT NOT NULL)");
            db.exec("INSERT INTO accounts VALUES(?,?)",
                    {std::string("login-A"),Json{{"chars",Json::array({{{"eid",first},{"level",10}},{{"eid",sibling},{"level",9},{"name","Sibling"}}})}}.dump()});
            db.exec("INSERT INTO accounts VALUES(?,?)",
                    {std::string("login-B"),Json{{"chars",Json::array({{{"eid",outsider},{"level",10}}})}}.dump()});
            inventory.grant_once(first,{{54005,3},{16001,1}},"storage-fixture");
            auto rowsForStorage=inventory.items(first);
            const auto stack=*std::find_if(rowsForStorage.begin(),rowsForStorage.end(),[](const auto &i){return i.item==54005;});
            const auto gear=*std::find_if(rowsForStorage.begin(),rowsForStorage.end(),[](const auto &i){return i.item==16001;});
            const auto stamp=now();
            inventory.storage_deposit("login-A",first,PersonalStorage::Character,gear.slot,55,gear.item,gear.uid,stamp);
            auto privatePacket=inventory.storage_snapshot("login-A",first,PersonalStorage::Character,stamp);
            require(privatePacket.size()==168&&get32(privatePacket,0,true)==first&&get32(privatePacket,12,true)==56&&
                    get32(privatePacket,16,true)==1&&get16(privatePacket,20,true)==0xc6&&
                    get32(privatePacket,22)==gear.item&&get16(privatePacket,26)==55&&get32(privatePacket,29)==gear.uid,
                    "Character storage snapshot differs from native wire");
            require(inventory.storage_items("login-A",sibling,PersonalStorage::Character,stamp).empty(),
                    "Sibling read the private character warehouse");
            inventory.storage_deposit("login-A",first,PersonalStorage::Account,stack.slot,0,stack.item,stack.uid,stamp);
            auto shared=inventory.storage_items("login-A",sibling,PersonalStorage::Account,stamp);
            require(shared.size()==1&&shared[0].uid==stack.uid&&shared[0].quantity==3,"Account warehouse not shared");
            auto accountPacket=inventory.storage_snapshot("login-A",sibling,PersonalStorage::Account,stamp);
            require(accountPacket.size()==168&&get32(accountPacket,12,true)==16&&get32(accountPacket,29)==stack.uid,
                    "Account storage snapshot differs from native wire");
            reject([&]{inventory.storage_withdraw("login-A",outsider,PersonalStorage::Account,0,0,stack.uid,stamp);},
                   "Foreign character impersonated account");
            reject([&]{inventory.storage_withdraw("login-B",outsider,PersonalStorage::Account,0,0,stack.uid,stamp);},
                   "Another account withdrew the stack");
            inventory.grant_once(sibling,{{54005,1}},"storage-blocked-slot");
            reject([&]{inventory.storage_withdraw("login-A",sibling,PersonalStorage::Account,0,0,stack.uid,stamp);},
                   "Withdrawal overwrote an occupied slot");
            const auto withdrawnSlot=inventory.storage_withdraw("login-A",sibling,PersonalStorage::Account,0,1,stack.uid,stamp);
            require(inventory.owned(sibling,withdrawnSlot,stack.item,stamp).uid==stack.uid,
                    "Withdrawal replaced the original stack UID");
            require(inventory.storage_items("login-A",first,PersonalStorage::Account,stamp).empty(),
                    "Warehouse duplicated the withdrawn stack");
            inventory.grant_timed_once(first,48017,1,60,"storage-rental");
            const auto rental=inventory.items(first).front();
            require(rental.expires.has_value(),"Storage rental fixture missing its deadline");
            inventory.storage_deposit("login-A",first,PersonalStorage::Account,rental.slot,0,rental.item,rental.uid,stamp);
            auto rentalCopy=inventory.storage_items("login-A",sibling,PersonalStorage::Account,stamp);
            require(rentalCopy.size()==1&&rentalCopy.front().expires==rental.expires,"Storage renewed rental validity");
            require(inventory.storage_items("login-A",sibling,PersonalStorage::Account,*rental.expires).empty(),
                    "Expired rental remained available in warehouse");
            inventory.grant_once(first,{{54005,100}},"warehouse-partial-stack");
            const auto original=inventory.items(first).front();
            auto split=inventory.storage_deposit("login-A",first,PersonalStorage::Account,original.slot,0,
                                                 original.item,original.uid,stamp,90);
            require(split.size()==1&&split[0].first==0&&split[0].second==90,
                    "Partial deposit acknowledgement lost quantity");
            require(inventory.owned(first,original.slot,original.item,stamp).quantity==10,
                    "Partial deposit lost remainder");
            auto splitStack=inventory.storage_items("login-A",sibling,PersonalStorage::Account,stamp).front();
            require(splitStack.uid!=original.uid&&splitStack.quantity==90,"Split stack reused original UID");
            auto merged=inventory.storage_deposit("login-A",first,PersonalStorage::Account,original.slot,1,
                                                  original.item,original.uid,stamp,10);
            require(merged.size()==1&&merged[0].first==0&&merged[0].second==10,
                    "Compatible stack did not merge in existing slot");
            auto mergedStack=inventory.storage_items("login-A",sibling,PersonalStorage::Account,stamp).front();
            require(mergedStack.uid==splitStack.uid&&mergedStack.quantity==100,"Merge lost quantity or destination UID");
            Wallet clanWallet(db,inventory);
            clanWallet.apply(first,50000,"clan-boundary");
            reject([&]{inventory.guild_create("login-A",first,"First clan");},"Clan creation accepted exact threshold");
            reject([&]{inventory.guild_create("login-A",sibling,"Too early");},"Clan creation accepted level below 10");
            clanWallet.apply(first,1,"clan-extra-carat");
            const auto firstClan=inventory.guild_create("login-A",first,"First clan");
            require(clanWallet.balance(first)==1&&inventory.guild_storage_membership_id("login-A",first)==firstClan,
                    "Clan creation failed to charge once and assign master");
            reject([&]{inventory.guild_create("login-A",first,"Duplicate");},"Master founded a second clan");
            clanWallet.apply(outsider,50001,"clan-outsider");
            reject([&]{inventory.guild_create("login-B",outsider,"FIRST CLAN");},"Case-equivalent clan name was accepted");
            require(clanWallet.balance(outsider)==50001,"Rejected guild creation charged Carats");
            inventory.guild_create("login-B",outsider,"Other clan");
            db.exec("CREATE TABLE native_public_character_names(character_eid INTEGER PRIMARY KEY,account_id TEXT NOT NULL,name TEXT NOT NULL)");
            db.exec("INSERT INTO native_public_character_names VALUES(61002,'login-A','Sibling')");
            auto invite=inventory.guild_invite("login-A",first,"sibling",stamp);
            reject([&]{inventory.guild_invite_answer("login-B",outsider,invite,true,stamp);},
                   "Foreign character accepted the guild invitation");
            reject([&]{inventory.guild_invite_answer("login-A",sibling,invite,true,stamp+60);},
                   "Expired guild invitation was accepted");
            require(inventory.guild_invite_answer("login-A",sibling,invite,true,stamp+1),
                    "Invited character could not join the clan");
            reject([&]{inventory.guild_invite_answer("login-A",sibling,invite,true,stamp+1);},
                   "Guild invitation replay succeeded");
            inventory.guild_change_rank("login-A",first,sibling,2);
            const auto guildSlot=inventory.storage_withdraw("login-A",first,PersonalStorage::Character,55,
                                                            std::nullopt,gear.uid,stamp);
            inventory.storage_deposit("login-A",first,PersonalStorage::Guild,guildSlot,55,gear.item,gear.uid,stamp);
            require(inventory.storage_items("login-A",sibling,PersonalStorage::Guild,stamp).front().uid==gear.uid,
                    "Guild member cannot see shared storage");
            require(inventory.storage_items("login-B",outsider,PersonalStorage::Guild,stamp).empty(),
                    "Other clan could see guild storage");
            reject([&]{inventory.storage_withdraw("login-A",sibling,PersonalStorage::Guild,55,std::nullopt,gear.uid,stamp);},
                   "Guild member withdrew without permission");
            reject([&]{inventory.guild_storage_set_rights("login-A",sibling,2,7);},
                   "Guild member modified its own permissions");
            inventory.guild_storage_set_rights("login-A",first,2,7);
            auto receivedGuildSlot=inventory.storage_withdraw("login-A",sibling,PersonalStorage::Guild,55,std::nullopt,gear.uid,stamp);
            require(inventory.owned(sibling,receivedGuildSlot,gear.item,stamp).uid==gear.uid,
                    "Guild permission grant did not enable withdrawal");
            inventory.guild_storage_set_rights("login-A",first,2,1);
            reject([&]{inventory.storage_deposit("login-A",sibling,PersonalStorage::Guild,receivedGuildSlot,55,gear.item,gear.uid,stamp);},
                   "Revoked guild permission left deposit enabled");
            clanWallet.apply(first,1000,"warehouse-carats-funds");
            require(inventory.storage_carats("login-A",first,PersonalStorage::Guild,800,"clan-carat-deposit")==800,
                    "Guild Carats were not deposited");
            require(inventory.storage_carats("login-A",first,PersonalStorage::Guild,800,"clan-carat-deposit")==800&&
                    clanWallet.balance(first)==201,"Duplicate warehouse request debited twice");
            reject([&]{inventory.storage_carats("login-A",first,PersonalStorage::Account,800,"clan-carat-deposit");},
                   "Warehouse replay changed destination");
            reject([&]{inventory.storage_carats("login-A",sibling,PersonalStorage::Guild,-300,"clan-carat-take");},
                   "Guild member withdrew Carats without permission");
            reject([&]{inventory.storage_carats("login-B",outsider,PersonalStorage::Guild,-300,"foreign-clan-carat-take");},
                   "Other clan withdrew Carats");
            inventory.guild_storage_set_rights("login-A",first,2,17);
            require(inventory.storage_carats("login-A",sibling,PersonalStorage::Guild,-300,"clan-carat-take")==500&&
                    clanWallet.balance(sibling)==300,"Carats withdrawal authorization or accounting failed");
            reject([&]{inventory.storage_carats("login-A",sibling,PersonalStorage::Guild,10,"clan-carat-put");},
                   "Withdrawal permission also enabled deposit");
            inventory.guild_storage_set_rights("login-A",first,2,1);
            reject([&]{inventory.storage_carats("login-A",sibling,PersonalStorage::Guild,-1,"clan-revoked-carat-take");},
                   "Revoked Carats permission remained effective");
        }
        fs::remove_all(fixture);
        std::cout << "PASS: immutable item UIDs; native stack limits; account isolation; idempotent grants; "
                     "three independent 44-slot bags; equipment/unequip; +18 record; permanent lifetime; "
                     "overflow rollback; full snapshots; item chat names\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\nEvidence: " << fixture.string() << "\n";
        return 1;
    }
}
