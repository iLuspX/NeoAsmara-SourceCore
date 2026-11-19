#include "native/game.hpp"
#include "native/gm.hpp"
#include <iostream>
using namespace rosh;
static void check(bool result,const char* message){if(!result)throw std::runtime_error(message);}
int wmain(int argc,wchar_t**argv){try{
 check(argc==3||argc==5,"Use isolated fixture: <root> <mail id> [recipient account and character ID]");
 fs::path root=argv[1];check(fs::exists(root/"GM_TEST_FIXTURE"),"Refusing live database");
 Content content(root);LegacyStore store(root/"Database/karos_gameplay.sqlite");auto &db=store.db();gm_schema(db);
 auto scene=std::make_shared<WorldScene>(content);auto population=scene->map("berneo");
 auto npc=std::find_if(population->begin(),population->end(),[](const auto &row){return row.second.npc_id==9300281&&row.second.alive;});
 check(npc!=population->end(),"Berneo mail NPC not found");auto position=npc->second.position;
 const auto eid=argc==5?static_cast<std::uint32_t>(std::stoul(argv[4])):50002u;
 const auto account=argc==5?utf8(argv[3]):std::string("gmfixture1");
 auto stamp=now();
 const std::wstring wideId=argv[2]; const std::string id(wideId.begin(),wideId.end());
 auto queued=db.query("SELECT attachments FROM gm_mail WHERE id=? AND target_eid=? AND status='pending'",{id,std::int64_t(eid)});
 check(queued.size()==1,"GM API mail is not queued for NPC retrieval");
 const auto attachment=Json::parse(queued[0].at("attachments").get<std::string>()).at(0);
 db.exec("CREATE TABLE IF NOT EXISTS character_locations(eid INTEGER PRIMARY KEY,map_name TEXT NOT NULL,x REAL NOT NULL,y REAL NOT NULL,z REAL NOT NULL,angle REAL NOT NULL,updated_at REAL NOT NULL)");
 db.exec("INSERT OR REPLACE INTO character_locations VALUES(?,?,?,?,?,?,?)",{std::int64_t(eid),std::string("berneo"),position.x,position.y,position.z,0.,stamp});
 store.update_character(account,eid,[](Json &character){character["hp"]=100;character["mp"]=100;character["map_id"]=19003;});
 Session session;session.id=token(20);session.identity.id=account;session.channel=1;session.expires=stamp+600;
 WorldSession game(content,store,session,root,eid,scene);
 struct Packet {int cat,sub;Bytes bytes;};std::vector<Packet> packets;
 auto send=[&](unsigned char cat,unsigned char sub,const Bytes &bytes){packets.push_back({cat,sub,bytes});};
 auto get=[&](int cat,int sub){auto i=std::find_if(packets.begin(),packets.end(),[&](const Packet &p){return p.cat==cat&&p.sub==sub;});if(i==packets.end())throw std::runtime_error("Packet absent "+std::to_string(cat)+"/"+std::to_string(sub));return i->bytes;};
 Bytes ready(21);put32(ready,1,eid,true);for(int i=0;i<3;i++)putfloat(ready,5+i*4,game.character()["pos"][i]);game.handle(4,0,ready,send);
 check(game.ready(),"Fixture character did not load");
 packets.clear();Bytes open(16);put32(open,12,npc->first,true);game.handle(16,2,open,send);
 auto list=get(16,2);check(get32(list,8,true)==1&&list.size()>24,"NPC did not open queued mail");
 auto read=Bytes(list.begin()+16,list.begin()+24);read.resize(10);put16(read,8,2,true);
 packets.clear();game.handle(16,1,read,send);auto detail=get(16,1);check(get16(detail,detail.size()-8*147-6,true)==1&&get32(detail,detail.size()-8*147+2)==attachment.at("id"),"Mail attachment preview missing");
 Bytes deletion(4);put32(deletion,0,1,true);deletion.insert(deletion.end(),read.begin(),read.end());
 packets.clear();game.handle(16,3,deletion,send);
 check(get(16,3)==Bytes(4),"Mailbox deleted an unclaimed GM attachment");
 auto claim=read;claim.resize(18);claim[9]=0;put16(claim,16,2,true);
 packets.clear();game.handle(16,5,claim,send);check(get(16,8)==read,"Wrong claim acknowledgement");auto after=get(16,1);check(get16(after,after.size()-8*147-6,true)==0,"Claimed attachment still visible");
 check(db.query("SELECT uid FROM gm_item_origins WHERE action_id=?",{id}).size()==1,"Claim did not register origin");
 packets.clear();game.handle(16,5,claim,send);check(get(16,8)==read,"Claim replay acknowledgement missing");
 check(db.query("SELECT uid FROM gm_item_origins WHERE action_id=?",{id}).size()==1,"Replay granted another item");
 packets.clear();game.handle(16,3,deletion,send);
 check(get(16,3)==deletion,"Mailbox did not acknowledge deletion after claiming the attachment");
 packets.clear();game.handle(16,2,open,send);
 check(get32(get(16,2),8,true)==0,"Deleted letter was still listed");
 check(db.query("SELECT uid FROM gm_item_origins WHERE action_id=?",{id}).size()==1,"Deletion removed item origin audit");
 for(int i=0;i<13;++i)
  db.exec("INSERT INTO gm_mail VALUES(?,?,?,?,?,'[]','pending',?)",
          {std::string("wire-capacity-")+std::to_string(i),std::int64_t(eid),std::string("System"),
           std::string("Queued letter"),std::string("Capacity fixture"),stamp+i});
 packets.clear();game.handle(16,2,open,send);
 auto firstPage=get(16,2);
 check(get32(firstPage,8,true)==12,"Mailbox exceeded client list capacity");
 Bytes clearFirst(4);put32(clearFirst,0,1,true);
 clearFirst.insert(clearFirst.end(),firstPage.begin()+16,firstPage.begin()+24);
 clearFirst.resize(14);put16(clearFirst,12,2,true);
 packets.clear();game.handle(16,3,clearFirst,send);
 check(get(16,3)==clearFirst&&get32(get(16,2),8,true)==12,"Queued letter did not fill newly freed slot");
 check(db.query("SELECT COUNT(*) AS n FROM gm_mail WHERE id LIKE 'wire-capacity-%' AND status='pending'")[0].at("n")==12,
       "Mailbox lost an overflow letter");
 std::cout<<"PASS: live WorldSession path for native NPC list/read/claim, inventory origin and replay.\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}

