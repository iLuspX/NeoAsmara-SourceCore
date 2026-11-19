#include "native/game.hpp"
#include "native/economy.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace rosh {
bool WorldSession::crafting_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const Sender&send){
 if(cat!=6||(sub!=0x16&&sub!=0x3f))return false;
 const bool player=sub==0x3f;const std::size_t size=player?18:14,status_at=size-2,recipe_at=player?12:8;
 auto reply=[&](std::uint16_t status){auto packet=body;put16(packet,status_at,status,true);send(cat,sub,packet);};
 try{
  if(!loaded_||body.size()!=size||get32(body,0,true)!=eid()||event.empty()||event.size()>100)
   throw std::runtime_error("Invalid native crafting request");
  const auto status=get16(body,status_at,true);
  // 0x176 is the native player's stop request; completion is server-timed.
  if(player&&status==0x176){
   if(pending_craft_&&std::equal(body.begin(),body.begin()+16,pending_craft_->request.begin()))pending_craft_.reset();
   reply(0x179);return true;
  }
  if(status!=(player?0x177:0x6f)||character_.value("hp",0)<=0||mounted_||mount_pending_)
   throw std::runtime_error("Character cannot craft now");
  const auto catalog=content_.get("data/skill_catalog.json").at("crafting");
  const auto id=get32(body,recipe_at,true);const auto key=std::to_string(id);
  if(!catalog.at("recipes").contains(key))throw std::runtime_error("Unknown native recipe");
  const auto& recipe=catalog.at("recipes").at(key);
  const auto p=character_.at("pos");const Position position{p[0],p[1],p[2]};
  if(player){
   const auto skill=get32(body,4,true),rank=get32(body,8,true);
   bool permitted=false;
   if(catalog.at("player").contains(key))for(const auto& link:catalog.at("player").at(key))
    if(link.at("skill")==skill&&link.at("rank")==rank&&
       character_.value("skills",Json::object()).value(std::to_string(skill),0u)>=rank)permitted=true;
   if(!permitted)throw std::runtime_error("Crafting skill or required rank missing");
   if(pending_craft_){
    if(pending_craft_->event==event&&pending_craft_->request==body){reply(0x177);return true;}
    throw std::runtime_error("A crafting operation is already in progress");
   }
   store_.db().exec("CREATE TABLE IF NOT EXISTS native_craft_operations(event_key TEXT PRIMARY KEY,eid INTEGER NOT NULL,recipe TEXT NOT NULL,success INTEGER NOT NULL)");
   const auto prior=store_.db().query("SELECT * FROM native_craft_operations WHERE event_key=?",{"player:"+event});
   if(!prior.empty()){
    if(prior[0].at("eid")!=eid()||prior[0].at("recipe")!=recipe.dump())throw std::runtime_error("Altered crafting replay");
    reply(prior[0].at("success").get<int>()?0x178:0x85);return true;
   }
   stand(send);pending_craft_=PendingCraft{body,event,map_scope(),position,now()+recipe.at("seconds").get<double>()};
   reply(0x177);scene_->action(scene_key(),cat,sub,body);return true;
  }
  const auto actor=get32(body,4,true);const auto found=scene_entities_->find(actor);
  if(found==scene_entities_->end()||!found->second.alive||found->second.position.distance(position)>100)
   throw std::runtime_error("Visit the recipe's crafting NPC");
  const auto npc=std::to_string(found->second.npc_id);
  if(!catalog.at("npcs").contains(npc)||std::find(catalog.at("npcs").at(npc).begin(),catalog.at("npcs").at(npc).end(),id)==catalog.at("npcs").at(npc).end())
   throw std::runtime_error("This NPC does not offer this recipe");
  const auto random=random_bytes(4);
  const bool success=craft_inventory(store_.db(),*inventory_,eid(),recipe,"npc:"+event,now(),get32(random,0)/4294967296.);
  reply(success?0x6f:0x85);send_inventory(send,true);
 }catch(const std::exception& error){
  if(body.size()==size&&get32(body,0,true)==eid())reply(0x73);
  send(11,1,notice(error.what()));
 }
 return true;
}
void WorldSession::tick_crafting(const Sender&send){
 if(!pending_craft_)return;
 auto& craft=*pending_craft_;const auto p=character_.at("pos");const Position position{p[0],p[1],p[2]};
 const bool canceled=character_.value("hp",0)<=0||mounted_||mount_pending_||sitting_||map_scope()!=craft.map||position.distance(craft.position)>1.;
 if(!canceled&&now()<craft.finish)return;
 auto pending=std::move(craft);pending_craft_.reset();auto reply=pending.request;
 std::uint16_t result=0x179;
 if(!canceled)try{
  const auto skill=get32(reply,4,true),rank=get32(reply,8,true);
  if(character_.value("skills",Json::object()).value(std::to_string(skill),0u)<rank)
   throw std::runtime_error("Crafting skill changed");
  const auto catalog=content_.get("data/skill_catalog.json").at("crafting");
  const auto random=random_bytes(4);
  result=craft_inventory(store_.db(),*inventory_,eid(),catalog.at("recipes").at(std::to_string(get32(reply,12,true))),
      "player:"+pending.event,now(),get32(random,0)/4294967296.)?0x178:0x85;
 }catch(const std::exception& error){result=0x73;send(11,1,notice(error.what()));}
 put16(reply,16,result,true);send(6,0x3f,reply);scene_->action(scene_key(),6,0x3f,reply);send_inventory(send,true);
}
Json native_buff_state(const Json& character,const Json& borrowed,const Json& item_links,double timestamp){
 Json result=Json::object();
 auto add=[&](std::uint32_t skill,int rank,std::uint32_t item,double expires){
  if(!skill||rank<1||rank>255||!std::isfinite(expires)||expires<=timestamp)return;
  const auto key=std::to_string(skill);
  // The native bar keys icons by skill ID, including potions sharing that skill.
  if(!result.contains(key)||expires>result.at(key).at("expires").get<double>())
   result[key]={{"rank",rank},{"item",item},{"expires",expires}};
 };
 const auto active=character.value("active_buffs",Json::object());
 for(const auto& [key,buff]:active.items())
  add(static_cast<std::uint32_t>(std::stoul(key)),buff.at("rank"),0,buff.at("expires"));
 for(const auto& [group,buff]:borrowed.items())
  add(buff.at("skill"),buff.value("rank",1),0,buff.at("expires"));
 for(const auto* field:{"consumable_effects","timed_item_buffs"}){
  const auto effects=character.value(field,Json::object());
  for(const auto& effect:effects){
   const auto item=effect.value("item_id",0u);
   const auto key=std::to_string(item);
   const auto skill=item_links.contains(key)?item_links.at(key).value("skill_id",item):item;
   add(skill,effect.value("rank",1),item,effect.value("expires",0.));
  }
 }
 return result;
}
Bytes native_buff_packet(std::uint32_t eid,const Json& entries,bool add,double timestamp){
 if(!entries.is_object()||entries.size()>65535||!std::isfinite(timestamp))throw std::runtime_error("Invalid native buff list");
 Bytes body(8+18*entries.size());put32(body,1,eid,true);body[5]=add?1:0;put16(body,6,static_cast<std::uint16_t>(entries.size()),true);
 std::size_t offset=8;
 for(const auto& [key,buff]:entries.items()){
  const auto id=std::stoull(key);
  if(!id||id>0xffffffffu)throw std::runtime_error("Invalid native buff ID");
  put32(body,offset,static_cast<std::uint32_t>(id),true);
  if(add){
   const auto rank=buff.at("rank").get<int>();
   const auto remaining=buff.at("expires").get<double>()-timestamp;
   if(rank<1||rank>255||!std::isfinite(remaining)||remaining<=0)throw std::runtime_error("Invalid native buff duration or rank");
   body[offset+4]=static_cast<unsigned char>(rank);
   put32(body,offset+6,buff.at("item"),true);
   put32(body,offset+10,static_cast<std::uint32_t>(std::min(std::ceil(remaining),double(std::numeric_limits<std::int32_t>::max()))),true);
  }
  offset+=18;
 }
 return body;
}
void WorldSession::sync_native_buffs(const Sender& send,bool force){
 if(!loaded_)return;
 const auto stamp=now();
 if(!force&&stamp<next_native_buff_poll_)return;
 next_native_buff_poll_=stamp+.25;
 if(native_buff_links_.is_null())native_buff_links_=content_.get("data/item_effect_links_428.json");
 const auto current=native_buff_state(character_,received_party_auras_,native_buff_links_,stamp);
 Json removed=Json::object(),changed=Json::object();
 for(const auto& [key,buff]:sent_native_buffs_.items())if(!current.contains(key))removed[key]=buff;
 for(const auto& [key,buff]:current.items())if(force||!sent_native_buffs_.contains(key)||sent_native_buffs_.at(key)!=buff)changed[key]=buff;
 // 05/09 is a delta: flag zero removes listed IDs; flag one adds/updates them.
 if(!removed.empty())send(5,9,native_buff_packet(eid(),removed,false,stamp));
 if(!changed.empty())send(5,9,native_buff_packet(eid(),changed,true,stamp));
 sent_native_buffs_=current;
}
void WorldSession::profession_request(const Bytes& body,const std::string& event,const Sender& send){
 // AMo 0x51B175 writes actor, destination class, tier and status in C08/03.
 if(body.size()!=14||get32(body,0,true)!=eid()||get16(body,12,true)!=0x11a||
    event.empty()||event.size()>100){
  send(11,1,notice("Pedido de profissao invalido."));
  return;
 }
 auto reply=[&](std::uint16_t status){auto packet=body;put16(packet,12,status,true);send(8,3,packet);};
 auto before=character_,beforeStats=stats_;
 bool committed=false;
 try{
  auto& db=store_.db();
  db.exec("CREATE TABLE IF NOT EXISTS native_profession_changes(event_key TEXT PRIMARY KEY,eid INTEGER NOT NULL,request TEXT NOT NULL)");
  Transaction tx(db);
  auto prior=db.query("SELECT eid,request FROM native_profession_changes WHERE event_key=?",{event});
  if(!prior.empty()){
   if(prior[0].at("eid")!=eid()||prior[0].at("request")!=hex(body))
    throw std::runtime_error("Pedido repetido de profissao alterado");
   const bool current_result=get32(body,4,true)==character_.at("cls").get<std::uint32_t>()&&
       get32(body,8,true)==character_.value("profession_grade",0u);
   tx.commit();reply(current_result?0x11a:0x11b);return;
  }
  const auto destination=get32(body,4,true);
  const auto tier=get32(body,8,true);
  const auto current=character_.at("cls").get<std::uint32_t>();
  const auto current_grade=character_.value("profession_grade",0u);
  const bool valid_change=tier==0?destination!=current:
      tier<=2&&destination==current&&tier==current_grade+1;
  if(!valid_change||destination/100!=current/100||character_.value("hp",0)<=0){
   reply(0x11b);return;
  }
  auto classes=content_.get("data/fletta_profession_records.json").at("classChange");
  Json record;
  for(const auto& row:classes){
   const auto& raw=row.at("raw_u32");
   if(raw.size()==6&&raw[0]==destination&&raw[1]==tier&&raw[2]==current){record=raw;break;}
  }
  if(record.is_null()){reply(0x11b);return;}
  if(character_.at("level").get<int>()<record[3].get<int>()){reply(0x11d);return;}
  Wallet wallet(db,*inventory_);
  const auto price=record[4].get<std::int64_t>();
  if(wallet.balance(eid())<price){reply(0x11c);return;}
  const auto certificate=record[5].get<std::uint32_t>();
  std::optional<ItemInstance> proof;
  const auto stamp=now();
  if(certificate){
   auto slots=inventory_->available(eid(),stamp);
   for(const auto& item:inventory_->items(eid()))if(item.item==certificate&&slots.count(item.slot)&&
      (!item.expires||*item.expires>stamp)){proof=item;break;}
   if(!proof){reply(0x11e);return;}
  }
  if(proof)inventory_->consume(eid(),*proof,1,"profession-certificate");
  if(price)wallet.apply(eid(),-price,"profession:"+event);
  character_["profession_grade"]=tier;
  if(destination!=current){
   character_["cls"]=destination;
   skills_->initialize(character_);
   load_templates();
  }
  recalculate();
  persist();
  db.exec("INSERT INTO native_profession_changes VALUES(?,?,?)",{event,std::int64_t(eid()),hex(body)});
  tx.commit();committed=true;
  casting_->cancel();
  reply(0x11a);
  send_inventory(send,true);
  send(5,5,skills_->list(character_,templates_.at(0x0505)));
  send(5,12,skills_->super_list(character_));
  send(8,2,stats_packet());
  send(8,0,gauge(0x0800));
  send(8,1,gauge(0x0801));
  refresh_self_appearance(send);
  publish_player(send);
 }catch(const std::exception&e){
  if(!committed){character_=std::move(before);stats_=std::move(beforeStats);load_templates();reply(0x11f);}
  send(11,1,notice(e.what()));
 }
}
bool WorldSession::super_stone_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const Sender&send){
 const bool choice=cat==6&&sub==0x5d;
 if(!choice&&(cat!=5||(sub!=13&&sub!=15)))return false;
 const bool remove=!choice&&sub==15;const auto status_at=choice?10u:remove?8u:14u;
 const auto expected_size=choice?12u:remove?10u:16u;
 auto before=character_,before_stats=stats_;bool committed=false;std::uint16_t failure=choice?0x2ad:remove?0x2aa:0x2a5;
 try{
  if(!loaded_||character_.value("hp",0)<=0||body.size()!=expected_size||(!choice&&get32(body,0,true)!=eid())||event.empty()||event.size()>116)
   throw std::runtime_error("Invalid super-skill stone request or unavailable character");
  auto& db=store_.db();
  db.exec("CREATE TABLE IF NOT EXISTS native_skill_operations(event_key TEXT PRIMARY KEY,eid INTEGER NOT NULL,opcode INTEGER NOT NULL,request TEXT NOT NULL,reply TEXT NOT NULL)");
  Bytes reply;
  {Transaction tx(db);const auto prior=db.query("SELECT * FROM native_skill_operations WHERE event_key=?",{event});
   if(!prior.empty()){
    if(prior.front().at("eid")!=eid()||prior.front().at("opcode")!=cat*256+sub||prior.front().at("request")!=hex(body))
     throw std::runtime_error("Altered replay of super-skill stone operation");
    reply=unhex(prior.front().at("reply"));
   }else{
    if(!choice){
    const auto mentors=content_.get("data/skill_catalog.json").at("super_mentors");
    const auto p=character_.at("pos");const Position position{p[0],p[1],p[2]};bool mentor_in_range=false;
    for(const auto& [actor,npc]:*scene_entities_)
     if(npc.alive&&std::find(mentors.begin(),mentors.end(),npc.npc_id)!=mentors.end()&&npc.position.distance(position)<=100){mentor_in_range=true;break;}
    if(!mentor_in_range)throw std::runtime_error("Visit a Super Skill instructor to change stones");
    }
    std::optional<ItemInstance> item;
    if(!remove)item=inventory_->owned(eid(),Inventory::key(body[choice?8:12],body[choice?9:13]),choice?140024u:get32(body,8,true),now());
    if(choice){
     const auto source=std::to_string(get32(body,0,true));
     if(character_.value("super_skill_progress",Json::object()).value(source,Json::object()).value("option",0u))failure=0x2ac;
     reply=skills_->change_super_choice(character_,body);
    }else{
    const auto plan=skills_->change_super_stone(character_,body,remove);
    Wallet wallet(db,*inventory_);const auto fee=plan.at("fee").get<std::int64_t>();
    if(wallet.balance(eid())<fee){failure=0xb;throw std::runtime_error("Insufficient Carats for the stone operation");}
    wallet.apply(eid(),-fee,"super-stone:"+event);
    reply=unhex(plan.at("reply"));
    if(item)
     character_["super_skill_progress"][plan.at("skill").get<std::string>()]["source_uid"]=item->uid;
    }
    if(item)inventory_->consume(eid(),*item,1,choice?"super-skill-choice-coupon":"super-skill-stone-applied");
    recalculate();persist();
    db.exec("INSERT INTO native_skill_operations VALUES(?,?,?,?,?)",{event,std::int64_t(eid()),std::int64_t(cat*256+sub),hex(body),hex(reply)});
   }
   tx.commit();committed=true;
  }
  if(choice)casting_->cancel();
  send(cat,sub,reply);send_inventory(send,true);if(choice)send(5,5,skills_->list(character_,templates_.at(0x0505)));send(5,12,skills_->super_list(character_));
  send(8,2,stats_packet());send(8,1,gauge(0x0801));send(8,0,gauge(0x0800));publish_player(send);
 }catch(const std::exception& error){
  if(committed)throw;
  character_=std::move(before);stats_=std::move(before_stats);
  if(body.size()==expected_size&&(choice||get32(body,0,true)==eid())){
   auto reply=body;put16(reply,status_at,failure,true);send(cat,sub,reply);
  }
  send(11,1,notice(error.what()));
 }
 return true;
}
bool WorldSession::super_skill_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const Sender&send){
 if(cat!=5||sub!=14)return false;
 auto before=character_,before_stats=stats_;bool committed=false;
 try{
  if(!loaded_||character_.value("hp",0)<=0||body.size()!=16||get32(body,0,true)!=eid()||event.empty()||event.size()>200)
   throw std::runtime_error("Pedido de super-habilidade inválido ou personagem indisponível");
  auto& db=store_.db();
  db.exec("CREATE TABLE IF NOT EXISTS native_skill_operations(event_key TEXT PRIMARY KEY,eid INTEGER NOT NULL,opcode INTEGER NOT NULL,request TEXT NOT NULL,reply TEXT NOT NULL)");
  Bytes reply;
  {Transaction tx(db);const auto prior=db.query("SELECT * FROM native_skill_operations WHERE event_key=?",{event});
   if(!prior.empty()){
    if(prior.front().at("eid")!=eid()||prior.front().at("opcode")!=0x50e||prior.front().at("request")!=hex(body))
     throw std::runtime_error("Pedido repetido de super-habilidade foi alterado");
    reply=unhex(prior.front().at("reply"));
   }else{
    reply=skills_->invest_super(character_,body);recalculate();persist();
    db.exec("INSERT INTO native_skill_operations VALUES(?,?,?,?,?)",{event,std::int64_t(eid()),std::int64_t(0x50e),hex(body),hex(reply)});
   }
   tx.commit();committed=true;
  }
  send(5,14,reply);send(5,5,skills_->list(character_,templates_.at(0x0505)));send(5,12,skills_->super_list(character_));
  send(8,2,stats_packet());send(8,1,gauge(0x0801));send(8,0,gauge(0x0800));publish_player(send);
 }catch(const std::exception& error){
  if(committed)throw;
  character_=std::move(before);stats_=std::move(before_stats);
  if(body.size()==16&&get32(body,0,true)==eid()){
   auto reply=body;put16(reply,13,0x2a8,true);send(5,14,reply);
  }
  send(11,1,notice(error.what()));
 }
 return true;
}
void WorldSession::skill_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const Sender&send){
 auto original=character_,originalStats=stats_;bool committed=false;
 try{
  if(event.empty()||event.size()>200)throw std::runtime_error("Identificador de operacao ausente");
  store_.db().exec("CREATE TABLE IF NOT EXISTS native_skill_operations(event_key TEXT PRIMARY KEY,eid INTEGER NOT NULL,opcode INTEGER NOT NULL,request TEXT NOT NULL,reply TEXT NOT NULL)");
  Transaction tx(store_.db());Bytes reply;auto op=cat*256+sub;auto prior=store_.db().query("SELECT * FROM native_skill_operations WHERE event_key=?",{event});
  if(!prior.empty()){
   if(prior[0].at("eid")!=eid()||prior[0].at("opcode")!=op||prior[0].at("request")!=hex(body))throw std::runtime_error("Pedido repetido com dados diferentes");reply=unhex(prior[0].at("reply"));
  }else{
   reply=cat==6?QuickSlots::change(character_,sub,body):cat==8?skills_->allocate(character_,body):skills_->learn(character_,body,sub==7);
   recalculate();persist();store_.db().exec("INSERT INTO native_skill_operations VALUES(?,?,?,?,?)",{event,std::int64_t(eid()),std::int64_t(op),hex(body),hex(reply)});
  }
  tx.commit();committed=true;send(cat,cat==6&&sub==82?47:sub,reply);if(cat==6)return;send(5,5,skills_->list(character_,templates_.at(0x0505)));send(8,2,stats_packet());send(8,1,gauge(0x0801));send(8,0,gauge(0x0800));publish_player(send);
 }catch(const std::exception&e){if(committed)throw;character_=std::move(original);stats_=std::move(originalStats);if(cat==8&&body.size()==18&&get32(body,0,true)==eid()){auto rejected=body;put16(rejected,16,0x121,true);send(cat,sub,rejected);}send(11,1,notice(e.what()));}
}
}
