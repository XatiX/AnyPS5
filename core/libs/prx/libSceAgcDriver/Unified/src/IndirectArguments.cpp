#include "prx/libSceAgcDriver/Unified/include/IndirectArguments.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"

namespace AgcDriver::Unified::Record {

bool PatchableArguments(const Pm4::DrawParameters::IndirectDraw& arguments) {
    return arguments.count <= PatchedRecordLimit && arguments.stride % 4u == 0u && (arguments.count <= 1u || arguments.stride >= arguments.recordBytes);
}

void PatchIndirectArguments(const Graphics::Context& context, VkCommandBuffer commands, const Pm4::DrawParameters::IndirectDraw& arguments, VkBuffer source, VkDeviceSize sourceOffset, VkBuffer destination, VkDeviceSize destinationOffset) {
    using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
    const auto bytes = arguments.RangeBytes();
    if (bytes == 0u) return;
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    Graphics::CopyBuffer(context, commands, source, sourceOffset, destination, destinationOffset, bytes);
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const auto update = context.Resolved(&Graphics::DeviceFunctions::cmdUpdateBuffer, "vkCmdUpdateBuffer");
    for (std::uint32_t record = 0; record < arguments.count; ++record) {
        const VkDeviceSize base = destinationOffset + static_cast<VkDeviceSize>(record) * arguments.stride;
        if (arguments.vertexRule == Rule::Constant) update(commands, destination, base + arguments.VertexDwordOffset(), 4, &arguments.vertexConstant);
        if (arguments.instanceRule == Rule::Constant) update(commands, destination, base + arguments.InstanceDwordOffset(), 4, &arguments.instanceConstant);
    }
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
}

}
