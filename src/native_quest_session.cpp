#include "native/game.hpp"
#include <sstream>
#include <limits>
namespace rosh {
void WorldSession::quest_overlay(const Json&message,const Sender&send){
 auto stage=content_.get("server/stage_templates.json").at("game");auto body=unhex(stage.at("020b"));if(body.size()<14)throw std::runtime_error("Invalid overlay carrier");body.resize(14);put32(body,0,103,true);auto raw=message.dump();if(raw.size()>65535)throw std::runtime_error("Quest UI exceeds native limit");body.insert(body.end(),raw.begin(),raw.end());body.push_back(0);send(2,11,body);
}
void WorldSession::sync_quests(const Sender&send,bool full,bool initial){
 if(!loaded_||!quests_)return;
 {Transaction tx(store_.db());quests_->synchronize(now());tx.commit();}
 if(initial&&!quest_snapshot_sent_){send(9,2,quests_->list_packet());send(9,4,quests_->completed_packet(now()));quest_snapshot_sent_=true;full=true;}
 for(auto&update:quests_->progress_updates())send(9,3,update);
 auto state=quests_->ui_state(character_.at("level"),full,now());auto raw=state.dump();auto&cache=full?quest_full_cache_:quest_held_cache_;if(raw!=cache){quest_overlay(state,send);cache=raw;}
}
bool WorldSession::quest_request(unsigned char cat,unsigned char sub,const Bytes&body,const std::string&event,const Sender&send){
 bool native=cat==9&&(sub==0||sub==1||sub==3),overlay=cat==7&&sub==1;
 if(!native&&!overlay)return false;
 if(overlay&&(body.size()<6||body.size()>4096||body.back()!=0||std::string(body.begin(),body.begin()+6)!="quest "))return false;
 auto before=character_,beforeStats=stats_;bool committed=false;
 try{
  if(!loaded_||character_.value("hp",0)<=0)throw std::runtime_error("Aguarde o mapa e mantenha o personagem vivo");
  if(overlay){
   if(std::find(body.begin(),body.end()-1,0)!=body.end()-1)throw std::runtime_error("Pedido de interface invalido");std::string text(body.begin(),body.end()-1);auto args=command_tokens(text);
   if(args==std::vector<std::string>{"quest","get"}){quest_full_cache_.clear();sync_quests(send,true);return true;}
   if(args.size()==3&&args[0]=="quest"&&args[1]=="drop"){
    std::size_t used=0;int id=std::stoi(args[2],&used);if(used!=args[2].size()||id<=0)throw std::runtime_error("ID de missao invalido");quests_->abandon(id);quest_overlay({{"cmd","quests.said"},{"word","dropped"}},send);quest_full_cache_.clear();sync_quests(send,true);return true;
   }throw std::runtime_error("Pedido de missao desconhecido");
  }
  Transaction tx(store_.db());bool changed=false;
  if(sub==0)changed=quests_->accept(body,character_,*scene_entities_,event,now());
  else if(sub==3)changed=quests_->talk(body,character_,*scene_entities_);
  else{
   auto reward=quests_->complete(body,character_,*scene_entities_,event,now());changed=true;
   if(!reward.at("replay").get<bool>()){
    auto amount=reward.at("xp").get<std::uint64_t>(),xp=character_.value("exp",std::uint64_t(0));if(amount>std::numeric_limits<std::uint64_t>::max()-xp)throw std::runtime_error("Experience overflow");int old=character_.at("level");character_["exp"]=xp+amount;int level=std::max(old,progression_.level(xp+amount));character_["level"]=level;character_["skill_points"]=character_.value("skill_points",0)+level-old;recalculate();if(level>old){character_["hp"]=character_.at("maxhp");character_["mp"]=character_.at("maxmp");}persist();
   }
  }
  tx.commit();committed=true;if(changed)send(9,sub,body);
  if(sub==1){send_inventory(send,true);send(8,2,stats_packet());send(8,0,gauge(0x0800));send(8,1,gauge(0x0801));send(9,4,quests_->completed_packet(now()));}
  sync_quests(send,sub!=3);
 }catch(const std::exception&e){if(committed)throw;character_=before;stats_=beforeStats;send(11,1,notice(e.what()));}
 return true;
}
}
