#include "prx/libSceAgcDriver/Unified/include/ResidencyService.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "BdaAbi.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Unified {

namespace {

constexpr VkAccessFlags ResidentAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;

VkBuffer bufferOf(std::uint64_t handle) {
    return reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(handle));
}

namespace Abi = ShaderRecompiler::BdaAbi;

static_assert(Abi::PageTablePageBits == GpuPageBits && Abi::PageTableLeafBits == GpuLeafBits && Abi::PageTableRootBits == GpuRootBits);
static_assert(ChunkBytes == GpuPageBytes);

constexpr std::size_t PageTableRootBytes = Abi::PageTableLeavesWord * sizeof(std::uint32_t);
constexpr std::size_t PageTableLeafBytes = GpuLeafEntries * sizeof(std::uint64_t);

}

ResidencyService& ResidencyService::Get() {
    static ResidencyService& service = *new ResidencyService();
    return service;
}

bool ResidencyService::Enabled() {
    static const bool enabled = std::getenv("APS5_RECODE_MEMORY") != nullptr && std::getenv("APS5_RECODE_NO_RESIDENCY") == nullptr;
    return enabled;
}

bool ResidencyService::PageTableEnabled() {
    static const bool enabled = Enabled() && std::getenv("APS5_RECODE_PAGE_TABLE") != nullptr && std::getenv("APS5_RECODE_NO_OWNED_REGIONS") == nullptr;
    return enabled;
}

