#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include "prx/libSceAgcDriver/Unified/include/Downgrades.hpp"
#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include "prx/libSceAgcDriver/Unified/include/FaultSites.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include "prx/libc/include/GuestArena.hpp"

namespace AgcDriver::Unified {

namespace {

thread_local volatile bool landing = false;

std::atomic<std::uint64_t> faultCalls{0};
std::atomic<std::uint64_t> faultNanoseconds{0};
std::atomic<std::uint64_t> faultLockNanoseconds{0};
std::atomic<std::uint64_t> faultTableNanoseconds{0};
std::atomic<std::uint64_t> faultProtectNanoseconds{0};

std::atomic<std::uint64_t> lockFreeWrites{0};

bool lockFreeFaults() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_RECODE_LOCKED_FAULTS");
        return value == nullptr || std::string_view(value) == "0";
    }();
    return on;
}

bool growsFaults() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_RECODE_GROW_FAULTS");
        return value != nullptr && std::string_view(value) != "0";
    }();
    return on;
}

std::uint64_t since(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

bool handleGuestFault(std::uintptr_t address, bool write) {
    if (landing) return false;
    const auto start = std::chrono::steady_clock::now();
    const bool handled = MemoryCore::Get().HandleFault(address, write);
    if (FaultSites::Enabled() && handled && write) FaultSites::Note();
    faultCalls.fetch_add(1, std::memory_order_relaxed);
    faultNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
    return handled;
}

void prepareGuestHostWrite(std::uintptr_t address, std::size_t bytes) {
    MemoryCore::Get().PrepareHostWrite(address, bytes);
}

}

MemoryCore& MemoryCore::Get() {
    static MemoryCore& core = *new MemoryCore();
    return core;
}

bool MemoryCore::Install() {
    pages.SetHasher(&MemoryCore::readableHash);
    FaultSites::Install();
#ifdef _WIN32
    if (!GuestArena::GuestArenaSetGpuFaultHandler_nid_no_patch(handleGuestFault)) return false;
    GuestArena::GuestArenaSetGpuHostWriteHook_nid_no_patch(prepareGuestHostWrite);
    if (std::getenv("APS5_RECODE_NO_PROTECT_OWNER") == nullptr) protector.Track(GuestArena::GuestArenaTrackProtect_nid_no_patch);
    return true;
#else
    if (!GuestArena::GuestArenaSetGpuFaultHandler_nid_no_patch(handleGuestFault)) return false;
    GuestArena::GuestArenaSetGpuHostWriteHook_nid_no_patch(prepareGuestHostWrite);
    return true;
#endif
}

void MemoryCore::Uninstall() {
    GuestArena::GuestArenaSetGpuHostWriteHook_nid_no_patch(nullptr);
    GuestArena::GuestArenaSetGpuFaultHandler_nid_no_patch(nullptr);
#ifdef _WIN32
    protector.Track(nullptr);
#endif
}

void MemoryCore::Register(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    protector.Apply(pages.Register(address, size));
}

void MemoryCore::Reserve(std::uint64_t address, std::uint64_t size) {
    pages.Reserve(address, size);
}

void MemoryCore::MakeResident(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    protector.Apply(pages.MakeResident(address, size));
}

void MemoryCore::Unregister(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    protector.Apply(pages.Unregister(address, size));
}

std::uint64_t MemoryCore::PageHash(std::uint64_t page) {
    const auto* words = reinterpret_cast<const std::uint64_t*>(page * PageBytes);
    std::array<std::uint64_t, 4> lanes{0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull};
    for (std::size_t i = 0; i < PageBytes / sizeof(std::uint64_t); i += lanes.size()) {
        for (std::size_t lane = 0; lane < lanes.size(); ++lane) lanes[lane] = (lanes[lane] ^ words[i + lane]) * 0x100000001b3ull + (lanes[lane] >> 29u);
    }
    return lanes[0] ^ (lanes[1] * 31u) ^ (lanes[2] * 131u) ^ (lanes[3] * 1031u);
}

