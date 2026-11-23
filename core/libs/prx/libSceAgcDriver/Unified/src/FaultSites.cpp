#include "prx/libSceAgcDriver/Unified/include/FaultSites.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace AgcDriver::Unified::FaultSites {

namespace {

struct Slot {
    std::atomic<std::uint64_t> site{0};
    std::atomic<std::uint64_t> count{0};
};

std::array<Slot, Slots>& slots() {
    static auto& table = *new std::array<Slot, Slots>();
    return table;
}

std::atomic<std::uint64_t> overflow{0};
thread_local std::uintptr_t pendingSite = 0;

void noteSite(std::uintptr_t site) {
    pendingSite = site;
}

std::string moduleOf(std::uint64_t site) {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(site), &module) == 0 || module == nullptr) return "?+" + std::to_string(site);
    char path[MAX_PATH] = {};
    const auto length = GetModuleFileNameA(module, path, MAX_PATH);
    std::string_view name(path, length);
    if (const auto slash = name.find_last_of("\\/"); slash != std::string_view::npos) name.remove_prefix(slash + 1);
    char text[160];
    std::snprintf(text, sizeof(text), "%.*s+%llx", static_cast<int>(name.size()), name.data(), static_cast<unsigned long long>(site - reinterpret_cast<std::uint64_t>(module)));
    return text;
#else
    char text[32];
    std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(site));
    return text;
#endif
}

}

bool Enabled() {
    static const bool on = std::getenv("APS5_RECODE_FAULT_SITES") != nullptr;
    return on;
}

void Install() {
#ifdef _WIN32
    if (Enabled()) GuestArena::GuestArenaSetGpuFaultSiteHook_nid_no_patch(noteSite);
#endif
}

void Note() {
    const auto site = static_cast<std::uint64_t>(pendingSite);
    pendingSite = 0;
    if (site == 0) return;
    auto& table = slots();
    auto index = static_cast<std::size_t>((site * 0x9E3779B97F4A7C15ull) >> 50) % Slots;
    for (std::size_t probe = 0; probe < 64; ++probe, index = (index + 1) % Slots) {
        auto current = table[index].site.load(std::memory_order_acquire);
        if (current == 0 && table[index].site.compare_exchange_strong(current, site, std::memory_order_acq_rel)) current = site;
        if (current == site) {
            table[index].count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    overflow.fetch_add(1, std::memory_order_relaxed);
}

std::string Line() {
    if (!Enabled()) return {};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> sites;
    std::uint64_t total = overflow.load(std::memory_order_relaxed);
    for (const auto& slot : slots()) {
        const auto site = slot.site.load(std::memory_order_acquire);
        if (site == 0) continue;
        const auto count = slot.count.load(std::memory_order_relaxed);
        total += count;
        sites.emplace_back(count, site);
    }
    std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::uint64_t top8 = 0;
    std::uint64_t top64 = 0;
    for (std::size_t i = 0; i < sites.size() && i < 64; ++i) {
        if (i < 8) top8 += sites[i].first;
        top64 += sites[i].first;
    }
    char head[256];
    std::snprintf(head, sizeof(head), "[recode-fault-sites] %llu write faults from %llu sites (%llu unplaced); top 8 %.1f %%, top 64 %.1f %%:",
        static_cast<unsigned long long>(total), static_cast<unsigned long long>(sites.size()), static_cast<unsigned long long>(overflow.load(std::memory_order_relaxed)),
        total != 0 ? 100.0 * static_cast<double>(top8) / static_cast<double>(total) : 0.0, total != 0 ? 100.0 * static_cast<double>(top64) / static_cast<double>(total) : 0.0);
    std::string line = head;
    for (std::size_t i = 0; i < sites.size() && i < Shown; ++i) line += " " + moduleOf(sites[i].second) + "=" + std::to_string(sites[i].first);
    return line + "\n";
}

}
