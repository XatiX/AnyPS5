#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_MEMORYCORE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_MEMORYCORE_HPP

#include "prx/libSceAgcDriver/Unified/include/GpuWritten.hpp"
#include "prx/libSceAgcDriver/Unified/include/PageTable.hpp"
#include "prx/libSceAgcDriver/Unified/include/Protector.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AgcDriver::Unified {

struct MemoryCoreCounts {
    std::uint64_t writesOpened = 0;
    std::uint64_t hostWritesOpened = 0;
    std::uint64_t gpuOwnedFaults = 0;
    std::uint64_t foreignFaults = 0;
    std::uint64_t staleFaults = 0;
    std::uint64_t landedBytes = 0;
    std::uint64_t driverPagesSettled = 0;
    std::uint64_t rearmedChanged = 0;
    std::uint64_t ownedPages = 0;
    std::uint64_t openBytes = 0;
    std::uint64_t faultsParked = 0;
    std::uint64_t faultsBypassed = 0;
    std::uint64_t faultsResolved = 0;
    std::uint64_t faultsOpenedStale = 0;
    std::uint64_t readAheadPages = 0;
    std::uint64_t ownedLandedBytes = 0;
    std::uint64_t diffLandedBytes = 0;
    std::uint64_t diffUnchangedBytes = 0;
    std::uint64_t flushes = 0;
    std::uint64_t flushPendingBytes = 0;
    std::uint64_t hotLandedPages = 0;
    std::uint64_t preImages = 0;
    std::uint64_t unviewedBytes = 0;
    std::uint64_t unreadableHashes = 0;
    std::uint64_t gpuOnlyBytes = 0;
    std::uint64_t bypassLanded = 0;
    std::uint64_t smallReads = 0;
    std::uint64_t smallReadBytes = 0;
    std::uint64_t imageFaults = 0;
    std::uint64_t imageWriteFaults = 0;
    std::uint64_t pieceWrites = 0;
    std::uint64_t ownedUnviewedBytes = 0;
    std::uint64_t diffChainedBytes = 0;
    std::uint64_t diffForeignBytes = 0;
    std::uint64_t faultCalls = 0;
    std::uint64_t faultNanoseconds = 0;
    std::uint64_t faultLockNanoseconds = 0;
    std::uint64_t faultTableNanoseconds = 0;
    std::uint64_t faultProtectNanoseconds = 0;
    std::uint64_t lockFreeWrites = 0;
};

struct GpuWriteMark {
    std::vector<DirtyRange> owned;
    std::vector<DirtyRange> open;
    std::vector<DirtyRange> upload;
    std::uint64_t sequence = 0;
};

class MemoryHost {
public:
    virtual ~MemoryHost() = default;
    virtual std::uint64_t Completed() = 0;
    virtual bool WaitFor(std::uint64_t serial) = 0;
    virtual bool ReadMirror(std::uint64_t address, std::span<std::byte> out) = 0;
    virtual std::byte* View(std::uint64_t address, std::size_t bytes) = 0;
    virtual std::byte* PatientView(std::uint64_t address, std::size_t bytes) { return View(address, bytes); }
    virtual std::vector<DirtyRange> ViewedPieces(std::uint64_t address, std::uint64_t size) { return View(address, static_cast<std::size_t>(size)) != nullptr ? std::vector<DirtyRange>{{address, size}} : std::vector<DirtyRange>{}; }
    virtual void Upload(std::uint64_t address, std::uint64_t size) = 0;
    virtual bool Bypass() = 0;
    virtual bool ImagePage(std::uint64_t) { return false; }
};

class MemoryCore {
public:
    static constexpr std::uint64_t ReadAheadPages = 16;
    static constexpr std::size_t HotPages = 32;
    static constexpr std::uint32_t HotLifetime = 4096;
    static constexpr std::uint64_t SmallReadBytes = 256;
    static constexpr int ResolveAttempts = 3;
    static constexpr std::uint64_t UnviewedReports = 8;
    static constexpr std::uint64_t LandedDiffBudgetBytes = 32u * 1024u * 1024u;
    static constexpr std::uint64_t DiffReports = 16;

