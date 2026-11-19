#include "native/inventory.hpp"
#include "native/economy.hpp"
#include <algorithm>
#include <limits>
namespace rosh {
static void insert_row(Sqlite&db,std::uint32_t eid,int slot,const ItemInstance&item){db.exec("INSERT INTO inventory_slots VALUES(?,?,?,?,?,?)",{std::int64_t(eid),std::int64_t(slot),std::int64_t(item.item),item.quantity,std::int64_t(item.uid),std::int64_t(item.created)});}
namespace {
Json storage_character(Sqlite &db,const std::string &account,std::uint32_t eid) {
    auto rows=db.query("SELECT payload FROM accounts WHERE id=?",{account});
    if (account.empty()||!eid||rows.empty()) throw std::runtime_error("Deposito indisponivel");
    const auto characters=Json::parse(rows[0].at("payload").get<std::string>()).value("chars",Json::array());
    const auto character=std::find_if(characters.begin(),characters.end(),
        [&](const auto &entry){return entry.value("eid",0u)==eid;});
    if (character==characters.end()) throw std::runtime_error("Personagem indisponivel");
    return *character;
}

Json guild_membership(Sqlite &db,const std::string &account,std::uint32_t eid) {
    auto rows=db.query("SELECT m.guild_id,m.rank,r.rights,c.rights AS carat_rights FROM native_guild_members m "
                       "JOIN native_guilds g ON g.id=m.guild_id "
                       "LEFT JOIN native_guild_storage_rights r ON r.guild_id=m.guild_id AND r.rank=m.rank "
                       "LEFT JOIN native_guild_carats_rights c ON c.guild_id=m.guild_id AND c.rank=m.rank "
                       "WHERE m.eid=? AND m.account_id=?", {std::int64_t(eid),account});
    if (rows.empty()) throw std::runtime_error("Voce nao tem um cla");
    return rows[0];
}

std::string storage_key(Sqlite &db,const std::string &account,std::uint32_t eid,PersonalStorage kind,
                        StorageRight required) {
    storage_character(db,account,eid);
    switch(kind) {
    case PersonalStorage::Character: return "character:"+account+":"+std::to_string(eid);
    case PersonalStorage::Account: return "account:"+account;
    case PersonalStorage::Guild: {
        const auto member=guild_membership(db,account,eid);
        const int rank=member.at("rank");
        const unsigned itemRights=rank==5||member.at("rights").is_null()?
            (rank>=4?7u:3u):member.at("rights").get<unsigned>();
        const unsigned caratRights=rank==5||member.at("carat_rights").is_null()?
            (rank>=4?3u:0u):member.at("carat_rights").get<unsigned>();
        const unsigned rights=itemRights|(caratRights<<3);
        if (!(rights&static_cast<unsigned>(required)))
            throw std::runtime_error("Seu cargo nao tem permissao para esta operacao no deposito do cla");
        return "guild:"+std::to_string(member.at("guild_id").get<std::int64_t>());
    }
    }
    throw std::runtime_error("Deposito indisponivel");
}

void check_storage_slot(PersonalStorage kind,int slot) {
    if (slot<0||slot>=storage_capacity(kind)) throw std::runtime_error("Slot do deposito invalido");
}

ItemInstance storage_item(const Json &row) {
    ItemInstance item{};
    item.slot=row.at("slot");item.item=row.at("item_id");item.quantity=row.at("quantity");
    item.uid=row.at("uid");item.created=row.at("created");item.grade=row.at("grade");
    if (!row.at("expires_at").is_null()) item.expires=row.at("expires_at").get<std::uint32_t>();
    return item;
}

constexpr auto storage_select=
    "SELECT s.*,COALESCE(e.grade,0) AS grade,r.expires_at FROM personal_storage s "
    "LEFT JOIN item_enchantments e ON e.uid=s.uid LEFT JOIN reward_instances r ON r.uid=s.uid ";

void archive_storage_expiry(Sqlite &db,const std::string &key,double time) {
    const std::vector<SqlValue> args{time,key,time};
    db.exec("INSERT OR IGNORE INTO native_expired_items "
            "SELECT s.uid,s.deposited_by,s.slot,0,s.item_id,s.quantity,s.created,r.expires_at,? "
            "FROM personal_storage s JOIN reward_instances r ON r.uid=s.uid "
            "WHERE s.owner_key=? AND r.expires_at<=?",args);
    db.exec("DELETE FROM personal_storage WHERE owner_key=? AND uid IN "
            "(SELECT uid FROM reward_instances WHERE expires_at<=?)",{key,time});
}
}

void Inventory::guild_storage_set_rights(const std::string &account,std::uint32_t eid,int rank,unsigned rights) {
    if (rank<0||rank>=5||rights>31||((rights&1u)==0&&rights!=0))
        throw std::runtime_error("Permissoes de deposito invalidas");
    Transaction tx(db_);
    storage_character(db_,account,eid);
    const auto member=guild_membership(db_,account,eid);
    if (member.at("rank")!=5) throw std::runtime_error("Apenas o lider do cla configura o deposito");
    const auto guild=member.at("guild_id").get<std::int64_t>();
    db_.exec("INSERT INTO native_guild_storage_rights VALUES(?,?,?) "
             "ON CONFLICT(guild_id,rank) DO UPDATE SET rights=excluded.rights",
             {guild,std::int64_t(rank),std::int64_t(rights&7u)});
    db_.exec("INSERT INTO native_guild_carats_rights VALUES(?,?,?) "
             "ON CONFLICT(guild_id,rank) DO UPDATE SET rights=excluded.rights",
             {guild,std::int64_t(rank),std::int64_t(rights>>3)});
    tx.commit();
}
std::int64_t Inventory::storage_carats(const std::string &account,std::uint32_t eid,PersonalStorage kind,
                                       std::int64_t delta,const std::string &event,std::optional<int> slot) {
    if (!delta||delta==std::numeric_limits<std::int64_t>::min()||event.empty()||event.size()>100)
        throw std::runtime_error("Quantidade de Carats invalida");
    Transaction tx(db_);
    const auto key=storage_key(db_,account,eid,kind,delta>0?StorageRight::DepositCarats:StorageRight::WithdrawCarats);
    const auto rows=db_.query("SELECT amount,slot,uid,created FROM storage_carats WHERE owner_key=?",{key});
    const auto balance=rows.empty()?0:rows[0].at("amount").get<std::int64_t>();
    const auto prior=db_.query("SELECT owner_key,eid,delta FROM storage_carats_events WHERE event_key=?",{event});
    if (!prior.empty()) {
        if(prior[0].at("owner_key")!=key||prior[0].at("eid")!=eid||prior[0].at("delta")!=delta)
            throw std::runtime_error("Operacao de Carats alterada");
        tx.commit();
        return balance;
    }
    if ((delta<0&&balance<-delta)||(delta>0&&delta>1000000000000LL-balance))
        throw std::runtime_error("Saldo de Carats insuficiente no deposito");
    const auto amount=balance+delta;
    auto occupied=db_.query("SELECT slot FROM personal_storage WHERE owner_key=?",{key});
    std::set<int> free;
    for(int i=0;i<storage_capacity(kind);++i)free.insert(i);
    for(const auto &row:occupied)free.erase(row.at("slot").get<int>());
    if(!balance&&(free.empty()||(slot&&!free.count(*slot))))throw InventoryFull();
    const auto target=balance?rows[0].at("slot").get<int>():slot.value_or(*free.begin());
    if(delta<0&&slot&&*slot!=target)throw std::runtime_error("Pilha de Carats indisponivel");
    const auto uid=rows.empty()?next_uid():rows[0].at("uid").get<std::uint32_t>();
    const auto created=rows.empty()?static_cast<std::uint32_t>(now()):rows[0].at("created").get<std::uint32_t>();
    Wallet wallet(db_,*this);
    if(!wallet.apply(eid,-delta,"storage-carats:"+event).second)
        throw std::runtime_error("Operacao de Carats duplicada");
    db_.exec("INSERT INTO storage_carats VALUES(?,?,?,?,?) ON CONFLICT(owner_key) DO UPDATE "
             "SET amount=excluded.amount,slot=excluded.slot",
             {key,amount,std::int64_t(target),std::int64_t(uid),std::int64_t(created)});
    db_.exec("INSERT INTO storage_carats_events VALUES(?,?,?,?)",{event,key,std::int64_t(eid),delta});
    tx.commit();
    return amount;
}
std::int64_t Inventory::guild_storage_membership_id(const std::string &account,std::uint32_t eid) {
    storage_character(db_,account,eid);
    return guild_membership(db_,account,eid).at("guild_id").get<std::int64_t>();
}
std::int64_t Inventory::guild_departure_latest() {
    return db_.query("SELECT COALESCE(MAX(id),0) AS id FROM native_guild_departures")[0].at("id");
}
void Inventory::guild_depart(const std::string&account,std::uint32_t eid,std::uint32_t target,
                              const std::string&event,double time) {
    if(!target||event.empty()||event.size()>128)throw std::runtime_error("Invalid guild departure");
    Transaction tx(db_);storage_character(db_,account,eid);
    const auto replay=db_.query("SELECT account,actor,target FROM native_guild_departures WHERE event_key=?",{event});
    if(!replay.empty()){
        if(replay[0].at("account")!=account||replay[0].at("actor")!=eid||replay[0].at("target")!=target)
            throw std::runtime_error("Guild departure replay mismatch");
        tx.commit();return;
    }
    const auto member=guild_membership(db_,account,eid);
    const auto guild=member.at("guild_id").get<std::int64_t>();const int rank=member.at("rank");
    const auto victim=db_.query("SELECT rank FROM native_guild_members WHERE guild_id=? AND eid=?",{guild,std::int64_t(target)});
    if(victim.empty())throw std::runtime_error("Character is not in your guild");
    if(victim[0].at("rank")==5)throw std::runtime_error("Transfer guild leadership before leaving");
    if(target!=eid&&(rank<4||rank<=victim[0].at("rank").get<int>()))throw std::runtime_error("Insufficient guild rank");
    db_.exec("INSERT INTO native_guild_departures(event_key,account,actor,target,guild_id,created) VALUES(?,?,?,?,?,?)",
             {event,account,std::int64_t(eid),std::int64_t(target),guild,time});
    const auto id=db_.query("SELECT last_insert_rowid() AS id")[0].at("id").get<std::int64_t>();
    db_.exec("INSERT INTO native_guild_departure_recipients SELECT ?,eid,account_id FROM native_guild_members WHERE guild_id=?",{id,guild});
    db_.exec("DELETE FROM native_guild_members WHERE eid=? AND guild_id=?",{std::int64_t(target),guild});
    db_.exec("DELETE FROM native_guild_invites WHERE inviter=? OR (target=? AND guild_id=?)",{std::int64_t(target),std::int64_t(target),guild});
    // Leaving revokes queued chat, even if the same character rejoins before polling.
    db_.exec("DELETE FROM native_guild_chat_recipients WHERE eid=?",{std::int64_t(target)});
    db_.exec("DELETE FROM native_guild_departure_recipients WHERE event_id IN (SELECT id FROM native_guild_departures WHERE created<?)",{time-300});
    db_.exec("DELETE FROM native_guild_departures WHERE created<?",{time-300});
    tx.commit();
}
Json Inventory::guild_departure_poll(const std::string&account,std::uint32_t eid,std::int64_t after,double time) {
    return db_.query("SELECT e.id,e.actor,e.target FROM native_guild_departures e "
                    "JOIN native_guild_departure_recipients r ON r.event_id=e.id "
                    "WHERE r.eid=? AND r.account=? AND e.id>? AND e.created>=? AND "
                    "((e.target=r.eid AND NOT EXISTS(SELECT 1 FROM native_guild_members m WHERE m.eid=r.eid)) OR "
                    "(e.target!=r.eid AND EXISTS(SELECT 1 FROM native_guild_members m WHERE m.eid=r.eid AND m.account_id=r.account AND m.guild_id=e.guild_id))) "
                    "ORDER BY e.id LIMIT 64",{std::int64_t(eid),account,after,time-300});
}
namespace {
std::optional<Json> guild_wire_replay(Sqlite&db,const std::string&event,std::uint32_t eid,const std::string&kind,const Json&request){
    if(event.empty()||event.size()>128)throw std::runtime_error("Invalid guild event");
    const auto rows=db.query("SELECT eid,kind,request,result FROM inventory_operations WHERE event_key=?",{event});
    if(rows.empty())return {};
    if(rows[0].at("eid")!=eid||rows[0].at("kind")!=kind||rows[0].at("request")!=request.dump())
        throw std::runtime_error("Guild invitation replay mismatch");
    return Json::parse(rows[0].at("result").get<std::string>());
}
void guild_wire_record(Sqlite&db,const std::string&event,std::uint32_t eid,const std::string&kind,const Json&request,const Json&result){
    db.exec("INSERT INTO inventory_operations VALUES(?,?,?,?,?)",{event,std::int64_t(eid),kind,request.dump(),result.dump()});
}
}
Json Inventory::guild_native_invite(const std::string&account,std::uint32_t eid,std::uint32_t target,
                                   const std::string&name,const std::string&event,double time){
    Transaction tx(db_);storage_character(db_,account,eid);
    const Json request={{"account",account},{"target",target},{"name",name}};
    if(const auto replay=guild_wire_replay(db_,event,eid,"guild-invite",request)){tx.commit();return *replay;}
    if(name.empty()||name.size()>128||name.find('\0')!=std::string::npos||wide(name).size()>32)
        throw std::runtime_error("Invalid character name");
    const auto named=db_.query("SELECT character_eid FROM native_public_character_names WHERE name=? COLLATE NOCASE LIMIT 2",{name});
    if(named.size()!=1)throw std::runtime_error("Character is unavailable");
    const auto resolved=named[0].at("character_eid").get<std::int64_t>();
    if(target&&target!=resolved)throw std::runtime_error("Invitation target mismatch");
    if(!db_.query("SELECT 1 FROM native_guild_invite_wire w JOIN native_guild_invites i ON i.token=w.token "
                  "WHERE i.expires>? AND (i.inviter=? OR i.target=? OR i.inviter=? OR i.target=?)",{time,std::int64_t(eid),std::int64_t(eid),resolved,resolved}).empty())
        throw std::runtime_error("A guild invitation is already pending");
    db_.exec("DELETE FROM native_guild_invite_wire WHERE token NOT IN (SELECT token FROM native_guild_invites WHERE expires>?)",{time});
    const auto token=guild_invite(account,eid,name,time);
    const auto rows=db_.query("SELECT i.token,i.inviter,i.target,i.target_account,i.guild_id,g.name AS guild_name,"
                             "a.name AS inviter_name,b.name AS target_name FROM native_guild_invites i "
                             "JOIN native_guilds g ON g.id=i.guild_id "
                             "JOIN native_public_character_names a ON a.character_eid=i.inviter "
                             "JOIN native_public_character_names b ON b.character_eid=i.target WHERE i.token=?",{token});
    if(rows.size()!=1||(target&&rows[0].at("target")!=target))throw std::runtime_error("Invitation target mismatch");
    const auto result=rows[0];
    db_.exec("INSERT INTO native_guild_invite_wire VALUES(?)",{token});
    guild_wire_record(db_,event,eid,"guild-invite",request,result);
    tx.commit();return result;
}
Json Inventory::guild_native_answer(const std::string&account,std::uint32_t eid,std::uint32_t inviter,
                                   bool accept,const std::string&event,double time){
    Transaction tx(db_);storage_character(db_,account,eid);
    const Json request={{"account",account},{"inviter",inviter},{"accept",accept}};
    if(const auto replay=guild_wire_replay(db_,event,eid,"guild-answer",request)){tx.commit();return *replay;}
    const auto rows=db_.query("SELECT i.token,i.inviter,i.target,i.target_account,i.guild_id,g.name AS guild_name,"
                             "a.account_id AS inviter_account,a.name AS inviter_name,b.name AS target_name "
                             "FROM native_guild_invites i JOIN native_guild_invite_wire w ON w.token=i.token "
                             "JOIN native_guilds g ON g.id=i.guild_id "
                             "JOIN native_public_character_names a ON a.character_eid=i.inviter "
                             "JOIN native_public_character_names b ON b.character_eid=i.target "
                             "WHERE i.inviter=? AND i.target=? AND i.target_account=? AND i.expires>? ORDER BY i.expires DESC LIMIT 1",
                             {std::int64_t(inviter),std::int64_t(eid),account,time});
    if(rows.empty())throw std::runtime_error("Guild invitation is absent or expired");
    auto result=rows[0];guild_invite_answer(account,eid,result.at("token"),accept,time);
    const auto status=std::int64_t(accept?0xdd:0xdc);result["status"]=status;
    db_.exec("INSERT INTO native_guild_invite_results(event_key,inviter,target,inviter_account,target_account,status,guild_id,guild_name,inviter_name,target_name,created) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
             {event,std::int64_t(inviter),std::int64_t(eid),result.at("inviter_account").get<std::string>(),account,status,
              result.at("guild_id").get<std::int64_t>(),result.at("guild_name").get<std::string>(),
              result.at("inviter_name").get<std::string>(),result.at("target_name").get<std::string>(),time});
    db_.exec("DELETE FROM native_guild_invite_wire WHERE token NOT IN (SELECT token FROM native_guild_invites)");
    db_.exec("DELETE FROM native_guild_invite_results WHERE created<?",{time-300});
    guild_wire_record(db_,event,eid,"guild-answer",request,result);
    tx.commit();return result;
}
std::int64_t Inventory::guild_native_result_latest(){
    return db_.query("SELECT COALESCE(MAX(id),0) AS id FROM native_guild_invite_results")[0].at("id");
}
Json Inventory::guild_native_results(const std::string&account,std::uint32_t eid,std::int64_t after,double time){
    return db_.query("SELECT * FROM native_guild_invite_results WHERE id>? AND created>=? AND "
                    "((inviter=? AND inviter_account=?) OR (target=? AND target_account=?)) ORDER BY id LIMIT 64",
                    {after,time-300,std::int64_t(eid),account,std::int64_t(eid),account});
}
std::int64_t Inventory::guild_chat_latest() {
    return db_.query("SELECT COALESCE(MAX(id),0) AS id FROM native_guild_chat")[0].at("id");
}
void Inventory::guild_chat_post(const std::string&account,std::uint32_t eid,const std::string&sender,
                                const std::string&message,const std::string&event,double time) {
    if(event.empty()||event.size()>128||sender.empty()||sender.size()>128||message.empty()||message.size()>440||
       message.find('\0')!=std::string::npos||sender.find('\0')!=std::string::npos)
        throw std::runtime_error("Invalid guild chat");
    wide(message);wide(sender);
    Transaction tx(db_);
    storage_character(db_,account,eid);
    const auto guild=guild_membership(db_,account,eid).at("guild_id").get<std::int64_t>();
    const auto old=db_.query("SELECT account,eid,sender,message FROM native_guild_chat WHERE event_key=?",{event});
    if(!old.empty()) {
        if(old[0].at("account")!=account||old[0].at("eid")!=eid||old[0].at("sender")!=sender||old[0].at("message")!=message)
            throw std::runtime_error("Guild chat replay mismatch");
        tx.commit();return;
    }
    db_.exec("INSERT INTO native_guild_chat(event_key,account,eid,guild_id,sender,message,created) VALUES(?,?,?,?,?,?,?)",
             {event,account,std::int64_t(eid),guild,sender,message,time});
    const auto id=db_.query("SELECT last_insert_rowid() AS id")[0].at("id").get<std::int64_t>();
    // Snapshot recipients so joining later cannot expose an earlier conversation.
    db_.exec("INSERT INTO native_guild_chat_recipients SELECT ?,eid,account_id FROM native_guild_members WHERE guild_id=?",{id,guild});
    // This is a delivery queue, not permanent chat history. Keep five minutes for live polling.
    db_.exec("DELETE FROM native_guild_chat_recipients WHERE message_id IN (SELECT id FROM native_guild_chat WHERE created<?)",{time-300});
    db_.exec("DELETE FROM native_guild_chat WHERE created<?",{time-300});
    tx.commit();
}
Json Inventory::guild_chat_poll(const std::string&account,std::uint32_t eid,std::int64_t after,double time) {
    return db_.query("SELECT c.id,c.eid,c.sender,c.message FROM native_guild_chat c "
                    "JOIN native_guild_chat_recipients r ON r.message_id=c.id "
                    "JOIN native_guild_members m ON m.eid=r.eid AND m.account_id=r.account AND m.guild_id=c.guild_id "
                    "WHERE r.eid=? AND r.account=? AND c.id>? AND c.created>=? ORDER BY c.id LIMIT 64",
                    {std::int64_t(eid),account,after,time-300});
}
std::int64_t Inventory::guild_create(const std::string &account,std::uint32_t eid,const std::string &name) {
    if (name.size()<3||name.size()>31||name.front()==' '||name.back()==' '||
        std::any_of(name.begin(),name.end(),[](unsigned char c){return c<32||c==127;}))
        throw std::runtime_error("Nome de cla invalido");
    wide(name);
    Transaction tx(db_);
    const auto character=storage_character(db_,account,eid);
    if (character.value("level",0)<10) throw std::runtime_error("Nivel insuficiente para fundar um cla");
    if (!db_.query("SELECT 1 FROM native_guild_members WHERE eid=?",{std::int64_t(eid)}).empty())
        throw std::runtime_error("O personagem ja pertence a um cla");
    if (!db_.query("SELECT 1 FROM native_guilds WHERE name=?",{name}).empty())
        throw std::runtime_error("Este nome de cla ja esta em uso");
    Wallet wallet(db_,*this);
    constexpr std::int64_t cost=50000;
    if (wallet.balance(eid)<=cost) throw std::runtime_error("Voce precisa de mais de 50.000 Carats");
    db_.exec("INSERT INTO native_guilds(name) VALUES(?)",{name});
    const auto guild=db_.query("SELECT last_insert_rowid() AS id").front().at("id").get<std::int64_t>();
    wallet.apply(eid,-cost,"guild-create:"+std::to_string(guild));
    db_.exec("INSERT INTO native_guild_members VALUES(?,?,?,5)",{std::int64_t(eid),account,guild});
    tx.commit();
    return guild;
}
std::string Inventory::guild_invite(const std::string &account,std::uint32_t eid,const std::string &name,double time) {
    Transaction tx(db_);
    storage_character(db_,account,eid);
    const auto member=guild_membership(db_,account,eid);
    if (member.at("rank").get<int>()<4) throw std::runtime_error("Seu cargo nao pode convidar");
    auto matches=db_.query("SELECT character_eid,account_id FROM native_public_character_names "
                           "WHERE name=? COLLATE NOCASE LIMIT 2",{name});
    if (matches.size()!=1) throw std::runtime_error("Personagem indisponivel");
    const auto target=matches[0].at("character_eid").get<std::uint32_t>();
    const auto targetAccount=matches[0].at("account_id").get<std::string>();
    const auto character=storage_character(db_,targetAccount,target);
    if (_stricmp(character.value("name","").c_str(),name.c_str())!=0)
        throw std::runtime_error("Personagem indisponivel");
    if (!db_.query("SELECT 1 FROM native_guild_members WHERE eid=?",{std::int64_t(target)}).empty())
        throw std::runtime_error("O personagem ja pertence a um cla");
    const auto guild=member.at("guild_id").get<std::int64_t>();
    if (db_.query("SELECT COUNT(*) AS count FROM native_guild_members WHERE guild_id=?",{guild})
            .front().at("count").get<int>()>=50)
        throw std::runtime_error("O cla esta cheio");
    db_.exec("DELETE FROM native_guild_invites WHERE target=? AND expires<=?",{std::int64_t(target),time});
    auto invite=token(16);
    db_.exec("INSERT INTO native_guild_invites VALUES(?,?,?,?,?,?) ON CONFLICT(guild_id,target) "
             "DO UPDATE SET token=excluded.token,inviter=excluded.inviter,target_account=excluded.target_account,expires=excluded.expires",
             {invite,guild,std::int64_t(eid),std::int64_t(target),targetAccount,time+60.});
    tx.commit();
    return invite;
}
bool Inventory::guild_invite_answer(const std::string &account,std::uint32_t eid,const std::string &invite,
                                    bool accept,double time) {
    Transaction tx(db_);
    storage_character(db_,account,eid);
    const auto rows=db_.query("SELECT i.guild_id FROM native_guild_invites i JOIN native_guild_members m "
                              "ON m.eid=i.inviter AND m.guild_id=i.guild_id AND m.rank>=4 "
                              "JOIN native_guilds g ON g.id=i.guild_id "
                              "WHERE i.token=? AND i.target=? AND i.target_account=? AND i.expires>?",
                              {invite,std::int64_t(eid),account,time});
    if (rows.empty()) throw std::runtime_error("Convite indisponivel");
    if (accept) {
        const auto guild=rows[0].at("guild_id").get<std::int64_t>();
        if (db_.query("SELECT COUNT(*) AS count FROM native_guild_members WHERE guild_id=?",{guild})
                .front().at("count").get<int>()>=50)
            throw std::runtime_error("O cla esta cheio");
        // A new membership must not receive a delayed departure from its previous membership.
        db_.exec("DELETE FROM native_guild_departure_recipients WHERE eid=?",{std::int64_t(eid)});
        db_.exec("INSERT INTO native_guild_members VALUES(?,?,?,1)",{std::int64_t(eid),account,guild});
        db_.exec("DELETE FROM native_guild_invites WHERE target=? AND target_account=?",{std::int64_t(eid),account});
    } else db_.exec("DELETE FROM native_guild_invites WHERE token=?",{invite});
    tx.commit();
    return accept;
}
void Inventory::guild_change_rank(const std::string &account,std::uint32_t eid,std::uint32_t target,int rank) {
    if (rank<0||rank>5||target==eid) throw std::runtime_error("Cargo invalido");
    Transaction tx(db_);
    storage_character(db_,account,eid);
    const auto member=guild_membership(db_,account,eid);
    const auto guild=member.at("guild_id").get<std::int64_t>();
    if (member.at("rank")!=5||
        db_.query("SELECT 1 FROM native_guild_members WHERE eid=? AND guild_id=?",
                  {std::int64_t(target),guild}).empty())
        throw std::runtime_error("Apenas o lider pode alterar cargos do cla");
    if (rank==5) db_.exec("UPDATE native_guild_members SET rank=4 WHERE eid=?",{std::int64_t(eid)});
    db_.exec("UPDATE native_guild_members SET rank=? WHERE eid=? AND guild_id=?",
             {std::int64_t(rank),std::int64_t(target),guild});
    tx.commit();
}

std::vector<ItemInstance> Inventory::storage_items(const std::string &account,std::uint32_t eid,
                                                    PersonalStorage kind,double time) {
    Transaction tx(db_);
    const auto key=storage_key(db_,account,eid,kind,StorageRight::View);
    archive_storage_expiry(db_,key,time);
    std::vector<ItemInstance> items;
    for (const auto &row:db_.query(std::string(storage_select)+"WHERE s.owner_key=? ORDER BY s.slot",{key}))
        items.push_back(storage_item(row));
    auto coins=db_.query("SELECT slot,amount,uid,created FROM storage_carats WHERE owner_key=? AND amount>0",{key});
    if(!coins.empty()){
        const auto &row=coins[0];
        ItemInstance item{};
        item.item=999999;item.slot=row.at("slot");item.quantity=row.at("amount");
        item.uid=row.at("uid");item.created=row.at("created");
        items.push_back(item);
    }
    tx.commit();
    return items;
}
Bytes Inventory::storage_snapshot(const std::string &account,std::uint32_t eid,PersonalStorage kind,double time) {
    const auto items=storage_items(account,eid,kind,time);
    Bytes reply(22);
    put32(reply,0,eid,true);
    put32(reply,12,storage_capacity(kind),true);
    put32(reply,16,static_cast<std::uint32_t>(items.size()),true);
    put16(reply,20,0xc6,true);
    for (auto item:items) {
        const auto storageSlot=item.slot;
        item.slot=0; // The inventory encoder supplies the shared instance and affix fields.
        auto record=update(eid,item);
        const auto offset=reply.size();
        reply.insert(reply.end(),record.begin()+6,record.end());
        put32(reply,offset,item.item);
        put16(reply,offset+4,storageSlot);
        reply[offset+6]=static_cast<unsigned char>(item.grade);
    }
    return reply;
}

std::vector<std::pair<int,std::int64_t>> Inventory::storage_deposit(
    const std::string &account,std::uint32_t eid,PersonalStorage kind,
    int from,int to,std::uint32_t iid,std::uint32_t uid,double time,
    std::optional<std::int64_t> quantity) {
    check_storage_slot(kind,to);
    Transaction tx(db_);
    const auto key=storage_key(db_,account,eid,kind,StorageRight::Deposit);
    archive_storage_expiry(db_,key,time);
    const auto item=owned(eid,from,iid,time);
    const auto rule=definition(iid);
    const auto count=quantity.value_or(item.quantity);
    if (item.uid!=uid||iid==999999||rule.empty()||rule.value("kind",0)==4)
        throw std::runtime_error("Item indisponivel para deposito");
    if (count<1||count>item.quantity||((item.expires||item.grade)&&count!=item.quantity))
        throw std::runtime_error("Quantidade invalida para deposito");

    const auto limit=std::clamp(rule.value("stack",1),1,10000);
    auto remaining=count;
    std::set<int> occupied;
    for(const auto &row:db_.query("SELECT slot FROM storage_carats WHERE owner_key=? AND amount>0",{key}))
        occupied.insert(row.at("slot").get<int>());
    std::vector<std::pair<int,std::int64_t>> placements;
    for (const auto &row:db_.query(std::string(storage_select)+"WHERE s.owner_key=? ORDER BY s.slot",{key})) {
        const auto target=storage_item(row);
        occupied.insert(target.slot);
        if (remaining&&limit>1&&iid==target.item&&target.quantity<limit&&
            !item.expires&&!target.expires&&!item.grade&&!target.grade&&!traced(uid)&&!traced(target.uid)) {
            const auto moved=std::min(remaining,limit-target.quantity);
            db_.exec("UPDATE personal_storage SET quantity=quantity+? WHERE owner_key=? AND slot=?",
                     {moved,key,std::int64_t(target.slot)});
            placements.emplace_back(target.slot,moved);
            remaining-=moved;
        }
    }
    if (remaining>limit) throw std::runtime_error("Quantidade excede o limite da pilha");
    if (remaining&&occupied.count(to)) throw std::runtime_error("Slot do deposito ocupado");
    if (count==item.quantity) detach(eid,from,iid,uid,time);
    else db_.exec("UPDATE inventory_slots SET quantity=quantity-? WHERE player_eid=? AND slot=?",
                  {count,std::int64_t(eid),std::int64_t(from)});
    if (remaining) {
        auto moved=item;
        moved.slot=to;moved.quantity=remaining;
        if (count!=item.quantity) moved.uid=next_uid();
        db_.exec("INSERT INTO personal_storage VALUES(?,?,?,?,?,?,?)",
                 {key,std::int64_t(to),std::int64_t(iid),remaining,std::int64_t(moved.uid),
                  std::int64_t(moved.created),std::int64_t(eid)});
        if (moved.uid!=uid) trace_derivation(uid,moved,eid);
        placements.emplace_back(to,remaining);
    }
    for (const auto &[slot,moved]:placements)
        trace_item(uid,"storage-deposit",eid,moved,from,slot);
    tx.commit();
    return placements;
}

int Inventory::storage_withdraw(const std::string &account,std::uint32_t eid,PersonalStorage kind,
                                int from,std::optional<int> to,std::uint32_t uid,double time) {
    check_storage_slot(kind,from);
    Transaction tx(db_);
    const auto key=storage_key(db_,account,eid,kind,StorageRight::Withdraw);
    auto rows=db_.query(std::string(storage_select)+"WHERE s.owner_key=? AND s.slot=? AND s.uid=?",
                        {key,std::int64_t(from),std::int64_t(uid)});
    if (rows.empty()) throw std::runtime_error("Item indisponivel no deposito");
    const auto item=storage_item(rows[0]);
    db_.exec("DELETE FROM personal_storage WHERE owner_key=? AND slot=?",{key,std::int64_t(from)});
    auto destination=restore(eid,item,time,to);
    trace_item(uid,"storage-withdraw",eid,item.quantity,from,destination);
    tx.commit();
    return destination;
}
ItemInstance Inventory::detach(std::uint32_t eid,int slot,std::uint32_t iid,std::uint32_t uid,double time,
                               std::optional<std::int64_t> quantity) {
    if (sqlite3_get_autocommit(db_.handle())) throw std::logic_error("Transfer requires a transaction");
    auto item=owned(eid,slot,iid,time);
    if (item.item==999999||item.uid!=uid) throw std::runtime_error("Item indisponivel para transferencia");
    const auto count=quantity.value_or(item.quantity);
    if (count<1||count>item.quantity) throw std::runtime_error("Quantidade invalida para transferencia");
    if (count==item.quantity) {
        db_.exec("DELETE FROM inventory_slots WHERE player_eid=? AND slot=? AND instance_uid=?",
                 {std::int64_t(eid),std::int64_t(slot),std::int64_t(uid)});
        return item;
    }

    if (item.expires||item.grade||definition(iid).value("stack",1)<item.quantity)
        throw std::runtime_error("Esta pilha nao pode ser dividida");
    db_.exec("UPDATE inventory_slots SET quantity=quantity-? WHERE player_eid=? AND slot=? AND instance_uid=?",
             {count,std::int64_t(eid),std::int64_t(slot),std::int64_t(uid)});
    item.quantity=count;
    item.uid=next_uid();
    trace_derivation(uid,item,eid);
    return item;
}
int Inventory::restore(std::uint32_t eid,const ItemInstance &item,double time,std::optional<int> slot) {
    if (sqlite3_get_autocommit(db_.handle())) throw std::logic_error("Transfer requires a transaction");
    if (!item.uid||!item.item||item.item==999999||item.quantity<1||item.quantity>10000||
        (item.expires&&*item.expires<=time)) throw std::runtime_error("Item indisponivel para transferencia");
    if (!db_.query("SELECT 1 FROM inventory_slots WHERE instance_uid=? UNION ALL "
                   "SELECT 1 FROM equipment_instances WHERE instance_uid=? UNION ALL "
                   "SELECT 1 FROM personal_storage WHERE uid=? LIMIT 1",
                   {std::int64_t(item.uid),std::int64_t(item.uid),std::int64_t(item.uid)}).empty())
        throw std::runtime_error("Item ja esta no inventario");
    auto free=available(eid,time);
    for (const auto &ownedItem:items(eid)) free.erase(ownedItem.slot);
    if (free.empty()||(slot&&!free.count(*slot))) throw InventoryFull();
    const auto destination=slot.value_or(*free.begin());
    insert_row(db_,eid,destination,item);
    db_.exec("UPDATE reward_instances SET player_eid=? WHERE uid=?",
             {std::int64_t(eid),std::int64_t(item.uid)});
    return destination;
}
std::optional<ConsignmentCategory> consignment_category(const Json &definition) {
    if (definition.value("kind",0)!=1) return std::nullopt;
    const int slot=definition.value("slot",0);
    switch (definition.value("subtype",0)) {
    case 4: // Fletta weapon
        if (slot==7) return ConsignmentCategory::WeaponOther;
        break;
    case 5: // Fletta armor
        if (slot==1) return ConsignmentCategory::ArmorTorso;
        break;
    case 6: // Mount
        if (slot==6) return ConsignmentCategory::MountShoulders;
        break;
    case 68: // Wings
        if (slot==30) return ConsignmentCategory::MountShoulders;
        break;
    }
    return std::nullopt;
}
Json consignment_search(Sqlite &db, const Inventory &inventory, const ConsignmentSearch &filter,
                        double time, std::uint32_t page) {
    if (!page||filter.min_level<0||filter.max_level<filter.min_level||
        filter.min_enhancement<0||filter.max_enhancement<filter.min_enhancement)
        throw std::runtime_error("Filtro de consignacao invalido");
    auto active=db.query("SELECT c.id,c.seller,c.item_id,c.uid,c.quantity,c.item_created,c.price,c.expires,"
                         "COALESCE(e.grade,0) AS enhancement FROM consignment c "
                         "LEFT JOIN item_enchantments e ON e.uid=c.uid "
                         "WHERE c.status='listed' AND c.expires>? AND (?=0 OR c.item_id=?) "
                         "AND (?=0 OR c.seller=?) AND COALESCE(e.grade,0) BETWEEN ? AND ? ORDER BY c.id",
                         {time,std::int64_t(filter.item),std::int64_t(filter.item),
                          std::int64_t(filter.seller),std::int64_t(filter.seller),
                          std::int64_t(filter.min_enhancement),std::int64_t(filter.max_enhancement)});
    active.erase(std::remove_if(active.begin(),active.end(),[&](Json &row) {
        const auto definition=inventory.definition(row.at("item_id"));
        if (definition.empty()) return true;
        const auto type=std::pair{definition.value("kind",0),definition.value("subtype",0)};
        const bool catalog_match=filter.catalog_type&&type==*filter.catalog_type&&
                                 (!filter.catalog_slot||definition.value("slot",0)==*filter.catalog_slot);
        const bool category_match=filter.category&&consignment_category(definition)==filter.category;
        if ((filter.catalog_type||filter.category)&&!catalog_match&&!category_match) return true;
        const auto level=definition.value("level",0);
        if (level>0&&(level<filter.min_level||level>filter.max_level)) return true;
        row["level"]=level;
        return false;
    }),active.end());
    const char *order=filter.order==ConsignmentOrder::Level?"level":
                      filter.order==ConsignmentOrder::Price?"price":"id";
    std::stable_sort(active.begin(),active.end(),[&](const Json &a,const Json &b){
        return filter.descending?a.at(order)>b.at(order):a.at(order)<b.at(order);
    });
    constexpr std::uint64_t page_size=10;
    const auto offset=std::min((std::uint64_t(page)-1)*page_size,std::uint64_t(active.size()));
    const auto end=std::min(offset+page_size,std::uint64_t(active.size()));
    return {{"page",page},{"total",active.size()},
            {"rows",Json(active.begin()+offset,active.begin()+end)}};
}
Bytes native_consignment_search(Sqlite &db, Inventory &inventory, const Bytes &request, double time,
                                std::map<std::uint32_t,std::int64_t> *quotes) {
    if (quotes) quotes->clear();
    if (request.size()!=52||get16(request,50,true)!=0xcb)
        throw std::runtime_error("Pedido de consignacao invalido");
    const auto mode=get32(request,4,true);
    const auto category=get32(request,8,true);
    const auto subcategory=get32(request,12,true);
    const auto page=std::max(1u,get32(request,38,true));
    ConsignmentSearch filter;
    if (mode==1) {
        filter.item=get32(request,34,true);
        if (!filter.item) throw std::runtime_error("Item de busca invalido");
    } else if (mode!=0) {
        throw std::runtime_error("Modo de busca indisponivel");
    } else if ((category==110&&subcategory==0)||(category==100&&subcategory==110)) {
        filter.category=ConsignmentCategory::WeaponOther;
    } else if ((category==202&&subcategory==0)||(category==200&&subcategory==202)) {
        filter.category=ConsignmentCategory::ArmorTorso;
        filter.catalog_type={{1,2}};
        filter.catalog_slot=1;
    } else if ((category==205&&subcategory==0)||(category==200&&subcategory==205)) {
        filter.category=ConsignmentCategory::MountShoulders;
        filter.catalog_type={{1,2}};
        filter.catalog_slot=3;
    } else if (category==200&&(subcategory==0||subcategory==200)) {
        filter.catalog_type={{1,2}};
    } else if (category==402&&subcategory==0) {
        filter.catalog_type={{2,9}};
    } else if (category==403&&subcategory==0) {
        filter.catalog_type={{2,8}};
    } else {
        return native_consignment_page(db,inventory,request,
                                        {{"page",page},{"total",0},{"rows",Json::array()}});
    }
    filter.min_level=std::min(255u,get32(request,16,true));
    filter.max_level=std::min(255u,get32(request,20,true));
    filter.min_enhancement=std::min(127u,get32(request,24,true));
    filter.max_enhancement=std::min(127u,get32(request,28,true));
    filter.order=request[32]==2?ConsignmentOrder::Level:
                 request[32]==3?ConsignmentOrder::Price:ConsignmentOrder::Listing;
    filter.descending=request[33]==1;
    const auto result=consignment_search(db,inventory,filter,time,page);
    auto response=native_consignment_page(db,inventory,request,result);
    if (quotes) {
        for (const auto &row:result.at("rows"))
            quotes->emplace(row.at("id").get<std::uint32_t>(),row.at("price").get<std::int64_t>());
    }
    return response;
}

Bytes native_consignment_seller_page(Sqlite &db, Inventory &inventory, std::uint32_t seller, double time) {
    if (!seller) throw std::runtime_error("Vendedor indisponivel");
    consignment_expire(db,seller,time);
    ConsignmentSearch filter;
    filter.seller=seller;
    filter.order=ConsignmentOrder::Listing;
    Bytes request(52);
    put16(request,50,0xcb,true);
    return native_consignment_page(db,inventory,request,consignment_search(db,inventory,filter,time,1));
}

Bytes native_consignment_page(Sqlite &db, Inventory &inventory, const Bytes &request, const Json &page) {
    const auto &rows=page.at("rows");
    if (request.size()!=52||get16(request,50,true)!=0xcb||rows.size()>10)
        throw std::runtime_error("Pagina de consignacao invalida");

    Bytes response=request;
    put32(response,38,page.at("page"),true);
    put32(response,42,static_cast<std::uint32_t>(rows.size()),true);
    put32(response,46,page.at("total"),true);
    const auto put64be=[](Bytes &bytes,std::size_t offset,std::uint64_t value){
        put32(bytes,offset,static_cast<std::uint32_t>(value>>32),true);
        put32(bytes,offset+4,static_cast<std::uint32_t>(value),true);
    };
    for (const auto &row:rows) {
        const auto seller=row.at("seller").get<std::uint32_t>();
        const auto names=db.query("SELECT json_extract(c.value,'$.name') AS name FROM accounts a,"
                                  "json_each(a.payload,'$.chars') c WHERE json_extract(c.value,'$.eid')=? LIMIT 2",
                                  {std::int64_t(seller)});
        if (names.size()!=1||!names.at(0).at("name").is_string())
            throw std::runtime_error("Vendedor da consignacao indisponivel");
        const auto name=names.at(0).at("name").get<std::string>();
        if (name.empty()||name.size()>39||name.find(char(0))!=std::string::npos)
            throw std::runtime_error("Nome do vendedor invalido");
        const auto id=row.at("id").get<std::uint64_t>();
        if (!id||id>std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("Identificador de anuncio invalido");

        ItemInstance item{};
        item.item=row.at("item_id");
        item.uid=row.at("uid");
        item.grade=row.at("enhancement");
        item.quantity=row.at("quantity");
        item.created=row.at("item_created");
        const auto record=inventory.update(seller,item);
        const auto start=response.size();
        response.resize(start+60);
        put32(response,start,static_cast<std::uint32_t>(id),true);
        put32(response,start+4,item.item,true);
        put32(response,start+8,item.grade,true);
        const auto price=row.at("price").get<std::uint64_t>();
        put64be(response,start+12,price/static_cast<std::uint64_t>(item.quantity));
        put64be(response,start+20,item.quantity);
        put64be(response,start+52,price);
        response.insert(response.end(),name.begin(),name.end());
        response.push_back(0);
        const auto tail=response.size();
        response.resize(tail+95);
        put32(response,tail,static_cast<std::uint32_t>(row.at("expires").get<double>()),true);
        for (std::size_t i=0;i<10;++i)
            put16(response,tail+73+2*i,get16(record,130+2*i),true);
    }
    return response;
}
void consignment_schema(Sqlite &db) {
    db.exec(R"SQL(
CREATE TABLE IF NOT EXISTS consignment(
 id INTEGER PRIMARY KEY AUTOINCREMENT, seller INTEGER NOT NULL, buyer INTEGER,
 item_id INTEGER NOT NULL, uid INTEGER NOT NULL, quantity INTEGER NOT NULL,
 item_created INTEGER NOT NULL, price INTEGER NOT NULL CHECK(price>0),
 expires REAL NOT NULL, status TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS consignment_seller_expiry ON consignment(seller,status,expires);
CREATE UNIQUE INDEX IF NOT EXISTS consignment_active_uid ON consignment(uid) WHERE status='listed';
CREATE TABLE IF NOT EXISTS consignment_registration(
 event TEXT PRIMARY KEY, seller INTEGER NOT NULL, request TEXT NOT NULL);
)SQL");
}
std::int64_t consignment_list(Sqlite &db, Inventory &inventory, std::uint32_t seller, int slot,
                             std::uint32_t iid, std::uint32_t uid, std::int64_t price, double time,
                             std::optional<std::int64_t> quantity) {
    if (price<1||price>1000000000000LL) throw std::runtime_error("Preco invalido");
    Transaction tx(db);
    const auto active=db.query("SELECT COUNT(*) AS count FROM consignment "
                               "WHERE seller=? AND status='listed' AND expires>?",
                               {std::int64_t(seller),time});
    if (active.at(0).at("count").get<int>()>=10)
        throw std::runtime_error("Limite de 10 anuncios ativos atingido");
    auto rule=inventory.definition(iid);
    const auto item=inventory.detach(seller,slot,iid,uid,time,quantity);
    if (rule.empty()||rule.value("kind",0)==4||item.expires)
        throw std::runtime_error("Item indisponivel para consignacao");
    db.exec("INSERT INTO consignment(seller,item_id,uid,quantity,item_created,price,expires,status) "
            "VALUES(?,?,?,?,?,?,?,'listed')",
            {std::int64_t(seller),std::int64_t(iid),std::int64_t(item.uid),item.quantity,
             std::int64_t(item.created),price,time+86400.});
    const auto listing=db.query("SELECT last_insert_rowid() AS id")[0].at("id").get<std::int64_t>();
    inventory.trace_item(item.uid,"consignment-list",seller,item.quantity,slot,-1);
    tx.commit();
    return listing;
}
void native_consignment_register(Sqlite &db, Inventory &inventory, std::uint32_t seller,
                                 const Bytes &request, const std::string &event, double time) {
    if (request.size()!=26||get16(request,24,true)!=0||get32(request,0,true)!=0||event.empty())
        throw std::runtime_error("Pedido de anuncio invalido");
    const auto bag=get32(request,4,true);
    const auto cell=get32(request,8,true);
    const auto count=get32(request,12,true);
    const auto unit_price=(std::uint64_t(get32(request,16,true))<<32)|get32(request,20,true);
    if (bag<1||bag>4||cell>47||!count||count>10000||!unit_price||unit_price>1000000000000ULL/count)
        throw std::runtime_error("Quantidade, preco ou bolsa invalida");
    const auto slot=Inventory::key(static_cast<int>(bag),static_cast<int>(cell));

    Transaction tx(db);
    const auto prior=db.query("SELECT seller,request FROM consignment_registration WHERE event=?",{event});
    if (!prior.empty()) {
        if (prior[0].at("seller")!=seller||prior[0].at("request")!=hex(request))
            throw std::runtime_error("Pedido repetido de anuncio alterado");
        tx.commit();
        return;
    }
    const auto items=inventory.items(seller);
    const auto selected=std::find_if(items.begin(),items.end(),[&](const auto &item){return item.slot==slot;});
    if (selected==items.end()) throw std::runtime_error("Item indisponivel para consignacao");
    consignment_list(db,inventory,seller,slot,selected->item,selected->uid,
                     static_cast<std::int64_t>(unit_price*count),time,count);
    db.exec("INSERT INTO consignment_registration VALUES(?,?,?)",{event,std::int64_t(seller),hex(request)});
    tx.commit();
}

void consignment_buy(Sqlite &db, Inventory &inventory, std::uint32_t buyer, std::int64_t listing,
                     std::int64_t expected_price, double time) {
    Transaction tx(db);
    const auto rows=db.query("SELECT * FROM consignment WHERE id=? AND status='listed' AND expires>?",
                             {listing,time});
    if (rows.empty()||rows[0].at("price")!=expected_price||rows[0].at("seller")==buyer)
        throw std::runtime_error("Anuncio indisponivel ou preco alterado");
    const auto &row=rows[0];
    const auto seller=row.at("seller").get<std::uint32_t>();
    ItemInstance item{};
    item.item=row.at("item_id");item.uid=row.at("uid");item.quantity=row.at("quantity");item.created=row.at("item_created");
    auto destination=inventory.restore(buyer,item,time);
    Wallet wallet(db,inventory);
    const auto event="consignment:"+std::to_string(listing);
    wallet.apply(buyer,-expected_price,event+":buy");
    wallet.apply(seller,expected_price,event+":sale");
    db.exec("UPDATE consignment SET status='sold',buyer=? WHERE id=?", {std::int64_t(buyer),listing});
    inventory.trace_item(item.uid,"consignment-buy",buyer,item.quantity,-1,destination);
    tx.commit();
}
void consignment_expire(Sqlite &db, std::uint32_t seller, double time) {
    const std::vector<SqlValue> args{std::int64_t(seller),time};
    if (db.query("SELECT 1 FROM consignment WHERE seller=? AND status='listed' AND expires<=? LIMIT 1",args).empty())
        return;
    Transaction tx(db);
    for (const auto &row:db.query("SELECT c.*,COALESCE(e.grade,0) AS grade FROM consignment c "
                                  "LEFT JOIN item_enchantments e ON e.uid=c.uid "
                                  "WHERE c.seller=? AND c.status='listed' AND c.expires<=?",
                                  args)) {
        const auto listing=row.at("id").get<std::int64_t>();
        const auto mail="consignment-return:"+std::to_string(listing);
        const Json attachment={{"id",row.at("item_id")},{"quantity",row.at("quantity")},
                               {"uid",row.at("uid")},{"created",row.at("item_created")},{"grade",row.at("grade")}};
        db.exec("INSERT INTO gm_mail VALUES(?,?,?,?,?,?,'pending',?)",
                {mail,std::int64_t(seller),std::string("Consignacao"),std::string("Anuncio expirado"),
                 std::string("Seu item voltou da consignacao apos 24 horas sem comprador."),
                 Json::array({attachment}).dump(),time});
        db.exec("UPDATE consignment SET status='return_pending' WHERE id=?",{listing});
    }
    tx.commit();
}
void consignment_claim_return(Sqlite &db, Inventory &inventory, std::uint32_t seller,
                              const std::string &mail, double time) {
    Transaction tx(db);
    const auto rows=db.query("SELECT * FROM consignment WHERE seller=? AND status='return_pending' "
                             "AND 'consignment-return:'||id=?",{std::int64_t(seller),mail});
    if (rows.empty()) throw std::runtime_error("Anexo indisponivel");
    const auto &row=rows[0];
    ItemInstance item{};
    item.item=row.at("item_id");item.uid=row.at("uid");item.quantity=row.at("quantity");item.created=row.at("item_created");
    const auto destination=inventory.restore(seller,item,time);
    inventory.trace_item(item.uid,"consignment-return",seller,item.quantity,-1,destination);
    db.exec("UPDATE consignment SET status='returned' WHERE id=?",{row.at("id").get<std::int64_t>()});
    db.exec("UPDATE gm_mail SET status='delivered' WHERE id=?",{mail});
    tx.commit();
}
void Inventory::move(std::uint32_t eid,const Bytes&body){
 if(body.size()!=16||get32(body,0,true)!=eid)throw std::runtime_error("Invalid bag move identity");
 auto source=key(body[4],body[5]),destination=key(body[10],body[11]);auto iid=get32(body,6,true),expected=get32(body,12,true);
 Transaction tx(db_);expire(eid,now());auto slots=available(eid,now());if(!slots.count(source)||!slots.count(destination))throw std::runtime_error("Slot indisponivel");
 auto original=items(eid);std::optional<ItemInstance> first,second;for(auto &i:original){if(i.slot==source)first=i;if(i.slot==destination)second=i;}
 if(!first||first->item!=iid||(second?second->item:0)!=expected)throw std::runtime_error("O item ou destino mudou. Sincronize o inventario");
 if(source!=destination){db_.exec("DELETE FROM inventory_slots WHERE player_eid=? AND slot IN (?,?)",{std::int64_t(eid),std::int64_t(source),std::int64_t(destination)});insert_row(db_,eid,destination,*first);trace_item(first->uid,"move",eid,first->quantity,source,destination);
  if(second){insert_row(db_,eid,source,*second);trace_item(second->uid,"move",eid,second->quantity,destination,source);}
 }
 tx.commit();
}
void Inventory::organize(std::uint32_t eid,const Bytes&body){
 if(body.size()<10||get32(body,0,true)!=eid||get16(body,8,true)!=0x22a)throw std::runtime_error("Invalid native sort request");
 auto count=get32(body,4,true);if(count>164||body.size()!=10+count*2)throw std::runtime_error("Invalid native sort size");
 std::set<int> requested;for(std::size_t at=10;at<body.size();at+=2)if(!requested.insert(key(body[at],body[at+1])).second)throw std::runtime_error("Duplicate native sort address");
 Transaction tx(db_);expire(eid,now());auto slots=available(eid,now());auto ordered=items(eid);
 for(auto slot:requested)if(!slots.count(slot))throw std::runtime_error("Sort requested an unavailable bag slot");
 ordered.erase(std::remove_if(ordered.begin(),ordered.end(),[&](const auto&item){return !slots.count(item.slot);}),ordered.end());
 std::stable_sort(ordered.begin(),ordered.end(),[](auto &a,auto &b){if(a.item!=b.item)return a.item<b.item;if(a.created!=b.created)return a.created<b.created;return a.uid<b.uid;});
 std::vector<ItemInstance> rows;std::map<std::uint32_t,std::size_t> partial;
 for(auto item:ordered){auto limit=std::clamp(rules_.value(std::to_string(item.item),Json::object()).value("stack",1),1,10000);
  if(item.item==999999||item.expires||limit<=1||item.quantity>limit||
     traced(item.uid)){rows.push_back(item);continue;}
  auto prior=partial.find(item.item);if(prior!=partial.end()){auto &target=rows[prior->second];auto amount=std::min(item.quantity,std::int64_t(limit)-target.quantity);target.quantity+=amount;item.quantity-=amount;}
  if(item.quantity){rows.push_back(item);if(item.quantity<limit)partial[item.item]=rows.size()-1;else partial.erase(item.item);}
 }
 if(rows.size()>slots.size())throw std::runtime_error("Inventario excede as bolsas disponiveis");
 // Closed/expired bags retain their original contents and UIDs, untouched by sorting.
 for(auto slot:slots)db_.exec("DELETE FROM inventory_slots WHERE player_eid=? AND slot=?",{std::int64_t(eid),std::int64_t(slot)});
 auto destination=slots.begin();
 for(auto& item:rows){
  const auto slot=*destination++;
  insert_row(db_,eid,slot,item);
  if(slot!=item.slot)trace_item(item.uid,"sort",eid,item.quantity,item.slot,slot);
 }
 tx.commit();
}
Json Inventory::trash(std::uint32_t eid,const Bytes&body,const std::string&event){
 // 06/41 has a 26-byte header followed by a NUL-terminated field.
 // The client sends four zero bytes when that field is empty.
 if(body.size()<27||body.size()>2030||get32(body,0,true)!=eid||get16(body,24,true)!=181||body.back()!=0||event.empty()||event.size()>118)throw std::runtime_error("Invalid native trash request");
 int slot=key(get32(body,4,true),get32(body,8,true));auto iid=get32(body,12,true);std::uint64_t quantity=0;for(int i=0;i<8;i++)quantity=(quantity<<8)|body[16+i];if(!quantity)throw std::runtime_error("Invalid removal quantity");
 Transaction tx(db_);auto request=hex(body);auto prior=db_.query("SELECT eid,kind,request,result FROM inventory_operations WHERE event_key=?",{event});
 if(!prior.empty()){if(prior[0].at("eid")!=eid||prior[0].at("kind")!="trash"||prior[0].at("request")!=request)throw std::runtime_error("Inventory replay changed its request");auto result=Json::parse(prior[0].at("result").get<std::string>());tx.commit();return result;}
 auto selected=owned(eid,slot,iid,now());
 if(quantity!=std::numeric_limits<std::uint64_t>::max()&&quantity>static_cast<std::uint64_t>(selected.quantity))throw std::runtime_error("Quantidade excede o item possuido");
 auto count=quantity==std::numeric_limits<std::uint64_t>::max()?selected.quantity:static_cast<std::int64_t>(quantity);
 consume(eid,selected,count,"discard");if(iid==999999){Wallet wallet(db_,*this);wallet.apply(eid,-count,"inventory:"+event);}
 Json result={{"quantity",count},{"item_id",iid},{"uid",selected.uid},{"created_at",selected.created},{"ground_id",nullptr}};
 db_.exec("INSERT INTO inventory_operations VALUES(?,?,?,?,?)",{event,std::int64_t(eid),std::string("trash"),request,result.dump()});tx.commit();return result;
}
}
