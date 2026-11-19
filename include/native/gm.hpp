#pragma once
#include "entities.hpp"
#include "inventory.hpp"
#include <set>
namespace rosh {
void gm_schema(Sqlite &db);
// Uses the same authority rules at login, on every API call and before queued delivery.
Json gm_authority(const fs::path &root, const std::string &account, std::uint32_t eid);
void gm_deliver(Sqlite &db, Inventory &inventory, const Json &request);
std::int64_t gm_mail_latest_unread(Sqlite &db, std::uint32_t eid, double time);
std::vector<Json> gm_mail_inbox(Sqlite &db, std::uint32_t eid, double time);
Json gm_mail_access(const fs::path &root, Sqlite &db, Inventory &inventory,
                    std::uint32_t eid, const Bytes &request, bool claim, double time);
Bytes native_mail_list(const std::vector<Json> &mails, double time);
Bytes native_mail_detail(Inventory &inventory, std::uint32_t eid, const Json &mail);
Json gm_equipment_options(const Json&rule=Json::object());
Json gm_equipment_validate(const Json&config,const Json&rule);
void gm_equipment_apply(Sqlite&db,Inventory&inventory,std::uint32_t eid,std::uint32_t uid,std::uint32_t item,const Json&config);
class GmController {
    fs::path root_;
    Content content_;
    EntityCatalog entities_;
    AccountStore accounts_;
    LegacyStore world_;
    Sqlite roles_, credentials_, runtime_, moderation_;
    Inventory inventory_;
    Json item_rows_, entity_rows_ = Json::array();
    std::map<std::string, Entity> placements_;
    struct Access { std::string account, generation; std::uint32_t eid; double until; };
    std::map<std::string, Access> sessions_;
    std::map<std::string, std::pair<int,double>> failures_;
    Json authorize(const std::string &secret);
    Json players();
    Json execute(const Json &staff, const Json &body);
  public:
    explicit GmController(const fs::path &root);
    Json request(const std::string &method, const std::string &path, const Json &body,
                 const std::string &bearer = "");
};
Bytes gm_mail_delete(Sqlite &db,std::uint32_t eid,const Bytes &request,double time);
// WorldSession calls this on its own connection thread, never through GUI database writes.
void gm_world_poll(const fs::path &root, Sqlite &db, Inventory &inventory, std::uint32_t eid,
                   const std::string &map, int channel, const Json &position,
                   std::set<std::uint32_t> &visible,
                   const std::function<void(unsigned char,unsigned char,const Bytes&)> &send,
                   const std::function<void()> &inventory_changed);
}
