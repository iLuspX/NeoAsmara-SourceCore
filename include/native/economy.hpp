#pragma once
#include "inventory.hpp"
namespace rosh {
// One recipe execution, including fee/materials/result, is atomic and replay-safe.
bool craft_inventory(Sqlite& db,Inventory& inventory,std::uint32_t eid,const Json& recipe,
                     const std::string& event,double timestamp,double roll);
class Wallet {
    Sqlite &db_;
    Inventory &inventory_;
    void synchronize(std::uint32_t eid, std::int64_t balance);

  public:
    Wallet(Sqlite &db, Inventory &inventory);
    std::int64_t balance(std::uint32_t eid);
    std::pair<std::int64_t, bool> apply(std::uint32_t eid, std::int64_t delta, const std::string &event);
    std::vector<Bytes> updates(std::uint32_t eid);
};
enum class ConsignmentCategory { MountShoulders, WeaponOther, ArmorTorso };

std::optional<ConsignmentCategory> consignment_category(const Json &definition);
enum class ConsignmentOrder { Listing, Level, Price };
struct ConsignmentSearch {
    std::optional<ConsignmentCategory> category;
    std::uint32_t item{}, seller{};
    std::optional<std::pair<int,int>> catalog_type;
    std::optional<int> catalog_slot;
    int min_level{}, max_level{255};
    int min_enhancement{}, max_enhancement{127};
    ConsignmentOrder order{ConsignmentOrder::Price};
    bool descending{};
};
Json consignment_search(Sqlite &db, const Inventory &inventory, const ConsignmentSearch &filter,
                        double time, std::uint32_t page);
Bytes native_consignment_search(Sqlite &db, Inventory &inventory, const Bytes &request, double time,
                                std::map<std::uint32_t,std::int64_t> *quotes=nullptr);
Bytes native_consignment_seller_page(Sqlite &db, Inventory &inventory, std::uint32_t seller, double time);
Bytes native_consignment_page(Sqlite &db, Inventory &inventory, const Bytes &request, const Json &page);
void consignment_schema(Sqlite &db);
void native_consignment_register(Sqlite &db, Inventory &inventory, std::uint32_t seller,
                                 const Bytes &request, const std::string &event, double time);
std::int64_t consignment_list(Sqlite &db, Inventory &inventory, std::uint32_t seller, int slot,
                             std::uint32_t iid, std::uint32_t uid, std::int64_t price, double time,
                             std::optional<std::int64_t> quantity={});
void consignment_buy(Sqlite &db, Inventory &inventory, std::uint32_t buyer, std::int64_t listing,
                     std::int64_t expected_price, double time);
void consignment_expire(Sqlite &db, std::uint32_t seller, double time);
void consignment_claim_return(Sqlite &db, Inventory &inventory, std::uint32_t seller,
                              const std::string &mail, double time);
} // namespace rosh
