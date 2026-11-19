#pragma once
#include "database.hpp"
#include <string>

namespace karos {

struct GMResult {
    bool ok{false};
    std::string message;
};

class GMService {
public:
    explicit GMService(Database& db) : db_(db) {}
    GMResult execute(const std::string& actor_account, const std::string& command);
private:
    Database& db_;
};

} // namespace karos
