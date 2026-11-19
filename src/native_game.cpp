#include "native/consumables.hpp"
#include "native/game.hpp"
#include "native/control.hpp"
#include "native/runtime.hpp"
#include "native/inventory.hpp"
#include "native/economy.hpp"
#include "native/gm.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace rosh {
static Bytes service_notice(const Identity &identity,const std::string &message){
    static constexpr const char *translations[][4]={
        {"Personagem distante do item","Você está longe demais do item.","Estás demasiado lejos del objeto.","You are too far from the item."},
        {"Personagem morto nao pode coletar itens","Você não pode coletar itens enquanto estiver morto.","No puedes recoger objetos mientras estás muerto.","You cannot pick up items while dead."},
        {"Item nao encontrado","Item não encontrado.","Objeto no encontrado.","Item not found."},
        {"Item coletado, expirado ou de outro mapa","O item já foi coletado, expirou ou está em outro mapa.","El objeto ya se recogió, caducó o pertenece a otro mapa.","The item was already picked up, expired, or is on another map."},
        {"Item de missao reservado ao personagem que possui o objetivo","Este item de missão está reservado para o personagem que tem o objetivo.","Este objeto está reservado para el personaje que tiene la misión.","This item is reserved for the character with the quest objective."},
        {"Item reservado por merito durante60segundos","O saque fica reservado por contribuição durante 60 segundos.","El botín está reservado por contribución durante 60 segundos.","Loot is reserved by contribution for 60 seconds."},
        {"Item indisponivel no deposito","O item não está disponível no depósito.","El objeto no está disponible en el depósito.","The item is unavailable in storage."},
        {"Seu cargo nao tem permissao para esta operacao no deposito do cla","Seu cargo não permite esta operação no depósito do clã.","Tu rango no permite realizar esta operación en el depósito del clan.","Your rank does not allow this operation in clan storage."},
        {"Abra o deposito perto do NPC","Abra o depósito perto do NPC.","Abre el depósito cerca del PNJ.","Open storage near the NPC."},
        {"Abra o correio perto do NPC","Abra o correio perto do NPC.","Abre el buzón cerca del PNJ.","Open the mailbox near the NPC."},
        {"Algumas cartas nao foram excluidas. Resgate os anexos antes de excluir.","Algumas cartas não foram apagadas. Retire os anexos antes de apagá-las.","Algunas cartas no se eliminaron. Recoge los adjuntos antes de borrarlas.","Some letters were not deleted. Claim the attachments before deleting them."},
        {"Mensagem indisponivel","A mensagem não está disponível.","El mensaje no está disponible.","The message is unavailable."},
        {"Pedido de correio invalido","Pedido de correio inválido.","Solicitud de buzón no válida.","Invalid mailbox request."},
        {"Pedido de exclusao de correio invalido","Pedido inválido para excluir cartas.","Solicitud de eliminación de cartas no válida.","Invalid request to delete letters."},
        {"Tipo de mensagem invalido","Tipo de mensagem inválido.","Tipo de mensaje no válido.","Invalid message type."},
        {"Autorizacao do remetente revogada","A autorização do remetente foi revogada.","Se revocó la autorización del remitente.","The sender's authorization was revoked."},
        {"Anexo indisponivel","O anexo não está disponível.","El adjunto no está disponible.","The attachment is unavailable."},
        {"Quantidade excede o item possuido","A quantidade é maior do que você tem.","La cantidad supera las unidades que tienes.","The quantity exceeds the number of items you own."},
        {"Todas as bolsas estao cheias","Todas as bolsas estão cheias.","Todas las bolsas están llenas.","All bags are full."},
        {"Deposito indisponivel","O depósito não está disponível.","El depósito no está disponible.","Storage is unavailable."},
        {"Correio indisponivel","O correio não está disponível.","El buzón no está disponible.","The mailbox is unavailable."},
        {"Voce nao tem um cla","Você não faz parte de um clã.","No perteneces a ningún clan.","You do not belong to a clan."},
        {"Apenas o lider do cla configura o deposito","Somente o líder do clã pode configurar o depósito.","Solo el líder del clan puede configurar el depósito.","Only the clan leader can configure storage."},
        {"Permissoes de deposito invalidas","As permissões do depósito são inválidas.","Los permisos del depósito no son válidos.","Invalid storage permissions."},
        {"Permissoes do cla salvas.","Permissões do clã salvas.","Permisos del clan guardados.","Clan permissions saved."},
        {"Abra este deposito primeiro","Abra este depósito primeiro.","Abre primero este depósito.","Open this storage first."},
        {"Saldo de Carats insuficiente no deposito","Não há Carats suficientes no depósito.","No hay suficientes Carats en el depósito.","There are not enough Carats in storage."},
    };
    const int column=identity.locale=="es"?2:identity.locale=="en"?3:1;
    for(const auto &entry:translations)
        if(message==entry[0])return notice(entry[column]);
    return notice(message);
}

