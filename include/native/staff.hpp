#pragma once
#include "core.hpp"
#include <optional>
namespace rosh {
std::string client_text(unsigned char sub,const Bytes&body,std::uint32_t eid);
std::vector<std::string> command_tokens(const std::string&text);
std::string canonical_staff_command(const std::string&text);
std::optional<std::string> normalized_native_command(unsigned char sub,const std::string&text);
Bytes staff_chat_reply(std::uint32_t eid,const std::string&text);
class StaffChannel {
 Sqlite &db_;
 public:
 explicit StaffChannel(Sqlite&db);
 std::int64_t latest();
 void post(const std::string&role,const std::string&account,std::uint32_t eid,const std::string&name,const std::string&text,const std::string&event,double timestamp);
 Json poll(std::int64_t after);
};
struct CommandPolicy { int level,min_args,max_args; };
const std::map<std::string,CommandPolicy>& staff_command_policies();
void authorize_staff_command(const std::string&role,const std::string&command,std::size_t arguments);
class StaffSession {
 Sqlite roles_,passwords_;
 std::string account_,generation_;
 std::uint32_t eid_;
 std::string grant();
 public:
 StaffSession(const fs::path &root,const std::string &account,std::uint32_t eid);
 bool valid();
 std::string role();
 std::optional<std::string> authentication(const std::vector<std::string>&parts);
};
}
