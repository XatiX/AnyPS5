#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PAGETABLE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PAGETABLE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Unified {

inline constexpr std::uint64_t PageBytes = 0x1000;
inline constexpr std::uint64_t RegionBytes = 0x400000;
inline constexpr std::size_t RegionPages = RegionBytes / PageBytes;

enum class PageState : std::uint8_t { Untracked, CpuClean, CpuDirty, GpuOwned };

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
    std::uint64_t gpuOwnedPages = 0;
};

class PageTable {
public:
    static constexpr std::uint32_t MaxGapPages = 3;
    static constexpr std::uint8_t BusyAfterWrites = 3;
    static constexpr std::uint8_t BusyConsumes = 64;

    std::vector<ProtectionChange> Register(std::uint64_t address, std::uint64_t size);
    std::vector<ProtectionChange> Unregister(std::uint64_t address, std::uint64_t size);
    bool HandleWrite(std::uint64_t address, std::vector<ProtectionChange>& changes);
    std::vector<DirtyRange> ConsumeDirty(std::vector<ProtectionChange>& changes);
    std::vector<ProtectionChange> MarkGpuOwned(std::uint64_t address, std::uint64_t size);
    std::vector<DirtyRange> GpuOwnedIn(std::uint64_t address, std::uint64_t size) const;
    std::vector<ProtectionChange> Reclaim(std::uint64_t address, std::uint64_t size, bool forWrite);
    [[nodiscard]] PageState StateOf(std::uint64_t address) const;
    [[nodiscard]] bool RegionDirty(std::uint64_t address) const;
    [[nodiscard]] std::size_t DirtyPages() const;
    [[nodiscard]] PageTableCounts Counts() const;

private:
    struct Region {
        std::array<PageState, RegionPages> states{};
        std::array<std::uint8_t, RegionPages> streak{};
        std::array<std::uint8_t, RegionPages> busy{};
        std::array<std::uint32_t, RegionPages> lastConsume{};
        std::array<std::uint64_t, RegionPages / 64> listed{};
        std::uint32_t dirtyPages = 0;
    };

    Region* regionOf(std::uint64_t page, bool create);
    const Region* regionOf(std::uint64_t page) const;
    void setState(std::uint64_t page, PageState next);
    void list(std::uint64_t page);
    void unlist(std::uint64_t page);
    static Protection protectionOf(PageState state);
    static void appendChange(std::vector<ProtectionChange>& changes, std::uint64_t page, Protection protection);

    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t, std::unique_ptr<Region>> regions;
    std::vector<std::uint64_t> dirty;
    std::uint32_t consumeEpoch = 0;
    PageTableCounts counts;
};

}

#endif