std::pair<int,int> attack_recovery(const Json&stats,const Json&ch,std::uint64_t damage){
 if(!damage||ch.value("hp",0)<=0)return {0,0};
 auto gain=[&](const char*current,const char*maximum,const char*percent){double missing=std::max(0,ch.value(maximum,0)-ch.value(current,0));return static_cast<int>(std::min(missing,std::floor(static_cast<double>(damage)*std::clamp(stats.value(percent,0.),0.,100.)/100)));};
 return {gain("hp","maxhp","lifesteal"),gain("mp","maxmp","mana_absorption")};
}
std::pair<int,int> regeneration_recovery(const Json&stats,const Json&ch){
 if(ch.value("hp",0)<=0)return {0,0};
 auto gain=[&](const char*current,const char*maximum,const char*rate){return static_cast<int>(std::min<double>(std::max(0,ch.value(maximum,0)-ch.value(current,0)),std::floor(std::max(0.,stats.value(rate,0.)))));};
 return {gain("hp","maxhp","hp_regen"),gain("mp","maxmp","mp_regen")};
}
Progression::Progression() : requirements_{48, 216, 520, 972, 1577, 2343}, thresholds_{0} {
    while (requirements_.size() < 120)
        requirements_.push_back(std::max(requirements_.back() + 1, static_cast<std::uint64_t>(std::nearbyint(
                                                                       requirements_.back() * 1.23))));
    for (int i = 0; i < 119; i++)
        thresholds_.push_back(thresholds_.back() + requirements_[i]);
}
int Progression::level(std::uint64_t experience) const {
    return static_cast<int>(std::upper_bound(thresholds_.begin(), thresholds_.end(), experience) -
                            thresholds_.begin());
}
std::uint64_t Progression::threshold(int level) const {
    return thresholds_.at(std::clamp(level, 1, 120) - 1);
}
std::uint64_t Progression::requirement(int level) const {
    return requirements_.at(std::clamp(level, 1, 120) - 1);
}
Json calculated_stats(const Json &profiles, int profession, int level, const Json &modifiers) {
    int family = profession / 100 * 100, branch = profession % 100 / 10, leaf = profession % 10;
    const std::map<int, std::array<double, 5>> base = {
        {100, {190, 60, 51.5, 8, 21}},  {200, {190, 91, 51.5, 8, 21}}, {300, {150, 80, 36.5, 9, 24}},
        {400, {160, 90, 40.5, 10, 14}}, {500, {160, 70, 40.5, 8, 24}}, {600, {150, 90, 36.5, 10, 14}},
        {800, {189, 117, 47.5, 10, 28}}};
    if (!base.count(family) || branch > 2 || leaf > 2 || (branch == 0 && leaf != 0))
        throw std::runtime_error("Unsupported class/profession");
    const auto &p = profiles.at("classes").at(std::to_string(family));
    auto b = base.at(family);
    int n = std::clamp(level, 1, 120) - 1;
    std::vector<double> attributes(5), original(5), delta(5);
    for (int i = 0; i < 5; i++) {
        original[i] = p["attributes"][i];
        attributes[i] = static_cast<int>(original[i] + p["attribute_growth"][i].get<double>() * n);
        delta[i] = attributes[i] - original[i];
    }
    double offense = 1 +
                     (branch == 1   ? .10
                      : branch == 2 ? .04
                                    : 0) +
                     (leaf == 1   ? .08
                      : leaf == 2 ? .03
                                  : 0),
           protection = 1 +
                        (branch == 1   ? .04
                         : branch == 2 ? .15
                                       : 0) +
                        (leaf == 1   ? .03
                         : leaf == 2 ? .08
                                     : 0);
    double attack_speed = family == 100   ? 1
                          : family == 200 ? .95
                          : family == 300 ? 1.25
                          : family == 400 ? .95
                          : family == 500 ? 1.1
                          : family == 600 ? .95
                                          : .85;
    Json s = {
        {"attributes", attributes},
        {"max_hp", static_cast<int>((b[0] + b[2] * n) * (1 + (protection - 1) * .6))},
        {"max_mp", static_cast<int>((b[1] + b[3] * n) * (1 + (offense - 1) * .5))},
        {"attack", (b[4] + p["attack_growth"].get<double>() * n + delta[0] * .9 + delta[1] * .4) * offense},
        {"magic_attack", (p["magic_base"].get<double>() + p["magic_growth"].get<double>() * n +
                          delta[3] * 1.5 + delta[4] * .4) *
                             offense},
        {"defense",
         (p["base_defense"].get<double>() + p["defense_growth"].get<double>() * n + delta[2] * .4) *
             protection},
        {"magic_defense", (8 + n * .7 + delta[4] * .45) * protection},
        {"critical", std::min(35., 3 + delta[1] * .08 + ((family == 300 || family == 500) ? 3 : 0))},
        {"hp_regen", 2 + n * .35},
        {"mp_regen", 1 + n * ((family == 400 || family == 600) ? .25 : .12)},
        {"attack_speed", attack_speed},
        {"cast_speed", family == 400 || family == 600 ? 1.1 : 1.},
        {"move_speed", 65.}};
    for (const auto &field : {"attack", "magic_attack", "defense", "magic_defense"}) {
        char rounded[128]{};
        std::snprintf(rounded, sizeof(rounded), "%.3f", s[field].get<double>());
        s[field] = std::strtod(rounded, nullptr);
    }
    auto mod = [&](int op, int part) {
        auto key = std::to_string(op);
        return modifiers.contains(key) ? modifiers.at(key).at(part).get<double>() : 0.;
    };
    auto add = [&](const char *field, double v) { s[field] = s[field].get<double>() + v; };
    // Native formatter006076CB and primary panel0049B13E: INT=5, WIS=4.
    const int attribute_ops[] = {1, 3, 2, 5, 4};
    original = attributes;
    for (int i = 0; i < 5; i++)
        attributes[i] = std::max(0., std::nearbyint((attributes[i] + mod(attribute_ops[i], 0)) *
                                                    std::max(0., 1 + mod(attribute_ops[i], 1) / 100)));
    attributes[0] += std::nearbyint(attributes[0] * mod(130, 1) / 100);
    for (int i = 0; i < 5; i++)
        delta[i] = attributes[i] - original[i];
    s["attributes"] = attributes;
    add("attack", delta[0] * .9 + delta[1] * .4);
    add("magic_attack", delta[3] * 1.5 + delta[4] * .4);
    add("max_hp", delta[2] * 12);
    add("max_mp", delta[4] * 4);
    add("defense", delta[2] * .4);
    add("magic_defense", delta[4] * .45);
    const std::map<int, const char *> flat = {{6, "attack"},   {9, "magic_attack"},   {11, "critical"},
                                              {17, "defense"}, {19, "magic_defense"}, {27, "move_speed"},
                                              {46, "max_hp"},  {47, "max_mp"}};
    const std::map<int, const char *> percent = {
        {6, "attack"},   {9, "magic_attack"},   {11, "critical"},    {13, "attack_speed"}, {14, "cast_speed"},
        {17, "defense"}, {19, "magic_defense"}, {27, "move_speed"},  {46, "max_hp"},       {47, "max_mp"},
        {71, "attack"},  {73, "magic_defense"}, {74, "magic_attack"}};
    for (const auto &[op, name] : flat)
        add(name, mod(op, 0));
    for (const auto &[op, name] : percent)
        s[name] = s[name].get<double>() * std::max(.1, 1 + mod(op, 1) / 100);
    s["critical_multiplier"] = std::clamp(1.5 + mod(12, 0) + mod(12, 1) / 100, 1.5, 3.);
    s["defense_rating"] = std::max(0., mod(16, 0));
    s["accuracy"] = std::max(0., (attributes[1] * .5 + n * .1 + mod(10, 0)) * std::max(0., 1 + mod(10, 1) / 100));
    s["defense_rating"] = std::max(0., s["defense_rating"].get<double>() * std::max(0., 1 + mod(16, 1) / 100));
    s["block_chance"] = std::clamp(mod(22, 0) + mod(22, 1), 0., 75.);
    s["hp_regen"] = std::max(0., (s["hp_regen"].get<double>() + mod(30, 0)) * std::max(0., 1 + mod(30, 1) / 100));
    s["mp_regen"] = std::max(0., (s["mp_regen"].get<double>() + mod(31, 0)) * std::max(0., 1 + mod(31, 1) / 100));
    s["evasion"] = std::clamp(mod(21, 0) + mod(21, 1), 0., 75.);
    s["critical_damage_reduction"] = std::clamp(mod(98, 1), 0., 90.);
    for (const auto &entry :
         std::vector<std::tuple<int, const char *, double>>{{64, "cooldown_reduction", 75},
                                                            {99, "lifesteal", 50},
                                                            {128, "healing", 100},
                                                            {129, "damage_reduction", 70}})
        s[std::get<1>(entry)] = std::clamp(mod(std::get<0>(entry), 1), 0., std::get<2>(entry));
    s["max_hp"] = std::max(1, static_cast<int>(s["max_hp"].get<double>()));
    s["max_mp"] = std::max(1, static_cast<int>(s["max_mp"].get<double>()));
    return s;
}
static void patch_appearance(Bytes &packet, std::size_t offset, const Json &character) {
    if (!character.contains("appearance") || character["appearance"].is_null())
        return;
    auto raw = unhex(character.at("appearance"));
    if (raw.size() != 6 && raw.size() != 8)
        throw std::runtime_error("Saved appearance is malformed");
    if (offset < 2 || packet.size() < offset + 6)
        throw std::runtime_error("Appearance carrier is truncated");
    std::copy(raw.begin(), raw.begin() + 2, packet.begin() + offset - 2);
    std::copy(raw.begin() + 2, raw.begin() + 6, packet.begin() + offset);
    if (raw.size() == 8)
        std::copy(raw.begin() + 6, raw.end(), packet.begin() + offset + 4);
}
static void patch_entity(Bytes &bytes, std::uint32_t captured, std::uint32_t actual) {
    if (captured == actual)
        return;
    for (bool big : {false, true}) {
        Bytes old(4), replacement(4);
        put32(old, 0, captured, big);
        put32(replacement, 0, actual, big);
        std::size_t offset = 0;
        while (offset + 4 <= bytes.size()) {
            auto it = std::search(bytes.begin() + offset, bytes.end(), old.begin(), old.end());
            if (it == bytes.end())
                break;
            offset = static_cast<std::size_t>(it - bytes.begin());
            std::copy(replacement.begin(), replacement.end(), it);
            offset += 4;
        }
    }
}
WorldSession::WorldSession(Content &content, LegacyStore &store, const Session &identity,
                           const fs::path &root, std::uint32_t eid,std::shared_ptr<WorldScene> scene)
    : content_(content), store_(store), identity_(identity), leases_(root / "Database/runtime.sqlite"),
      character_(store.character(identity.identity.id, eid)), map_name_("berneo"), map_id_(19003) {
    root_ = root;
    scene_=scene?std::move(scene):std::make_shared<WorldScene>(content_);
    roles_=std::make_unique<Sqlite>(root/"Database/karos_roles.sqlite",true);
    rendered_role_=character_role(*roles_,identity_.identity.id,eid);
    staff_=std::make_unique<StaffSession>(root,identity_.identity.id,eid);
    staff_channel_=std::make_unique<StaffChannel>(store_.db());staff_chat_after_=staff_channel_->latest();
    store_.db().exec("CREATE TABLE IF NOT EXISTS native_public_character_names(character_eid INTEGER PRIMARY KEY,account_id TEXT NOT NULL,name TEXT NOT NULL);CREATE INDEX IF NOT EXISTS native_public_character_name_lookup ON native_public_character_names(name)");
    store_.db().exec("INSERT INTO native_public_character_names VALUES(?,?,?) ON CONFLICT(character_eid) DO UPDATE SET account_id=excluded.account_id,name=excluded.name",{std::int64_t(eid),identity_.identity.id,character_.at("name").get<std::string>()});
    gm_schema(store_.db());
    leases_.exec("CREATE TABLE IF NOT EXISTS native_player_leases(eid INTEGER PRIMARY KEY,session TEXT NOT "
                 "NULL,account TEXT NOT NULL,channel INTEGER NOT NULL,expires REAL NOT NULL)");
    claim();
    try {
        store_.db().exec(
            "CREATE TABLE IF NOT EXISTS character_locations(eid INTEGER PRIMARY KEY,map_name TEXT NOT NULL,x "
            "REAL NOT NULL,y REAL NOT NULL,z REAL NOT NULL,angle REAL NOT NULL,updated_at REAL NOT NULL)");
        auto locations =
            store_.db().query("SELECT * FROM character_locations WHERE eid=?", {std::int64_t(eid)});
        Json position = {9020., 6590., 627.};
        double angle = 0.;
        bool restored_location = false;
        if (!locations.empty()) {
            auto row = locations[0];
            auto maps = content_.get("data/maplist.json");
            for (const auto &map : maps) {
                auto name = map.value("name", "");
                auto saved_id=character_.value("map_id",0u);
                const auto saved_name=row.at("map_name").get<std::string>();
                const auto id=map.at("head")[1].get<std::uint32_t>();
                const bool name_matches=_stricmp(name.c_str(),saved_name.c_str())==0 ||
                    (saved_id==id && scene_map_name(name,id)==saved_name);
                if (name_matches && (!saved_id||id==saved_id)) {
                    map_name_ = name;
                    auto head = map.at("head");
                    if (head.size() > 1)
                        map_id_ = head[1];
                    position = {row["x"], row["y"], row["z"]};
                    angle = row["angle"];
                    restored_location = true;
                    break;
                }
            }
        }
        // A first login must use this client's native creation point, not a
        // coordinate copied from somebody else's capture. Existing saves win.
        position_guard_ = restored_location;
        character_["pos"] = position;
        character_["angle"] = angle;
        character_["map"] = map_name_;
        character_["map_id"] = map_id_;
        dungeon_group_=scene_->instance_group(eid);
        int level = progression_.level(character_.value("exp", std::uint64_t(0)));
        character_["level"] = level;
        store_.db().exec("CREATE TABLE IF NOT EXISTS equipment_instances(player_eid INTEGER,slot "
                         "INTEGER,item_id INTEGER,instance_uid INTEGER,created_at INTEGER,PRIMARY "
                         "KEY(player_eid,slot));CREATE TABLE IF NOT EXISTS inventory_uid_seq(id INTEGER "
                         "PRIMARY KEY AUTOINCREMENT);CREATE TABLE IF NOT EXISTS item_enchantments(uid "
                         "INTEGER PRIMARY KEY,grade INTEGER NOT NULL CHECK(grade BETWEEN 0 AND 127))");
        {
            Transaction tx(store_.db());
            auto rows = store_.db().query(
                "SELECT slot,item_id FROM equipment_instances WHERE player_eid=? ORDER BY slot",
                {std::int64_t(eid)});
            if (rows.empty() && !character_.contains("equipment_slots")) {
                auto gear = character_.value("gear", Json::object());
                if (gear.empty())
                    gear = content_.get("server/starter_gear.json")
                               .at(std::to_string(character_.at("cls").get<int>() / 100 * 100));
                Json initial = Json::object();
                for (int i = 0; i < 4; i++)
                    if (gear.at("armor")[i] != 0)
                        initial[std::to_string(i + 1)] = gear["armor"][i];
                if (gear.value("weapon", 0))
                    initial["7"] = gear["weapon"];
                if (gear.value("shield", 0))
                    initial["8"] = gear["shield"];
                for (auto item = initial.begin(); item != initial.end(); ++item) {
                    store_.db().exec("INSERT INTO inventory_uid_seq DEFAULT VALUES");
                    auto uid =
                        store_.db().query("SELECT last_insert_rowid() AS uid")[0]["uid"].get<std::int64_t>() +
                        0x01000000;
                    store_.db().exec("INSERT INTO equipment_instances VALUES(?,?,?,?,?)",
                                     {std::int64_t(eid), std::int64_t(std::stoi(item.key())),
                                      item.value().get<std::int64_t>(), uid, std::int64_t(now())});
                }
            }
            tx.commit();
        }
        inventory_ = std::make_unique<Inventory>(store_.db(), content_);
        guild_chat_after_=inventory_->guild_chat_latest();
        guild_departure_after_=inventory_->guild_departure_latest();
        guild_invite_result_after_=inventory_->guild_native_result_latest();
        dungeons_=std::make_unique<DungeonStore>(store_.db(),content_);
        if(dungeons_->rule(map_id_).value("instanced",false)){
            auto run=dungeons_->resume(eid,map_id_,identity_.channel,now());
            if(run.is_null()){
                const auto old=dungeons_->last_membership(eid,map_id_);
                if(old.is_object()){
                    const auto entrance=Json::parse(old.at("entrance").get<std::string>());
                    dungeons_->leave("run:"+old.at("id").get<std::string>(),eid,now());
                    map_name_=entrance.at("world_name");map_id_=entrance.at("map_id");
                    character_["pos"]={entrance.at("x"),entrance.at("y"),entrance.at("z")};character_["angle"]=entrance.value("angle",0.);character_["map"]=map_name_;character_["map_id"]=map_id_;
                    dungeon_group_.clear();
                }else{
                    // One-time adoption of characters saved inside a dungeon by older builds.
                    run=dungeons_->join(eid,scene_->instance_group(eid),map_id_,identity_.channel,resolve_native_map(content_,"berneo"),now());
                }
            }
            if(run.is_object())dungeon_group_="run:"+run.at("id").get<std::string>();
        }

        if(content_.has("data/consumable_effects.json"))
            consumables_=std::make_unique<Consumables>(store_.db(),*inventory_,content_);
        premium_=std::make_unique<PremiumShop>(store_.db(),*inventory_,content_);
        loot_=std::make_unique<GroundLoot>(store_.db(),content_,*inventory_);
        quests_=std::make_unique<QuestBook>(store_.db(),*inventory_,content_,eid);
        npc_shops_=std::make_unique<NpcShops>(store_.db(),content_,*inventory_);
        scene_entities_=scene_->map(scene_map_name());
        scene_->restore(scene_map_name(),store_.db(),map_scope(),now());
        scene_entities_=scene_->map(scene_map_name());
        skills_=std::make_unique<SkillBook>(content_);
        casting_=std::make_unique<ActiveSkills>(content_);
        if(skills_->initialize(character_))store_.checkpoint(identity_.identity.id,character_);
        recalculate();
        load_templates();
        if(QuickSlots::initialize(character_,templates_.at(0x0205)))store_.checkpoint(identity_.identity.id,character_);
    } catch (...) {
        release();
        throw;
    }
}
namespace {
int party_aura_group(int skill){
 switch(skill){
 case 3005:return 300501;
 case 3103:case 3903:return 310301;
 case 3904:return 390401;
 case 3905:return 390501;
 case 3908:return 390801;
 case 3102:case 3902:case 4504:return 3902;
 default:return 0;
 }
}
}
Json WorldSession::active_party_auras(double timestamp)const{
 Json result=Json::object();
 if(!casting_||!skills_||character_.value("hp",0)<=0)return result;
 auto active=character_.value("active_buffs",Json::object());
 for(const auto& [key,buff]:active.items()){
  const int id=std::stoi(key),group=party_aura_group(id);
  if(!group||buff.at("expires").get<double>()<=timestamp||!skills_->cast_allowed(character_.at("cls"),id))continue;
  auto effects=casting_->effects(id,buff.at("rank"));
  double power=0.;
  for(const auto& effect:effects)power+=std::abs(effect.at("flat").get<double>())+std::abs(effect.at("percent").get<double>());
  const auto slot=std::to_string(group);
  if(power>0&&(!result.contains(slot)||power>result.at(slot).at("power").get<double>()))
   result[slot]={{"skill",id},{"rank",buff.at("rank")},{"power",power},{"expires",buff.at("expires")},{"effects",std::move(effects)}};
 }
 return result;
}
void WorldSession::refresh_party_auras(const Sender& send){
 auto current=scene_->party_auras(scene_key(),now());
 if(current==received_party_auras_)return;
 received_party_auras_=std::move(current);
 recalculate();
 send(8,2,stats_packet());
}
void WorldSession::tick_mana_recovery(double timestamp,const Sender& send){
 if(character_.value("hp",0)<=0){mana_recovery_schedule_.clear();return;}
 auto personal=active_party_auras(timestamp);
 const Json* rest=nullptr;
 const auto own=personal.find("390801");
 const auto borrowed=received_party_auras_.find("390801");
 if(own!=personal.end())rest=&*own;
 if(borrowed!=received_party_auras_.end()&&borrowed->at("expires").get<double>()>timestamp&&
    (!rest||borrowed->at("power").get<double>()>rest->at("power").get<double>()))rest=&*borrowed;
 const int maximum=character_.at("maxmp");
 const int current=character_.at("mp");
 double gain=0.;
 std::map<int,std::pair<double,double>> updated;
 auto pulse=[&](int id,double period,double expires,const Json& effects){
  if(expires<=timestamp)return;
  auto previous=mana_recovery_schedule_.find(id);
  if(previous==mana_recovery_schedule_.end()||previous->second.first!=expires){
   updated[id]={expires,timestamp+period};
   return;
  }
  auto next=previous->second.second;
  if(next<=timestamp){
   for(const auto& effect:effects)if(effect.at("operation")==29)
    gain+=effect.at("flat").get<double>()+maximum*effect.at("percent").get<double>()/100.;
   next+=period*(std::floor((timestamp-next)/period)+1);
  }
  updated[id]={expires,next};
 };
 if(rest)pulse(3908,4.,rest->at("expires"),rest->at("effects"));
 const auto active=character_.value("active_buffs",Json::object());
 for(const int id:{7004,11004,12104,12305,12503,12703}){
  const auto buff=active.find(std::to_string(id));
  if(buff==active.end()||buff->at("expires").get<double>()<=timestamp||
     !skills_->cast_allowed(character_.at("cls"),id))continue;
  pulse(id,2.,buff->at("expires"),casting_->effects(id,buff->at("rank")));
 }
 const auto recovered=static_cast<int>(std::clamp(std::nearbyint(gain),0.,double(std::max(0,maximum-current))));
 if(recovered){
  try{character_["mp"]=current+recovered;persist();}
  catch(...){character_["mp"]=current;throw;}
 }
 mana_recovery_schedule_=std::move(updated);
 if(recovered)send(8,1,gauge(0x0801));
}
void WorldSession::recalculate() {
    auto traits = equipped_traits(content_, store_.db(), eid(), now());
    const auto timestamp=now();
    auto personal=active_party_auras(timestamp);
    auto effective=character_;
    if(effective.contains("active_buffs")){
        auto& active=effective["active_buffs"];
        for(auto it=active.begin();it!=active.end();){
            int id=std::stoi(it.key()),group=party_aura_group(id);
            const auto key=std::to_string(group);
            if(group&&personal.contains(key)&&
               (personal.at(key).at("skill")!=id||
                (received_party_auras_.contains(key)&&received_party_auras_.at(key).at("expires").get<double>()>timestamp&&
                 received_party_auras_.at(key).at("power").get<double>()>personal.at(key).at("power").get<double>())))
                it=active.erase(it);
            else ++it;
        }
    }
    auto modifiers=character_modifiers(content_,effective,timestamp);
    for(const auto& [group,aura]:received_party_auras_.items()){
        if(aura.at("expires").get<double>()<=timestamp||
           (personal.contains(group)&&personal.at(group).at("power").get<double>()>=aura.at("power").get<double>()))continue;
        for(const auto& effect:aura.at("effects")){
            auto key=effect.at("operation").dump();
            if(!modifiers.contains(key))modifiers[key]={0.,0.};
            modifiers[key][0]=modifiers[key][0].get<double>()+effect.at("flat").get<double>();
            modifiers[key][1]=modifiers[key][1].get<double>()+effect.at("percent").get<double>();
        }
    }
    if(auto speed=mounted_speed();speed>0){if(!modifiers.contains("27"))modifiers["27"]={0.,0.};modifiers["27"][0]=modifiers["27"][0].get<double>()+speed;}
    for (auto effect = modifiers.begin(); effect != modifiers.end(); ++effect) {
        if (!traits["mods"].contains(effect.key()))
            traits["mods"][effect.key()] = {0., 0.};
        for (int part = 0; part < 2; part++)
            traits["mods"][effect.key()][part] =
                traits["mods"][effect.key()][part].get<double>() + effect.value()[part].get<double>();
    }
    stats_ = calculated_stats(content_.get("data/balance_profiles.json"), character_.at("cls"),
                              character_.at("level"), traits.at("mods"));
    const auto mountedBonus=mounted_speed();
    const auto movementPercent=traits["mods"].contains("27")?traits["mods"]["27"][1].get<double>():0.;
    stats_["walking_speed"]=(stats_.at("move_speed").get<double>()-mountedBonus*std::max(.1,1+movementPercent/100))*.695;
    const auto stamp=now();
    stats_["xp_bonus"]=stats_.value("xp_bonus",0.)+Consumables::bonus(character_,"xp",stamp);
    stats_["fletta_bonus"]=stats_.value("fletta_bonus",0.)+Consumables::bonus(character_,"fletta",stamp);
    stats_["critical_multiplier"] =
        stats_["critical_multiplier"].get<double>() + traits["critical_bonus"].get<double>();
    stats_["mana_absorption"] = traits["mana_absorption"];
    for(auto [field,operation]:std::vector<std::pair<const char*,const char*>>{{"physical_reflection","96"},{"magic_reflection","138"}}){auto mods=traits.at("mods");stats_[field]=mods.contains(operation)?std::clamp(mods[operation][1].get<double>(),0.,100.):0.;}
    stats_["pvp_defense"] = traits["pvp_defense"];
    stats_["pvp_critical_reduction"] = traits["pvp_critical_reduction"];
    auto rate=[&](const char*op){return traits["mods"].contains(op)?std::max(.1,1+traits["mods"][op][1].get<double>()/100):1.;};
    stats_["physical_factor"]=rate("71")*rate("6");stats_["magic_factor"]=rate("9")*rate("74");
    stats_["physical_intrinsic"]=stats_["attack"].get<double>()/stats_["physical_factor"].get<double>();
    stats_["magic_intrinsic"]=stats_["magic_attack"].get<double>()/stats_["magic_factor"].get<double>();
    character_["equipment_slots"] = Json::object();
    auto attributes = content_.get("data/item_attrs.json");
    for (const auto &item :
         store_.db().query("SELECT e.slot,e.item_id,COALESCE(g.grade,0) AS grade FROM "
                           "equipment_instances e LEFT JOIN item_enchantments g ON g.uid=e.instance_uid "
                           "LEFT JOIN reward_instances r ON r.uid=e.instance_uid WHERE e.player_eid=? "
                           "AND (r.uid IS NULL OR (r.player_eid=e.player_eid AND r.expires_at>?))",
                           {std::int64_t(eid()), now()})) {
        int slot = item.at("slot");
        if (slot < 1 || slot > 30)
            throw std::runtime_error("Saved equipment slot outside native range");
        character_["equipment_slots"][std::to_string(slot)] = item.at("item_id");
        auto id = std::to_string(item.at("item_id").get<int>());
        if (!attributes.contains(id))
            continue;
        auto row = enchant_row(attributes[id], item.at("grade"));
        auto mods = traits.at("mods");
        auto pct = [&](int op) {
            return mods.contains(std::to_string(op))
                       ? std::max(.1, 1 + mods[std::to_string(op)][1].get<double>() / 100)
                       : 1.;
        };
        stats_["attack"] =
            stats_["attack"].get<double>() +
            (row["vals"][0].get<double>() + row["vals"][1].get<double>()) / 2 * pct(71) * pct(6);
        for (const auto &[name, operation] : std::vector<std::pair<const char *, int>>{
                 {"defense", 17}, {"magic_defense", 19}, {"magic_attack", 74}})
            stats_[name] = stats_[name].get<double>() +
                           row.value(name, 0.) * pct(operation) * (operation == 74 ? pct(9) : 1.);
    }
    character_["maxhp"] = stats_["max_hp"];
    character_["maxmp"] = stats_["max_mp"];
    character_["hp"] = std::clamp(character_.value("hp", 0), 0, stats_["max_hp"].get<int>());
    character_["mp"] = std::clamp(character_.value("mp", 0), 0, stats_["max_mp"].get<int>());
    const auto &slots = character_.at("equipment_slots");
    stats_["shield_active"]=slots.contains("8");
    if(!slots.contains("8"))stats_["block_chance"]=0.;
    character_["gear"] = {{"armor", Json::array({slots.value("1", 0), slots.value("2", 0),
                                                 slots.value("3", 0), slots.value("4", 0)})},
                          {"weapon", slots.value("7", 0)},
                          {"shield", slots.value("8", 0)}};
}
void WorldSession::claim() {
    Transaction tx(leases_);
    leases_.exec("DELETE FROM native_player_leases WHERE expires<=?", {now()});
    leases_.exec("INSERT INTO native_player_leases VALUES(?,?,?,?,?)",
                 {std::int64_t(eid()), identity_.id, identity_.identity.id, std::int64_t(identity_.channel),
                  now() + 30});
    tx.commit();
    next_renew_ = now() + 5;
}
void WorldSession::release() {
    leases_.exec("DELETE FROM native_player_leases WHERE eid=? AND session=?",
                 {std::int64_t(eid()), identity_.id});
}
WorldSession::~WorldSession() {
    if(scene_)scene_->leave_party(eid());
    try {
        if(scene_){auto damage=scene_->depart(scene_key());if(damage&&character_.contains("hp"))character_["hp"]=std::max(0,character_["hp"].get<int>()-damage);}
        persist();
    } catch (...) {
    }
    try {
        release();
    } catch (...) {
    }
}
void WorldSession::tick() {
    if (now() < next_renew_)
        return;
    leases_.exec("UPDATE native_player_leases SET expires=? WHERE eid=? AND session=?",
                 {now() + 30, std::int64_t(eid()), identity_.id});
    if (leases_.changes() != 1)
        throw std::runtime_error("Character session lease expired");
    next_renew_ = now() + 5;
}
void WorldSession::admin_tick(const Sender &send) {
    if(!loaded_)return;
    tick_crafting(send);
    if(character_.value("hp",0)<=0&&!death_notice_sent_){
        casting_->cancel();clear_mount();stand(send);
        send(8,0,gauge(0x0800));send(8,1,gauge(0x0801));
        quest_overlay({{"cmd","asmara.death"},{"dead",true}},send);
        death_notice_sent_=true;
    }
    if(next_regeneration_==0)next_regeneration_=now()+5;
    if(mounted_&&mounted_speed()<=0){auto action=mount_action_;clear_mount();appearance_refresh_pending_=true;recalculate();send(8,1,gauge(0x0801));if(action.size()==36){for(int i=0;i<3;i++)putfloat(action,13+i*4,character_["pos"][i]);putfloat(action,25,character_["angle"]);send(5,3,action);scene_->action(scene_key(),5,3,action);}}
    if(mount_pending_&&!mounted_){
        bool owned=false;
        for(const auto& item:inventory_->items(eid(),true))if(item.slot==6&&item.uid==mount_uid_&&(!item.expires||*item.expires>now()))owned=true;
        if(!owned){clear_mount();appearance_refresh_pending_=true;recalculate();}
    }
    if(mount_pending_&&now()>mount_pending_until_)mount_pending_=false;
    if(appearance_refresh_pending_&&!mounted_&&!mount_pending_)refresh_self_appearance(send);
    if(now()>=next_scene_tick_){
        next_scene_tick_=now()+.1;
        scene_->simulate(now());
        auto damage=scene_->take_damage(scene_key());
        if(damage){character_["hp"]=std::max(0,character_["hp"].get<int>()-damage);persist();stand(send);send(8,0,gauge(0x0800));}
        const auto [healing,mana]=scene_->take_recovery(scene_key());
        const auto hp=character_.at("hp").get<int>(),mp=character_.at("mp").get<int>();
        if((healing||mana)&&hp>0){
            const auto gained_hp=std::min(healing,std::max(0,character_.at("maxhp").get<int>()-hp));
            const auto gained_mp=std::min(mana,std::max(0,character_.at("maxmp").get<int>()-mp));
            if(gained_hp||gained_mp){
                try{character_["hp"]=hp+gained_hp;character_["mp"]=mp+gained_mp;persist();}
                catch(...){character_["hp"]=hp;character_["mp"]=mp;scene_->restore_recovery(scene_key(),healing,mana);throw;}
                if(gained_hp)send(8,0,gauge(0x0800));
                if(gained_mp)send(8,1,gauge(0x0801));
            }
        }
        std::vector<QuestKillItems> reflectedRewards;
        auto reflectedKills=scene_->resolve_reflections(scene_key(),now(),[&](Entity&mob){reflectedRewards.push_back(commit_kill(mob));});
        for(const auto& rewards:reflectedRewards)send_quest_kill_items(rewards,send);
        if(reflectedKills){send(8,2,stats_packet());send(8,0,gauge(0x0800));send(8,1,gauge(0x0801));sync_ground(send);sync_quests(send);}
        sync_dungeon(send);
        scene_entities_=scene_->map(scene_map_name());
        refresh_party_auras(send);
        tick_mana_recovery(now(),send);
        publish_player(send);
    }
    if(now()<next_gm_poll_) return;
    next_gm_poll_=now()+1;
    poll_staff_channel(send);
    poll_guild_channel(send);
    const auto unread=gm_mail_latest_unread(store_.db(),eid(),now());
    if(unread>mailbox_notified_){
        // The client renders its localized new-mail message and refreshes an open mailbox.
        Bytes incoming(4);
        put32(incoming,0,1,true);
        send(16,4,incoming);
        mailbox_notified_=unread;
    }
    poll_guild_invites(send);
    if(guild_revision()!=guild_revision_)
        sync_guild(send,false);
    if(inventory_->expire(eid(),now())){
        recalculate();persist();send_inventory(send,true);
        send(8,2,stats_packet());send(8,0,gauge(0x0800));send(8,1,gauge(0x0801));
        refresh_self_appearance(send);
        publish_player(send);
    }
    bool expired=false;auto buffs=character_.value("active_buffs",Json::object());
    for(auto it=buffs.begin();it!=buffs.end();)if(it.value().at("expires").get<double>()<=now()){it=buffs.erase(it);expired=true;}else ++it;
    auto beforeEffects=character_,beforeStats=stats_;
    if(Consumables::tick(character_,now())||expired){
        try{character_["active_buffs"]=buffs;recalculate();persist();}
        catch(...){character_=std::move(beforeEffects);stats_=std::move(beforeStats);throw;}
        send(8,2,stats_packet());send(8,0,gauge(0x0800));send(8,1,gauge(0x0801));publish_player(send);
    }
    if(now()>=next_regeneration_){
        auto gain=regeneration_recovery(stats_,character_);auto before=character_;
        if(gain.first||gain.second){
            try{character_["hp"]=character_["hp"].get<int>()+gain.first;character_["mp"]=character_["mp"].get<int>()+gain.second;persist();}catch(...){character_=std::move(before);throw;}
            if(gain.first)send(8,0,gauge(0x0800));if(gain.second)send(8,1,gauge(0x0801));publish_player(send);
        }
        // Local policy: one live regeneration pulse per5s; no offline catch-up.
        next_regeneration_=now()+5;
    }
    NativeControl(root_).poll_save(identity_.id,[&]{persist();store_.db().exec("PRAGMA wal_checkpoint(PASSIVE)");});
    auto current_role=character_role(*roles_,identity_.identity.id,eid());
    if(current_role!=rendered_role_){rendered_role_=current_role;refresh_self_appearance(send);}
    scene_visibility(*scene_,*scene_entities_,character_.at("pos"),scene_visible_,send);
    sync_ground(send);
    publish_player(send);
    gm_world_poll(root_,store_.db(),*inventory_,eid(),map_name_,identity_.channel,
                  character_.at("pos"),gm_visible_,send,[&]{send_inventory(send,true);});
    sync_quests(send);
}
void WorldSession::publish_player(const Sender &send){
    if(!loaded_)return;
    sync_native_buffs(send);
    auto found=templates_.find(0x0401);
    if(found==templates_.end())throw std::runtime_error("Native movement carrier missing");
    auto combat=stats_;combat["hp"]=character_.at("hp");combat["mp"]=character_.at("mp");combat["level"]=character_.at("level");combat["attack_speed_multiplier"]=character_.value("attack_speed_multiplier",1.);
    combat["profession"]=character_.at("cls");
    combat["god_mode"]=character_.value("god_mode",false);
    combat["moral_value"]=character_.value("moral_value",std::int64_t(0));
    combat["pk_mode"]=pk_mode_&&character_.value("level",1)>35;
    combat["party_auras"]=active_party_auras(now());
    combat["cash_visibility"]=character_.value("cash_visibility",Json::array({0,0}));
    combat["mounted"]=mounted_&&mounted_speed()>0;
    combat["sitting"]=sitting_&&character_.value("hp",0)>0;
    if(combat["mounted"]==true&&mount_action_.size()==36){auto action=mount_action_;for(int i=0;i<3;i++)putfloat(action,13+i*4,character_["pos"][i]);putfloat(action,25,character_["angle"]);combat["mount_action"]=hex(action);}
    scene_->update_player(scene_key(),eid(),scene_map_name(),character_.at("pos"),appearance(),found->second,vanished_,send,combat);
}
void WorldSession::load_templates() {
    auto legacy = content_.get("server/world_core.json");
    for (const auto &record : legacy) {
        auto raw = unhex(record);
        if (raw.size() < 6)
            continue;
        int key = raw[0] * 256 + raw[1];
        if (!templates_.count(key))
            templates_[key] = Bytes(raw.begin() + 6, raw.end());
    }
    auto classes = content_.get("server/class_world_templates.json");
    auto source = classes.at(std::to_string(character_.at("cls").get<int>() / 100 * 100));
    for (auto it = source["packets"].begin(); it != source["packets"].end(); ++it) {
        unsigned a{}, b{};
        if (std::sscanf(it.key().c_str(), "%x/%x", &a, &b) != 2)
            throw std::runtime_error("Invalid packet catalog key");
        auto bytes = unhex(it.value().at("hex"));
        patch_entity(bytes, source.at("source_eid"), eid());
        templates_[static_cast<int>(a * 256 + b)] = bytes;
    }
    for (auto &[op, bytes] : templates_) {
        patch_entity(bytes, 0x2892, eid());
        patch_entity(bytes, 0x297e, eid());
    }
}
void WorldSession::persist() {
    if (!character_.contains("pos"))
        return;
    character_["map_id"]=map_id_;
    auto p = character_.at("pos");
    store_.update_character(identity_.identity.id, eid(), [&](Json &ch) {
        for (const auto &key :
             {"hp", "mp", "maxhp", "maxmp", "pos", "angle", "map", "map_id", "gear", "equipment_slots", "level", "exp", "fletta", "fletta_energy", "moral_value", "skill_points", "skills", "super_skill_progress", "fletta_paths", "skill_cooldowns", "active_buffs", "consumable_effects", "potion_cooldowns", "quickslots", "cash_visibility", "cls", "profession_grade"})
            if(character_.contains(key))ch[key] = character_.at(key);
        store_.db().exec(
            "INSERT INTO character_locations VALUES(?,?,?,?,?,?,?) ON CONFLICT(eid) DO UPDATE SET "
            "map_name=excluded.map_name,x=excluded.x,y=excluded.y,z=excluded.z,angle=excluded.angle,updated_"
            "at=excluded.updated_at",
            {std::int64_t(eid()), map_name_, p[0].get<double>(), p[1].get<double>(), p[2].get<double>(),
             character_["angle"].get<double>(), now()});
    });
}
static void gear_packet(Bytes &bytes, const Json &character, int first, int weapon) {
    if (character.contains("equipment_slots")) {
        for (int slot = 1; slot <= 30; slot++)
            put32(bytes, first + (slot - 1) * 4, character["equipment_slots"].value(std::to_string(slot), 0));
    } else {
        auto gear = character.at("gear");
        for (int i = 0; i < 4; i++)
            put32(bytes, first + i * 4, gear.at("armor")[i]);
        put32(bytes, weapon, gear.at("weapon"));
        put32(bytes, weapon + 4, gear.value("shield", 0));
    }
}
Bytes WorldSession::enter_packet() {
    auto bytes = templates_.at(0x0205);
    put32(bytes, 0, eid(), true);
    // AMo 0x6516ac copies the 02/05 character record from body +4:
    // class is its first LE16, and the profession grade follows at body +6.
    put16(bytes, 4, character_.at("cls"));
    bytes.at(6)=static_cast<unsigned char>(std::min(2u,character_.value("profession_grade",0u)));
    patch_appearance(bytes, 8, character_);
    put32(bytes, 16, character_.at("hp"));
    put32(bytes, 24, character_.at("mp"));
    put32(bytes, 44, character_.at("level"));
    gear_packet(bytes, character_, 127, 151);
    QuickSlots::patch_entry(character_,bytes);
    return bytes;
}
static Bytes native_guild_invitation(const Json&row,unsigned status){
 Bytes reply(14);put32(reply,0,row.at("inviter").get<std::uint32_t>(),true);put32(reply,4,row.at("target").get<std::uint32_t>(),true);
 put16(reply,8,status,true);put32(reply,10,row.at("guild_id").get<std::uint32_t>(),true);
 for(const char*field:{"target_name","guild_name","inviter_name"}){const auto text=row.at(field).get<std::string>();reply.insert(reply.end(),text.begin(),text.end());reply.push_back(0);}
 return reply;
}
void WorldSession::poll_guild_channel(const Sender&send){
 if(!inventory_||!loaded_)return;
 for(const auto&row:inventory_->guild_native_results(identity_.identity.id,eid(),guild_invite_result_after_,now())){
  send(12,4,native_guild_invitation(row,row.at("status").get<unsigned>()));guild_invite_result_after_=row.at("id");
 }
 for(const auto&row:inventory_->guild_departure_poll(identity_.identity.id,eid(),guild_departure_after_,now())){
  Bytes reply(10);put32(reply,0,row.at("actor").get<std::uint32_t>(),true);put32(reply,4,row.at("target").get<std::uint32_t>(),true);put16(reply,8,0xe7,true);
  send(12,5,reply);guild_departure_after_=row.at("id");
 }
 for(const auto&row:inventory_->guild_chat_poll(identity_.identity.id,eid(),guild_chat_after_,now())){
  Bytes reply(9);put32(reply,0,row.at("eid").get<std::uint32_t>(),true);put32(reply,4,3,true);
  const auto name=row.at("sender").get<std::string>(),message=row.at("message").get<std::string>();
  reply.insert(reply.end(),name.begin(),name.end());reply.push_back(0);
  reply.insert(reply.end(),message.begin(),message.end());reply.push_back(0);
  send(3,0,reply);guild_chat_after_=row.at("id");
 }
}
void WorldSession::poll_staff_channel(const Sender&send){
 auto role=staff_->role();bool allowed=role=="Helper"||((role=="Mod"||role=="GM"||role=="Admin")&&staff_->valid());
 if(!allowed){staff_chat_after_=staff_channel_->latest();return;}
 for(auto&row:staff_channel_->poll(staff_chat_after_)){
  // The receiver EID is local; the channel works across maps and WorldServers.
  auto message="[STAFF] "+row.at("sender").get<std::string>()+": "+row.at("message").get<std::string>();
  auto text=wide(message);for(std::size_t at=0;at<text.size();){auto end=std::min(at+std::size_t(108),text.size());if(end<text.size()&&text[end-1]>=0xd800&&text[end-1]<=0xdbff)--end;send(3,0,staff_chat_reply(eid(),utf8(text.substr(at,end-at))));at=end;}
  staff_chat_after_=row.at("id");
 }
}
void WorldSession::refresh_self_appearance(const Sender&send){
    // Own04/02 rebuilds riding state. Stats/gauges and observers update now;
    // defer the local rebuild until the captured dismount transition finishes.
    if(mounted_||mount_pending_){appearance_refresh_pending_=true;return;}
    send(4,2,appearance());appearance_refresh_pending_=false;
    // Own actor recreation invalidates the client's visible actor registry.
    // Invalidate this observer's delivery state so nearby actors are sent again.
    scene_visible_.clear();ground_visible_.clear();gm_visible_.clear();
    scene_->reset_observer(scene_key());
    if(loaded_){
        scene_entities_=scene_->map(scene_map_name());
        scene_visibility(*scene_,*scene_entities_,character_.at("pos"),scene_visible_,send);
        sync_ground(send);
        publish_player(send);
        sync_native_buffs(send,true);
    }
}
Bytes WorldSession::appearance() {
    auto bytes = templates_.at(0x0402);
    bytes[0] = 1;
    bytes[1] = 0;
    put32(bytes, 2, eid());
    for (int i = 0; i < 3; i++)
        putfloat(bytes, 6 + i * 4, character_["pos"][i]);
    putfloat(bytes, 18, character_.at("angle"));
    auto movement=stats_.at("move_speed").get<double>()*character_.value("move_speed_multiplier",1.);
    putfloat(bytes,38,movement);putfloat(bytes,42,stats_.at("walking_speed").get<double>()*character_.value("move_speed_multiplier",1.));
    putfloat(bytes,46,attack_rate(stats_.at("attack_speed").get<double>(),character_.value("attack_speed_multiplier",1.)));
    for (const auto &pair : std::vector<std::pair<int, const char *>>{
             {22, "hp"}, {26, "maxhp"}, {30, "mp"}, {34, "maxmp"}, {50, "level"}, {240, "cls"}})
        put32(bytes, pair.first, character_.at(pair.second));
    patch_appearance(bytes, 244, character_);
    gear_packet(bytes, character_, 72, 96);
    auto name = displayed_name(character_.at("name"),rendered_role_);
    if (name.size() > 200)
        throw std::runtime_error("Native name too long");
    bytes.resize(254);
    bytes[253] = static_cast<unsigned char>(name.size());
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.push_back(0);
    return bytes;
}
Bytes WorldSession::gauge(int opcode) {
    auto bytes = templates_.at(opcode);
    put32(bytes, 1, eid(), true);
    for (const auto &[offset, name] :
         std::vector<std::pair<int, const char *>>{{9, "hp"}, {13, "maxhp"}, {17, "mp"}, {21, "maxmp"}})
        put32(bytes, offset, character_.at(name));
    int level = character_.at("level");
    put16(bytes, 37, static_cast<std::uint16_t>(level));
    put32(bytes, 39, character_.value("moral_value", 0));
    if (bytes.size() >= 103) {
        put32(bytes, 51, character_.value("fletta_energy", 0));
        put32(bytes, 55, level <= 1 ? 125 : 125 * (level + 1));
        put32(bytes, 59, character_.value("fletta", 0));
        auto xp = character_.value("exp", std::uint64_t(0));
        auto base = progression_.threshold(level);
        xp = xp >= base ? xp - base : 0;
        auto requirement = progression_.requirement(level);
        put32(bytes, 63, static_cast<std::uint32_t>(xp));
        put32(bytes, 67, static_cast<std::uint32_t>(xp >> 32));
        put32(bytes, 71, static_cast<std::uint32_t>(requirement));
        put32(bytes, 75, static_cast<std::uint32_t>(requirement >> 32));
        put32(bytes, 87, character_.value("skill_points", 0));
    }
    putfloat(bytes, 25, stats_["move_speed"].get<double>()*character_.value("move_speed_multiplier",1.));
    putfloat(bytes, 29, stats_.at("walking_speed").get<double>() * character_.value("move_speed_multiplier",1.));
    putfloat(bytes, 33, attack_rate(stats_["attack_speed"].get<double>(),character_.value("attack_speed_multiplier",1.)));
    return bytes;
}
Bytes native_character_stats_packet(const Bytes&carrier,const Json&base,const Json&total,double attack_multiplier,double movement_multiplier){
    if(carrier.size()!=788||!std::isfinite(movement_multiplier)||movement_multiplier<=0)throw std::runtime_error("Invalid native character parameter carrier");
    auto bytes=carrier;
    // 00662D8A copies body+4 to the native784-byte hero parameter structure.
    // The second380-byte block is the intrinsic baseline, not another total.
    for(int shift:{0,380}){
        const auto&stats=shift?base:total;
        for(int i=0;i<5;i++){
            int origin=std::clamp(base.at("attributes")[i].get<int>(),0,65535),value=std::clamp(stats.at("attributes")[i].get<int>(),0,65535);
            put16(bytes,6+shift+2*i,std::min(origin,value));put16(bytes,18+shift+2*i,std::max(0,value-origin));
        }
        // Native label/getter proof: 0049B2D2,007BE998; offsets include actor4.
        for(auto [offset,name]:std::vector<std::pair<int,const char*>>{{32,"max_hp"},{36,"hp_regen"},{44,"max_mp"},{48,"mp_regen"},{60,"evasion"},{64,"block_chance"},{68,"accuracy"},{92,"defense_rating"},{96,"defense"},{120,"magic_defense"},{132,"critical_multiplier"},{164,"critical"},{292,"physical_reflection"},{304,"lifesteal"},{316,"mana_absorption"}})
            putfloat(bytes,offset+shift,stats.value(name,0.));
        putfloat(bytes,76+shift,stats.at("move_speed").get<double>()*(shift?1.:movement_multiplier));
        putfloat(bytes,72+shift,stats.at("move_speed").get<double>()*.695*(shift?1.:movement_multiplier));
        putfloat(bytes,88+shift,attack_rate(stats.at("attack_speed").get<double>(),shift?1.:attack_multiplier));
        putfloat(bytes,128+shift,100*stats.at("cast_speed").get<double>());
        // Native client adds equipped itemAttr damage itself. Sending the
        // final damage again here would count the weapon twice.
        putfloat(bytes,84+shift,stats.value("physical_intrinsic",stats.at("attack").get<double>()));
        putfloat(bytes,112+shift,stats.value("magic_intrinsic",stats.at("magic_attack").get<double>()));
        for(int offset:{104,116,148,152,156,160,272})putfloat(bytes,offset+shift,0.);
        putfloat(bytes,280+shift,stats.value("physical_factor",1.)-1);
        putfloat(bytes,284+shift,stats.value("magic_factor",1.)-1);
    }
    return bytes;
}
Bytes WorldSession::stats_packet() {
    auto baseline=calculated_stats(content_.get("data/balance_profiles.json"),character_.at("cls"),character_.at("level"));
    auto packet=native_character_stats_packet(templates_.at(0x0802),baseline,stats_,character_.value("attack_speed_multiplier",1.),character_.value("move_speed_multiplier",1.));
    putfloat(packet,72,stats_.at("walking_speed").get<double>()*character_.value("move_speed_multiplier",1.));
    return packet;
}
void WorldSession::enter(const Sender &send) {
    sitting_=false;
    send(2, 5, enter_packet());
    Bytes map(17);
    put32(map, 0, eid(), true);
    put32(map, 4, map_id_, true);
    const auto dungeon=dungeons_->rule(map_id_);
    if(dungeon.value("instanced",false)&&dungeon.value("entry_group",0)>0){map[8]=1;put32(map,9,dungeon.at("entry_group"),true);put32(map,13,map_id_,true);}
    send(4, 4, map);
}
void WorldSession::send_inventory(const Sender &send, bool snapshot) {
    auto bag = inventory_->items(eid()), equipment = inventory_->items(eid(), true);
    std::map<int, Bytes> current_bag, current_equipment;
    auto timestamp = now();
    Wallet wallet(store_.db(), *inventory_);
    auto cash = wallet.balance(eid());
    auto available_slots=inventory_->available(eid(),timestamp);
    for (auto item : bag) {
        if(!available_slots.count(item.slot))continue;
        if (item.expires && *item.expires <= timestamp)
            continue;
        if (item.item == 999999 && cash == 0)
            item.quantity = 0;
        current_bag[item.slot] = inventory_->update(eid(), item);
    }
    for (const auto &item : equipment)
        if (!item.expires || *item.expires > timestamp)
            current_equipment[item.slot] = inventory_->update(eid(), item, true);
    auto publish = [&](const auto &current, const auto &previous) {
        for (const auto &[slot, body] : previous)
            if (!current.count(slot)||current.at(slot)!=body) {
                auto removed = body;
                removed[12]=0;
                put32(removed, 21, 0);
                put32(removed, 25, 0);
                send(6, 6, removed);
            }
        for (const auto &[slot, body] : current)
            if (snapshot || !previous.count(slot) || previous.at(slot) != body)
                send(6, 6, body);
    };
    // The native equipment cache requires explicit 06/06 records before acknowledgement.
    publish(current_equipment, sent_equipment_);
    if (snapshot)
        for (const auto &body : inventory_->snapshots(eid(), timestamp))
            send(6, 5, body);
    publish(current_bag, sent_items_);
    if (snapshot)
        for (int slot : inventory_->available(eid(), timestamp))
            if (!current_bag.count(slot)) {
                ItemInstance empty;
                empty.slot = slot;
                empty.item = 999999;
                empty.uid = 1;
                empty.quantity = 0;
                send(6, 6, inventory_->update(eid(), empty, false, false));
            }
    sent_items_ = std::move(current_bag);
    sent_equipment_ = std::move(current_equipment);
    sync_quests(send);
}

