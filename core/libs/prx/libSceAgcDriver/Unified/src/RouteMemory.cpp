#include "prx/libSceAgcDriver/Unified/include/RouteMemory.hpp"
#include "prx/libSceAgcDriver/Unified/include/RouteDefaults.hpp"
#include "prx/libSceAgcDriver/Unified/include/Downgrades.hpp"
#include "prx/libSceAgcDriver/Unified/include/FaultSites.hpp"
#include "prx/libSceAgcDriver/Unified/include/Front.hpp"
#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include "prx/libSceAgcDriver/Unified/include/MirrorHost.hpp"
#include "prx/libSceAgcDriver/Unified/include/ResidencyService.hpp"
#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include "prx/libSceAgcDriver/Unified/include/ProofVerdict.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordVerify.hpp"
#include "prx/libSceAgcDriver/Unified/include/DrawRecorder.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordDraw.hpp"
#include "prx/libSceAgcDriver/Unified/include/Resolve.hpp"
#include "prx/libSceAgcDriver/Unified/include/UploadClash.hpp"
#include "prx/libSceAgcDriver/Unified/include/DepthRecords.hpp"
#include "prx/libSceAgcDriver/Unified/include/Images.hpp"
#include "prx/libSceAgcDriver/Unified/include/MetadataWatch.hpp"
#include <atomic>
#include "prx/libSceAgcDriver/Execution/include/PerformanceOverlay.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <set>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <span>
#include <utility>
#include <string>
#include <string_view>
#include <array>

