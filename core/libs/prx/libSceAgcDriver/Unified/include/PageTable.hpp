#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PAGETABLE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PAGETABLE_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::Unified {

inline constexpr std::uint64_t PageBytes = 0x1000;
inline constexpr std::uint64_t RegionBytes = 0x400000;
inline constexpr std::size_t RegionPages = RegionBytes / PageBytes;

enum class PageState : std::uint8_t { Untracked, CpuClean, CpuDirty, GpuOwned, Reserved };

enum class Protection : std::uint8_t { ReadWrite, ReadOnly, NoAccess };

struct ProtectionChange {
    std::uint64_t address = 0;
    std::uint64_t size = 0;
    Protection protection = Protection::ReadWrite;

    bool operator==(const ProtectionChange&) const = default;
};

struct DirtyRange {
    std::uint64_t address = 0;
    std::uint64_t size = 0;

    bool operator==(const DirtyRange&) const = default;
};

struct PageTableCounts {
    std::uint64_t writeFaults = 0;
    std::uint64_t gpuFaults = 0;
    std::uint64_t consumes = 0;
    std::uint64_t consumedPages = 0;
    std::uint64_t keptBusy = 0;
    std::uint64_t rearmed = 0;
    std::uint64_t unchangedBusy = 0;
    std::uint64_t gpuOwnedPages = 0;
    std::uint64_t gpuWrittenPages = 0;
};

struct GpuTake {
    std::vector<ProtectionChange> changes;
    std::vector<DirtyRange> owned;
    std::vector<DirtyRange> dirty;
    std::vector<DirtyRange> open;
};

class PageTable {
public:
    static constexpr std::uint32_t MaxGapPages = 3;
    static constexpr std::uint8_t BusyAfterWrites = 3;
    static constexpr std::uint8_t BusyConsumes = 64;
    static constexpr std::uint32_t QuietEpochs = 2;

    std::vector<ProtectionChange> Register(std::uint64_t address, std::uint64_t size);
    void Reserve(std::uint64_t address, std::uint64_t size);
    std::vector<ProtectionChange> MakeResident(std::uint64_t address, std::uint64_t size);
    std::vector<ProtectionChange> Unregister(std::uint64_t address, std::uint64_t size);
    [[nodiscard]] bool OwnedBy(std::uint64_t address, std::uint64_t size) const;
    [[nodiscard]] std::vector<std::uint64_t> BusyPages() const;
    bool HandleWrite(std::uint64_t address, std::vector<ProtectionChange>& changes);
    using WriteCheck = std::function<bool(std::uint64_t page, bool proven)>;
    std::vector<DirtyRange> ConsumeDirty(std::vector<ProtectionChange>& changes, const WriteCheck& written = {});
    void AdvanceFrame();
    std::vector<ProtectionChange> MarkGpuOwned(std::uint64_t address, std::uint64_t size);
    GpuTake TakeForGpu(std::uint64_t address, std::uint64_t size, bool takeDirty = true);
    std::vector<ProtectionChange> MarkDeviceWritten(std::uint64_t address, std::uint64_t size);
    std::vector<DirtyRange> GpuOwnedIn(std::uint64_t address, std::uint64_t size) const;
    [[nodiscard]] bool CleanIn(std::uint64_t address, std::uint64_t size) const;
    void NoteGpuWritten(std::uint64_t address, std::uint64_t size);
    [[nodiscard]] bool GpuWrittenIn(std::uint64_t address, std::uint64_t size) const;
    std::vector<ProtectionChange> Reclaim(std::uint64_t address, std::uint64_t size, bool forWrite);
    [[nodiscard]] PageState StateOf(std::uint64_t address) const;
    [[nodiscard]] bool Covers(std::uint64_t address, std::uint64_t size) const;
    using WrittenRun = void (*)(void* context, std::uintptr_t begin, std::uintptr_t end);
    void Collect(std::uint64_t address, std::uint64_t size, WrittenRun written, void* context, const WriteCheck& changed = {});
    [[nodiscard]] bool RegionDirty(std::uint64_t address) const;
    [[nodiscard]] std::size_t DirtyPages() const;
    [[nodiscard]] PageTableCounts Counts() const;
    [[nodiscard]] bool BeginWriteFault(std::uint64_t address);
    void EndWriteFault(std::uint64_t address);

private:
    static constexpr std::size_t LeafBits = 13;
    static constexpr std::size_t LeafRegions = std::size_t{1} << LeafBits;
    static constexpr std::size_t LeafCount = std::size_t{1} << 13;

    struct Region {
        std::array<std::atomic<PageState>, RegionPages> states{};
        std::array<std::atomic<std::uint64_t>, RegionPages / 64> faulted{};
        std::array<std::atomic<std::uint64_t>, RegionPages / 64> opening{};
        std::atomic<bool> hasFaults{false};
        std::array<std::uint8_t, RegionPages> streak{};
        std::array<std::uint32_t, RegionPages> busyUntil{};
        std::array<std::uint32_t, RegionPages> lastConsume{};
        std::array<std::uint64_t, RegionPages / 64> listed{};
        std::array<std::uint64_t, RegionPages / 64> unreported{};
        std::array<std::uint64_t, RegionPages / 64> written{};
        std::array<std::uint64_t, RegionPages / 64> device{};
        std::array<std::uint64_t, RegionPages / 64> gpuWritten{};
        std::uint32_t dirtyPages = 0;
        std::uint32_t trackedPages = 0;
        std::uint32_t writablePages = 0;
        std::uint32_t unreportedPages = 0;
    };

    struct Leaf {
        std::array<std::atomic<Region*>, LeafRegions> regions{};
    };

    Region* regionOf(std::uint64_t page, bool create);
    const Region* regionOf(std::uint64_t page) const;
    Region* tracked(std::uint64_t page) const;
    PageState setState(std::uint64_t page, PageState next);
    void account(Region& region, std::size_t slot, PageState state, PageState next);
    void settle(Region& region, std::uint64_t page, bool listed);
    void drainFaults();
    void list(std::uint64_t page);
    void unlist(std::uint64_t page);
    static Protection protectionOf(PageState state);
    static void appendChange(std::vector<ProtectionChange>& changes, std::uint64_t page, Protection protection);

    mutable std::mutex mutex;
    std::array<std::atomic<Leaf*>, LeafCount> leaves{};
    std::vector<std::unique_ptr<Leaf>> ownedLeaves;
    std::vector<std::pair<std::uint64_t, std::unique_ptr<Region>>> regions;
    std::unordered_map<std::uint64_t, Region*> farRegions;
    std::atomic<std::uint64_t> pendingFaults{0};
    std::vector<std::uint64_t> dirty;
    std::uint32_t consumeEpoch = 0;
    std::uint32_t frameEpoch = 0;
    bool framePaced = false;
    PageTableCounts counts;
};

}

#endif