bool WorldSession::attack_potion_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const Sender&send){
    if(cat!=6||sub!=2||body.size()!=16)return false;
    auto iid=get32(body,6,true);
    if(consumables_&&consumables_->supports(iid)){
        auto before=character_,beforeStats=stats_;
        bool committed=false;
        try{
            if(!loaded_||get32(body,0,true)!=eid()||body[10]!=0||get32(body,11,true)!=eid()||body[15]!=0||
               event.empty()||event.size()>118)throw std::runtime_error("Pedido de consumivel invalido");
            store_.update_character(identity_.identity.id,eid(),[&](Json& saved){
                auto& db=store_.db();
                if(!db.query("SELECT 1 FROM sqlite_master WHERE type='table' AND name='native_speed_potion_uses'").empty()){
                    auto legacy=db.query("SELECT eid,request FROM native_speed_potion_uses WHERE event_key=?",{event});
                    if(!legacy.empty()){
                        if(legacy[0].at("eid")!=eid()||legacy[0].at("request")!=hex(body))
                            throw std::runtime_error("Pedido repetido de pocao foi alterado");
                        return;
                    }
                }
                if(!consumables_->use(character_,Inventory::key(body[4],body[5]),iid,event,now(),stats_.value("healing",0.),map_id_))return;
                recalculate();
                for(auto field:{"consumable_effects","potion_cooldowns","hp","mp","maxhp","maxmp","skills",
                                "skill_points","active_buffs","fletta_paths"})
                    if(character_.contains(field))saved[field]=character_.at(field);
            });
            committed=true;
            send(6,2,body);send_inventory(send,true);send(8,2,stats_packet());
            send(8,0,gauge(0x0800));send(8,1,gauge(0x0801));
            if(character_.value("skills",Json::object())!=before.value("skills",Json::object()))
                send(5,5,skills_->list(character_,templates_.at(0x0505)));
            if(character_.value("fletta_paths",Json::object())!=before.value("fletta_paths",Json::object()))
                for(auto& allocation:skills_->allocations(character_))send(8,4,allocation);
            refresh_self_appearance(send);publish_player(send);
        }catch(const std::exception& e){
            if(!committed){character_=std::move(before);stats_=std::move(beforeStats);}
            send(11,1,notice(e.what()));
        }
        return true;
    }
    auto policy=content_.get("data/attack_speed_consumables.json");
    if(!policy.contains(std::to_string(iid)))return false;
    auto before=character_,old_stats=stats_;bool committed=false;
    try{
        if(!loaded_||character_.value("hp",0)<=0||get32(body,0,true)!=eid()||body[10]!=0||get32(body,11,true)!=eid()||body[15]!=0||event.empty()||event.size()>118)throw std::runtime_error("Pedido de pocao invalido");
        auto&db=store_.db();db.exec("CREATE TABLE IF NOT EXISTS native_speed_potion_uses(event_key TEXT PRIMARY KEY,eid INTEGER NOT NULL,request TEXT NOT NULL)");
        auto effect=policy.at(std::to_string(iid));auto stamp=now();auto request=hex(body);
        store_.update_character(identity_.identity.id,eid(),[&](Json&saved){
            auto previous=db.query("SELECT eid,request FROM native_speed_potion_uses WHERE event_key=?",{event});
            if(!previous.empty()){if(previous[0].at("eid")!=eid()||previous[0].at("request")!=request)throw std::runtime_error("Pedido repetido de pocao foi alterado");return;}
            auto item=inventory_->owned(eid(),Inventory::key(body[4],body[5]),iid,stamp);
            auto cooldowns=character_.value("potion_cooldowns",Json::object());auto key="item:"+std::to_string(iid);
            if(cooldowns.value(key,0.)>stamp)throw std::runtime_error("Aguarde a recarga da pocao");
            inventory_->consume(eid(),item);
            auto active=character_.value("consumable_effects",Json::object());effect["expires"]=stamp+effect.at("duration").get<double>();
            active[effect.at("group").get<std::string>()]=effect;character_["consumable_effects"]=active;
            cooldowns[key]=stamp+effect.at("cooldown").get<double>();character_["potion_cooldowns"]=cooldowns;
            recalculate();
            for(auto field:{"consumable_effects","potion_cooldowns","hp","mp","maxhp","maxmp"})saved[field]=character_.at(field);
            db.exec("INSERT INTO native_speed_potion_uses VALUES(?,?,?)",{event,std::int64_t(eid()),request});
        });committed=true;
        send(6,2,body);send_inventory(send,true);send(8,2,stats_packet());send(8,0,gauge(0x0800));refresh_self_appearance(send);publish_player(send);
    }catch(const std::exception&e){if(!committed){character_=before;stats_=old_stats;}send(11,1,notice(e.what()));}
    return true;
}
bool WorldSession::enchant_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const Sender&send){
 if(cat!=6||sub!=23)return false;
 auto before=character_,beforeStats=stats_;bool committed=false;
 try{
  if(!loaded_||character_.value("hp",0)<=0||body.size()!=43||get32(body,0,true)!=eid()||get16(body,28,true)!=0x74||body[42]>3||event.empty()||event.size()>128)throw std::runtime_error("Pedido de melhoria invalido");
  auto&enchantDb=store_.db();enchantDb.exec("CREATE TABLE IF NOT EXISTS native_enchant_operations(event_key TEXT PRIMARY KEY,eid INTEGER,request TEXT,response TEXT)");
  auto replay=enchantDb.query("SELECT eid,request,response FROM native_enchant_operations WHERE event_key=?",{event});
  if(!replay.empty()){if(replay[0].at("eid")!=eid()||replay[0].at("request")!=hex(body))throw std::runtime_error("Pedido repetido de melhoria alterado");send(6,23,unhex(replay[0].at("response")));send_inventory(send,true);return true;}
  // Observed 2026-10-28: BE32 category/slot/item triples, not packed addresses or UIDs.
  int category=get32(body,4,true),slot=get32(body,8,true);auto scroll=inventory_->owned(eid(),Inventory::key(category,slot),get32(body,12,true),now());
  bool weapon=scroll.item==60901;if(!weapon&&scroll.item!=60902)throw std::runtime_error("Use um pergaminho comum de arma ou armadura");
  int targetCategory=get32(body,16,true),targetSlot=get32(body,20,true);auto iid=get32(body,24,true);std::optional<ItemInstance>target;
  for(auto&i:inventory_->items(eid(),targetCategory==0))if(i.slot==(targetCategory==0?targetSlot:Inventory::key(targetCategory,targetSlot))&&i.item==iid)target=i;
  if(!target||target->quantity!=1||(target->expires&&*target->expires<=now()))throw std::runtime_error("Equipamento nao pertence ao personagem ou expirou");
  auto rule=inventory_->definition(iid);int equipSlot=rule.value("slot",0);if(rule.value("kind",0)!=1||(weapon?equipSlot!=7:!std::set<int>{1,2,3,4,8}.count(equipSlot)))throw std::runtime_error("Pergaminho incompativel com o equipamento");
  auto policy=content_.get("server/enchant_policy.json");int grade=target->grade+1;
  if(grade>policy.at("max_grade").get<int>())throw std::runtime_error("Equipamento ja esta no grau maximo");
  enchant_row(content_.get("data/item_attrs.json").at(std::to_string(iid)),grade);
  double chance=0;if(grade>13)chance=policy.at("local_chances").at(std::to_string(grade));else{auto rows=content_.database("database/content_typed.sqlite")->query("SELECT o12_f64 AS chance FROM ItemEnchant WHERE o0_i=0 AND o4_i=? AND o8_i=? LIMIT 1",{std::int64_t(grade),std::int64_t(weapon?1:2)});if(rows.empty())throw std::runtime_error("Probabilidade nativa ausente");chance=rows[0].at("chance");}
  if(!std::isfinite(chance)||chance<0||chance>100||!policy.at("safe_failure").get<bool>())throw std::runtime_error("Politica de melhoria invalida");
  bool optional=get32(body,30,true)!=0xffffffffu||get32(body,34,true)!=0xffffffffu||get32(body,38,true)!=0xffffffffu;
  if(optional){auto extra=inventory_->owned(eid(),Inventory::key(get32(body,30,true),get32(body,34,true)),get32(body,38,true),now());if(extra.item!=99991||chance<100)throw std::runtime_error("Bonus especial ainda sem formula comprovada; materiais preservados. Retire a opcao extra para melhoria comum.");}
  auto&db=store_.db();db.exec("CREATE TABLE IF NOT EXISTS native_enchant_operations(event_key TEXT PRIMARY KEY,eid INTEGER,request TEXT,response TEXT)");
  Bytes response;
  {Transaction tx(db);auto prior=db.query("SELECT eid,request,response FROM native_enchant_operations WHERE event_key=?",{event});
   if(!prior.empty()){if(prior[0].at("eid")!=eid()||prior[0].at("request")!=hex(body))throw std::runtime_error("Pedido repetido de melhoria alterado");response=unhex(prior[0].at("response"));}
   else{auto random=random_bytes(4);bool success=get32(random,0)/4294967296.*100<chance;inventory_->consume(eid(),scroll,1,"enhancement-scroll");if(success)db.exec("INSERT INTO item_enchantments VALUES(?,?) ON CONFLICT(uid) DO UPDATE SET grade=excluded.grade",{std::int64_t(target->uid),std::int64_t(grade)});response=body;put16(response,28,success?0x74:0x75,true);recalculate();persist();db.exec("INSERT INTO native_enchant_operations VALUES(?,?,?,?)",{event,std::int64_t(eid()),hex(body),hex(response)});}
   tx.commit();committed=true;
  }
  send(6,23,response);send_inventory(send,true);send(8,2,stats_packet());refresh_self_appearance(send);publish_player(send);
 }catch(const std::exception&e){if(!committed){character_=before;stats_=beforeStats;}send(11,1,notice(e.what()));}
 return true;
}
std::string WorldSession::guild_revision() {
    const auto rows=store_.db().query(
        "SELECT m.guild_id,g.name,p.notice,p.rank_names,"
        "(SELECT group_concat(eid||':'||rank) FROM (SELECT eid,rank FROM native_guild_members "
        "WHERE guild_id=m.guild_id ORDER BY eid)) AS roster,"
        "(SELECT group_concat(rank||':'||rights) FROM (SELECT rank,rights FROM native_guild_storage_rights "
        "WHERE guild_id=m.guild_id ORDER BY rank)) AS item_rights,"
        "(SELECT group_concat(rank||':'||rights) FROM (SELECT rank,rights FROM native_guild_carats_rights "
        "WHERE guild_id=m.guild_id ORDER BY rank)) AS carat_rights "
        "FROM native_guild_members m JOIN native_guilds g ON g.id=m.guild_id "
        "LEFT JOIN native_guild_profiles p ON p.guild_id=g.id WHERE m.eid=? AND m.account_id=?",
        {std::int64_t(eid()),identity_.identity.id});
    return rows.empty()?std::string():rows.front().dump();
}