namespace AgcDriver::Unified::RouteMemory {

namespace {

std::uint64_t nanosecondsSince(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

std::atomic<bool>& refusedFlag() {
    static std::atomic<bool> refused{false};
    return refused;
}

}

bool DrawScope() {
    static const bool scoped = [] {
        const char* value = RouteValue("APS5_RECODE_MEMORY");
        return value != nullptr && std::strcmp(value, "draws") == 0;
    }();
    return scoped;
}

Totals& Counts() {
    static Totals& totals = *new Totals();
    return totals;
}

bool Refused() {
    return refusedFlag().load(std::memory_order_relaxed);
}

bool Installed() {
    static const bool installed = [] {
        const bool ok = MemoryCore::Get().Install();
        if (!ok) {
            refusedFlag().store(true, std::memory_order_relaxed);
            std::fputs("[recode-memory] write faults refused (no APS5RZ01 mark and no APS5_PROTECT_TRACKING, or APS5_NO_PROTECT_TRACKING); memory core off\n", stderr);
        }
        return ok;
    }();
    return installed;
}

namespace {

bool registerRange(std::uint64_t begin, std::uint64_t end) {
    if (begin == 0 || end <= begin) return false;
    static std::mutex seenMutex;
    static std::map<std::pair<std::uint64_t, std::uint64_t>, bool>& seen = *new std::map<std::pair<std::uint64_t, std::uint64_t>, bool>();
    const auto first = begin & ~(PageBytes - 1);
    const auto last = (end + PageBytes - 1) & ~(PageBytes - 1);
    const bool inside = GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(first), static_cast<std::size_t>(last - first));
    {
        std::lock_guard lock(seenMutex);
        if (!seen.emplace(std::pair{first, last}, inside).second) return inside;
    }
    if (!inside) {
        Counts().outsideArena.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    MemoryCore::Get().Register(first, last - first);
    ResidencyService::Get().Add(first, last - first);
    auto& totals = Counts();
    totals.registrations.fetch_add(1, std::memory_order_relaxed);
    totals.registeredBytes.fetch_add(last - first, std::memory_order_relaxed);
    return true;
}

void consumeMemory() {
    if (!Installed()) return;
    const auto start = std::chrono::steady_clock::now();
    const auto ranges = MemoryCore::Get().ConsumeDirty();
    if (Downgrades::Watching()) {
        for (const auto& range : ranges) Downgrades::WatchTouch("a CPU-dirty range", range.address, range.size);
        Downgrades::WatchPoll();
    }
    if (Mirror::WritesEnabled()) {
        const auto upload = MemoryCore::Get().WithoutGpuWritten(ranges);
        UploadClash::CheckUploads(upload);
        if (Images::DepthRecordsOn()) {
            for (const auto& range : upload) Images::Depths().NoteCpuWrites(range.address, range.size);
        }
        ResidencyService::Get().NoteDirty(upload);
        MemoryCore::Get().LandAhead();
        MemoryCore::Get().LandOpened();
    } else {
        ResidencyService::Get().NoteDirty(ranges);
    }
    const auto nanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    auto& totals = Counts();
    std::uint64_t bytes = 0;
    for (const auto& range : ranges) bytes += range.size;
    totals.consumes.fetch_add(1, std::memory_order_relaxed);
    totals.ranges.fetch_add(ranges.size(), std::memory_order_relaxed);
    totals.dirtyBytes.fetch_add(bytes, std::memory_order_relaxed);
    totals.consumeNanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
    auto seen = totals.maxConsumeNanoseconds.load(std::memory_order_relaxed);
    while (nanoseconds > seen && !totals.maxConsumeNanoseconds.compare_exchange_weak(seen, nanoseconds, std::memory_order_relaxed)) {
    }
}

}

void RegisterUses(std::span<const GuestUse> uses) {
    if (!Installed()) return;
    for (const auto& use : uses) registerRange(use.address, use.address + use.size);
}

bool Active() {
    static const bool active = RouteValue("APS5_RECODE_MEMORY") != nullptr;
    return active;
}

void RegisterAccess(GuestRanges reads, GuestRanges writes, bool unknown) {
    if (!DrawScope() || !Installed()) return;
    auto& totals = Counts();
    totals.accesses.fetch_add(1, std::memory_order_relaxed);
    if (unknown) totals.unknownAccesses.fetch_add(1, std::memory_order_relaxed);
    for (const auto& [begin, end] : reads) registerRange(begin, end);
    for (const auto& [begin, end] : writes) registerRange(begin, end);
}

bool RegisterProofRanges(GuestRanges ranges) {
    if (!DrawScope() || !Installed()) return false;
    bool registered = true;
    for (const auto& [begin, end] : ranges) registered = registerRange(begin, end) && registered;
    auto& totals = Counts();
    totals.proofObjects.fetch_add(1, std::memory_order_relaxed);
    if (!registered) totals.proofObjectsOutside.fetch_add(1, std::memory_order_relaxed);
    return registered;
}

void RegisterMapping(const GuestAllocations::Mapped& ranges) {
    if (!Active() || DrawScope() || !Installed()) return;
    auto& totals = Counts();
    for (const auto& weak : ranges) {
        const auto range = weak.lock();
        if (!range || range->bytes == 0) continue;
        const auto first = range->address & ~(PageBytes - 1);
        const auto last = (range->address + range->bytes + PageBytes - 1) & ~(PageBytes - 1);
        MemoryCore::Get().Register(first, last - first);
        ResidencyService::Get().Add(first, last - first);
        totals.registrations.fetch_add(1, std::memory_order_relaxed);
        totals.registeredBytes.fetch_add(last - first, std::memory_order_relaxed);
    }
}

namespace {

bool coreCoversWrites(std::uintptr_t address, std::size_t bytes) {
    return MemoryCore::Get().Pages().Covers(address, bytes);
}

struct WriteAudit {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::uint64_t> hashes;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> reported;
    std::atomic<std::uint64_t> pages{0};
    std::atomic<std::uint64_t> misses{0};
    std::vector<std::uintptr_t> listed;
};

WriteAudit& writeAudit() {
    static auto* audit = new WriteAudit;
    return *audit;
}

struct AuditForward {
    void (*written)(void*, std::uintptr_t, std::uintptr_t);
    void* context;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>>* runs;
};

void auditWritten(void* context, std::uintptr_t begin, std::uintptr_t end) {
    auto* forward = static_cast<AuditForward*>(context);
    forward->runs->emplace_back(begin, end);
    forward->written(forward->context, begin, end);
}

bool coreCollectWrites(std::uintptr_t address, std::size_t bytes, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context) {
    static const bool audit = std::getenv("APS5_RECODE_AUDIT_CORE_WRITES") != nullptr;
    if (!audit) {
        MemoryCore::Get().CollectWrites(address, bytes, written, context);
        return true;
    }
    auto& state = writeAudit();
    std::lock_guard lock(state.mutex);
    state.reported.clear();
    AuditForward forward{written, context, &state.reported};
    auto& pages = MemoryCore::Get().Pages();
    MemoryCore::Get().CollectWrites(address, bytes, auditWritten, &forward);
    const auto end = address + bytes;
    for (auto page = address & ~(PageBytes - 1); page < end; page += PageBytes) {
        const auto kind = pages.StateOf(page);
        if (kind != PageState::CpuClean && kind != PageState::CpuDirty) continue;
        std::uint64_t hash = 0xcbf29ce484222325ull;
        const auto* words = reinterpret_cast<const std::uint64_t*>(page);
        for (std::size_t i = 0; i < PageBytes / sizeof(std::uint64_t); ++i) hash = (hash ^ words[i]) * 0x100000001b3ull;
        state.pages.fetch_add(1, std::memory_order_relaxed);
        auto [entry, inserted] = state.hashes.try_emplace(page, hash);
        if (inserted || entry->second == hash) continue;
        entry->second = hash;
        bool seen = false;
        for (const auto& [from, to] : state.reported) seen = seen || (page >= from && page < to);
        if (seen) continue;
        if (state.listed.size() < 8 && std::find(state.listed.begin(), state.listed.end(), page) == state.listed.end()) state.listed.push_back(page);
        state.misses.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

}

std::string WriteAuditLine() {
    auto& state = writeAudit();
    if (state.pages.load(std::memory_order_relaxed) == 0) return {};
    std::string pages;
    {
        std::lock_guard lock(state.mutex);
        for (const auto page : state.listed) {
            char one[32];
            std::snprintf(one, sizeof(one), " 0x%llx", static_cast<unsigned long long>(page));
            pages += one;
        }
    }
    char text[160];
    std::snprintf(text, sizeof(text), "[recode-audit] pages hashed %llu, missed writes %llu, first missed pages", static_cast<unsigned long long>(state.pages.load(std::memory_order_relaxed)), static_cast<unsigned long long>(state.misses.load(std::memory_order_relaxed)));
    return std::string(text) + pages + "\n";
}

namespace {

void coreNoteWritten(std::uintptr_t address, std::size_t bytes) {
    MemoryCore::Get().NoteDriverWrite(address, bytes);
    MetadataWatch::CheckWrite(address, bytes, true, 0u);
    if (Images::DepthRecordsOn() && Images::Depths().NoteHtileWrites(address, bytes) != 0u) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) std::fprintf(stderr, "[recode-depth] first driver write over a depth record's HTILE: 0x%llx+0x%llx\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
    }
}

const GuestMemory::CoreWriteHooks coreWriteHooks{coreCoversWrites, coreCollectWrites, coreNoteWritten};

std::pair<std::uint64_t, std::uint64_t> allocationAround(std::uint64_t first, std::uint64_t last) {
    static const bool whole = std::getenv("APS5_RECODE_NO_ALLOCATION_RANGES") == nullptr;
    if (!whole) return {first, last};
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    bool readable = false;
    if (auto found = GuestAllocations::GuestAllocationsLookup_nid_postfix(static_cast<std::uintptr_t>(first), false)) {
        address = found->address;
        bytes = found->bytes;
        readable = found->readable;
    }
    const auto begin = address & ~(PageBytes - 1);
    const auto end = (address + bytes + PageBytes - 1) & ~(PageBytes - 1);
    if (bytes == 0 || !readable || begin > first || last > end) return {first, last};
    if (!GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(begin), static_cast<std::size_t>(end - begin))) return {first, last};
    return {begin, end};
}

}

bool ServesOwned() {
    static const bool serves = ResidencyService::Enabled() && std::getenv("APS5_RECODE_NO_OWNED_REGIONS") == nullptr;
    if (!serves || !Installed()) return false;
    static const bool hooked = [] {
        if (std::getenv("APS5_RECODE_CORE_WRITES") != nullptr) GuestMemory::SetCoreWriteHooks(&coreWriteHooks);
        return true;
    }();
    return hooked;
}

bool ServeOwned(const Graphics::Context& context, std::uint64_t address, std::uint64_t size, OwnedSpan* span) {
    if (size == 0 || !ServesOwned()) return false;
    auto& service = ResidencyService::Get();
    if (!service.Attach(context)) return false;
    const auto first = address & ~(PageBytes - 1);
    const auto last = (address + size + PageBytes - 1) & ~(PageBytes - 1);
    auto& totals = Counts();
    totals.ownedServes.fetch_add(1, std::memory_order_relaxed);
    struct Served {
        std::uint64_t last = 0;
        std::uint64_t frame = 0;
        std::uint64_t removals = 0;
        OwnedSpan span;
    };
    static std::mutex servedMutex;
    static auto& served = *new std::map<std::uint64_t, Served>();
    const auto frame = service.Frame();
    const auto removals = service.Removals();
    std::lock_guard lock(servedMutex);
    if (const auto found = served.find(first); found != served.end() && found->second.last == last && found->second.frame == frame && found->second.removals == removals) {
        *span = found->second.span;
        span->address += address - first;
        return true;
    }
    if (!GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(first), static_cast<std::size_t>(last - first))) return false;
    const auto start = std::chrono::steady_clock::now();
    const auto [owner, ownerEnd] = allocationAround(first, last);
    const auto from = std::max(owner, first & ~(ChunkBytes - 1));
    const auto to = std::min(ownerEnd, (last + ChunkBytes - 1) & ~(ChunkBytes - 1));
    const auto committed = GuestMemory::CommittedRanges(from, static_cast<std::size_t>(to - from));
    auto& core = MemoryCore::Get();
    core.Reserve(from, to - from);
    for (const auto& [begin, end] : committed) core.MakeResident(begin, end - begin);
    const auto base = service.Serve(from, to - from, committed, owner, ownerEnd - owner);
    totals.ownedWalks.fetch_add(1, std::memory_order_relaxed);
    totals.ownedWalkNanoseconds.fetch_add(nanosecondsSince(start), std::memory_order_relaxed);
    if (!base) {
        served.erase(first);
        return false;
    }
    const OwnedSpan whole{base->handle, base->address + (first - from), from - base->offset};
    served[first] = {last, frame, service.Removals(), whole};
    *span = whole;
    span->address += address - first;
    return true;
}

void ServeFaultPages(std::span<const std::uint64_t> pages) {
    if (pages.empty() || !ServesOwned()) return;
    auto& service = ResidencyService::Get();
    if (!service.Attached()) return;
    struct FaultPage {
        std::uint64_t page = 0;
        std::uint64_t first = 0;
        std::uint64_t last = 0;
    };
    std::vector<FaultPage> found;
    found.reserve(pages.size());
    {
        const auto lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
        for (const auto address : pages) {
            const auto page = address & ~(ChunkBytes - 1);
            FaultPage entry{page, page, page + ChunkBytes};
            auto range = std::upper_bound(lease.begin(), lease.end(), page, [](std::uint64_t at, const auto& candidate) { return at < candidate->address; });
            if (range != lease.begin()) {
                const auto& allocation = *--range;
                const auto first = allocation->address & ~(PageBytes - 1);
                const auto last = (allocation->address + allocation->bytes + PageBytes - 1) & ~(PageBytes - 1);
                if (allocation->bytes != 0 && first <= page && page + ChunkBytes <= last) {
                    entry.first = first;
                    entry.last = last;
                }
            }
            found.push_back(entry);
        }
    }
    auto& core = MemoryCore::Get();
    for (const auto& [page, first, last] : found) {
        if (!GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(page), static_cast<std::size_t>(ChunkBytes))) continue;
        const bool whole = registerRange(first, last);
        if (!whole && first != page) registerRange(page, page + ChunkBytes);
        const auto committed = GuestMemory::CommittedRanges(page, static_cast<std::size_t>(ChunkBytes));
        core.Reserve(page, ChunkBytes);
        for (const auto& [from, to] : committed) core.MakeResident(from, to - from);
        if (whole) service.ServeFaultPage(page, first, last - first, committed);
        else service.ServeFaultPage(page, 0, 0, committed);
    }
}


