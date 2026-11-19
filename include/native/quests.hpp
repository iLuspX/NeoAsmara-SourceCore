#pragma once
#include "scene.hpp"
#include "inventory.hpp"
namespace rosh {
struct QuestKillItems {
    std::vector<std::pair<std::uint32_t,std::int64_t>> ground, delivered;
    bool inventory_full{};
};
class QuestBook {
    Sqlite &db_;
    Inventory &inventory_;
    Json catalog_,policy_;
    std::uint32_t eid_;
    std::map<std::pair<int,int>,int> sent_;
    struct Goal {int kind{},group{},required{};std::vector<int> indexes;};
    std::vector<Goal> groups(const Json &quest) const;
    const Json &quest(int id) const;
    int validate(const Bytes &body,std::size_t length) const;
    void npc(const SceneEntities &entities,const Json &character,std::uint32_t actor,int type) const;
    bool seen(const std::string &event,const Bytes &body);
    void remember(const std::string &event,const Bytes &body);
    int cycle(int qid) const;
    std::map<int,int> values(int qid) const;
    std::int64_t owned(int item,double timestamp) const;
    void aggregate(int qid);
  public:
    QuestBook(Sqlite &db,Inventory &inventory,Content &content,std::uint32_t eid);
    static std::string local_window(double timestamp,int reset_hour);
    std::string mode(int qid) const;
    bool ever_completed(int qid) const;
    bool blocked(int qid,double timestamp) const;
    bool active(int qid) const;
    bool accept(const Bytes &body,const Json &character,const SceneEntities &entities,const std::string &event,double timestamp);
    bool talk(const Bytes &body,const Json &character,const SceneEntities &entities);
    Json complete(const Bytes &body,const Json &character,const SceneEntities &entities,const std::string &event,double timestamp);
    void synchronize(double timestamp);
    void kill(std::uint32_t npc);
    std::vector<std::pair<std::uint32_t,std::int64_t>> drops(std::uint32_t npc,double timestamp);
    QuestKillItems route_kill_items(const std::vector<std::pair<std::uint32_t,std::int64_t>>& rolled,
                                  const std::string& kill);
    bool abandon(int qid);
    Bytes list_packet() const;
    Bytes completed_packet(double timestamp) const;
    std::vector<Bytes> progress_updates();
    Json ui_state(int level,bool full,double timestamp) const;
};
}
