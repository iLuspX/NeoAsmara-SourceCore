#include "gameplay.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

namespace karos {
namespace {
class Query {
public:
    Query(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Query() { sqlite3_finalize(stmt_); }
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
    void number(int n, std::int64_t v) {
        if (sqlite3_bind_int64(stmt_, n, v) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
    }
    void real(int n, double v) {
        if (sqlite3_bind_double(stmt_, n, v) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
    }
    void text(int n, const std::string& v) {
        if (sqlite3_bind_text(stmt_, n, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
    }
    bool next() {
        const auto rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    void run() { if (next()) throw std::runtime_error("unexpected SQL result row"); }
    sqlite3_stmt* get() const { return stmt_; }
    std::int64_t integer(int n) { return sqlite3_column_int64(stmt_,n); }
    double decimal(int n) { return sqlite3_column_double(stmt_,n); }
    std::string str(int n) {
        const unsigned char* p=sqlite3_column_text(stmt_,n);
        return p ? reinterpret_cast<const char*>(p) : "";
    }
private:
    sqlite3* db_{};
    sqlite3_stmt* stmt_{};
};

void execute(sqlite3* db, const char* sql) {
    char* err=nullptr;
    if (sqlite3_exec(db,sql,nullptr,nullptr,&err)!=SQLITE_OK) {
        std::string message=err?err:sqlite3_errmsg(db);
        sqlite3_free(err);
        throw std::runtime_error(message);
    }
}

struct Transaction {
    sqlite3* db;
    bool active{true};
    explicit Transaction(sqlite3* handle):db(handle) { execute(db,"BEGIN IMMEDIATE"); }
    ~Transaction() { if(active) sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); }
    void commit() { execute(db,"COMMIT"); active=false; }
};

std::int64_t resolve_time(std::int64_t now) {
    if(now>0) return now;
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

double distance3(double ax,double ay,double az,double bx,double by,double bz) {
    return std::hypot(std::hypot(ax-bx,ay-by),az-bz);
}

// No user-supplied damage: this deliberately simple formula is for an OFFLINE LAB.
// It is NOT validated Karos retail combat balance.
int lab_damage(int level,double mob_defense) {
    const int defense = std::isfinite(mob_defense) ?
        static_cast<int>(std::clamp(mob_defense,0.0,1000000.0)) : 0;
    return std::max(1,20 + level*3 - defense);
}

void give_exp_inside_transaction(sqlite3* db, std::int64_t eid,
                                 std::int64_t old_exp, std::int64_t exp, int level,
                                 int& level_after) {
    if (exp<=0) {level_after=level;return;}
    if (old_exp > std::numeric_limits<std::int64_t>::max() - exp)
        throw std::runtime_error("XP overflow");
    auto new_exp=old_exp+exp;
    int points=0;
    while(level<110) {
        Query threshold(db,"SELECT cumulative_exp,skill_points_reward FROM level_curve WHERE target_level=?");
        threshold.number(1,level+1);
        if(!threshold.next() || new_exp<threshold.integer(0)) break;
        ++level;
        points += std::max(0,static_cast<int>(threshold.integer(1)));
    }
    Query update(db,"UPDATE characters SET exp=?,level=?,skill_points=skill_points+?,updated_at=CURRENT_TIMESTAMP WHERE eid=?");
    update.number(1,new_exp);update.number(2,level);update.number(3,points);update.number(4,eid);
    update.run();
    level_after=level;
}

} // namespace

Gameplay::Gameplay(Database& db): db_(db) {
    // An additive migration: never modifies reference tables or existing saves.
    execute(db_.native_handle(),R"SQL(
      CREATE TABLE IF NOT EXISTS world_mob_state(
        spawn_id INTEGER PRIMARY KEY,
        remaining_hp INTEGER NOT NULL CHECK(remaining_hp>=0),
        respawn_at INTEGER NOT NULL DEFAULT 0,
        FOREIGN KEY(spawn_id) REFERENCES spawn_points(spawn_id) ON DELETE CASCADE
      )
    )SQL");
}

std::vector<NearbyMob> Gameplay::nearby(std::int64_t eid,double radius,int limit,std::int64_t now) {
    if (!(std::isfinite(radius) && radius>0 && radius<=5000) || limit<1 || limit>100)
        throw std::invalid_argument("invalid radius/limit");
    auto c=db_.get_character(eid);
    if(!c) throw std::invalid_argument("character not found");
    now=resolve_time(now);
    Query q(db_.native_handle(), R"SQL(
      SELECT s.spawn_id,s.npc_id,m.name,m.level,m.max_hp,
             COALESCE(t.remaining_hp,m.max_hp),COALESCE(t.respawn_at,0),
             s.x,s.y,s.z
      FROM spawn_points s JOIN monster_definitions m ON m.npc_id=s.npc_id
      LEFT JOIN world_mob_state t ON t.spawn_id=s.spawn_id
      WHERE s.map_name=? AND s.enabled=1 AND m.enabled=1
        AND ABS(s.x-?)<=? AND ABS(s.y-?)<=? AND ABS(s.z-?)<=?
      LIMIT 10000
    )SQL");
    q.text(1,c->map_name);
    q.real(2,c->x);q.real(3,radius);q.real(4,c->y);q.real(5,radius);
    q.real(6,c->z);q.real(7,radius);
    std::vector<NearbyMob> result;
    while(q.next()) {
        const double d=distance3(c->x,c->y,c->z,q.decimal(7),q.decimal(8),q.decimal(9));
        if(d>radius) continue;
        const auto respawn=q.integer(6);
        const int hp=respawn>now ? 0 : (respawn!=0 ? static_cast<int>(q.integer(4)) : static_cast<int>(q.integer(5)));
        result.push_back({q.integer(0),static_cast<int>(q.integer(1)),q.str(2),
                          static_cast<int>(q.integer(3)),hp,static_cast<int>(q.integer(4)),
                          std::max<std::int64_t>(0,respawn-now),d});
    }
    std::sort(result.begin(),result.end(),[](const NearbyMob& a,const NearbyMob& b){return a.distance<b.distance;});
    if(result.size()>static_cast<std::size_t>(limit)) result.resize(static_cast<std::size_t>(limit));
    return result;
}

AttackResult Gameplay::attack(std::int64_t eid,std::int64_t spawn_id,std::int64_t now) {
    now=resolve_time(now);
    auto* db=db_.native_handle();
    Transaction tx(db);
    auto c=db_.get_character(eid);
    if(!c) throw std::invalid_argument("character not found");
    Query spawn(db,R"SQL(
        SELECT s.map_name,s.x,s.y,s.z,s.respawn_seconds,m.npc_id,m.name,m.level,
               m.max_hp,m.defense,m.exp_reward
        FROM spawn_points s JOIN monster_definitions m ON s.npc_id=m.npc_id
        WHERE s.spawn_id=? AND s.enabled=1 AND m.enabled=1
    )SQL");
    spawn.number(1,spawn_id);
    if(!spawn.next()) throw std::invalid_argument("spawn not found or disabled");
    if(c->map_name!=spawn.str(0) ||
        distance3(c->x,c->y,c->z,spawn.decimal(1),spawn.decimal(2),spawn.decimal(3))>25.0)
        throw std::runtime_error("too far from mob or different map");
    const auto npc=spawn.integer(5);
    const auto max_hp=static_cast<int>(spawn.integer(8));
    if(max_hp<1) throw std::runtime_error("mob has invalid HP");
    const auto mob_name=spawn.str(6);
    const auto xp=std::max<std::int64_t>(0,spawn.integer(10));
    const auto respawn_seconds=std::max<std::int64_t>(1,spawn.integer(4));
    Query state(db,"SELECT remaining_hp,respawn_at FROM world_mob_state WHERE spawn_id=?");
    state.number(1,spawn_id);
    int hp=max_hp;
    std::int64_t respawn_at=0;
    if(state.next()){ hp=static_cast<int>(state.integer(0)); respawn_at=state.integer(1); }
    if(respawn_at>now) throw std::runtime_error("mob dead; wait for respawn");
    if(respawn_at!=0 && respawn_at<=now) hp=max_hp;
    if(hp<=0) hp=max_hp;
    const int damage=std::min(hp,lab_damage(c->level,spawn.decimal(9)));
    const int remaining=hp-damage;
    const bool killed=remaining==0;
    const std::int64_t next_respawn=killed ? now+respawn_seconds : 0;
    Query set(db,R"SQL(
      INSERT INTO world_mob_state(spawn_id,remaining_hp,respawn_at) VALUES(?,?,?)
      ON CONFLICT(spawn_id) DO UPDATE SET remaining_hp=excluded.remaining_hp,
                                       respawn_at=excluded.respawn_at
    )SQL");
    set.number(1,spawn_id);set.number(2,remaining);set.number(3,next_respawn);set.run();
    AttackResult result{mob_name,damage,remaining,killed,0,c->level,next_respawn,{}};
    if(killed) {
        result.gained_exp=xp;
        give_exp_inside_transaction(db,eid,c->exp,xp,c->level,result.level_after);
        Query drops(db,"SELECT item_id,chance_bp,min_qty,max_qty FROM monster_drops WHERE npc_id=?");
        drops.number(1,npc);
        static thread_local std::mt19937 rng{std::random_device{}()};
        while(drops.next()) {
            const int chance=static_cast<int>(drops.integer(1));
            const int min_qty=static_cast<int>(drops.integer(2));
            const int max_qty=static_cast<int>(drops.integer(3));
            if(min_qty<1 || max_qty<min_qty || max_qty>1000000) continue;
            if(chance<1 || chance>10000 || std::uniform_int_distribution<int>(1,10000)(rng)>chance) continue;
            const int qty=std::uniform_int_distribution<int>(min_qty,max_qty)(rng);
            Query ground(db,R"SQL(
                INSERT INTO ground_items(map_name,item_id,quantity,x,y,z,owner_eid)
                VALUES(?,?,?,?,?,?,?)
            )SQL");
            ground.text(1,c->map_name);ground.number(2,drops.integer(0));
            ground.number(3,qty);ground.real(4,spawn.decimal(1));
            ground.real(5,spawn.decimal(2));ground.real(6,spawn.decimal(3));
            ground.number(7,eid);ground.run();
            result.ground_item_ids.push_back(sqlite3_last_insert_rowid(db));
        }
    }
    tx.commit();
    return result;
}

void Gameplay::pickup(std::int64_t eid,std::int64_t ground_item_id) {
    auto* db=db_.native_handle();
    Transaction tx(db);
    auto c=db_.get_character(eid);
    if(!c) throw std::invalid_argument("character not found");
    Query ground(db,R"SQL(
      SELECT map_name,item_id,quantity,x,y,z,owner_eid,picked_up
      FROM ground_items WHERE ground_item_id=?
    )SQL");
    ground.number(1,ground_item_id);
    if(!ground.next()) throw std::runtime_error("item on ground not found");
    if(ground.integer(7)!=0) throw std::runtime_error("item already picked up");
    if(ground.str(0)!=c->map_name || (sqlite3_column_type(ground.get(),6)!=SQLITE_NULL && ground.integer(6)!=eid) ||
       distance3(c->x,c->y,c->z,ground.decimal(3),ground.decimal(4),ground.decimal(5))>25.0)
        throw std::runtime_error("item is too far, different map or owned by another character");
    const int item_id=static_cast<int>(ground.integer(1));
    int quantity=static_cast<int>(ground.integer(2));
    Query def(db,"SELECT stack_limit FROM item_definitions WHERE item_id=? AND enabled=1");
    def.number(1,item_id);
    if(!def.next() || quantity<1 || quantity>1000000) throw std::runtime_error("invalid item definition");
    const int stack_limit=std::max(1,static_cast<int>(def.integer(0)));
    Query stack(db,"SELECT slot,quantity FROM inventory WHERE character_eid=? AND item_id=? ORDER BY slot");
    stack.number(1,eid);stack.number(2,item_id);
    while(stack.next() && quantity>0) {
        const int room=std::max(0,stack_limit-static_cast<int>(stack.integer(1)));
        const int n=std::min(room,quantity);
        if(!n) continue;
        Query add(db,"UPDATE inventory SET quantity=quantity+? WHERE character_eid=? AND slot=?");
        add.number(1,n);add.number(2,eid);add.number(3,stack.integer(0));add.run();
        quantity-=n;
    }
    while(quantity>0) {
        Query slots(db,"SELECT slot FROM inventory WHERE character_eid=? ORDER BY slot");slots.number(1,eid);
        int slot=0;
        while(slots.next()) {
            if(slots.integer(0)==slot) ++slot;
            else if(slots.integer(0)>slot) break;
        }
        if(slot>=120) throw std::runtime_error("inventory full");
        const int n=std::min(stack_limit,quantity);
        Query insert(db,"INSERT INTO inventory(character_eid,slot,item_id,quantity) VALUES(?,?,?,?)");
        insert.number(1,eid);insert.number(2,slot);insert.number(3,item_id);insert.number(4,n);insert.run();
        quantity-=n;
    }
    Query finish(db,"UPDATE ground_items SET picked_up=1 WHERE ground_item_id=? AND picked_up=0");
    finish.number(1,ground_item_id);finish.run();
    if(sqlite3_changes(db)!=1) throw std::runtime_error("ground item was concurrently collected");
    tx.commit();
}

// All operations below are offline/core-side.  A future authenticated network
// adapter must bind each request to its session character; never trust an EID
// claimed by an unauthenticated game packet.
std::vector<NearbyNpc> Gameplay::nearby_npcs(std::int64_t eid,double radius) {
    if(!std::isfinite(radius) || radius<=0 || radius>3000) throw std::invalid_argument("invalid NPC radius");
    auto c=db_.get_character(eid);
    if(!c) throw std::invalid_argument("character not found");
    Query q(db_.native_handle(),"SELECT npc_id,name,npc_type,x,y,z FROM npc_definitions WHERE map_name=?");
    q.text(1,c->map_name);
    std::vector<NearbyNpc> result;
    while(q.next()) {
        const double d=distance3(c->x,c->y,c->z,q.decimal(3),q.decimal(4),q.decimal(5));
        if(d<=radius) result.push_back({q.integer(0),q.str(1),q.str(2),d});
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.distance<b.distance;});
    return result;
}

// Requires same map + proximity to the referenced NPC, and enabled shop.
static std::int64_t checked_shop(sqlite3* db,const Character& c,std::int64_t npc_id) {
    Query shop(db,R"SQL(
       SELECT sh.shop_id,n.map_name,n.x,n.y,n.z
       FROM shop_definitions sh JOIN npc_definitions n ON sh.npc_id=n.npc_id
       WHERE sh.npc_id=? AND sh.enabled=1
    )SQL");
    shop.number(1,npc_id);
    if(!shop.next()) throw std::runtime_error("NPC has no active shop");
    if(c.map_name!=shop.str(1) || distance3(c.x,c.y,c.z,shop.decimal(2),shop.decimal(3),shop.decimal(4))>25.)
        throw std::runtime_error("shop NPC is not in interaction range");
    return shop.integer(0);
}

std::vector<ShopOffer> Gameplay::shop(std::int64_t eid,std::int64_t npc_id) {
    auto c=db_.get_character(eid);
    if(!c) throw std::invalid_argument("character not found");
    const auto shop_id=checked_shop(db_.native_handle(),*c,npc_id);
    Query q(db_.native_handle(),R"SQL(
      SELECT si.item_id,i.name,si.price,si.stock FROM shop_items si
      JOIN item_definitions i ON i.item_id=si.item_id
      WHERE si.shop_id=? AND i.enabled=1 ORDER BY si.item_id
    )SQL");
    q.number(1,shop_id);
    std::vector<ShopOffer> result;
    while(q.next()) result.push_back({static_cast<int>(q.integer(0)),q.str(1),q.integer(2),static_cast<int>(q.integer(3))});
    return result;
}

static void add_to_inventory(sqlite3* db,std::int64_t eid,int item_id,int qty,int stack_limit) {
    if(qty<1 || stack_limit<1) throw std::invalid_argument("invalid inventory quantity/stack");
    Query old(db,"SELECT slot,quantity FROM inventory WHERE character_eid=? AND item_id=? ORDER BY slot");
    old.number(1,eid);old.number(2,item_id);
    while(old.next() && qty>0) {
        const int room=std::max(0,stack_limit-static_cast<int>(old.integer(1)));
        const int fill=std::min(qty,room);
        if(!fill) continue;
        Query upd(db,"UPDATE inventory SET quantity=quantity+? WHERE character_eid=? AND slot=?");
        upd.number(1,fill);upd.number(2,eid);upd.number(3,old.integer(0));upd.run();
        qty-=fill;
    }
    while(qty>0) {
        Query slots(db,"SELECT slot FROM inventory WHERE character_eid=? ORDER BY slot");slots.number(1,eid);
        int slot=0;
        while(slots.next()) {
            if(slots.integer(0)==slot) ++slot;
            else if(slots.integer(0)>slot) break;
        }
        if(slot>=120) throw std::runtime_error("inventory full");
        const int fill=std::min(qty,stack_limit);
        Query insert(db,"INSERT INTO inventory(character_eid,slot,item_id,quantity) VALUES(?,?,?,?)");
        insert.number(1,eid);insert.number(2,slot);insert.number(3,item_id);insert.number(4,fill);insert.run();
        qty-=fill;
    }
}

static void change_gold(sqlite3* db,std::int64_t eid,std::int64_t delta,const std::string& reason) {
    Query balance(db,"SELECT gold FROM characters WHERE eid=?");balance.number(1,eid);
    if(!balance.next()) throw std::runtime_error("character not found");
    const auto old=balance.integer(0);
    if((delta<0 && old < -delta) || (delta>0 && old>std::numeric_limits<std::int64_t>::max()-delta))
        throw std::runtime_error("not enough Carats or gold overflow");
    const auto after=old+delta;
    Query update(db,"UPDATE characters SET gold=?,updated_at=CURRENT_TIMESTAMP WHERE eid=?");
    update.number(1,after);update.number(2,eid);update.run();
    Query wallet(db,"INSERT INTO character_wallet(character_eid,gold) VALUES(?,?) ON CONFLICT(character_eid) DO UPDATE SET gold=excluded.gold");
    wallet.number(1,eid);wallet.number(2,after);wallet.run();
    Query ledger(db,"INSERT INTO wallet_ledger(character_eid,currency,delta,balance_after,reason) VALUES(?,'gold',?,?,?)");
    ledger.number(1,eid);ledger.number(2,delta);ledger.number(3,after);ledger.text(4,reason);ledger.run();
}

void Gameplay::buy(std::int64_t eid,std::int64_t npc_id,int item_id,int qty) {
    if(qty<1 || qty>10000) throw std::invalid_argument("buy quantity must be 1..10000");
    auto* db=db_.native_handle();Transaction tx(db);
    auto c=db_.get_character(eid); if(!c) throw std::invalid_argument("character not found");
    const auto shop_id=checked_shop(db,*c,npc_id);
    Query offer(db,R"SQL(
       SELECT si.price,si.stock,i.stack_limit FROM shop_items si
       JOIN item_definitions i ON i.item_id=si.item_id AND i.enabled=1
       WHERE si.shop_id=? AND si.item_id=?
    )SQL");
    offer.number(1,shop_id);offer.number(2,item_id);
    if(!offer.next()) throw std::runtime_error("item not sold in this shop");
    const auto price=offer.integer(0);const auto stock=offer.integer(1);
    const int stack=static_cast<int>(offer.integer(2));
    if(price<=0) throw std::runtime_error("item price is unset; not allowed to buy for free");
    if(stock>=0 && stock<qty) throw std::runtime_error("not enough shop stock");
    if(price>std::numeric_limits<std::int64_t>::max()/qty) throw std::runtime_error("cost overflow");
    const auto cost=price*qty;
    change_gold(db,eid,-cost,"NPC shop purchase");
    add_to_inventory(db,eid,item_id,qty,stack);
    if(stock>=0) {
       Query change(db,"UPDATE shop_items SET stock=stock-? WHERE shop_id=? AND item_id=? AND stock>=?");
       change.number(1,qty);change.number(2,shop_id);change.number(3,item_id);change.number(4,qty);change.run();
       if(sqlite3_changes(db)!=1) throw std::runtime_error("shop stock changed concurrently");
    }
    tx.commit();
}

void Gameplay::sell(std::int64_t eid,std::int64_t npc_id,int item_id,int qty) {
    if(qty<1 || qty>10000) throw std::invalid_argument("sell quantity must be 1..10000");
    auto* db=db_.native_handle();Transaction tx(db);
    auto c=db_.get_character(eid); if(!c) throw std::invalid_argument("character not found");
    (void)checked_shop(db,*c,npc_id);
    Query item(db,"SELECT sell_price FROM item_definitions WHERE item_id=? AND enabled=1");item.number(1,item_id);
    if(!item.next() || item.integer(0)<=0) throw std::runtime_error("item has no configured resale price");
    const auto price=item.integer(0);
    if(price>std::numeric_limits<std::int64_t>::max()/qty) throw std::runtime_error("sale overflow");
    Query owned(db,"SELECT COALESCE(SUM(quantity),0) FROM inventory WHERE character_eid=? AND item_id=?");
    owned.number(1,eid);owned.number(2,item_id);
    if(!owned.next() || owned.integer(0)<qty) throw std::runtime_error("not enough item quantity");
    Query slots(db,"SELECT slot,quantity FROM inventory WHERE character_eid=? AND item_id=? ORDER BY slot");
    slots.number(1,eid);slots.number(2,item_id);
    int remaining=qty;
    while(slots.next() && remaining>0) {
        const int have=static_cast<int>(slots.integer(1));
        const int use=std::min(remaining,have);
        if(use==have) {
            Query del(db,"DELETE FROM inventory WHERE character_eid=? AND slot=?");
            del.number(1,eid);del.number(2,slots.integer(0));del.run();
        } else {
            Query upd(db,"UPDATE inventory SET quantity=quantity-? WHERE character_eid=? AND slot=?");
            upd.number(1,use);upd.number(2,eid);upd.number(3,slots.integer(0));upd.run();
        }
        remaining-=use;
    }
    if(remaining) throw std::runtime_error("inventory changed during transaction");
    Query cleanup(db,"DELETE FROM inventory WHERE character_eid=? AND quantity=0");cleanup.number(1,eid);cleanup.run();
    change_gold(db,eid,price*qty,"NPC shop sale");
    tx.commit();
}

std::vector<PortalOffer> Gameplay::nearby_portals(std::int64_t eid,double radius) {
    if(!std::isfinite(radius) || radius<=0 || radius>3000) throw std::invalid_argument("invalid portal radius");
    auto c=db_.get_character(eid);if(!c) throw std::invalid_argument("character not found");
    Query p(db_.native_handle(),"SELECT portal_id,target_map,min_level,x,y,z FROM portals WHERE source_map=? AND enabled=1");
    p.text(1,c->map_name);
    std::vector<PortalOffer> result;
    while(p.next()) {
        double d=distance3(c->x,c->y,c->z,p.decimal(3),p.decimal(4),p.decimal(5));
        if(d<=radius) result.push_back({p.integer(0),p.str(1),static_cast<int>(p.integer(2)),d});
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.distance<b.distance;});
    return result;
}

void Gameplay::travel(std::int64_t eid,std::int64_t portal_id) {
    auto* db=db_.native_handle();Transaction tx(db);
    auto c=db_.get_character(eid);if(!c) throw std::invalid_argument("character not found");
    Query p(db,R"SQL(
       SELECT source_map,target_map,x,y,z,target_x,target_y,target_z,min_level
       FROM portals WHERE portal_id=? AND enabled=1
    )SQL");
    p.number(1,portal_id);if(!p.next()) throw std::runtime_error("portal disabled or missing");
    if(c->map_name!=p.str(0) || distance3(c->x,c->y,c->z,p.decimal(2),p.decimal(3),p.decimal(4))>25.)
        throw std::runtime_error("portal is too far or on a different map");
    if(c->level<p.integer(8)) throw std::runtime_error("minimum portal level not met");
    Query map(db,"SELECT enabled FROM map_definitions WHERE map_name=? COLLATE NOCASE");map.text(1,p.str(1));
    if(!map.next() || map.integer(0)!=1) throw std::runtime_error("target map disabled or not imported");
    Query update(db,"UPDATE characters SET map_name=?,x=?,y=?,z=?,updated_at=CURRENT_TIMESTAMP WHERE eid=?");
    update.text(1,p.str(1));update.real(2,p.decimal(5));update.real(3,p.decimal(6));update.real(4,p.decimal(7));update.number(5,eid);update.run();
    tx.commit();
}

} // namespace karos
