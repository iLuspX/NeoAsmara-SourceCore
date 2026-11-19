#include "native/entities.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace rosh {
double Position::distance(const Position &other) const {
    return std::hypot(x - other.x, y - other.y);
}
namespace {
double interpolate(const Json &points, int level) {
    level = std::clamp(level, 1, 120);
    for (std::size_t i = 1; i < points.size(); i++) {
        int low = points[i - 1][0], high = points[i][0];
        double a = points[i - 1][1], b = points[i][1];
        if (level <= high)
            return std::nearbyint(a + (b - a) * (level - low) / (high - low));
    }
    return points.back()[1];
}
const std::map<std::string, std::pair<double, double>> bounds = {{"respawn_seconds", {1, 86400}},
                                                                 {"evasion_percent", {0, 75}},
                                                                 {"respawn_jitter_seconds", {0, 120}},
                                                                 {"wander_radius", {0, 120}},
                                                                 {"spawn_range", {1, 600}},
                                                                 {"wander_speed", {1, 100}},
                                                                 {"wander_pause_min", {0, 60}},
                                                                 {"wander_pause_max", {0, 120}},
                                                                 {"active_radius", {150, 3000}},
                                                                 {"attack_cooldown_seconds", {.1, 30}},
                                                                 {"attack_windup_seconds", {0, 5}},
                                                                 {"reaction_seconds", {0, 30}}};
} // namespace
EntityCatalog::EntityCatalog(Content &content)
    : content_(content), spawns_(content.get("data/spawns.json")),
      npc_(content.get("data/native_npc_info.json").at("records")),
      observed_(content.get("server/npc_observed_stats.json")),
      respawns_(content.get("server/respawn_catalog.json")),
      spawn_policy_(content.get("server/spawn_rules.json")),
      progression_(content.get("server/mob_progression.json")),
      hp_overrides_(content.get("server/boss_hp_overrides.json")),
      prototypes_(content.get("server/native_npc_prototypes.json").at("maps")) {
    mob_profiles_ = content_.has("server/devtool_map_rules.json")
        ? content_.get("server/devtool_map_rules.json").value("mob_profiles", Json::object()) : Json::object();
    auto database = content_.database("database/content_typed.sqlite");
    for (const auto &row : database->query("SELECT npc_id,spawn_template FROM server_npc_templates")) {
        auto id = row.at("npc_id").get<std::uint32_t>();
        auto bytes = unhex(row.at("spawn_template"));
        if (bytes.size() == 254 && get32(bytes, 64) == id)
            templates_[id] = bytes;
    }
    for (const auto &row : database->query(
             "SELECT map_name,spawn_index,npc_id,entity_eid,x,y,z,spawn_template FROM server_npc_placements"))
        placements_[{row.at("map_name"), row.at("spawn_index")}] = row;
    // Exact capture templates remain preferred over the common native actor layout.
    auto captures = content_.get("server/mob_templates_verified.json").at("templates");
    for (auto it = captures.begin(); it != captures.end(); ++it) {
        Bytes body;
        if (it.value().is_string())
            body = unhex(it.value());
        else if (it.value().is_object() && it.value().contains("hex"))
            body = unhex(it.value().at("hex"));
        if (body.size() == 254 && get32(body, 64) == std::stoul(it.key()))
            templates_[static_cast<std::uint32_t>(std::stoul(it.key()))] = body;
    }
    // Recover only unambiguous, exact-type service NPC bindings from the supplied captures.
    auto map_list = content_.get("data/maplist.json");
    for (const auto &file : {"server/npc_placements_verified.json", "server/map_captures_428.json"})
        for (const auto &capture : content_.get(file)) {
            auto id = capture.at("npc_id").get<std::uint32_t>();
            auto key = std::to_string(id);
            if (!npc_.contains(key))
                continue;
            auto numeric = unhex(npc_.at(key).at("numeric_raw"));
            if (get32(numeric, 8) == 2 && id != 44444444)
                continue;
            auto body = unhex(capture.at("hex"));
            if (body.size() != 254 || get32(body, 64) != id ||
                get32(body, 2) != capture.at("eid").get<std::uint32_t>())
                continue;
            templates_.try_emplace(id, body);
            Position position{capture.at("position")[0], capture.at("position")[1],
                              capture.at("position")[2]};
            for (auto map = spawns_.begin(); map != spawns_.end(); ++map) {
                if (capture.contains("map_id")) {
                    bool matches = false;
                    for (const auto &record : map_list)
                        if (record.at("head")[1] == capture.at("map_id") && record.at("name") == map.key())
                            matches = true;
                    if (!matches)
                        continue;
                }
                std::vector<std::size_t> candidates;
                for (std::size_t index = 0; index < map.value().size(); index++) {
                    const auto &spawn = map.value()[index];
                    if (spawn.at("id") != id)
                        continue;
                    int tile = spawn.value("tile", 0);
                    Position native{spawn.at("x").get<double>() + tile % 64 * 5120,
                                    spawn.at("z").get<double>() + tile / 64 * 5120, spawn.at("y")};
                    if (native.distance(position) < 5 && std::abs(native.z - position.z) < 5)
                        candidates.push_back(index);
                }
                if (candidates.size() != 1)
                    continue;
                auto binding = std::make_pair(map.key(), candidates[0]);
                if (placements_.count(binding))
                    continue;
                placements_[binding] = {{"npc_id", id},    {"entity_eid", capture.at("eid")},
                                        {"x", position.x}, {"y", position.y},
                                        {"z", position.z}, {"spawn_template", capture.at("hex")}};
            }
        }
    auto defaults = checked(spawn_policy_.value("defaults", Json::object()), {{"respawn_seconds", 30.},
                                                                              {"respawn_jitter_seconds", 4.},
                                                                              {"wander_radius", 45.},
                                                                              {"spawn_range", 180.},
                                                                              {"wander_speed", 25.},
                                                                              {"wander_pause_min", 3.},
                                                                              {"wander_pause_max", 7.},
                                                                              {"active_radius", 1200.},
                                                                              {"attack_cooldown_seconds", 2.},
                                                                              {"attack_windup_seconds", .6},
                                                                              {"reaction_seconds", 3.}});
    spawn_policy_["defaults"] = defaults;
}
Json EntityCatalog::checked(const Json &overrides, const Json &base) const {
    auto result = base;
    for (auto entry = overrides.begin(); entry != overrides.end(); ++entry) {
        if(entry.key()=="damage_channel"){if(entry.value()!="physical"&&entry.value()!="magic")throw std::runtime_error("Invalid mob damage channel");result[entry.key()]=entry.value();continue;}
        if (!bounds.count(entry.key()) || !entry.value().is_number())
            throw std::runtime_error("Invalid spawn policy field");
        double value = entry.value();
        auto [low, high] = bounds.at(entry.key());
        if (!std::isfinite(value) || value < low || value > high)
            throw std::runtime_error("Spawn policy outside validated range");
        result[entry.key()] = value;
    }
    if (result.at("wander_pause_min").get<double>() > result.at("wander_pause_max").get<double>() ||
        result.at("wander_radius").get<double>() > result.at("spawn_range").get<double>())
        throw std::runtime_error("Inconsistent spawn radii or pause interval");
    return result;
}
Json EntityCatalog::rules(std::uint32_t id, bool combat, const std::string &map) const {
    auto result = spawn_policy_.at("defaults");
    auto key = std::to_string(id);
    auto record = respawns_.value(key, Json::object());
    auto category = record.value("category", "normal");
    if (combat && spawn_policy_.contains("respawn_by_category")) {
        if (category == "boss") {
            bool dungeon = record.value("dungeon_default", false);
            for (const auto &name : spawn_policy_.value("dungeon_maps", Json::array()))
                if (name == map)
                    dungeon = true;
            if (dungeon)
                result = checked(spawn_policy_.value("dungeon_defaults", Json::object()), result);
            else if (spawn_policy_.contains("boss_difficulty_bands")) {
                auto bands = spawn_policy_.at("boss_difficulty_bands");
                auto selected = bands.back();
                int previous = -1;
                for (const auto &band : bands) {
                    int max = band.at("max_level");
                    double hours = band.at("hours");
                    if (max <= previous || hours < 2 || hours > 12)
                        throw std::runtime_error("Invalid boss respawn band");
                    previous = max;
                    if (record.value("level", 1) <= max) {
                        selected = band;
                        break;
                    }
                }
                result["respawn_seconds"] =
                    (record.value("neo", false) ? 12. : selected.at("hours").get<double>()) * 3600;
                result["respawn_jitter_seconds"] = 0.;
            }
        } else {
            auto categories = spawn_policy_.at("respawn_by_category");
            result["respawn_seconds"] = categories.value(
                category, categories.value("normal", result.at("respawn_seconds").get<double>()));
            result["respawn_jitter_seconds"] = 0.;
        }
    }
    auto overrides = spawn_policy_.value("npc_overrides", Json::object()).value(key, Json::object());
    result = checked(overrides, result);
    if (!combat)
        result["wander_radius"] = 0.;
    return result;
}
int EntityCatalog::health(std::uint32_t id, int level, int observed) const {
    auto key = std::to_string(id);
    if (hp_overrides_.contains(key))
        return hp_overrides_.at(key);
    auto category = respawns_.value(key, Json::object()).value("category", "normal");
    return std::max(observed, static_cast<int>(std::nearbyint(
                                  interpolate(progression_.at("hp_by_level"), level) *
                                  progression_.at("hp_multipliers").at(category).get<double>())));
}
int EntityCatalog::damage(std::uint32_t id, int level, double roll) const {
    auto category = respawns_.value(std::to_string(id), Json::object()).value("category", "normal");
    auto variance = progression_.at("damage_variance");
    return std::max(1,
                    static_cast<int>(std::nearbyint(
                        interpolate(progression_.at("damage_by_level"), level) *
                        progression_.at("damage_multipliers").at(category).get<double>() *
                        (variance[0].get<double>() + (variance[1].get<double>() - variance[0].get<double>()) *
                                                         std::clamp(roll, 0., 1.)))));
}
bool EntityCatalog::event_level_allowed(std::uint32_t id, int mob_level, int player_level) const {
    return respawns_.value(std::to_string(id), Json::object()).value("native_effect", 0) != 97 ||
           (player_level >= std::max(1, mob_level - 5) && player_level <= mob_level + 15);
}
void EntityCatalog::apply_mob_edit(Entity &entity, const Json &edit) const {
    entity.aggro = std::clamp(edit.value("aggro", entity.aggro), 0., 600.);
    if (edit.contains("hp"))
        entity.hp = entity.maxhp = std::clamp(edit.at("hp").get<int>(), 1, 1000000000);
    entity.rules = checked(edit.value("behavior", Json::object()), entity.rules);
    if (edit.contains("xp"))
        entity.rules["dev_xp"] = std::clamp(edit.at("xp").get<int>(), 0, 100000000);
    if (edit.contains("drops"))
        entity.rules["dev_drops"] = edit.at("drops");
}
std::map<std::uint32_t, Entity> EntityCatalog::load(const std::string &map,
                                                    const std::set<std::uint32_t> &reserved) {
    std::map<std::uint32_t, Entity> result;
    std::string source_map=map;
    auto separator=source_map.find('#');
    if(separator!=std::string::npos){auto instance=source_map.find('#',separator+1);if(instance!=std::string::npos)source_map.resize(instance);if(!spawns_.contains(source_map))source_map.resize(separator);}
    if(!spawns_.contains(source_map)){
        for(auto it=spawns_.begin();it!=spawns_.end();++it)if(_stricmp(it.key().c_str(),map.c_str())==0){source_map=it.key();break;}
        if(map=="Siege_002"&&spawns_.contains("Siege_"))source_map="Siege_";
    }
    if (!spawns_.contains(source_map))
        return result;
    Json dungeon=Json::object();
    if(map.find("#run:")!=std::string::npos&&content_.has("server/dungeon_rules.json")){
        const auto first=map.find('#'),last=map.find('#',first+1);
        dungeon=content_.get("server/dungeon_rules.json").at("maps").value(map.substr(first+1,last-first-1),Json::object());
    }
    // Low IDs already belong to the client's map clone prototypes.
    std::uint32_t next = std::max<std::uint32_t>(1500,prototype_count(source_map)+100);
    std::size_t index = 0;
    std::vector<std::pair<std::uint32_t, int>> expanded_spots;
    for (const auto &source : spawns_.at(source_map)) {
        auto current = index++;
        std::uint32_t id = source.at("id");
        auto key = std::to_string(id);
        int tile = source.value("tile", 0);
        if (tile < 0 || !npc_.contains(key))
            continue;
        const auto &info = npc_.at(key);
        auto numeric = unhex(info.at("numeric_raw"));
        Entity entity;
        entity.npc_id = id;
        entity.map = source_map;
        entity.name = info.value("name", "");
        const auto kind=get32(numeric,8);
        // Lupinel objective actors are visible before the battle, but the event
        // must explicitly activate combat. Do not run ordinary mob AI on them.
        const bool lupinel_objective = source_map == "asmara" && (id == 11003 || id == 11004);
        entity.combat = kind == 2 && !lupinel_objective;
        // Service guards are distinct from hostile monsters named "Orc Guard".
        static const std::set<std::uint32_t> city_guards={9200191,9200202,9300211,9350005,9350006,9350016};
        entity.guard=kind==1&&city_guards.count(id);
        if((kind==1||kind==2)&&!prototype_reference(source_map,id))continue;
        entity.level = std::max(1, info.value("level", 1));
        entity.aggro = std::max(0., info.value("aggro", 0.));
        entity.attack_range = std::max(1., info.value("atk_range", 20.));
        entity.spawn_index = current;
        entity.home = {source.at("x").get<double>() + tile % 64 * 5120,
                       source.at("z").get<double>() + tile / 64 * 5120, source.at("y")};
        if (!std::isfinite(entity.home.x) || !std::isfinite(entity.home.y) || !std::isfinite(entity.home.z))
            continue;
        if (id == 44444444 && source.value("model", "") == "NPC_021_DoorGuard") {
            entity.combat = false;
            entity.guard = true;
        }
        auto observed = observed_.value(key, Json::object());
        if (observed.contains("level"))
            entity.level = observed.at("level");
        entity.maxhp =
            entity.combat || entity.guard ? health(id, entity.level, observed.value("maxhp", 0)) : observed.value("maxhp", 1);
        if (hp_overrides_.contains(key))
            entity.maxhp = hp_overrides_.at(key);
        if(lupinel_objective)entity.maxhp=100;
        entity.hp = entity.maxhp;
        entity.category = respawns_.value(key, Json::object()).value("category", "normal");
        auto binding = placements_.find({source_map, current});
        if (binding != placements_.end()) {
            const auto &row = binding->second;
            Position captured{row.at("x"), row.at("y"), row.at("z")};
            auto bytes = unhex(row.at("spawn_template"));
            if (row.at("npc_id") == id && captured.distance(entity.home) < 5 &&
                std::abs(captured.z - entity.home.z) < 5 && bytes.size() == 254 && get32(bytes, 64) == id) {
                entity.carrier = bytes;
                entity.combat = false;
                // A capture's actor ID is not an identity for this session.
            }
        }
        if (!entity.eid || reserved.count(entity.eid) || result.count(entity.eid)) {
            do {
                next++;
            } while (reserved.count(next) || result.count(next));
            if (next > 65535)
                throw std::runtime_error("Map exceeds native entity capacity");
            entity.eid = next;
        }
        if (entity.carrier.empty() && templates_.count(id))
            entity.carrier = templates_.at(id);
        entity.position = entity.home;
        entity.rules = rules(id, entity.combat || entity.guard, source_map);
        if(map.find("#run:")!=std::string::npos||source_map=="Antcave"||source_map=="Centri_Hale")entity.rules["vertical_range"]=80.;
        if(source.contains("native_reference"))entity.rules["native_reference"]=source.at("native_reference");
        if(!dungeon.empty()&&entity.combat){
            entity.rules["no_respawn"]=true;
            const auto active=dungeon.value("initial_active_references",Json::array());
            const auto reference=source.value("native_reference",0u);
            if(source.value("native_initially_hidden",false)&&std::find(active.begin(),active.end(),reference)==active.end())
                entity.rules["dungeon_dormant"]=true;
        }
        if(entity.guard){entity.aggro=350;entity.attack_range=50;entity.rules["wander_radius"]=0.;entity.rules["spawn_range"]=600.;}
        auto overrides = spawn_policy_.value("npc_overrides", Json::object()).value(key, Json::object());
        if (entity.combat && !overrides.contains("spawn_range"))
            entity.rules["spawn_range"] =
                std::max(entity.rules.at("spawn_range").get<double>(),
                         std::min(600., std::max(350., entity.aggro + entity.attack_range)));
        if (source.contains("dev")) {
            const auto &edit = source.at("dev");
            if (!entity.combat && !entity.guard)
                entity.rules["dev_always_visible"] = edit.value("always_visible", false);
            if (entity.combat && !entity.guard)
                apply_mob_edit(entity, edit);
        }
        if (entity.combat && !entity.guard && mob_profiles_.contains(key))
            apply_mob_edit(entity, mob_profiles_.at(key));
        if (entity.combat && !entity.guard && source.contains("dev")) {
            const auto count = std::clamp(source.at("dev").value("quantity", 1), 1, 30);
            if (count > 1)
                expanded_spots.emplace_back(entity.eid, count);
        }
        result.emplace(entity.eid, std::move(entity));
    }
    // Allocate additional actors after the originals to preserve their IDs and respawn records.
    for (const auto &[eid, count] : expanded_spots) {
        const auto original = result.at(eid);
        for (int member = 1; member < count; ++member) {
            do {
                ++next;
            } while (reserved.count(next) || result.count(next));
            if (next > 65535)
                throw std::runtime_error("Map exceeds native entity capacity");
            Entity extra = original;
            extra.eid = next;
            extra.spawn_index = 10000000ULL + original.spawn_index * 30 + member;
            result.emplace(extra.eid, std::move(extra));
        }
    }
    return result;
}
Entity EntityCatalog::summoned(std::uint32_t type,std::uint32_t eid,const std::string&map,const Position&position){
 if(!prototype_reference(map,type))throw std::runtime_error("Este modelo de mob nao esta carregado neste mapa pelo client");
 auto key=std::to_string(type);if(!npc_.contains(key))throw std::runtime_error("Tipo nativo de mob desconhecido");auto info=npc_.at(key);auto numeric=unhex(info.at("numeric_raw"));if(get32(numeric,8)!=2)throw std::runtime_error("Somente mobs de combate podem ser invocados");
 Entity entity;entity.eid=eid;entity.npc_id=type;entity.map=map;entity.name=info.value("name","");entity.combat=true;entity.level=std::max(1,info.value("level",1));entity.aggro=std::max(0.,info.value("aggro",0.));entity.attack_range=std::max(1.,info.value("atk_range",20.));entity.category=respawns_.value(key,Json::object()).value("category","normal");entity.maxhp=health(type,entity.level);entity.hp=entity.maxhp;entity.home=entity.position=position;entity.rules=rules(type,true,map);entity.spawn_index=1000000000ULL+eid;if(templates_.count(type))entity.carrier=templates_.at(type);
 if(mob_profiles_.contains(key))apply_mob_edit(entity,mob_profiles_.at(key));
 return entity;
}
Bytes EntityCatalog::appearance(const Entity &entity) const {
    auto body = entity.carrier;
    if (body.empty()) {
        body.resize(254);
        body[1] = 1;
        put32(body, 22, entity.maxhp);
        put32(body, 26, entity.maxhp);
        putfloat(body, 38, 40);
        putfloat(body, 42, 10);
        putfloat(body, 46, .87f);
        put16(body, 50, static_cast<std::uint16_t>(std::clamp(entity.level, 1, 65535)));
        put32(body, 64, entity.npc_id);
    }
    if (body.size() != 254 || body[0] != 0 || body[1] != 1 || get32(body, 64) != entity.npc_id)
        throw std::runtime_error("Native entity carrier identity mismatch");
    put32(body, 2, entity.eid);
    put32(body, 68, entity.rules.value("native_reference",prototype_reference(entity.map,entity.npc_id)));
    putfloat(body, 6, static_cast<float>(entity.position.x));
    putfloat(body, 10, static_cast<float>(entity.position.y));
    putfloat(body, 14, static_cast<float>(entity.position.z));
    if (entity.combat) {
        if (getfloat(body, 38) <= 0)
            putfloat(body, 38, 40);
        if (getfloat(body, 42) <= 0)
            putfloat(body, 42, 10);
        if (getfloat(body, 46) <= 0)
            putfloat(body, 46, .87f);
        put32(body, 22, entity.hp);
        put32(body, 26, entity.maxhp);
        put16(body, 50, static_cast<std::uint16_t>(std::clamp(entity.level, 1, 65535)));
    }
    return body;
}
std::uint32_t EntityCatalog::prototype_reference(const std::string&map,std::uint32_t type)const{
 auto key=map;auto split=key.find('#');if(split!=std::string::npos){auto second=key.find('#',split+1);if(second!=std::string::npos)key.resize(second);if(!prototypes_.contains(key))key.resize(split);}
 for(auto it=prototypes_.begin();it!=prototypes_.end();++it)if(_stricmp(it.key().c_str(),key.c_str())==0)return it.value().at("references").value(std::to_string(type),0u);
 return 0;
}
std::uint32_t EntityCatalog::prototype_count(const std::string&map)const{
 auto key=map;auto split=key.find('#');if(split!=std::string::npos){auto second=key.find('#',split+1);if(second!=std::string::npos)key.resize(second);if(!prototypes_.contains(key))key.resize(split);}
 for(auto it=prototypes_.begin();it!=prototypes_.end();++it)if(_stricmp(it.key().c_str(),key.c_str())==0)return it.value().value("prototype_count",0u);
 return 0;
}
double EntityCatalog::simulation_interval()const{
 auto hz=spawn_policy_.value("simulation_hz",10.);if(!std::isfinite(hz)||hz<1||hz>10)throw std::runtime_error("Simulation frequency must be 1..10 Hz");return 1/hz;
}
double MobSimulation::uniform(double low, double high) {
    return std::uniform_real_distribution<double>(low, high)(random_);
}
std::optional<EntityEvent> MobSimulation::step(Entity &entity, const Position &to, double dt, double speed) {
    auto from = entity.position;
    double distance = from.distance(to);
    if (distance < 1 || dt <= 0)
        return {};
    const bool running=entity.state==MobState::Chase||entity.state==MobState::Returning;
    // Native 04/02 carries running/walking rates at +38/+42. The former
    // fixed140 server rate disagreed with the walking rate sent to the client.
    const float native_speed=entity.carrier.size()==254?getfloat(entity.carrier,running?38:42):(running?32.f:11.083333f);
    const double rate=std::isfinite(native_speed)&&native_speed>0?native_speed:(running?32.:11.083333);
    speed=speed<0?rate:std::min(speed,rate);
    double ratio = std::min(distance, speed * dt) / distance;
    entity.position.x += (to.x - from.x) * ratio;
    entity.position.y += (to.y - from.y) * ratio;
    entity.position.z += (to.z - from.z) * ratio;
    entity.angle = std::atan2(to.y - from.y, to.x - from.x);
    return EntityEvent{EntityEvent::Move, entity.eid, 0, from, 0, static_cast<unsigned char>(running?1:0),static_cast<float>(speed)};
}
std::optional<EntityEvent> MobSimulation::return_home(Entity &entity, double dt) {
    entity.target = 0;
    entity.impact_at = 0;
    double distance = entity.position.distance(entity.home);
    auto event = step(entity, entity.home, dt);
    if (entity.position.distance(entity.home) < 1) {
        entity.position = entity.home;
        entity.state = MobState::Idle;
        entity.hp = entity.maxhp;
        entity.damage.clear();
        entity.damage_names.clear();entity.party_damage.clear();entity.party_damage_names.clear();
        entity.party_contributors.clear();entity.loot_owner=0;entity.loot_recipients.clear();
        entity.debuffs = Json::object();
        entity.hp_dirty = true;
        entity.wander.reset();
        entity.pause_until = 0;
    }
    return event;
}
// Only passive detection uses the level gap. Direct hits still provoke mobs.
static bool detects_player_level(const Entity &entity, const Observer &player) {
    return entity.guard || static_cast<std::int64_t>(player.level) - entity.level < 25;
}
void MobSimulation::provoke(Entity &entity, const Observer &observer) {
    if (entity.alive && entity.combat &&
        (entity.state == MobState::Idle || entity.state == MobState::Returning) && observer.ready && !observer.hidden &&
        observer.hp > 0 && std::abs(observer.position.z-entity.position.z)<=entity.rules.value("vertical_range",1e15) &&
        observer.position.distance(entity.home) <= entity.rules.at("spawn_range").get<double>()) {
        entity.detected_target = 0;
        entity.target = observer.eid;
        entity.state = MobState::Chase;
        entity.wander.reset();
        entity.impact_at = 0;
    }
}
std::vector<EntityEvent> MobSimulation::tick(std::map<std::uint32_t, Entity> &entities,
                                             const std::map<std::uint32_t, Observer> &observers,
                                             double timestamp) {
    double elapsed = timestamp - last_;
    bool update = elapsed >= interval_-1e-9;
    if (update)
        last_ = timestamp;
    double dt = update ? std::clamp(elapsed, 0., .8) : 0;
    std::vector<EntityEvent> events;
    auto publish = [&](const std::optional<EntityEvent> &event) {
        if (event)
            events.push_back(*event);
    };
    auto cancel_detection = [&](Entity &entity) {
        if (!entity.detected_target)
            return;
        entity.detected_target = 0;
        events.push_back({EntityEvent::Disengage, entity.eid, 0, entity.position, 0});
    };
    auto return_to_spawn = [&](Entity &entity) {
        cancel_detection(entity);
        if(entity.aggro_notice_sent){entity.aggro_notice_sent=false;events.push_back({EntityEvent::Disengage,entity.eid,0,entity.position,0});}
        publish(return_home(entity, dt));
        if (entity.state == MobState::Idle)
            events.push_back({EntityEvent::Reset, entity.eid, 0, entity.position, 0});
    };
    // A player can have only one pending proximity reaction in this map.
    // Keep a reservation through the tick that starts the chase, so another
    // mob begins its own reaction window on a later tick.
    std::map<std::uint32_t, std::uint32_t> reacting;
    if (update) {
        for (auto &[id, entity] : entities) {
            if (!entity.alive || entity.state != MobState::Idle || !entity.detected_target)
                continue;
            const auto target = observers.find(entity.detected_target);
            const double range = entity.rules.at("spawn_range");
            if (target != observers.end() && target->second.ready && !target->second.hidden &&
                target->second.hp > 0 && detects_player_level(entity, target->second) &&
                entity.position.distance(target->second.position) <= entity.aggro &&
                target->second.position.distance(entity.home) <= range && entity.position.distance(entity.home) <= range &&
                reacting.emplace(entity.detected_target, id).second)
                continue;
            cancel_detection(entity);
        }
    }
    for (auto &[id, entity] : entities) {
        if (!entity.combat&&!entity.guard)
            continue;
        if(entity.rules.value("dungeon_dormant",false))continue;
        if (!entity.alive) {
            if (!entity.rules.value("no_respawn",false) && entity.respawn_at > 0 && timestamp >= entity.respawn_at) {
                entity.position = entity.home;
                entity.hp = entity.maxhp;
                entity.alive = true;
                ++entity.life_generation;
                entity.state = MobState::Idle;
                entity.target = 0;
                entity.wander.reset();
                entity.pause_until = timestamp + 1;
                entity.last_attack = 0;
                entity.impact_at = 0;
                entity.damage.clear();
                entity.damage_names.clear();entity.party_damage.clear();entity.party_damage_names.clear();
                entity.party_contributors.clear();entity.loot_owner=0;entity.loot_recipients.clear();
                entity.debuffs = Json::object();
                entity.stun_stop_sent = false;
                entity.aggro_notice_sent = false;
                entity.detected_target = 0;
                entity.respawn_at = 0;
                events.push_back({EntityEvent::Respawn, id, 0, entity.position, 0});
            }
            continue;
        }
        if (!update)
            continue;
        double range = entity.rules.at("spawn_range");
        if (entity.state == MobState::Returning || entity.position.distance(entity.home) > range) {
            entity.state = MobState::Returning;
            return_to_spawn(entity);
            continue;
        }
        if(entity.aggro_notice_sent && !entity.target){
            entity.aggro_notice_sent=false;
            events.push_back({EntityEvent::Disengage,id,0,entity.position,0});
        }
        bool stunned = false;
        std::uint32_t forced = 0;
        double newest = 0;
        for (const auto &effect : entity.debuffs) {
            if (effect.value("expires", 0.) <= timestamp)
                continue;
            std::set<int> operations;
            for (const auto &row : effect.at("effects")) {
                int operation = row.at("operation");
                operations.insert(operation);
                if (operation == 25 && row.at("flat").get<double>() > 0 &&
                    effect.at("expires").get<double>() > newest) {
                    forced = effect.value("caster_eid", 0u);
                    newest = effect.at("expires");
                }
            }
            if (operations.count(85) && operations.count(86) && operations.count(87))
                stunned = true;
        }
        auto taunt = observers.find(forced);
        if (!entity.guard && taunt != observers.end() && taunt->second.hp > 0 && taunt->second.ready &&
            !taunt->second.hidden && taunt->second.position.distance(entity.home) <= range &&
            entity.target != forced) {
            if (entity.detected_target) {
                reacting.erase(entity.detected_target);
                entity.detected_target = 0;
            }
            entity.target = forced;
            entity.state = MobState::Chase;
            entity.impact_at = 0;
            entity.wander.reset();
        }
        if (stunned) {
            if (entity.detected_target) {
                reacting.erase(entity.detected_target);
                cancel_detection(entity);
            }
            entity.impact_at = 0;
            if (!entity.stun_stop_sent) {
                entity.stun_stop_sent = true;
                events.push_back({EntityEvent::Stop, id, 0, entity.position, 0});
            }
            continue;
        }
        entity.stun_stop_sent = false;
        if (entity.state == MobState::Idle) {
            const double reaction = entity.guard ? 0. : entity.rules.value("reaction_seconds", 3.);
            const Observer *nearest = nullptr;
            double distance = std::numeric_limits<double>::max();
            bool active = false;
            for (const auto &[pid, player] : observers) {
                if (!player.ready || player.hidden || player.hp <= 0 || std::abs(player.position.z-entity.position.z)>entity.rules.value("vertical_range",1e15))
                    continue;
                if(entity.guard&&player.moral>=0)continue;
                double d = entity.position.distance(player.position);
                if (d <= entity.rules.at("active_radius").get<double>())
                    active = true;
                if (!detects_player_level(entity, player))
                    continue;
                if (reaction > 0) {
                    const auto reservation = reacting.find(pid);
                    if ((reservation != reacting.end() && reservation->second != id) ||
                        (entity.detected_target && entity.detected_target != pid))
                        continue;
                }
                const double acquisition = entity.guard ? entity.attack_range : entity.aggro;
                if (acquisition > 0 && d <= acquisition && player.position.distance(entity.home) <= range &&
                    (d < distance || (d == distance && nearest && pid < nearest->eid))) {
                    nearest = &player;
                    distance = d;
                }
            }
            if (nearest) {
                if (reaction > 0) {
                    reacting.emplace(nearest->eid, id);
                    if (!entity.detected_target) {
                        entity.detected_target = nearest->eid;
                        entity.react_at = timestamp + reaction;
                        entity.wander.reset();
                        events.push_back({EntityEvent::Detection, id, nearest->eid, entity.position, 0});
                        events.push_back({EntityEvent::Stop, id, 0, entity.position, 0});
                    }
                    if (timestamp < entity.react_at) continue;
                }
                entity.detected_target = 0;
                entity.target = nearest->eid;
                entity.state = entity.guard ? MobState::Attack : MobState::Chase;
                entity.wander.reset();
                if(!entity.guard&&!entity.aggro_notice_sent){entity.aggro_notice_sent=true;events.push_back({EntityEvent::Aggro,id,nearest->eid,entity.position,0});}
                events.push_back({EntityEvent::Stop, id, 0, entity.position, 0});
                continue;
            }
            cancel_detection(entity);
            if (entity.guard || !active) {
                entity.wander.reset();
                continue;
            }
            double radius = std::min(range, entity.rules.at("wander_radius").get<double>());
            if (radius <= 0)
                continue;
            if (!entity.wander) {
                if (timestamp < entity.pause_until)
                    continue;
                double angle = uniform(0, 6.283185307179586), r = radius * std::sqrt(uniform(0, 1));
                entity.wander = Position{entity.home.x + std::cos(angle) * r,
                                         entity.home.y + std::sin(angle) * r, entity.position.z};
            }
            if (entity.position.distance(*entity.wander) < 6) {
                entity.wander.reset();
                entity.pause_until = timestamp + uniform(entity.rules.at("wander_pause_min"),
                                                         entity.rules.at("wander_pause_max"));
                events.push_back({EntityEvent::Stop, id, 0, entity.position, 0});
            } else
                publish(step(entity, *entity.wander, dt, entity.rules.at("wander_speed")));
            continue;
        }
        auto target = observers.find(entity.target);
        if (target == observers.end() || target->second.hp <= 0 || target->second.hidden ||
            !target->second.ready || std::abs(target->second.position.z-entity.position.z)>entity.rules.value("vertical_range",1e15) || (entity.guard&&target->second.moral>=0) || target->second.position.distance(entity.home) > range) {
            entity.state = MobState::Returning;
            return_to_spawn(entity);
            continue;
        }
        const auto &player = target->second;
        if(!entity.guard&&entity.state==MobState::Chase&&!entity.aggro_notice_sent){entity.aggro_notice_sent=true;events.push_back({EntityEvent::Aggro,id,player.eid,entity.position,0});}
        double distance = entity.position.distance(player.position);
        if (entity.guard && distance > entity.attack_range) {
            entity.target = 0;
            entity.impact_at = 0;
            entity.state = MobState::Idle;
            if(entity.aggro_notice_sent){entity.aggro_notice_sent=false;events.push_back({EntityEvent::Disengage,id,0,entity.position,0});}
            events.push_back({EntityEvent::Stop, id, 0, entity.position, 0});
            continue;
        }
        if (entity.state == MobState::Chase) {
            if (distance <= entity.attack_range) {
                entity.state = MobState::Attack;
                events.push_back({EntityEvent::Stop, id, 0, entity.position, 0});
                continue;
            }
            double reach = std::max(1., entity.attack_range * .85),
                   ratio = std::max(0., (distance - reach) / std::max(distance, .001));
            publish(
                step(entity,
                     {entity.position.x + (player.position.x - entity.position.x) * ratio,
                      entity.position.y + (player.position.y - entity.position.y) * ratio, player.position.z},
                     dt));
            continue;
        }
        if (entity.state == MobState::Attack) {
            if (distance > entity.attack_range) {
                const bool winding_up = entity.impact_at > 0;
                if (winding_up) {
                    entity.impact_at = 0;
                    events.push_back({EntityEvent::Stop, id, 0, entity.position, 0});
                }
                entity.state = MobState::Chase;
                continue;
            }
            if (entity.impact_at) {
                if (timestamp >= entity.impact_at) {
                    entity.impact_at = 0;
                    events.push_back({EntityEvent::AttackImpact, id, player.eid, entity.position,
                                      catalog_.damage(entity.npc_id, entity.level, uniform(0, 1))});
                }
                continue;
            }
            if (timestamp - entity.last_attack >= entity.rules.at("attack_cooldown_seconds").get<double>()) {
                entity.last_attack = timestamp;
                double windup = entity.rules.at("attack_windup_seconds");
                if (windup > 0) {
                    entity.impact_at = timestamp + windup;
                    events.push_back({EntityEvent::AttackStart, id, player.eid, entity.position, 0});
                } else
                    events.push_back({EntityEvent::AttackImpact, id, player.eid, entity.position,
                                      catalog_.damage(entity.npc_id, entity.level, uniform(0, 1))});
            }
        }
    }
    return events;
}
void SpawnPersistence::initialize(Sqlite &db) {
    db.exec("CREATE TABLE IF NOT EXISTS spawn_deaths(map_name TEXT NOT NULL,spawn_index INTEGER NOT "
            "NULL,npc_id INTEGER NOT NULL,kill_id TEXT NOT NULL,died_at REAL NOT NULL,respawn_at REAL NOT "
            "NULL,PRIMARY KEY(map_name,spawn_index,npc_id))");
}
std::string SpawnPersistence::scope(const std::string &map, int channel, std::uint32_t id) {
    if (channel < 1 || channel > 4)
        throw std::runtime_error("Invalid public channel");
    const std::set<std::uint32_t> special = {1900329, 1900349, 1900339, 1900308, 1900309, 1900319,
                                             9000000, 9003555, 9005675, 9007695, 9009600, 9000199};
    if (special.count(id))
        return "special:" + std::to_string(id) + ":" + map;
    return "public:" + std::to_string(channel) + ":" + map;
}
static void restore_death(Entity &entity, const Json &row) {
    entity.alive = false;
    entity.hp = 0;
    entity.state = MobState::Dead;
    entity.target = 0;
    entity.impact_at = 0;
    entity.respawn_at = row.at("respawn_at");
}
void SpawnPersistence::restore(Sqlite &db, const std::string &scope, Entity &entity, double timestamp) {
    if (!entity.combat && !entity.guard)
        return;
    auto rows = db.query("SELECT respawn_at FROM spawn_deaths WHERE map_name=? AND spawn_index=? AND "
                         "npc_id=? AND respawn_at>?",
                         {scope, std::int64_t(entity.spawn_index), std::int64_t(entity.npc_id), timestamp});
    if (!rows.empty())
        restore_death(entity, rows[0]);
}
bool SpawnPersistence::claim(Sqlite &db, const std::string &scope, Entity &entity, const std::string &kill,
                             double timestamp, double roll) {
    if (!entity.combat && !entity.guard)
        return true;
    if (kill.empty() || kill.size() > 128 || !std::isfinite(timestamp) || !std::isfinite(roll) || roll < 0 ||
        roll > 1)
        throw std::runtime_error("Invalid spawn death claim");
    auto rows =
        db.query("SELECT respawn_at FROM spawn_deaths WHERE map_name=? AND spawn_index=? AND npc_id=?",
                 {scope, std::int64_t(entity.spawn_index), std::int64_t(entity.npc_id)});
    if (!rows.empty() && rows[0].at("respawn_at").get<double>() > timestamp) {
        restore_death(entity, rows[0]);
        return false;
    }
    double deadline = entity.rules.value("no_respawn",false)?1e15:timestamp + entity.rules.at("respawn_seconds").get<double>() +
                      entity.rules.at("respawn_jitter_seconds").get<double>() * roll;
    db.exec(
        "INSERT INTO spawn_deaths VALUES(?,?,?,?,?,?) ON CONFLICT(map_name,spawn_index,npc_id) DO UPDATE SET "
        "kill_id=excluded.kill_id,died_at=excluded.died_at,respawn_at=excluded.respawn_at",
        {scope, std::int64_t(entity.spawn_index), std::int64_t(entity.npc_id), kill, timestamp, deadline});
    entity.respawn_at = deadline;
    return true;
}

} // namespace rosh
