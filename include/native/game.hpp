#pragma once
#include "core.hpp"
#include "scene.hpp"
#include "dungeons.hpp"
#include "staff.hpp"
#include "loot.hpp"
#include "skills.hpp"
#include "quickslots.hpp"
#include "costumes.hpp"
#include "quests.hpp"
#include "casting.hpp"
#include "premium.hpp"
#include <set>

namespace rosh {
class Inventory;
class Consumables;
class Progression {
    std::vector<std::uint64_t> requirements_, thresholds_;

  public:
    Progression();
    int level(std::uint64_t experience) const;
    std::uint64_t threshold(int level) const;
    std::uint64_t requirement(int level) const;
};
Json calculated_stats(const Json &profiles, int profession, int level,
                      const Json &modifiers = Json::object());
Json equipped_traits(Content &content, Sqlite &db, std::uint32_t eid, double timestamp);
Json character_modifiers(Content &content, const Json &character, double timestamp);
Json native_buff_state(const Json& character,const Json& borrowed,const Json& item_links,double timestamp);
Bytes native_buff_packet(std::uint32_t eid,const Json& entries,bool add,double timestamp);
Json enchant_row(const Json &rows, int grade);
std::pair<int,int> attack_recovery(const Json&stats,const Json&character,std::uint64_t applied_damage);
std::pair<int,int> regeneration_recovery(const Json&stats,const Json&character);
Bytes native_character_stats_packet(const Bytes&carrier,const Json&base,const Json&total,double attack_multiplier=1.,double movement_multiplier=1.);
Json native_map_destinations(Content&content);
Json resolve_native_map(Content&content,const std::string&query);
class WorldSession {
    Content &content_;
    LegacyStore &store_;
    Session identity_;
    Sqlite leases_;
    Json character_, stats_;
    std::map<int, Bytes> templates_;
    std::string map_name_;
    std::string dungeon_group_;
    std::unique_ptr<DungeonStore> dungeons_;
    std::int64_t dungeon_event_after_{};
    double next_dungeon_poll_{};
    bool dungeon_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    void sync_dungeon(const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);

