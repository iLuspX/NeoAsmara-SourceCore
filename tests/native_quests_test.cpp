#include "native/game.hpp"
#include <iostream>
#include <ctime>
using namespace rosh;
static void check(bool ok,const char*text){if(!ok)throw std::runtime_error(text);}
static Bytes request(std::uint32_t eid,int qid,int actor,int sub){Bytes b(sub==0?14:sub==1?18:22);put32(b,0,eid,true);put32(b,4,qid,true);put32(b,8,actor,true);if(sub==1)put32(b,12,0xffffffff,true);put16(b,b.size()-2,0x51,true);return b;}
static double local(int day,int hour,int minute=0){std::tm t{};t.tm_year=126;t.tm_mon=9;t.tm_mday=day;t.tm_hour=hour;t.tm_min=minute;t.tm_isdst=-1;return static_cast<double>(std::mktime(&t));}
int wmain(int argc,wchar_t**argv){try{
 check(argc==3,"Use isolated fixture and Python parity data");fs::path root=argv[1];check(fs::is_regular_file(root/"GM_TEST_FIXTURE"),"Refusing live database");Content content(root);
 {auto expected=Json::parse(read_file(argv[2]));Sqlite parity(":memory:");Inventory bag(parity,content);QuestBook native(parity,bag,content,85001);
  for(auto&r:expected.at("states"))parity.exec("INSERT INTO quest_state VALUES(?,?,?,?)",{r[0].get<std::int64_t>(),r[1].get<std::int64_t>(),r[2].get<std::int64_t>(),r[3].get<std::int64_t>()});
  for(auto&r:expected.at("objectives"))parity.exec("INSERT INTO quest_objectives VALUES(?,?,?,?)",{r[0].get<std::int64_t>(),r[1].get<std::int64_t>(),r[2].get<std::int64_t>(),r[3].get<std::int64_t>()});
  for(auto&r:expected.at("history"))parity.exec("INSERT INTO quest_completions VALUES(?,?,?,?,?)",{r[0].get<std::int64_t>(),r[1].get<std::int64_t>(),r[2].get<std::int64_t>(),r[3].get<double>(),r[4].get<std::string>()});
  check(native.list_packet()==unhex(expected.at("list")),"Native quest list differs from Python bytes");check(native.completed_packet(expected.at("timestamp"))==unhex(expected.at("completed")),"Native quest history differs from Python bytes");check(native.ui_state(120,true,expected.at("timestamp"))==expected.at("ui"),"Grouped quest HUD/availability differs from Python");
 }
 Sqlite db(":memory:");Inventory inventory(db,content);GroundLoot ground(db,content,inventory);QuestBook book(db,inventory,content,85001),other(db,inventory,content,85002);
 auto catalog=content.get("data/quest_catalog.json");SceneEntities npcs;std::map<int,int>npcActor;int next=10;
 for(auto q=catalog.begin();q!=catalog.end();++q){if(!q->value("supported",false))continue;std::vector<int>types={q->at("start_npc"),q->at("end_npc")};for(auto&o:q->at("objectives"))if(o.at("kind")==8)types.push_back(o.at("target"));for(int type:types)if(type&&!npcActor.count(type)){Entity e;e.eid=next++;e.npc_id=type;e.position=e.home={1000,1000,50};e.alive=true;npcActor[type]=e.eid;npcs[e.eid]=e;}}
 Json ch={{"eid",85001},{"hp",100},{"level",120},{"pos",{1000,1000,50}}};
 auto accept=[&](int id,const std::string&event,double stamp){return book.accept(request(85001,id,npcActor.at(catalog.at(std::to_string(id)).at("start_npc")),0),ch,npcs,event,stamp);};
 auto talk=[&](int id){for(auto&o:catalog.at(std::to_string(id)).at("objectives"))if(o.at("kind")==8){auto b=request(85001,id,8,3);put32(b,12,o.at("target"),true);put32(b,16,0,true);check(!book.talk(b,ch,npcs),"Zero talk probe advanced objective");put32(b,16,999,true);book.talk(b,ch,npcs);}};
 auto finish=[&](int id,const std::string&event,double stamp){return book.complete(request(85001,id,npcActor.at(catalog.at(std::to_string(id)).at("end_npc")),1),ch,npcs,event,stamp);};
 double stamp=local(26,3,59);check(QuestBook::local_window(stamp,4)=="2026-10-25"&&QuestBook::local_window(local(26,4),4)=="2026-10-26","Daily boundary does not follow host-local04:00");
 check(accept(99990210,"daily-accept",stamp),"Daily quest refused");check(!accept(99990210,"daily-accept",stamp),"Accept replay duplicated quest");check(!other.active(99990210),"Quest shared between characters");
 auto list=book.list_packet();check(get16(list,4,true)==1&&list.size()==6+0x414&&get32(list,6)==99990210,"Wrong native quest-list layout");
 bool rejected=false;try{finish(99990210,"daily-incomplete",stamp);}catch(...){rejected=true;}check(rejected&&!book.ever_completed(99990210),"Incomplete quest paid reward");
 talk(99990210);auto state=book.ui_state(120,true,stamp);check(state.at("active")[0].at("ready")==true&&state.at("active")[0].at("goals")[0]==1,"Talk goal/HUD readiness missing");
 auto reward=finish(99990210,"daily-finish",stamp);check(!reward.at("replay").get<bool>()&&!book.active(99990210),"Completed quest remains active");
 auto count=[&](int item){std::int64_t n=0;for(auto&i:inventory.items(85001))if(i.item==static_cast<unsigned>(item))n+=i.quantity;return n;};check(count(64003)==3,"Native quest item reward missing");
 check(finish(99990210,"daily-finish",stamp).at("replay")==true&&finish(99990210,"daily-new-wire-replay",stamp).at("replay")==true&&count(64003)==3,"Turn-in replay duplicated reward");
 check(get16(book.list_packet(),4,true)==0&&book.ui_state(120,true,stamp).at("active").empty(),"Completed quest remains in native list or HUD");auto history=book.completed_packet(stamp);check(get16(history,4,true)==0&&get16(history,6,true)==1&&get32(history,8)==99990210,"Completed IDs sent as ready active quests");
 rejected=false;try{accept(99990210,"daily-blocked",stamp);}catch(...){rejected=true;}check(rejected,"Daily quest repeated before reset");check(accept(99990210,"daily-new-window",local(26,4)),"Daily quest did not unlock at04:00");talk(99990210);finish(99990210,"daily-second-cycle",local(26,4));check(count(64003)==6,"Second daily cycle lost reward");
 db.exec("INSERT INTO quest_cycles VALUES(85001,9000001,0)");db.exec("INSERT INTO quest_completions VALUES(85001,9000001,0,?,?)",{stamp,std::string("2026-10-25")});
 check(accept(9000002,"unique-accept",stamp),"Prerequisite completion lost");talk(9000002);check(finish(9000002,"unique-finish",stamp).at("xp")==120601,"Native quest XP reward changed");rejected=false;try{accept(9000002,"unique-again",local(27,4));}catch(...){rejected=true;}check(rejected,"Unique quest can be accepted again");
 int repeat=0,killquest=0,collection=0;
 for(auto q=catalog.begin();q!=catalog.end();++q)if(q->value("supported",false)&&q->at("min_level").get<int>()<=120&&q->at("max_level").get<int>()>=120&&q->at("prerequisite")==0){int id=std::stoi(q.key());if(q->at("native_type")==3&&!repeat)repeat=id;if(!killquest&&q->at("objectives").size()==1&&q->at("objectives")[0].at("kind")==1)killquest=id;if(!collection&&q->at("objectives").size()==1&&q->at("objectives")[0].at("kind")==3)collection=id;}
 check(repeat&&killquest&&collection,"Native quest coverage fixture unavailable");
 check(accept(killquest,"kill-accept",stamp),"Kill quest refused");auto killGoal=catalog.at(std::to_string(killquest)).at("objectives")[0];auto forged=request(85001,killquest,1,3);put32(forged,12,killGoal.at("target"),true);put32(forged,16,9999,true);check(!book.talk(forged,ch,npcs),"Client claimed fake kills");
 int need=killGoal.at("required");for(int i=0;i<need+3;i++)book.kill(killGoal.at("target"));auto progress=db.query("SELECT value FROM quest_objectives WHERE player_eid=85001 AND quest_id=?",{std::int64_t(killquest)});check(progress[0].at("value")==need,"Server kills missing or exceed goal cap");
 check(accept(collection,"collect-accept",stamp),"Collection quest refused");auto collect=catalog.at(std::to_string(collection)).at("objectives")[0];inventory.grant_once(85001,{{collect.at("target"),collect.at("required")}},"collect-items");book.synchronize(stamp);check(db.query("SELECT value FROM quest_objectives WHERE player_eid=85001 AND quest_id=?",{std::int64_t(collection)})[0].at("value")==collect.at("required"),"Owned collection items not reflected");
 check(book.abandon(collection)&&!book.abandon(collection),"Abandon is not idempotent");check(!book.active(collection)&&!book.ever_completed(collection),"Abandon falsely completed quest");
 // Transaction failure must restore objective consumption and reward markers.
 auto q=catalog.at(std::to_string(repeat));ch["level"]=120;check(accept(repeat,"repeat-accept",stamp),"Repeatable quest refused");
 for(auto&o:q.at("objectives")){int kind=o.at("kind");if(kind==1)for(int i=0;i<o.at("required").get<int>();i++)book.kill(o.at("target"));else if(kind==3)inventory.grant_once(85001,{{o.at("target"),o.at("required")}},"repeat-collect-"+std::to_string(o.at("target").get<int>()));}
 talk(repeat);book.synchronize(stamp);db.exec("CREATE TRIGGER quest_fail BEFORE INSERT ON native_quest_rewards BEGIN SELECT RAISE(ABORT,'fixture reward failure'); END");rejected=false;try{finish(repeat,"repeat-failed",stamp);}catch(...){rejected=true;}check(rejected&&book.active(repeat),"Failed reward completed quest");db.exec("DROP TRIGGER quest_fail");finish(repeat,"repeat-success",stamp);check(accept(repeat,"repeat-second",stamp),"Repeatable quest did not unlock immediately");
 // Native profession chain: letters (kind9) are supplied by the issuing NPC,
 // ordinary mission drops remain conditional and keep their native 3000/10000 rate.
 QuestBook advancement(db,inventory,content,85003);Json adv={{"eid",85003},{"hp",100},{"level",45},{"pos",{1000,1000,50}}};db.exec("INSERT INTO quest_state VALUES(85003,19005056,0,1)");
 auto takeAdv=[&](int id){return advancement.accept(request(85003,id,npcActor.at(catalog.at(std::to_string(id)).at("start_npc")),0),adv,npcs,"advance-accept-"+std::to_string(id),stamp);};
 auto endAdv=[&](int id){return advancement.complete(request(85003,id,npcActor.at(catalog.at(std::to_string(id)).at("end_npc")),1),adv,npcs,"advance-finish-"+std::to_string(id),stamp);};
 auto countAdv=[&](int id){std::int64_t count=0;for(auto&i:inventory.items(85003))if(i.item==static_cast<unsigned>(id))count+=i.quantity;return count;};
 check(takeAdv(19005057)&&countAdv(70215)==1,"Recommendation letter missing on quest acceptance");check(!takeAdv(19005057)&&countAdv(70215)==1,"Repeated acceptance duplicates letter");endAdv(19005057);check(countAdv(70215)==0,"Delivered letter not consumed");
 check(advancement.drops(5090744,stamp).empty(),"Profession item drops before mission is active");check(takeAdv(19005058),"Profession collection quest remains blocked");check(advancement.drops(1001801,stamp).empty(),"Profession item drops from wrong species");int trials=0;for(int i=0;i<1000;i++){for(auto&drop:advancement.drops(5090744,stamp)){check(drop.first==70216,"Profession mob dropped wrong objective");trials++;}}check(trials>150&&trials<450,"Native 30 percent profession drop rate changed");
 auto delivery=advancement.route_kill_items({{70216,1},{54001,1}},"advance-fixture-drop");
 check(delivery.ground.size()==1&&delivery.ground[0].first==54001&&delivery.delivered.size()==1&&countAdv(70216)==1,
       "Quest-only kill reward entered ground loot or normal item was redirected");
 check(advancement.route_kill_items({{70216,1}},"advance-fixture-drop").delivered.empty()&&countAdv(70216)==1,
       "Quest kill delivery replay duplicated the item");
 inventory.grant_once(85003,{{70216,9},{70217,5},{70218,5}},"advance-fixture-goals");check(advancement.drops(5090744,stamp).empty(),"Profession objective continues dropping after required count");endAdv(19005058);check(takeAdv(19005059)&&countAdv(70214)==1,"Completion letter missing");endAdv(19005059);check(countAdv(70214)==0&&countAdv(70211)==1,"Profession chain certificate or letter consumption missing");
 int supported=0;for(auto q=catalog.begin();q!=catalog.end();++q)if(q->value("supported",false))supported++;check(supported==705,"Native quest catalog changed without audit");
 std::cout<<"PASS:705 native quest definitions; grouped objectives; authoritative talk/kill/collection; profession letters and conditional native-rate drops; native list/HUD/history; replay-safe rewards; rollback; prerequisites; unique/repeatable/daily04:00; character isolation and abandon\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