void LandGpuWrites(GuestRanges ranges) {
    static const bool ahead = std::getenv("APS5_RECODE_NO_READBACK_AHEAD") == nullptr;
    if (!ahead || ranges.empty() || !ServesOwned()) return;
    std::unique_lock<GuestMemory::GpuMutexType> gpu(GuestMemory::GpuMutex(), std::defer_lock);
    if (!GuestMemory::GpuMutex().HeldByThisThread()) gpu.lock();
    const auto pieces = ResidencyService::Get().ReadBack(ranges);
    auto& core = MemoryCore::Get();
    for (const auto& piece : pieces) core.LandGpuWrite(piece.guest, piece.bytes, GuestMemory::OwnStoreTarget(piece.guest, piece.bytes.size()));
}

void NoteDriverStore(std::uint64_t address, std::uint64_t bytes) {
    if (bytes == 0u) return;
    if (ResidentDrawInputs()) {
        const std::array written{std::pair{address, address + bytes}};
        NoteGpuWrites(written);
    }
    coreNoteWritten(static_cast<std::uintptr_t>(address), static_cast<std::size_t>(bytes));
    static const bool uploads = std::getenv("APS5_RECODE_DRIVER_STORE_UPLOADS") == nullptr || std::string_view(std::getenv("APS5_RECODE_DRIVER_STORE_UPLOADS")) != "0";
    if (uploads && Mirror::WritesEnabled()) {
        const std::array stored{DirtyRange{address, bytes}};
        ResidencyService::Get().NoteDirty(stored);
    }
}