    static MemoryCore& Get();

    MemoryCore() = default;
    ~MemoryCore();
    MemoryCore(const MemoryCore&) = delete;
    MemoryCore& operator=(const MemoryCore&) = delete;

    bool Install();
    void Uninstall();
    void Register(std::uint64_t address, std::uint64_t size);
    void Reserve(std::uint64_t address, std::uint64_t size);
    void OpenWhole(std::uint64_t address, std::uint64_t size) { pages.OpenWhole(address, size); }
    void MakeResident(std::uint64_t address, std::uint64_t size);
    [[nodiscard]] bool OwnedBy(std::uint64_t address, std::uint64_t size) const { return pages.OwnedBy(address, size); }
    void Unregister(std::uint64_t address, std::uint64_t size);
    std::vector<DirtyRange> ConsumeDirty();
    void AdvanceFrame() { pages.AdvanceFrame(); }
    void MarkGpuOwned(std::uint64_t address, std::uint64_t size);
    bool HandleFault(std::uint64_t address, bool write);
    void PrepareHostWrite(std::uint64_t address, std::uint64_t size);
    void LandGpuWrite(std::uint64_t address, std::span<const std::byte> bytes, void* view = nullptr);
    void CollectWrites(std::uint64_t address, std::uint64_t size, PageTable::WrittenRun written, void* context);
    void NoteDriverWrite(std::uint64_t address, std::uint64_t size);
    void SetHost(MemoryHost* next);
    [[nodiscard]] std::uint64_t CompletedSerial() const {
        auto* current = host.load(std::memory_order_acquire);
        return current != nullptr ? current->Completed() : 0u;
    }
    GpuWriteMark NoteGpuWrite(std::uint64_t address, std::uint64_t size, std::uint64_t serial, bool takeDirty = true);
    void NoteLandedGpuWrite(std::uint64_t address, std::uint64_t size, std::uint64_t serial);
    void ReleaseLandedWrite(std::uint64_t address, std::uint64_t size, std::uint64_t sequence);
    std::uint64_t NoteWrittenBytes(std::uint64_t address, std::uint64_t size, std::uint64_t serial);
    std::uint64_t NoteStoredBytes(std::uint64_t address, std::span<const std::byte> before, std::uint64_t serial);
    void ReleaseStoredBytes(std::uint64_t address, std::uint64_t size, std::uint64_t sequence);
    void TrackGpuShared(std::uint64_t address, std::uint64_t size);
    void LandDiff(std::uint64_t address, std::span<const std::byte> before, std::span<const std::byte> after, std::uint64_t serial);
    void FlushGpuWrites(std::uint64_t address, std::uint64_t size);
    void LandAhead();
    void LandOpened();
    [[nodiscard]] std::vector<DirtyRange> WithoutGpuWritten(std::span<const DirtyRange> ranges) const;
    [[nodiscard]] GpuWritten& Written() { return written; }
    [[nodiscard]] PageTable& Pages() { return pages; }
    [[nodiscard]] Protector& Protection() { return protector; }
    [[nodiscard]] MemoryCoreCounts Counts() const;

private:
    struct Parked {
        std::uint64_t page = 0;
        bool write = false;
        bool done = false;
        bool opened = false;
    };
    struct Hot {
        std::uint64_t page = 0;
        std::uint32_t life = 0;
    };

    static std::uint64_t PageHash(std::uint64_t page);
    static bool readableHash(std::uint64_t page, std::uint64_t& hash);
    bool rehash(std::uint64_t page, bool proven);
    bool resolveOwnedFault(std::uint64_t address, bool write);
    bool resolvePage(std::uint64_t page, bool write);
    void takeViewed(GpuWriteMark& mark, std::uint64_t address, std::uint64_t size, bool takeDirty);
    void leaveUnviewed(GpuWriteMark& mark, std::uint64_t address, std::uint64_t size, MemoryHost* current);
    void landDiffThrough(std::byte* view, std::uint64_t address, std::span<const std::byte> before, std::span<const std::byte> after, std::uint64_t serial, MemoryHost& current);
    void openWithPreImage(std::uint64_t page);
    void keepSharedPreImage(std::uint64_t page);
    std::uint64_t landOpenPieces(std::uint64_t page, std::uint64_t completed, std::uint64_t begin, std::uint64_t end);
    void noteHot(std::uint64_t page);
    void resolverLoop();

