#include "prx/libSceAgcDriver/Unified/include/Images.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <algorithm>
#include <exception>

namespace AgcDriver::Unified::Images {

std::optional<ImageKey> KeyOf(std::span<const std::uint32_t, 8> words, std::string& reason) {
    Graphics::GuestTextureResource resource{};
    std::uint64_t bytes = 0;
    try {
        resource = Graphics::DecodeTextureResource(words);
        bytes = Graphics::SurfaceGuestBytes(resource);
    } catch (const std::exception& error) {
        reason = std::string("image decode: ") + error.what();
        return std::nullopt;
    }
    if (resource.baseAddress == 0u || bytes == 0u) {
        reason = "image without memory";
        return std::nullopt;
    }
    if (resource.samples > 1u || resource.fmaskCmaskAddress != 0u) {
        reason = "multisampled image";
        return std::nullopt;
    }
    if (resource.dccAddress != 0u) {
        reason = "compressed image metadata";
        return std::nullopt;
    }
    ImageKey key;
    key.surface = {resource.baseAddress, bytes, resource.format, static_cast<std::uint32_t>(resource.tileMode), static_cast<std::uint32_t>(resource.dimension), resource.width, resource.height, resource.depthOrLastArray, std::max(resource.mipCount, resource.allocatedMipCount), resource.pipeBankXor, resource.gcnPitch != 0u ? resource.gcnPitch : resource.linearPitch};
    const auto swizzle = static_cast<std::uint32_t>(resource.dstSelX) | static_cast<std::uint32_t>(resource.dstSelY) << 4u | static_cast<std::uint32_t>(resource.dstSelZ) << 8u | static_cast<std::uint32_t>(resource.dstSelW) << 12u | static_cast<std::uint32_t>(resource.bcSwizzle) << 16u;
    key.view = {resource.baseLevel, resource.lastLevel, resource.baseArray, resource.depthOrLastArray, swizzle};
    return key;
}

std::size_t SurfaceTable::KeyHash::operator()(const SurfaceKey& key) const {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 0x100000001b3ull; };
    mix(key.address);
    mix(key.bytes);
    mix(key.format);
    mix(key.tileMode);
    mix(key.dimension);
    mix(key.width);
    mix(key.height);
    mix(key.depth);
    mix(key.mips);
    mix(key.pipeBankXor);
    mix(key.pitch);
    return static_cast<std::size_t>(hash);
}

std::optional<Found> SurfaceTable::Find(const SurfaceKey& key, std::span<const std::uint32_t, 8> words, std::string& reason) {
    std::lock_guard lock(mutex);
    counts.lookups++;
    if (images.ChangedSince(key.address, key.bytes, 0)) {
        counts.gpuWritten++;
        return Found{nullptr, false, true};
    }
    if (const auto found = surfaces.find(key); found != surfaces.end()) {
        auto& surface = found->second;
        if (surface->state == State::Ready && (shared.ChangedSince(key.address, key.bytes, surface->stamp) || images.ChangedSince(key.address, key.bytes, surface->imageStamp))) {
            surface->state = State::Stale;
            work.push_back(surface);
            counts.stale++;
        }
        if (surface->state == State::Ready) counts.ready++;
        else counts.pending++;
        return Found{surface, surface->state == State::Ready};
    }
    auto created = device.Create(key, words, reason);
    if (created == nullptr) {
        counts.refused++;
        return std::nullopt;
    }
    auto surface = std::make_shared<Surface>();
    surface->key = key;
    surface->device = std::move(created);
    surfaces.emplace(key, surface);
    work.push_back(surface);
    counts.created++;
    counts.pending++;
    return Found{surface, false};
}

std::vector<std::shared_ptr<Surface>> SurfaceTable::TakeWork() {
    std::lock_guard lock(mutex);
    std::vector<std::shared_ptr<Surface>> taken;
    taken.swap(work);
    return taken;
}

void SurfaceTable::MarkReady(Surface& surface, std::uint64_t stamp, std::uint64_t imageStamp) {
    std::lock_guard lock(mutex);
    surface.state = State::Ready;
    surface.stamp = stamp;
    surface.imageStamp = imageStamp;
    counts.detiled++;
}

void SurfaceTable::Requeue(const std::shared_ptr<Surface>& surface) {
    std::lock_guard lock(mutex);
    work.push_back(surface);
}

TableCounts SurfaceTable::TakeCounts() {
    std::lock_guard lock(mutex);
    const auto taken = counts;
    counts = {};
    return taken;
}

std::size_t SurfaceTable::Size() const {
    std::lock_guard lock(mutex);
    return surfaces.size();
}

}