    std::string scene_map_name(const std::string&name,std::uint32_t id) const;
    std::string scene_map_name() const {return scene_map_name(map_name_,map_id_);}
    std::string map_scope() const;
    std::uint32_t map_id_{};
    bool loaded_{false}, position_guard_{true}, resolve_native_arrival_{false};
    bool death_notice_sent_{false};
    bool revival_request(unsigned char cat,unsigned char sub,const Bytes& body,const std::string& event,const std::function<void(unsigned char,unsigned char,const Bytes&)>& send);
    bool mounted_{false}, mount_pending_{false};
    bool sitting_{};
    void stand(const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    bool appearance_refresh_pending_{false};
    std::uint32_t mount_uid_{};
    double mount_cooldown_{}, mount_pending_until_{};
    Bytes mount_action_;
    double mounted_speed();
    void clear_mount();
    bool mount_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    double next_renew_{};
    fs::path root_;
    double next_gm_poll_{};
    double next_scene_tick_{};
    double next_regeneration_{};
    std::map<int,std::pair<double,double>> mana_recovery_schedule_;
    void tick_mana_recovery(double timestamp,const std::function<void(unsigned char,unsigned char,const Bytes&)>& send);
    std::set<std::uint32_t> gm_visible_;
    std::shared_ptr<WorldScene> scene_;
    std::shared_ptr<const SceneEntities> scene_entities_;
    std::uint32_t mailbox_actor_{};
    std::int64_t mailbox_notified_{};
    std::string mailbox_scope_;
    std::uint32_t storage_actor_{};
    std::string storage_scope_;
    unsigned char storage_list_opcode_{};
    std::map<int,std::uint32_t> storage_seen_;
    std::set<std::string> shown_guild_invites_;
    std::string guild_revision_;
    std::string guild_revision();
    void save_guild_profile(unsigned flags,const std::string &message,const Json &names);
    void sync_guild(const std::function<void(unsigned char,unsigned char,const Bytes&)> &send, bool overlay=true);
    void poll_guild_invites(const std::function<void(unsigned char,unsigned char,const Bytes&)> &send);
    bool guild_request(unsigned char cat,unsigned char sub,const Bytes &body,
                       const std::function<void(unsigned char,unsigned char,const Bytes&)> &send,const std::string&event);
    std::set<std::uint32_t> scene_visible_;
    std::unique_ptr<Sqlite> roles_;
    std::string rendered_role_;
    std::unique_ptr<NpcShops> npc_shops_;
    std::unique_ptr<StaffSession> staff_;
    std::unique_ptr<StaffChannel> staff_channel_;
    std::int64_t staff_chat_after_{};
    std::int64_t guild_chat_after_{};
    std::int64_t guild_departure_after_{};
    std::int64_t guild_invite_result_after_{};
    void poll_guild_channel(const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    void poll_staff_channel(const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    bool vanished_{}, pk_mode_{};
    std::string scene_key() const {return identity_.id+":"+std::to_string(eid());}
    void publish_player(const std::function<void(unsigned char,unsigned char,const Bytes&)> &send);
    Progression progression_;
    std::unique_ptr<Inventory> inventory_;
    std::unique_ptr<Consumables> consumables_;
    std::unique_ptr<PremiumShop> premium_;
    bool premium_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    bool enchant_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    bool attack_potion_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    std::unique_ptr<GroundLoot> loot_;
    std::unique_ptr<SkillBook> skills_;
    std::unique_ptr<QuestBook> quests_;
    std::unique_ptr<ActiveSkills> casting_;
    Json received_party_auras_=Json::object();
    Json sent_native_buffs_=Json::object(),native_buff_links_;
    double next_native_buff_poll_{};
    void sync_native_buffs(const std::function<void(unsigned char,unsigned char,const Bytes&)>&send,bool force=false);
    Json active_party_auras(double timestamp)const;
    void refresh_party_auras(const std::function<void(unsigned char,unsigned char,const Bytes&)>&);
    bool active_skill_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    bool quest_snapshot_sent_{};
    std::string quest_full_cache_,quest_held_cache_;
    void quest_overlay(const Json&message,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    void sync_quests(const std::function<void(unsigned char,unsigned char,const Bytes&)>&send,bool full=false,bool initial=false);
    bool quest_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    void skill_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    bool super_skill_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    bool super_stone_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    struct PendingCraft { Bytes request; std::string event,map; Position position; double finish{}; };
    std::optional<PendingCraft> pending_craft_;
    bool crafting_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    void tick_crafting(const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    bool costume_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    std::map<std::uint32_t,std::uint32_t> ground_visible_;
    void sync_ground(const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    QuestKillItems commit_kill(Entity&mob);
    void profession_request(const Bytes&,const std::string&,
                            const std::function<void(unsigned char,unsigned char,const Bytes&)>&);
    void send_quest_kill_items(const QuestKillItems&,const std::function<void(unsigned char,unsigned char,const Bytes&)>&);
    std::map<int, Bytes> sent_items_, sent_equipment_;
    void persist();
    void claim();
    void release();
    void load_templates();
    Bytes appearance();
    Bytes enter_packet();
    Bytes gauge(int opcode);
    Bytes stats_packet();
    void refresh_self_appearance(const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    void recalculate();
    void staff_command(const std::vector<std::string>&args,const std::string&event,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    void staff_command_impl(const std::vector<std::string>&args,const std::string&event,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    void transition_map(const Json&destination,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send,bool internal=false);
    bool travel_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);
    bool mine_registration_request(unsigned char cat,unsigned char sub,const Bytes&body,
                                   const std::function<void(unsigned char,unsigned char,const Bytes&)>&send);

    Bytes consignment_query_;
    std::map<std::uint32_t,std::int64_t> consignment_quotes_;

  public:
    using Sender = std::function<void(unsigned char, unsigned char, const Bytes &)>;
    WorldSession(Content &content, LegacyStore &store, const Session &identity, const fs::path &root,
                 std::uint32_t eid,std::shared_ptr<WorldScene> scene = {});
    ~WorldSession();
    void enter(const Sender &send);
    bool handle(unsigned char category, unsigned char sub, const Bytes &body, const Sender &send,
                const std::string &event = "");
    void tick();
    void admin_tick(const Sender &send);
    void send_inventory(const Sender &send, bool snapshot);
    std::uint32_t eid() const {
        return character_.at("eid");
    }
    bool ready() const {
        return loaded_;
    }
    const Json &character() const {
        return character_;
    }
};
} // namespace rosh