    std::mutex hashMutex;
    std::mutex collectMutex;
    std::mutex mutex;
    PageTable pages;
    std::unordered_map<std::uint64_t, std::uint64_t> busyHashes;
    std::unordered_map<std::uint64_t, std::uint64_t> collectHashes;
    Protector protector;
    GpuWritten written;
    std::atomic<MemoryHost*> host{nullptr};
    std::unordered_map<std::uint64_t, std::vector<std::byte>> preImages;
    std::unordered_map<std::uint64_t, std::vector<std::byte>> landedDiffs;
    struct StoredBefore {
        std::vector<std::byte> bytes;
        std::uint64_t sequence = 0;
    };
    std::unordered_set<std::uint64_t> sharedPages;
    std::unordered_map<std::uint64_t, std::vector<std::byte>> sharedPreImages;
    std::atomic<std::uint64_t> sharedNarrowedBytes{0};
    mutable std::mutex storedMutex;
    std::map<std::uint64_t, StoredBefore> storedBefore;
    std::uint64_t landedDiffBytes = 0;
    std::vector<Hot> hot;
    std::mutex parkMutex;
    std::condition_variable parkSignal;
    std::deque<Parked*> parked;
    std::thread resolver;
    bool resolverStarted = false;
    bool resolverStop = false;
    std::thread::id resolverThread;
    std::atomic<std::uint64_t> writesOpened{0};
    std::atomic<std::uint64_t> hostWritesOpened{0};
    std::atomic<std::uint64_t> gpuOwnedFaults{0};
    std::atomic<std::uint64_t> foreignFaults{0};
    std::atomic<std::uint64_t> staleFaults{0};
    std::atomic<std::uint64_t> landedBytes{0};
    std::atomic<std::uint64_t> driverPagesSettled{0};
    std::atomic<std::uint64_t> rearmedChanged{0};
    std::atomic<std::uint64_t> ownedPages{0};
    std::atomic<std::uint64_t> openBytes{0};
    std::atomic<std::uint64_t> faultsParked{0};
    std::atomic<std::uint64_t> faultsBypassed{0};
    std::atomic<std::uint64_t> faultsResolved{0};
    std::atomic<std::uint64_t> faultsOpenedStale{0};
    std::atomic<std::uint64_t> readAheadPages{0};
    std::atomic<std::uint64_t> ownedLandedBytes{0};
    std::atomic<std::uint64_t> diffLandedBytes{0};
    std::atomic<std::uint64_t> diffUnchangedBytes{0};
    std::atomic<std::uint64_t> flushes{0};
    std::atomic<std::uint64_t> flushPendingBytes{0};
    std::atomic<std::uint64_t> hotLandedPages{0};
    std::atomic<std::uint64_t> preImageCount{0};
    std::atomic<std::uint64_t> unviewedBytes{0};
    std::atomic<std::uint64_t> unreadableHashes{0};
    std::atomic<std::uint64_t> gpuOnlyBytes{0};
    std::atomic<std::uint64_t> unviewedReports{0};
    std::atomic<std::uint64_t> bypassLanded{0};
    std::atomic<std::uint64_t> imageFaults{0};
    std::atomic<std::uint64_t> imageWriteFaults{0};
    std::atomic<std::uint64_t> smallReads{0};
    std::atomic<std::uint64_t> smallReadBytes{0};
    std::atomic<std::uint64_t> pieceWrites{0};
    std::atomic<std::uint64_t> ownedUnviewedBytes{0};
    std::atomic<std::uint64_t> diffChainedBytes{0};
    std::atomic<std::uint64_t> diffForeignBytes{0};
    std::atomic<std::uint64_t> diffReports{0};
};

}

#endif
