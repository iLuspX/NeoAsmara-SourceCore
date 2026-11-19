#include "native/dungeons.hpp"
#include "native/scene.hpp"
#include <iostream>
using namespace rosh;
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>static void rejected(F&& action,const char* message){bool failed=false;try{action();}catch(const std::exception&){failed=true;}check(failed,message);}
int wmain(int argc,wchar_t** argv){try{
    check(argc==4,"Expected staged content, NEW test database, and capture fixture");
    check(!fs::exists(argv[2]),"Refusing an existing database");
    Content content(argv[1]);Sqlite db(argv[2]);DungeonStore dungeons(db,content);
    const auto captured=Json::parse(read_file(argv[3]));
    constexpr std::uint32_t map=29614;
    const auto definition=dungeons.rule(map),spawns=content.get("data/spawns.json").at(definition.at("spawn_key").get<std::string>());
    Json entrance={{"map_id",19003},{"world_name","berneo"},{"x",9020},{"y",6590},{"z",627}};
    auto first=dungeons.join(91001,"party:test-a",map,1,entrance,1000.);
    auto second=dungeons.join(91002,"party:test-a",map,1,entrance,1001.);
    check(first.at("id")==second.at("id"),"Party split across dungeon instances");
    auto unrelated=dungeons.join(91003,"solo:91003",map,1,entrance,1001.);
    auto channel=dungeons.join(91004,"party:test-a",map,2,entrance,1001.);
    check(unrelated.at("id")!=first.at("id")&&channel.at("id")!=first.at("id"),"Dungeon crossed group/channel boundary");
    const auto run="run:"+first.at("id").get<std::string>();
    auto again=dungeons.join(91001,"party:test-a",map,1,entrance,1002.);
    check(again.at("id")==first.at("id")&&!again.at("new_member").get<bool>(),"Re-entry created a second admission");
    auto resumed=dungeons.resume(91001,map,1,1003.);
    check(resumed.at("id")==first.at("id")&&dungeons.resume(91001,map,2,1003.).is_null(),"Reconnect did not retain private progress");
    const auto& trigger=definition.at("triggers").at("5");const auto& position=trigger.at("position");
    Position gate{position.at(0),position.at(1),position.at(2)};
    rejected([&]{dungeons.trigger(run,91003,map,5,gate,1004.);},"Nonmember changed the dungeon");
    rejected([&]{dungeons.trigger(run,91001,map,5,gate,1004.);},"Gate opened before room clear");
    auto kill=[&](std::size_t index){
        const auto& spawn=spawns.at(index);Entity mob;mob.combat=true;mob.npc_id=spawn.at("id");mob.spawn_index=index;
        mob.rules["native_reference"]=spawn.at("native_reference");dungeons.kill(run,mob,1005.);
    };
    const auto& stage=definition.at("stages").at(0);const auto requirements=stage.at("required_spawns");
    check(requirements.size()>1,"Capture room fixture lacks independent enemies");
    for(std::size_t i=0;i+1<requirements.size();i++)kill(requirements.at(i));
    const auto marker=stage.contains("native_event")?"native:"+stage.at("native_event").dump():"stage:0";
    check(db.query("SELECT 1 FROM dungeon_events WHERE run_id=? AND event_key=?",{first.at("id").get<std::string>(),marker+":0"}).empty(),"Door opened with a remaining enemy");
    kill(requirements.back());
    auto events=dungeons.events(run,91001,0,1006.);check(!events.empty(),"Room clear produced no native event");
    auto expected=unhex(captured.at("first_room_29614").at("body"));
    check(std::any_of(events.begin(),events.end(),[&](const auto& row){return unhex(row.at("body"))==expected;}),"Native room event differs from supplied capture");
    const auto count=events.size();kill(requirements.back());check(dungeons.events(run,91001,0,1006.).size()==count,"Duplicate death replayed room events");
    check(dungeons.events(run,91003,0,1006.).empty(),"Private event state leaked to another group");
    check(dungeons.events(run,91002,0,1006.).size()==count,"Party member did not receive room state");
    check(dungeons.events(run,91002,events.back().at("sequence"),1006.).empty(),"Event cursor repeated room state");
    for(const auto& required_stage:trigger.at("requires_stages"))
        for(const auto& index:definition.at("stages").at(required_stage.get<std::size_t>()).at("required_spawns"))kill(index);
    rejected([&]{dungeons.trigger(run,91001,map,5,{gate.x+1000,gate.y,gate.z},1007.);},"Remote gate activation accepted");
    dungeons.trigger(run,91001,map,5,gate,1007.);
    const auto opened=dungeons.events(run,91001,0,1008.).size();dungeons.trigger(run,91001,map,5,gate,1008.);
    check(dungeons.events(run,91001,0,1008.).size()==opened,"Repeated trigger replayed its animation");
    check(!dungeons.active_references(run).empty(),"Room events did not activate the native next wave");
    for(const auto& index:definition.at("completion_spawns"))kill(index);
    check(db.query("SELECT status FROM dungeon_runs WHERE id=?",{first.at("id").get<std::string>()}).front().at("status")=="completed","Final bosses did not complete the instance");
    rejected([&]{dungeons.join(91005,"party:test-a",map,1,entrance,1009.);},"New entrant joined a completed instance");
    check(dungeons.entrance(run,91001)==entrance,"Revival lost the actual outside entrance");
    dungeons.leave(run,91001,1010.);check(dungeons.resume(91002,map,1,1011.).is_object(),"One departure reset another member's instance");
    dungeons.leave(run,91002,1012.);auto fresh=dungeons.join(91001,"party:test-a",map,1,entrance,1013.);
    check(fresh.at("id")!=first.at("id")&&dungeons.events("run:"+fresh.at("id").get<std::string>(),91001,0,1014.).empty(),"New run inherited dead mobs or open doors");
    check(dungeons.resume(91001,map,1,4000.).is_null()&&dungeons.last_membership(91001,map).is_object(),"Expired/off-channel run lost its safe return point");
    auto before=db.query("SELECT COUNT(*) AS n FROM dungeon_runs").front().at("n");
    {Transaction rollback(db);dungeons.join(91006,"rollback",map,1,entrance,1015.);}
    check(db.query("SELECT COUNT(*) AS n FROM dungeon_runs").front().at("n")==before,"Failed admission left an orphan run");
    {
        const auto tower=dungeons.rule(29016);
        const auto tower_spawns=content.get("data/spawns.json").at(tower.at("spawn_key").get<std::string>());
        const auto tower_run=dungeons.join(91110,"tower:early-door",29016,1,entrance,1016.);
        Entity boss;boss.combat=true;boss.npc_id=tower_spawns[0].at("id");boss.spawn_index=0;
        boss.rules["native_reference"]=tower_spawns[0].at("native_reference");
        check(boss.rules.at("native_reference")==500,"Tower regression fixture lost its overlapping reference");
        dungeons.kill("run:"+tower_run.at("id").get<std::string>(),boss,1017.);
        check(db.query("SELECT 1 FROM dungeon_events WHERE run_id=? AND event_key LIKE 'native:500:%'",{tower_run.at("id").get<std::string>()}).empty(),"First-room actor 500 opened room-clear event 500 in a later room");
        const auto trigger=tower.at("triggers").at("200").at("position");
        const auto group="run:"+tower_run.at("id").get<std::string>();
        dungeons.trigger(group,91110,29016,200,{trigger[0],trigger[1],trigger[2]},1018.);
        Bytes hidden(20);put32(hidden,0,29016,true);put32(hidden,4,47,true);put32(hidden,8,501,true);put32(hidden,12,2,true);put32(hidden,16,0xffffffff,true);
        db.exec("INSERT INTO dungeon_events(run_id,event_key,body) VALUES(?,?,?)",{tower_run.at("id").get<std::string>(),"migration-hide:501",hex(hidden)});
        db.exec("DROP TABLE dungeon_deactivations");
        DungeonStore migrated(db,content);
        check(migrated.inactive_references(group).count(501)&&!migrated.active_references(group).count(501),"Existing native despawn history was not restored");
        migrated.trigger(group,91110,29016,200,{trigger[0],trigger[1],trigger[2]},1019.);
        check(migrated.inactive_references(group).count(501)&&!migrated.active_references(group).count(501),"Replayed old trigger resurrected a later disabled wave");
        WorldScene scene(content);const auto name="catacombBelltower#29016#run:state-test";
        scene.apply_dungeon_actors(name,{501},{});
        auto actors=scene.map(name);auto actor=std::find_if(actors->begin(),actors->end(),[](const auto& pair){return pair.second.rules.value("native_reference",0u)==501;});
        check(actor!=actors->end()&&actor->second.attackable(false),"Native wave activation left actor unavailable");
        scene.apply_dungeon_actors(name,{}, {501});actors=scene.map(name);
        actor=std::find_if(actors->begin(),actors->end(),[](const auto& pair){return pair.second.rules.value("native_reference",0u)==501;});
        check(!actor->second.attackable(false)&&actor->second.rules.value("dungeon_dormant",false),"Native despawn left an attacking/visible world actor");
    }
    const auto maze_rule=dungeons.rule(29015);
    if(maze_rule.contains("initial_packets")){
        const auto maze=dungeons.join(91101,"maze:test",29015,1,entrance,1016.);
        const auto maze_run="run:"+maze.at("id").get<std::string>();
        const auto initial=dungeons.events(maze_run,91101,0,1017.);
        check(initial.size()==137,"Peltrok maze initialization packets are missing");
        for(std::size_t i=0;i<initial.size();++i){
            check(initial[i].at("subcode")==4,"Maze doors sent as ordinary script-action packets");
            check(initial[i].at("body")==maze_rule.at("initial_packets").at(i).at("body"),"Maze initialization differs from recorded native packet");
        }
        dungeons.join(91102,"maze:test",29015,1,entrance,1018.);
        check(dungeons.events(maze_run,91102,0,1019.).size()==initial.size(),"Party join duplicated maze initialization");
        check(dungeons.events(maze_run,91101,initial.back().at("sequence"),1019.).empty(),"Maze state replayed despite event cursor");
        check(dungeons.events(maze_run,91103,0,1019.).empty(),"Maze state leaked to a foreign player");
    }
    std::cout<<"PASS: party/channel isolation; reconnect; native captured door event; room-clear gating; triggers; wave activation; replay protection; completion; safe exit; fresh runs; expired returns; atomic admission; captured maze-door initialization\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}}
