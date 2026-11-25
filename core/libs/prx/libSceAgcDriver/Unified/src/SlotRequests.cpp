#include "prx/libSceAgcDriver/Unified/include/SlotRequests.hpp"
#include "prx/libSceAgcDriver/Unified/include/Heap.hpp"
#include "IntermediateRepresentation/IrMetadata/BufferFormat.hpp"
#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/DescriptorBindingBuilder.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "RuntimeAbi.hpp"
#include <algorithm>
#include <exception>
#include <optional>
#include <string>

namespace AgcDriver::Unified::Heap {

namespace {

bool comparesDepth(const ShaderRecompiler::ShaderInfo& info, const ShaderRecompiler::ResourceSnapshot& snapshot, std::uint32_t sampler) {
    for (const auto& pair : info.sampledPairs) {
        if (pair.sampler != sampler || pair.image >= info.images.size() || pair.image >= snapshot.images.size() || pair.image >= info.runtimeImageModes.size() || !info.images[pair.image].depthCompare) continue;
        const auto& modes = info.runtimeImageModes[pair.image];
        try {
            const auto mode = ShaderRecompiler::ResourceMaterializer::RuntimeImageMode(info.images[pair.image], snapshot.images[pair.image], modes);
            if (mode < modes.size() && modes[mode].depthCompare) return true;
        } catch (const std::exception&) {
            continue;
        }
    }
    return false;
}

}

std::uint32_t PointFilterWord(std::uint32_t filter) {
    const bool mipmapped = ((filter >> 26u) & 3u) != 0u;
    return (filter & ~(0xffu << 20u)) | (1u << 24u) | (mipmapped ? 1u << 26u : 0u);
}

bool BuildSlotRequests(const ShaderRecompiler::CompiledShaderInfo& compiled, const ShaderRecompiler::ResourceSnapshot& snapshot, std::vector<SlotRequest>& requests, SlotStatistics& statistics, std::string& reason, bool allowStorage) {
    using Kind = ShaderRecompiler::DescriptorBindingKind;
    const auto& info = compiled.info;
    const auto& layout = compiled.bindings;
    requests.clear();
    requests.reserve(layout.directSlotCount);
    std::vector<std::uint32_t> modeOf(info.images.size(), NoSlot);
    std::size_t slots = 0;
    std::optional<ShaderRecompiler::SamplerCoordinateFacts> coordinates;
    if (std::ranges::any_of(snapshot.samplers, [](const ShaderRecompiler::DescriptorValue& sampler) { return (sampler.dwords[0] & (1u << 15u)) != 0u; })) {
        try {
            coordinates = ShaderRecompiler::ProveSamplerCoordinates(info, snapshot);
        } catch (const std::exception& error) {
            reason = std::string("sampler coordinates: ") + error.what();
            return false;
        }
    }
    for (const auto& binding : layout.descriptors) {
        const bool samplerHeap = binding.kind == Kind::Samplers;
        const bool imageHeap = ShaderRecompiler::ImageBindingResourceClass(binding.kind) != ShaderRecompiler::ImageResourceClass::None;
        if (!samplerHeap && !imageHeap) continue;
        const auto kind = static_cast<std::uint32_t>(binding.kind);
        const bool storageHeap = imageHeap && kind > LastSampledKind;
        if (storageHeap && !allowStorage) {
            reason = "storage image kind";
            return false;
        }
        std::uint32_t previousResource = NoSlot;
        std::uint32_t mipOffset = 0;
        for (std::size_t element = 0; element < binding.resources.size(); ++element) {
            ++slots;
            const auto resource = binding.resources[element];
            SlotRequest request{kind, 0u, {}};
            if (samplerHeap) {
                ++statistics.samplers;
                if (resource >= snapshot.samplers.size()) {
                    reason = "sampler without a snapshot";
                    return false;
                }
                std::copy_n(snapshot.samplers[resource].dwords.begin(), 4u, request.words.begin());
                request.flags = RequestFlag::Sampler;
                if ((request.words[0] & (1u << 15u)) != 0u && coordinates) {
                    if (resource < coordinates->coordinatesIgnored.size() && coordinates->coordinatesIgnored[resource]) request.words[0] &= ~(1u << 15u);
                    else if (resource < coordinates->unnormalized.size() && coordinates->unnormalized[resource]) request.flags |= RequestFlag::Unnormalized;
                }
                if (comparesDepth(info, snapshot, resource)) request.flags |= RequestFlag::Compare;
                if ((element & 1u) != 0u) {
                    request.words[2] = PointFilterWord(request.words[2]);
                    request.flags |= RequestFlag::Point;
                }
                requests.push_back(request);
                continue;
            }
            ++statistics.images;
            if (resource >= snapshot.images.size() || resource >= info.images.size() || resource >= info.runtimeImageModes.size()) {
                reason = "image without a snapshot";
                return false;
            }
            const auto& descriptor = snapshot.images[resource].dwords;
            if (descriptor[0] == 0u && (descriptor[1] & 0xffu) == 0u) {
                ++statistics.inactiveImages;
                request.flags = RequestFlag::Inactive;
                requests.push_back(request);
                continue;
            }
            const auto& modes = info.runtimeImageModes[resource];
            auto mode = modeOf[resource];
            if (mode == NoSlot) {
                try {
                    mode = modeOf[resource] = ShaderRecompiler::ResourceMaterializer::RuntimeImageMode(info.images[resource], snapshot.images[resource], modes);
                } catch (const std::exception& error) {
                    reason = std::string("image mode: ") + error.what();
                    return false;
                }
            }
            if (mode >= modes.size() || ShaderRecompiler::DescriptorBindingForImage(modes[mode]) != binding.kind || modes[mode].constantSwizzle) {
                ++statistics.inactiveImages;
                request.flags = RequestFlag::Inactive;
                requests.push_back(request);
                continue;
            }
            request.words = descriptor;
            if (storageHeap) {
                mipOffset = resource == previousResource ? mipOffset + 1u : 0u;
                previousResource = resource;
                const auto& image = info.images[resource];
                const auto baseLevel = (request.words[3] >> 12u) & 0xfu;
                request.flags = RequestFlag::Storage;
                if (image.written || image.atomic || image.atomic64) request.flags |= RequestFlag::Written;
                if (image.atomic64) request.flags |= RequestFlag::Atomic64;
                else if (image.atomic) request.flags |= RequestFlag::Atomic;
                if (ShaderRecompiler::RuntimeAbi::UintStorageImageBinding(kind)) request.flags |= RequestFlag::UintView;
                request.flags |= (baseLevel + (image.mipMode == ShaderRecompiler::ImageMipMode::DynamicStorage ? mipOffset : 0u)) << RequestFlag::MipShift;
                requests.push_back(request);
                continue;
            }
            if (modes[mode].conversionFormat != ShaderRecompiler::IrBufferFormat::Invalid || modes[mode].depthBits) request.words[3] = (request.words[3] & ~0xfffu) | ShaderRecompiler::ShaderImageIdentitySwizzle;
            if (coordinates && resource < coordinates->unnormalizedImages.size() && coordinates->unnormalizedImages[resource] && resource < coordinates->imagesAtBaseLevel.size() && coordinates->imagesAtBaseLevel[resource]) {
                const auto baseLevel = (request.words[3] >> 12u) & 0xfu;
                request.words[3] = (request.words[3] & ~(0xfu << 16u)) | (baseLevel << 16u);
            }
            requests.push_back(request);
        }
    }
    if (slots != layout.directSlotCount) {
        reason = "slot count disagrees with the layout";
        return false;
    }
    return true;
}

}
