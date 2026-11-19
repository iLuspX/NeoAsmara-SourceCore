#include "native/game.hpp"
#include <algorithm>
namespace rosh {
bool WorldSession::premium_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const Sender&send){
 bool gacha=cat==21&&(sub==2||sub==3||sub==4),overlay=false,buy=false;std::vector<std::string>args;
 if(cat==7&&sub==1&&body.size()>1&&body.size()<=4096&&body.back()==0&&std::find(body.begin(),body.end()-1,0)==body.end()-1){
  std::string text(body.begin(),body.end()-1);overlay=text.rfind("kgpshop ",0)==0||text.rfind("npcshop cash",0)==0;if(overlay)args=command_tokens(text);
 }
 if(!gacha&&!overlay)return false;
 try{
  if(!loaded_||character_.value("hp",0)<=0)throw std::runtime_error("Aguarde o mapa e mantenha o personagem vivo");
  const auto&account=identity_.identity.id;
  if(gacha){
   auto reply=sub==2?premium_->machines(body):sub==3?premium_->prizes(body):premium_->spin(account,eid(),body,event);send(21,sub,reply);if(sub==4)send_inventory(send,true);return true;
  }
  if((args.size()==2||args.size()==3)&&args[0]=="kgpshop"&&args[1]=="open"){for(auto&message:premium_->state(account,character_.at("cls")))quest_overlay(message,send);return true;}
  if(args==std::vector<std::string>{"kgpshop","history"}){quest_overlay(premium_->history(account),send);return true;}
  if(args.size()>=2&&args[0]=="npcshop"&&args[1]=="cashtopup"){send(3,0,staff_chat_reply(eid(),"Cash: utilize a pagina oficial do projeto."));return true;}
  buy=args.size()>=2&&args[0]=="npcshop"&&args[1]=="cashbuy";
  if(buy&&args.size()==4){
   auto number=[](const std::string&value){if(value.empty()||value.size()>10||value.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("Produto ou quantidade invalidos");return std::stoi(value);};
   auto result=premium_->buy(account,eid(),character_.at("cls"),number(args[2]),number(args[3]),event);send_inventory(send,true);quest_overlay(result,send);return true;
  }
  throw std::runtime_error("Pedido da loja invalido");
 }catch(const std::exception&e){
  std::string error=e.what();
  if(gacha){Bytes code(2);put16(code,0,error.find("insuficiente")!=std::string::npos?603:(error.find("space")!=std::string::npos||error.find("full")!=std::string::npos||error.find("espaco")!=std::string::npos)?604:605,true);send(2,11,code);}
  if(buy)quest_overlay({{"cmd","shop.bought"},{"ok",false},{"cents",premium_->cents(identity_.identity.id)},{"title","Compra nao realizada"},{"text",error}},send);
  send(3,0,staff_chat_reply(eid(),error));return true;
 }
}
}