void WorldSession::sync_guild(const Sender &send, bool overlay) {
    const auto revision=guild_revision();
    auto &db=store_.db();
    const auto member=db.query("SELECT m.guild_id,m.rank,g.name,p.notice,p.rank_names FROM native_guild_members m "
                               "JOIN native_guilds g ON g.id=m.guild_id LEFT JOIN native_guild_profiles p ON p.guild_id=g.id "
                               "WHERE m.eid=? AND m.account_id=?",{std::int64_t(eid()),identity_.identity.id});
    Json state={{"cmd","guild"},{"member",!member.empty()},{"found_level",10},{"found_karats",50000},{"wait",0}};
    if(member.empty()){
        guild_revision_=revision;
        if(overlay)quest_overlay(state,send);
        return;
    }
    const auto &m=member[0];
    const auto guild=m.at("guild_id").get<std::int64_t>();
    const auto rank=m.at("rank").get<int>();
    const Json defaults={"Sem cargo","Membro 3","Membro 2","Membro 1","Oficial","Lider"};
    Json names=Json::array({"","","","","",""});
    if(!m.at("rank_names").is_null())names=Json::parse(m.at("rank_names").get<std::string>());
    auto grades=defaults;
    for(int i=1;i<=5;++i)if(!names.at(i).get<std::string>().empty())grades[i]=names[i];
    unsigned rights=7;
    for(const auto &r:db.query("SELECT rank,rights FROM native_guild_storage_rights WHERE guild_id=? AND rank BETWEEN 1 AND 3",{guild})){
        const auto tab=3-r.at("rank").get<int>();
        const auto flags=r.at("rights").get<unsigned>();
        rights=(rights&~(1u<<tab))|((flags&2u)?1u<<tab:0);
        if(flags&4u)rights|=1u<<(3+tab);
    }
    for(const auto &r:db.query("SELECT rank,rights FROM native_guild_carats_rights WHERE guild_id=? AND rank BETWEEN 1 AND 3",{guild})){
        const auto tab=3-r.at("rank").get<int>();
        const auto flags=r.at("rights").get<unsigned>();
        if(flags&1u)rights|=1u<<(6+tab);
        if(flags&2u)rights|=1u<<(9+tab);
    }
    Json roster=Json::array();
    Sqlite runtime(root_/"Database/runtime.sqlite",true);
    const auto online=runtime.query("SELECT eid FROM native_player_leases WHERE expires>?",{now()});
    for(const auto &row:db.query("SELECT m.eid,m.rank,a.payload FROM native_guild_members m "
                                 "JOIN accounts a ON a.id=m.account_id WHERE m.guild_id=? ORDER BY m.rank DESC,m.eid",{guild})){
        const auto id=row.at("eid").get<std::uint32_t>();
        const auto characters=Json::parse(row.at("payload").get<std::string>()).value("chars",Json::array());
        for(const auto &ch:characters)if(ch.value("eid",0u)==id){
            const bool active=std::any_of(online.begin(),online.end(),[&](const auto &a){return a.at("eid")==id;});
            roster.push_back({{"id",id},{"name",ch.value("name","")},{"level",ch.value("level",1)},
                              {"class",std::to_string(ch.value("cls",0))},{"grade",row.at("rank")},
                              {"online",active},{"where",ch.value("map","")}});
        }
    }
    state.update({{"id",guild},{"name",m.at("name")},{"grade",rank},{"rank",grades[rank]},
                  {"master",rank==5},{"leads",rank>=4},{"me",eid()},{"members",roster},{"level",1},{"cap",50},
                  {"grades",grades},{"defaults",defaults},{"own",names},{"rights",rights},
                  {"notice",m.at("notice").is_null()?Json(""):m.at("notice")},{"notice_max",150},
                  {"points",0},{"flasks",0},{"karats",Wallet(db,*inventory_).balance(eid())},
                  {"wars",Json::array()},{"mark",Json::array()}});
    const auto leader=std::find_if(roster.begin(),roster.end(),[](const Json &row){
        return row.at("grade")==5;
    });
    if(leader!=roster.end()){
        // The AMo GuildInfo handler copies the 180-byte prefix into guild state at +4.
        Bytes info(180);
        put32(info,0,static_cast<std::uint32_t>(guild));
        put32(info,4,leader->at("id").get<std::uint32_t>());
        put32(info,20,1);
        for(unsigned tab=0;tab<3;++tab){
            put32(info,0x34+tab*4,(rights>>tab)&1u);
            put32(info,0x40+tab*4,(rights>>(3+tab))&1u);
            put32(info,0x4c+tab*4,(rights>>(6+tab))&1u);
            put32(info,0x58+tab*4,(rights>>(9+tab))&1u);
        }
        auto append=[](Bytes &packet,const std::string &value){
            packet.insert(packet.end(),value.begin(),value.end());
            packet.push_back(0);
        };
        append(info,m.at("name").get<std::string>());
        append(info,leader->at("name").get<std::string>());
        info.insert(info.end(),3,0);
        append(info,state.at("notice").get<std::string>());
        for(const auto &grade:grades)append(info,grade.get<std::string>());
        send(12,0,info);

        auto send_member=[&](unsigned context,std::uint32_t id,int level,int job,int grade,const std::string &name){
            Bytes packet(36);
            put32(packet,0,context,true);
            put32(packet,8,id);
            put32(packet,12,level);
            put32(packet,16,job);
            put32(packet,20,grade);
            append(packet,name);
            packet.push_back(0);
            send(12,1,packet);
        };
        // Context 4 replaces the roster. Context 3 adds each remaining member silently.
        send_member(4,eid(),character_.value("level",1),character_.value("cls",0),rank,
                    character_.at("name").get<std::string>());
        for(const auto &row:roster){
            const auto id=row.at("id").get<std::uint32_t>();
            if(id!=eid())
                send_member(3,id,row.at("level").get<int>(),std::stoi(row.at("class").get<std::string>()),
                            row.at("grade").get<int>(),row.at("name").get<std::string>());
        }
    }
    if(overlay)quest_overlay(state,send);
    guild_revision_=revision;
}

