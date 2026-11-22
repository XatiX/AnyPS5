#include "prx/libSceAgcDriver/Unified/include/Protector.hpp"
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace AgcDriver::Unified {

namespace {

std::uint64_t pageDown(std::uint64_t address) {
    return address & ~(PageBytes - 1);
}

std::uint64_t pageUp(std::uint64_t address) {
    return (address + PageBytes - 1) & ~(PageBytes - 1);
}

}

void Protector::NeverArm(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    auto begin = pageDown(address);
    auto end = pageUp(address + size);
    auto it = excluded.upper_bound(begin);
    if (it != excluded.begin()) {
        auto previous = std::prev(it);
        if (previous->second >= begin) {
            begin = previous->first;
            end = std::max(end, previous->second);
            it = excluded.erase(previous);
        }
    }
    while (it != excluded.end() && it->first <= end) {
        end = std::max(end, it->second);
        it = excluded.erase(it);
    }
    excluded.emplace(begin, end);
}

void Protector::AllowArm(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    const auto begin = pageDown(address);
    const auto end = pageUp(address + size);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> kept;
    for (auto it = excluded.begin(); it != excluded.end();) {
        if (it->second <= begin || it->first >= end) {
            ++it;
            continue;
        }
        if (it->first < begin) kept.emplace_back(it->first, begin);
        if (it->second > end) kept.emplace_back(end, it->second);
        it = excluded.erase(it);
    }
    for (const auto& [first, last] : kept) excluded.emplace(first, last);
}

bool Protector::NeverArmed(std::uint64_t address) const {
    std::lock_guard lock(mutex);
    auto it = excluded.upper_bound(address);
    if (it == excluded.begin()) return false;
    --it;
    return address < it->second;
}

bool Protector::Readable(std::uint64_t address) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT) return false;
    const auto access = memory.Protect & 0xffu;
    return (memory.Protect & PAGE_GUARD) == 0 && access != PAGE_NOACCESS && access != 0;
#else
    static_cast<void>(address);
    return false;
#endif
}

const char* Protector::RegionKind(std::uint64_t address) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory)) return "unknown";
    if (memory.State != MEM_COMMIT) return memory.State == MEM_RESERVE ? "reserved" : "free";
    return memory.Type == MEM_MAPPED ? "mapped" : memory.Type == MEM_PRIVATE ? "private" : memory.Type == MEM_IMAGE ? "image" : "other";
#else
    static_cast<void>(address);
    return "unknown";
#endif
}

bool Protector::SectionBacked(std::uint64_t address) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT && memory.Type == MEM_MAPPED;
#else
    static_cast<void>(address);
    return false;
#endif
}

std::vector<DirtyRange> Protector::ReadableRanges(std::uint64_t address, std::uint64_t size) {
    std::vector<DirtyRange> readable;
#ifdef _WIN32
    const auto end = address + size;
    for (auto cursor = address; cursor < end;) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory)) break;
        const auto stop = std::min<std::uint64_t>(end, reinterpret_cast<std::uint64_t>(memory.BaseAddress) + memory.RegionSize);
        const auto access = memory.Protect & 0xffu;
        const bool open = memory.State == MEM_COMMIT && (memory.Protect & PAGE_GUARD) == 0 && access != PAGE_NOACCESS && access != 0;
        if (open) {
            if (!readable.empty() && readable.back().address + readable.back().size == cursor) readable.back().size += stop - cursor;
            else readable.push_back({cursor, stop - cursor});
        }
        cursor = stop;
    }
#else
    readable.push_back({address, size});
#endif
    return readable;
}

void Protector::Track(Tracker next) {
    std::lock_guard lock(mutex);
    tracker = next;
}

bool Protector::protect(std::uint64_t address, std::uint64_t size, Protection protection, bool* held) {
    ++counts.calls;
    counts.bytes += size;
    if (tracker != nullptr) {
        ++counts.tracked;
        bool kept = false;
        const bool done = tracker(static_cast<std::uintptr_t>(address), static_cast<std::size_t>(size), static_cast<std::uint32_t>(protection), &kept);
        if (!done) ++counts.failures;
        if (kept) {
            ++counts.held;
            if (held != nullptr) *held = true;
        }
        return done;
    }
#ifdef _WIN32
    const DWORD flags = protection == Protection::ReadWrite ? PAGE_READWRITE : protection == Protection::ReadOnly ? PAGE_READONLY : PAGE_NOACCESS;
    DWORD previous = 0;
    bool done = VirtualProtect(reinterpret_cast<void*>(address), static_cast<SIZE_T>(size), flags, &previous) != 0;
    if (!done) {
        done = true;
        bool any = false;
        const auto end = address + size;
        for (auto cursor = address; cursor < end;) {
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory)) {
                done = false;
                break;
            }
            const auto regionEnd = std::min<std::uint64_t>(end, reinterpret_cast<std::uint64_t>(memory.BaseAddress) + memory.RegionSize);
            if (memory.State == MEM_COMMIT) {
                ++counts.splitCalls;
                any = true;
                if (VirtualProtect(reinterpret_cast<void*>(cursor), static_cast<SIZE_T>(regionEnd - cursor), flags, &previous) == 0) done = false;
            } else {
                counts.skippedBytes += regionEnd - cursor;
            }
            cursor = regionEnd;
        }
        done = done && any;
    }
#else
    const int flags = protection == Protection::ReadWrite ? PROT_READ | PROT_WRITE : protection == Protection::ReadOnly ? PROT_READ : PROT_NONE;
    const bool done = mprotect(reinterpret_cast<void*>(address), static_cast<std::size_t>(size), flags) == 0;
#endif
    if (!done) ++counts.failures;
    return done;
}

std::size_t Protector::applyLocked(std::span<const ProtectionChange> changes, bool* held) {
    std::size_t calls = 0;
    for (const auto& change : changes) {
        if (change.size == 0) continue;
        if (change.protection == Protection::ReadWrite) {
            calls += protect(change.address, change.size, change.protection, held) ? 1 : 0;
            continue;
        }
        auto cursor = change.address;
        const auto end = change.address + change.size;
        auto it = excluded.upper_bound(cursor);
        if (it != excluded.begin()) --it;
        for (; cursor < end && it != excluded.end() && it->first < end; ++it) {
            if (it->second <= cursor) continue;
            if (it->first > cursor) calls += protect(cursor, it->first - cursor, change.protection, held) ? 1 : 0;
            const auto skippedEnd = std::min(end, it->second);
            counts.neverArmed += skippedEnd - std::max(cursor, it->first);
            cursor = skippedEnd;
        }
        if (cursor < end) calls += protect(cursor, end - cursor, change.protection, held) ? 1 : 0;
    }
    return calls;
}

std::size_t Protector::Apply(std::span<const ProtectionChange> changes, bool* held) {
    std::lock_guard lock(mutex);
    return applyLocked(changes, held);
}

void Protector::Defer(std::span<const ProtectionChange> changes) {
    std::lock_guard lock(mutex);
    for (const auto& change : changes) {
        ++counts.deferred;
        if (!pending.empty()) {
            auto& last = pending.back();
            if (last.protection == change.protection && last.address + last.size == change.address) {
                last.size += change.size;
                continue;
            }
        }
        pending.push_back(change);
    }
}

std::size_t Protector::Flush() {
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> taken;
    taken.swap(pending);
    return applyLocked(taken);
}

ProtectorCounts Protector::Counts() const {
    std::lock_guard lock(mutex);
    return counts;
}

}
