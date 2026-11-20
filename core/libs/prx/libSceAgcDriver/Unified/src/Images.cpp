#include "prx/libSceAgcDriver/Unified/include/Images.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <algorithm>
#include <exception>

namespace AgcDriver::Unified::Images {

std::optional<ImageKey> KeyOf(std::span<const std::uint32_t, 8> words, std::string& reason) {
    Graphics::GuestTextureResource resource{};
    try {
        resource = Graphics::DecodeTextureResource(words);
    } catch (const std::exception& error) {
        reason = std::string("image decode: ") + error.what();
        return std::nullopt;
    }
    return KeyOf(resource, reason);
}

std::optional<ImageKey> KeyOf(const Graphics::GuestTextureResource& resource, std::string& reason) {
    std::uint64_t bytes = 0;
    try {
        bytes = Graphics::SurfaceGuestBytes(resource);
    } catch (const std::exception& error) {
        reason = std::string("image size: ") + error.what();
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

std::shared_ptr<DeviceSurface> SurfaceDevice::CreateFromResource(const SurfaceKey&, const Graphics::GuestTextureResource&, std::string& reason) {
    reason = "surface device cannot create from a resource";
    return nullptr;
}

std::optional<Found> SurfaceTable::Find(const SurfaceKey& key, std::span<const std::uint32_t, 8> words, std::string& reason) {
    return findWith(key, [&](std::string& why) { return device.Create(key, words, why); }, reason);
}

std::optional<Found> SurfaceTable::FindResource(const SurfaceKey& key, const Graphics::GuestTextureResource& resource, std::string& reason) {
    return findWith(key, [&](std::string& why) { return device.CreateFromResource(key, resource, why); }, reason);
}

template<typename TCreate>
std::optional<Found> SurfaceTable::findWith(const SurfaceKey& key, TCreate&& create, std::string& reason) {
    std::lock_guard lock(mutex);
    counts.lookups++;
    const auto found = surfaces.find(key);
    const auto owner = found != surfaces.end() ? OwnerOf(*found->second) : std::uintptr_t{0};
    if (gpuWrites.ChangedSince(key.address, key.bytes, 0, owner)) {
        if (found != surfaces.end()) found->second->gpuNewer = false;
        counts.gpuWritten++;
        return Found{nullptr, false, true};
    }
    if (found != surfaces.end()) {
        auto& surface = found->second;
        surface->used = ++clock;
        if (surface->gpuNewer) {
            if (!shared.ChangedSince(key.address, key.bytes, surface->stamp, owner)) {
                counts.ownServed++;
                counts.ready++;
                return Found{surface, true};
            }
            surface->gpuNewer = false;
            counts.gpuWritten++;
            return Found{nullptr, false, true};
        }
        if (surface->state == State::Ready && (shared.ChangedSince(key.address, key.bytes, surface->stamp, Events::ImageSetOwner) || gpuWrites.ChangedSince(key.address, key.bytes, surface->imageStamp))) {
            surface->state = State::Stale;
            work.push_back(surface);
            counts.stale++;
        }
        if (surface->state == State::Ready) counts.ready++;
        else counts.pending++;
        return Found{surface, surface->state == State::Ready};
    }
    auto created = create(reason);
    if (created == nullptr) {
        counts.refused++;
        return std::nullopt;
    }
    auto surface = std::make_shared<Surface>();
    surface->key = key;
    surface->device = std::move(created);
    surface->used = ++clock;
    surfaces.emplace(key, surface);
    heldBytes += key.bytes;
    work.push_back(surface);
    counts.created++;
    counts.pending++;
    if (budget != 0u && heldBytes > budget) evict();
    return Found{surface, false};
}

void SurfaceTable::evict() {
    std::vector<std::pair<std::uint64_t, SurfaceKey>> order;
    order.reserve(surfaces.size());
    for (const auto& [key, surface] : surfaces) order.emplace_back(surface->used, key);
    std::ranges::sort(order, {}, &std::pair<std::uint64_t, SurfaceKey>::first);
    const auto target = budget - budget / 8u;
    for (const auto& [used, key] : order) {
        if (heldBytes <= target || surfaces.size() <= 1u) break;
        const auto found = surfaces.find(key);
        found->second->retired = true;
        heldBytes -= key.bytes;
        surfaces.erase(found);
        counts.evicted++;
    }
}

std::vector<std::shared_ptr<Surface>> SurfaceTable::TakeWork() {
    std::lock_guard lock(mutex);
    std::vector<std::shared_ptr<Surface>> taken;
    taken.swap(work);
    std::erase_if(taken, [](const std::shared_ptr<Surface>& surface) { return surface->retired; });
    return taken;
}

void SurfaceTable::MarkReady(Surface& surface, std::uint64_t stamp, std::uint64_t imageStamp) {
    std::lock_guard lock(mutex);
    surface.state = State::Ready;
    surface.stamp = stamp;
    surface.imageStamp = imageStamp;
    counts.detiled++;
}

void SurfaceTable::MarkGpuWritten(Surface& surface) {
    std::lock_guard lock(mutex);
    surface.state = State::Ready;
    surface.gpuNewer = true;
    surface.stamp = shared.Serial();
    surface.imageStamp = gpuWrites.Serial();
    counts.ownWrites++;
}

void SurfaceTable::ClearGpuNewer(Surface& surface) {
    std::lock_guard lock(mutex);
    surface.gpuNewer = false;
}

std::vector<std::shared_ptr<Surface>> SurfaceTable::Overlapping(std::uint64_t address, std::uint64_t bytes) const {
    std::lock_guard lock(mutex);
    std::vector<std::shared_ptr<Surface>> overlapping;
    const auto end = address + bytes;
    for (const auto& [key, surface] : surfaces) {
        if (key.address < end && address < key.address + key.bytes) overlapping.push_back(surface);
    }
    return overlapping;
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

std::uint64_t SurfaceTable::HeldBytes() const {
    std::lock_guard lock(mutex);
    return heldBytes;
}

}
