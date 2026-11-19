#include "native/dungeons.hpp"
#include <algorithm>
#include <cmath>
#include <set>
namespace rosh {
namespace {
std::string key(const std::string& run) { return run.rfind("run:",0)==0 ? run.substr(4) : run; }
std::set<std::size_t> killed(Sqlite& db, const std::string& run) {
    std::set<std::size_t> result;
    for(const auto& row:db.query("SELECT spawn_index FROM dungeon_kills WHERE run_id=?",{run}))
        result.insert(row.at("spawn_index").get<std::size_t>());
    return result;
}
bool satisfied(const Json& required,const std::set<std::size_t>& deaths) {
    return std::all_of(required.begin(),required.end(),[&](const auto& index){return deaths.count(index.template get<std::size_t>());});
}
}
DungeonStore::DungeonStore(Sqlite& db,Content& content):db_(db),rules_(content.has("server/dungeon_rules.json")?
    content.get("server/dungeon_rules.json").at("maps"):Json::object()) {
    Transaction schema(db_);
    const bool had_inactive=!db_.query("SELECT 1 FROM sqlite_master WHERE type='table' AND name='dungeon_deactivations'").empty();
    db_.exec("CREATE TABLE IF NOT EXISTS dungeon_runs(id TEXT PRIMARY KEY,map_id INTEGER NOT NULL,channel INTEGER NOT NULL,"
             "owner TEXT NOT NULL,status TEXT NOT NULL,created REAL NOT NULL,updated REAL NOT NULL);"
             "CREATE INDEX IF NOT EXISTS dungeon_runs_owner ON dungeon_runs(owner,map_id,channel,status);"
             "CREATE TABLE IF NOT EXISTS dungeon_members(run_id TEXT NOT NULL,eid INTEGER NOT NULL,inside INTEGER NOT NULL,"
             "entrance TEXT NOT NULL,PRIMARY KEY(run_id,eid));"
             "CREATE TABLE IF NOT EXISTS dungeon_kills(run_id TEXT NOT NULL,spawn_index INTEGER NOT NULL,npc_id INTEGER NOT NULL,"
             "created REAL NOT NULL,PRIMARY KEY(run_id,spawn_index));"
             "CREATE TABLE IF NOT EXISTS dungeon_events(sequence INTEGER PRIMARY KEY AUTOINCREMENT,run_id TEXT NOT NULL,"
             "event_key TEXT NOT NULL,body TEXT NOT NULL,UNIQUE(run_id,event_key));"
             "CREATE TABLE IF NOT EXISTS dungeon_activations(run_id TEXT NOT NULL,reference INTEGER NOT NULL,PRIMARY KEY(run_id,reference));"
             "CREATE TABLE IF NOT EXISTS dungeon_deactivations(run_id TEXT NOT NULL,reference INTEGER NOT NULL,PRIMARY KEY(run_id,reference));"
             "CREATE TABLE IF NOT EXISTS dungeon_triggers(run_id TEXT NOT NULL,trigger_id INTEGER NOT NULL,"
             "PRIMARY KEY(run_id,trigger_id));");
    if(db_.query("SELECT 1 FROM pragma_table_info('dungeon_events') WHERE name='subcode'").empty())
        db_.exec("ALTER TABLE dungeon_events ADD COLUMN subcode INTEGER NOT NULL DEFAULT 0");
    if(!had_inactive){
        for(const auto& event:db_.query("SELECT run_id,body FROM dungeon_events WHERE subcode=0 ORDER BY sequence")){
            const auto body=unhex(event.at("body").get<std::string>());
            if(body.size()!=20)continue;
            const auto kind=get32(body,4,true),reference=get32(body,8,true);
            const auto run=event.at("run_id").get<std::string>();
            if(kind==46){db_.exec("DELETE FROM dungeon_deactivations WHERE run_id=? AND reference=?",{run,std::int64_t(reference)});db_.exec("INSERT OR IGNORE INTO dungeon_activations VALUES(?,?)",{run,std::int64_t(reference)});}
            if(kind==47){db_.exec("DELETE FROM dungeon_activations WHERE run_id=? AND reference=?",{run,std::int64_t(reference)});db_.exec("INSERT OR IGNORE INTO dungeon_deactivations VALUES(?,?)",{run,std::int64_t(reference)});}
        }
    }
    schema.commit();
}
Json DungeonStore::rule(std::uint32_t map) const { return rules_.value(std::to_string(map),Json::object()); }
Json DungeonStore::resume(std::uint32_t eid,std::uint32_t map,int channel,double stamp) {
    auto rows=db_.query("SELECT r.* FROM dungeon_runs r JOIN dungeon_members m ON m.run_id=r.id "
        "WHERE m.eid=? AND m.inside=1 AND r.map_id=? AND r.channel=? AND r.status IN ('active','completed') "
        "AND r.updated>? ORDER BY r.created DESC LIMIT 1",{std::int64_t(eid),std::int64_t(map),std::int64_t(channel),stamp-1800});
    return rows.empty()?Json():rows.front();
}
Json DungeonStore::join(std::uint32_t eid,const std::string& owner,std::uint32_t map,int channel,const Json& entrance,double stamp) {
    if(!rule(map).value("instanced",false)||owner.empty()||channel<1||channel>4||!std::isfinite(stamp))
        throw std::runtime_error("Invalid dungeon admission");
    Transaction tx(db_);
    auto rows=db_.query("SELECT * FROM dungeon_runs WHERE owner=? AND map_id=? AND channel=? "
        "AND status IN ('active','completed') AND updated>? ORDER BY created DESC LIMIT 1",
        {owner,std::int64_t(map),std::int64_t(channel),stamp-1800});
    Json run;
    if(rows.empty()) {
        auto id=token(24);
        db_.exec("INSERT INTO dungeon_runs VALUES(?,?,?,?,'active',?,?)",{id,std::int64_t(map),std::int64_t(channel),owner,stamp,stamp});
        const auto initial=rule(map).value("initial_packets",Json::array());
        if(initial.size()>4096)throw std::runtime_error("Unbounded dungeon initialization");
        for(const auto& packet:initial){
            const auto body=unhex(packet.at("body").get<std::string>());
            if(packet.at("sub")!=4||body.size()!=12||!get32(body,4,true))
                throw std::runtime_error("Invalid native maze-door initialization");
            db_.exec("INSERT INTO dungeon_events(run_id,event_key,body,subcode) VALUES(?,?,?,4)",
                {id,"maze:"+std::to_string(get32(body,4,true)),hex(body)});
        }
        run={{"id",id},{"status","active"},{"map_id",map}};
    } else run=rows.front();
    const auto id=run.at("id").get<std::string>();
    auto prior=db_.query("SELECT inside FROM dungeon_members WHERE run_id=? AND eid=?",{id,std::int64_t(eid)});
    if(run.at("status")=="completed"&&prior.empty())throw std::runtime_error("This dungeon has already been completed");
    run["new_member"]=prior.empty();
    run["charge_entry"]=prior.empty()||prior.front().at("inside")==0;
    db_.exec("INSERT INTO dungeon_members VALUES(?,?,1,?) ON CONFLICT(run_id,eid) DO UPDATE SET inside=1",
        {id,std::int64_t(eid),entrance.dump()});
    db_.exec("UPDATE dungeon_runs SET updated=? WHERE id=?",{stamp,id});
    tx.commit();return run;
}
void DungeonStore::leave(const std::string& run,std::uint32_t eid,double stamp) {
    if(run.rfind("run:",0)!=0)return;
    Transaction tx(db_);auto id=key(run);
    db_.exec("UPDATE dungeon_members SET inside=0 WHERE run_id=? AND eid=?",{id,std::int64_t(eid)});
    if(db_.query("SELECT 1 FROM dungeon_members WHERE run_id=? AND inside=1 LIMIT 1",{id}).empty())
        db_.exec("UPDATE dungeon_runs SET status='closed',updated=? WHERE id=?",{stamp,id});
    tx.commit();
}
void DungeonStore::emit(const std::string& run,const std::string& prefix,std::uint32_t map,const Json& events) {
    int index=0;
    for(const auto& event:events) {
        if(event.size()!=4)throw std::runtime_error("Invalid native dungeon event");
        Bytes body(20);put32(body,0,map,true);
        for(int part=0;part<4;part++)put32(body,4+part*4,event.at(part).get<std::uint32_t>(),true);
        db_.exec("INSERT OR IGNORE INTO dungeon_events(run_id,event_key,body) VALUES(?,?,?)",
            {run,prefix+":"+std::to_string(index++),hex(body)});
        if(db_.changes()==0)continue;
        if(event.at(0)==46){db_.exec("DELETE FROM dungeon_deactivations WHERE run_id=? AND reference=?",{run,event.at(1).get<std::int64_t>()});db_.exec("INSERT OR IGNORE INTO dungeon_activations VALUES(?,?)",{run,event.at(1).get<std::int64_t>()});}
        if(event.at(0)==47){db_.exec("DELETE FROM dungeon_activations WHERE run_id=? AND reference=?",{run,event.at(1).get<std::int64_t>()});db_.exec("INSERT OR IGNORE INTO dungeon_deactivations VALUES(?,?)",{run,event.at(1).get<std::int64_t>()});}
    }
}
void DungeonStore::kill(const std::string& run,const Entity& mob,double stamp) {
    if(run.rfind("run:",0)!=0||!mob.combat||mob.guard||mob.spawn_index>=1000000000ULL)return;
    const auto id=key(run);Transaction tx(db_);
    auto rows=db_.query("SELECT map_id FROM dungeon_runs WHERE id=? AND status='active'",{id});
    if(rows.empty()){tx.commit();return;}
    auto map=rows.front().at("map_id").get<std::uint32_t>();auto definition=rule(map);
    db_.exec("INSERT OR IGNORE INTO dungeon_kills VALUES(?,?,?,?)",{id,std::int64_t(mob.spawn_index),std::int64_t(mob.npc_id),stamp});
    const auto native_reference=std::to_string(mob.rules.value("native_reference",0u));
    const auto native_events=definition.value("kill_events",Json::object());
    const auto stages=definition.value("stages",Json::array());
    const bool room_event=std::any_of(stages.begin(),stages.end(),[&](const Json& stage){
        return stage.contains("native_event")&&stage.at("native_event").dump()==native_reference;
    });
    if(native_events.contains(native_reference)&&!room_event)emit(id,"native:"+native_reference,map,native_events.at(native_reference));
    const auto deaths=killed(db_,id);int stage=0;
    for(const auto& row:stages) {
        if(satisfied(row.at("required_spawns"),deaths))emit(id,row.contains("native_event")?"native:"+row.at("native_event").dump():"stage:"+std::to_string(stage),map,row.at("events"));
        ++stage;
    }
    auto completion=definition.value("completion_spawns",Json::array());
    if(!completion.empty()&&satisfied(completion,deaths))
        db_.exec("UPDATE dungeon_runs SET status='completed',updated=? WHERE id=?",{stamp,id});
    else db_.exec("UPDATE dungeon_runs SET updated=? WHERE id=?",{stamp,id});
    tx.commit();
}
void DungeonStore::trigger(const std::string& run,std::uint32_t eid,std::uint32_t map,std::uint32_t trigger,const Position& position,double stamp) {
    if(run.rfind("run:",0)!=0)throw std::runtime_error("No active dungeon instance");
    const auto id=key(run);Transaction tx(db_);
    auto rows=db_.query("SELECT 1 FROM dungeon_runs r JOIN dungeon_members m ON m.run_id=r.id "
        "WHERE r.id=? AND r.map_id=? AND m.eid=? AND m.inside=1 AND r.status IN ('active','completed')",
        {id,std::int64_t(map),std::int64_t(eid)});
    if(rows.empty())throw std::runtime_error("Invalid dungeon membership");
    auto triggers=rule(map).value("triggers",Json::object());auto name=std::to_string(trigger);
    if(!triggers.contains(name))throw std::runtime_error("Unknown dungeon trigger");
    const auto& definition=triggers.at(name);
    if(definition.value("passive_only",false)){tx.commit();return;}
    auto at=definition.at("position");Position expected{at.at(0),at.at(1),at.at(2)};
    if(position.distance(expected)>200||std::abs(position.z-expected.z)>100)
        throw std::runtime_error("Dungeon trigger is out of reach");
    const auto deaths=killed(db_,id);const auto stages=rule(map).value("stages",Json::array());
    for(const auto& prerequisite:definition.value("requires_stages",Json::array()))
        if(!satisfied(stages.at(prerequisite.get<std::size_t>()).at("required_spawns"),deaths))
            throw std::runtime_error("Defeat the enemies guarding this passage first");
    db_.exec("INSERT OR IGNORE INTO dungeon_triggers VALUES(?,?)",{id,std::int64_t(trigger)});
    if(!db_.changes()){tx.commit();return;}
    emit(id,definition.contains("native_event")?"native:"+definition.at("native_event").dump():"trigger:"+name,map,definition.at("events"));
    for(const auto& reference:definition.value("activate_references",Json::array())){
        db_.exec("DELETE FROM dungeon_deactivations WHERE run_id=? AND reference=?",{id,reference.get<std::int64_t>()});
        db_.exec("INSERT OR IGNORE INTO dungeon_activations VALUES(?,?)",{id,reference.get<std::int64_t>()});
    }
    db_.exec("UPDATE dungeon_runs SET updated=? WHERE id=?",{stamp,id});tx.commit();
}
Json DungeonStore::events(const std::string& run,std::uint32_t eid,std::int64_t after,double stamp) {
    if(run.rfind("run:",0)!=0)return Json::array();
    auto id=key(run);
    if(db_.query("SELECT 1 FROM dungeon_members WHERE run_id=? AND eid=? AND inside=1",{id,std::int64_t(eid)}).empty())return Json::array();
    db_.exec("UPDATE dungeon_runs SET updated=? WHERE id=? AND status IN ('active','completed')",{stamp,id});
    return db_.query("SELECT sequence,body,subcode FROM dungeon_events WHERE run_id=? AND sequence>? ORDER BY sequence",{id,after});
}
Json DungeonStore::entrance(const std::string& run,std::uint32_t eid) {
    auto rows=db_.query("SELECT entrance FROM dungeon_members WHERE run_id=? AND eid=?",{key(run),std::int64_t(eid)});
    return rows.empty()?Json():Json::parse(rows.front().at("entrance").get<std::string>());
}
Json DungeonStore::last_membership(std::uint32_t eid,std::uint32_t map){
    auto rows=db_.query("SELECT r.id,m.entrance FROM dungeon_runs r JOIN dungeon_members m ON m.run_id=r.id WHERE m.eid=? AND m.inside=1 AND r.map_id=? ORDER BY r.created DESC LIMIT 1",{std::int64_t(eid),std::int64_t(map)});
    return rows.empty()?Json():rows.front();
}
std::set<std::uint32_t> DungeonStore::active_references(const std::string& run) {
    std::set<std::uint32_t> result;
    for(const auto& row:db_.query("SELECT reference FROM dungeon_activations WHERE run_id=?",{key(run)}))result.insert(row.at("reference").get<std::uint32_t>());
    return result;
}
std::set<std::uint32_t> DungeonStore::inactive_references(const std::string& run) {
    std::set<std::uint32_t> result;
    for(const auto& row:db_.query("SELECT reference FROM dungeon_deactivations WHERE run_id=?",{key(run)}))result.insert(row.at("reference").get<std::uint32_t>());
    return result;
}
}
