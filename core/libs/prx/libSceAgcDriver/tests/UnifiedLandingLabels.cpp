#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Unified/include/DrawRecorder.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "execution/VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>

namespace {

using AgcDriver::Graphics::Recorder;
using AgcDriver::Graphics::Require;

constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t LabelOffset = 8192;
constexpr std::size_t PageOffset = 16384;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint32_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "cannot allocate the landing test block");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(block); }
    std::uint32_t* Words() const { return block; }

private:
    std::uint32_t* block = nullptr;
};

void Prepare(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    const std::array<std::uint32_t, 4> zero{};
    Require(device.FillBuffer(block.Address(), BlockBytes, zero), "the device must import the landing test block");
    device.WaitIdle();
    Require(device.FillBuffer(block.Address() + BlockBytes - 16, 16, zero), "the preceding GPU work was not recorded");
}

void LabelWaitsForALanding(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    Prepare(device, block);
    auto* recorder = Recorder::Active();
    Require(recorder != nullptr, "the Vulkan recorder is not active");
    const auto label = block.Address() + LabelOffset;
    bool landed = false;
    std::uint32_t labelAtLanding = 0xffffffffu;
    AgcDriver::Unified::Record::LandAfterBatch(*recorder, [&] {
        labelAtLanding = block.Words()[LabelOffset / 4];
        block.Words()[PageOffset / 4] = 0x11111111u;
        landed = true;
    });
    Require(AgcDriver::Unified::Record::LandingsPending(), "a scheduled landing is not pending");
    const std::uint32_t value = 0x5a5a5a5au;
    const auto bytes = std::as_bytes(std::span(&value, 1));
    Require(device.WriteLabelOnGpu(label, bytes, 10, 0, false) == 5, "a label written while a landing is pending was not put behind the batch's completions");
    Require(recorder->CompletionLabelIn(label, 4), "the label is not a pending completion store");
    Require(!landed && block.Words()[LabelOffset / 4] == 0u, "the label is visible before the landing ran");
    device.WaitIdle();
    Require(landed, "the landing did not run with its batch");
    Require(labelAtLanding == 0u, "the label was visible when the page written ahead of it landed");
    Require(block.Words()[PageOffset / 4] == 0x11111111u && block.Words()[LabelOffset / 4] == value, "the landing or the label did not reach memory");
    Require(!AgcDriver::Unified::Record::LandingsPending(), "a landing that ran is still pending");
}

void LabelWithoutALandingStaysOnTheGpu(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    Prepare(device, block);
    const auto label = block.Address() + LabelOffset + 64;
    const std::uint32_t value = 0x3c3c3c3cu;
    Require(device.WriteLabelOnGpu(label, std::as_bytes(std::span(&value, 1)), 11, 0, false) == 0, "a label with no landing pending was not stored on the GPU");
    device.WaitIdle();
    Require(block.Words()[(LabelOffset + 64) / 4] == value, "the GPU label did not reach memory");
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_RECODE_HOST_COPY_THREAD", "0");
#else
    setenv("APS5_RECODE_HOST_COPY_THREAD", "0", 1);
#endif
    try {
        GuestBlock block;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        LabelWaitsForALanding(*device, block);
        LabelWithoutALandingStaysOnTheGpu(*device, block);
        LabelWaitsForALanding(*device, block);
        std::cout << "agc_unified_landing_labels_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
