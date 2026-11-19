#include "native/game.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace rosh {
Json character_modifiers(Content &content, const Json &character, double timestamp) {
    auto catalog = content.get("data/skill_catalog.json"),
         attributes = content.get("data/fletta_attributes.json");
    auto skills = catalog.at("skills"), trees = catalog.at("trees"), super_tree=catalog.value("super_tree",Json::object());
    int cls = character.at("cls");
    Json mods = Json::object();
    auto add = [&](int operation, double flat, double percent) {
        auto key = std::to_string(operation);
        if (!mods.contains(key))
            mods[key] = {0., 0.};
        mods[key][0] = mods[key][0].get<double>() + flat;
        mods[key][1] = mods[key][1].get<double>() + percent;
    };
    auto allowed = [&](int iid) {
        const auto super_key=std::to_string(iid);
        if(super_tree.contains(super_key)&&super_tree.at(super_key).at("family")==cls/100*100)return true;
        if((iid==4803||iid==4004)&&cls/100==2)return true;
        std::vector<int> codes = {cls / 100 * 100, cls};
        if (cls % 10)
            codes.push_back(cls / 10 * 10);
        for (int code : codes) {
            auto key = std::to_string(code);
            if (trees.contains(key))
                for (const auto &id : trees[key])
                    if (id == iid)
                        return true;
        }
        return false;
    };
    auto add_skill = [&](const std::string &id, int rank) {
        if (!skills.contains(id))
            return;
        auto row = skills[id];
        std::vector<std::string> sources = {id};
        int aura = row.value("aura_skill", 0);
        if (aura && std::to_string(aura) != id)
            sources.push_back(std::to_string(aura));
        for (const auto &source : sources)
            if (skills.contains(source) && skills[source].at("levels").contains(std::to_string(rank))) {
                const auto level=skill_level_with_stone(catalog,character,static_cast<std::uint32_t>(std::stoul(source)),rank);
                for (const auto &effect : level.at("effects"))
                    add(effect.at("operation"), effect.at("flat"), effect.at("percent"));
            }
    };
    auto learned = character.value("skills", Json::object());
    for (auto entry = learned.begin(); entry != learned.end(); ++entry)
        if (skills.contains(entry.key()) && skills[entry.key()].value("passive", false) &&
            allowed(std::stoi(entry.key())))
            add_skill(entry.key(), entry.value());
    auto paths = character.value("fletta_paths", Json::object());
    for (auto path = paths.begin(); path != paths.end(); ++path) {
        Json best;
        for (const auto &row : attributes)
            if (row.at("group") == std::stoi(path.key()) && row.at("path") == path.value().at("path") &&
                row.at("threshold").get<int>() <= path.value().at("points").get<int>() &&
                (best.is_null() || row.at("rank").get<int>() > best.at("rank").get<int>()))
                best = row;
        if (!best.is_null())
            add_skill(std::to_string(best.at("skill_id").get<int>()), best.at("rank"));
    }
    auto buffs = character.value("active_buffs", Json::object());
    for (const auto &entry : buffs.items())
        if (entry.value().at("expires").get<double>() > timestamp && allowed(std::stoi(entry.key())))
            add_skill(entry.key(), entry.value().at("rank"));
    for (const auto &effect : character.value("consumable_effects", Json::object())) {
        if (effect.at("expires").get<double>() <= timestamp)
            continue;
        auto kind = effect.at("kind").get<std::string>();
        if (kind == "buff")
            add(effect.at("op"), effect.at("flat"), effect.at("percent"));
        else if (kind == "native_buff")
            for (const auto &row : effect.at("effects"))
                add(row.at("operation"), row.at("flat"), row.at("percent"));
    }
    for (const auto &effect : character.value("timed_item_buffs", Json::object()))
        if (effect.value("expires", 0.) > timestamp) {
            int percent = std::clamp(effect.value("percent", 0), 0, 100);
            add(71, 0, percent);
            add(74, 0, percent);
        }
    return mods;
}
Json enchant_row(const Json &rows, int grade) {
    if (grade < 0 || grade > 18)
        throw std::runtime_error("Enhancement grade outside local policy");
    Json low, high, anchor;
    for (const auto &row : rows) {
        int level = row.at("lvl");
        if (level == grade)
            return row;
        if (level == 12)
            low = row;
        if (level == 13)
            high = row;
        if (level == 16)
            anchor = row;
    }
    if (grade <= 13 || low.is_null() || high.is_null())
        throw std::runtime_error("Native enhancement attributes missing");
    int span = 1;
    if (!anchor.is_null()) {
        low = high;
        high = anchor;
        span = 3;
    }
    auto result = high;
    result["lvl"] = grade;
    result["source"] = "local-extension";
    auto extend = [&](double a, double b) {
        return std::nearbyint(std::max(0., b + (b - a) / span * (grade - high.at("lvl").get<int>())) *
                              1000000) /
               1000000;
    };
    for (const auto &field : {"vals", "defense", "magic_defense", "magic_attack"})
        if (high.contains(field)) {
            if (high[field].is_array()) {
                result[field] = Json::array();
                for (std::size_t i = 0; i < high[field].size(); i++)
                    result[field].push_back(extend(low[field][i], high[field][i]));
            } else
                result[field] = extend(low[field], high[field]);
        }
    return result;
}
Json equipped_traits(Content &content, Sqlite &db, std::uint32_t eid, double timestamp) {
    const auto links = content.get("data/item_effect_links_428.json"),
               skills = content.get("data/skill_catalog.json").at("skills"),
               fusions = content.get("data/fusion_effects_native.json"),
               starter = content.get("data/starter_costume_choices.json");
    const std::set<int> supported = {1,  2,  3,  4,  5,  6,  9,  10, 11, 12, 13, 14, 16,  17,
                                     19, 21, 22, 27, 30, 31, 46, 47, 64, 71, 73, 74, 96, 98, 99, 128, 129};
    db.exec("CREATE TABLE IF NOT EXISTS item_custom_affixes(uid INTEGER NOT NULL,layer TEXT NOT "
            "NULL,position INTEGER NOT NULL,stat TEXT NOT NULL,value REAL NOT NULL,PRIMARY "
            "KEY(uid,layer,position));CREATE TABLE IF NOT EXISTS item_pk_fusions(uid INTEGER PRIMARY "
            "KEY,partner_item_id INTEGER NOT NULL,partner_grade INTEGER NOT NULL);CREATE TABLE IF NOT EXISTS "
            "reward_instances(uid INTEGER PRIMARY KEY,player_eid INTEGER NOT NULL,item_id INTEGER NOT "
            "NULL,expires_at INTEGER NOT NULL,source TEXT NOT NULL)");
    Json mods = Json::object();
    double critical = 0, mana = 0, pvp_defense = 0, pvp_critical = 0;
    std::set<int> starter_pieces;
    auto add = [&](int operation, double flat, double percent) {
        auto key = std::to_string(operation);
        if (!mods.contains(key))
            mods[key] = {0., 0.};
        mods[key][0] = mods[key][0].get<double>() + flat;
        mods[key][1] = mods[key][1].get<double>() + percent;
    };
    auto standard = [&](const std::string &stat, double value) {
        const std::map<std::string, int> flats = {{"physical_attack_flat", 6},   {"magic_attack_flat", 9},
                                                  {"physical_defense_flat", 17}, {"magic_defense_flat", 19},
                                                  {"critical_chance_flat", 11},  {"max_hp_flat", 46},
                                                  {"max_mp_flat", 47}, {"strength",1}, {"dexterity",3},
                                                  {"vitality",2}, {"intelligence",5}, {"wisdom",4},
                                                  {"hp_regen_flat",30}, {"mp_regen_flat",31},
                                                  {"accuracy",10}, {"defense_rating",16}};
        const std::map<std::string, int> percents = {
            {"cast_speed", 14},   {"magic_attack", 74},     {"max_hp", 46},        {"physical_attack", 6},
            {"attack_speed", 13}, {"physical_defense", 17}, {"magic_defense", 19}, {"move_speed", 27},
            {"max_mp",47}, {"block_chance",22}, {"cooldown_reduction",64}};
        if (flats.count(stat))
            add(flats.at(stat), value, 0);
        else if (stat == "attack_power") {
            add(6, 0, value);
            add(74, 0, value);
        } else if(stat=="hp_absorption"){
            add(99,0,value);
        } else if(stat=="evasion"){
            add(21,0,value);
        } else if(stat=="critical_reduction"){
            add(98,0,value*100);
        } else if(stat=="physical_reflection"){add(96,0,value);}
        else if(stat=="magic_reflection"){add(138,0,value);}
        else if (percents.count(stat))
            add(percents.at(stat), 0, value);
        else
            throw std::runtime_error("Unrecognized saved equipment effect: " + stat);
    };
    auto equipped = db.query("SELECT e.item_id,e.instance_uid FROM equipment_instances e LEFT JOIN "
                             "reward_instances r ON r.uid=e.instance_uid WHERE e.player_eid=? AND (r.uid IS "
                             "NULL OR (r.player_eid=e.player_eid AND r.expires_at>?))",
                             {std::int64_t(eid), timestamp});
    for (const auto &item : equipped) {
        auto iid = std::to_string(item.at("item_id").get<int>());
        auto uid = item.at("instance_uid").get<std::int64_t>();
        Json effects = Json::array();
        if (links.contains(iid)) {
            auto skill = std::to_string(links[iid].value("skill_id", 0));
            if (skills.contains(skill) && skills[skill].contains("levels") &&
                skills[skill]["levels"].contains("1"))
                effects = skills[skill]["levels"]["1"].value("effects", Json::array());
        }
        auto fused = db.query("SELECT partner_item_id,partner_grade FROM item_pk_fusions WHERE uid=?", {uid});
        if (!fused.empty()) {
            auto partner = std::to_string(fused[0].at("partner_item_id").get<int>());
            int grade = fused[0].at("partner_grade");
            if (!fusions.contains(partner))
                throw std::runtime_error("Saved fusion has no native content");
            const auto &rows = fusions[partner];
            Json extra;
            if (rows.contains(std::to_string(grade)))
                extra = rows[std::to_string(grade)].at("effects");
            else if (grade > 13 && grade <= 18 && rows.contains("12") && rows.contains("13")) {
                extra = rows["13"].at("effects");
                for (auto &effect : extra) {
                    Json lower = effect;
                    for (const auto &candidate : rows["12"].at("effects"))
                        if (candidate.at("operation") == effect.at("operation"))
                            lower = candidate;
                    for (const auto &name : {"flat", "percent"})
                        effect[name] = std::max(
                            0., effect[name].get<double>() +
                                    (effect[name].get<double>() - lower[name].get<double>()) * (grade - 13));
                }
            } else
                throw std::runtime_error("Saved fusion grade has no native content");
            for (const auto &effect : extra)
                effects.push_back(effect);
        }
        for (const auto &effect : effects) {
            int operation = effect.at("operation");
            if (supported.count(operation))
                add(operation, effect.at("flat"), effect.at("percent"));
            else if (operation == 55)
                pvp_defense += effect.at("flat").get<double>();
            else if (operation == 97)
                pvp_critical += effect.at("flat").get<double>();
        }
        for (const auto &row :
             db.query("SELECT layer,stat,value FROM item_custom_affixes WHERE uid=? ORDER BY layer,position",
                      {uid})) {
            if (row.at("layer") == "starter")
                starter_pieces.insert(std::stoi(iid));
            auto stat = row.at("stat").get<std::string>();
            double value = row.at("value");
            if (stat == "critical_multiplier")
                critical += value;
            else if (stat == "mana_absorption")
                mana += value;
            else
                standard(stat, value);
        }
    }
    std::size_t count = 0;
    for (const auto &set : starter.at("parts")) {
        std::size_t found = 0;
        for (const auto &id : set)
            if (starter_pieces.count(id))
                found++;
        count = std::max(count, found);
    }
    for (auto entry = starter["set_bonus"].begin(); entry != starter["set_bonus"].end(); ++entry)
        if (count >= std::stoul(entry.key()))
            for (const auto &bonus : entry.value())
                standard(bonus[0], bonus[1]);
    return {{"mods", mods},
            {"critical_bonus", critical},
            {"mana_absorption", std::min(100., mana)},
            {"pvp_defense", pvp_defense},
            {"pvp_critical_reduction", std::min(1., pvp_critical)}};
}
} // namespace rosh