bool ResidencyService::Attach(const Graphics::Context& graphics) {
    if (attached.load(std::memory_order_acquire)) return true;
    if (refused.load(std::memory_order_acquire) || !Enabled()) return false;
    std::lock_guard lock(mutex);
    if (attached.load(std::memory_order_relaxed)) return true;
    if (!VulkanResidency::Supported(graphics)) {
        refused.store(true, std::memory_order_release);
        std::fputs("[recode-residency] the device has no sparse residency buffers or device addresses; residency off\n", stderr);
        return false;
    }
    context.emplace(graphics);
    device = std::make_unique<VulkanResidency>(*context);
    ledger = std::make_unique<Residency>(*device, 0);
    uploads = std::make_unique<ResidentUploads>(*ledger, GuestMemory::CommittedRanges);
    if (PageTableEnabled()) {
        pages = std::make_unique<GpuPageTable>();
        pageBuffer = std::make_shared<Graphics::Buffer>(*context, Abi::PageTableBytes(PageTableLeafCapacity), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto bytes = pageBuffer->Bytes();
        std::memset(bytes.data(), 0, bytes.size());
        const Abi::Header header{Abi::PageTableVersion, 0, sizeof(std::uint64_t), 0};
        std::memcpy(bytes.data(), &header, sizeof(header));
        ledger->SetPageTable(pages.get());
    }
    for (const auto& [address, size] : queued) ledger->Add(address, size);
    queued.clear();
    queued.shrink_to_fit();
    attached.store(true, std::memory_order_release);
    return true;
}

bool ResidencyService::Attached() const {
    return attached.load(std::memory_order_acquire);
}

void ResidencyService::Add(std::uint64_t address, std::uint64_t size) {
    if (!Enabled() || size == 0) return;
    std::lock_guard lock(mutex);
    if (ledger) ledger->Add(address, size);
    else if (!refused.load(std::memory_order_relaxed)) queued.emplace_back(address, size);
}

void ResidencyService::NoteDirty(std::span<const DirtyRange> ranges) {
    if (!Attached() || ranges.empty()) return;
    std::lock_guard lock(mutex);
    uploads->NoteDirty(ranges);
    if (served.empty()) return;
    const auto now = frame.load(std::memory_order_relaxed);
    for (const auto& range : ranges) {
        auto it = served.upper_bound(range.address);
        if (it == served.begin()) continue;
        --it;
        if (range.address < it->second) uploads->Prepare(range.address, std::min(range.size, it->second - range.address), now);
    }
}

std::optional<DeviceSpan> ResidencyService::Serve(std::uint64_t address, std::uint64_t size, std::span<const std::pair<std::uint64_t, std::uint64_t>> committed) {
    if (!Attached() || size == 0) return std::nullopt;
    std::lock_guard lock(mutex);
    const auto end = address + size;
    if (!served.contains(address)) {
        if (!ledger->Owns(address, size)) {
            ledger->RemoveUnboundInside(address, size);
            if (!ledger->Add(address, size)) {
                ++serveRefusals;
                return std::nullopt;
            }
        }
        served.emplace(address, end);
    }
    const auto now = frame.load(std::memory_order_relaxed);
    for (const auto& [from, to] : committed) {
        const auto first = std::max(from, address);
        const auto last = std::min(to, end);
        if (first < last && !uploads->Prepare(first, last - first, now)) {
            served.erase(address);
            ledger->Remove(address);
            ++serveRefusals;
            return std::nullopt;
        }
    }
    auto base = ledger->Base(address);
    if (base) base->bytes = std::min(base->bytes, size);
    return base;
}

bool ResidencyService::ServeFaultPage(std::uint64_t page, std::uint64_t allocation, std::uint64_t allocationBytes, std::span<const std::pair<std::uint64_t, std::uint64_t>> committed) {
    if (!Attached()) return false;
    std::lock_guard lock(mutex);
    if (!pages) return false;
    const auto end = page + GpuPageBytes;
    if (!ledger->Owns(page, GpuPageBytes)) {
        bool added = false;
        if (allocationBytes != 0 && allocation <= page && end - allocation <= allocationBytes) {
            ledger->RemoveUnboundInside(allocation, allocationBytes);
            added = ledger->Add(allocation, allocationBytes);
        }
        if (!added) {
            ledger->RemoveUnboundInside(page, GpuPageBytes);
            added = ledger->Add(page, GpuPageBytes);
        }
        if (!added) {
            ++faultPagesRefused;
            return false;
        }
    }
    const auto now = frame.load(std::memory_order_relaxed);
    for (const auto& [from, to] : committed) {
        const auto first = std::max(from, page);
        const auto last = std::min(to, end);
        if (first < last && !uploads->Prepare(first, last - first, now)) {
            ++faultPagesRefused;
            return false;
        }
    }
    served.try_emplace(page, end);
    ++faultPagesServed;
    return true;
}

std::shared_ptr<Graphics::Buffer> ResidencyService::PageTableBuffer() const {
    if (!Attached()) return nullptr;
    std::lock_guard lock(mutex);
    return pageBuffer;
}

std::vector<PendingUpload> ResidencyService::ReadBack(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    std::vector<PendingUpload> pieces;
    if (!Attached() || ranges.empty()) return pieces;
    std::lock_guard lock(mutex);
    std::size_t total = 0;
    for (const auto& [address, bytes] : ranges) {
        const auto end = address + bytes;
        for (auto at = address; at < end;) {
            const auto next = std::min(end, (at & ~(ChunkBytes - 1)) + ChunkBytes);
            const auto span = ledger->Translate(at, next - at);
            if (!span) {
                readBackMissedBytes += next - at;
            } else if (!pieces.empty() && pieces.back().handle == span->handle && pieces.back().offset + pieces.back().bytes.size() == span->offset && pieces.back().guest + pieces.back().bytes.size() == at) {
                pieces.back().bytes.resize(pieces.back().bytes.size() + (next - at));
                total += next - at;
            } else {
                pieces.push_back({span->handle, span->offset, at, std::vector<std::byte>(next - at)});
                total += next - at;
            }
            at = next;
        }
    }
    if (pieces.empty()) return pieces;
    Graphics::Buffer staging(*context, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    {
        Graphics::CommandBatch batch(*context);
        Graphics::RecordMemoryBarrier(*context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, ResidentAccess, VK_ACCESS_TRANSFER_READ_BIT);
        const auto copy = context->Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
        std::size_t at = 0;
        for (const auto& piece : pieces) {
            const VkBufferCopy region{piece.offset, at, piece.bytes.size()};
            copy(batch.Handle(), bufferOf(piece.handle), staging.Handle(), 1, &region);
            at += piece.bytes.size();
        }
        Graphics::RecordMemoryBarrier(*context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
    }
    staging.Invalidate();
    const auto bytes = staging.Bytes();
    std::size_t at = 0;
    for (auto& piece : pieces) {
        std::memcpy(piece.bytes.data(), bytes.data() + at, piece.bytes.size());
        at += piece.bytes.size();
    }
    ++readBacks;
    readBackBytes += total;
    return pieces;
}

bool ResidencyService::Prepare(std::uint64_t address, std::uint64_t size) {
    if (!Attached()) return false;
    std::lock_guard lock(mutex);
    return uploads->Prepare(address, size, frame.load(std::memory_order_relaxed));
}

std::optional<DeviceSpan> ResidencyService::Translate(std::uint64_t address, std::uint64_t size) const {
    if (!Attached()) return std::nullopt;
    std::lock_guard lock(mutex);
    return ledger->Translate(address, size);
}

bool ResidencyService::Pending() const {
    if (!Attached()) return false;
    std::lock_guard lock(mutex);
    return uploads->Pending() || (pages && pages->Dirty());
}

std::size_t ResidencyService::Record(VkCommandBuffer commands, std::vector<std::shared_ptr<void>>& kept) {
    if (!Attached()) return 0;
    std::lock_guard lock(mutex);
    const auto now = frame.load(std::memory_order_relaxed);
    for (const auto& [address, end] : served) ledger->Touch(address, now);
    auto taken = uploads->Take();
    auto dirty = pages ? pages->TakeDirty() : GpuPageTableDirty{};
    std::erase_if(dirty.leaves, [&](std::uint32_t leaf) {
        if (leaf < PageTableLeafCapacity) return false;
        ++pageLeavesDropped;
        return true;
    });
    if (taken.empty() && !dirty.root && dirty.leaves.empty()) return 0;
    std::size_t uploadBytes = 0;
    for (const auto& upload : taken) uploadBytes += upload.bytes.size();
    const auto tableBytes = (dirty.root ? PageTableRootBytes : 0) + dirty.leaves.size() * PageTableLeafBytes;
    const auto total = uploadBytes + tableBytes;
    auto staging = std::make_shared<Graphics::Buffer>(*context, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto bytes = staging->Bytes();
    std::size_t at = 0;
    for (const auto& upload : taken) {
        std::memcpy(bytes.data() + at, upload.bytes.data(), upload.bytes.size());
        at += upload.bytes.size();
    }
    std::vector<VkBufferCopy> tableRegions;
    if (dirty.root) {
        const Abi::Header header{Abi::PageTableVersion, std::min(pages->Leaves(), PageTableLeafCapacity), sizeof(std::uint64_t), 0};
        std::memcpy(bytes.data() + at, &header, sizeof(header));
        std::memcpy(bytes.data() + at + Abi::PageTableHeaderWords * sizeof(std::uint32_t), pages->Root().data(), pages->Root().size_bytes());
        tableRegions.push_back({at, 0, PageTableRootBytes});
        at += PageTableRootBytes;
    }
    for (const auto leaf : dirty.leaves) {
        const auto entries = pages->Leaf(leaf);
        std::memcpy(bytes.data() + at, entries.data(), entries.size_bytes());
        tableRegions.push_back({at, PageTableRootBytes + leaf * PageTableLeafBytes, PageTableLeafBytes});
        at += PageTableLeafBytes;
    }
    Graphics::RecordMemoryBarrier(*context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, ResidentAccess, VK_ACCESS_TRANSFER_WRITE_BIT);
    const auto copy = context->Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    at = 0;
    for (const auto& upload : taken) {
        const VkBufferCopy region{at, upload.offset, upload.bytes.size()};
        copy(commands, staging->Handle(), bufferOf(upload.handle), 1, &region);
        at += upload.bytes.size();
    }
    if (!tableRegions.empty()) copy(commands, staging->Handle(), pageBuffer->Handle(), static_cast<std::uint32_t>(tableRegions.size()), tableRegions.data());
    Graphics::RecordMemoryBarrier(*context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, ResidentAccess);
    kept.push_back(std::move(staging));
    if (!tableRegions.empty()) kept.push_back(pageBuffer);
    recordedUploads += taken.size();
    recordedBytes += uploadBytes;
    pageUpdates += tableRegions.size();
    pageUpdateBytes += tableBytes;
    return taken.size() + tableRegions.size();
}

void ResidencyService::AdvanceFrame() {
    const auto now = frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!Attached()) return;
    std::lock_guard lock(mutex);
    ledger->SetBudget(device->BudgetBytes());
    uploads->Evict(now);
}

std::string ResidencyService::Line() const {
    if (!Attached()) return {};
    std::lock_guard lock(mutex);
    const auto ledgerCounts = ledger->Counts();
    const auto uploadCounts = uploads->Counts();
    char line[512];
    std::snprintf(line, sizeof(line), "[recode-residency] ranges %llu, pools %llu, bound %llu MiB of %llu MiB budget, chunks bound %llu, evicted %llu, refused %llu; uploads dirty %llu (%llu MiB) full %llu (%llu MiB) not resident %llu MiB; recorded %llu (%llu MiB); served %llu, serves refused %llu\n",
        static_cast<unsigned long long>(ledgerCounts.ranges), static_cast<unsigned long long>(ledgerCounts.pools), static_cast<unsigned long long>(ledgerCounts.boundBytes >> 20), static_cast<unsigned long long>(ledgerCounts.budget >> 20),
        static_cast<unsigned long long>(ledgerCounts.bound), static_cast<unsigned long long>(ledgerCounts.evicted), static_cast<unsigned long long>(ledgerCounts.refused),
        static_cast<unsigned long long>(uploadCounts.dirtyUploads), static_cast<unsigned long long>(uploadCounts.dirtyBytes >> 20), static_cast<unsigned long long>(uploadCounts.fullUploads), static_cast<unsigned long long>(uploadCounts.fullBytes >> 20), static_cast<unsigned long long>(uploadCounts.skippedBytes >> 20),
        static_cast<unsigned long long>(recordedUploads), static_cast<unsigned long long>(recordedBytes >> 20), static_cast<unsigned long long>(served.size()), static_cast<unsigned long long>(serveRefusals));
    if (!pages) return line;
    char table[320];
    std::snprintf(table, sizeof(table), "[recode-page-table] leaves %u of %u, updates %llu (%llu MiB), leaves dropped %llu, shared pages %llu; fault pages served %llu, refused %llu; read backs %llu (%llu KiB), not resident %llu KiB\n", pages->Leaves(), PageTableLeafCapacity,
        static_cast<unsigned long long>(pageUpdates), static_cast<unsigned long long>(pageUpdateBytes >> 20), static_cast<unsigned long long>(pageLeavesDropped), static_cast<unsigned long long>(ledgerCounts.sharedPages),
        static_cast<unsigned long long>(faultPagesServed), static_cast<unsigned long long>(faultPagesRefused), static_cast<unsigned long long>(readBacks), static_cast<unsigned long long>(readBackBytes >> 10), static_cast<unsigned long long>(readBackMissedBytes >> 10));
    return std::string(line) + table;
}

}
