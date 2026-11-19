#include "native/game.hpp"
#include <iostream>
using namespace rosh;
int wmain(int argc,wchar_t** argv){try{
    if(argc!=2)throw std::runtime_error("Expected isolated authenticated content root");
    Content content(argv[1]);EntityCatalog catalog(content);
    const auto definitions=content.get("server/dungeon_rules.json").at("maps");
    const auto spawns=content.get("data/spawns.json");
    int maps=0,actors=0,callbacks=0;
    for(const auto& entry:definitions.items()){
        const auto& rule=entry.value();
        if(!rule.contains("spawn_key"))continue;
        const auto source=rule.at("spawn_key").get<std::string>();
        if(!spawns.contains(source)||spawns.at(source).empty())throw std::runtime_error("Missing dungeon actors: "+entry.key());
        const auto name=rule.at("name").get<std::string>()+"#"+entry.key()+"#run:resource-audit";
        const auto entities=catalog.load(name,{});
        if(entities.size()!=spawns.at(source).size())throw std::runtime_error("Dungeon actor count differs from reviewed native MO: "+entry.key());
        std::set<std::uint32_t> references;
        for(const auto& [id,actor]:entities){
            const auto packet=catalog.appearance(actor);const auto ref=get32(packet,68);
            if(!ref||!references.insert(ref).second)throw std::runtime_error("Invalid/duplicate native dungeon reference: "+entry.key());
            if(!std::isfinite(actor.position.x)||!std::isfinite(actor.position.y)||!std::isfinite(actor.position.z))throw std::runtime_error("Non-finite dungeon spawn");
        }
        for(const auto& trigger:rule.at("triggers")){
            if(trigger.at("position").size()!=3)throw std::runtime_error("Invalid native trigger coordinates");
            callbacks+=trigger.at("events").size();
        }
        for(const auto& event:rule.at("kill_events"))callbacks+=event.size();
        ++maps;actors+=entities.size();
    }
    std::cout<<"PASS: "<<maps<<" configured dungeon variants, "<<actors<<" native actors, "<<callbacks<<" callback actions; all actor references and coordinates validated\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
