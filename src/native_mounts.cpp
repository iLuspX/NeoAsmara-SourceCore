#include "native/game.hpp"
#include "native/inventory.hpp"
#include <cmath>
namespace rosh {
void WorldSession::stand(const Sender&send){
 if(!sitting_)return;
 sitting_=false;Bytes body(11);put32(body,0,eid(),true);
 send(3,2,body);scene_->action(scene_key(),3,2,body);publish_player(send);
}
double WorldSession::mounted_speed(){
 if(!mounted_||character_.value("hp",0)<=0)return 0;
 const auto mounts=content_.get("data/native_mounts.json");
 for(const auto&item:inventory_->items(eid(),true))
  if(item.slot==6&&item.uid==mount_uid_&&(!item.expires||*item.expires>now())&&mounts.contains(std::to_string(item.item)))
   return mounts.at(std::to_string(item.item)).at("move_speed");
 return 0;
}
void WorldSession::clear_mount(){mounted_=false;mount_pending_=false;mount_uid_=0;mount_action_.clear();}
bool WorldSession::mount_request(unsigned char cat,unsigned char sub,const Bytes&body,const Sender&send){
 // AMo CCommunityActionPacket 00649B65: mode 2 calls CHero::mount;
 // 05/04 only ends the cast. Captured frames 13196/13202 carry this handshake.
 if(cat==3&&sub==2){
  try{
   if(!loaded_||body.size()!=11||get32(body,0,true)!=eid())throw std::runtime_error("Invalid character action");
   const auto mode=body[4];
   if(mode>2)throw std::runtime_error("Unknown native character action");
   const auto cast=casting_->state();
   if(mode==1&&(character_.value("hp",0)<=0||mounted_||mount_pending_||(!cast.empty()&&cast.value("expires",0.)>now())))
    throw std::runtime_error("Character cannot sit during this action");
   if(mode==2&&(!mounted_||mounted_speed()<=0||character_.value("hp",0)<=0))throw std::runtime_error("Mount activation has not completed");
   if(mode==0&&(mounted_||mount_pending_)){clear_mount();recalculate();send(8,2,stats_packet());send(8,1,gauge(0x0801));}
   sitting_=mode==1;
   Bytes reply(11);put32(reply,0,eid(),true);reply[4]=mode;
   if(mode==2)put32(reply,6,eid(),true);
   send(3,2,reply);scene_->action(scene_key(),3,2,reply);publish_player(send);
  }catch(const std::exception&e){send(11,1,notice(e.what()));}
  return true;
 }
 if(cat!=5||(sub!=3&&sub!=4)||body.size()<9||get32(body,5,true)!=5)return false;
 try{
  if(!loaded_||character_.value("hp",0)<=0||vanished_)throw std::runtime_error("Personagem deve estar vivo e com mapa carregado para montar");
  if(sub==4){
   if(body.size()!=25||body[0]!=0||get32(body,1,true)!=eid())throw std::runtime_error("Conclusao de montaria invalida");
   for(int i=0;i<4;i++)if(!std::isfinite(getfloat(body,9+i*4)))throw std::runtime_error("Posicao de montaria invalida");
   if(mount_pending_&&now()<=mount_pending_until_){
    auto reply=body;for(int i=0;i<3;i++)putfloat(reply,9+i*4,character_["pos"][i]);putfloat(reply,21,character_["angle"]);
    std::uint32_t owned=0;
    for(const auto& item:inventory_->items(eid(),true))if(item.slot==6&&item.uid==mount_uid_&&(!item.expires||*item.expires>now()))owned=item.uid;
    if(!owned)throw std::runtime_error("A montaria equipada mudou ou expirou durante a chamada");
    mounted_=!mounted_;if(!mounted_)mount_uid_=0;mount_pending_=false;recalculate();
    send(5,4,reply);scene_->action(scene_key(),5,4,reply);
    // Retail capture 13199 -> 13204 -> 13207: finish, hero parameters, gauge.
    send(8,2,stats_packet());send(8,1,gauge(0x0801));publish_player(send);
   }
   return true;
  }
  if(body.size()!=36||body[0]!=0||get32(body,1,true)!=eid()||get32(body,9,true)>1||body[29]!=0||get32(body,30,true)!=eid()||get16(body,34,true)!=0x24)throw std::runtime_error("Acao de montaria invalida");
  for(int i=0;i<4;i++)if(!std::isfinite(getfloat(body,13+i*4)))throw std::runtime_error("Posicao de montaria invalida");
  if(mount_pending_||now()<mount_cooldown_)throw std::runtime_error("Aguarde a troca de montaria terminar");
  auto mounts=content_.get("data/native_mounts.json");std::uint32_t uid=0;
  for(const auto&item:inventory_->items(eid(),true))if(item.slot==6&&(!item.expires||*item.expires>now())&&mounts.contains(std::to_string(item.item)))uid=item.uid;
  if(!uid)throw std::runtime_error("Equipe uma montaria valida no slot de ombro");
  stand(send);mount_uid_=uid;mount_pending_=true;mount_pending_until_=now()+10;mount_cooldown_=now()+1;
  mount_action_=body;
  // 2026-10-28 Genesis capture: client sends 0, server acknowledges 1.
  // This is an action flag, never a mount model/item ID. Slot 6 selects the model.
  put32(mount_action_,9,1,true);
  auto pos=character_.at("pos");for(int i=0;i<3;i++)putfloat(mount_action_,13+4*i,pos[i]);putfloat(mount_action_,25,character_.at("angle"));
  recalculate();
  // Captured gauge -> action -> skill5 completion. Recreating the actor here
  // destroys the client's riding transition.
  send(8,1,gauge(0x0801));send(5,3,mount_action_);scene_->action(scene_key(),5,3,mount_action_);publish_player(send);
 }catch(const std::exception&e){send(11,1,notice(e.what()));}
 return true;
}
}
