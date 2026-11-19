#include "native/staff.hpp"
#include "native/scene.hpp"
#include <algorithm>
#include <cctype>
namespace rosh {
static int length(const std::string&s){auto value=wide(s);int count=0;for(auto c:value)if(c<0xdc00||c>0xdfff)count++;return count;}
static std::string ascii_lower(std::string s){for(auto&c:s)if(c>='A'&&c<='Z')c+=32;return s;}
std::string canonical_staff_command(const std::string&text){
 auto command=ascii_lower(text);
 if(command=="sc"||command=="staff")return "staffchat";
 static const std::map<std::string,std::string> aliases={{"modhelp","gmhelp"},{"adminhelp","gmhelp"},{"devhelp","gmhelp"},{"modstatus","gmstatus"},{"adminstatus","gmstatus"},{"devstatus","gmstatus"},{"carat","carats"},{"speedattack","attackspeed"},{"aspeed","attackspeed"},{"noclip","passwall"},{"dano","damage"},{"rastrearitem","itemtrace"}};
 auto found=aliases.find(command);return found==aliases.end()?command:found->second;
}
const std::map<std::string,CommandPolicy>& staff_command_policies(){
 static const std::map<std::string,CommandPolicy> policies={
 {"help",{0,0,0}},{"pk",{0,0,1}},{"cashshop",{0,0,0}},
 {"titles",{0,0,0}},{"titleuse",{0,1,1}},{"titlecatalog",{5,0,0}},{"titlegrant",{5,2,25}},{"titlerevoke",{5,2,25}},{"itemstats",{0,0,1}},{"itemtrace",{5,1,2}},{"party",{0,0,2}},{"damage",{0,0,1}},
 {"gmhelp",{1,0,1}},{"gmstatus",{1,0,0}},{"staffchat",{1,1,30}},
 {"where",{1,0,0}},{"stats",{1,0,1}},{"inventory",{1,0,0}},{"cooldowns",{1,0,0}},{"item",{1,1,1}},{"finditem",{1,1,20}},{"queststatus",{1,0,0}},{"maps",{1,0,1}},{"online",{1,0,1}},
 {"npc",{2,0,1}},{"testdamage",{2,0,1}},{"vanish",{2,0,1}},{"teleport",{2,3,3}},{"map",{2,1,20}},{"goto",{2,1,1}},{"heal",{2,0,0}},
 {"mobinfo",{3,1,1}},{"findmob",{3,1,15}},{"bosstimers",{3,0,1}},{"god",{3,0,1}},{"movespeed",{3,0,1}},{"attackspeed",{3,0,1}},{"spawn",{3,1,2}},{"respawn",{3,0,0}},{"announce",{3,1,30}},{"kick",{3,2,30}},
 {"profession",{5,1,1}},{"fletta",{5,0,1}},{"give",{5,1,2}},{"carats",{5,0,1}},{"setlevel",{5,1,1}},{"setexp",{5,1,1}},{"starter",{5,0,0}},{"forcesave",{5,0,0}},{"passwall",{5,0,1}},{"debug",{5,0,1}},{"devinspect",{5,0,1}},{"addcash",{5,3,25}},{"cashbalance",{5,1,1}},{"setrole",{5,2,2}},{"restart",{5,0,1}},{"reset",{5,0,1}},{"shutdown",{5,0,1}},{"cancel",{5,0,0}},
 {"ban",{3,4,30}},{"unban",{3,2,2}},{"mute",{2,4,30}},{"unmute",{2,2,2}},{"sanctions",{2,1,1}}};return policies;
}
void authorize_staff_command(const std::string&role,const std::string&command,std::size_t arguments){
 static const std::map<std::string,int> levels={{"Player",0},{"Alpha",0},{"Beta",0},{"Helper",1},{"Ticket",1},{"Mod",2},{"GM",3},{"Admin",5},{"Dev",5}};
 auto level=levels.find(role);auto policy=staff_command_policies().find(command);
 if(policy==staff_command_policies().end())throw std::runtime_error("Comando desconhecido. Use //gmhelp.");
 if(level==levels.end()||level->second<policy->second.level)throw std::runtime_error("Cargo insuficiente para //"+command);
 if(arguments<static_cast<std::size_t>(policy->second.min_args)||arguments>static_cast<std::size_t>(policy->second.max_args))throw std::runtime_error("Quantidade de argumentos invalida para //"+command+". Use //gmhelp.");
}
std::optional<std::string> normalized_native_command(unsigned char sub,const std::string&text){
 if(text.rfind("//",0)==0)return text;
 // Native UI command paths can consume the slash prefix. Normalize only
 // recognized command words; never broadcast credential commands as chat.
 if((sub!=0&&sub!=8)||text.empty()||text[0]=='!')return {};
 auto candidate=text[0]=='/'?text.substr(1):text;
 auto first=canonical_staff_command(candidate.substr(0,candidate.find_first_of(" \t")));
 if(sub==0&&text[0]!='/'&&first!="staffregister"&&first!="stafflogin")return {};
 static const std::set<std::string> auth={"staffhelp","staffregister","stafflogin","stafflogout","adminauth","devauth","gmauth","modauth"};
 if(staff_command_policies().count(first)||auth.count(first))return "//"+candidate;
 return {};
}
Bytes staff_chat_reply(std::uint32_t eid,const std::string&text){
 if(!eid)throw std::runtime_error("Invalid command recipient");auto value=wide(text);if(value.empty())value=L"Comando recebido. Use //staffhelp.";
 if(value.size()>110){value.resize(110);if(value.back()>=0xd800&&value.back()<=0xdbff)value.pop_back();}
 for(auto&c:value)if(c<32)c=L' ';auto message=utf8(value);Bytes body(9);put32(body,0,eid,true);put32(body,4,1,true);std::string sender="Server";body.insert(body.end(),sender.begin(),sender.end());body.push_back(0);body.insert(body.end(),message.begin(),message.end());body.push_back(0);return body;
}
StaffChannel::StaffChannel(Sqlite&db):db_(db){
 db_.exec("CREATE TABLE IF NOT EXISTS native_staff_chat(id INTEGER PRIMARY KEY AUTOINCREMENT,event_key TEXT NOT NULL UNIQUE,account TEXT NOT NULL,eid INTEGER NOT NULL,sender TEXT NOT NULL,message TEXT NOT NULL,created REAL NOT NULL)");
}
std::int64_t StaffChannel::latest(){return db_.query("SELECT COALESCE(MAX(id),0) AS id FROM native_staff_chat")[0].at("id");}
void StaffChannel::post(const std::string&role,const std::string&account,std::uint32_t eid,const std::string&name,const std::string&text,const std::string&event,double timestamp){
 authorize_staff_command(role,"staffchat",1);
 if(account.empty()||!eid||name.empty()||length(name)>30||text.empty()||length(text)>110||event.empty()||event.size()>160)throw std::runtime_error("Mensagem da equipe invalida");
 for(auto c:wide(text))if(c<32)throw std::runtime_error("Controle invalido na mensagem da equipe");
 Transaction tx(db_);auto prior=db_.query("SELECT account,eid,sender,message FROM native_staff_chat WHERE event_key=?",{event});
 if(!prior.empty()){auto&r=prior[0];if(r.at("account")!=account||r.at("eid")!=eid||r.at("sender")!=name||r.at("message")!=text)throw std::runtime_error("Recibo do chat da equipe alterado");tx.commit();return;}
 auto recent=db_.query("SELECT created FROM native_staff_chat WHERE account=? ORDER BY id DESC LIMIT 1",{account});if(!recent.empty()&&recent[0].at("created").get<double>()+.75>timestamp)throw std::runtime_error("Aguarde antes de enviar outra mensagem para a equipe");
 db_.exec("INSERT INTO native_staff_chat(event_key,account,eid,sender,message,created) VALUES(?,?,?,?,?,?)",{event,account,std::int64_t(eid),name,text,timestamp});tx.commit();
}
Json StaffChannel::poll(std::int64_t after){return db_.query("SELECT id,sender,message FROM native_staff_chat WHERE id>? ORDER BY id LIMIT 64",{after});}
std::string client_text(unsigned char sub,const Bytes&body,std::uint32_t eid){
 if(body.size()>512)throw std::runtime_error("Invalid chat payload");std::size_t start;
 if(sub==0){if(body.size()<12||body[8]||get32(body,0,true)!=eid)throw std::runtime_error("Chat identity mismatch");start=9;}
 else if(sub==8){Bytes header={0,0,0,0,0,25,0};if(body.size()<10||!std::equal(header.begin(),header.end(),body.begin()))throw std::runtime_error("Invalid command header");start=7;}
 else throw std::runtime_error("Unknown text opcode");
 auto nameEnd=std::find(body.begin()+start,body.end(),0);if(nameEnd==body.end()||nameEnd==body.begin()+start||nameEnd-(body.begin()+start)>72)throw std::runtime_error("Invalid sender field");
 auto textStart=nameEnd+1,textEnd=std::find(textStart,body.end(),0);auto bytes=textEnd-textStart;
 if(bytes<1||bytes>220)throw std::runtime_error("Invalid text length");std::string text(textStart,textEnd);
 try{wide(text);}catch(...){int n=MultiByteToWideChar(1252,0,text.data(),text.size(),nullptr,0);std::wstring converted(n,L'\0');MultiByteToWideChar(1252,0,text.data(),text.size(),converted.data(),n);text=utf8(converted);}
 if(length(text)>110)throw std::runtime_error("Text exceeds110characters");for(auto c:wide(text))if(c<32&&c!=9)throw std::runtime_error("Unsupported control character");return text;
}
std::vector<std::string> command_tokens(const std::string&text){
 std::vector<std::string> out;std::string token;char quote=0;bool escaped=false,active=false;
 for(char c:text){if(escaped){token+=c;escaped=false;active=true;continue;}if(c=='\\'&&quote!='\''){escaped=true;active=true;continue;}if(quote){if(c==quote)quote=0;else token+=c;active=true;continue;}if(c=='\''||c=='"'){quote=c;active=true;continue;}if(c==' '||c=='\t'){if(active){out.push_back(token);token.clear();active=false;}}else{token+=c;active=true;}}
 if(quote||escaped)throw std::runtime_error("Sintaxe invalida: confira as aspas");if(active)out.push_back(token);if(!out.empty()){if(out[0].rfind("//",0)==0)out[0].erase(0,2);out[0]=canonical_staff_command(out[0]);if(out[0].empty())out.clear();}return out;
}
StaffSession::StaffSession(const fs::path&root,const std::string&account,std::uint32_t eid):roles_(root/"Database/karos_roles.sqlite",true),passwords_(root/"Database/staff_second_login.sqlite"),account_(account),eid_(eid){
 passwords_.exec("CREATE TABLE IF NOT EXISTS staff_credentials(account TEXT,eid INTEGER,generation TEXT NOT NULL,salt BLOB NOT NULL,digest BLOB NOT NULL,PRIMARY KEY(account,eid));CREATE TABLE IF NOT EXISTS staff_login_failures(account TEXT,eid INTEGER,failures INTEGER,until_at REAL,PRIMARY KEY(account,eid))");
}
std::string StaffSession::role(){auto value=character_role(roles_,account_,eid_);return value.empty()?"Player":value;}
std::string StaffSession::grant(){
 auto current=role();if(current!="Mod"&&current!="GM"&&current!="Admin")throw std::runtime_error("E necessario cargo Mod ou superior atribuido pelo servidor");
 auto rows=roles_.query("SELECT auth_generation FROM staff_roles WHERE account_id=? AND character_eid=?",{account_,std::int64_t(eid_)});
 if(rows.empty()||rows[0].at("auth_generation").is_null()||rows[0].at("auth_generation")=="")throw std::runtime_error("Cargo revogado");return rows[0].at("auth_generation");
}
bool StaffSession::valid(){try{return !generation_.empty()&&secure_equal(sha256(generation_),sha256(grant()));}catch(...){generation_.clear();return false;}}
std::optional<std::string> StaffSession::authentication(const std::vector<std::string>&args){
 if(args.empty())return {};auto cmd=args[0];
 if(cmd=="adminauth"||cmd=="devauth"||cmd=="gmauth"||cmd=="modauth"){generation_.clear();return "Comando removido. Use //staffregister e //stafflogin.";}
 if(cmd=="staffhelp")return "//staffregister senha confirmacao; //stafflogin senha; //stafflogout; //gmstatus";
 if(cmd=="stafflogout"){generation_.clear();return "Segundo login staff encerrado.";}
 if(cmd=="gmstatus"||cmd=="adminstatus"||cmd=="modstatus")return "Cargo: "+role()+"; segundo login: "+(valid()?"autenticado":"nao autenticado");
 if(cmd!="staffregister"&&cmd!="stafflogin")return {};
 generation_.clear();auto generation=grant();
 if(args.size()!=(cmd=="staffregister"?3u:2u))throw std::runtime_error("Use //staffregister senha confirmacao ou //stafflogin senha");auto password=args[1];int size=length(password);
 if(size<12||size>128||std::any_of(password.begin(),password.end(),[](unsigned char c){return std::isspace(c);}))throw std::runtime_error("Senha staff:12 a128 caracteres, sem espacos");
 if(cmd=="staffregister"){
  if(password!=args[2])throw std::runtime_error("As senhas nao coincidem");auto salt=random_bytes(32);auto digest=pbkdf2(password,salt,600000);Transaction tx(passwords_);
  auto prior=passwords_.query("SELECT generation FROM staff_credentials WHERE account=? AND eid=?",{account_,std::int64_t(eid_)});if(!prior.empty()&&prior[0].at("generation")==generation)throw std::runtime_error("Senha ja cadastrada. Use //stafflogin senha");
  if(grant()!=generation)throw std::runtime_error("O cargo mudou. Tente novamente");
  passwords_.exec("INSERT INTO staff_credentials VALUES(?,?,?,?,?) ON CONFLICT(account,eid) DO UPDATE SET generation=excluded.generation,salt=excluded.salt,digest=excluded.digest",{account_,std::int64_t(eid_),generation,salt,digest});passwords_.exec("DELETE FROM staff_login_failures WHERE account=? AND eid=?",{account_,std::int64_t(eid_)});tx.commit();return "Senha staff cadastrada. Use //stafflogin senha para esta sessao.";
 }
 auto fail=passwords_.query("SELECT failures,until_at FROM staff_login_failures WHERE account=? AND eid=?",{account_,std::int64_t(eid_)});if(!fail.empty()&&fail[0].at("failures").get<int>()>=5&&fail[0].at("until_at").get<double>()>now())throw std::runtime_error("Muitas tentativas. Aguarde5minutos");
 auto rows=passwords_.query("SELECT generation,salt,digest FROM staff_credentials WHERE account=? AND eid=?",{account_,std::int64_t(eid_)});
 if(rows.empty()||rows[0].at("generation")!=generation)throw std::runtime_error("Cadastre sua senha com //staffregister senha confirmacao");
 if(!secure_equal(pbkdf2(password,unhex(rows[0].at("salt")),600000),unhex(rows[0].at("digest")))){
  int failures=(!fail.empty()&&fail[0].at("until_at").get<double>()>now()?fail[0].at("failures").get<int>():0)+1;passwords_.exec("INSERT INTO staff_login_failures VALUES(?,?,?,?) ON CONFLICT(account,eid) DO UPDATE SET failures=excluded.failures,until_at=excluded.until_at",{account_,std::int64_t(eid_),std::int64_t(failures),now()+300});throw std::runtime_error("Senha staff invalida");
 }
 if(grant()!=generation)throw std::runtime_error("O cargo mudou. Autentique novamente");passwords_.exec("DELETE FROM staff_login_failures WHERE account=? AND eid=?",{account_,std::int64_t(eid_)});generation_=generation;return "Segundo login staff autenticado nesta sessao.";
}
}
