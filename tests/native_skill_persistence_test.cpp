#include "native/game.hpp"
#include <iostream>
using namespace rosh;
static void check(bool value,const char*text){if(!value)throw std::runtime_error(text);}
int wmain(int argc,wchar_t**argv){try{
 check(argc==2,"Use an isolated copied fixture");fs::path root=argv[1];check(fs::is_regular_file(root/"GM_TEST_FIXTURE"),"Refusing live account database");Content content(root);SkillBook book(content);auto classes=content.get("server/class_world_templates.json");
 for(auto entry=classes.begin();entry!=classes.end();++entry){Json ch={{"eid",73000},{"cls",std::stoi(entry.key())},{"level",120}};check(book.initialize(ch),"Initial skill seed missing");auto original=ch.at("skills");check(!book.initialize(ch)&&ch.at("skills")==original,"Re-entry overwrote learned skills");auto carrier=unhex(entry.value().at("packets").at("02/05").at("hex"));QuickSlots::initialize(ch,carrier);QuickSlots::patch_entry(ch,carrier);check(ch.at("quickslots").size()==36,"Native bar size changed");auto list=book.list(ch,unhex(entry.value().at("packets").at("05/05").at("hex")));check(list.size()==6+list[5]*8u,"Malformed skill list");}
 constexpr std::uint32_t eid=73001;Progression progression;LegacyStore store(root/"Database/karos_gameplay.sqlite");
 Json ch={{"eid",eid},{"name","SkillSaveFixture"},{"cls",100},{"level",120},{"exp",progression.threshold(120)},{"hp",100000},{"mp",100000},{"skill_points",10000},{"fletta",5000},{"fletta_energy",0},{"moral_value",0}};
 store.db().exec("INSERT INTO accounts(id,payload) VALUES(?,?)",{std::string("skill-save-fixture"),Json{{"chars",Json::array({ch})}}.dump()});
 Session identity;identity.identity.id="skill-save-fixture";identity.id=token(20);identity.channel=1;identity.expires=now()+600;
 auto scene=std::make_shared<WorldScene>(content);std::uint32_t learned=0;Bytes allocation(18),quick(20);Json expectedPaths,expectedBar;int pointsAfter=0;
 struct Packet{int cat,sub;Bytes body;};std::vector<Packet>packets;auto send=[&](unsigned char cat,unsigned char sub,const Bytes&body){packets.push_back({cat,sub,body});};
 auto ready=[&](WorldSession&game){Bytes body(21);put32(body,1,eid,true);putfloat(body,5,9020);putfloat(body,9,6590);putfloat(body,13,627);game.handle(4,0,body,send);};
 {
  WorldSession game(content,store,identity,root,eid,scene);ready(game);auto catalog=content.get("data/skill_catalog.json").at("skills");
  for(auto it=catalog.begin();it!=catalog.end();++it){auto id=static_cast<std::uint32_t>(std::stoul(it.key()));auto&row=it.value();int kind=row.at("kind");if(book.allowed(100,id)&&!game.character().at("skills").contains(it.key())&&row.at("levels").contains("1")&&(row.value("passive",false)||kind==11||kind==13||kind==21||kind==23||kind==25)){learned=id;break;}}
  check(learned,"No learnable class fixture skill");Bytes learn(10);put32(learn,0,eid,true);put32(learn,4,learned,true);put16(learn,8,36,true);game.handle(5,6,learn,send,"skill-save-learn");check(game.character().at("skills").value(std::to_string(learned),0)==1,"Native learn handler did not save skill");pointsAfter=game.character().at("skill_points");game.handle(5,6,learn,send,"skill-save-learn");check(game.character().at("skill_points")==pointsAfter,"Repeated skill request spent twice");
  auto attribute=content.get("data/fletta_attributes.json")[0];put32(allocation,0,eid,true);put32(allocation,4,attribute.at("group"),true);put32(allocation,8,attribute.at("path"),true);put32(allocation,12,1,true);put16(allocation,16,0x120,true);game.handle(8,4,allocation,send,"skill-save-fletta");check(game.character().at("fletta")==4999,"Fletta not debited");game.handle(8,4,allocation,send,"skill-save-fletta");check(game.character().at("fletta")==4999,"Repeated Fletta spent twice");expectedPaths=game.character().at("fletta_paths");
  store.db().exec("CREATE TRIGGER fixture_fail_skill BEFORE UPDATE ON accounts WHEN NEW.id='skill-save-fixture' BEGIN SELECT RAISE(ABORT,'fixture transaction rejection'); END");game.handle(8,4,allocation,send,"skill-save-forced-fail");check(game.character().at("fletta")==4999&&game.character().at("fletta_paths")==expectedPaths,"Failed checkpoint spent Fletta or mutated paths");store.db().exec("DROP TRIGGER fixture_fail_skill");
  put32(quick,0,eid,true);put32(quick,4,35,true);put32(quick,8,learned);put32(quick,12,9);put32(quick,16,0);game.handle(6,11,quick,send,"skill-save-hotbar-skill");
  auto potion=quick;put32(potion,4,34,true);put32(potion,8,50001);put32(potion,12,1);put32(potion,16,0);game.handle(6,11,potion,send,"skill-save-hotbar-potion");expectedBar=game.character().at("quickslots");check(expectedBar[35][0]==learned&&expectedBar[34][0]==50001,"Skill/potion binding lost");
  auto forged=quick;put32(forged,0,eid+1,true);game.handle(6,11,forged,send,"skill-save-forged-hotbar");check(game.character().at("quickslots")==expectedBar,"Foreign bar identity accepted");
  Bytes full(436);put32(full,0,eid,true);for(std::size_t i=0;i<36;i++)for(std::size_t c=0;c<3;c++)put32(full,4+i*12+c*4,expectedBar[i][c]);game.handle(6,82,full,send,"skill-save-full-hotbar");
 }
 packets.clear();identity.id=token(20);
 {
  WorldSession game(content,store,identity,root,eid,scene);check(game.character().at("skills").at(std::to_string(learned))==1&&game.character().at("skill_points")==pointsAfter,"Skill state lost across disconnect");check(game.character().at("fletta_paths")==expectedPaths&&game.character().at("fletta")==4999,"Fletta paths lost across disconnect");game.enter(send);
  check(!packets.empty()&&packets[0].cat==2&&packets[0].sub==5,"Native entry packet missing");auto&entry=packets[0].body;check(get32(entry,QuickSlots::entry_offset+35*12)==learned&&get32(entry,QuickSlots::entry_offset+34*12)==50001,"Entry replayed captured bar instead of saved one");ready(game);bool list=false,paths=false;for(auto&p:packets){if(p.cat==5&&p.sub==5)for(int i=0;i<p.body[5];i++)if(get32(p.body,6+i*4)==learned)list=true;if(p.cat==8&&p.sub==4&&p.body==allocation)paths=true;}check(list&&paths,"Reconnect did not send learned list and Fletta paths");
 }
 std::cout<<"PASS: seven class seeds; learned skills/Fletta saved and restored; duplicate requests and failed checkpoint rollback; per-character skill/potion quickslots; native entry bar; full-bar save; forged identity rejection. Not installed.\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
