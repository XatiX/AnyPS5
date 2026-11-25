#include "prx/libSceAgcDriver/Unified/include/RouteDefaults.hpp"
#include <cstdlib>
#include <string_view>
#include <utility>

namespace AgcDriver::Unified {

namespace {

bool routeOff() {
    static const bool off = [] {
        const char* value = std::getenv("APS5_RECODE_ROUTE");
        return value != nullptr && std::string_view(value) == "0";
    }();
    return off;
}

}

const char* RouteValue(const char* name) {
    if (const char* value = std::getenv(name)) return value;
    if (routeOff()) return nullptr;
    constexpr std::pair<std::string_view, const char*> defaults[] = {
        {"APS5_RECODE_DRAW", "record"},
        {"APS5_RECODE_IMAGES", "1"},
        {"APS5_RECODE_MEMORY", "draws"},
        {"APS5_RECODE_PROOF", "1"},
        {"APS5_RECODE_RECORD", "1"},
        {"APS5_RECODE_TABLES", "1"},
        {"APS5_RECODE_DISPATCH", "all"},
        {"APS5_RECODE_MIRROR_INPUTS", "1"},
    };
    for (const auto& [key, value] : defaults) {
        if (key == name) return value;
    }
    return nullptr;
}

}