void WorldSession::poll_guild_invites(const Sender &send) {
    const auto stamp=now();
    std::set<std::string> current;
    for(const auto &row:store_.db().query("SELECT i.token,i.expires,i.inviter,i.target,i.guild_id,g.name,g.name AS guild_name,"
              "a.name AS inviter_name,b.name AS target_name,CASE WHEN w.token IS NULL THEN 0 ELSE 1 END AS native "
              "FROM native_guild_invites i JOIN native_guilds g ON g.id=i.guild_id "
              "JOIN native_public_character_names a ON a.character_eid=i.inviter "
              "JOIN native_public_character_names b ON b.character_eid=i.target "
              "LEFT JOIN native_guild_invite_wire w ON w.token=i.token WHERE i.target=? AND i.target_account=? AND i.expires>? "
              "ORDER BY i.expires DESC LIMIT 10",{std::int64_t(eid()),identity_.identity.id,stamp})){
        const auto id=row.at("token").get<std::string>();
        current.insert(id);
        if(!shown_guild_invites_.count(id)&&row.at("native").get<int>())send(12,4,native_guild_invitation(row,0xdb));
        else if(!shown_guild_invites_.count(id))
            quest_overlay({{"cmd","ask"},{"kind","guild"},{"id",id},{"name",row.at("name")},
                           {"ms",static_cast<int>((row.at("expires").get<double>()-stamp)*1000)},{"term",60000},
                           {"yes","guild.accept "+id},{"no","guild.decline "+id}},send);
    }
    for(const auto &id:shown_guild_invites_)if(!current.count(id))quest_overlay({{"cmd","ask.off"},{"id",id}},send);
    shown_guild_invites_=std::move(current);
}

