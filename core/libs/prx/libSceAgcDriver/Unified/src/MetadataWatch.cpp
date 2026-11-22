#include "prx/libSceAgcDriver/Unified/include/MetadataWatch.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string_view>

namespace AgcDriver::Unified::MetadataWatch {

namespace {

struct Range {
    std::uint64_t end = 0;
    std::uint64_t target = 0;
    bool dcc = false;
};

struct State {
    std::mutex mutex;
    std::map<std::uint64_t, Range> ranges;
    std::uint64_t longest = 0;
    std::set<std::uint64_t> fastClearTargets;
    std::set<std::uint64_t> dccTargets;
    std::uint64_t driverWrites = 0;
    std::uint64_t gpuWrites = 0;
    std::uint64_t reported = 0;
};

State& state() {
    static auto& shared = *new State();
    return shared;
}

void add(State& shared, std::uint64_t address, std::uint64_t bytes, std::uint64_t target, bool dcc) {
    if (address == 0u || bytes == 0u) return;
    shared.ranges[address] = {address + bytes, target, dcc};
    shared.longest = std::max(shared.longest, bytes);
}

}

bool Active() {
    static const bool active = [] {
        const char* value = std::getenv("APS5_RECODE_METADATA_WATCH");
        return value != nullptr && std::string_view(value) != "0";
    }();
    return active;
}

void NoteTarget(const Graphics::ColorTarget& color) {
    if (!Active()) return;
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    add(shared, color.dccAddress, color.bytes / 256u + 1u, color.address, true);
    add(shared, color.cmaskAddress, color.cmaskBytes, color.address, false);
    if (color.dccAddress != 0u) shared.dccTargets.insert(color.address);
    if (color.cmaskFastClear) shared.fastClearTargets.insert(color.address);
}

void CheckWrite(std::uint64_t address, std::uint64_t size, bool driver, std::uint64_t program) {
    if (!Active() || size == 0u) return;
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    const auto from = address > shared.longest ? address - shared.longest : 0u;
    for (auto at = shared.ranges.lower_bound(from); at != shared.ranges.end() && at->first < address + size; ++at) {
        if (at->second.end <= address) continue;
        ++(driver ? shared.driverWrites : shared.gpuWrites);
        if (shared.reported++ < Reports) std::fprintf(stderr, "[recode-metadata] %s write 0x%llx+0x%llx over the %s of target 0x%llx (0x%llx+0x%llx), program %016llx\n", driver ? "driver" : "GPU", static_cast<unsigned long long>(address), static_cast<unsigned long long>(size), at->second.dcc ? "DCC" : "CMASK", static_cast<unsigned long long>(at->second.target), static_cast<unsigned long long>(at->first), static_cast<unsigned long long>(at->second.end - at->first), static_cast<unsigned long long>(program));
        return;
    }
}

std::string Line() {
    if (!Active()) return {};
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    return "[recode-metadata] targets with DCC " + std::to_string(shared.dccTargets.size()) + ", with fast clear enabled " + std::to_string(shared.fastClearTargets.size()) + "; writes over their DCC or CMASK: by the driver " + std::to_string(shared.driverWrites) + ", by recorded GPU work " + std::to_string(shared.gpuWrites) + "\n";
}

}
