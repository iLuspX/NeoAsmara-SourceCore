#include "native/game.hpp"
#include <cmath>
namespace rosh {
bool WorldSession::active_skill_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const Sender&send){
 if(cat!=5||!loaded_)return false;
 // Native kind 2 uses C05/03 too, but is an animation, not a combat cast.
 if(sub==3&&body.size()>=9&&get32(body,5,true)>=11&&get32(body,5,true)<=14){
  try{
   if(body.size()!=36||body[0]!=0||get32(body,1,true)!=eid()||get16(body,34,true)!=0x24)
    throw std::runtime_error("Invalid native emotion request");
   const auto pending=casting_->state();
   if(character_.value("hp",0)<=0||mounted_||mount_pending_||vanished_||pending_craft_||
      (!pending.empty()&&pending.value("expires",0.)>now()))throw std::runtime_error("Character cannot use an emotion now");
   if(body[29]!=0||get32(body,30,true)!=eid())throw std::runtime_error("An emotion requires the character as its own target");
   for(int axis=0;axis<4;++axis)if(!std::isfinite(getfloat(body,13+4*axis)))
    throw std::runtime_error("Invalid emotion coordinates");
   const auto list=skills_->list(character_,templates_.at(0x0505));const auto id=get32(body,5,true),rank=get32(body,9,true);
   bool known=false;
   for(unsigned index=0;index<list[5];++index)if(get32(list,6+index*4)==id&&get32(list,6+list[5]*4+index*4)==rank&&rank==1)known=true;
   if(!known)throw std::runtime_error("Unknown native emotion or rank");
   stand(send);auto reply=body;
   for(int axis=0;axis<3;++axis)putfloat(reply,13+axis*4,character_.at("pos").at(axis));
   putfloat(reply,25,character_.at("angle"));
   send(5,3,reply);scene_->action(scene_key(),5,3,reply);return true;
  }catch(const std::exception& error){
   if(body.size()==36&&get32(body,1,true)==eid()){
    auto reply=body;put16(reply,34,0x21,true);send(5,3,reply);
   }
   send(11,1,notice(error.what()));return true;
  }
 }
 if(sub==0&&body.size()>5&&(body[5]==1||body[5]==5)){
  try{
   auto target=attack_target(body,sub,eid());if(character_.value("hp",0)<=0||vanished_)throw std::runtime_error("Personagem nao pode atacar");
   if(body[5]==1){auto pending=casting_->state();if(pending.empty()||pending.at("expires").get<double>()<now()||(!pending.value("area",false)&&pending.at("target")!=target))throw std::runtime_error("Animacao de habilidade sem cast autorizado");}
   auto reply=body;for(int i=0;i<3;i++)putfloat(reply,10+i*4,character_["pos"][i]);putfloat(reply,22,character_["angle"]);send(5,0,reply);scene_->action(scene_key(),5,0,reply);
  }catch(const std::exception&e){send(11,1,notice(e.what()));}return true;
 }
 if(sub!=3&&sub!=4)return false;
 auto original=character_,originalStats=stats_;bool committed=false;
 try{
  if(sub==4){
   if(body.size()!=25||body[0]!=0||get32(body,1,true)!=eid())throw std::runtime_error("Cancelamento de habilidade invalido");
   casting_->cancel();auto reply=body;put32(reply,5,0xffffffff,true);for(int i=0;i<3;i++)putfloat(reply,9+i*4,character_["pos"][i]);putfloat(reply,21,character_["angle"]);send(cat,sub,reply);scene_->action(scene_key(),cat,sub,reply);return true;
  }
  if(vanished_)throw std::runtime_error("Desative vanish para usar habilidades");
  if(event.empty()||event.size()>200)throw std::runtime_error("Identificador de operacao ausente");
  store_.db().exec("CREATE TABLE IF NOT EXISTS native_active_skill_operations(event_key TEXT PRIMARY KEY,eid INTEGER NOT NULL,request TEXT NOT NULL,response TEXT NOT NULL)");
  Transaction tx(store_.db());auto prior=store_.db().query("SELECT * FROM native_active_skill_operations WHERE event_key=?",{event});Bytes reply;Json profile;
  if(!prior.empty()){
   if(prior[0].at("eid")!=eid()||prior[0].at("request")!=hex(body))throw std::runtime_error("Pedido repetido com dados diferentes");reply=unhex(prior[0].at("response"));
  }else{
   scene_entities_=scene_->map(scene_map_name());
   const auto skill_id=body.size()==36?get32(body,5,true):0u;
   const bool sacrifice=ActiveSkills::sacrifice_healing(skill_id);
   const bool friendly_target=(ActiveSkills::targeted_healing(skill_id)||sacrifice)&&
       body[29]==0&&scene_->can_heal_player(scene_key(),get32(body,30,true),sacrifice);
   profile=casting_->admit(character_,stats_,*scene_entities_,*skills_,body,now(),
       pk_mode_&&character_.value("level",1)>35,friendly_target);
   reply=body;for(int i=0;i<3;i++)putfloat(reply,13+i*4,character_["pos"][i]);putfloat(reply,25,character_["angle"]);
   recalculate();persist();store_.db().exec("INSERT INTO native_active_skill_operations VALUES(?,?,?,?)",{event,std::int64_t(eid()),hex(body),hex(reply)});
  }
  tx.commit();committed=true;stand(send);
  if(!profile.is_null()&&profile.at("kind")==13&&!profile.value("area",false))scene_->skill_effects(scene_key(),profile.at("target"),profile,now());
  if(profile.contains("healing")){
   const auto& heal=profile.at("healing");
   const auto target=profile.at("target").get<std::uint32_t>();
   if(target==eid())scene_->heal_party(scene_key(),heal[0],heal[1],heal[2]);
   else scene_->heal_player(scene_key(),target,heal[0],heal[1],heal[2],
       ActiveSkills::sacrifice_healing(profile.at("id")));
  }
  if(profile.contains("defense_effects"))
   scene_->place_defense_radar(scene_key(),profile.at("defense_aura"),now(),profile.at("duration"),profile.at("defense_effects"));
  if(profile.contains("radar")){
   const auto& radar=profile.at("radar");
   scene_->place_healing_radar(scene_key(),now(),profile.at("duration"),radar[0],radar[1]);
  }
  if(character_.at("hp")!=original.at("hp"))send(8,0,gauge(0x0800));
  send(8,1,gauge(0x0801));send(cat,sub,reply);if(prior.empty())scene_->action(scene_key(),cat,sub,reply);send(8,2,stats_packet());publish_player(send);
 }catch(const std::exception&e){if(committed)throw;character_=std::move(original);stats_=std::move(originalStats);casting_->cancel();Bytes cancel(25);put32(cancel,1,eid(),true);put32(cancel,5,0xffffffff,true);for(int i=0;i<3;i++)putfloat(cancel,9+i*4,character_["pos"][i]);putfloat(cancel,21,character_["angle"]);send(5,4,cancel);send(11,1,notice(e.what()));}
 return true;
}
}