void WorldSession::save_guild_profile(unsigned flags,const std::string &message,const Json &names) {
    auto &db=store_.db();
    const auto &account=identity_.identity.id;
    Transaction tx(db);
    for(unsigned tab=0;tab<3;++tab)
        inventory_->guild_storage_set_rights(account,eid(),3-tab,1u|
            ((flags&(1u<<tab))?2u:0)|((flags&(1u<<(3+tab)))?4u:0)|
            ((flags&(1u<<(6+tab)))?8u:0)|((flags&(1u<<(9+tab)))?16u:0));
    const auto guild=inventory_->guild_storage_membership_id(account,eid());
    db.exec("INSERT INTO native_guild_profiles VALUES(?,?,?) ON CONFLICT(guild_id) "
            "DO UPDATE SET notice=excluded.notice,rank_names=excluded.rank_names",
            {guild,message,names.dump()});
    tx.commit();
}

bool WorldSession::guild_request(unsigned char cat,unsigned char sub,const Bytes &body,const Sender &send,const std::string&event) {
    if(cat==12&&sub==4){
        try{
            if(!loaded_||body.size()<17||body.size()>512||get32(body,10,true)!=0)throw std::runtime_error("Invalid native guild invitation");
            std::size_t at=14;std::vector<std::string>texts;
            for(int n=0;n<3;++n){const auto end=std::find(body.begin()+at,body.end(),0);if(end==body.end()||end-(body.begin()+at)>128)throw std::runtime_error("Invalid invitation text");texts.emplace_back(body.begin()+at,end);wide(texts.back());at=static_cast<std::size_t>(end-body.begin())+1;}
            if(at!=body.size())throw std::runtime_error("Trailing invitation data");
            const auto inviter=get32(body,0,true),target=get32(body,4,true);const auto code=get16(body,8,true);
            if(code==0xdb){
                if(inviter!=eid())throw std::runtime_error("Invalid guild inviter");
                const auto row=inventory_->guild_native_invite(identity_.identity.id,eid(),target,texts[0],event,now());
                send(12,4,native_guild_invitation(row,0xdb));
            }else if(code==0xdd||code==0xdc||code==0x271){
                if(target!=eid())throw std::runtime_error("Invalid guild invite recipient");
                inventory_->guild_native_answer(identity_.identity.id,eid(),inviter,code==0xdd,event,now());
                poll_guild_channel(send);sync_guild(send,false);poll_guild_invites(send);
            }else throw std::runtime_error("Unsupported guild invitation code");
        }catch(const std::exception&e){send(11,1,service_notice(identity_.identity,e.what()));}
        return true;
    }
    if(cat==12&&sub==5){
        try{
            if(!loaded_||body.size()!=10||get32(body,0,true)!=eid()||get16(body,8,true)!=0xe7)
                throw std::runtime_error("Invalid guild departure request");
            inventory_->guild_depart(identity_.identity.id,eid(),get32(body,4,true),event,now());
            poll_guild_channel(send);sync_guild(send,false);
        }catch(const std::exception&e){send(11,1,service_notice(identity_.identity,e.what()));}
        return true;
    }

    if(cat==12&&sub==0x10){
        try{
            // CGuildInfoAllchange: BE character, guild, status, twelve rights and seven C strings.
            constexpr std::size_t prefix=58;
            if(!loaded_||body.size()<prefix+7||body.size()>prefix+7+150+250||
               get32(body,0,true)!=eid()||get16(body,8,true)!=0xee||
               get32(body,4,true)!=inventory_->guild_storage_membership_id(identity_.identity.id,eid()))
                throw std::runtime_error("Pedido de cla invalido");
            unsigned flags=0;
            for(unsigned i=0;i<12;++i){
                const auto value=get32(body,10+i*4,true);
                if(value>1)throw std::runtime_error("Permissoes de deposito invalidas");
                flags|=value<<i;
            }
            auto cursor=body.begin()+prefix;
            auto read=[&](std::size_t limit){
                const auto end=std::find(cursor,body.end(),0);
                if(end==body.end()||static_cast<std::size_t>(end-cursor)>limit)
                    throw std::runtime_error("Texto de cla invalido");
                std::string value(cursor,end);
                cursor=end+1;
                wide(value);
                return value;
            };
            const auto message=read(150);
            read(0); // Reserved grade zero.
            Json names=Json::array({""});
            for(unsigned rank=1;rank<=5;++rank)names.push_back(read(50));
            if(cursor!=body.end())throw std::runtime_error("Pedido de cla invalido");
            save_guild_profile(flags,message,names);
            sync_guild(send,false);
            send(11,1,service_notice(identity_.identity,"Permissoes do cla salvas."));
        }catch(const std::exception &e){send(11,1,service_notice(identity_.identity,e.what()));}
        return true;
    }
    if(cat!=7||sub!=1||body.size()<7||body.size()>4096||body.back()!=0)return false;
    std::string text(body.begin(),body.end()-1);
    if(text.rfind("npcshop guild.",0)==0)text.erase(0,8);
    // The existing karos_aa overlay bridge sends "guild action" for a "guild.action" click.
    if(text.rfind("guild ",0)==0)text[5]='.';
    if(text.rfind("guild.",0)!=0)return false;
    try{
        if(!loaded_||text.find('\0')!=std::string::npos)throw std::runtime_error("Pedido de cla invalido");
        const auto args=command_tokens(text);
        const auto &account=identity_.identity.id;
        auto decode=[](const std::string &encoded,std::size_t limit){
            if(encoded=="-")return std::string();
            if(encoded.size()>2*limit)throw std::runtime_error("Texto de cla muito longo");
            const auto bytes=unhex(encoded);
            std::string value(bytes.begin(),bytes.end());
            if(value.find('\0')!=std::string::npos)throw std::runtime_error("Texto de cla invalido");
            wide(value);
            return value;
        };
        if(args==std::vector<std::string>{"guild.open"}||args==std::vector<std::string>{"guild.get"}){
            sync_guild(send);return true;
        }
        if(args.size()==2&&args[0]=="guild.invite"){
            inventory_->guild_invite(account,eid(),decode(args[1],64),now());
            quest_overlay({{"cmd","guild.said"},{"word","invited"}},send);
        }else if(args.size()==2&&(args[0]=="guild.accept"||args[0]=="guild.decline")){
            if(inventory_->guild_invite_answer(account,eid(),args[1],args[0]=="guild.accept",now()))
                quest_overlay({{"cmd","news"},{"kind","guild"},{"name",character_.at("name")}},send);
            poll_guild_invites(send);
        }else if(args.size()==3&&args[0]=="guild.grade"){
            if(args[2].size()!=1||args[2][0]<'0'||args[2][0]>'5')throw std::runtime_error("Cargo invalido");
            std::size_t parsed=0;
            const auto target=std::stoull(args[1],&parsed);
            if(parsed!=args[1].size()||target==0||target>0xffffffffu)throw std::runtime_error("Personagem invalido");
            inventory_->guild_change_rank(account,eid(),static_cast<std::uint32_t>(target),args[2][0]-'0');
            quest_overlay({{"cmd","guild.said"},{"word","graded"}},send);
        }else if(args.size()==8&&args[0]=="guild.edit"){
            std::size_t parsed=0;
            const auto flags=std::stoul(args[1],&parsed);
            if(parsed!=args[1].size()||flags>4095)throw std::runtime_error("Permissoes de deposito invalidas");
            Json names=Json::array({""});
            for(std::size_t i=3;i<args.size();++i)names.push_back(decode(args[i],50));
            save_guild_profile(flags,decode(args[2],150),names);
            quest_overlay({{"cmd","guild.said"},{"word","saved"}},send);
        }else throw std::runtime_error("Pedido de cla ainda indisponivel");
        sync_guild(send);
    }catch(const std::exception &e){send(11,1,notice(e.what()));}
    return true;
}

