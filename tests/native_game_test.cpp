#include "native/game.hpp"
#include <cmath>
#include <iostream>
using namespace rosh;
static void equal(const Json &actual, const Json &expected, const std::string &name) {
    if (expected.is_number()) {
        if (!actual.is_number() || std::abs(actual.get<double>() - expected.get<double>()) > 0.000001)
            throw std::runtime_error(name + ": expected " + expected.dump() + ", got " + actual.dump());
    } else if (expected.is_object()) {
        for (auto it = expected.begin(); it != expected.end(); ++it)
            equal(actual.at(it.key()), it.value(), name + "/" + it.key());
    } else if (expected.is_array()) {
        if (actual.size() != expected.size())
            throw std::runtime_error(name + ": array size");
        for (std::size_t i = 0; i < expected.size(); i++)
            equal(actual.at(i), expected.at(i), name + "/" + std::to_string(i));
    } else if (actual != expected)
        throw std::runtime_error(name + ": different value");
}
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("Usage: native game tests <encrypted root> <parity fixture>");
        Content content(argv[1]);
        auto fixture = Json::parse(read_file(argv[2]));
        auto profiles = content.get("data/balance_profiles.json");
        std::size_t count = 0;
        for (const auto &test : fixture.at("balance")) {
            auto result =
                calculated_stats(profiles, test.at("profession"), test.at("level"), test.at("modifiers"));
            equal(result, test.at("expected"), std::to_string(count++));
        }
        for (const auto &test : fixture.at("enhancement")) {
            auto result = enchant_row(test.at("original"), test.at("grade"));
            equal(result, test.at("expected"), "equipment/" + test.at("item").get<std::string>());
            count++;
        }
        Progression progression;
        for (int level = 1; level <= 120; level++) {
            if (progression.level(progression.threshold(level)) != level)
                throw std::runtime_error("XP level threshold mismatch");
            if (level > 1 && progression.level(progression.threshold(level) - 1) != level - 1)
                throw std::runtime_error("XP off-by-one");
        }
        std::cout << "PASS: " << count
                  << " Python formula parity cases; all 120 progression boundaries; native +0..+18 equipment "
                     "attributes\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
