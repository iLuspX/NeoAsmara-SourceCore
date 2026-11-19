#include "native/entities.hpp"
#include <iostream>
#include <algorithm>
using namespace rosh;
static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int wmain(int argc, wchar_t **argv) {
    try {
        require(argc == 2, "Usage: entity tests <encrypted native root>");
        Content content(argv[1]);
        EntityCatalog catalog(content);
        const auto native_spawns=content.get("data/spawns.json");
        std::size_t loaded = 0, maps = 0, services = 0, combat = 0;
        for (auto map = catalog.maps().begin(); map != catalog.maps().end(); ++map) {
            auto entities = catalog.load(map.key(), {0x2892});
            require(!entities.count(0x2892), "Mob overwrote a player EID");
            maps++;
            for (const auto &[eid, entity] : entities) {
                auto body = catalog.appearance(entity);
                auto reference=catalog.prototype_reference(map.key(),entity.npc_id);
                const auto& placements=native_spawns.at(map.key());
                if(entity.spawn_index<placements.size())reference=placements.at(entity.spawn_index).value("native_reference",reference);
                require(body.size() == 254 && get32(body, 64) == entity.npc_id && get32(body, 2) == eid &&
                            get32(body, 68) == reference,
                        "Entity model/type carrier mismatch");
                require(entity.position.distance(entity.home) == 0, "Initial mob position changed");
                if (entity.combat) {
                    combat++;
                    require(entity.hp > 0 && entity.hp == entity.maxhp, "Mob health progression invalid");
                    require(getfloat(body, 38) > 0 && getfloat(body, 46) > 0, "Mob animation rates missing");
                } else {
                    services++;
                    require(entity.rules.at("wander_radius") == 0., "Service NPC wanders away");
                }
            }
            loaded += entities.size();
        }
        require(maps >= 21 && loaded > 9000 && services > 50 && combat > 1000,
                "Native map population unexpectedly missing");
        require(catalog.load("Peltrok").size()==catalog.load("peltrok").size()&&!catalog.load("Peltrok").empty(),"Native map case mismatch removed all NPCs/mobs");
        require(catalog.load("Siege_002").size()==catalog.load("Siege_").size()&&!catalog.load("Siege_002").empty(),"Siege asset alias removed native placements");
        require(catalog.health(9900285, 85) == 6000000 && catalog.health(9901075, 75) == 3000000,
                "Explicit boss HP policy lost");
        auto normal = catalog.rules(1001801, true, "berneo");
        require(normal.at("respawn_seconds") == 10., "Captured Spiderling respawn policy lost");
        auto neo = catalog.rules(9900285, true, "berneo");
        require(neo.at("respawn_seconds") == 43200., "Neo respawn policy lost");
        auto population = catalog.load("berneo");
        auto sample = std::find_if(population.begin(), population.end(), [](const auto &entry) {
            return entry.second.combat && entry.second.npc_id == 1001801;
        });
        require(sample != population.end(), "Spiderling missing in Berneo");
        Entity entity = sample->second;
        entity.eid = 1;
        entity.aggro = 100;
        entity.rules["spawn_range"] = 350.;
        std::map<std::uint32_t, Entity> entities = {{1, entity}};
        Observer player{100, {entity.home.x + 50, entity.home.y, entity.home.z}, 100, 1, false, true};
        std::map<std::uint32_t, Observer> players = {{100, player}};
        MobSimulation ai(catalog, 42, 1000);
        // Five mobs detect the same player. The first waits three seconds
        // and only one can transition into chase per reaction window.
        auto spot = entities;
        for (std::uint32_t id=2; id<=5; ++id) {
            auto neighbor=entity;neighbor.eid=id;spot.emplace(id,neighbor);
        }
        MobSimulation spotAi(catalog,42,500);
        auto approaches=[&](double timestamp){
            auto events=spotAi.tick(spot,players,timestamp);
            return std::count_if(events.begin(),events.end(),[](const auto& event){return event.kind==EntityEvent::Aggro;});
        };
        const auto detections = spotAi.tick(spot, players, 500.25);
        require(std::count_if(detections.begin(),detections.end(),[](const auto& event){return event.kind==EntityEvent::Detection;})==1,
                "Only one mob in the spot may start a proximity reaction");
        require(std::none_of(detections.begin(),detections.end(),[](const auto& event){return event.kind==EntityEvent::Aggro;})&&approaches(503.)==0,
                "Mob pursued before its reaction window");
        require(approaches(503.25)==1&&approaches(503.5)==0&&approaches(506.25)==0&&approaches(506.5)==1,
                "Nearby mobs must acquire the player in separate reaction windows");
        auto distant=players;distant.at(100).position.x+=500;
        auto exitEvents=spotAi.tick(spot,distant,506.75);
        require(std::none_of(exitEvents.begin(),exitEvents.end(),[](const auto& event){return event.kind==EntityEvent::Aggro;}),"Mob acquired an out-of-range player");
        auto levelSpot = entities;
        levelSpot.at(1).rules["wander_radius"] = 0.;
        auto highLevelPlayers = players;
        auto &highLevel = highLevelPlayers.at(100);
        highLevel.level = entity.level + 24;
        MobSimulation levelAi(catalog, 42, 700);
        levelAi.tick(levelSpot, highLevelPlayers, 700.25);
        require(levelSpot.at(1).detected_target == highLevel.eid, "Mob must detect a player 24 levels higher");
        highLevel.level++;
        const auto cancelledReaction = levelAi.tick(levelSpot, highLevelPlayers, 703.5);
        require(std::any_of(cancelledReaction.begin(),cancelledReaction.end(),[](const auto& event){return event.kind==EntityEvent::Disengage;}),
                "Cancelling passive detection must clear the client reaction state");
        levelAi.tick(levelSpot, highLevelPlayers, 707.);
        require(levelSpot.at(1).detected_target == 0 && levelSpot.at(1).state == MobState::Idle,
                "Mob 25 levels below the player must cancel passive detection");
        levelAi.provoke(levelSpot.at(1), highLevel);
        require(levelSpot.at(1).state == MobState::Chase && levelSpot.at(1).target == highLevel.eid,
                "A direct attack must provoke a mob even across the level gap");
        entities.at(1).rules["reaction_seconds"]=0.;
        auto acquired=ai.tick(entities, players, 1000.25);
        require(std::count_if(acquired.begin(),acquired.end(),[](const auto&e){return e.kind==EntityEvent::Aggro;})==1,"Detection must publish one native aggression notice");
        require(entities.at(1).state == MobState::Chase, "Mob did not acquire nearby player");
        auto chase=ai.tick(entities, players, 1000.50);
        auto nativeRun=getfloat(catalog.appearance(entity),38);
        require(chase.size()==1&&chase[0].kind==EntityEvent::Move&&chase[0].move_mode==1&&chase[0].move_speed==nativeRun,"Chase must use the actor's native running mode and speed without repeating aggro");
        require(entities.at(1).position.distance(entity.home) <= nativeRun*.25+.0001, "AI movement exceeds the speed published to the client");
        players[100].position = entities.at(1).position;
        auto reached = ai.tick(entities, players, 1000.65);
        require(entities.at(1).state == MobState::Attack && reached.size() == 1 &&
                    reached.front().kind == EntityEvent::Stop,
                "Mob must stop moving on the client before beginning an attack");
        players[100].hidden = true;
        ai.tick(entities, players, 1000.75);
        for (int i = 0; i < 10; i++)
            ai.tick(entities, players, 1001. + i * .25);
        require(entities.at(1).target == 0 && entities.at(1).position.distance(entity.home) < 1 &&
                    entities.at(1).hp == entities.at(1).maxhp,
                "Vanish or return-home recovery failed");
        entities.at(1).alive = false;
        entities.at(1).state = MobState::Dead;
        entities.at(1).hp = 0;
        entities.at(1).respawn_at = 2000;
        ai.tick(entities, players, 1999.9);
        require(!entities.at(1).alive, "Mob respawned before deadline");
        ai.tick(entities, players, 2000);
        require(entities.at(1).alive && entities.at(1).hp == entities.at(1).maxhp,
                "Mob did not respawn at deadline");
        entities.at(1).position = {entity.home.x + 100, entity.home.y, entity.home.z};
        entities.at(1).state = MobState::Returning;
        ai.tick(entities, players, 3000);
        require(entities.at(1).position.distance(entity.home) <= 100,
                "AI moved away while returning after a long pause");
        auto &returning = entities.at(1);
        returning.state = MobState::Returning;
        returning.position = {returning.home.x + std::min(10.,nativeRun*.25*.8), returning.home.y, returning.home.z};
        returning.hp = returning.maxhp - 1;
        returning.damage[100] = 1;
        auto returned = ai.tick(entities, players, 3000.25);
        require(returned.size() == 2 && returned[0].kind == EntityEvent::Move &&
                    returned[1].kind == EntityEvent::Reset && returning.position.distance(returning.home) == 0 &&
                    returning.hp == returning.maxhp && returning.hp_dirty && returning.damage.empty(),
                "Returning mob must stop at its exact spawn and publish recovered HP");
        auto unavailable = player;
        unavailable.ready = false;
        ai.provoke(returning, unavailable);
        require(returning.target == 0 && returning.state == MobState::Idle,
                "Mob accepted a player whose map session was not ready");
        auto catalog_rows = content.get("server/respawn_catalog.json");
        for (auto row = catalog_rows.begin(); row != catalog_rows.end(); ++row)
            if (row.value().value("native_effect", 0) == 97) {
                auto id = static_cast<std::uint32_t>(std::stoul(row.key()));
                require(catalog.event_level_allowed(id, 50, 45) && catalog.event_level_allowed(id, 50, 65) &&
                            !catalog.event_level_allowed(id, 50, 44) &&
                            !catalog.event_level_allowed(id, 50, 66),
                        "Event boss level boundaries incorrect");
                break;
            }
        entities.at(1) = entity;
        entities.at(1).state = MobState::Attack;
        entities.at(1).target = 100;
        entities.at(1).position = player.position;
        entities.at(1).impact_at = 3000.1;
        entities.at(1).debuffs = {{"stun",
                                   {{"expires", 4000.},
                                    {"caster_eid", 100},
                                    {"effects", Json::array({{{"operation", 85}, {"flat", 0}},
                                                             {{"operation", 86}, {"flat", 0}},
                                                             {{"operation", 87}, {"flat", 0}}})}}}};
        players[100].hidden = false;
        auto stopped = ai.tick(entities, players, 3000.5);
        require(entities.at(1).impact_at == 0 && stopped.size() == 1 && stopped[0].kind == EntityEvent::Stop,
                "Stun failed to cancel the pending attack");
        Entity attacker = entity;
        attacker.state = MobState::Attack;
        attacker.target = player.eid;
        attacker.position = attacker.home;
        attacker.attack_range = 50;
        attacker.rules["attack_windup_seconds"] = .6;
        std::map<std::uint32_t, Entity> windup_mobs{{1, attacker}};
        auto runner = player;
        runner.position = {attacker.home.x + 49, attacker.home.y, attacker.home.z};
        std::map<std::uint32_t, Observer> runners{{runner.eid, runner}};
        MobSimulation windup_ai(catalog, 95, 3500);
        auto started = windup_ai.tick(windup_mobs, runners, 3500.25);
        require(started.size() == 1 && started.front().kind == EntityEvent::AttackStart,
                "Mob must wind up when the target is within melee range");
        runners.at(runner.eid).position.x += 6;
        auto evaded = windup_ai.tick(windup_mobs, runners, 3501);
        require(evaded.size() == 1 && evaded.front().kind == EntityEvent::Stop &&
                    windup_mobs.at(1).impact_at == 0 && windup_mobs.at(1).state == MobState::Chase,
                "Leaving melee range must cancel the queued hit and signal stop");
        windup_mobs.at(1) = attacker;
        auto out_of_range = windup_ai.tick(windup_mobs, runners, 3501.25);
        require(out_of_range.empty() && windup_mobs.at(1).impact_at == 0 &&
                    windup_mobs.at(1).state == MobState::Chase,
                "Mob must resume chasing instead of starting a hit beyond melee range");
        windup_mobs.at(1) = attacker;
        runners.at(runner.eid).position.x -= 6;
        started = windup_ai.tick(windup_mobs, runners, 3501.5);
        require(started.size() == 1 && started.front().kind == EntityEvent::AttackStart,
                "Returning to range must allow a normal attack");
        auto impact = windup_ai.tick(windup_mobs, runners, 3502.25);
        require(impact.size() == 1 && impact.front().kind == EntityEvent::AttackImpact,
                "An in-range attack must still land after windup");
        // Service guards retain their own model, are not loot targets, and
        // cancel an already pending attack as soon as morality reaches zero.
        Entity guard=entity;guard.eid=3;guard.combat=false;guard.guard=true;guard.state=MobState::Idle;guard.target=0;guard.debuffs=Json::object();guard.position=guard.home=player.position;guard.aggro=350;guard.attack_range=50;
        std::map<std::uint32_t,Entity>guards{{3,guard}};players[100].hidden=false;players[100].moral=0;
        MobSimulation guardAI(catalog,91,4000);guardAI.tick(guards,players,4000.1);require(guards.at(3).target==0,"Guard attacks neutral morality");
        players[100].moral=-1;
        auto originalPosition=players[100].position;
        players[100].position.x+=100;
        auto distantEvents=guardAI.tick(guards,players,4000.2);
        require(guards.at(3).target==0&&distantEvents.empty(),"Guard pursues a player outside melee range");
        players[100].position=originalPosition;
        guardAI.tick(guards,players,4000.3);
        require(guards.at(3).target==100&&guards.at(3).state==MobState::Attack,"Guard ignores negative morality in reach");
        players[100].position.x+=100;
        guards.at(3).impact_at=4000.4;
        auto departed=guardAI.tick(guards,players,4000.4);
        require(guards.at(3).target==0&&guards.at(3).impact_at==0,"Guard kept an attack after the target left its reach");
        for(auto&e:departed)require(e.kind==EntityEvent::Stop,"Guard moved or landed a hit out of reach");
        players[100].position=originalPosition;
        guardAI.tick(guards,players,4000.5);
        guards.at(3).state=MobState::Attack;guards.at(3).impact_at=4000.6;players[100].moral=0;auto cancelled=guardAI.tick(guards,players,4000.6);require(guards.at(3).impact_at==0&&guards.at(3).target==0,"Guard completes attack after moral recovery");for(auto&e:cancelled)require(e.kind!=EntityEvent::AttackImpact,"Neutral player received pending guard damage");
        auto fixture = fs::temp_directory_path() / wide("rosh_spawn_persistence_" + token(8));
        fs::create_directories(fixture);
        {
            Sqlite db(fixture / "world.sqlite");
            SpawnPersistence::initialize(db);
            auto scope = SpawnPersistence::scope("berneo", 1, 19003);
            auto other = SpawnPersistence::scope("berneo", 2, 19003);
            Entity dead = entity;
            {
                Transaction tx(db);
                require(SpawnPersistence::claim(db, scope, dead, "first-death", 5000, 0),
                        "First death claim failed");
                tx.commit();
            }
            {
                Transaction tx(db);
                require(!SpawnPersistence::claim(db, scope, dead, "duplicate-death", 5001, 0),
                        "Repeated kill can pay twice");
                tx.commit();
            }
            auto restored = entity;
            SpawnPersistence::restore(db, scope, restored, 5001);
            require(!restored.alive && restored.respawn_at == 5000 + entity.rules.at("respawn_seconds").get<double>(),
                    "Death deadline was not restored across a reload");
            auto independent = entity;
            SpawnPersistence::restore(db, other, independent, 5001);
            require(independent.alive, "Public channels share their ordinary mob deaths");
            require(SpawnPersistence::scope("Arena", 1, 1900329) ==
                        SpawnPersistence::scope("Arena", 4, 1900329),
                    "Hidden special map scope not shared");
        }
        fs::remove_all(fixture);
        std::cout << "PASS: " << maps << " maps, " << loaded << " native placements (" << services
                  << " services, " << combat
                  << " mobs); exact model IDs; boss health/timers; aggro; bounded movement; vanish; "
                     "return-home; stun; durable respawn; duplicate death rejection; public/special scopes\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
