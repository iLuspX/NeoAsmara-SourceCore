#include "native/titles.hpp"
#include "native/game.hpp"
#include "native/combat.hpp"
#include "native/inventory.hpp"
#include "native/runtime.hpp"
#include "native/control.hpp"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <sstream>
#include <regex>

namespace rosh {
namespace {
std::int64_t integer(const std::string& text,std::int64_t low,std::int64_t high){
 std::size_t used{};std::int64_t value;
 try{value=std::stoll(text,&used);}catch(...){throw std::runtime_error("Valor inteiro invalido");}
 if(used!=text.size()||value<low||value>high)throw std::runtime_error("Inteiro fora do intervalo permitido");return value;
}
double number(const std::string&text,double low,double high){
 std::size_t used{};double value;
 try{value=std::stod(text,&used);}catch(...){throw std::runtime_error("Valor numerico invalido");}
 if(used!=text.size()||!std::isfinite(value)||value<low||value>high)throw std::runtime_error("Numero fora do intervalo permitido");return value;
}
std::string joined(const std::vector<std::string>&args,std::size_t from){std::string result;for(std::size_t i=from;i<args.size();++i){if(i>from)result+=' ';result+=args[i];}return result;}
bool toggle(bool current,const std::vector<std::string>&args){
 auto action=args.size()==1?(current?"off":"on"):args[1];
 for(auto&c:action)c=std::tolower(static_cast<unsigned char>(c));
 if(action!="on"&&action!="off"&&action!="status")throw std::runtime_error("Use on, off ou status");return action=="status"?current:action=="on";
}
Json target_character(Sqlite&db,const std::string&query){
 for(auto&row:db.query("SELECT id,payload FROM accounts")){
  auto account=Json::parse(row.at("payload").get<std::string>());
  for(auto ch:account.value("chars",Json::array()))if(query==std::to_string(ch.at("eid").get<std::uint32_t>())||_stricmp(query.c_str(),ch.value("name","").c_str())==0){ch["account_id"]=row.at("id");return ch;}
 }throw std::runtime_error("Personagem nao encontrado");
}
int role_level(const std::string&role){static const std::map<std::string,int> levels={{"Player",0},{"Alpha",0},{"Beta",0},{"Helper",1},{"Ticket",1},{"Mod",2},{"GM",3},{"Admin",5},{"Dev",5}};auto found=levels.find(role);return found==levels.end()?0:found->second;}
}

void WorldSession::staff_command(const std::vector<std::string>&input,const std::string&event,const Sender&send){
 auto before=character_;auto persisted=store_.character(identity_.identity.id,eid());
 try{staff_command_impl(input,event,send);}catch(...){
  auto error=std::current_exception();
  try{if(store_.character(identity_.identity.id,eid())==persisted){character_=before;load_templates();recalculate();}}catch(...){}
  std::rethrow_exception(error);
 }
}
void WorldSession::staff_command_impl(const std::vector<std::string>&input,const std::string&event,const Sender&send){
 if(input.empty())return;
 auto args=input;args[0]=canonical_staff_command(args[0]);auto command=args[0];auto role=staff_->role();
 authorize_staff_command(role,command,args.size()-1);
 if(staff_command_policies().at(command).level>0&&role!="Helper"&&!staff_->valid())throw std::runtime_error("Use //stafflogin para autenticar o cargo");
 auto reply=[&](const std::string&text){send(3,0,staff_chat_reply(eid(),text));};
 if(command=="staffchat"){staff_channel_->post(role,identity_.identity.id,eid(),character_.at("name"),joined(args,1),event,now());poll_staff_channel(send);return;}
 auto vitals=[&]{recalculate();character_["hp"]=character_.at("maxhp");character_["mp"]=character_.at("maxmp");persist();send(8,2,stats_packet());send(8,0,gauge(0x0800));send(8,1,gauge(0x0801));publish_player(send);};
 if(command=="help"){reply("//party //damage //titles //titleuse ID|off //itemstats //pk [on|off|status] //cashshop; equipe: //staffhelp");return;}
 if(command=="cashshop"){
  if(character_.value("hp",0)<=0)throw std::runtime_error("Mantenha o personagem vivo para abrir a loja");
  quest_overlay({{"cmd","open"},{"page","shop"}},send);
  return;
 }
 if(command=="pk"){
  auto next=toggle(pk_mode_,args);
  if(next&&character_.value("level",1)<=35)throw std::runtime_error("Modo PK disponivel a partir do nivel 36");
  pk_mode_=next;
  publish_player(send);
  reply(pk_mode_?"Modo PK ativo.":"Modo PK desativado.");
  return;
 }
 if(command=="party"){auto lines=scene_->party(scene_key(),std::vector<std::string>(args.begin()+1,args.end()));refresh_party_auras(send);publish_player(send);for(auto&line:lines)reply(line);return;}
 if(command=="damage"){for(auto&line:scene_->damage_ranking(scene_key(),args.size()==2?args[1]:"solo"))reply(line);return;}
 if(command=="titles"||command=="titleuse"||command=="titlecatalog"||command=="titlegrant"||command=="titlerevoke"){
  auto catalog=content_.get("data/title_catalog.json").at("titles");auto&db=store_.db();
  initialize_titles(db);
  auto row_for=[&](int id){for(auto&row:catalog)if(row.at("id")==id)return row;throw std::runtime_error("Titulo inexistente");};
  if(command=="titles"){auto owned=db.query("SELECT title_id FROM character_titles WHERE account_id=? AND character_eid=? ORDER BY title_id",{identity_.identity.id,std::int64_t(eid())});if(owned.empty())reply("Nenhum titulo concedido. Titulos nao concedem permissoes ou atributos.");for(auto&row:owned){auto title=row_for(row.at("title_id"));reply(title.at("id").dump()+" - "+title.value("name",""));}return;}
  if(command=="titleuse"){
   if(args[1]=="off")db.exec("DELETE FROM character_title_selection WHERE character_eid=?",{std::int64_t(eid())});
   else{auto id=integer(args[1],1,0x7fffffff);if(db.query("SELECT title_id FROM character_titles WHERE account_id=? AND character_eid=? AND title_id=?",{identity_.identity.id,std::int64_t(eid()),id}).empty())throw std::runtime_error("Esse titulo nao foi concedido ao seu personagem");db.exec("INSERT INTO character_title_selection VALUES(?,?) ON CONFLICT(character_eid) DO UPDATE SET title_id=excluded.title_id",{std::int64_t(eid()),id});}reply("Titulo selecionado.");return;
  }
  if(role!="Admin"||!staff_->valid()||!title_owner(root_,identity_.identity.id))throw std::runtime_error("Somente o proprietario autenticado pode conceder ou revogar titulos");
  if(command=="titlecatalog"){for(auto&row:catalog)reply(row.at("id").dump()+" - "+row.value("name","")+" ["+row.value("rarity","")+"]");return;}
  auto target=target_character(db,args[1]);
  auto targetAccount=target.at("account_id").get<std::string>();
  auto targetId=target.at("eid").get<std::uint32_t>();
  int tid;
  if(args[2]=="cargo"){
   static const std::map<std::string,int> role_titles={{"Alpha",1101},{"Beta",1102},{"Helper",2001},{"Mod",2002},{"GM",2003},{"Admin",2004}};
   auto found=role_titles.find(character_role(*roles_,targetAccount,targetId));
   if(found==role_titles.end())throw std::runtime_error("O cargo atual nao tem titulo associado");
   tid=found->second;
  }else tid=static_cast<int>(integer(args[2],1,0x7fffffff));
  auto title=row_for(tid);auto reason=joined(args,3);if(wide(reason).size()>300||std::any_of(reason.begin(),reason.end(),[](unsigned char c){return c<32;}))throw std::runtime_error("Motivo invalido");
  change_title(db,event,identity_.identity.id,eid(),targetAccount,targetId,tid,command=="titlerevoke",reason);
  reply(std::string(command=="titlerevoke"?"Titulo revogado: ":"Titulo concedido: ")+title.value("name",""));return;
 }
 if(command=="itemtrace"){
  if(role!="Admin"||!title_owner(root_,identity_.identity.id))
   throw std::runtime_error("Somente o proprietario autenticado pode rastrear itens");
  auto uid=integer(args[1],1,0xffffffff);
  const auto page=args.size()==3?integer(args[2],1,10000):1;
  auto& db=store_.db();
  Transaction tx(db);
  auto origins=db.query(
   "WITH RECURSIVE chain(uid,depth) AS (SELECT ?,0 UNION ALL "
   "SELECT l.parent_uid,c.depth+1 FROM gm_item_lineage l JOIN chain c ON l.uid=c.uid WHERE c.depth<32) "
   "SELECT o.*,datetime(o.created,'unixepoch') AS utc FROM gm_item_origins o JOIN chain c ON o.uid=c.uid LIMIT 1",{uid});
  if(origins.empty()){tx.commit();reply("UID sem registro de origem no GM Tool.");return;}
  const auto& origin=origins[0];
  auto creator=origin.value("creator_name",std::string());
  auto recipient=origin.value("recipient_name",std::string());
  if(creator.empty())creator="EID "+origin.at("creator_eid").dump();
  if(recipient.empty())recipient="EID "+origin.at("recipient_eid").dump();
  auto holders=db.query(
   "SELECT player_eid,quantity,'inventario' AS location FROM inventory_slots WHERE instance_uid=? "
   "UNION ALL SELECT player_eid,1,'equipado' FROM equipment_instances WHERE instance_uid=?",{uid,uid});
  std::string current;
  if(holders.size()==1){
   const auto& holder=holders[0];
   auto id=holder.at("player_eid").dump();
   auto name="EID "+id;
   try{name=target_character(db,id).value("name",name);}catch(const std::exception&){}
   current="Atual: "+name+" | "+holder.at("location").get<std::string>()+
           " | quantidade "+holder.at("quantity").dump();
  }else if(holders.empty())current="UID sem localizacao atual no inventario ou equipamento.";
  else current="UID duplicado. Confira o banco antes de concluir a auditoria.";
  auto history=db.query(
   "SELECT *,datetime(occurred,'unixepoch') AS utc FROM gm_item_history "
   "WHERE uid=? ORDER BY sequence LIMIT 9 OFFSET ?",{uid,(page-1)*8});
  auto lineage=db.query("SELECT parent_uid,item_id FROM gm_item_lineage WHERE uid=?",{uid});
  auto children=db.query("SELECT uid FROM gm_item_lineage WHERE parent_uid=? ORDER BY uid LIMIT 8",{uid});
  tx.commit();
  reply("UID "+std::to_string(uid)+" | "+inventory_->name(lineage.empty()?origin.at("item_id"):lineage[0].at("item_id")));
  if(!lineage.empty())reply("Parent UID "+lineage[0].at("parent_uid").dump()+" | GM origin UID "+origin.at("uid").dump());
  if(!children.empty()){
   std::string line="Derived UIDs:";
   for(const auto& row:children)line+=" "+row.at("uid").dump();
   reply(line);
  }
  reply("Gerado por: "+creator+" | "+origin.at("utc").get<std::string>()+" UTC");
  reply("Entrega: "+recipient+" | "+std::string(origin.at("method")=="mail"?"correio GM":"direta GM")+
        " | quantidade "+origin.at("quantity").dump());
  reply(current);
  static const std::map<std::string,std::string> labels={
   {"move","Move"},{"sort","Sort"},{"equip","Equip"},{"unequip","Unequip"},{"expiry","Expiry"},
   {"equipment-expiry","Equipment expiry"},{"discard","Discard"},{"npc-sale","NPC sale"},{"use","Use"},
   {"open-box","Open box"},{"gacha-spin","Gacha spin"},{"quest-turn-in","Quest turn-in"},
   {"profession-certificate","Profession certificate"},{"travel-scroll","Travel scroll"},
   {"enhancement-scroll","Enhancement scroll"},{"consumed","Consumed"},{"derived","Derived"}
  };
  for(std::size_t i=0;i<std::min<std::size_t>(8,history.size());++i){
   const auto& row=history[i];
   auto operation=row.at("operation").get<std::string>();
   if(labels.count(operation))operation=labels.at(operation);
   reply(row.at("utc").get<std::string>()+" UTC | "+operation+" | EID "+row.at("holder_eid").dump()+
         " | slots "+row.at("source_slot").dump()+"/"+row.at("destination_slot").dump()+
         " | x"+row.at("quantity").dump());
  }
  if(history.empty())reply("Sem movimentacoes registradas nesta pagina.");
  if(history.size()>8)reply("Mais registros: //itemtrace "+std::to_string(uid)+" "+std::to_string(page+1));
  return;
 }
 if(command=="itemstats"){
  static const std::map<std::string,int> slots={{"arma",7},{"asas",17},{"torso",1},{"calca",2},{"luvas",3},{"botas",4},{"joias",-1}};auto query=args.size()==2?args[1]:"";for(auto&c:query)c=std::tolower(static_cast<unsigned char>(c));if(!query.empty()&&!slots.count(query))throw std::runtime_error("Use //itemstats arma|asas|torso|calca|luvas|botas|joias");auto rules=content_.get("data/native_item_rules.json");bool found=false;
  for(bool equipped:{true,false})for(auto&item:inventory_->items(eid(),equipped)){
   int slot=rules.value(std::to_string(item.item),Json::object()).value("slot",0);if(!query.empty()&&slot!=slots.at(query)&&!(query=="joias"&&(slot==21||slot==22||slot==24)))continue;
   auto affixes=store_.db().query("SELECT layer,stat,value FROM item_custom_affixes WHERE uid=? ORDER BY layer,position",{std::int64_t(item.uid)});auto fusion=store_.db().query("SELECT partner_item_id,partner_grade FROM item_pk_fusions WHERE uid=?",{std::int64_t(item.uid)});bool vampire=query=="joias"&&(item.item==50018||item.item==51018||item.item==52018);if(affixes.empty()&&fusion.empty()&&!vampire)continue;found=true;reply(inventory_->name(item.item)+(equipped?" [equipado]":" [inventario; inativo]"));for(auto&line:affixes){auto stat=line.at("stat").get<std::string>();bool flat=stat.find("_flat")!=std::string::npos||stat=="critical_multiplier";reply(line.value("layer","")+": "+stat+" +"+line.at("value").dump()+(flat?"":"%"));}for(auto&line:fusion)reply("Fusao local: "+inventory_->name(line.at("partner_item_id"))+" +"+line.at("partner_grade").dump());if(vampire)reply(std::string("Vampiro: +")+(item.item==50018?"2":"1")+"% de absorcao de HP ao equipar");
  }if(!found)reply("Nenhum item com estes atributos");return;
 }
 if(command=="gmhelp"){
  int page=args.size()==1?1:integer(args[1],1,10);
  static const std::map<int,std::string> pages={
   {1,"//where //stats [2] //inventory //cooldowns //item ID //finditem nome //maps [pagina]"},
   {2,"//heal //teleport x y z //map nome|ID //vanish [on|off|status] //npc [activate|status]"},
   {3,"//god [on|off|status] //movespeed 1..3 //attackspeed 1..5 //spawn ID [1..10] //respawn"},
   {4,"Admin: //give ID [quantidade] //carats [delta] //setlevel 1..120 //setexp total //fletta [quantidade]"},
   {5,"//profession ID //starter //forcesave //setrole nome Alpha|Beta|Helper|Mod|GM|Admin|Player"},
   {6,"//ban///mute nome character|account|hardware 30m|2h|7d|perm motivo //unban///unmute nome alvo"},
   {7,"//passwall [1..5000] //debug [on|off|status] //devinspect [EID] //addcash nome quantidade motivo"},
   {8,"//online [pagina] //goto nome //cashbalance nome //sanctions nome //kick nome motivo"},
   {9,"//announce mensagem //restart [segundos] //reset [segundos] //shutdown [segundos] //cancel"},
   {10,"GM: //mobinfo EID //findmob nome //bosstimers [pagina]"}};
  reply(pages.at(page));return;
 }
 if(command=="where"){reply(map_name_+": "+character_.at("pos").dump());return;}
 if(command=="stats"){
  reply(args.size()>1&&args[1]=="2"?"Atributos "+stats_.at("attributes").dump()+" DEF "+stats_.at("defense").dump()+" MDEF "+stats_.at("magic_defense").dump()+" Fletta "+character_.value("fletta",Json(0)).dump():"Lv "+character_.at("level").dump()+" HP "+character_.at("hp").dump()+"/"+character_.at("maxhp").dump()+" MP "+character_.at("mp").dump()+"/"+character_.at("maxmp").dump()+" ATK "+stats_.at("attack").dump()+" MATK "+stats_.at("magic_attack").dump());return;
 }
 if(command=="inventory"){
  std::string value;for(auto&item:inventory_->items(eid())){if(!value.empty())value+="; ";value+=std::to_string(item.slot)+":"+inventory_->name(item.item)+"x"+std::to_string(item.quantity);if(wide(value).size()>100)break;}reply(value.empty()?"Inventario vazio":value);return;
 }
 if(command=="cooldowns"){
  std::string value;auto cooldowns=character_.value("potion_cooldowns",Json::object());for(auto&item:cooldowns.items())if(item.value().is_number()&&item.value().get<double>()>now())value+=item.key()+":"+std::to_string(static_cast<int>(item.value().get<double>()-now()))+"s; ";reply(value.empty()?"Sem cooldowns de pocoes ativos":value);return;
 }
 if(command=="testdamage"){
  int level=args.size()>1?integer(args[1],1,120):1;auto damage=damage_range(stats_.at("attack"),level,character_.at("level"));reply("Alvo Lv "+std::to_string(level)+"; dano normal "+std::to_string(damage.first)+".."+std::to_string(damage.second));return;
 }
 if(command=="item"||command=="finditem"){
  auto catalog=content_.get("data/item_catalog_verified.json");auto query=joined(args,1);int matches=0;std::string value;
  for(auto&row:catalog){auto name=row.value("name","");bool found=command=="item"?row.at("id").get<std::uint32_t>()==static_cast<std::uint32_t>(integer(query,1,0xffffffffLL)):std::search(name.begin(),name.end(),query.begin(),query.end(),[](unsigned char a,unsigned char b){return std::tolower(a)==std::tolower(b);})!=name.end();if(found){if(matches)value+="; ";value+=std::to_string(row.at("id").get<std::uint32_t>())+" "+name;if(command=="item")value+=": "+row.value("description","");if(++matches==3||command=="item")break;}}
  reply(value.empty()?"Nenhum item verificado encontrado":value);return;
 }
 if(command=="npc"){
  auto action=args.size()==2?args[1]:"activate";for(auto&c:action)c=std::tolower(static_cast<unsigned char>(c));if(action!="activate"&&action!="status")throw std::runtime_error("Use //npc activate ou //npc status");int count=0;
  if(action=="activate"){scene_->recover_npcs(scene_map_name());scene_entities_=scene_->map(scene_map_name());}
  auto p=character_.at("pos");Position position{p[0],p[1],p[2]};for(auto&[id,npc]:*scene_entities_)if(!npc.combat){++count;if(action=="activate"&&npc.position.distance(position)<=3000){Bytes removed(5,1);put32(removed,1,id,true);send(4,3,removed);send(4,2,scene_->appearance(npc));scene_visible_.insert(id);}}reply("NPCs em "+map_name_+": "+std::to_string(count));return;
 }
 if(command=="vanish"){vanished_=toggle(vanished_,args);publish_player(send);reply(vanished_?"Vanish ativado":"Vanish desativado");return;}
 if(command=="god"||command=="debug"){
  auto key=command=="god"?"god_mode":"debug_mode";character_[key]=toggle(character_.value(key,false),args);publish_player(send);reply(command+": "+(character_[key].get<bool>()?"ON":"OFF"));return;
 }
 if(command=="devinspect"){
  if(args.size()==2){auto id=integer(args[1],1,0xffffffffLL);auto it=scene_entities_->find(static_cast<std::uint32_t>(id));if(it==scene_entities_->end())throw std::runtime_error("Entidade ausente no mapa");auto&e=it->second;reply("NPC "+std::to_string(e.npc_id)+"; HP "+std::to_string(e.hp)+"/"+std::to_string(e.maxhp)+"; estado "+std::to_string(static_cast<int>(e.state))+"; alvo "+std::to_string(e.target));}
  else{int mobs=0,npcs=0;for(auto&[id,e]:*scene_entities_)(e.combat?mobs:npcs)++;reply(map_name_+"; mobs="+std::to_string(mobs)+"; NPCs="+std::to_string(npcs)+"; pronto="+std::to_string(loaded_));}return;
 }
 if(command=="movespeed"||command=="attackspeed"){
  auto key=command=="movespeed"?"move_speed_multiplier":"attack_speed_multiplier";int cap=command=="movespeed"?3:5;if(args.size()==2)character_[key]=number(args[1],1,cap);
  send(8,2,stats_packet());refresh_self_appearance(send);publish_player(send);reply(command+": "+std::to_string(character_.value(key,1.))+"x");return;
 }
 if(command=="heal"){vitals();reply("HP e MP restaurados");return;}
 if(command=="forcesave"){
  persist();store_.db().exec("PRAGMA wal_checkpoint(PASSIVE)");auto grant=roles_->query("SELECT auth_generation FROM staff_roles WHERE account_id=? AND character_eid=?",{identity_.identity.id,std::int64_t(eid())});if(grant.empty())throw std::runtime_error("Cargo revogado");auto count=NativeControl(root_).save_all(event+":save",identity_.identity.id,eid(),grant[0].at("auth_generation"),identity_.id);reply("Personagem salvo; saves solicitados nas outras "+std::to_string(count)+" sessoes");return;
 }
 if(command=="fletta"){
  if(args.size()==2){character_["fletta"]=integer(args[1],0,1000000);persist();send(8,1,gauge(0x0801));}reply("Fletta: "+character_.value("fletta",Json(0)).dump());return;
 }
 if(command=="setlevel"||command=="setexp"){
  auto experience=command=="setlevel"?progression_.threshold(integer(args[1],1,120)):static_cast<std::uint64_t>(integer(args[1],0,progression_.threshold(120)+1000000000ULL));
  auto old=character_.at("level").get<int>();auto level=progression_.level(experience);character_["exp"]=experience;character_["level"]=level;character_["skill_points"]=std::max(0,character_.value("skill_points",0)+level-old);vitals();reply("Nivel "+std::to_string(level)+"; XP total "+std::to_string(experience));return;
 }
 if(command=="profession"){
  int code=integer(args[1],100,999),family=character_.at("cls").get<int>()/100*100;
  bool valid=false;
  // Match the native class hierarchy used by the skill catalog.
  if(code/100*100==family&&(code%100==0||(code/10%10>=1&&code/10%10<=2&&code%10<=2)))valid=true;
  if(!valid||character_.at("level").get<int>()<(code%10?45:code%100?10:1))throw std::runtime_error("Profissao invalida para a classe ou nivel");character_["cls"]=code;load_templates();clear_mount();vitals();refresh_self_appearance(send);reply("Profissao salva. Reentre na selecao para atualizar a arvore.");return;
 }
 if(command=="give"){
  auto id=static_cast<std::uint32_t>(integer(args[1],1,0xffffffffLL));int quantity=args.size()==3?integer(args[2],1,10000):1;if(id==999999)throw std::runtime_error("Use //carats para moeda");if(id<54000&&quantity!=1)throw std::runtime_error("Equipamentos: uma instancia por entrega");inventory_->grant_once(eid(),{{id,quantity}},event+":give");send_inventory(send,true);reply("Item recebido: "+inventory_->name(id));return;
 }
 if(command=="carats"){
  Wallet wallet(store_.db(),*inventory_);if(args.size()==2){auto text=args[1];text.erase(std::remove_if(text.begin(),text.end(),[](char c){return c=='.'||c==','||c=='_';}),text.end());wallet.apply(eid(),integer(text,-1000000000000LL,1000000000000LL),event+":carats");send_inventory(send,true);}reply("Carats: "+std::to_string(wallet.balance(eid())));return;
 }
 if(command=="addcash"||command=="cashbalance"){
  auto target=target_character(store_.db(),args[1]);auto account=target.at("account_id").get<std::string>();auto&db=store_.db();db.exec("CREATE TABLE IF NOT EXISTS premium_wallet(account_id TEXT PRIMARY KEY,balance INTEGER NOT NULL CHECK(balance BETWEEN 0 AND 4294967295));CREATE TABLE IF NOT EXISTS premium_ledger(event_key TEXT PRIMARY KEY,account_id TEXT NOT NULL,amount INTEGER NOT NULL,actor_account TEXT NOT NULL,actor_eid INTEGER NOT NULL,reason TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
  Transaction tx(db);auto rows=db.query("SELECT balance FROM premium_wallet WHERE account_id=?",{account});std::int64_t balance=rows.empty()?0:rows[0].at("balance").get<std::int64_t>();
  if(command=="addcash"){
   auto amount=integer(args[2],1,1000000000);auto reason=joined(args,3);if(reason.empty()||wide(reason).size()>200)throw std::runtime_error("Motivo deve ter 1 a 200 caracteres");auto key=event+":cash";auto prior=db.query("SELECT account_id,amount,actor_account,actor_eid,reason FROM premium_ledger WHERE event_key=?",{key});
   if(!prior.empty()){auto&r=prior[0];if(r.at("account_id")!=account||r.at("amount")!=amount||r.at("actor_account")!=identity_.identity.id||r.at("actor_eid")!=eid()||r.at("reason")!=reason)throw std::runtime_error("Evento Cash repetido com dados diferentes");}
   else{if(balance+amount>0xffffffffLL)throw std::runtime_error("Limite de Cash excedido");balance+=amount;db.exec("INSERT INTO premium_wallet VALUES(?,?) ON CONFLICT(account_id) DO UPDATE SET balance=excluded.balance",{account,balance});db.exec("INSERT INTO premium_ledger(event_key,account_id,amount,actor_account,actor_eid,reason) VALUES(?,?,?,?,?,?)",{key,account,amount,identity_.identity.id,std::int64_t(eid()),reason});}
  }tx.commit();reply("Cash de "+target.value("name","")+": "+std::to_string(balance));return;
 }
 if(command=="teleport"||command=="passwall"){
  if(character_.at("hp").get<int>()<=0)throw std::runtime_error("Personagem morto nao pode teleportar");auto pos=character_.at("pos");
  if(command=="teleport")pos={number(args[1],.001,99999.999),number(args[2],.001,99999.999),number(args[3],-10000,10000)};
  else{auto distance=args.size()==2?number(args[1],1,5000):150.;auto angle=character_["angle"].get<double>();pos[0]=pos[0].get<double>()+std::cos(angle)*distance;pos[1]=pos[1].get<double>()+std::sin(angle)*distance;if(pos[0].get<double>()<=0||pos[0].get<double>()>=100000||pos[1].get<double>()<=0||pos[1].get<double>()>=100000)throw std::runtime_error("Destino fora dos limites do mapa");}
  character_["pos"]=pos;clear_mount();recalculate();persist();refresh_self_appearance(send);publish_player(send);scene_visibility(*scene_,*scene_entities_,pos,scene_visible_,send);reply("Posicao atualizada e salva");return;
 }
 if(command=="maps"){
  auto rows=native_map_destinations(content_);int pages=std::max(1,static_cast<int>((rows.size()+7)/8));int page=args.size()==2?integer(args[1],1,pages):1;reply("Mapas "+std::to_string(page)+"/"+std::to_string(pages));
  for(std::size_t i=(page-1)*8;i<std::min(rows.size(),static_cast<std::size_t>(page*8));i++)reply(rows[i].at("map_id").dump()+" "+rows[i].value("world_name",""));return;
 }
 if(command=="map"){transition_map(resolve_native_map(content_,joined(args,1)),send);return;}
 if(command=="online"||command=="goto"){
  auto rows=scene_->online(role=="Admin");
  if(command=="online"){int page=args.size()==2?integer(args[1],1,100000):1;reply("Online neste canal: "+std::to_string(rows.size())+"; pagina "+std::to_string(page));for(std::size_t i=(page-1)*5;i<std::min(rows.size(),static_cast<std::size_t>(page*5));i++)reply(rows[i].value("name","")+" / "+rows[i].value("map",""));return;}
  auto target=target_character(store_.db(),args[1]);auto id=target.at("eid").get<std::uint32_t>();auto leases=leases_.query("SELECT channel FROM native_player_leases WHERE eid=? AND expires>?",{std::int64_t(id),now()});if(leases.empty())throw std::runtime_error("Personagem nao esta online");if(leases[0].at("channel")!=identity_.channel)throw std::runtime_error("Entre no canal "+leases[0].at("channel").dump()+" para usar //goto");
  for(auto&row:rows)if(row.at("eid")==id){auto p=row.at("position");if(row.at("map")==map_name_)staff_command({"teleport",p[0].dump(),p[1].dump(),p[2].dump()},event+":goto",send);else{auto destination=resolve_native_map(content_,row.at("map"));destination["x"]=p[0];destination["y"]=p[1];destination["z"]=p[2];transition_map(destination,send);}return;}throw std::runtime_error("Personagem indisponivel ou ainda carregando o mapa");
 }
 if(command=="mobinfo"||command=="findmob"||command=="bosstimers"){
  auto entities=scene_->map(scene_map_name());
  if(command=="mobinfo"){
   auto actor=static_cast<std::uint32_t>(integer(args[1],1,0xffffffff));
   auto found=entities->find(actor);
   if(found==entities->end())throw std::runtime_error("EID ausente deste mapa");
   const auto& entity=found->second;
   reply(entity.name+" | ID "+std::to_string(entity.npc_id)+" | EID "+std::to_string(actor));
   reply("Nivel "+std::to_string(entity.level)+" | HP "+std::to_string(entity.hp)+"/"+std::to_string(entity.maxhp));
   reply("Posicao "+Json::array({entity.position.x,entity.position.y,entity.position.z}).dump()+
         " | alvo "+std::to_string(entity.target));
   return;
  }
  if(command=="findmob"){
   auto query=joined(args,1);
   auto needle=wide(query);
   for(auto& c:needle)c=std::towlower(c);
   if(needle.size()<2||needle.size()>60)throw std::runtime_error("Use de 2 a 60 caracteres");
   std::map<std::uint32_t,const Entity*> matches;
   for(const auto& [actor,entity]:*entities)if(entity.combat||entity.guard){
    auto name=wide(entity.name);
    for(auto& c:name)c=std::towlower(c);
    if(name.find(needle)!=std::wstring::npos)matches.try_emplace(entity.npc_id,&entity);
   }
   if(matches.empty()){reply("Nenhum mob ou guarda encontrado neste mapa.");return;}
   int sent=0;
   for(const auto& [id,entity]:matches){
    reply(entity->name+" | ID "+std::to_string(id)+" | EID "+std::to_string(entity->eid));
    if(++sent==8)break;
   }
   if(matches.size()>8)reply("Mostrando 8 tipos. Use um nome mais especifico.");
   return;
  }
  auto page=args.size()==1?1:integer(args[1],1,10000);
  std::vector<const Entity*> bosses;
  for(const auto& [actor,entity]:*entities)
   if(entity.combat&&entity.category=="boss")bosses.push_back(&entity);
  auto first=static_cast<std::size_t>((page-1)*8);
  if(first>=bosses.size()){reply("Nenhum chefe nesta pagina do mapa.");return;}
  const auto stamp=now();
  for(auto it=bosses.begin()+first;it!=bosses.end()&&it!=bosses.begin()+first+std::min<std::size_t>(8,bosses.size()-first);++it){
   const auto& boss=**it;
   auto status=boss.alive?"vivo":"renasce em "+std::to_string(static_cast<std::int64_t>(std::ceil(std::max(0.,boss.respawn_at-stamp))))+" s";
   reply(boss.name+" | EID "+std::to_string(boss.eid)+" | "+status);
  }
  return;
 }
 if(command=="spawn"){
  auto type=static_cast<std::uint32_t>(integer(args[1],1,0xffffffffLL));int count=args.size()==3?integer(args[2],1,10):1;auto p=character_.at("pos");scene_->summon(scene_map_name(),type,count,{p[0],p[1],p[2]});scene_entities_=scene_->map(scene_map_name());scene_visibility(*scene_,*scene_entities_,p,scene_visible_,send);reply(std::to_string(count)+" mobs invocados com IA no mapa");return;
 }
 if(command=="respawn"){auto count=scene_->respawn(scene_map_name(),store_.db(),map_scope(),now());scene_entities_=scene_->map(scene_map_name());reply(std::to_string(count)+" respawns agendados");return;}
 if(command=="starter"){
  std::string key="starter-v3:"+std::to_string(eid());std::vector<std::pair<std::uint32_t,int>> items;bool legacy=!store_.db().query("SELECT 1 FROM inventory_grants WHERE event_key IN (?,?)",{"starter-v1:"+std::to_string(eid()),"starter-open-v2:"+std::to_string(eid())}).empty();
  if(!legacy){items={{54005,100},{54205,100}};if(character_.at("cls").get<int>()/100*100==800)items.push_back({69507,100});}if(store_.db().query("SELECT 1 FROM inventory_slots WHERE player_eid=? AND item_id=999000121",{std::int64_t(eid())}).empty())items.push_back({999000121,1});bool changed=inventory_->grant_once(eid(),items,key);send_inventory(send,true);reply(changed?"Starter entregue e salvo":"Starter ja recebido por este personagem");return;
 }
 if(command=="queststatus"){auto rows=store_.db().query("SELECT * FROM quest_state WHERE player_eid=?",{std::int64_t(eid())});reply(rows.empty()?"Nenhuma quest ativa":rows.dump());return;}
 if(command=="setrole"){
  auto target=target_character(store_.db(),args[1]);auto chosen=args[2];for(auto&c:chosen)c=std::tolower(static_cast<unsigned char>(c));static const std::map<std::string,std::string> names={{"player","Player"},{"alpha","Alpha"},{"beta","Beta"},{"helper","Helper"},{"ticket","Helper"},{"mod","Mod"},{"gm","GM"},{"admin","Admin"},{"dev","Admin"}};if(!names.count(chosen))throw std::runtime_error("Cargo invalido");chosen=names.at(chosen);auto account=target.at("account_id").get<std::string>();auto targetid=target.at("eid").get<std::uint32_t>();
  Sqlite roles(root_/"Database/karos_roles.sqlite");Transaction tx(roles);auto generation=token(24);if(chosen=="Player")roles.exec("DELETE FROM staff_roles WHERE account_id=? AND character_eid=?",{account,std::int64_t(targetid)});else roles.exec("INSERT INTO staff_roles(account_id,character_eid,character_name,role,auth_generation) VALUES(?,?,?,?,?) ON CONFLICT(account_id,character_eid) DO UPDATE SET character_name=excluded.character_name,role=excluded.role,auth_generation=excluded.auth_generation,granted_at=CURRENT_TIMESTAMP",{account,std::int64_t(targetid),target.at("name").get<std::string>(),chosen,generation});
  roles.exec("INSERT INTO staff_audit(account_id,character_eid,character_name,new_role,changed_by) VALUES(?,?,?,?,?)",{account,std::int64_t(targetid),target.at("name").get<std::string>(),chosen,identity_.identity.id});tx.commit();Sqlite credentials(root_/"Database/staff_second_login.sqlite");credentials.exec("DELETE FROM staff_credentials WHERE account=? AND eid=?",{account,std::int64_t(targetid)});reply("Cargo atualizado: "+target.value("name","")+" -> "+chosen);return;
 }
 if(command=="ban"||command=="unban"||command=="mute"||command=="unmute"||command=="sanctions"||command=="kick"){
  auto target=target_character(store_.db(),args[1]);auto account=target.at("account_id").get<std::string>();auto targetid=target.at("eid").get<std::uint32_t>();int level=role_level(role);
  Sqlite db(root_/"Database/moderation.sqlite");db.exec("CREATE TABLE IF NOT EXISTS native_kicks(session TEXT PRIMARY KEY,until_at REAL NOT NULL,reason TEXT NOT NULL);CREATE TABLE IF NOT EXISTS moderation_audit(id INTEGER PRIMARY KEY,action TEXT NOT NULL,sanction_id INTEGER,actor_account TEXT,actor_eid INTEGER,created REAL)");
  if(command=="sanctions"){
   auto rows=db.query("SELECT id,kind,scope,expires FROM sanctions WHERE revoked IS NULL AND (expires IS NULL OR expires>?) AND ((scope='account' AND subject=?) OR (scope='character' AND subject=?) OR (scope='hardware' AND subject IN (SELECT fingerprint FROM device_observations WHERE account_id=?)))",{now(),account,std::to_string(targetid),account});if(rows.empty())reply("Sem punicoes ativas");else for(auto&r:rows)reply("#"+r.at("id").dump()+" "+r.value("kind","")+"/"+r.value("scope","")+" "+(r.at("expires").is_null()?"permanente":std::to_string(std::max(0,static_cast<int>(r.at("expires").get<double>()-now())))+"s"));return;
  }
  if(command=="kick"){
   if(role_level(character_role(*roles_,account,targetid))>=level)throw std::runtime_error("Nao e permitido expulsar cargo igual ou superior");auto reason=joined(args,2);auto sessions=leases_.query("SELECT session FROM native_player_leases WHERE eid=? AND expires>?",{std::int64_t(targetid),now()});Transaction tx(db);for(auto&s:sessions)db.exec("INSERT INTO native_kicks VALUES(?,?,?) ON CONFLICT(session) DO UPDATE SET until_at=excluded.until_at,reason=excluded.reason",{s.at("session").get<std::string>(),now()+30,reason});db.exec("INSERT INTO moderation_audit(action,actor_account,actor_eid,created) VALUES(?,?,?,?)",{"kick:"+std::to_string(targetid),identity_.identity.id,std::int64_t(eid()),now()});tx.commit();reply("Sessoes em encerramento: "+std::to_string(sessions.size()));return;
  }
  auto scope=args[2];for(auto&c:scope)c=std::tolower(static_cast<unsigned char>(c));if(scope=="char"||scope=="personagem")scope="character";if(scope=="conta")scope="account";if(scope=="hw")scope="hardware";if(scope!="character"&&scope!="account"&&scope!="hardware")throw std::runtime_error("Alvo: character, account ou hardware");
  bool remove=command=="unban"||command=="unmute";auto kind=command=="mute"||command=="unmute"?"mute":"ban";int minimum=scope=="hardware"?5:scope=="account"||std::string(kind)=="ban"?3:2;if(level<minimum)throw std::runtime_error("Cargo insuficiente para este alvo de punicao");
  double seconds=0;if(!remove){auto duration=args[3];for(auto&c:duration)c=std::tolower(static_cast<unsigned char>(c));if(duration!="perm"&&duration!="permanent"&&duration!="permanente"){std::smatch matched;if(!std::regex_match(duration,matched,std::regex("([1-9][0-9]{0,6})([smhd])")))throw std::runtime_error("Duracao: 30m, 2h, 7d ou perm");seconds=integer(matched[1],1,9999999)*(matched[2]=="d"?86400.:matched[2]=="h"?3600.:matched[2]=="m"?60.:1.);if(seconds>315360000)throw std::runtime_error("Duracao maxima de dez anos");}if(level==2&&(seconds==0||seconds>86400))throw std::runtime_error("Mod: mute temporario por ate 24h");}
  std::set<std::string> affected={account};std::vector<std::string> subjects;
  if(scope=="hardware"){for(auto&r:db.query("SELECT fingerprint FROM device_observations WHERE account_id=?",{account})){auto device=r.at("fingerprint").get<std::string>();subjects.push_back(device);for(auto&a:db.query("SELECT account_id FROM device_observations WHERE fingerprint=?",{device}))affected.insert(a.at("account_id"));}if(subjects.empty())throw std::runtime_error("Nenhuma assinatura de dispositivo observada");}else subjects.push_back(scope=="character"?std::to_string(targetid):account);
  if(scope=="character"){if(role_level(character_role(*roles_,account,targetid))>=level)throw std::runtime_error("Nao e permitido punir cargo igual ou superior");}else for(auto&aid:affected)for(auto&r:roles_->query("SELECT role FROM staff_roles WHERE account_id=?",{aid}))if(role_level(r.at("role"))>=level)throw std::runtime_error("A conta ou dispositivo inclui cargo igual ou superior");
  auto reason=remove?std::string():joined(args,4);if(!remove&&(reason.empty()||wide(reason).size()>200))throw std::runtime_error("Informe motivo de 1 a 200 caracteres");Transaction tx(db);int changed=0;
  for(auto&subject:subjects){if(remove)db.exec("UPDATE sanctions SET revoked=? WHERE kind=? AND scope=? AND subject=? AND revoked IS NULL AND (expires IS NULL OR expires>?)",{now(),std::string(kind),scope,subject,now()});else db.exec("INSERT INTO sanctions(kind,scope,subject,actor_account,actor_eid,reason,created,expires) VALUES(?,?,?,?,?,?,?,?)",{std::string(kind),scope,subject,identity_.identity.id,std::int64_t(eid()),reason,now(),seconds?SqlValue(now()+seconds):SqlValue(nullptr)});changed+=db.changes();db.exec("INSERT INTO moderation_audit(action,actor_account,actor_eid,created) VALUES(?,?,?,?)",{command,identity_.identity.id,std::int64_t(eid()),now()});}tx.commit();reply(std::string(remove?"Revogadas: ":"Aplicadas: ")+std::to_string(changed));return;
 }
 if(command=="announce"){
  auto text=joined(args,1);if(text.empty()||wide(text).size()>110)throw std::runtime_error("Anuncio: 1 a 110 caracteres");JobQueue jobs(root_);for(auto&s:jobs.services())if(s.value("alive",false)&&s.value("role","")=="WorldServer")jobs.enqueue(s.at("instance"),"announce",{{"text",text}});reply("Anuncio enviado aos canais ativos");return;
 }
 if(command=="restart"||command=="reset"||command=="shutdown"||command=="cancel"){
  NativeControl controls(root_);if(command=="cancel"){reply(controls.cancel()?"Contagem cancelada":"Nenhuma contagem ativa");return;}
  auto grant=roles_->query("SELECT auth_generation FROM staff_roles WHERE account_id=? AND character_eid=?",{identity_.identity.id,std::int64_t(eid())});if(grant.empty())throw std::runtime_error("Cargo revogado");int delay=args.size()==2?integer(args[1],0,3600):60;controls.schedule(command,delay,identity_.identity.id,eid(),grant[0].at("auth_generation"));reply(command+" agendado para "+std::to_string(delay)+" segundos; saves serao confirmados antes");return;
 }
 throw std::runtime_error("Comando desconhecido. Use //gmhelp.");
}
}
