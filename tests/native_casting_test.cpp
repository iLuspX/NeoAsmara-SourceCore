#include "native/game.hpp"
#include <iostream>
#include <cmath>
using namespace rosh;
static void check(bool ok,const char*message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F f,const char*message){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,message);}
struct Packet{int cat,sub;Bytes body;};
int wmain(int argc,wchar_t**argv){try{
 check(argc==2,"Use isolated fixture");fs::path root=argv[1];check(fs::is_regular_file(root/"GM_TEST_FIXTURE"),"Refusing live databases");Content content(root);SkillBook learned(content);ActiveSkills casting(content);auto catalog=content.get("data/skill_catalog.json");
 auto world=std::make_shared<WorldScene>(content);auto entities=world->map("berneo");const Entity*chosen=nullptr;for(auto&[id,mob]:*entities)if(mob.combat&&mob.alive&&mob.category=="normal"&&mob.level<=15&&(!chosen||mob.maxhp<chosen->maxhp))chosen=&mob;check(chosen,"No skill target");auto mob=*chosen;
 constexpr std::uint32_t eid=87001;Json character={{"eid",eid},{"name","CastFixture"},{"cls",220},{"level",115},{"exp",Progression().threshold(115)},{"hp",100},{"maxhp",1000},{"mp",100000},{"maxmp",100000},{"skills",Json::object()},{"pos",{mob.position.x,mob.position.y,mob.position.z}},{"fletta",0},{"fletta_energy",0},{"moral_value",0},{"skill_points",0}};
 Json stats={{"attack",20.},{"magic_attack",40.},{"cast_speed",1.},{"cooldown_reduction",0.}};
 auto request=[&](int id,int kind){Bytes b(36);put32(b,1,eid,true);put32(b,5,id,true);put32(b,9,1,true);for(int i=0;i<3;i++)putfloat(b,13+i*4,character["pos"][i]);b[29]=(kind==11||kind==13)?1:0;put32(b,30,b[29]?mob.eid:eid,true);put16(b,34,0x24,true);return b;};
 int contracts=0;
 for(int id:{3001,3005,3904,3903,3902,3907,3002,4503,4502,4803,3901,4004}){
  int cls=0;for(auto&t:catalog.at("trees").items())for(auto&skill:t.value())if(skill==id)cls=std::stoi(t.key());if(!cls&&(id==4004||id==4803))cls=200;check(cls!=0,"Captured skill absent from trees");auto ch=character;ch["cls"]=cls;ch["skills"][std::to_string(id)]=1;auto row=catalog.at("skills").at(std::to_string(id));int kind=row.at("kind");auto b=request(id,kind);auto cost=std::ceil(row.at("levels").at("1").at("mana").get<double>());auto result=casting.admit(ch,stats,*entities,learned,b,1000);
  check(ch.at("mp").get<double>()==100000-cost&&result.at("id")==id,"Captured skill mana/admission differs");check(ch["skill_cooldowns"][std::to_string(id)].get<double>()>1000,"Cooldown absent");rejects([&]{casting.admit(ch,stats,*entities,learned,b,1000.1);},"Cooldown bypass accepted");
  if(kind==23||kind==25)check(ch.at("active_buffs").contains(std::to_string(id))&&result.at("effects")==casting.effects(id,1),"Native aura/self buff missing");
  Bytes impact(32);put32(impact,1,eid,true);impact[5]=1;put32(impact,6,id,true);impact[26]=1;impact[27]=(kind==21||kind==23||kind==25)?0:1;put32(impact,28,result.at("target"),true);check(casting.impact(impact,eid,1001).at("id")==id,"Authorized impact rejected");for(int extra=1;extra<row.at("levels").at("1").value("hits",id==3901?2:1);extra++)casting.impact(impact,eid,1001);rejects([&]{casting.impact(impact,eid,1001);},"Repeated impact accepted");++contracts;
 }
 auto ch=character;ch["skills"]["3001"]=1;auto b=request(3001,11);auto forged=b;put32(forged,1,eid+1,true);rejects([&]{casting.admit(ch,stats,*entities,learned,forged,2000);},"Forged cast accepted");ch["mp"]=0;rejects([&]{casting.admit(ch,stats,*entities,learned,b,2000);},"No-mana cast accepted");ch["mp"]=1000;casting.admit(ch,stats,*entities,learned,b,2000);Bytes impact(32);put32(impact,1,eid,true);impact[5]=1;put32(impact,6,3001,true);impact[26]=1;impact[27]=1;put32(impact,28,mob.eid,true);rejects([&]{casting.impact(impact,eid,2007);},"Expired cast accepted");casting.cancel();rejects([&]{casting.impact(impact,eid,2001);},"Cancelled cast accepted");
 // Exercise real session dispatch, durable mana/cooldown/buff state and replay ledger.
 LegacyStore store(root/"Database/karos_gameplay.sqlite");auto account="cast-"+token(8);character["skills"]={{"3001",1},{"3902",1},{"3903",1}};store.db().exec("INSERT INTO accounts VALUES(?,?)",{account,Json{{"chars",Json::array({character})}}.dump()});store.db().exec("CREATE TABLE IF NOT EXISTS character_locations(eid INTEGER PRIMARY KEY,map_name TEXT NOT NULL,x REAL NOT NULL,y REAL NOT NULL,z REAL NOT NULL,angle REAL NOT NULL,updated_at REAL NOT NULL)");store.db().exec("INSERT INTO character_locations VALUES(?,?,?,?,?,?,?)",{std::int64_t(eid),std::string("berneo"),mob.position.x,mob.position.y,mob.position.z,0.,now()});
 Session identity;identity.id=token(20);identity.identity.id=account;identity.channel=1;identity.expires=now()+600;std::vector<Packet>packets;auto send=[&](unsigned char c,unsigned char s,const Bytes&body){packets.push_back({c,s,body});};
 {WorldSession game(content,store,identity,root,eid,world);Bytes ready(21);put32(ready,1,eid,true);for(int i=0;i<3;i++)putfloat(ready,5+i*4,character["pos"][i]);game.handle(4,0,ready,send);packets.clear();auto mana=game.character().at("mp").get<int>();game.handle(5,3,b,send,"cast-once");check(game.character().at("mp")==mana-6,"Live cast failed to spend native mana");game.handle(5,3,b,send,"cast-once");check(game.character().at("mp")==mana-6,"Replayed cast charged mana twice");
  game.handle(5,1,impact,send,"impact-once");bool hit=false;for(auto&p:packets)if(p.cat==5&&p.sub==2){check(p.body[5]==1&&get32(p.body,6,true)==3001&&get32(p.body,11,true)==11&&get32(p.body,21)>0,"Skill damage packet is malformed");hit=true;}check(hit,"Live skill impact never applied");auto count=packets.size();game.handle(5,1,impact,send,"impact-replay");for(std::size_t i=count;i<packets.size();i++)check(!(packets[i].cat==5&&packets[i].sub==2),"Replayed skill impact dealt damage");
  Bytes animation(49);put32(animation,1,eid,true);animation[5]=5;animation[48]=36;auto start=packets.size();game.handle(5,0,animation,send,"attack-finish");check(packets.size()>start&&packets.back().cat==5&&packets.back().sub==0,"Native attack ending mode5 rejected");
  Sleep(550);auto buff=request(3902,23);put32(buff,30,0,true);game.handle(5,3,buff,send,"self-buff");check(game.character().at("active_buffs").contains("3902"),"Self buff was not applied");
  animation[5]=1;put32(animation,27,eid,true);start=packets.size();game.handle(5,0,animation,send,"buff-animation");check(packets.size()>start&&packets.back().cat==5&&packets.back().sub==0,"Authorized self animation rejected");
  auto self=impact;put32(self,6,3902,true);self[27]=0;put32(self,28,eid,true);start=packets.size();game.handle(5,1,self,send,"self-buff-impact");bool selfAck=false;for(std::size_t i=start;i<packets.size();i++)if(packets[i].cat==5&&packets[i].sub==2){auto&wire=packets[i].body;check(wire[15]==0&&get32(wire,16)==eid&&get32(wire,21)==0&&get32(wire,11,true)==0xffffffff,"Self effect completion packet malformed");selfAck=true;}check(selfAck,"Self effect impact rejected as a mob attack");
  const std::uint32_t friendEid=87002;
  auto ally=character;ally["eid"]=friendEid;ally["name"]="AuraFixtureAlly";ally["cls"]=100;ally["skills"]=Json::object();
  auto friendIdentity=identity;friendIdentity.id=token(20);friendIdentity.identity.id="aura-ally-"+token(8);
  store.db().exec("INSERT INTO accounts VALUES(?,?)",{friendIdentity.identity.id,Json{{"chars",Json::array({ally})}}.dump()});
  store.db().exec("INSERT INTO character_locations VALUES(?,?,?,?,?,?,?)",{std::int64_t(friendEid),std::string("berneo"),mob.position.x,mob.position.y,mob.position.z,0.,now()});
  double allyDefense=0;
  auto allySend=[&](unsigned char cat,unsigned char sub,const Bytes& packet){
   if(cat==8&&sub==2)allyDefense=getfloat(packet,96);
  };
  WorldSession recipient(content,store,friendIdentity,root,friendEid,world);
  auto allyReady=ready;put32(allyReady,1,friendEid,true);recipient.handle(4,0,allyReady,allySend);
  const auto baseDefense=allyDefense;
  Sleep(550);
  auto aura=request(3903,25);game.handle(5,3,aura,send,"shelter-aura");
  check(game.character().at("active_buffs").contains("3903"),"Shelter aura failed to activate");
  Sleep(120);recipient.admin_tick(allySend);
  check(allyDefense==baseDefense,"Solo player received party aura");
  const auto casterKey=identity.id+":"+std::to_string(eid);
  const auto allyKey=friendIdentity.id+":"+std::to_string(friendEid);
  world->party(casterKey,{"invite","AuraFixtureAlly"});
  world->party(allyKey,{"accept"});
  Sleep(120);recipient.admin_tick(allySend);
  check(allyDefense>baseDefense,"Party member did not receive physical protection");
  world->party(casterKey,{"leave"});
  Sleep(120);recipient.admin_tick(allySend);
  check(allyDefense==baseDefense,"Aura remained after the caster left the party");
  check(store.character(friendIdentity.identity.id,friendEid).value("active_buffs",Json::object()).empty(),
        "Borrowed aura was saved as an owned effect");
  auto saved=store.character(account,eid);check(saved.at("mp")==game.character().at("mp")&&saved.at("skill_cooldowns")==game.character().at("skill_cooldowns")&&saved.at("active_buffs")==game.character().at("active_buffs"),"Skill state not saved");
 }
 {WorldSession game(content,store,identity,root,eid,world);check(game.character().at("skill_cooldowns").contains("3001"),"Reconnect erased cooldown");}
 // Target control uses the same authoritative debuff channel consumed by mob AI.
 WorldScene controls(content);auto current=controls.map("berneo")->at(mob.eid);Bytes appearance(260),movement(34);Json combat={{"hp",1000},{"level",current.level},{"attack",1.},{"critical",0.}};controls.update_player("control",eid,"berneo",character["pos"],appearance,movement,false,send,combat);
 Json control={{"id",4004},{"rank",1},{"duration",7.},{"effects",casting.effects(4004,1)}};controls.skill_effects("control",mob.eid,control,5000);check(controls.map("berneo")->at(mob.eid).debuffs.at("4004").at("expires")==5007.,"Target control effects missing");
 check(!learned.allowed(220,4803)&&learned.cast_allowed(220,4803)&&!learned.cast_allowed(400,4803),"Supplemental skill leaked into ordinary learning or another class");
 auto area=character;area["skills"]={{"3907",1},{"4004",1}};auto ground=request(3907,11);ground[29]=0;put32(ground,30,0,true);casting.admit(area,stats,*entities,learned,ground,6000);auto before=controls.map("berneo");controls.summon("berneo",mob.npc_id,2,mob.position);auto after=controls.map("berneo");Json targets=Json::array();for(auto&[id,m]:*after)if(!before->count(id))targets.push_back(id);check(targets.size()==2,"Area target fixture missing");
 Bytes multi(37);put32(multi,1,eid,true);multi[5]=1;put32(multi,6,3907,true);multi[26]=2;for(int i=0;i<2;i++){multi[27+i*5]=1;put32(multi,28+i*5,targets[i],true);}auto profile=casting.impact(multi,eid,6001);check(profile.at("targets")==targets,"Area target list truncated");auto start=packets.size();
 for(auto&t:profile.at("targets")){auto single=multi;single.resize(32);single[26]=1;put32(single,28,t,true);auto perTarget=profile;perTarget["target"]=t;controls.strike("control",1,single,6001,[](Entity&){},perTarget);}int areaHits=0;for(std::size_t i=start;i<packets.size();i++)if(packets[i].cat==5&&packets[i].sub==2)areaHits++;check(areaHits==2,"Area impact did not affect both valid mobs");rejects([&]{casting.impact(multi,eid,6001);},"Replayed area dealt damage");
 ground=request(4004,13);ground[29]=0;put32(ground,30,eid,true);casting.admit(area,stats,*entities,learned,ground,6010);put32(multi,6,4004,true);put32(multi,33,targets[0],true);rejects([&]{casting.impact(multi,eid,6011);},"Duplicate area target accepted");
 if(catalog.at("skills").at("3901").contains("prerequisites")){
  auto trainee=character;trainee["skills"]=Json::object();trainee["skill_points"]=10;Bytes learn(10);put32(learn,0,eid,true);put32(learn,4,3901,true);put16(learn,8,36,true);rejects([&]{learned.learn(trainee,learn,false);},"Official prerequisite ignored");check(trainee.at("skill_points")==10,"Rejected prerequisite spent points");trainee["skills"]["3002"]=1;learned.learn(trainee,learn,false);check(trainee.at("skills").at("3901")==1,"Satisfied prerequisite refused");
  check(catalog.at("skills").at("3907").at("levels").at("1").at("magic_min")==412.,"Official magic damage channel ignored");
 }
 std::cout<<"PASS: "<<contracts<<" captured skill IDs; mana/rank/class/cooldown/target checks; self/aura effects; native multi-hit counts and expiry; live skill damage, replay protection, durable mana/cooldown and reconnect; target debuffs; area target validation and all admitted targets; official prerequisite and damage-channel checks\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
