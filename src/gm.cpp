#include "gm.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace karos {
namespace {
std::vector<std::string> tokenize(const std::string& input) {
    std::istringstream in(input);
    std::vector<std::string> out;
    std::string word;
    while (in >> word) out.push_back(word);
    return out;
}
int as_int(const std::string& s, const std::string& label) {
    std::size_t used=0; long v=0;
    try { v=std::stol(s,&used,10); } catch (...) { throw std::invalid_argument(label+" must be an integer"); }
    if (used!=s.size() || v < -2147483647L || v > 2147483647L) throw std::invalid_argument(label+" is out of range");
    return static_cast<int>(v);
}
double as_double(const std::string& s, const std::string& label) {
    std::size_t used=0; double v=0;
    try { v=std::stod(s,&used); } catch (...) { throw std::invalid_argument(label+" must be numeric"); }
    if (used!=s.size() || !std::isfinite(v)) throw std::invalid_argument(label+" is invalid");
    return v;
}
} // namespace

GMResult GMService::execute(const std::string& actor_account, const std::string& command) {
    try {
        auto words = tokenize(command);
        if (!words.empty() && (words[0] == "//gm" || words[0] == "/gm")) words.erase(words.begin());
        if (words.empty() || words[0] == "help") {
            return {true,
              "GM: help | whoami | setgm <account> <player|gm|admin> | level <character> <1-110> | "
              "xp <character> <amount> | gold <character> <amount> | "
              "item <character> <item_id> <quantity> | tp <character> <map> <x> <y> <z> | "
              "spawn <npc_id> <map> <x> <y> <z> [quantity] | announce <message>"};
        }
        const std::string role = db_.gm_role(actor_account);
        if (words[0] == "whoami") return {true,"Account="+actor_account+"; role="+(role=="owner"?"Admin":role)};
        if (role != "gm" && role != "owner") {
            db_.audit(actor_account,"gm_denied","",command);
            return {false,"Access denied: this account does not have GM privileges."};
        }
        if (words[0] == "setgm") {
            if (role != "owner") return {false,"Admin role required to promote or demote GMs."};
            if (words.size()!=3) return {false,"Usage: //gm setgm <account> <player|gm|admin>"};
            db_.set_gm_role(words[1],words[2],actor_account);
            db_.audit(actor_account,"gm_set_role",words[1],words[2]);
            return {true,"Role for "+words[1]+" set to "+words[2]+"."};
        }
        if (words[0] == "level") {
            if(words.size()!=3) return {false,"Usage: //gm level <character> <1-110>"};
            int level=as_int(words[2],"level"); db_.set_level(words[1],level);
            db_.audit(actor_account,"gm_level",words[1],std::to_string(level));
            return {true,"Level saved for "+words[1]+". The live client still requires a protocol update."};
        }
        if (words[0] == "xp") {
            if(words.size()!=3) return {false,"Usage: //gm xp <character> <amount>"};
            auto ch=db_.get_character_by_name(words[1]); if(!ch) return {false,"Character not found."};
            int amount=as_int(words[2],"XP"); db_.grant_exp(ch->eid,amount);
            db_.audit(actor_account,"gm_xp",words[1],std::to_string(amount));
            return {true,"XP updated in database. Live client synchronization is not implemented."};
        }
        if (words[0] == "gold") {
            if(words.size()!=3) return {false,"Usage: //gm gold <character> <positive-or-negative-amount>"};
            auto ch=db_.get_character_by_name(words[1]); if(!ch) return {false,"Character not found."};
            int amount=as_int(words[2],"gold"); if(amount==0) return {false,"Amount must be non-zero."};
            db_.add_gold(ch->eid,amount); db_.audit(actor_account,"gm_gold",words[1],std::to_string(amount));
            return {true,"Gold updated in database. Visual updates require the network adapter."};
        }
        if (words[0] == "item") {
            if(words.size()!=4) return {false,"Usage: //gm item <character> <item_id> <quantity>"};
            auto ch=db_.get_character_by_name(words[1]); if(!ch) return {false,"Character not found."};
            int item=as_int(words[2],"item_id"), qty=as_int(words[3],"quantity");
            int slot=db_.add_item(ch->eid,item,qty);
            db_.audit(actor_account,"gm_item",words[1],"item="+std::to_string(item)+" qty="+std::to_string(qty));
            return {true,"Item saved to database in slot "+std::to_string(slot)+". Client inventory packet not implemented."};
        }
        if (words[0] == "tp") {
            if(words.size()!=6) return {false,"Usage: //gm tp <character> <map> <x> <y> <z>"};
            db_.set_position(words[1],words[2],as_double(words[3],"x"),as_double(words[4],"y"),as_double(words[5],"z"));
            db_.audit(actor_account,"gm_tp",words[1],words[2]+" "+words[3]+" "+words[4]+" "+words[5]);
            return {true,"Destination saved to database; in-game teleport requires the network adapter."};
        }
        if (words[0] == "spawn") {
            if(words.size()!=6 && words.size()!=7) return {false,"Usage: //gm spawn <npc_id> <map> <x> <y> <z> [quantity]"};
            int npc=as_int(words[1],"npc_id"); int count=words.size()==7?as_int(words[6],"quantity"):1;
            db_.queue_spawn(actor_account,npc,words[2],as_double(words[3],"x"),as_double(words[4],"y"),as_double(words[5],"z"),count);
            db_.audit(actor_account,"gm_spawn",std::to_string(npc),words[2]+" count="+std::to_string(count));
            return {true,"Spawn queued in gm_spawn_requests. World processing remains pending."};
        }
        if (words[0] == "announce") {
            if(words.size()<2) return {false,"Usage: //gm announce <message>"};
            std::string message; for(std::size_t i=1;i<words.size();++i){if(i>1)message+=' ';message+=words[i];}
            db_.add_announcement(actor_account,message); db_.audit(actor_account,"gm_announce","all",message);
            return {true,"Announcement queued; network broadcast remains pending."};
        }
        return {false,"Unknown command. Use //gm help."};
    } catch (const std::exception& e) {
        return {false,std::string("GM error: ")+e.what()};
    }
}

} // namespace karos
