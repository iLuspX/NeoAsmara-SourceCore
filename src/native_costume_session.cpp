#include "native/game.hpp"
#include <algorithm>
namespace rosh {
static Bytes choice_message(Content&content,const Json&message){auto body=unhex(content.get("server/stage_templates.json").at("game").at("020b"));if(body.size()<14)throw std::runtime_error("Overlay carrier missing");body.resize(14);put32(body,0,103,true);auto text=message.dump();if(text.size()>65535)throw std::runtime_error("Overlay message too large");body.insert(body.end(),text.begin(),text.end());body.push_back(0);return body;}
bool WorldSession::costume_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const Sender&send){
 if(!loaded_)return false;
 if(cat==7&&sub==1){
  if(body.size()<2||body.size()>4096||body.back()!=0||std::any_of(body.begin(),body.end()-1,[](auto c){return c<32||c>126;}))return false;
  auto words=command_tokens(std::string(body.begin(),body.end()-1));if(words.size()!=4||words[0]!="npcshop"||words[1]!="starterchoose")return false;
  try{
   for(int index=2;index<4;index++)if(words[index].empty()||std::any_of(words[index].begin(),words[index].end(),[](auto c){return c<'0'||c>'9';}))throw std::runtime_error("Escolha invalida");
   auto raw=std::stoull(words[2]);if(raw>0xffffffffULL)throw std::runtime_error("UID fora do limite");auto uid=static_cast<std::uint32_t>(raw);auto option=std::stoul(words[3]);if(option<1||option>2)throw std::runtime_error("Escolha opcao1 ou2");
   if(!character_.contains("_costume_choice_uid")||character_.at("_costume_choice_uid")!=uid)throw std::runtime_error("Abra novamente a selecao desta peca");
   CashChoices choices(store_.db(),content_,*inventory_);choices.choose(eid(),uid,character_.at("cls"),static_cast<int>(option));
   character_.erase("_costume_choice_uid");
   recalculate();persist();send_inventory(send,true);send(2,11,choice_message(content_,{{"cmd","asmara.choice.done"}}));
   send(8,2,stats_packet());send(8,0,gauge(0x0800));send(8,1,gauge(0x0801));publish_player(send);
  }catch(const std::exception&e){send(11,1,notice(e.what()));}
  return true;
 }
 if(cat!=6||(sub!=0&&sub!=2))return false;
 if(sub==2){
  if(body.size()!=16||get32(body,0,true)!=eid()||body[10]!=0||get32(body,11,true)!=eid()||body[15]!=0)return false;auto iid=get32(body,6,true);
  const auto definition=inventory_->definition(iid);
  if(definition.value("kind",0)==11&&definition.value("subtype",0)==39&&definition.value("slot",0)==10){
   Bytes wear(12);put32(wear,0,eid(),true);wear[4]=body[4];wear[5]=body[5];wear[6]=10;put32(wear,7,iid,true);
   try{handle(6,0,wear,send,event);}catch(const std::exception&e){send(11,1,notice(e.what()));}return true;
  }
  if(iid!=999000303&&(iid<180148||iid>180175)&&(iid<1000900000||iid>=1000900028))return false;
  try{auto slot=Inventory::key(body[4],body[5]);CashChoices choices(store_.db(),content_,*inventory_);if(iid==999000303||iid<180148){choices.open(eid(),slot,iid,character_.at("cls"));send(6,2,body);send_inventory(send,true);send(11,1,notice("Recebeu quatro caixas: torso, calca, luvas e botas. Escolha uma configuracao em cada peca."));}
   else{auto item=inventory_->owned(eid(),slot,iid,now());auto preview=choices.preview(eid(),item.uid,character_.at("cls"));character_["_costume_choice_uid"]=item.uid;send(6,2,body);send(2,11,choice_message(content_,preview));}
  }catch(const std::exception&e){send(11,1,notice(e.what()));}return true;
 }
 if((body.size()!=11&&body.size()!=12)||get32(body,0,true)!=eid())return false;
 // The client may send an equip request for the legacy selector templates.
 // Translate that request into opening the box, never into wearing it.
 auto selector=get32(body,7,true);
 if((selector>=180148&&selector<180176)||(selector>=1000900000&&selector<1000900028)){
  Bytes use(16);put32(use,0,eid(),true);use[4]=body[4];use[5]=body[5];put32(use,6,selector,true);put32(use,11,eid(),true);
  return costume_request(6,2,use,event,send);
 }

 return false;
}
}