bool ResidentDrawInputs() {
    static const bool resident = [] {
        if (std::getenv("APS5_RECODE_IMPORTED_INPUTS") != nullptr) return false;
        GuestMemory::SetGpuWriteHook([](std::uint64_t address, std::uint64_t bytes) {
            const std::array written{std::pair{address, address + bytes}};
            NoteGpuWrites(written);
        });
        return true;
    }();
    return resident;
}

void NoteGpuWrites(GuestRanges ranges) {
    if (Downgrades::Watching()) {
        for (const auto& [begin, end] : ranges) {
            if (end > begin) Downgrades::WatchTouch("a GPU write note", begin, end - begin);
        }
    }
    if (ranges.empty() || !ResidentDrawInputs() || !Installed()) return;
    static std::mutex markedMutex;
    static auto& marked = *new std::map<std::uint64_t, std::uint64_t>();
    std::lock_guard lock(markedMutex);
    for (auto [begin, end] : ranges) {
        if (end <= begin) continue;
        begin &= ~(PageBytes - 1);
        end = (end + PageBytes - 1) & ~(PageBytes - 1);
        auto next = marked.upper_bound(begin);
        if (next != marked.begin()) {
            const auto previous = std::prev(next);
            if (previous->second >= end) continue;
            if (previous->second >= begin) {
                begin = previous->first;
                next = marked.erase(previous);
            }
        }
        while (next != marked.end() && next->first <= end) {
            end = std::max(end, next->second);
            next = marked.erase(next);
        }
        marked.emplace(begin, end);
        MemoryCore::Get().Pages().NoteGpuWritten(begin, end - begin);
        static const bool tracks = std::getenv("APS5_RECODE_NO_GPU_WRITE_TRACKING") == nullptr;
        if (tracks && registerRange(begin, end)) MemoryCore::Get().TrackGpuShared(begin, end - begin);
    }
}

