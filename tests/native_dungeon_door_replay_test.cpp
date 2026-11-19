#include "native/dungeons.hpp"
#include <iostream>
using namespace rosh;
int wmain(int argc,wchar_t** argv){try{
    if(argc!=4||fs::exists(argv[2]))throw std::runtime_error("Expected content root, NEW isolated database, and door capture fixture");
    Content content(argv[1]);Sqlite db(argv[2]);DungeonStore store(db,content);
    const auto cases=Json::parse(read_file(argv[3])).at("cases");
    int verified=0;
    for(const auto& entry:cases.items()){
        const auto map=static_cast<std::uint32_t>(std::stoul(entry.key()));
        const auto rule=store.rule(map);
        const auto spawns=content.get("data/spawns.json").at(rule.at("spawn_key").get<std::string>());
        const auto run=store.join(980001,"capture:"+entry.key(),map,1,Json::object(),1000.);
        const auto group="run:"+run.at("id").get<std::string>();
        for(const auto& step:entry.value()){
            if(step.contains("kill")){
                for(std::size_t index=0;index<spawns.size();++index)if(spawns[index].value("native_reference",0u)==step.at("kill")){
                    Entity mob;mob.combat=true;mob.npc_id=spawns[index].at("id");mob.spawn_index=index;
                    mob.rules["native_reference"]=step.at("kill");store.kill(group,mob,1001.);break;
                }
            }else if(step.contains("trigger")){
                const auto id=step.at("trigger").get<std::uint32_t>();
                const auto p=rule.at("triggers").at(std::to_string(id)).at("position");
                store.trigger(group,980001,map,id,{p.at(0),p.at(1),p.at(2)},1002.);
            }else if(step.contains("expect")){
                const auto events=store.events(group,980001,0,1003.);
                const auto wanted=step.at("expect").get<std::string>();
                if(std::none_of(events.begin(),events.end(),[&](const Json& event){return event.at("body")==wanted;}))
                    throw std::runtime_error("Missing captured door/passage event in map "+entry.key()+" at frame "+step.at("frame").dump()+": "+wanted);
                ++verified;
            }
        }
    }
    std::cout<<"PASS: "<<verified<<" captured door/passage/wave events reproduced by the Core across "<<cases.size()<<" dungeon variants\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
