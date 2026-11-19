#include "native/game.hpp"
#include <algorithm>
#include <limits>
#include <iostream>
using namespace rosh;
static void check(bool pass,const char* message){if(!pass)throw std::runtime_error(message);}
struct Packet{int cat,sub;Bytes body;};
int wmain(int argc,wchar_t**argv){try{
 check(argc==2&&fs::exists(fs::path(argv[1])/"GM_TEST_FIXTURE"),"Isolated action fixture required");
 const fs::path root=argv[1];Content content(root);LegacyStore store(root/"Database/karos_gameplay.sqlite");
 auto scene=std::make_shared<WorldScene>(content);Position position{};bool found=false;
 for(const auto& [id,npc]:*scene->map("berneo"))if(npc.npc_id==9300111){position=npc.position;found=true;break;}
 check(found,"Native safe instructor position missing");
 store.db().exec("CREATE TABLE IF NOT EXISTS character_locations(eid INTEGER PRIMARY KEY,map_name TEXT NOT NULL,x REAL NOT NULL,y REAL NOT NULL,z REAL NOT NULL,angle REAL NOT NULL,updated_at REAL NOT NULL)");
 for(int family:{100,200,300,400,500,600,800}){
  const std::uint32_t eid=956000+family;const auto account="actions-"+token(12);
  Json character={{"eid",eid},{"name","ActionFixture"},{"cls",family},{"level",115},{"exp",Progression().threshold(115)},
   {"hp",100},{"mp",100},{"fletta",0},{"skill_points",0},{"moral_value",0},{"map_id",19003}};
  store.db().exec("INSERT INTO accounts VALUES(?,?)",{account,Json{{"chars",Json::array({character})}}.dump()});
  store.db().exec("INSERT INTO character_locations VALUES(?,'berneo',?,?,?,0,?)",{std::int64_t(eid),position.x,position.y,position.z,now()});
  Session identity;identity.id=token(20);identity.identity.id=account;identity.identity.locale="en";identity.channel=1;identity.expires=now()+600;
  std::vector<Packet> packets;const auto send=[&](unsigned char cat,unsigned char sub,const Bytes&body){packets.push_back({cat,sub,body});};
  WorldSession game(content,store,identity,root,eid,scene);Bytes ready(21);put32(ready,1,eid,true);
  for(int axis=0;axis<3;++axis)putfloat(ready,5+axis*4,game.character().at("pos").at(axis));
  game.handle(4,0,ready,send);
  const auto has_pose=[&](int mode){return std::any_of(packets.begin(),packets.end(),[&](const auto& p){return p.cat==3&&p.sub==2&&p.body.size()==11&&get32(p.body,0,true)==eid&&p.body[4]==mode;});};
  Bytes sit(11);put32(sit,0,eid,true);sit[4]=1;packets.clear();game.handle(3,2,sit,send);
  check(has_pose(1),"Native sit was ignored for a class");
  check(std::none_of(packets.begin(),packets.end(),[](const auto& p){return p.cat==4&&p.sub==4;}),"Sitting reloaded the map");
  auto invalid=sit;put32(invalid,0,eid+1,true);packets.clear();game.handle(3,2,invalid,send);check(!has_pose(1),"Spoofed actor action accepted");
  invalid=sit;invalid[4]=3;packets.clear();game.handle(3,2,invalid,send);check(!has_pose(3),"Unproven emotion invented from pose mode");
  auto stand=sit;stand[4]=0;packets.clear();game.handle(3,2,stand,send);check(has_pose(0),"Native stand was ignored");
  game.handle(3,2,sit,send);packets.clear();Bytes move(34);put32(move,2,eid,true);
  for(int axis=0;axis<3;++axis){putfloat(move,6+axis*4,game.character().at("pos").at(axis));putfloat(move,18+axis*4,game.character().at("pos").at(axis));}
  game.handle(4,1,move,send);check(has_pose(0),"Movement did not end the seated pose");
  check(!game.character().contains("sitting"),"Transient pose leaked into character persistence");
  const auto before=game.character();
  auto has_emotion=[&](std::uint32_t id,int status){return std::any_of(packets.begin(),packets.end(),[&](const auto& p){return p.cat==5&&p.sub==3&&p.body.size()==36&&get32(p.body,5,true)==id&&get16(p.body,34,true)==status;});};
  for(const auto emotion:{11u,12u,13u,14u}){
   Bytes action(36);put32(action,1,eid,true);put32(action,5,emotion,true);put32(action,9,1,true);
   for(int axis=0;axis<3;++axis)putfloat(action,13+axis*4,game.character().at("pos").at(axis).get<double>()+100);
   put32(action,30,eid,true);put16(action,34,0x24,true);packets.clear();game.handle(5,3,action,send);
   check(has_emotion(emotion,0x24),"Native common emotion was treated as an unavailable combat skill");
   check(game.character()==before,"Common emotion spent mana, points or changed saved state");
   for(const auto& p:packets)if(p.cat==5&&p.sub==3)check(getfloat(p.body,13)==game.character().at("pos").at(0).get<float>(),"Emotion relayed forged coordinates");
   auto invalid=action;put32(invalid,30,eid+1,true);packets.clear();game.handle(5,3,invalid,send);
   check(has_emotion(emotion,0x21)&&!has_emotion(emotion,0x24),"Emotion accepted another actor target or failed to reset native client state");
   invalid=action;put32(invalid,9,2,true);packets.clear();game.handle(5,3,invalid,send);
   check(has_emotion(emotion,0x21),"Emotion accepted an invented rank");
   invalid=action;putfloat(invalid,13,std::numeric_limits<float>::infinity());packets.clear();game.handle(5,3,invalid,send);
   check(has_emotion(emotion,0x21),"Emotion accepted non-finite coordinates");
   check(game.character()==before,"Rejected emotion changed character state");
  }
 }
 std::cout<<"PASS: seven classes; native sit/stand and movement; four native emotions with own target/rank/coordinates; unavailable reply resets client; no mana/point/save changes or map reload\n";
 return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}}
