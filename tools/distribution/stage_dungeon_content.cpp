#include "native/core.hpp"
#include <iostream>
using namespace rosh;
int wmain(int argc,wchar_t** argv){try{
    if(argc!=4&&argc!=5)throw std::runtime_error("Expected installed server, reviewed resource folder, NEW staging folder, and optional --rules-only");
    if(argc==5&&std::wstring(argv[4])!=L"--rules-only")throw std::runtime_error("Unknown staging option");
    fs::path installed=argv[1], reviewed=argv[2], stage=argv[3];
    if(fs::exists(stage))throw std::runtime_error("Staging folder must be new");
    const auto key=unprotect_key(installed/"Config/Keys/content.key.dpapi");
    auto manifest=Json::parse(decrypt_content(read_file(installed/"Data/catalog.dat"),key,"catalog"));
    auto& resources=manifest.at("resources");
    const std::vector<std::string> names=argc==5?std::vector<std::string>{"server/dungeon_rules.json"}:
        std::vector<std::string>{"data/spawns.json","server/native_npc_prototypes.json","server/dungeon_rules.json"};
    std::map<std::string,Json> bundles,expected;
    std::map<std::string,std::string> categories;
    for(const auto& name:names){
        auto entry=resources.at(resources.contains(name)?name:"server/native_map_arrivals.json");
        const auto filename=entry.at("file").get<std::string>(),category=entry.at("category").get<std::string>();
        if(fs::path(filename).filename()!=filename)throw std::runtime_error("Invalid bundle path");
        if(!bundles.count(filename)){
            const auto bytes=read_file(installed/"Data"/filename);
            if(hex(sha256(bytes))!=entry.at("sha256"))throw std::runtime_error("Installed content authentication failed");
            bundles[filename]=Json::parse(decrypt_content(bytes,key,category));categories[filename]=category;
        }
        const auto bytes=read_file(reviewed/name);auto content=Json::parse(bytes);
        if(name=="data/spawns.json"||name=="server/native_npc_prototypes.json"){
            auto merged=bundles[filename].at(name);
            const auto definitions=Json::parse(read_file(reviewed/"server/dungeon_rules.json")).at("maps");
            for(const auto& definition:definitions){
                if(!definition.contains("spawn_key"))continue;
                for(const auto& map:{definition.at("spawn_key").get<std::string>(),definition.at("name").get<std::string>()}){
                    if(name=="data/spawns.json"){
                        if(!content.contains(map)||content.at(map).empty())throw std::runtime_error("Empty reviewed dungeon");
                        merged[map]=content.at(map);
                    }else merged["maps"][map]=content.at("maps").at(map);
                }
            }
            content=std::move(merged);
        }
        bundles[filename][name]=content;expected[name]=content;
        entry["member"]=name;resources[name]=entry;manifest["sourceOverrides"][name]=hex(sha256(bytes));
    }
    for(const auto& [filename,bundle]:bundles){
        const auto text=bundle.dump(),category=categories.at(filename);
        const auto bytes=encrypt_content(Bytes(text.begin(),text.end()),key,category);const auto digest=hex(sha256(bytes));
        for(auto& entry:resources)if(entry.at("file")==filename)entry["sha256"]=digest;
        write_file(stage/"Data"/filename,bytes);
    }
    const auto text=manifest.dump();write_file(stage/"Data/catalog.dat",encrypt_content(Bytes(text.begin(),text.end()),key,"catalog"));
    write_file(stage/"Config/Keys/content.key.dpapi",read_file(installed/"Config/Keys/content.key.dpapi"));
    Content verify(stage);
    for(const auto& [name,content]:expected)if(verify.get(name)!=content)throw std::runtime_error("Staged dungeon readback failed");
    std::cout<<"PASS: "<<names.size()<<" dungeon resources staged; existing bundles preserved; authenticated readback\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}}
