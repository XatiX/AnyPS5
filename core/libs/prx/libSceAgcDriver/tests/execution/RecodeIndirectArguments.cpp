#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#if defined(ANYPS5_UNIFIED)
#include "prx/libSceAgcDriver/Unified/include/IndirectArguments.hpp"

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using namespace AgcDriver::Graphics;
using AgcDriver::Unified::Record::PatchableArguments;
using AgcDriver::Unified::Record::PatchIndirectArguments;
using IndirectDraw = AgcDriver::Pm4::DrawParameters::IndirectDraw;
using Rule = IndirectDraw::Rule;

struct Case {
    const char* name;
    std::uint32_t recordBytes;
    std::uint32_t stride;
    std::uint32_t count;
    Rule vertexRule;
    Rule instanceRule;
};

constexpr std::size_t SourceOffset = 48;
constexpr std::size_t DestinationOffset = 80;
constexpr std::size_t Margin = 64;
constexpr int Untouched = 0xee;

IndirectDraw argumentsOf(const Case& one) {
    IndirectDraw arguments{};
    arguments.recordBytes = one.recordBytes;
    arguments.stride = one.stride;
    arguments.count = one.count;
    arguments.vertexRule = one.vertexRule;
    arguments.instanceRule = one.instanceRule;
    arguments.vertexConstant = 0x11223344u;
    arguments.instanceConstant = 0x55667788u;
    return arguments;
}

void patch(const Context& context, const Case& one) {
    const auto arguments = argumentsOf(one);
    Require(PatchableArguments(arguments), std::string(one.name) + ": refused");
    const auto bytes = static_cast<std::size_t>(arguments.RangeBytes());
    Buffer source(context, SourceOffset + bytes + Margin, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Buffer destination(context, DestinationOffset + bytes + Margin, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::uint32_t state = 0x9e3779b9u;
    for (auto& byte : source.Bytes()) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<std::byte>(state >> 24u);
    }
    std::memset(destination.Bytes().data(), Untouched, destination.Bytes().size());
    std::vector<std::byte> expected(destination.Bytes().begin(), destination.Bytes().end());
    std::memcpy(expected.data() + DestinationOffset, source.Bytes().data() + SourceOffset, bytes);
    for (std::uint32_t record = 0; record < one.count; ++record) {
        const auto base = DestinationOffset + static_cast<std::size_t>(record) * one.stride;
        if (one.vertexRule == Rule::Constant) std::memcpy(expected.data() + base + arguments.VertexDwordOffset(), &arguments.vertexConstant, 4);
        if (one.instanceRule == Rule::Constant) std::memcpy(expected.data() + base + arguments.InstanceDwordOffset(), &arguments.instanceConstant, 4);
    }
    CommandBatch batch(context);
    PatchIndirectArguments(context, batch.Handle(), arguments, source.Handle(), SourceOffset, destination.Handle(), DestinationOffset);
    RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    batch.SubmitAndWait();
    std::size_t wrong = 0;
    std::size_t first = expected.size();
    for (std::size_t at = 0; at < expected.size(); ++at) {
        if (destination.Bytes()[at] == expected[at]) continue;
        ++wrong;
        first = std::min(first, at);
    }
    std::cout << one.name << ": " << bytes << " bytes, " << wrong << " wrong" << '\n';
    Require(wrong == 0u, std::string(one.name) + ": " + std::to_string(wrong) + " bytes differ, the first at " + std::to_string(first));
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        {
            const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
            const Case cases[] = {
                {"one record, both constant", 16u, 16u, 1u, Rule::Constant, Rule::Constant},
                {"one indexed record, both constant", 20u, 20u, 1u, Rule::Constant, Rule::Constant},
                {"one indexed record, stride 0", 20u, 0u, 1u, Rule::Constant, Rule::Constant},
                {"one record, vertex constant", 16u, 16u, 1u, Rule::Constant, Rule::InPlace},
                {"one indexed record, instance constant", 20u, 20u, 1u, Rule::InPlace, Rule::Constant},
                {"seven records, stride 32", 16u, 32u, 7u, Rule::Constant, Rule::Constant},
                {"five indexed records, stride 28", 20u, 28u, 5u, Rule::Constant, Rule::InPlace},
                {"1024 indexed records", 20u, 20u, 1024u, Rule::Constant, Rule::Constant},
                {"no records", 20u, 20u, 0u, Rule::Constant, Rule::Constant},
            };
            int failed = 0;
            for (const auto& one : cases) {
                try {
                    patch(context, one);
                } catch (const std::exception& error) {
                    std::cerr << "FAIL " << error.what() << '\n';
                    ++failed;
                }
            }
            const Case refused[] = {
                {"1025 records", 20u, 20u, 1025u, Rule::Constant, Rule::Constant},
                {"stride 18", 16u, 18u, 2u, Rule::Constant, Rule::Constant},
                {"stride below the record", 20u, 16u, 2u, Rule::Constant, Rule::Constant},
            };
            for (const auto& one : refused) {
                if (!PatchableArguments(argumentsOf(one))) continue;
                std::cerr << "FAIL " << one.name << ": accepted" << '\n';
                ++failed;
            }
            device->WaitIdle();
            if (failed != 0) return 1;
        }
        device.reset();
        std::puts("recode indirect argument tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#else
int main() {
    std::puts("skipped, the unified backend is not built");
    return VulkanTestSkipped;
}
#endif
