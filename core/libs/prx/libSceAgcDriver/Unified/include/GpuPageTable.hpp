#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_GPUPAGETABLE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_GPUPAGETABLE_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace AgcDriver::Unified {

inline constexpr unsigned GpuPageBits = 16;
inline constexpr unsigned GpuLeafBits = 12;
inline constexpr unsigned GpuRootBits = 16;
inline constexpr std::uint64_t GpuPageBytes = std::uint64_t{1} << GpuPageBits;
inline constexpr std::uint32_t GpuLeafEntries = std::uint32_t{1} << GpuLeafBits;
inline constexpr std::uint32_t GpuRootEntries = std::uint32_t{1} << GpuRootBits;
inline constexpr std::uint64_t GpuAddressSpace = std::uint64_t{1} << (GpuPageBits + GpuLeafBits + GpuRootBits);
inline constexpr std::uint64_t GpuPageRead = 1;
inline constexpr std::uint64_t GpuPageWrite = 2;
inline constexpr std::uint64_t GpuPagePermissions = GpuPageRead | GpuPageWrite;

struct GpuPageTableDirty {
    bool root = false;
    std::vector<std::uint32_t> leaves;
};

class GpuPageTable {
public:
    GpuPageTable();

    bool Map(std::uint64_t address, std::uint64_t size, std::uint64_t deviceAddress, std::uint64_t permissions);
    void Unmap(std::uint64_t address, std::uint64_t size);
    [[nodiscard]] std::uint64_t Lookup(std::uint64_t address, bool write = false) const;
    [[nodiscard]] std::span<const std::uint32_t> Root() const { return root; }
    [[nodiscard]] std::uint32_t Leaves() const { return static_cast<std::uint32_t>(leaves.size()); }
    [[nodiscard]] std::span<const std::uint64_t> Leaf(std::uint32_t index) const;
    GpuPageTableDirty TakeDirty();

private:
    using LeafEntries = std::vector<std::uint64_t>;

    std::uint64_t* entry(std::uint64_t page, bool create);
    void markLeaf(std::uint32_t index);

    std::vector<std::uint32_t> root;
    std::vector<std::unique_ptr<LeafEntries>> leaves;
    std::vector<bool> leafDirty;
    GpuPageTableDirty dirty;
};

}

#endif