void ConsumeNow() {
    if (Active()) consumeMemory();
}

std::size_t ConsumeIn(std::span<const std::pair<std::uint64_t, std::uint64_t>> spans) {
    if (!Active() || !Installed() || !Mirror::WritesEnabled()) return 0;
    const auto ranges = spans.empty() ? MemoryCore::Get().ConsumeDirtyQuick() : MemoryCore::Get().ConsumeDirtyIn(spans);
    if (ranges.empty()) return 0;
    const auto upload = MemoryCore::Get().WithoutGpuWritten(ranges);
    UploadClash::CheckUploads(upload);
    if (Images::DepthRecordsOn()) {
        for (const auto& range : upload) Images::Depths().NoteCpuWrites(range.address, range.size);
    }
    ResidencyService::Get().NoteDirty(upload);
    return ranges.size();
}

bool UploadsPending() {
    return ServesOwned() && ResidencyService::Get().Pending();
}

std::size_t RecordUploads(VkCommandBuffer commands, std::vector<std::shared_ptr<void>>& kept) {
    if (!ServesOwned()) return 0;
    const auto start = std::chrono::steady_clock::now();
    const auto recorded = ResidencyService::Get().Record(commands, kept);
    if (recorded != 0) {
        auto& totals = Counts();
        totals.ownedRecords.fetch_add(1, std::memory_order_relaxed);
        totals.ownedRecordNanoseconds.fetch_add(nanosecondsSince(start), std::memory_order_relaxed);
    }
    return recorded;
}

void AdvanceFrame() {
    if (!Installed()) return;
    MemoryCore::Get().AdvanceFrame();
    ResidencyService::Get().AdvanceFrame();
    Counts().frames.fetch_add(1, std::memory_order_relaxed);
}

}