bool WorldSession::handle(unsigned char cat, unsigned char sub, const Bytes &body, const Sender &send,const std::string &event) {
    if(character_.value("debug_mode",false))try{RuntimeLog(root_/"Logs"/("Debug-"+std::to_string(eid())+".jsonl")).write("packet","category="+std::to_string(cat)+" sub="+std::to_string(sub)+" bytes="+std::to_string(body.size()));}catch(...){}
    if(costume_request(cat,sub,body,event,send))return true;
    if(revival_request(cat,sub,body,event,send))return true;
    if(travel_request(cat,sub,body,event,send))return true;
    if(mine_registration_request(cat,sub,body,send))return true;
    if(dungeon_request(cat,sub,body,send))return true;
    if(mount_request(cat,sub,body,send))return true;
    if(super_skill_request(cat,sub,body,event,send))return true;
    if(super_stone_request(cat,sub,body,event,send))return true;
    if(crafting_request(cat,sub,body,event,send))return true;
    if(active_skill_request(cat,sub,body,event,send))return true;
    if(quest_request(cat,sub,body,event,send))return true;
    if(premium_request(cat,sub,body,event,send))return true;
    if(guild_request(cat,sub,body,send,event))return true;
    if(attack_potion_request(cat,sub,body,event,send))return true;
    if(enchant_request(cat,sub,body,event,send))return true;
    if(cat==6&&sub==0x21){
        if(!loaded_||body.size()!=26)return true;
        try{
            if(character_.value("hp",0)<=0)throw std::runtime_error("Personagem indisponivel");
            native_consignment_register(store_.db(),*inventory_,eid(),body,event,now());
        }catch(const std::exception &error){send(11,1,notice(error.what()));return true;}
        auto reply=body;
        put16(reply,24,0xcb,true);
        send(cat,sub,reply);
        send_inventory(send,true);
        try{
            send(6,0x1e,native_consignment_seller_page(store_.db(),*inventory_,eid(),now()));
        }catch(const std::exception &error){send(11,1,notice(error.what()));}
        return true;
    }
    if(cat==6&&(sub==0x1e||sub==0x24)){
        if(!loaded_)return true;
        if(sub==0x1e){
            if(body.size()==52&&get16(body,50,true)==0xcb&&
               std::all_of(body.begin(),body.begin()+50,[](unsigned char byte){return byte==0;})){
                try{send(cat,sub,native_consignment_seller_page(store_.db(),*inventory_,eid(),now()));}
                catch(const std::exception &error){send(11,1,notice(error.what()));}
                return true;
            }
            consignment_query_.clear();
            try{
                auto response=native_consignment_search(store_.db(),*inventory_,body,now(),&consignment_quotes_);
                consignment_query_=body;
                send(cat,sub,response);
            }catch(const std::exception &error){send(11,1,notice(error.what()));}
            return true;
        }
        if(body.size()!=10||get16(body,8,true)!=0)return true;
        const auto listing=get32(body,4,true);
        auto reply=body;
        try{
            const auto quote=consignment_quotes_.find(listing);
            if(character_.value("hp",0)<=0||quote==consignment_quotes_.end())
                throw std::runtime_error("Anuncio indisponivel. Atualize a busca.");
            consignment_buy(store_.db(),*inventory_,eid(),listing,quote->second,now());
            consignment_quotes_.erase(quote);
        }catch(const std::exception &error){
            put16(reply,8,0xd0,true);
            send(cat,sub,reply);
            send(11,1,notice(error.what()));
            return true;
        }

        put16(reply,8,0xcb,true);
        send(cat,sub,reply);
        send_inventory(send,true);
        Bytes removed(12);
        put32(removed,4,1,true);
        put32(removed,8,listing);
        send(6,0x20,removed);
        put32(removed,0,1,true);
        send(6,0x20,removed);
        try{
            send(6,0x1e,native_consignment_search(store_.db(),*inventory_,consignment_query_,now(),&consignment_quotes_));
        }catch(const std::exception &error){send(11,1,notice(error.what()));}
        return true;
    }
    if(cat==6&&sub==0x55){
        auto before=character_,beforeStats=stats_;bool committed=false;
        try{
            if(!loaded_||character_.value("hp",0)<=0)throw std::runtime_error("Entre no mapa e mantenha o personagem vivo");
            Bytes reply;{Transaction tx(store_.db());reply=inventory_->additional_attributes(eid(),body,event,now());recalculate();persist();tx.commit();committed=true;}
            send_inventory(send,true);send(8,2,stats_packet());send(8,0,gauge(0x0800));send(8,1,gauge(0x0801));send(6,0x55,reply);publish_player(send);
        }catch(const std::exception&e){if(!committed){character_=before;stats_=beforeStats;}send(11,1,notice(e.what()));}
        return true;
    }
    // This Genesis client forwards its explicit two-character command prefix
    // through 07/01, not the legacy 03/08 chat-command transport.
    if(cat==7&&sub==1&&loaded_&&body.size()>1&&body.size()<=512&&body.back()==0&&std::find(body.begin(),body.end()-1,0)==body.end()-1){
        std::string text(body.begin(),body.end()-1);auto normalized=normalized_native_command(8,text);
        if(normalized){Bytes command(7);command[5]=25;auto name=character_.at("name").get<std::string>();command.insert(command.end(),name.begin(),name.end());command.push_back(0);command.insert(command.end(),normalized->begin(),normalized->end());command.push_back(0);return handle(3,8,command,send,event);}
    }
    if(cat==8&&sub==3&&loaded_){profession_request(body,event,send);return true;}
    if(loaded_&&((cat==5&&(sub==6||sub==7))||(cat==8&&sub==4)||(cat==6&&(sub==11||sub==12||sub==82)))){skill_request(cat,sub,body,event,send);return true;}
    if(loaded_)scene_entities_=scene_->map(scene_map_name());
    if(cat==5&&(sub==0||sub==1)&&loaded_){
        auto pending=casting_->state();bool applied=false;std::uint64_t appliedDamage=0;
        std::vector<QuestKillItems> rewards;
        auto killed=[&](Entity&mob){rewards.push_back(commit_kill(mob));};
        try{
            admin_tick(send);auto before=character_.value("exp",std::uint64_t(0));auto moralBefore=character_.value("moral_value",std::int64_t(0));Json skill=Json::object();if(sub==1&&body.size()>5&&body[5]==1)skill=casting_->impact(body,eid(),now());
            if(!skill.empty()&&(skill.at("kind")==21||skill.at("kind")==23||skill.at("kind")==25||skill.at("kind")==28)){
                auto pos=character_.at("pos");auto carrier=unhex(content_.get("server/combat_templates.json").at("0502"));auto hit=combat_damage(carrier,eid(),skill.at("target"),0,{pos[0],pos[1],pos[2]},false,skill.at("id"),true);hit[5]=1;hit[15]=0;hit[25]=0;send(5,2,hit);scene_->action(scene_key(),5,2,hit);return true;
            }
            if(!skill.empty()){
                auto pos=character_.at("pos");auto current=scene_->map(scene_map_name());for(auto&target:skill.at("targets")){auto found=current->find(target.get<std::uint32_t>());if(found==current->end()||!found->second.attackable(pk_mode_&&character_.value("level",1)>35)||found->second.position.distance({pos[0],pos[1],pos[2]})>350)throw std::runtime_error("Alvo da habilidade fora de alcance");}
                for(auto&target:skill.at("targets")){auto hit=body;hit.resize(32);hit[26]=1;put32(hit,28,target,true);auto profile=skill;profile["target"]=target;appliedDamage+=scene_->strike(scene_key(),sub,hit,now(),killed,profile);applied=true;}
            }else{appliedDamage=scene_->strike(scene_key(),sub,body,now(),killed);applied=true;}
            stand(send);
            auto recovery=attack_recovery(stats_,character_,appliedDamage);if(recovery.first||recovery.second){character_["hp"]=character_["hp"].get<int>()+recovery.first;character_["mp"]=character_["mp"].get<int>()+recovery.second;persist();if(recovery.first)send(8,0,gauge(0x0800));if(recovery.second)send(8,1,gauge(0x0801));publish_player(send);}
            if(character_.value("exp",std::uint64_t(0))!=before){send(8,2,stats_packet());send(8,0,gauge(0x0800));send(8,1,gauge(0x0801));sync_ground(send);publish_player(send);}
            else if(character_.value("moral_value",std::int64_t(0))!=moralBefore){send(8,2,stats_packet());refresh_self_appearance(send);publish_player(send);}
            sync_quests(send);
        }catch(const std::exception&e){if(!applied)casting_->restore(pending);send(11,1,notice(e.what()));}
        for(const auto& items:rewards)send_quest_kill_items(items,send);
        return true;
    }
    if(cat==6&&sub==3&&loaded_){
        Json row;
        try{
            if(character_.value("hp",0)<=0)
                throw std::runtime_error("Personagem morto nao pode coletar itens");
            auto &db=store_.db();
            Transaction tx(db);
            if(!event.empty()&&!db.query("SELECT 1 FROM inventory_operations WHERE event_key=?",{event}).empty())
                throw std::runtime_error("Pedido de coleta ja processado");
            row=loot_->pickup(eid(),map_scope(),character_.at("pos"),body,now());
            if(!event.empty())
                db.exec("INSERT INTO inventory_operations VALUES(?,?,?,?,?)",
                        {event,std::int64_t(eid()),std::string("ground-pickup"),hex(body),std::string("null")});
            tx.commit();
        }catch(const std::exception &error){send(11,1,service_notice(identity_.identity,error.what()));return true;}
        ground_visible_.erase(get32(body,4,true));
        send(6,3,body);
        send_inventory(send,true);
        const auto &locale=identity_.identity.locale;
        const char *received=locale=="es"?"Objeto recibido: ":locale=="en"?"Item received: ":"Item recebido: ";
        send(11,1,notice(received+inventory_->name(row.at("item_id"))));
        return true;
    }
    if(cat==6&&loaded_&&(sub==8||sub==0x41||sub==0x51||sub==0x46||sub==0x0c)){
        try{
            if(sub==8){inventory_->move(eid(),body);send_inventory(send,true);send(cat,sub,body);}
            else if(sub==0x41){inventory_->trash(eid(),body,event);send_inventory(send,false);send(cat,sub,Bytes(body.begin(),body.begin()+26));}
            else if(sub==0x51){inventory_->organize(eid(),body);send_inventory(send,true);Bytes reply(10);put32(reply,0,eid(),true);put16(reply,8,0x22a,true);send(cat,sub,reply);}
            else if(sub==0x46){
                // AMo 0049AABB serializes actor+target+two flags (10 bytes).
                // CSetCostumePacket 0065989B reads actor+two flags (6 bytes).
                auto offset=body.size()==10?8u:4u;
                if((body.size()!=6&&body.size()!=10)||get32(body,0,true)!=eid()||(body.size()==10&&get32(body,4,true)!=eid())||body[offset]>1||body[offset+1]>1)throw std::runtime_error("Invalid item control");
                auto before=character_;try{character_["cash_visibility"]={body[offset],body[offset+1]};persist();}catch(...){character_=before;throw;}
                Bytes reply(6);put32(reply,0,eid(),true);reply[4]=body[offset];reply[5]=body[offset+1];send(cat,sub,reply);scene_->action(scene_key(),cat,sub,reply);publish_player(send);
            }
            else if(body.size()!=8||get32(body,0,true)!=eid())throw std::runtime_error("Invalid item channel request");
        }catch(const std::exception&e){send_inventory(send,true);if(sub==0x51){Bytes reply(10);put32(reply,0,eid(),true);put16(reply,8,0x22a,true);send(cat,sub,reply);}send(11,1,service_notice(identity_.identity,e.what()));}
        return true;
    }
    if(cat==3&&(sub==0||sub==8)&&loaded_){
        try{
            RuntimeLog(root_/"Logs"/("CommandTransport-"+std::to_string(eid())+".jsonl")).write("received","transport=3/"+std::to_string(sub)+" bytes="+std::to_string(body.size()));
            auto message=client_text(sub,body,eid());
            auto normalized=normalized_native_command(sub,message);
            if(sub==8||message.rfind("//",0)==0||normalized){
                if(!normalized){send(3,0,staff_chat_reply(eid(),"Comando desconhecido. Use //staffhelp ou //gmhelp."));return true;}
                auto args=command_tokens(*normalized);auto response=staff_->authentication(args);
                if(response){send(3,0,staff_chat_reply(eid(),*response));return true;}
                if(args.empty()){send(3,0,staff_chat_reply(eid(),"Use //staffhelp, //gmstatus ou //gmhelp para consultar os comandos."));return true;}
                staff_command(args,event,send);
            }else{
                Sqlite moderation(root_/"Database/moderation.sqlite",true);bool muted=false;
                for(auto &subject:std::vector<std::pair<std::string,std::string>>{{"character",std::to_string(eid())},{"account",identity_.identity.id},{"hardware",hex(identity_.fingerprint)}})
                    if(!moderation.query("SELECT id FROM sanctions WHERE kind='mute' AND scope=? AND subject=? AND revoked IS NULL AND (expires IS NULL OR expires>?)",{subject.first,subject.second,now()}).empty())muted=true;
                if(muted)throw std::runtime_error("Chat bloqueado pela moderacao");
                const auto channel=get32(body,4,true);
                const auto sender=displayed_name(character_.at("name"),rendered_role_,30);
                if(channel==3){
                    inventory_->guild_chat_post(identity_.identity.id,eid(),sender,message,event,now());
                    poll_guild_channel(send);
                }else scene_->chat(scene_key(),sender,message,channel);
            }
        }catch(const std::exception&e){send(3,0,staff_chat_reply(eid(),e.what()));}
        return true;
    }
    if(cat==6&&(sub==13||sub==14)){
        if(!loaded_)throw std::runtime_error("World not ready");
        try{auto reply=npc_shops_->transact(*scene_entities_,character_,sub,body,event);send_inventory(send,true);send(cat,sub,reply);}
        catch(const std::exception &error){send_inventory(send,true);send(11,1,notice(error.what()));}
        return true;
    }
    if(cat==12&&sub==3){
        // AMo 006730f7 writes EID, a BE16 character count, 0xe2 and NUL-terminated LE32 characters.
        if(body.size()<12||body.size()>136||get32(body,0,true)!=eid()||get16(body,6,true)!=0xe2)
            return true;
        const auto length=get16(body,4,true);
        if(body.size()!=12+4u*length||get32(body,8+4u*length)!=0)return true;
        auto reply=body;
        try {
            if(!loaded_||character_.value("hp",0)<=0)throw std::runtime_error("Cla indisponivel");
            std::wstring name;
            for(std::size_t i=0;i<length;++i){
                const auto ch=get32(body,8+4*i);
                if(!ch||ch>0xffffu)throw std::runtime_error("Nome de cla invalido");
                name.push_back(static_cast<wchar_t>(ch));
            }
            inventory_->guild_create(identity_.identity.id,eid(),utf8(name));
            send_inventory(send,false);
            send(cat,sub,reply);
        }catch(const std::exception &e){
            put16(reply,6,0xe6,true);
            send(cat,sub,reply);
            send(11,1,notice(e.what()));
            return true;
        }
        sync_guild(send,false);
        return true;
    }
    const bool guildStorage=cat==12&&(sub==0x0c||sub==0x0d||sub==0x0e);
    if(guildStorage||(cat==6&&(sub==0x1a||sub==0x1b||sub==0x1c||sub==0x29||sub==0x2a||sub==0x2b))){
        const auto kind=guildStorage?PersonalStorage::Guild:
            sub>=0x29?PersonalStorage::Account:PersonalStorage::Character;
        const unsigned char listOpcode=guildStorage?0x0e:kind==PersonalStorage::Account?0x2b:0x1c;
        const bool opening=sub==listOpcode;
        const bool deposit=sub==listOpcode-2;
        const bool framed=body.size()==(deposit?34u:22u)&&get32(body,0,true)==eid()&&
                          get16(body,body.size()-2,true)==0xc6;
        try {
            if(!loaded_||character_.value("hp",0)<=0)throw std::runtime_error("Deposito indisponivel");
            if(!framed)
                throw std::runtime_error("Pedido de deposito invalido");
            const auto actor=get32(body,4,true);
            const auto npc=scene_entities_->find(actor);
            const auto &p=character_.at("pos");
            const Position pos{p[0],p[1],p[2]};
            // The shared 0x20008 service identifies Anne and its equivalents in the native NPC table.
            static const std::set<std::uint32_t> keepers{9200141,9300131,9400121,9500131,9600131,
                                                        9700051,9800061,9810016,9850014,9850114};
            if(npc==scene_entities_->end()||!npc->second.alive||!keepers.count(npc->second.npc_id)||
               npc->second.position.distance(pos)>350)
                throw std::runtime_error("Abra o deposito perto do NPC");
            const auto stamp=now();
            const auto &account=identity_.identity.id;
            Transaction storageTransaction(store_.db());
            auto storageScope=map_scope();
            if(guildStorage)
                storageScope+=":guild:"+std::to_string(inventory_->guild_storage_membership_id(account,eid()));
            std::vector<Bytes> acknowledgements;
            if(opening) {
                if(std::any_of(body.begin()+8,body.begin()+20,[](auto byte){return byte!=0;}))
                    throw std::runtime_error("Pedido de deposito invalido");
            } else {
                if(storage_actor_!=actor||storage_scope_!=storageScope||storage_list_opcode_!=listOpcode)
                    throw std::runtime_error("Abra este deposito primeiro");
                auto &db=store_.db();
                const auto transferKind="storage:"+std::to_string(cat)+"/"+std::to_string(sub)+":"+storageScope;
                const auto prior=event.empty()?Json::array():db.query(
                    "SELECT eid,kind,request,result FROM inventory_operations WHERE event_key=?",{event});
                if(!prior.empty()){
                    if(prior[0].at("eid")!=eid()||prior[0].at("kind")!=transferKind||prior[0].at("request")!=hex(body))
                        throw std::runtime_error("Pedido repetido de deposito alterado");
                    for(const auto &ack:Json::parse(prior[0].at("result").get<std::string>()))
                        acknowledgements.push_back(unhex(ack.get<std::string>()));
                } else if(deposit) {
                    const auto category=get32(body,8,true),bagSlot=get32(body,12,true),item=get32(body,16,true);
                    const auto destination=get32(body,28,true);
                    if(category>4||bagSlot>47||destination>=storage_capacity(kind))
                        throw std::runtime_error("Slot do deposito invalido");
                    const auto source=Inventory::key(category,bagSlot);
                    const auto owned=inventory_->owned(eid(),source,item,stamp);
                    const auto requested=(std::uint64_t(get32(body,20,true))<<32)|get32(body,24,true);
                    if(!requested||requested>static_cast<std::uint64_t>(owned.quantity))
                        throw std::runtime_error("Quantidade invalida para deposito");
                    std::vector<std::pair<int,std::int64_t>> placements;
                    if(item==999999){
                        inventory_->storage_carats(account,eid(),kind,static_cast<std::int64_t>(requested),event,destination);
                        for(const auto &entry:inventory_->storage_items(account,eid(),kind,stamp))
                            if(entry.item==999999)placements.emplace_back(entry.slot,requested);
                    }else{
                        placements=inventory_->storage_deposit(account,eid(),kind,source,destination,item,owned.uid,stamp,
                                                               static_cast<std::int64_t>(requested));
                    }
                    for(const auto &[slot,quantity]:placements){
                        auto reply=body;
                        put32(reply,20,static_cast<std::uint32_t>(std::uint64_t(quantity)>>32),true);
                        put32(reply,24,static_cast<std::uint32_t>(quantity),true);
                        put32(reply,28,slot,true);
                        acknowledgements.push_back(std::move(reply));
                    }
                } else {
                    // AMo 004C2224 and 004C2314 send the storage slot and an all-ones whole-stack quantity.
                    const auto slot=get32(body,8,true);
                    if(slot>=storage_capacity(kind))
                        throw std::runtime_error("Pedido de retirada invalido");
                    const auto selected=storage_seen_.find(static_cast<int>(slot));
                    if(selected==storage_seen_.end())throw std::runtime_error("Item indisponivel no deposito");
                    const auto entries=inventory_->storage_items(account,eid(),kind,stamp);
                    const auto current=std::find_if(entries.begin(),entries.end(),[&](const auto &item){
                        return item.slot==static_cast<int>(slot)&&item.uid==selected->second;
                    });
                    if(current==entries.end())throw std::runtime_error("Item indisponivel no deposito");
                    const auto requested=(std::uint64_t(get32(body,12,true))<<32)|get32(body,16,true);
                    if(current->item==999999){
                        const auto amount=requested==std::numeric_limits<std::uint64_t>::max()?
                            current->quantity:requested<=static_cast<std::uint64_t>(current->quantity)?
                            static_cast<std::int64_t>(requested):0;
                        inventory_->storage_carats(account,eid(),kind,-amount,event,slot);
                    }else{
                        if(requested!=std::numeric_limits<std::uint64_t>::max())
                            throw std::runtime_error("Pedido de retirada invalido");
                        inventory_->storage_withdraw(account,eid(),kind,slot,std::nullopt,selected->second,stamp);
                    }
                    acknowledgements.push_back(body);
                }
                if(!event.empty()&&prior.empty()){
                    auto encoded=Json::array();
                    for(const auto &ack:acknowledgements)encoded.push_back(hex(ack));
                    db.exec("INSERT INTO inventory_operations VALUES(?,?,?,?,?)",
                            {event,std::int64_t(eid()),transferKind,hex(body),encoded.dump()});
                }
            }
            auto reply=inventory_->storage_snapshot(account,eid(),kind,stamp);
            storageTransaction.commit();
            if(opening){
                storage_actor_=actor;
                storage_scope_=storageScope;
                storage_list_opcode_=listOpcode;
            }else{
                send_inventory(send,false);
                for(const auto &ack:acknowledgements)send(cat,sub,ack);
            }
            storage_seen_.clear();
            for(std::size_t record=22;record<reply.size();record+=146)
                storage_seen_.emplace(get16(reply,record+4),get32(reply,record+7));
            send(cat,listOpcode,reply);
        }catch(const std::exception &e){
            if(framed&&!opening&&!deposit){auto reply=body;put16(reply,20,0xc7,true);send(cat,sub,reply);}
            send(11,1,service_notice(identity_.identity,e.what()));
        }
        return true;
    }
    if(cat==17&&sub==0){
        // The pet panel's empty-list reply keeps the owner and status and advertises one slot.
        if(loaded_&&body.size()==14&&get32(body,0,true)==eid()&&get32(body,4,true)==0&&
           get16(body,8,true)==0x01c9&&get32(body,10,true)==0){
            auto reply=body;
            put32(reply,10,1,true);
            send(cat,sub,reply);
        }
        return true;
    }
    if(cat==16&&(sub==1||sub==2||sub==3||sub==5)){
        try {
            if(!loaded_||character_.value("hp",0)<=0)
                throw std::runtime_error("Correio indisponivel");
            const bool opening=sub==2;
            if(opening&&(body.size()!=16||std::any_of(body.begin(),body.begin()+12,[](auto c){return c!=0;})))
                throw std::runtime_error("Pedido de correio invalido");
            const auto scope=map_scope();
            const auto actor=opening?get32(body,12,true):mailbox_actor_;
            const auto npc=scene_entities_->find(actor);
            const auto &p=character_.at("pos");
            const Position position{p[0],p[1],p[2]};
            if((!opening&&mailbox_scope_!=scope)||npc==scene_entities_->end()||
               !npc->second.alive||(npc->second.npc_id!=9300281&&npc->second.npc_id!=9810021)||
               npc->second.position.distance(position)>350)
                throw std::runtime_error("Abra o correio perto do NPC");
            const auto stamp=now();
            auto &db=store_.db();
            if(opening){
                auto list=native_mail_list(gm_mail_inbox(db,eid(),stamp),stamp);
                mailbox_actor_=actor;
                mailbox_scope_=scope;
                send(cat,2,list);
            }else if(sub==3){
                auto reply=gm_mail_delete(db,eid(),body,stamp);
                send(cat,3,reply);
                const auto deleted=get32(reply,0,true);
                if(deleted)
                    send(cat,2,native_mail_list(gm_mail_inbox(db,eid(),stamp),stamp));
                if(deleted<get32(body,0,true))
                    send(11,1,service_notice(identity_.identity,"Algumas cartas nao foram excluidas. Resgate os anexos antes de excluir."));
            }else{
                auto mail=gm_mail_access(root_,db,*inventory_,eid(),body,sub==5,stamp);
                if(sub==5){
                    Bytes reply(body.begin(),body.begin()+8);
                    reply.insert(reply.end(),body.end()-2,body.end());
                    send(cat,8,reply);
                    send_inventory(send,false);
                }
                send(cat,2,native_mail_list(gm_mail_inbox(db,eid(),stamp),stamp));
                send(cat,1,native_mail_detail(*inventory_,eid(),mail));
            }
        }catch(const std::exception &e){send(11,1,service_notice(identity_.identity,e.what()));}
        return true;
    }
    if(cat==6&&sub==2&&loaded_&&body.size()==16&&get32(body,0,true)==eid()&&body[10]==0&&get32(body,11,true)==eid()&&body[15]==0){
        int slot=-1;try{slot=Inventory::key(body[4],body[5]);}catch(const std::exception&){}
        auto iid=get32(body,6,true);
        if(store_.db().query("SELECT item_id FROM inventory_slots WHERE player_eid=? AND slot=? AND item_id=?",{std::int64_t(eid()),std::int64_t(slot),std::int64_t(iid)}).empty()){
            auto p=character_.at("pos");Position position{p[0],p[1],p[2]};
            for(const auto &[id,npc]:*scene_entities_)if(npc.alive&&(!npc.combat||npc.aggro<=0)&&npc.position.distance(position)<=350){send(cat,sub,body);return true;}
        }
    }
    if (cat == 6 && (sub == 0 || sub == 1)) {
        if (!loaded_ || character_.at("hp").get<int>() <= 0 || (body.size() != 11 && body.size() != 12) ||
            get32(body, 0, true) != eid())
            throw std::runtime_error("Invalid equipment action state or identity");
        auto before = character_, old_stats = stats_;
        int target = 0;
        unsigned char equipped_grade = 0;
        try {
            store_.update_character(identity_.identity.id, eid(), [&](Json &saved) {
                auto item = get32(body, 7, true);
                if (sub == 0) {
                    target = inventory_->equip(eid(), Inventory::key(body[4], body[5]), item,
                                               character_.at("cls"), character_.at("level"), body[6], now());
                    const auto equipment = inventory_->items(eid(), true);
                    const auto equipped = std::find_if(equipment.begin(), equipment.end(), [&](const auto &entry) {
                        return entry.slot == target && entry.item == item;
                    });
                    if (equipped == equipment.end())
                        throw std::runtime_error("Equipped item missing after inventory update");
                    equipped_grade = static_cast<unsigned char>(equipped->grade);
                } else {
                    target = body[6];
                    std::optional<int> destination;
                    if (body[4] >= 1 && body[4] <= 4)
                        destination = Inventory::key(body[4], body[5]);
                    inventory_->unequip(eid(), target, item, destination, now());
                }
                recalculate();
                saved["equipment_slots"] = character_.at("equipment_slots");
                saved["gear"] = character_.at("gear");
                for (const auto &name : {"hp", "mp", "maxhp", "maxmp"})
                    saved[name] = character_.at(name);
            });
        } catch (const std::exception &error) {
            character_ = before;
            stats_ = old_stats;
            send_inventory(send, true);
            send(11, 1, service_notice(identity_.identity, error.what()));
            return true;
        } catch (...) {
            character_ = before;
            stats_ = old_stats;
            throw;
        }
        send_inventory(send, true);
        auto reply = body;
        if (sub == 1) {
            reply.resize(11);
            reply[4] = 0;
            reply[5] = 0;
            reply[6] = static_cast<unsigned char>(target);
        } else {
            reply.resize(12);
            reply[11] = equipped_grade;
            reply[5] = body[4] == 1 ? body[5] : static_cast<unsigned char>(20 + (body[4] - 2) * 48 + body[5]);
            reply[6] = static_cast<unsigned char>(target);
        }
        send(6, sub, reply);
        send(8, 2, stats_packet());
        send(8, 0, gauge(0x0800));
        send(8, 1, gauge(0x0801));
        return true;
    }
    if (cat == 8 && sub == 9) {
        if (body.size() != 25 || get32(body, 0, true) != eid())
            throw std::runtime_error("Invalid map-ready acknowledgement");
        send(8, 9, body);
        return true;
    }
    if (cat != 4 || (sub != 0 && sub != 1))
        return false;
    if ((!loaded_ || character_.value("hp",0)<=0) && sub != 0)
        return true;
    auto actor_offset = sub == 0 ? 1u : 2u;
    if (body.size() < (sub == 0 ? 21u : 30u) || get32(body, actor_offset, true) != eid())
        throw std::runtime_error("Invalid movement actor or length");
    auto offset = sub == 0 ? 5 : 6;
    double x = getfloat(body, offset), y = getfloat(body, offset + 4), z = getfloat(body, offset + 8),
           angle = sub == 0            ? getfloat(body, 17)
                   : body.size() >= 34 ? getfloat(body, 30)
                                       : 0;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(angle) || x <= 0 ||
        x >= 100000 || y <= 0 || y >= 100000 || std::abs(z) > 10000)
        throw std::runtime_error("Invalid movement coordinates");
    if (loaded_ && sub == 1 && !position_guard_ && content_.has("server/devtool_map_rules.json")) {
        const auto map_rules = content_.get("server/devtool_map_rules.json");
        const auto old_position = character_.at("pos");
        const double start[2] = {old_position[0], old_position[1]};
        const double end[2] = {x, y};
        for (const auto& barrier : map_rules.value("barriers", Json::array())) {
            if (barrier.at("map_id") != map_id_) continue;
            const double minimum[2] = {barrier.at("x1"), barrier.at("y1")};
            const double maximum[2] = {barrier.at("x2"), barrier.at("y2")};
            auto inside = [&](const double* point) {
                return point[0] >= minimum[0] && point[0] <= maximum[0] &&
                       point[1] >= minimum[1] && point[1] <= maximum[1];
            };
            if (inside(start) && !inside(end)) continue;
            double enter=0., exit=1.;
            for (int axis=0; axis<2 && enter<=exit; ++axis) {
                const double delta=end[axis]-start[axis];
                if (std::abs(delta)<1.e-9) {
                    if (start[axis]<minimum[axis]||start[axis]>maximum[axis]) enter=2.;
                } else {
                    const double a=(minimum[axis]-start[axis])/delta;
                    const double b=(maximum[axis]-start[axis])/delta;
                    enter=std::max(enter,std::min(a,b));
                    exit=std::min(exit,std::max(a,b));
                }
            }
            if (enter<=exit) {
                refresh_self_appearance(send);
                return true;
            }
        }
    }
    if (position_guard_) {
        position_guard_ = false;
        if(resolve_native_arrival_&&sub==0){character_["pos"]={x,y,z};character_["angle"]=angle;resolve_native_arrival_=false;}
    } else {
        character_["pos"] = {x, y, z};
        character_["angle"] = angle;
    }
    persist();
    if(sub==1)stand(send);
    if (!loaded_ && sub == 0) {
        send(4, 2, appearance());
        // Preserve the observed Python entry sequence after own actor creation.
        // These carriers were previously omitted from the native bootstrap.
        for(int opcode:{0x0a0f,0x0f11,0x1601})
            if(templates_.count(opcode))send(static_cast<unsigned char>(opcode>>8),static_cast<unsigned char>(opcode),templates_.at(opcode));
        for (int opcode : {0x0807, 0x0808})
            if (templates_.count(opcode))
                send(8, static_cast<unsigned char>(opcode), templates_.at(opcode));
        send(8, 2, stats_packet());
        send(8, 1, gauge(0x0801));
        send(8, 0, gauge(0x0800));
        send_inventory(send, true);
        loaded_ = true;
        auto cash=character_.value("cash_visibility",Json::array({0,0}));Bytes visibility(6);put32(visibility,0,eid(),true);visibility[4]=cash.at(0).get<unsigned char>();visibility[5]=cash.at(1).get<unsigned char>();send(6,0x46,visibility);
        send(5,5,skills_->list(character_,templates_.at(0x0505)));
        send(5,12,skills_->super_list(character_));
        for(auto&allocation:skills_->allocations(character_))send(8,4,allocation);
        sync_quests(send,true,true);
        sync_guild(send,false);
        sync_dungeon(send);
        scene_visibility(*scene_,*scene_entities_,character_.at("pos"),scene_visible_,send);
    }
    publish_player(send);
    return true;
}
} // namespace rosh