bool MemoryCore::readableHash(std::uint64_t page, std::uint64_t& hash) {
    if (!Protector::Readable(page * PageBytes)) return false;
    hash = PageHash(page);
    return true;
}

bool MemoryCore::rehash(std::uint64_t page, bool proven) {
    if (!Protector::Readable(page * PageBytes)) {
        unreadableHashes.fetch_add(1, std::memory_order_relaxed);
        return proven;
    }
    const auto hash = PageHash(page);
    auto [entry, inserted] = busyHashes.try_emplace(page, hash);
    if (inserted || proven || entry->second != hash) {
        entry->second = hash;
        return true;
    }
    return false;
}

std::vector<DirtyRange> MemoryCore::ConsumeDirty() {
    static const bool hashBusy = std::getenv("APS5_RECODE_NO_BUSY_HASH") == nullptr;
    static const bool unlockedHash = std::getenv("APS5_RECODE_NO_UNLOCKED_HASH") == nullptr;
    std::lock_guard hashLock(hashMutex);
    std::unordered_map<std::uint64_t, bool> hashed;
    if (hashBusy && unlockedHash) {
        for (const auto page : pages.BusyPages()) hashed.emplace(page, rehash(page, false));
    }
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    std::vector<std::uint64_t> unchanged;
    const PageTable::WriteCheck written = [this, &hashed, &unchanged](std::uint64_t page, bool proven) {
        bool changed = false;
        if (const auto found = hashed.find(page); !proven && found != hashed.end()) changed = found->second;
        else changed = rehash(page, proven);
        if (!changed) unchanged.push_back(page);
        return changed;
    };
    auto ranges = hashBusy ? pages.ConsumeDirty(changes, written) : pages.ConsumeDirty(changes);
    protector.Apply(changes);
    for (const auto& [page, hash] : pages.TakeRearmedUnproven()) {
        if (pages.StateOf(page * PageBytes) != PageState::CpuClean || PageHash(page) == hash) continue;
        ranges.push_back({page * PageBytes, PageBytes});
        rearmedChanged.fetch_add(1, std::memory_order_relaxed);
    }
    for (const auto page : unchanged) {
        if (pages.StateOf(page * PageBytes) != PageState::CpuClean || !rehash(page, false)) continue;
        ranges.push_back({page * PageBytes, PageBytes});
        rearmedChanged.fetch_add(1, std::memory_order_relaxed);
    }
    for (const auto& range : ranges) {
        Downgrades::WatchTouch("a CPU change found on consume", range.address, range.size);
        Events::Shared().Note(range.address, range.size);
    }
    return narrowShared(std::move(ranges));
}

std::vector<DirtyRange> MemoryCore::ConsumeDirtyQuick() {
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    auto ranges = pages.ConsumeDirty(changes);
    protector.Apply(changes);
    for (const auto& range : ranges) Events::Shared().Note(range.address, range.size);
    return narrowShared(std::move(ranges));
}

std::vector<DirtyRange> MemoryCore::ConsumeDirtyIn(std::span<const std::pair<std::uint64_t, std::uint64_t>> spans) {
    std::lock_guard hashLock(hashMutex);
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    const PageTable::WriteCheck written = [this](std::uint64_t page, bool proven) { return rehash(page, proven); };
    auto ranges = pages.ConsumeDirtyIn(spans, changes, written);
    protector.Apply(changes);
    for (const auto& range : ranges) {
        Downgrades::WatchTouch("a CPU change found on an operation's consume", range.address, range.size);
        Events::Shared().Note(range.address, range.size);
    }
    return narrowShared(std::move(ranges));
}

