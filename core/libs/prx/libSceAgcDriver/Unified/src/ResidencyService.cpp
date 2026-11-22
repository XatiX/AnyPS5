#include "prx/libSceAgcDriver/Unified/include/ResidencyService.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "BdaAbi.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
constexpr std::size_t ImportSpacesKept = 8;
constexpr std::size_t StagingMinBytes = std::size_t{1} << 20u;
constexpr std::size_t StagingIdleBytes = std::size_t{64} << 20u;

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
    static const bool enabled = Enabled() && std::getenv("APS5_RECODE_NO_PAGE_TABLE") == nullptr && std::getenv("APS5_RECODE_NO_OWNED_REGIONS") == nullptr;
    return enabled;
}

VkMemoryPropertyFlags ResidencyService::ShaderReadMemory(const Graphics::Context& context, VkDeviceSize bytes) {
    constexpr VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    constexpr VkMemoryPropertyFlags device = host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    static const bool hostOnly = std::getenv("APS5_RECODE_HOST_SHADER_DATA") != nullptr;
    if (hostOnly) return host;
    for (std::uint32_t i = 0; i < context.memory.memoryTypeCount; ++i) {
        const auto& type = context.memory.memoryTypes[i];
        const auto& heap = context.memory.memoryHeaps[type.heapIndex];
        if ((type.propertyFlags & device) == device && (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 && heap.size >= bytes * 4u) return device;
    }
    return host;
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
    device->SetBatched(batchedBinds);
    ledger = std::make_unique<Residency>(*device, 0);
    ledger->SetKeep(keep);
    uploads = std::make_unique<ResidentUploads>(*ledger, GuestMemory::CommittedRanges);
    uploads->SetView([](std::uint64_t address, std::size_t bytes) { return static_cast<const std::byte*>(GuestMemory::OwnStoreTarget(address, bytes)); });
    if (PageTableEnabled()) {
        pages = std::make_unique<GpuPageTable>();
        pageBuffer = std::make_shared<Graphics::Buffer>(*context, Abi::PageTableBytes(PageTableLeafCapacity), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, ShaderReadMemory(*context, Abi::PageTableBytes(PageTableLeafCapacity)));
        auto bytes = pageBuffer->Bytes();
        std::memset(bytes.data(), 0, bytes.size());
        const Abi::Header header{Abi::PageTableVersion, 0, sizeof(std::uint64_t), 0};
        std::memcpy(bytes.data(), &header, sizeof(header));
        ledger->SetPageTable(pages.get());
        if (std::getenv("APS5_RECODE_NO_NULL_PAGE") == nullptr) {
            nullPage = std::make_shared<Graphics::Buffer>(*context, GpuPageBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, ShaderReadMemory(*context, GpuPageBytes));
            std::memset(nullPage->Bytes().data(), 0, nullPage->Bytes().size());
            pages->Map(0, GpuPageBytes, nullPage->DeviceAddress(), GpuPageRead);
        }
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

std::optional<DeviceSpan> ResidencyService::Serve(std::uint64_t address, std::uint64_t size, std::span<const std::pair<std::uint64_t, std::uint64_t>> committed, std::uint64_t owner, std::uint64_t ownerSize) {
    if (!Attached() || size == 0) return std::nullopt;
    std::lock_guard lock(mutex);
    const auto end = address + size;
    if (!ledger->Owns(address, size)) {
        if (ownerSize != 0 && owner <= address && end - owner <= ownerSize) {
            ledger->RemoveUnboundInside(owner, ownerSize);
            if (ledger->Add(owner, ownerSize)) ++ownerRanges;
        }
        if (!claim(address, size, ownerSize != 0 ? "owned" : "mirror")) {
            ++serveRefusals;
            noteRefusal("the ledger takes no range there", address, size);
            return std::nullopt;
        }
    }
    if (auto [at, added] = served.try_emplace(address, end); !added) at->second = std::max(at->second, end);
    const auto now = frame.load(std::memory_order_relaxed);
    for (const auto& [from, to] : committed) {
        const auto first = std::max(from, address);
        const auto last = std::min(to, end);
        if (first < last && !uploads->Prepare(first, last - first, now)) {
            ++serveRefusals;
            noteRefusal("a committed part could not be made resident", first, last - first);
            return std::nullopt;
        }
    }
    if (auto whole = ledger->Translate(address, size)) return whole;
    auto base = ledger->Base(address);
    if (base) base->bytes = std::min(base->bytes, size);
    return base;
}

bool ResidencyService::ServeFaultPage(std::uint64_t page, std::uint64_t allocation, std::uint64_t allocationBytes, std::span<const std::pair<std::uint64_t, std::uint64_t>> committed) {
    if (!Attached()) return false;
    std::lock_guard lock(mutex);
    if (!pages) return false;
    const auto end = page + GpuPageBytes;
    if (pages->Lookup(page) != 0) {
        ++faultPagesMapped;
        return true;
    }
    if (!ledger->Owns(page, GpuPageBytes)) {
        bool added = false;
        if (allocationBytes != 0 && allocation <= page && end - allocation <= allocationBytes) {
            ledger->RemoveUnboundInside(allocation, allocationBytes);
            added = ledger->Add(allocation, allocationBytes);
        }
        if (!added) added = claim(page, GpuPageBytes, "page");
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
    if (const auto bound = ledger->Translate(page, GpuPageBytes); bound && pages->Lookup(page) == 0) pages->Map(page, GpuPageBytes, bound->address, GpuPagePermissions);
    served.try_emplace(page, end);
    ++faultPagesServed;
    return true;
}

void ResidencyService::MapImports(std::uint64_t serial, std::shared_ptr<const std::vector<Abi::Range>> ranges) {
    if (!Attached() || ranges == nullptr) return;
    std::vector<SpaceCommit> commits;
    if (mirrorSpaces.load(std::memory_order_acquire)) {
        commits.reserve(ranges->size());
        for (const auto& range : *ranges) {
            if (range.end > range.begin) commits.push_back({range.begin, range.end - range.begin, GuestMemory::CommittedRanges(range.begin, static_cast<std::size_t>(range.end - range.begin))});
        }
    }
    std::lock_guard lock(mutex);
    if (!pages || serial == importSerial) return;
    if (mirrorSpaces.load(std::memory_order_relaxed)) {
        serveSpaces(commits);
        importSerial = serial;
        importSpaces.push_back(std::move(ranges));
        while (importSpaces.size() > ImportSpacesKept) importSpaces.pop_front();
        ++importMaps;
        return;
    }
    std::vector<LinearSpan> spans;
    spans.reserve(ranges->size());
    for (const auto& range : *ranges) spans.push_back({range.begin, range.end, range.deviceAddress});
    auto next = WholePages(spans);
    for (const auto& [page, target] : importPages) {
        if (const auto found = next.find(page); found != next.end() && found->second == target) continue;
        if (pages->Lookup(page) != target) continue;
        pages->Unmap(page, GpuPageBytes);
        if (const auto bound = ledger->Translate(page, GpuPageBytes)) pages->Map(page, GpuPageBytes, bound->address, GpuPagePermissions);
    }
    for (const auto& [page, target] : next) {
        if (pages->Lookup(page) != target) pages->Map(page, GpuPageBytes, target, GpuPagePermissions);
    }
    importPages = std::move(next);
    importSerial = serial;
    importSpaces.push_back(std::move(ranges));
    while (importSpaces.size() > ImportSpacesKept) importSpaces.pop_front();
    ++importMaps;
}

std::vector<std::uint64_t> ResidencyService::ForgetImport(std::uint64_t base, std::uint64_t bytes, std::uint64_t deviceBegin, std::uint64_t deviceEnd) {
    std::vector<std::uint64_t> forgotten;
    if (!Attached() || bytes == 0u) return forgotten;
    std::lock_guard lock(mutex);
    if (!pages) return forgotten;
    for (auto it = importPages.lower_bound(base & ~(GpuPageBytes - 1)); it != importPages.end() && it->first < base + bytes;) {
        const auto [page, target] = *it;
        if (target < deviceBegin || target >= deviceEnd) {
            ++it;
            continue;
        }
        if (pages->Lookup(page) == target) {
            pages->Unmap(page, GpuPageBytes);
            if (const auto bound = ledger->Translate(page, GpuPageBytes)) pages->Map(page, GpuPageBytes, bound->address, GpuPagePermissions);
        }
        it = importPages.erase(it);
        ++importPagesForgotten;
    }
    importSerial = 0;
    for (auto it = writtenImports.lower_bound(base & ~(GpuPageBytes - 1)); it != writtenImports.end() && it->first < base + bytes;) {
        const auto [chunk, target] = *it;
        if (target < deviceBegin || target >= deviceEnd) {
            ++it;
            continue;
        }
        if (pages->Lookup(chunk) == target) {
            pages->Unmap(chunk, GpuPageBytes);
            const std::array refresh{DirtyRange{chunk, GpuPageBytes}};
            uploads->NoteDirty(refresh);
            if (const auto bound = ledger->Translate(chunk, GpuPageBytes)) pages->Map(chunk, GpuPageBytes, bound->address, GpuPagePermissions);
        }
        forgotten.push_back(chunk);
        it = writtenImports.erase(it);
        ++writtenImportsForgotten;
    }
    return forgotten;
}

bool ResidencyService::MapWrittenImport(std::uint64_t chunk, std::uint64_t target) {
    if (!Attached() || target == 0u) return false;
    std::lock_guard lock(mutex);
    if (!pages || mirrorSpaces || writtenImports.contains(chunk)) return false;
    pages->Map(chunk, GpuPageBytes, target, GpuPagePermissions);
    writtenImports.emplace(chunk, target);
    return true;
}

bool ResidencyService::WrittenImportAny(std::uint64_t address, std::uint64_t size) const {
    if (!Attached() || size == 0) return false;
    std::lock_guard lock(mutex);
    const auto found = writtenImports.lower_bound(address & ~(ChunkBytes - 1));
    return found != writtenImports.end() && found->first < address + size;
}

std::vector<std::uint64_t> ResidencyService::UnmappedPages(std::uint64_t address, std::uint64_t size) const {
    std::vector<std::uint64_t> missing;
    if (!Attached() || size == 0) return missing;
    std::lock_guard lock(mutex);
    if (!pages) return missing;
    for (auto page = address & ~(GpuPageBytes - 1); page < address + size; page += GpuPageBytes) {
        if (pages->Lookup(page) == 0) missing.push_back(page);
    }
    return missing;
}

std::optional<DeviceSpan> ResidencyService::ReturnWrittenImport(std::uint64_t chunk) {
    if (!Attached()) return std::nullopt;
    std::lock_guard lock(mutex);
    if (!pages) return std::nullopt;
    const auto found = writtenImports.find(chunk);
    if (found == writtenImports.end()) return std::nullopt;
    auto bound = ledger->Translate(chunk, GpuPageBytes);
    if (!bound) {
        if (!claim(chunk, GpuPageBytes, "chunk")) return std::nullopt;
        if (!uploads->PrepareFilled(chunk, GpuPageBytes, frame.load(std::memory_order_relaxed))) return std::nullopt;
        bound = ledger->Translate(chunk, GpuPageBytes);
        if (!bound) return std::nullopt;
        served.try_emplace(chunk, chunk + GpuPageBytes);
        ++writtenImportsBound;
    }
    if (pages->Lookup(chunk) == found->second) {
        pages->Unmap(chunk, GpuPageBytes);
        pages->Map(chunk, GpuPageBytes, bound->address, GpuPagePermissions);
    }
    writtenImports.erase(found);
    ++writtenImportsReturned;
    return bound;
}

bool ResidencyService::WrittenImport(std::uint64_t chunk) const {
    if (!Attached()) return false;
    std::lock_guard lock(mutex);
    return writtenImports.contains(chunk);
}

bool ResidencyService::WrittenImportThroughout(std::uint64_t address, std::uint64_t size) const {
    if (!Attached() || size == 0) return false;
    std::lock_guard lock(mutex);
    const auto first = address & ~(ChunkBytes - 1);
    auto found = writtenImports.lower_bound(first);
    for (auto chunk = first; chunk < address + size; chunk += ChunkBytes, ++found) {
        if (found == writtenImports.end() || found->first != chunk) return false;
    }
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
    if (ReadsOffGpuMutex()) {
        if (!readOnSettleQueue(pieces, staging.Handle())) {
            ++readBackFailures;
            return {};
        }
        ++settleReadBacks;
    } else {
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

bool ResidencyService::ReadsOffGpuMutex() const {
    return Attached() && context->settleQueue != VK_NULL_HANDLE && context->settleQueueMutex != nullptr && context->settlePool != VK_NULL_HANDLE;
}

bool ResidencyService::readOnSettleQueue(std::span<const PendingUpload> pieces, VkBuffer staging) {
    const auto& c = *context;
    std::lock_guard queueLock(*c.settleQueueMutex);
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = c.settlePool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (c.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(c.device, &allocate, &commands) != VK_SUCCESS) return false;
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    bool done = c.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin) == VK_SUCCESS;
    if (done) {
        Graphics::RecordMemoryBarrier(c, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, ResidentAccess, VK_ACCESS_TRANSFER_READ_BIT);
        const auto copy = c.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
        std::size_t at = 0;
        for (const auto& piece : pieces) {
            const VkBufferCopy region{piece.offset, at, piece.bytes.size()};
            copy(commands, bufferOf(piece.handle), staging, 1, &region);
            at += piece.bytes.size();
        }
        Graphics::RecordMemoryBarrier(c, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        done = c.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands) == VK_SUCCESS;
    }
    VkFence fence = VK_NULL_HANDLE;
    if (done) {
        const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        done = c.Function<PFN_vkCreateFence>("vkCreateFence")(c.device, &fenceInfo, nullptr, &fence) == VK_SUCCESS;
    }
    if (done) {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        done = c.Function<PFN_vkQueueSubmit>("vkQueueSubmit")(c.settleQueue, 1, &submit, fence) == VK_SUCCESS && c.Function<PFN_vkWaitForFences>("vkWaitForFences")(c.device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    }
    if (fence != VK_NULL_HANDLE) c.Function<PFN_vkDestroyFence>("vkDestroyFence")(c.device, fence, nullptr);
    c.Function<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(c.device, c.settlePool, 1, &commands);
    return done;
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

std::uint64_t ResidencyService::TranslateMapped(std::uint64_t address, std::uint64_t size, bool write) const {
    if (!Attached() || size == 0) return 0;
    std::lock_guard lock(mutex);
    if (!pages) return 0;
    const auto device = pages->Lookup(address, write);
    if (device == 0) return 0;
    const auto firstPage = address >> GpuPageBits;
    const auto lastPage = (address + size - 1) >> GpuPageBits;
    for (auto page = firstPage + 1; page <= lastPage; ++page) {
        const auto guest = page << GpuPageBits;
        if (pages->Lookup(guest, write) != device + (guest - address)) return 0;
    }
    return device;
}

std::pair<std::uint64_t, std::uint64_t> ResidencyService::MappedPrefix(std::uint64_t address, std::uint64_t size, bool write) const {
    if (!Attached() || size == 0) return {0u, 0u};
    std::lock_guard lock(mutex);
    if (!pages) return {0u, 0u};
    const auto device = pages->Lookup(address, write);
    if (device == 0) return {0u, 0u};
    const auto lastPage = (address + size - 1) >> GpuPageBits;
    for (auto page = (address >> GpuPageBits) + 1; page <= lastPage; ++page) {
        const auto guest = page << GpuPageBits;
        if (pages->Lookup(guest, write) != device + (guest - address)) return {device, guest - address};
    }
    return {device, size};
}

std::shared_ptr<Graphics::Buffer> ResidencyService::staging(std::size_t bytes) {
    const auto rounded = std::bit_ceil(std::max(bytes, StagingMinBytes));
    std::size_t idle = 0;
    for (const auto& buffer : stagingPool) {
        if (buffer.use_count() != 1) continue;
        if (buffer->Bytes().size() == rounded) {
            ++stagingReused;
            return buffer;
        }
        idle += buffer->Bytes().size();
    }
    if (idle + rounded > StagingIdleBytes) {
        std::erase_if(stagingPool, [&](const std::shared_ptr<Graphics::Buffer>& buffer) {
            if (idle + rounded <= StagingIdleBytes || buffer.use_count() != 1) return false;
            idle -= buffer->Bytes().size();
            return true;
        });
    }
    auto created = std::make_shared<Graphics::Buffer>(*context, rounded, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    stagingPool.push_back(created);
    ++stagingCreated;
    return created;
}

bool ResidencyService::Pending() const {
    if (!Attached()) return false;
    std::lock_guard lock(mutex);
    return uploads->Pending() || (pages && pages->Dirty()) || device->Queued();
}

void ResidencyService::SetBatchedBinds(bool enabled) {
    std::lock_guard lock(mutex);
    batchedBinds = enabled;
    if (device) device->SetBatched(enabled);
}

std::size_t ResidencyService::Record(VkCommandBuffer commands, std::vector<std::shared_ptr<void>>& kept) {
    if (!Attached()) return 0;
    std::lock_guard lock(mutex);
    if (const auto hold = device->Flush()) {
        if (context->recorder != nullptr) {
            context->recorder->HoldNextSubmit(hold->first, hold->second);
        } else {
            const VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &hold->first, &hold->second};
            context->Function<PFN_vkWaitSemaphoresKHR>("vkWaitSemaphoresKHR")(context->device, &wait, UINT64_MAX);
        }
    }
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
    auto staging = this->staging(total);
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

void ResidencyService::SetMirrorSpaces(bool enabled) {
    std::lock_guard lock(mutex);
    mirrorSpaces.store(enabled, std::memory_order_release);
    if (!enabled || !pages) return;
    for (const auto& [page, target] : importPages) {
        if (pages->Lookup(page) != target) continue;
        pages->Unmap(page, GpuPageBytes);
        if (const auto bound = ledger->Translate(page, GpuPageBytes)) pages->Map(page, GpuPageBytes, bound->address, GpuPagePermissions);
    }
    importPages.clear();
    importSerial = 0;
}

std::optional<DeviceSpan> ResidencyService::MirrorSpan(std::uint64_t address, std::uint64_t size) {
    if (!Attached() || size == 0) return std::nullopt;
    if (auto span = Translate(address, size)) return span;
    const auto committed = GuestMemory::CommittedRanges(address, static_cast<std::size_t>(size));
    if (!Serve(address, size, committed)) return std::nullopt;
    return Translate(address, size);
}

bool ResidencyService::claim(std::uint64_t address, std::uint64_t size, const char* user) {
    if (ledger->Owns(address, size) || !ledger->Covering(address, size).empty()) return true;
    ledger->RemoveUnboundInside(address, size);
    if (ledger->Add(address, size)) return true;
    static const char* const chosen = std::getenv("APS5_RECODE_COVER");
    static const std::string covered = chosen != nullptr ? chosen : "all";
    if (covered != "all" && covered.find(user) == std::string::npos) return false;
    std::vector<GuestSpan> gaps;
    if (!ledger->Cover(address, size, &gaps)) return false;
    if (++coveredSpans <= 16) {
        std::string line = "[recode-residency] span covered across ranges (" + std::string(user) + "): ";
        char text[64];
        std::snprintf(text, sizeof(text), "0x%llx+0x%llx; gaps", static_cast<unsigned long long>(address), static_cast<unsigned long long>(size));
        line += text;
        for (const auto& gap : gaps) {
            std::snprintf(text, sizeof(text), " 0x%llx+0x%llx", static_cast<unsigned long long>(gap.address), static_cast<unsigned long long>(gap.size));
            line += text;
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    return true;
}

void ResidencyService::noteRefusal(const char* why, std::uint64_t address, std::uint64_t size) {
    if (refusalLines >= 8) return;
    ++refusalLines;
    std::fprintf(stderr, "[recode-residency] serve refused, %s: 0x%llx+0x%llx\n", why, static_cast<unsigned long long>(address), static_cast<unsigned long long>(size));
}

void ResidencyService::serveSpaces(std::span<const SpaceCommit> commits) {
    const auto now = frame.load(std::memory_order_relaxed);
    for (const auto& commit : commits) {
        const auto size = commit.size;
        if (!claim(commit.address, size, "space")) {
            ++spacesRefused;
            noteRefusal("the ledger takes no range for a space there", commit.address, size);
            continue;
        }
        bool served = true;
        for (const auto& [from, to] : commit.committed) {
            if (to <= from || uploads->Prepare(from, to - from, now)) continue;
            served = false;
            noteRefusal("a committed part of a space could not be made resident", from, to - from);
        }
        if (served) {
            ++spacesServed;
            spaceBytes += size;
        } else {
            ++spacesRefused;
        }
    }
}

void ResidencyService::SetKeep(Residency::Keep next) {
    std::lock_guard lock(mutex);
    keep = next;
    if (ledger) ledger->SetKeep(next);
}

std::uint64_t ResidencyService::Removals() const {
    if (!Attached()) return 0;
    std::lock_guard lock(mutex);
    return ledger->Removals();
}

std::string ResidencyService::Line() const {
    if (!Attached()) return {};
    std::lock_guard lock(mutex);
    const auto ledgerCounts = ledger->Counts();
    const auto uploadCounts = uploads->Counts();
    std::uint64_t stagingHeld = 0;
    for (const auto& buffer : stagingPool) stagingHeld += buffer->Bytes().size();
    char line[768];
    std::snprintf(line, sizeof(line), "[recode-residency] ranges %llu, pools %llu, bound %llu MiB of %llu MiB budget, chunks bound %llu, evicted %llu, refused %llu; uploads dirty %llu (%llu MiB) full %llu (%llu MiB) not resident %llu MiB; recorded %llu (%llu MiB); staging %llu created, %llu reused, %llu held (%llu MiB); served %llu, serves refused %llu, allocation ranges %llu, spans covered across ranges %llu (%llu gap ranges)\n",
        static_cast<unsigned long long>(ledgerCounts.ranges), static_cast<unsigned long long>(ledgerCounts.pools), static_cast<unsigned long long>(ledgerCounts.boundBytes >> 20), static_cast<unsigned long long>(ledgerCounts.budget >> 20),
        static_cast<unsigned long long>(ledgerCounts.bound), static_cast<unsigned long long>(ledgerCounts.evicted), static_cast<unsigned long long>(ledgerCounts.refused),
        static_cast<unsigned long long>(uploadCounts.dirtyUploads), static_cast<unsigned long long>(uploadCounts.dirtyBytes >> 20), static_cast<unsigned long long>(uploadCounts.fullUploads), static_cast<unsigned long long>(uploadCounts.fullBytes >> 20), static_cast<unsigned long long>(uploadCounts.skippedBytes >> 20),
        static_cast<unsigned long long>(recordedUploads), static_cast<unsigned long long>(recordedBytes >> 20), static_cast<unsigned long long>(stagingCreated), static_cast<unsigned long long>(stagingReused), static_cast<unsigned long long>(stagingPool.size()), static_cast<unsigned long long>(stagingHeld >> 20), static_cast<unsigned long long>(served.size()), static_cast<unsigned long long>(serveRefusals), static_cast<unsigned long long>(ownerRanges), static_cast<unsigned long long>(coveredSpans), static_cast<unsigned long long>(ledgerCounts.gapRanges));
    if (!pages) return line;
    char table[768];
    std::snprintf(table, sizeof(table), "[recode-page-table] leaves %u of %u, updates %llu (%llu MiB), leaves dropped %llu, shared pages %llu; imports mapped %llu pages, %llu spaces, %llu pages of retired imports dropped; fault pages served %llu, already mapped %llu, refused %llu; read backs %llu (%llu KiB), not resident %llu KiB; GPU-written chunks on their import %llu (%llu forgotten, %llu returned, %llu of them bound on return)\n", pages->Leaves(), PageTableLeafCapacity,
        static_cast<unsigned long long>(pageUpdates), static_cast<unsigned long long>(pageUpdateBytes >> 20), static_cast<unsigned long long>(pageLeavesDropped), static_cast<unsigned long long>(ledgerCounts.sharedPages),
        static_cast<unsigned long long>(importPages.size()), static_cast<unsigned long long>(importMaps), static_cast<unsigned long long>(importPagesForgotten), static_cast<unsigned long long>(faultPagesServed), static_cast<unsigned long long>(faultPagesMapped), static_cast<unsigned long long>(faultPagesRefused), static_cast<unsigned long long>(readBacks), static_cast<unsigned long long>(readBackBytes >> 10), static_cast<unsigned long long>(readBackMissedBytes >> 10), static_cast<unsigned long long>(writtenImports.size()), static_cast<unsigned long long>(writtenImportsForgotten), static_cast<unsigned long long>(writtenImportsReturned), static_cast<unsigned long long>(writtenImportsBound));
    std::string spaces;
    const auto binds = device->TakeCounts();
    if (binds.calls != 0 || binds.queued != 0) {
        char text[224];
        std::snprintf(text, sizeof(text), "[recode-binds] %s: %llu vkQueueBindSparse calls (%llu waited on a fence), %llu chunks, %llu queued, %.1f ms in the calls\n", batchedBinds ? "batched" : "one call per bind", static_cast<unsigned long long>(binds.calls), static_cast<unsigned long long>(binds.syncCalls), static_cast<unsigned long long>(binds.chunks), static_cast<unsigned long long>(binds.queued), static_cast<double>(binds.nanoseconds) / 1e6);
        spaces += text;
    }
    if (settleReadBacks != 0 || readBackFailures != 0) {
        char text[160];
        std::snprintf(text, sizeof(text), "[recode-readback] %llu read-backs on the settle queue, %llu failed\n", static_cast<unsigned long long>(settleReadBacks), static_cast<unsigned long long>(readBackFailures));
        spaces += text;
    }
    if (mirrorSpaces) {
        char text[192];
        std::snprintf(text, sizeof(text), "[recode-mirror-spaces] served %llu, refused %llu, %llu MiB\n", static_cast<unsigned long long>(spacesServed), static_cast<unsigned long long>(spacesRefused), static_cast<unsigned long long>(spaceBytes >> 20));
        spaces += text;
    }
    return std::string(line) + table + spaces;
}

}
