#pragma once
#include "core.hpp"
#include <optional>
namespace rosh {
class NativeControl {
 fs::path root_;
 Sqlite db_;
 bool authorized(const Json&row);
 void announce(const std::string&text);
 public:
 explicit NativeControl(const fs::path&root);
 int save_all(const std::string&group,const std::string&account,std::uint32_t eid,const std::string&generation,const std::string&except="");
 void poll_save(const std::string&session,const std::function<void()>&save);
 void schedule(const std::string&action,int delay,const std::string&account,std::uint32_t eid,const std::string&generation);
 bool cancel();
 std::optional<std::string> poll(double timestamp);
};
}