std::vector<DirtyRange> MemoryCore::narrowShared(std::vector<DirtyRange> ranges) {
    if (sharedPreImages.empty()) return ranges;
    std::vector<DirtyRange> narrowed;
    for (const auto& range : ranges) {
        const auto end = range.address + range.size;
        for (auto at = range.address; at < end;) {
            const std::uint64_t page = at & ~(PageBytes - 1u);
            const auto stop = std::min(end, page + PageBytes);
            const auto found = sharedPreImages.find(page);
            if (found == sharedPreImages.end() || !Protector::Readable(page)) {
                narrowed.push_back({at, stop - at});
                at = stop;
                continue;
            }
            const auto* now = reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(page));
            const auto& before = found->second;
            for (auto offset = at - page; offset < stop - page;) {
                if (now[offset] == before[offset]) {
                    ++offset;
                    continue;
                }
                auto last = offset + 1u;
                while (last < stop - page && now[last] != before[last]) ++last;
                narrowed.push_back({page + offset, last - offset});
                sharedNarrowedBytes.fetch_add(last - offset, std::memory_order_relaxed);
                offset = last;
            }
            sharedPreImages.erase(found);
            at = stop;
        }
    }
    return narrowed;
}

void MemoryCore::TrackGpuShared(std::uint64_t address, std::uint64_t size) {
    if (size == 0) return;
    std::lock_guard lock(mutex);
    for (std::uint64_t page = address & ~(PageBytes - 1u); page < address + size; page += PageBytes) sharedPages.insert(page);
}

void MemoryCore::keepSharedPreImage(std::uint64_t page) {
    if (!sharedPages.contains(page) || sharedPreImages.contains(page) || !Protector::Readable(page)) return;
    auto& image = sharedPreImages[page];
    image.resize(PageBytes);
    std::memcpy(image.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(page)), PageBytes);
}

void MemoryCore::MarkGpuOwned(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    protector.Apply(pages.MarkGpuOwned(address, size));
}

