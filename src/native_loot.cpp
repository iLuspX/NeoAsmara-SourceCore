#include "native/loot.hpp"
#include <algorithm>
#include <cmath>
namespace rosh {
static std::uint32_t map_crc(const std::string&name){std::uint32_t crc=0xffffffff;for(unsigned char c:name){crc^=c;for(int bit=0;bit<8;bit++)crc=(crc>>1)^(0xedb88320u&(~(crc&1)+1));}return ~crc;}
std::int64_t GroundLoot::integer(std::int64_t lo,std::int64_t hi){if(lo>hi)throw std::runtime_error("Invalid loot quantity range");return std::uniform_int_distribution<std::int64_t>(lo,hi)(random_);}
GroundLoot::GroundLoot(Sqlite&db,Content&content,Inventory&inventory):db_(db),inventory_(inventory),policy_(content.get("server/drop_policy.json")),distribution_(content.get("server/loot_distribution.json")),catalog_(content.get("server/respawn_catalog.json")),gear_(content.get("data/native_equipment_loot.json").at("items")),super_(content.get("data/super_skill_drops.json")){
 db_.exec("CREATE TABLE IF NOT EXISTS mob_kills(kill_id TEXT PRIMARY KEY,npc_id INTEGER NOT NULL,player_eid INTEGER NOT NULL,killed_at INTEGER NOT NULL);CREATE TABLE IF NOT EXISTS ground_items(id INTEGER PRIMARY KEY AUTOINCREMENT,kill_id TEXT NOT NULL,map_name TEXT NOT NULL,item_id INTEGER NOT NULL,quantity INTEGER NOT NULL CHECK(quantity>0),x REAL NOT NULL,y REAL NOT NULL,z REAL NOT NULL,owner_eid INTEGER NOT NULL,created_at INTEGER NOT NULL,expires_at INTEGER NOT NULL,picked_at INTEGER,instance_uid INTEGER,instance_created_at INTEGER);CREATE TABLE IF NOT EXISTS ground_item_merit(drop_id INTEGER NOT NULL,player_eid INTEGER NOT NULL,damage INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(drop_id,player_eid))");
 auto columns=db_.query("PRAGMA table_info(ground_items)");for(auto name:{"instance_uid","instance_created_at"}){bool exists=false;for(auto&r:columns)if(r.at("name")==name)exists=true;if(!exists)db_.exec(std::string("ALTER TABLE ground_items ADD COLUMN ")+name+" INTEGER");}
 auto rules=content.get("data/native_item_rules.json");for(auto it=rules.begin();it!=rules.end();++it)if(it.value().value("kind",0)==4)quest_only_.insert(static_cast<std::uint32_t>(std::stoul(it.key())));
}
static int scaled_drop_chance(int basis_points,double multiplier){
 return static_cast<int>(std::lround(std::clamp(basis_points*multiplier,0.,10000.)));
}
std::vector<std::pair<std::uint32_t,std::int64_t>> GroundLoot::distributed(const Entity&mob,const std::string&map,const std::set<std::uint32_t>&blocked,double drop_multiplier){
 auto record=catalog_.value(std::to_string(mob.npc_id),Json::object());bool neo=record.value("neo",false);auto category=record.value("category",std::string("normal"));auto level=record.value("level",mob.level);
 bool dungeon=record.value("dungeon_default",false);std::string lower=map;for(auto&c:lower)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));for(auto token:{"dungeon","cave","maze","labyrinth","sewer","catacomb","gelmenco","shulanak"})if(lower.find(token)!=std::string::npos)dungeon=true;
 int low=neo?distribution_["neo_equipment_levels"][0].get<int>():std::max(1,level-distribution_.at("level_window").get<int>());int high=neo?distribution_["neo_equipment_levels"][1].get<int>():level;
 int fallback=-1;std::vector<std::pair<std::uint32_t,Json>> pool;
 for(auto it=gear_.begin();it!=gear_.end();++it){int lv=it.value().at("level");if(lv<=high)fallback=std::max(fallback,lv);if(lv>=low&&lv<=high)pool.push_back({static_cast<std::uint32_t>(std::stoul(it.key())),it.value()});}
 if(pool.empty()&&fallback>=0)for(auto it=gear_.begin();it!=gear_.end();++it)if(it.value().at("level")==fallback)pool.push_back({static_cast<std::uint32_t>(std::stoul(it.key())),it.value()});
 std::map<int,std::map<int,std::vector<std::uint32_t>>> families;for(auto&[id,row]:pool)if(!blocked.count(id))families[row.at("family")][row.at("quality")].push_back(id);
 int chance=distribution_["equipment_chance_bp"].value(category,500);if(dungeon)chance=std::min(10000,chance*distribution_.at("dungeon_equipment_chance_multiplier").get<int>());
 std::vector<std::pair<std::uint32_t,std::int64_t>> result;
 for(int roll=0;roll<distribution_["equipment_rolls"].value(neo?"neo":category,1);roll++){
  if(families.empty()||integer(0,9999)>=scaled_drop_chance(chance,drop_multiplier))continue;auto family=families.begin();std::advance(family,integer(0,families.size()-1));int total=0;for(auto&[q,ids]:family->second)total+=std::max(1,distribution_["quality_weights"].value(std::to_string(q),1));auto ticket=integer(0,total-1);
  for(auto&[q,ids]:family->second){ticket-=std::max(1,distribution_["quality_weights"].value(std::to_string(q),1));if(ticket<0){std::sort(ids.begin(),ids.end());result.push_back({ids[integer(0,ids.size()-1)],1});break;}}
 }
 if(category=="boss")for(auto&mat:distribution_.at("boss_materials")){auto id=mat.at("id").get<std::uint32_t>();if(!blocked.count(id)&&integer(0,9999)<scaled_drop_chance(mat.at("chance_bp").get<int>(),drop_multiplier))result.push_back({id,1});}
 bool large=neo;for(auto id:distribution_.at("large_carats_boss_ids"))if(id==mob.npc_id)large=true;
 if(large&&!blocked.count(999999))for(int n=integer(distribution_["large_carats_piles"][0],distribution_["large_carats_piles"][1]);n>0;n--)result.push_back({999999,integer(distribution_["large_carats_quantity"][0],distribution_["large_carats_quantity"][1])});
 if(level>=super_.at("minimum_level").get<int>()&&integer(0,9999)<scaled_drop_chance(super_["chance_bp"].value(neo?"neo":category,15),drop_multiplier)){
  int lo=neo?4:level>=90&&category=="boss"?3:level>=60?2:1,hi=neo?6:level>=90&&category=="boss"?5:level>=60?4:3,total=0;std::vector<std::pair<std::uint32_t,int>> options;
  for(auto it=super_["items"].begin();it!=super_["items"].end();++it){int rank=it.value().at("rank");auto id=static_cast<std::uint32_t>(std::stoul(it.key()));if(rank>=lo&&rank<=hi&&!blocked.count(id)){int weight=std::max(1,super_["rank_weights"].at(std::to_string(rank)).get<int>());options.push_back({id,weight});total+=weight;}}
  if(total){auto ticket=integer(0,total-1);for(auto&[id,w]:options)if((ticket-=w)<0){result.push_back({id,1});break;}}
 }
 return result;
}
void GroundLoot::record(const std::string&kill,const Entity&mob,std::uint32_t owner,const std::string&scope,std::int64_t carats,double timestamp,const std::vector<std::pair<std::uint32_t,std::int64_t>>&quest_drops,double drop_bonus){
 const auto drop_multiplier=1+std::clamp(drop_bonus,0.,100.)/100.;
 Transaction tx(db_);db_.exec("INSERT INTO mob_kills VALUES(?,?,?,?)",{kill,std::int64_t(mob.npc_id),std::int64_t(owner),std::int64_t(timestamp)});
 std::map<std::uint32_t,Json> specific;for(auto&r:db_.query("SELECT * FROM drop_rules WHERE npc_id=? OR npc_id=0 ORDER BY npc_id DESC",{std::int64_t(mob.npc_id)})){auto id=r.at("item_id").get<std::uint32_t>();if(!specific.count(id))specific[id]=r;}
 if(const auto defaults=policy_.find("captured_drop_defaults");defaults!=policy_.end()){
  for(const auto &key:{std::to_string(mob.npc_id),std::string("0")}){
   const auto rules=defaults->find(key);
   if(rules==defaults->end())continue;
   for(const auto &rule:*rules)specific.try_emplace(rule.at("item_id").get<std::uint32_t>(),rule);
  }
 }
 if (mob.rules.contains("dev_drops")) {
  for (const auto& rule : mob.rules.at("dev_drops")) {
   specific[rule.at("item_id").get<std::uint32_t>()] = rule;
  }
 }
 std::set<std::uint32_t>blocked=quest_only_;
 std::vector<Json>rules;
 for(const auto &[id,rule]:specific){
  if(rule.at("enabled").get<int>()&&!quest_only_.count(id))rules.push_back(rule);
  blocked.insert(id);
 }
 std::shuffle(rules.begin(),rules.end(),random_);
 std::vector<std::pair<std::uint32_t,std::int64_t>> drops;for(auto&r:rules){if(drops.size()>=static_cast<std::size_t>(std::clamp(policy_.value("max_material_drops",6),1,30)))break;if(integer(0,9999)<scaled_drop_chance(r.at("chance_bp").get<int>(),drop_multiplier))drops.push_back({r.at("item_id"),integer(r.at("min_qty"),r.at("max_qty"))});}
 auto extras=distributed(mob,mob.map,blocked,drop_multiplier);drops.insert(drops.end(),extras.begin(),extras.end());drops.insert(drops.end(),quest_drops.begin(),quest_drops.end());
 if(drops.empty()){std::vector<std::uint32_t>junk;for(auto id:distribution_.at("junk_items"))if(!blocked.count(id.get<std::uint32_t>()))junk.push_back(id);if(!junk.empty())drops.push_back({junk[(map_crc(mob.map)+mob.npc_id+mob.level/10)%junk.size()],1});}
 bool large=catalog_.value(std::to_string(mob.npc_id),Json::object()).value("neo",false);for(auto id:distribution_.at("large_carats_boss_ids"))if(id==mob.npc_id)large=true;if(carats>0&&!large)drops.push_back({999999,carats});
 const double scatter = std::clamp(policy_.value("scatter_radius", 45.), 0., 80.);
 const double minimum = std::clamp(policy_.value("scatter_minimum", 10.), 0., scatter);
 constexpr double full_turn = 6.283185307179586;
 const auto phase = std::uniform_real_distribution<double>(0.,full_turn)(random_);
 for(std::size_t index=0;index<drops.size();++index){
  const auto [id,quantity]=drops[index];
  // Rotate the layout for each kill, keeping angular separation within that kill.
  const auto angle=phase+full_turn*index/drops.size();
  const auto radius = std::sqrt(std::uniform_real_distribution<double>(minimum * minimum, scatter * scatter)(random_));
  // Persist the coordinates at the same precision as the native appearance packet.
  const auto drop_x = static_cast<float>(mob.position.x + std::cos(angle) * radius);
  const auto drop_y = static_cast<float>(mob.position.y + std::sin(angle) * radius);
  const auto drop_z = static_cast<float>(mob.position.z);
  auto uid=inventory_.next_uid();inventory_.initial_affixes(uid,id);std::optional<std::int64_t>reused;
  auto sequence=db_.query("SELECT seq FROM sqlite_sequence WHERE name='ground_items'");
  if(!sequence.empty()&&sequence[0].at("seq").get<std::int64_t>()>=65535){
   // Recycle only records long after they ceased to be visible. Never wrap an
   // active actor ID or fail every subsequent kill after 65,535 lifetime drops.
   auto cutoff=std::int64_t(timestamp)-120;
   db_.exec("DELETE FROM ground_item_merit WHERE drop_id IN (SELECT id FROM ground_items WHERE expires_at<=? OR picked_at<=?)",{cutoff,cutoff});
   db_.exec("DELETE FROM ground_items WHERE expires_at<=? OR picked_at<=?",{cutoff,cutoff});
   auto free=db_.query("SELECT 1 AS id WHERE NOT EXISTS(SELECT 1 FROM ground_items WHERE id=1) UNION ALL SELECT g.id+1 FROM ground_items g WHERE g.id<65535 AND NOT EXISTS(SELECT 1 FROM ground_items n WHERE n.id=g.id+1) ORDER BY id LIMIT 1");
   if(free.empty())throw std::runtime_error("Todos os atores de drop estao ocupados; recompensa preservada");reused=free[0].at("id").get<std::int64_t>();
  }
  auto recipient=!quest_only_.count(id)&&mob.loot_owner?mob.loot_owner:owner;
  db_.exec("INSERT INTO ground_items(id,kill_id,map_name,item_id,quantity,x,y,z,owner_eid,created_at,expires_at,instance_uid,instance_created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",{reused?SqlValue(*reused):SqlValue(nullptr),kill,scope,std::int64_t(id),quantity,double(drop_x),double(drop_y),double(drop_z),std::int64_t(recipient),std::int64_t(timestamp),std::int64_t(timestamp)+120,std::int64_t(uid),std::int64_t(timestamp)});auto drop=db_.query("SELECT last_insert_rowid() AS id")[0].at("id").get<std::int64_t>();if(drop>65535)throw std::runtime_error("Ground actor namespace exhausted; kill transaction rolled back");
  std::uint64_t total=0;for(auto&[eid,damage]:mob.damage)total+=damage;
  db_.exec("INSERT INTO ground_item_merit VALUES(?,?,?)",{drop,std::int64_t(recipient),std::int64_t(mob.damage.count(recipient)?mob.damage.at(recipient):0)});
  if(!quest_only_.count(id)&&mob.loot_owner){for(auto member:mob.loot_recipients)if(member!=recipient)db_.exec("INSERT INTO ground_item_merit VALUES(?,?,?)",{drop,std::int64_t(member),std::int64_t(mob.damage.count(member)?mob.damage.at(member):0)});}
  else for(auto&[eid,damage]:mob.damage)if(eid!=recipient&&total&&static_cast<long double>(damage)/total>.6L)db_.exec("INSERT INTO ground_item_merit VALUES(?,?,?)",{drop,std::int64_t(eid),std::int64_t(std::min<std::uint64_t>(damage,0x7fffffffffffffffULL))});
 }tx.commit();
}
Json GroundLoot::pickup(std::uint32_t eid,const std::string&scope,const Json&pos,const Bytes&body,double timestamp){
 if(body.size()!=14||get32(body,0,true)!=eid||get16(body,12,true)!=0x005a)
  throw std::runtime_error("Invalid pickup identity or action");
 const auto id=get32(body,4,true),item=get32(body,8,true);
 if(!id||id>65535)throw std::runtime_error("Invalid ground actor");
 Transaction tx(db_);auto rows=db_.query("SELECT * FROM ground_items WHERE id=?",{std::int64_t(id)});if(rows.empty())throw std::runtime_error("Item nao encontrado");auto row=rows[0];
 if(!row.at("picked_at").is_null()||row.at("expires_at").get<double>()<=timestamp||row.at("map_name")!=scope||row.at("item_id")!=item)throw std::runtime_error("Item coletado, expirado ou de outro mapa");
 if(quest_only_.count(item)&&row.at("owner_eid")!=eid)throw std::runtime_error("Item de missao reservado ao personagem que possui o objetivo");
 if(row.at("owner_eid")!=eid&&timestamp<row.at("created_at").get<double>()+60&&db_.query("SELECT 1 FROM ground_item_merit WHERE drop_id=? AND player_eid=?",{std::int64_t(id),std::int64_t(eid)}).empty())throw std::runtime_error("Item reservado por merito durante60segundos");
 const double dx=pos.at(0).get<double>()-row.at("x").get<float>();
 const double dy=pos.at(1).get<double>()-row.at("y").get<float>();
 const double dz=pos.at(2).get<double>()-row.at("z").get<float>();
 const double distance=std::hypot(dx,dy,dz);
 const double pickup_range=std::clamp(policy_.value("pickup_distance",75.),1.,500.);
 if(!std::isfinite(distance)||distance>pickup_range)throw std::runtime_error("Personagem distante do item");
 auto quantity=row.at("quantity").get<std::int64_t>();
 if(item==999999){
  // The 16-bit ground actor is recycled. The kill and actor pair identifies
  // the specific pile, including older rows that have no instance UID.
  const auto event="ground-carats:"+hex(sha256(row.at("kill_id").get<std::string>()))+":"+std::to_string(id);
  Wallet wallet(db_,inventory_);
  wallet.apply(eid,quantity,event);
 }
 else {
  if(quantity<1||quantity>10000)throw std::runtime_error("Invalid ground stack");
  const auto uid=row.at("instance_uid").get<std::uint32_t>();
  const auto created=row.at("instance_created_at").is_null()?row.at("created_at").get<std::uint32_t>():row.at("instance_created_at").get<std::uint32_t>();
  const auto expiry=db_.query("SELECT expires_at FROM reward_instances WHERE uid=?",{std::int64_t(uid)});
  if(!expiry.empty()&&expiry[0].at("expires_at").get<double>()<=timestamp)
   throw std::runtime_error("Item temporario expirado");
  const auto definition=inventory_.definition(item);
  const auto duration=definition.value("duration",0);
  bool transferred=true;
  try {
   inventory_.insert(eid,item,1,{},uid,created);
  }catch(const InventoryFull&){
   // Ordinary stacks can use room in existing stacks even when every slot is occupied.
   if(duration>0||!expiry.empty()||definition.value("stack",1)<=1||definition.value("kind",0)==1||inventory_.traced(uid))
    throw;
   inventory_.insert(eid,item,static_cast<int>(quantity));
   transferred=false;
  }
  if(transferred){
   if(expiry.empty()&&duration>0)
    db_.exec("INSERT INTO reward_instances VALUES(?,?,?,?,?)",{std::int64_t(uid),std::int64_t(eid),std::int64_t(item),std::int64_t(timestamp)+duration,std::string("ground-pickup")});
   if(quantity>1)inventory_.insert(eid,item,static_cast<int>(quantity-1));
   db_.exec("UPDATE reward_instances SET player_eid=? WHERE uid=?",{std::int64_t(eid),std::int64_t(uid)});
  }
 }
 db_.exec("UPDATE ground_items SET picked_at=? WHERE id=? AND picked_at IS NULL",{std::int64_t(timestamp),std::int64_t(id)});tx.commit();return row;
}
std::map<std::uint32_t,Json> GroundLoot::visible(const std::string&scope,const Json&pos,double timestamp){std::map<std::uint32_t,Json>out;for(auto&row:db_.query("SELECT * FROM ground_items WHERE map_name=? AND picked_at IS NULL AND expires_at>? AND id BETWEEN 1 AND 65535",{scope,timestamp})){double d=std::hypot(pos[0].get<double>()-row.at("x").get<double>(),pos[1].get<double>()-row.at("y").get<double>());if(d<=3000)out[row.at("id").get<std::uint32_t>()]=row;}return out;}
Bytes GroundLoot::spawn(std::uint32_t eid,const Json &row) {
 Bytes packet(181);
 put32(packet,0,eid,true);
 put32(packet,4,row.at("id").get<std::uint32_t>(),true);
 // Both Genesis traces use this appearance header for normal monster drops.
 put32(packet,12,10,true);
 put32(packet,22,row.at("item_id"));
 put32(packet,28,row.at("instance_uid"));
 const auto created=row.at("instance_created_at").is_null()?row.at("created_at"):row.at("instance_created_at");
 put32(packet,32,created.get<std::uint32_t>());
 const auto quantity=row.at("quantity").get<std::uint64_t>();
 put32(packet,36,static_cast<std::uint32_t>(quantity));
 put32(packet,40,static_cast<std::uint32_t>(quantity>>32));
 // This is the item lifetime field. Ground expiry is enforced separately.
 put32(packet,76,4102444800u);
 putfloat(packet,167,row.at("x"));
 putfloat(packet,171,row.at("y"));
 putfloat(packet,175,row.at("z"));
 return packet;
}
Bytes GroundLoot::remove(std::uint32_t actor,std::uint32_t item){Bytes b(14);put32(b,0,0xffffffff,true);put32(b,4,actor,true);put32(b,8,item,true);put16(b,12,0x01d6,true);return b;}
}
