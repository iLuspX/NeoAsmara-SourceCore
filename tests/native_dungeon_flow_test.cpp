#include "native/game.hpp"
#include "native/inventory.hpp"
#include <iostream>
using namespace rosh;
static void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
struct Packet{int cat,sub;Bytes body;};
int wmain(int argc,wchar_t** argv){try{
    check(argc==2||argc==3,"Expected isolated dungeon fixture and optional all/berneo/peltrok/tickets/public focus");const fs::path root=argv[1];
    const auto focus=argc==3?fs::path(argv[2]).string():"all";
    check(focus=="all"||focus=="berneo"||focus=="peltrok"||focus=="tickets"||focus=="public","Unknown dungeon test focus");
    check(fs::exists(root/"GM_TEST_FIXTURE"),"Refusing live player databases");
    Content content(root);LegacyStore store(root/"Database/karos_gameplay.sqlite");
    auto scene=std::make_shared<WorldScene>(content);auto typed=content.database("database/content_typed.sqlite");
    store.db().exec("CREATE TABLE IF NOT EXISTS character_locations(eid INTEGER PRIMARY KEY,map_name TEXT NOT NULL,x REAL NOT NULL,y REAL NOT NULL,z REAL NOT NULL,angle REAL NOT NULL,updated_at REAL NOT NULL)");
    auto seed=[&](std::uint32_t eid,int cls,std::uint32_t keeper,const std::string& world="berneo",const Json& coordinates=Json()){
        const auto actors=scene->map(world);const Entity* actor=nullptr;
        for(const auto& entry:*actors)if(entry.second.npc_id==keeper){actor=&entry.second;break;}
        check(actor||coordinates.is_array(),"Native dungeon keeper is absent");
        const std::string account="dungeon-flow-"+token(10);
        int map_id=0;for(const auto& map:content.get("data/maplist.json"))if(map.at("name")==world){map_id=map.at("head").at(1);break;}
        check(map_id>0,"Fixture outside map is unknown");
        Json ch={{"eid",eid},{"name","DungeonFixture"},{"cls",cls},{"level",115},{"exp",Progression().threshold(115)},{"hp",100},{"mp",100},{"fletta",0},{"fletta_energy",0},{"skill_points",0},{"moral_value",0},{"map_id",map_id}};
        store.db().exec("INSERT INTO accounts VALUES(?,?)",{account,Json{{"chars",Json::array({ch})}}.dump()});
        const Position origin=coordinates.is_array()?Position{coordinates.at(0),coordinates.at(1),coordinates.at(2)}:actor->position;
        store.db().exec("INSERT INTO character_locations VALUES(?,?,?,?,?,?,?)",{std::int64_t(eid),world,origin.x,origin.y,origin.z,0.,now()});
        Session session;session.id=token(20);session.identity.id=account;session.identity.locale="en";session.channel=1;session.expires=now()+3600;
        return std::make_pair(session,actor?actor->eid:0u);
    };
    auto ready=[](WorldSession& game,const WorldSession::Sender& send){Bytes b(21);put32(b,1,game.eid(),true);for(int i=0;i<3;i++)putfloat(b,5+4*i,game.character().at("pos").at(i));game.handle(4,0,b,send);};
    if(focus=="all"||focus=="berneo")for(int cls:{100,200,300,400,500,600,800}){
        const auto eid=std::uint32_t(920000+cls);auto [session,keeper]=seed(eid,cls,9399901);
        std::vector<Packet> packets;auto send=[&](unsigned char cat,unsigned char sub,const Bytes& b){packets.push_back({cat,sub,b});};
        std::string run;
        {
            WorldSession game(content,store,session,root,eid,scene);ready(game,send);
            Inventory inventory(store.db(),content);Wallet wallet(store.db(),inventory);wallet.apply(eid,10000,"dungeon-funds-"+std::to_string(eid));
            const auto before=wallet.balance(eid);Bytes request(14);put32(request,0,19391,true);put32(request,4,29003,true);put32(request,8,keeper,true);put16(request,12,0x123,true);
            packets.clear();game.handle(4,7,request,send,"dungeon-admission-"+std::to_string(eid));
            check(!game.ready(),"Dungeon entrance did not start native loading");
            bool change=false;for(const auto& p:packets)if(p.cat==4&&p.sub==4){check(p.body[8]==1&&get32(p.body,9,true)==19391&&get32(p.body,13,true)==29003,"Native dungeon initialization fields lost");change=true;}
            check(change&&wallet.balance(eid)==before-500,"Dungeon fee/transition mismatch");
            game.handle(4,7,request,send,"dungeon-admission-"+std::to_string(eid));check(wallet.balance(eid)==before-500,"Duplicate admission charged twice");
            packets.clear();ready(game,send);
            std::map<std::uint32_t,std::set<std::uint32_t>> references;
            for(const auto& p:packets)if(p.cat==4&&p.sub==2&&p.body.size()==254&&p.body[0]==0&&p.body[1]==1)references[get32(p.body,64)].insert(get32(p.body,68));
            check(std::any_of(references.begin(),references.end(),[](const auto& row){return row.second.size()>1;}),"Dungeon mobs of the same type lost their individual native references");
            const auto member=store.db().query("SELECT run_id FROM dungeon_members WHERE eid=? AND inside=1",{std::int64_t(eid)});
            check(member.size()==1,"Dungeon membership was not saved");run=member.front().at("run_id");
            check(!scene->map("catacomb02#29003#run:"+run)->empty(),"Private dungeon has no mobs");
        }
        const auto gate=typed->query("SELECT o8_f64 AS x,o16_f64 AS y,o40_f64 AS z FROM mapWarpGate WHERE o0_i=19303").front();
        store.db().exec("UPDATE character_locations SET x=?,y=?,z=? WHERE eid=?",{gate.at("x").get<double>(),gate.at("y").get<double>(),gate.at("z").get<double>(),std::int64_t(eid)});
        session.id=token(20);
        {
            WorldSession reconnected(content,store,session,root,eid,scene);ready(reconnected,send);
            check(store.db().query("SELECT run_id FROM dungeon_members WHERE eid=? AND inside=1",{std::int64_t(eid)}).front().at("run_id")==run,"Reconnect allocated a fresh dungeon");
            Bytes exit(6);put32(exit,0,19303,true);put16(exit,4,0x123,true);packets.clear();reconnected.handle(4,6,exit,send,"dungeon-exit-"+std::to_string(eid));
            check(reconnected.character().at("map_id")==19003&&!reconnected.ready(),"Dungeon exit did not return to Berneo");
            check(store.db().query("SELECT status FROM dungeon_runs WHERE id=?",{run}).front().at("status")=="closed","Last departure did not close the run");
            ready(reconnected,send);check(reconnected.ready(),"Outside map did not reload after exit");
        }
    }
    // The Peltrok keeper must use mapWarpTargetPos, not the interior rgn_default marker.
    if(focus=="all"||focus=="peltrok")for(int cls:{100,200,300,400,500,600,800}){
        constexpr std::uint32_t map=29015;
        const auto route=typed->query("SELECT o0_i AS route_group,o8_i AS keeper FROM mapWarp WHERE o16_i=?",{std::int64_t(map)}).front();
        const auto native=typed->query("SELECT o8_f64 AS x,o16_f64 AS y,o24_f64 AS z FROM mapWarpTargetPos WHERE o0_i=?",{std::int64_t(map)}).front();
        const auto eid=940000+cls;auto [session,keeper]=seed(eid,cls,route.at("keeper"),"Peltrok");
        WorldSession game(content,store,session,root,eid,scene);
        std::vector<Packet> packets;auto send=[&](unsigned char cat,unsigned char sub,const Bytes& body){packets.push_back({cat,sub,body});};ready(game,send);
        Inventory inventory(store.db(),content);Wallet wallet(store.db(),inventory);wallet.apply(eid,100000,"peltrok-funds-"+std::to_string(cls));
        Bytes request(14);put32(request,0,route.at("route_group"),true);put32(request,4,map,true);put32(request,8,keeper,true);put16(request,12,0x123,true);
        game.handle(4,7,request,send,"peltrok-entry-"+std::to_string(cls));
        check(!game.ready()&&game.character().at("map_id")==map,"Peltrok keeper did not admit character");
        packets.clear();ready(game,send);
        const auto initial=content.get("server/dungeon_rules.json").at("maps").at("29015").value("initial_packets",Json::array());
        if(!initial.empty())check(std::count_if(packets.begin(),packets.end(),[](const Packet& p){return p.cat==10&&p.sub==4&&p.body.size()==12;})==static_cast<int>(initial.size()),"Peltrok map ready omitted native maze-door state");
        const auto position=game.character().at("pos");
        for(int axis=0;axis<3;++axis)check(std::abs(position.at(axis).get<double>()-native.at(axis==0?"x":axis==1?"y":"z").get<double>())<.01,"Peltrok keeper bypassed the native entrance and teleported inside a room");
        const auto destinations=native_map_destinations(content);
        const auto destination=std::find_if(destinations.begin(),destinations.end(),[](const Json& row){return row.at("map_id")==map;});
        check(destination!=destinations.end(),"Peltrok dungeon destination is absent");
        for(auto axis:{"x","y","z"})check(destination->at(axis)==native.at(axis),"GM/map destination disagrees with the native NPC entrance");
    }
    if(focus=="all"||focus=="tickets")for(const auto map:{29014u,29016u}){
        const auto routes=typed->query("SELECT o0_i AS route_group,o8_i AS keeper,o24_i AS requirement FROM mapWarp WHERE o16_i=?",{std::int64_t(map)});
        check(!routes.empty(),"Missing ticket dungeon route");const auto route=routes.front();
        const auto eid=930000+map;auto [session,keeper]=seed(eid,600,route.at("keeper"),map==29016?"Tempteroon":"berneo");
        std::vector<Packet> packets;auto send=[&](unsigned char cat,unsigned char sub,const Bytes& b){packets.push_back({cat,sub,b});};
        WorldSession game(content,store,session,root,eid,scene);ready(game,send);
        Inventory inventory(store.db(),content);Wallet wallet(store.db(),inventory);wallet.apply(eid,100000,"ticket-funds-"+std::to_string(map));
        const auto limits=typed->query("SELECT o16_i AS item,o24_i AS consume FROM mapWarpStat WHERE o0_i=? AND o4_i=0",{route.at("requirement").get<std::int64_t>()});
        check(!limits.empty()&&limits.front().at("item").get<int>()>0,"Missing native entrance ticket");
        const auto ticket=limits.front().at("item").get<std::uint32_t>();
        auto count=[&]{std::int64_t result=0;for(const auto& item:inventory.items(eid))if(item.item==ticket)result+=item.quantity;return result;};
        Bytes request(14);put32(request,0,route.at("route_group"),true);put32(request,4,map,true);put32(request,8,keeper,true);put16(request,12,0x123,true);
        const auto outside=game.character().at("map_id");const auto money=wallet.balance(eid);game.handle(4,7,request,send,"missing-ticket-"+std::to_string(map));
        check(game.ready()&&game.character().at("map_id")==outside&&wallet.balance(eid)==money,"Missing ticket admitted or charged the character");
        check(store.db().query("SELECT 1 FROM dungeon_members WHERE eid=? AND inside=1",{std::int64_t(eid)}).empty(),"Failed ticket admission left membership");
        inventory.grant_once(eid,{{ticket,2}},"fixture-tickets-"+std::to_string(map));
        game.handle(4,7,request,send,"valid-ticket-"+std::to_string(map));
        check(!game.ready()&&game.character().at("map_id")==map,"Valid native ticket was refused");
        const auto remaining=2-(limits.front().at("consume")==1?1:0);check(count()==remaining,"Entrance consumed the wrong ticket quantity");
        game.handle(4,7,request,send,"valid-ticket-"+std::to_string(map));check(count()==remaining,"Duplicate entrance consumed another ticket");
        ready(game,send);
        const auto rule=content.get("server/dungeon_rules.json").at("maps").at(std::to_string(map));
        const auto native=typed->query("SELECT o8_f64 AS x,o16_f64 AS y,o24_f64 AS z FROM mapWarpTargetPos WHERE o0_i=?",{std::int64_t(map)}).front();
        for(int axis=0;axis<3;++axis)check(std::abs(game.character().at("pos").at(axis).get<double>()-native.at(axis==0?"x":axis==1?"y":"z").get<double>())<.01,"Valid native ticket-dungeon entrance was replaced by scene marker");
        if(map==29016){
            const auto run=store.db().query("SELECT run_id FROM dungeon_members WHERE eid=? AND inside=1",{std::int64_t(eid)}).front().at("run_id").get<std::string>();
            const auto name=rule.at("name").get<std::string>()+"#29016#run:"+run;
            std::size_t dormant=0;for(const auto& [id,mob]:*scene->map(name))dormant+=mob.rules.value("dungeon_dormant",false);
            const auto trigger_position=rule.at("triggers").at("200").at("position");
            Bytes walk(34);put32(walk,2,eid,true);for(int axis=0;axis<3;++axis)putfloat(walk,6+axis*4,trigger_position.at(axis));game.handle(4,1,walk,send);
            Bytes trigger(8);put32(trigger,0,map,true);put32(trigger,4,200,true);packets.clear();game.handle(10,1,trigger,send);
            check(std::any_of(packets.begin(),packets.end(),[](const Packet& p){return p.cat==10&&p.sub==0;}),"Bell-tower trigger did not send native scenario actions");
            std::size_t after=0;for(const auto& [id,mob]:*scene->map(name))after+=mob.rules.value("dungeon_dormant",false);
            check(after<dormant,"Bell-tower first wave stayed dormant");
        }
    }
    if(focus=="all"||focus=="public")for(const auto gate_id:{295902u,19400u}){
        const auto gate=typed->query("SELECT o4_i AS map_id,o8_f64 AS x,o16_f64 AS y,o40_f64 AS z,o60_i AS target FROM mapWarpGate WHERE o0_i=?",{std::int64_t(gate_id)}).front();
        std::string world;for(const auto& map:content.get("data/maplist.json"))if(map.at("head").at(1)==gate.at("map_id"))world=map.at("name");
        const auto eid=960000+gate_id;auto [session,ignored]=seed(eid,100,0,world,Json::array({gate.at("x"),gate.at("y"),gate.at("z")}));
        std::vector<Packet> packets;auto send=[&](unsigned char cat,unsigned char sub,const Bytes& body){packets.push_back({cat,sub,body});};
        WorldSession game(content,store,session,root,eid,scene);ready(game,send);
        Bytes portal(6);put32(portal,0,gate_id,true);put16(portal,4,0x123,true);packets.clear();game.handle(4,6,portal,send,"public-entry-"+std::to_string(gate_id));
        const auto destination=gate_id==295902u?29501:29502;
        if(game.character().at("map_id")!=destination||game.ready()){
            for(const auto& packet:packets)if(packet.cat==11)std::cerr<<"Public dungeon refusal: "<<std::string(packet.body.begin(),packet.body.end())<<'\n';
            throw std::runtime_error("Public dungeon portal did not load its native destination");
        }
        ready(game,send);
        check(store.db().query("SELECT 1 FROM dungeon_members WHERE eid=? AND inside=1",{std::int64_t(eid)}).empty(),"Public dungeon incorrectly created a private instance");
        if(destination==29501){
            const auto floor=typed->query("SELECT o8_f64 AS x,o16_f64 AS y,o40_f64 AS z FROM mapWarpGate WHERE o0_i=295104").front();
            Bytes walk(34);put32(walk,2,eid,true);for(int axis=0;axis<3;++axis)putfloat(walk,6+4*axis,floor.at(axis==0?"x":axis==1?"y":"z"));game.handle(4,1,walk,send);
            put32(portal,0,295104,true);packets.clear();game.handle(4,6,portal,send,"public-ant-floor");
            check(game.ready()&&game.character().at("pos").at(0).get<double>()>13000,"Antcave internal floor portal failed");
            check(std::none_of(packets.begin(),packets.end(),[](const Packet& p){return p.cat==4&&p.sub==4;}),"Antcave internal floor change reloaded the world");
        }
    }
    EntityCatalog catalog(content);
    const auto dungeon_definitions=content.get("server/dungeon_rules.json").at("maps");
    if(focus=="all")for(const auto& entry:dungeon_definitions.items()){
        const auto definition=entry.value();if(!definition.value("instanced",false))continue;
        const auto name=definition.at("name").get<std::string>()+"#"+entry.key()+"#run:coverage";
        const auto actors=catalog.load(name);check(!actors.empty(),"Native dungeon is empty");
        std::set<std::uint32_t> refs;
        for(const auto& [eid,mob]:actors){check(mob.rules.value("no_respawn",false),"Dungeon mob retained ordinary field respawn");const auto ref=get32(catalog.appearance(mob),68);check(ref>0&&refs.insert(ref).second,"Duplicate/missing native dungeon spawn reference");}
    }
    std::cout<<"PASS: dungeon flow focus="<<focus<<"; applicable keeper/portal, entrance coordinates, maze initialization, ticket/replay and instance checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}}
