#include "prx/libSceAgcDriver/Unified/include/ResidencyService.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Unified {

namespace {

constexpr VkAccessFlags ResidentAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;

VkBuffer bufferOf(std::uint64_t handle) {
    return reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(handle));
}

}

ResidencyService& ResidencyService::Get() {
    static ResidencyService& service = *new ResidencyService();
    return service;
}

bool ResidencyService::Enabled() {
    static const bool enabled = std::getenv("APS5_RECODE_MEMORY") != nullptr && std::getenv("APS5_RECODE_NO_RESIDENCY") == nullptr;
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
    uploads = std::make_unique<ResidentUploads>(*ledger);
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

std::size_t ResidencyService::Record(VkCommandBuffer commands, std::vector<std::shared_ptr<void>>& kept) {
    if (!Attached()) return 0;
    std::lock_guard lock(mutex);
    auto taken = uploads->Take();
    if (taken.empty()) return 0;
    std::size_t total = 0;
    for (const auto& upload : taken) total += upload.bytes.size();
    auto staging = std::make_shared<Graphics::Buffer>(*context, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto bytes = staging->Bytes();
    std::size_t at = 0;
    for (const auto& upload : taken) {
        std::memcpy(bytes.data() + at, upload.bytes.data(), upload.bytes.size());
        at += upload.bytes.size();
    }
    Graphics::RecordMemoryBarrier(*context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, ResidentAccess, VK_ACCESS_TRANSFER_WRITE_BIT);
    const auto copy = context->Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    at = 0;
    for (const auto& upload : taken) {
        const VkBufferCopy region{at, upload.offset, upload.bytes.size()};
        copy(commands, staging->Handle(), bufferOf(upload.handle), 1, &region);
        at += upload.bytes.size();
    }
    Graphics::RecordMemoryBarrier(*context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, ResidentAccess);
    kept.push_back(std::move(staging));
    recordedUploads += taken.size();
    recordedBytes += total;
    return taken.size();
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
    std::snprintf(line, sizeof(line), "[recode-residency] ranges %llu, pools %llu, bound %llu MiB of %llu MiB budget, chunks bound %llu, evicted %llu, refused %llu; uploads dirty %llu (%llu MiB) full %llu (%llu MiB) not resident %llu MiB; recorded %llu (%llu MiB)\n",
        static_cast<unsigned long long>(ledgerCounts.ranges), static_cast<unsigned long long>(ledgerCounts.pools), static_cast<unsigned long long>(ledgerCounts.boundBytes >> 20), static_cast<unsigned long long>(ledgerCounts.budget >> 20),
        static_cast<unsigned long long>(ledgerCounts.bound), static_cast<unsigned long long>(ledgerCounts.evicted), static_cast<unsigned long long>(ledgerCounts.refused),
        static_cast<unsigned long long>(uploadCounts.dirtyUploads), static_cast<unsigned long long>(uploadCounts.dirtyBytes >> 20), static_cast<unsigned long long>(uploadCounts.fullUploads), static_cast<unsigned long long>(uploadCounts.fullBytes >> 20), static_cast<unsigned long long>(uploadCounts.skippedBytes >> 20),
        static_cast<unsigned long long>(recordedUploads), static_cast<unsigned long long>(recordedBytes >> 20));
    return line;
}

}
