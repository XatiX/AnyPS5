#include "prx/libSceAgcDriver/Unified/include/SlotPatch.hpp"
#include <algorithm>
#include <cstring>

namespace AgcDriver::Unified::Resolve {

bool PatchSlots(const ShaderRecompiler::RecompileResult& result, std::span<const std::uint32_t> slots, ShaderRecompiler::TemplateWords& words, std::string& reason) {
    if (!result.directHeap) {
        if (slots.empty()) return true;
        reason = "slots for a stage without a descriptor heap";
        return false;
    }
    if (slots.size() != result.directSlotCount) {
        reason = "slot count differs from the stage's direct slots";
        return false;
    }
    const auto end = static_cast<std::size_t>(result.directSlotDword) + slots.size();
    const auto shaderData = std::ranges::find_if(result.bindings, [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; });
    if (shaderData != result.bindings.end()) {
        const auto index = static_cast<std::uint32_t>(shaderData - result.bindings.begin());
        auto refilled = std::ranges::find_if(words.bindings, [&](const auto& pair) { return pair.first == index; });
        if (refilled == words.bindings.end()) {
            words.bindings.emplace_back(index, shaderData->guestDescriptor);
            refilled = std::prev(words.bindings.end());
        }
        auto& target = refilled->second;
        if (target.size() < end) target.resize(end, 0u);
        std::ranges::copy(slots, target.begin() + result.directSlotDword);
        return true;
    }
    if (end * sizeof(std::uint32_t) > words.pushConstants.size()) {
        reason = "direct slots lie outside the push data";
        return false;
    }
    std::memcpy(words.pushConstants.data() + static_cast<std::size_t>(result.directSlotDword) * sizeof(std::uint32_t), slots.data(), slots.size_bytes());
    return true;
}

}