bool MemoryCore::HandleFault(std::uint64_t address, bool write) {
    const auto entered = std::chrono::steady_clock::now();
    if (write && lockFreeFaults() && pages.BeginWriteFault(address)) {
        const auto page = address & ~(PageBytes - 1);
        Downgrades::WatchTouch("a CPU write fault", page, PageBytes);
        Events::Shared().Note(page, PageBytes);
        faultTableNanoseconds.fetch_add(since(entered), std::memory_order_relaxed);
        const auto grown = growsFaults() ? pages.GrowWriteFault(address) : std::vector<DirtyRange>{};
        bool held = false;
        const auto protectStart = std::chrono::steady_clock::now();
        protector.Open(page, PageBytes, &held);
        pages.EndWriteFault(address);
        for (const auto& run : grown) protector.Open(run.address, run.size);
        pages.EndGrow(grown);
        faultProtectNanoseconds.fetch_add(since(protectStart), std::memory_order_relaxed);
        writesOpened.fetch_add(1, std::memory_order_relaxed);
        lockFreeWrites.fetch_add(1, std::memory_order_relaxed);
        return !held;
    }
    std::unique_lock lock(mutex);
    faultLockNanoseconds.fetch_add(since(entered), std::memory_order_relaxed);
    const auto state = pages.StateOf(address);
    if (state == PageState::Untracked || state == PageState::Reserved) {
        foreignFaults.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (state == PageState::GpuOwned) {
        gpuOwnedFaults.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();
        return resolveOwnedFault(address, write);
    }
    if (!write) {
        if (host.load(std::memory_order_acquire) == nullptr || !Protector::Readable(address)) return false;
        staleFaults.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (state != PageState::CpuClean) {
        staleFaults.fetch_add(1, std::memory_order_relaxed);
        Events::Shared().Note(address & ~(PageBytes - 1), PageBytes);
        const ProtectionChange reopen{address & ~(PageBytes - 1), PageBytes, Protection::ReadWrite};
        bool held = false;
        return protector.Apply(std::span(&reopen, 1), &held) == 1 && !held;
    }
    std::vector<ProtectionChange> changes;
    const auto tableStart = std::chrono::steady_clock::now();
    if (!pages.HandleWrite(address, changes)) return false;
    Downgrades::WatchTouch("a CPU write fault", address & ~(PageBytes - 1), PageBytes);
    Events::Shared().Note(address & ~(PageBytes - 1), PageBytes);
    faultTableNanoseconds.fetch_add(since(tableStart), std::memory_order_relaxed);
    if (!sharedPages.empty()) {
        for (const auto& change : changes) {
            for (auto page = change.address; page < change.address + change.size; page += PageBytes) keepSharedPreImage(page);
        }
    }
    bool held = false;
    const auto protectStart = std::chrono::steady_clock::now();
    protector.Apply(changes, &held);
    faultProtectNanoseconds.fetch_add(since(protectStart), std::memory_order_relaxed);
    writesOpened.fetch_add(1, std::memory_order_relaxed);
    return !held;
}

void MemoryCore::PrepareHostWrite(std::uint64_t address, std::uint64_t size) {
    if (size == 0) return;
    Downgrades::WatchTouch("a host write", address, size);
    Events::Shared().Note(address, size);
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    const auto end = address + size;
    for (auto page = address & ~(PageBytes - 1); page < end; page += PageBytes) {
        const auto state = pages.StateOf(page);
        if (state == PageState::GpuOwned) {
            openWithPreImage(page);
            hostWritesOpened.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (state != PageState::CpuClean) continue;
        if (pages.HandleWrite(page, changes)) hostWritesOpened.fetch_add(1, std::memory_order_relaxed);
    }
    if (!sharedPages.empty()) {
        for (const auto& change : changes) {
            for (auto at = change.address; at < change.address + change.size; at += PageBytes) keepSharedPreImage(at);
        }
    }
    protector.Apply(changes);
}

bool MemoryCore::OpenCleanForImport(std::uint64_t address, std::uint64_t size) {
    if (size == 0) return false;
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    const auto end = address + size;
    for (auto page = address & ~(PageBytes - 1); page < end; page += PageBytes) {
        if (pages.StateOf(page) != PageState::CpuClean) continue;
        static_cast<void>(pages.HandleWrite(page, changes));
    }
    if (changes.empty()) return false;
    if (!sharedPages.empty()) {
        for (const auto& change : changes) {
            for (auto at = change.address; at < change.address + change.size; at += PageBytes) keepSharedPreImage(at);
        }
    }
    protector.Apply(changes);
    return true;
}

void MemoryCore::LandGpuWrite(std::uint64_t address, std::span<const std::byte> bytes, void* view) {
    if (bytes.empty()) return;
    Downgrades::WatchTouch("a GPU write landed from residency", address, bytes.size());
    if (view != nullptr && view != reinterpret_cast<void*>(address)) {
        std::memcpy(view, bytes.data(), bytes.size());
        landedBytes.fetch_add(bytes.size(), std::memory_order_relaxed);
        return;
    }
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> open;
    std::vector<ProtectionChange> close;
    const auto end = address + bytes.size();
    for (auto page = address & ~(PageBytes - 1); page < end; page += PageBytes) {
        const auto state = pages.StateOf(page);
        if (state != PageState::CpuClean && state != PageState::GpuOwned) continue;
        open.push_back({page, PageBytes, Protection::ReadWrite});
        close.push_back({page, PageBytes, state == PageState::CpuClean ? Protection::ReadOnly : Protection::NoAccess});
    }
    protector.Apply(open);
    landing = true;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    std::memcpy(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
    std::atomic_signal_fence(std::memory_order_seq_cst);
    landing = false;
    protector.Apply(close);
    landedBytes.fetch_add(bytes.size(), std::memory_order_relaxed);
}

void MemoryCore::CollectWrites(std::uint64_t address, std::uint64_t size, PageTable::WrittenRun written, void* context) {
    std::lock_guard lock(collectMutex);
    pages.Collect(address, size, written, context, [this](std::uint64_t page, bool proven) {
        if (!Protector::Readable(page * PageBytes)) {
            unreadableHashes.fetch_add(1, std::memory_order_relaxed);
            return proven;
        }
        const auto hash = PageHash(page);
        auto [entry, inserted] = collectHashes.try_emplace(page, hash);
        if (proven || (!inserted && entry->second != hash)) {
            entry->second = hash;
            return true;
        }
        return false;
    });
}

void MemoryCore::NoteDriverWrite(std::uint64_t address, std::uint64_t size) {
    if (size == 0) return;
    std::lock_guard lock(mutex);
    if (written.Any(address, size)) {
        written.Discard(address, size);
        for (auto page = address & ~(PageBytes - 1); page < address + size; page += PageBytes) {
            if (pages.StateOf(page) == PageState::GpuOwned) openWithPreImage(page);
        }
    }
    protector.Apply(pages.MarkDeviceWritten(address, size));
    driverPagesSettled.fetch_add((size + PageBytes - 1) / PageBytes, std::memory_order_relaxed);
}

MemoryCoreCounts MemoryCore::Counts() const {
    MemoryCoreCounts counts;
    counts.writesOpened = writesOpened.load(std::memory_order_relaxed);
    counts.hostWritesOpened = hostWritesOpened.load(std::memory_order_relaxed);
    counts.gpuOwnedFaults = gpuOwnedFaults.load(std::memory_order_relaxed);
    counts.foreignFaults = foreignFaults.load(std::memory_order_relaxed);
    counts.staleFaults = staleFaults.load(std::memory_order_relaxed);
    counts.faultCalls = faultCalls.load(std::memory_order_relaxed);
    counts.faultNanoseconds = faultNanoseconds.load(std::memory_order_relaxed);
    counts.faultLockNanoseconds = faultLockNanoseconds.load(std::memory_order_relaxed);
    counts.faultTableNanoseconds = faultTableNanoseconds.load(std::memory_order_relaxed);
    counts.faultProtectNanoseconds = faultProtectNanoseconds.load(std::memory_order_relaxed);
    counts.lockFreeWrites = lockFreeWrites.load(std::memory_order_relaxed);
    counts.landedBytes = landedBytes.load(std::memory_order_relaxed);
    counts.driverPagesSettled = driverPagesSettled.load(std::memory_order_relaxed);
    counts.rearmedChanged = rearmedChanged.load(std::memory_order_relaxed);
    counts.ownedPages = ownedPages.load(std::memory_order_relaxed);
    counts.openBytes = openBytes.load(std::memory_order_relaxed);
    counts.faultsParked = faultsParked.load(std::memory_order_relaxed);
    counts.faultsBypassed = faultsBypassed.load(std::memory_order_relaxed);
    counts.faultsResolved = faultsResolved.load(std::memory_order_relaxed);
    counts.faultsOpenedStale = faultsOpenedStale.load(std::memory_order_relaxed);
    counts.readAheadPages = readAheadPages.load(std::memory_order_relaxed);
    counts.ownedLandedBytes = ownedLandedBytes.load(std::memory_order_relaxed);
    counts.diffLandedBytes = diffLandedBytes.load(std::memory_order_relaxed);
    counts.diffUnchangedBytes = diffUnchangedBytes.load(std::memory_order_relaxed);
    counts.flushes = flushes.load(std::memory_order_relaxed);
    counts.flushPendingBytes = flushPendingBytes.load(std::memory_order_relaxed);
    counts.hotLandedPages = hotLandedPages.load(std::memory_order_relaxed);
    counts.preImages = preImageCount.load(std::memory_order_relaxed);
    counts.unviewedBytes = unviewedBytes.load(std::memory_order_relaxed);
    counts.unreadableHashes = unreadableHashes.load(std::memory_order_relaxed);
    counts.gpuOnlyBytes = gpuOnlyBytes.load(std::memory_order_relaxed);
    counts.bypassLanded = bypassLanded.load(std::memory_order_relaxed);
    counts.smallReads = smallReads.load(std::memory_order_relaxed);
    counts.imageFaults = imageFaults.load(std::memory_order_relaxed);
    counts.imageWriteFaults = imageWriteFaults.load(std::memory_order_relaxed);
    counts.smallReadBytes = smallReadBytes.load(std::memory_order_relaxed);
    counts.pieceWrites = pieceWrites.load(std::memory_order_relaxed);
    counts.ownedUnviewedBytes = ownedUnviewedBytes.load(std::memory_order_relaxed);
    counts.diffChainedBytes = diffChainedBytes.load(std::memory_order_relaxed);
    counts.diffForeignBytes = diffForeignBytes.load(std::memory_order_relaxed);
    return counts;
}

}
