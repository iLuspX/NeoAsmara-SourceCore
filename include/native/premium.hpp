#pragma once
#include "inventory.hpp"
namespace rosh {
inline void initialize_catalog_edits(Sqlite& db) {
 db.exec("CREATE TABLE IF NOT EXISTS cash_price_edits(offer INTEGER PRIMARY KEY,price INTEGER NOT NULL CHECK(price BETWEEN 1 AND 1000000));"
         "CREATE TABLE IF NOT EXISTS gacha_prize_edits(tab INTEGER NOT NULL,position INTEGER NOT NULL,item INTEGER NOT NULL,quantity INTEGER NOT NULL,weight INTEGER NOT NULL,grade INTEGER NOT NULL,PRIMARY KEY(tab,position));"
         "CREATE TABLE IF NOT EXISTS premium_catalog_audit(id INTEGER PRIMARY KEY,actor_account TEXT NOT NULL,actor_eid INTEGER NOT NULL,kind TEXT NOT NULL,old_value TEXT NOT NULL,new_value TEXT NOT NULL,created_at INTEGER NOT NULL)");
}

inline Json current_gacha_pool(Sqlite& db, const Json& gacha, int tab) {
 auto pool=gacha.at("tabs").at(std::to_string(tab));
 for(const auto& edit:db.query("SELECT position,item,quantity,weight,grade FROM gacha_prize_edits WHERE tab=?",{std::int64_t(tab)})) {
  auto index=edit.at("position").get<std::size_t>();
  if(index>=pool.size())throw std::runtime_error("Gacha edit outside the current prize list");
  pool.at(index)={{"item",edit.at("item")},{"quantity",edit.at("quantity")},{"weight",edit.at("weight")},{"grade",edit.at("grade")}};
 }
 return pool;
}

class PremiumShop {
 Content &content_;
 Sqlite&db_;Inventory&inventory_;Json shop_,gacha_,rules_;
 std::map<int,std::int64_t> cash_quotes_;
 std::map<int,Json> gacha_quotes_;
 Json replay(const std::string&event,const std::string&account,std::uint32_t eid,const std::string&kind,const std::string&request);
 void record(const std::string&event,const std::string&account,std::uint32_t eid,const std::string&kind,const std::string&request,const Json&result);
 public:
 PremiumShop(Sqlite&db,Inventory&inventory,Content&content);
 std::int64_t balance(const std::string&account);
 std::int64_t cents(const std::string&account);
 std::vector<Json> state(const std::string&account,int profession);
 Json history(const std::string&account);
 Json buy(const std::string&account,std::uint32_t eid,int profession,int offer,int copies,const std::string&event);
 Bytes machines(const Bytes&request);
 Bytes prizes(const Bytes&request);
 Bytes spin(const std::string&account,std::uint32_t eid,const Bytes&body,const std::string&event,const std::function<std::uint32_t(std::uint32_t)>&random={});
};
}
