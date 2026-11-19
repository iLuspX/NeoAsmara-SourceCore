#include "native/premium.hpp"
#include "native/costumes.hpp"
#include <algorithm>
#include <limits>
namespace rosh {
PremiumShop::PremiumShop(Sqlite&db,Inventory&inventory,Content&content):content_(content),db_(db),inventory_(inventory),shop_(content.get("data/cash_shop_local.json")),gacha_(content.get("data/gacha_local.json")),rules_(content.get("data/native_item_rules.json")){
 db_.exec("CREATE TABLE IF NOT EXISTS premium_wallet(account_id TEXT PRIMARY KEY,balance INTEGER NOT NULL CHECK(balance BETWEEN 0 AND 4294967295));CREATE TABLE IF NOT EXISTS premium_ledger(event_key TEXT PRIMARY KEY,account_id TEXT NOT NULL,amount INTEGER NOT NULL,actor_account TEXT NOT NULL,actor_eid INTEGER NOT NULL,reason TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);CREATE TABLE IF NOT EXISTS local_item_orders(event_key TEXT PRIMARY KEY,account_id TEXT NOT NULL,player_eid INTEGER NOT NULL,kind TEXT NOT NULL,request TEXT NOT NULL,result TEXT NOT NULL,created_at INTEGER NOT NULL)");
 initialize_catalog_edits(db_);
}
std::int64_t PremiumShop::balance(const std::string&account){auto rows=db_.query("SELECT balance FROM premium_wallet WHERE account_id=?",{account});return rows.empty()?0:rows[0].at("balance").get<std::int64_t>();}
std::int64_t PremiumShop::cents(const std::string&account){return std::min<std::int64_t>(2147483647,balance(account)*100);}
Json PremiumShop::replay(const std::string&event,const std::string&account,std::uint32_t eid,const std::string&kind,const std::string&request){
 if(event.empty()||event.size()>160)throw std::runtime_error("Identificador de compra invalido");auto rows=db_.query("SELECT * FROM local_item_orders WHERE event_key=?",{event});if(rows.empty())return nullptr;auto&r=rows[0];if(r.at("account_id")!=account||r.at("player_eid")!=eid||r.at("kind")!=kind||r.at("request")!=request)throw std::runtime_error("Recibo reutilizado com outros dados");return Json::parse(r.at("result").get<std::string>());
}
void PremiumShop::record(const std::string&event,const std::string&account,std::uint32_t eid,const std::string&kind,const std::string&request,const Json&result){db_.exec("INSERT INTO local_item_orders VALUES(?,?,?,?,?,?,?)",{event,account,std::int64_t(eid),kind,request,result.dump(),std::int64_t(now())});}
std::vector<Json>PremiumShop::state(const std::string&account,int profession){
 int family=profession/100*100;
 const auto edits=db_.query("SELECT offer,price FROM cash_price_edits ORDER BY offer");
 std::map<int,std::int64_t> prices;for(const auto& row:edits)prices[row.at("offer")]=row.at("price");
 cash_quotes_.clear();Json articles=Json::array();for(auto&a:shop_.at("offers")){
  if(a.value("unavailable",false))continue;
  bool excluded=false;Json entries=Json::array();for(auto&i:a.at("items")){auto id=i[0].get<std::uint32_t>();if(rules_.value(std::to_string(id),Json::object()).value("subtype",0)==78)excluded=true;entries.push_back(Json::array({inventory_.name(id),i[1],id}));}if(excluded)continue;
  const int id=a.at("id");
  const auto price=prices.count(id)?prices.at(id):a.at("cash").get<std::int64_t>();
  cash_quotes_[id]=price;
  articles.push_back(Json::array({a.at("id"),a.at("section"),a.at("title"),price*100,0,a.at("picture"),Json::array({Json::array({0,entries})}),a.at("about"),Json{{"class",a.value("class",0)},{"level",a.value("level",0)},{"mode",a.value("purchase_mode",std::string("direct"))},{"boxed",a.value("boxed",false)},{"counterpart",a.value("counterpart",0)},{"productGroup",a.value("product_group",0)},{"duration",a.value("duration_seconds",0)},{"displayTitle",a.value("display_title",std::string())}}}));
 }
 std::vector<Json>chunks;Json chunk=Json::array();std::size_t bytes=0;
 for(auto&article:articles){auto size=article.dump().size();if(!chunk.empty()&&(chunk.size()>=12||bytes+size>24000)){chunks.push_back(std::move(chunk));chunk=Json::array();bytes=0;}chunk.push_back(article);bytes+=size;}if(!chunk.empty())chunks.push_back(std::move(chunk));
 auto version=shop_.at("version").get<std::string>()+":"+std::to_string(family)+":"+hex(sha256(edits.dump())).substr(0,12);std::vector<Json>out={{{"cmd","shop.balance"},{"cents",cents(account)}},{{"cmd","shop.sections"},{"version",version},{"front",1},{"parts",chunks.size()},{"sections",shop_.at("sections")},{"playerClass",family}}};
 for(std::size_t i=0;i<chunks.size();i++)out.push_back({{"cmd","shop.articles"},{"version",version},{"part",i},{"articles",std::move(chunks[i])}});return out;
}
Json PremiumShop::buy(const std::string&account,std::uint32_t eid,int profession,int id,int copies,const std::string&event){
 if(copies<1||copies>50)throw std::runtime_error("Quantidade de compra invalida");Json offer;for(auto&a:shop_.at("offers"))if(a.at("id")==id){offer=a;break;}if(offer.is_null())throw std::runtime_error("Produto inexistente");(void)profession;
 for(auto&i:offer.at("items")){auto key=std::to_string(i[0].get<std::uint32_t>());if(!rules_.contains(key))throw std::runtime_error("Item sem definicao nativa");if(rules_.at(key).value("subtype",0)==78)throw std::runtime_error("Pedras de super habilidade nao sao vendidas diretamente");for(auto&coin:gacha_.at("coin_ids"))if(i[0]==coin&&i[1].get<std::int64_t>()*copies>100)throw std::runtime_error("Limite de 100 Gacha Coins por compra");}
 auto request=Json::array({id,copies}).dump();Transaction tx(db_);auto old=replay(event,account,eid,"cash",request);if(!old.is_null()){tx.commit();return old;}
 if(offer.value("unavailable",false))throw std::runtime_error("Item indisponivel nesta versao do client");
 const auto edited=db_.query("SELECT price FROM cash_price_edits WHERE offer=?",{std::int64_t(id)});
 const auto unit=edited.empty()?offer.at("cash").get<std::int64_t>():edited.front().at("price").get<std::int64_t>();
 const auto quote=cash_quotes_.find(id);
 if((quote!=cash_quotes_.end()&&quote->second!=unit)||(quote==cash_quotes_.end()&&!edited.empty()))throw std::runtime_error("Preco alterado. Reabra a Loja Cash antes de comprar.");
 if(unit<=0||unit>0xffffffffLL/copies)throw std::runtime_error("Preco invalido");auto cost=unit*copies;
 db_.exec("UPDATE premium_wallet SET balance=balance-? WHERE account_id=? AND balance>=?",{cost,account,cost});if(db_.changes()!=1)throw std::runtime_error("Cash insuficiente");
 std::vector<std::pair<std::uint32_t,int>>permanent;int index=0;
 if(offer.value("purchase_mode",std::string("direct"))=="boxed"){
  if(!offer.contains("choice_targets")||offer.at("choice_targets").size()!=4)throw std::runtime_error("Conjunto de caixas invalido");CashChoices choices(db_,content_,inventory_);
  for(int copy=0;copy<copies;copy++)for(auto&i:offer.at("choice_targets")){if(i[1]!=1)throw std::runtime_error("Caixa exige uma peca unica");choices.grant_catalog_box(eid,i[0],event+":choice:"+std::to_string(index++));}
 }else for(auto&i:offer.at("items")){
  auto item=i[0].get<std::uint32_t>();auto amount=i[1].get<std::int64_t>()*copies;if(amount<1||amount>10000)throw std::runtime_error("Quantidade do produto invalida");auto&r=rules_.at(std::to_string(item));int duration=(r.at("kind")==1||r.at("kind")==11)?r.value("duration",0):0;
  if(duration)inventory_.grant_timed_once(eid,item,static_cast<int>(amount),duration,event+":timed:"+std::to_string(index));else permanent.push_back({item,static_cast<int>(amount)});index++;
 }if(!permanent.empty())inventory_.grant_once(eid,permanent,event+":goods");
 db_.exec("INSERT INTO premium_ledger(event_key,account_id,amount,actor_account,actor_eid,reason) VALUES(?,?,?,?,?,?)",{event,account,-cost,account,std::int64_t(eid),std::string("Cash Shop: ")+offer.at("title").get<std::string>()});Json result={{"cmd","shop.bought"},{"ok",true},{"cents",cents(account)},{"title","Compra concluida"},{"text",offer.at("title").get<std::string>()+" - entregue no inventario."}};record(event,account,eid,"cash",request,result);tx.commit();return result;
}
Json PremiumShop::history(const std::string&account){Json rows=Json::array();for(auto&r:db_.query("SELECT o.created_at,o.request,l.amount FROM local_item_orders o LEFT JOIN premium_ledger l ON l.event_key=o.event_key WHERE o.account_id=? AND o.kind='cash' ORDER BY o.created_at DESC LIMIT 25",{account})){auto q=Json::parse(r.at("request").get<std::string>());for(auto&a:shop_.at("offers"))if(a.at("id")==q[0]){rows.push_back(Json::array({r.at("created_at"),a.at("title"),q[1],(r.at("amount").is_null()?a.at("cash").get<std::int64_t>()*q[1].get<int>():-r.at("amount").get<std::int64_t>())*100,"game"}));break;}}return {{"cmd","shop.history"},{"rows",rows}};}
Bytes PremiumShop::machines(const Bytes&request){
 if(request.size()!=8||get16(request,0,true)<1||get16(request,0,true)>5||get16(request,2,true)!=1||get32(request,4,true)!=0)throw std::runtime_error("Maquina inexistente");Bytes out(8+5*16);put16(out,0,get16(request,0,true),true);put16(out,2,1,true);put16(out,4,5,true);put16(out,6,5,true);for(int tab=1;tab<=5;tab++){int offset=8+(tab-1)*16;put16(out,offset,tab);put32(out,offset+2,1);put16(out,offset+6,2);put16(out,offset+8,gacha_.at("price"));put32(out,offset+12,current_gacha_pool(db_,gacha_,tab).at(0).at("item"));}return out;
}
Bytes PremiumShop::prizes(const Bytes&request){machines(request);const auto tab=get16(request,0,true);auto pool=current_gacha_pool(db_,gacha_,tab);gacha_quotes_[tab]=pool;if(pool.size()>1000)throw std::runtime_error("Lista de premios excede o limite nativo");Bytes out(8+pool.size()*12);put16(out,0,get16(request,0,true),true);put16(out,2,1,true);put16(out,4,pool.size(),true);for(std::size_t i=0;i<pool.size();i++){auto&p=pool[i];put16(out,8+i*12,i+1);put32(out,10+i*12,p.at("item"));put16(out,14+i*12,p.at("quantity"));put16(out,16+i*12,p.at("weight"));}return out;}
Bytes PremiumShop::spin(const std::string&account,std::uint32_t eid,const Bytes&body,const std::string&event,const std::function<std::uint32_t(std::uint32_t)>&random){
 if(body.size()!=24||get16(body,0,true)<1||get16(body,0,true)>5||get32(body,2,true)!=1||get32(body,14,true)||get32(body,18,true)||get16(body,22,true)!=606)throw std::runtime_error("Maquina inexistente");int tab=get16(body,0,true),slot=Inventory::key(get32(body,6,true),get32(body,10,true));Transaction tx(db_);auto request=hex(body);auto old=replay(event,account,eid,"gacha",request);if(!old.is_null()){tx.commit();return unhex(old.at("body"));}
 ItemInstance coin;bool found=false;for(auto&i:inventory_.items(eid))if(i.slot==slot&&(!i.expires||*i.expires>now()))for(auto&id:gacha_.at("coin_ids"))if(i.item==id){coin=i;found=true;}int price=gacha_.at("price");if(!found||coin.quantity<price||price<1)throw std::runtime_error("Gacha Coin insuficiente");
 auto pool=current_gacha_pool(db_,gacha_,tab);
 const auto quote=gacha_quotes_.find(tab);
 if((quote!=gacha_quotes_.end()&&quote->second!=pool)||(quote==gacha_quotes_.end()&&pool!=gacha_.at("tabs").at(std::to_string(tab))))throw std::runtime_error("Premios alterados. Reabra a Loteria antes de girar.");
 std::uint32_t total=0;for(auto&p:pool){int weight=p.at("weight"),quantity=p.at("quantity");if(weight<1||weight>65535||quantity<1||quantity>65535)throw std::runtime_error("Premio invalido");total+=weight;}if(!total)throw std::runtime_error("Sem premios");std::uint32_t ticket;
 if(random)ticket=random(total);else{auto limit=(0x100000000ULL/total)*total;do{ticket=get32(random_bytes(4),0);}while(ticket>=limit);ticket%=total;}if(ticket>=total)throw std::runtime_error("Sorteio invalido");Json prize;for(auto&p:pool){auto weight=p.at("weight").get<std::uint32_t>();if(ticket<weight){prize=p;break;}ticket-=weight;}
 inventory_.consume(eid,coin,price,"gacha-spin");inventory_.grant_once(eid,{{prize.at("item"),prize.at("quantity")}},event+":prize");auto reply=body;put32(reply,14,prize.at("item"),true);put32(reply,18,prize.at("grade"),true);record(event,account,eid,"gacha",request,{{"body",hex(reply)},{"item",prize.at("item")},{"quantity",prize.at("quantity")}});tx.commit();return reply;
}
}
