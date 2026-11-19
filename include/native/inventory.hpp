#pragma once
#include "core.hpp"
#include "equipment_rng.hpp"
#include <optional>
#include <set>
namespace rosh {
struct ItemInstance {
    int slot{};
    std::uint32_t item{}, uid{};
    std::int64_t quantity{};
    int grade{};
    std::uint32_t created{};
    std::optional<std::uint32_t> expires;
};
enum class PersonalStorage { Character, Account, Guild };
enum class StorageRight : unsigned { View=1, Deposit=2, Withdraw=4, DepositCarats=8, WithdrawCarats=16 };
inline constexpr int storage_capacity(PersonalStorage kind) { return kind==PersonalStorage::Account?16:56; }
class Wallet;
class GroundLoot;
class CashChoices;
struct InventoryFull : std::runtime_error {
    InventoryFull() : std::runtime_error("Todas as bolsas estao cheias") {}
};
class Inventory {
    friend class Wallet;
    friend class GroundLoot;
    friend class CashChoices;
    Sqlite &db_;
    Json rules_, records_, templates_;
    std::map<std::string,std::uint16_t>visual_lookup_;
    std::shared_ptr<EquipmentRng> affixes_;
    void initial_affixes(std::uint32_t uid,std::uint32_t item);
    void visual_attributes(Bytes &bytes,const ItemInstance&item);
    std::map<std::uint32_t, Json> names_;
    std::uint32_t next_uid();
    int insert(std::uint32_t eid, std::uint32_t item, int quantity, std::optional<int> preferred = {},
               std::optional<std::uint32_t> uid = {}, std::optional<std::uint32_t> created = {},
               bool separate = false);

  public:
    Inventory(Sqlite &db, Content &content);
    static int key(int category, int slot);
    static std::pair<int, int> address(int key);
    std::set<int> available(std::uint32_t eid, double timestamp);
    std::vector<ItemInstance> items(std::uint32_t eid, bool equipped = false);
    bool expire(std::uint32_t eid, double timestamp);
    ItemInstance owned(std::uint32_t eid, int slot, std::uint32_t item, double timestamp);
    bool grant_once(std::uint32_t eid, const std::vector<std::pair<std::uint32_t, int>> &entries,
                    const std::string &event, bool separate = false);
    bool grant_timed_once(std::uint32_t eid,std::uint32_t item,int quantity,int seconds,const std::string &event);
    void consume(std::uint32_t eid, const ItemInstance &item, std::int64_t quantity = 1,
                 const std::string& reason = "consumed");
    void trace_item(std::uint32_t uid,const std::string& operation,std::uint32_t holder,
                    std::int64_t quantity,int source_slot,int destination_slot);
    bool traced(std::uint32_t uid);
    void trace_derivation(std::uint32_t parent,const ItemInstance& child,std::uint32_t owner);
    void move(std::uint32_t eid,const Bytes&body);
    ItemInstance detach(std::uint32_t eid,int slot,std::uint32_t iid,std::uint32_t uid,double time,
                        std::optional<std::int64_t> quantity={});
    int restore(std::uint32_t eid,const ItemInstance &item,double time,std::optional<int> slot={});
    std::vector<ItemInstance> storage_items(const std::string &account,std::uint32_t eid,
                                            PersonalStorage kind,double time);
    Bytes storage_snapshot(const std::string &account,std::uint32_t eid,PersonalStorage kind,double time);
    std::vector<std::pair<int,std::int64_t>> storage_deposit(
        const std::string &account,std::uint32_t eid,PersonalStorage kind,
        int from,int to,std::uint32_t item,std::uint32_t uid,double time,
        std::optional<std::int64_t> quantity={});
    int storage_withdraw(const std::string &account,std::uint32_t eid,PersonalStorage kind,
                         int from,std::optional<int> to,std::uint32_t uid,double time);
    void guild_storage_set_rights(const std::string &account,std::uint32_t eid,int rank,unsigned rights);
    std::int64_t guild_storage_membership_id(const std::string &account,std::uint32_t eid);
    std::int64_t storage_carats(const std::string &account,std::uint32_t eid,PersonalStorage kind,
                                std::int64_t delta,const std::string &event,std::optional<int> slot={});
    std::int64_t guild_create(const std::string &account,std::uint32_t eid,const std::string &name);
    std::string guild_invite(const std::string &account,std::uint32_t eid,const std::string &name,double time);
    bool guild_invite_answer(const std::string &account,std::uint32_t eid,const std::string &invite,bool accept,double time);
    void guild_change_rank(const std::string &account,std::uint32_t eid,std::uint32_t target,int rank);
    void guild_depart(const std::string&account,std::uint32_t eid,std::uint32_t target,
                      const std::string&event,double time);
    std::int64_t guild_departure_latest();
    Json guild_departure_poll(const std::string&account,std::uint32_t eid,std::int64_t after,double time);
    Json guild_native_invite(const std::string&account,std::uint32_t eid,std::uint32_t target,
                             const std::string&name,const std::string&event,double time);
    Json guild_native_answer(const std::string&account,std::uint32_t eid,std::uint32_t inviter,
                             bool accept,const std::string&event,double time);
    std::int64_t guild_native_result_latest();
    Json guild_native_results(const std::string&account,std::uint32_t eid,std::int64_t after,double time);
    std::int64_t guild_chat_latest();
    void guild_chat_post(const std::string&account,std::uint32_t eid,const std::string&sender,
                         const std::string&message,const std::string&event,double time);
    Json guild_chat_poll(const std::string&account,std::uint32_t eid,std::int64_t after,double time);
    void organize(std::uint32_t eid,const Bytes&body);
    Json trash(std::uint32_t eid,const Bytes&body,const std::string&event);
    int equip(std::uint32_t eid, int inventory_slot, std::uint32_t item, int profession, int level,
              int requested_slot, double timestamp);
    int unequip(std::uint32_t eid, int equipment_slot, std::uint32_t item, std::optional<int> destination,
                double timestamp);
    Bytes update(std::uint32_t eid, const ItemInstance &item, bool equipped = false, bool create = true);
    std::vector<Bytes> snapshots(std::uint32_t eid, double timestamp);
    std::string name(std::uint32_t item) const;
    Json definition(std::uint32_t item) const {return rules_.value(std::to_string(item),Json::object());}
    Bytes additional_attributes(std::uint32_t eid,const Bytes&body,const std::string&event,double timestamp);
};
} // namespace rosh
