#include "prx/libSceAgcDriver/Unified/include/UploadClash.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string_view>

namespace AgcDriver::Unified::UploadClash {

namespace {

struct Written {
    std::uint64_t end = 0;
    std::uint64_t program = 0;
    std::chrono::steady_clock::time_point at;
};

struct State {
    std::mutex mutex;
    std::map<std::uint64_t, Written> writes;
    std::uint64_t longest = 0;
    std::uint64_t checks = 0;
    std::uint64_t clashes = 0;
    std::uint64_t clashBytes = 0;
    std::uint64_t reported = 0;
    std::map<std::uint64_t, std::uint64_t> tracked;
    std::uint64_t longestTracked = 0;
    std::uint64_t untracked = 0;
    std::uint64_t untrackedBytes = 0;
    std::uint64_t untrackedReported = 0;
};

bool covered(const State& shared, std::uint64_t begin, std::uint64_t end) {
    const auto from = begin > shared.longestTracked ? begin - shared.longestTracked : 0u;
    auto cursor = begin;
    for (auto at = shared.tracked.lower_bound(from); at != shared.tracked.end() && at->first <= cursor && cursor < end; ++at) {
        if (at->second > cursor) cursor = at->second;
    }
    return cursor >= end;
}

State& state() {
    static auto& shared = *new State();
    return shared;
}

}

bool Active() {
    static const bool active = [] {
        const char* value = std::getenv("APS5_RECODE_UPLOAD_CLASH");
        return value != nullptr && std::string_view(value) != "0";
    }();
    return active;
}

void NoteWrite(std::uint64_t address, std::uint64_t size, std::uint64_t program) {
    if (!Active() || size == 0) return;
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    if (shared.writes.size() >= HeldWrites && !shared.writes.contains(address)) shared.writes.erase(shared.writes.begin());
    shared.writes[address] = {address + size, program, std::chrono::steady_clock::now()};
    shared.longest = std::max(shared.longest, size);
}

void NoteTracked(std::uint64_t address, std::uint64_t size) {
    if (!Active() || size == 0) return;
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    if (shared.tracked.size() >= HeldWrites && !shared.tracked.contains(address)) shared.tracked.erase(shared.tracked.begin());
    auto& end = shared.tracked[address];
    end = std::max(end, address + size);
    shared.longestTracked = std::max(shared.longestTracked, size);
}

void CheckUploads(std::span<const DirtyRange> ranges) {
    if (!Active() || ranges.empty()) return;
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    const auto now = std::chrono::steady_clock::now();
    for (const auto& range : ranges) {
        ++shared.checks;
        const auto end = range.address + range.size;
        const auto from = range.address > shared.longest ? range.address - shared.longest : 0u;
        for (auto at = shared.writes.lower_bound(from); at != shared.writes.end() && at->first < end; ++at) {
            if (at->second.end <= range.address) continue;
            const auto begin = std::max(at->first, range.address);
            const auto stop = std::min(at->second.end, end);
            ++shared.clashes;
            shared.clashBytes += stop - begin;
            const bool known = covered(shared, begin, stop);
            if (!known) {
                ++shared.untracked;
                shared.untrackedBytes += stop - begin;
            }
            if (!known && shared.untrackedReported++ < Reports) {
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - at->second.at).count();
                std::fprintf(stderr, "[recode-clash] UNTRACKED: CPU upload 0x%llx+0x%llx over the GPU write of program %016llx to 0x%llx+0x%llx (never noted to the memory core), noted %lld ms before\n", static_cast<unsigned long long>(range.address), static_cast<unsigned long long>(range.size), static_cast<unsigned long long>(at->second.program), static_cast<unsigned long long>(at->first), static_cast<unsigned long long>(at->second.end - at->first), static_cast<long long>(age));
            }
            if (shared.reported++ < Reports) {
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - at->second.at).count();
                std::fprintf(stderr, "[recode-clash] CPU upload 0x%llx+0x%llx over the GPU write of program %016llx to 0x%llx+0x%llx noted %lld ms before\n", static_cast<unsigned long long>(range.address), static_cast<unsigned long long>(range.size), static_cast<unsigned long long>(at->second.program), static_cast<unsigned long long>(at->first), static_cast<unsigned long long>(at->second.end - at->first), static_cast<long long>(age));
            }
        }
    }
}

std::string Line() {
    if (!Active()) return {};
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    return "[recode-clash] CPU-dirty ranges checked " + std::to_string(shared.checks) + ", overlapping a recorded GPU write " + std::to_string(shared.clashes) + " (" + std::to_string(shared.clashBytes >> 10) + " KiB), over a write never noted to the memory core " + std::to_string(shared.untracked) + " (" + std::to_string(shared.untrackedBytes >> 10) + " KiB); GPU write ranges held " + std::to_string(shared.writes.size()) + "\n";
}

}
