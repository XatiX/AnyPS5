#include "prx/libSceAgcDriver/Unified/include/Images.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
    if (resource.dccAddress != 0u && !MetadataRecords()) {
        reason = "compressed image metadata";
        return std::nullopt;
    }
    ImageKey key;
    key.surface = {resource.baseAddress, bytes, resource.format, static_cast<std::uint32_t>(resource.tileMode), static_cast<std::uint32_t>(resource.dimension), resource.width, resource.height, resource.depthOrLastArray, std::max(resource.mipCount, resource.allocatedMipCount), resource.pipeBankXor, resource.gcnPitch != 0u ? resource.gcnPitch : resource.linearPitch};
    const auto swizzle = static_cast<std::uint32_t>(resource.dstSelX) | static_cast<std::uint32_t>(resource.dstSelY) << 4u | static_cast<std::uint32_t>(resource.dstSelZ) << 8u | static_cast<std::uint32_t>(resource.dstSelW) << 12u | static_cast<std::uint32_t>(resource.bcSwizzle) << 16u;
    key.view = {resource.baseLevel, resource.lastLevel, resource.baseArray, resource.depthOrLastArray, swizzle, resource.format};
    return key;
}

bool MetadataRecords() {
    static const bool records = [] {
        const char* value = std::getenv("APS5_RECODE_METADATA_RECORDS");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return records;
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

bool OwnersEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_RECODE_IMAGE_OWNERS");
        return value == nullptr || (std::string_view(value) != "0" && std::string_view(value) != "off");
    }();
    return enabled;
}

bool OwnsClass(std::string_view name) {
    if (!OwnersEnabled()) return false;
    const char* value = std::getenv("APS5_RECODE_IMAGE_OWNERS");
    if (value == nullptr) return true;
    const std::string_view text(value);
    return text == "1" || text == "all" || text.find(name) != std::string_view::npos;
}

bool SameBacking(const SurfaceKey& left, const SurfaceKey& right) {
    if (left == right) return true;
    if (left.address != right.address || left.bytes != right.bytes || left.tileMode != right.tileMode || left.width != right.width || left.height != right.height || left.mips != right.mips || left.pipeBankXor != right.pipeBankXor || left.pitch != right.pitch) return false;
    const auto flat = [](std::uint32_t dimension) { return dimension == static_cast<std::uint32_t>(Graphics::TextureDimension::k2D) || dimension == static_cast<std::uint32_t>(Graphics::TextureDimension::k2DArray); };
    if (left.dimension != right.dimension && !(flat(left.dimension) && flat(right.dimension) && left.depth == right.depth)) return false;
    if (CompressedSiblings(left.format, right.format)) return true;
    return Graphics::BlockWidth(left.format) == 1u && Graphics::BlockWidth(right.format) == 1u && Graphics::BytesPerElement(left.format) == Graphics::BytesPerElement(right.format);
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
    std::erase_if(newer, [](const std::shared_ptr<Surface>& surface) { return !surface->gpuNewer || surface->retired; });
    if (!newer.empty() && (found == surfaces.end() || !found->second->gpuNewer)) {
        for (auto& other : newer) {
            const auto& otherKey = other->key;
            if (otherKey == key || !SameBacking(otherKey, key)) continue;
            const auto otherOwner = OwnerOf(*other);
            if (gpuWrites.ChangedSince(key.address, key.bytes, other->imageStamp, otherOwner) || shared.ChangedSince(key.address, key.bytes, other->stamp, otherOwner)) break;
            other->used = ++clock;
            counts.aliasServed++;
            counts.ready++;
            return Found{other, true};
        }
    }
    const auto owner = found != surfaces.end() ? OwnerOf(*found->second) : std::uintptr_t{0};
    const auto since = found != surfaces.end() && found->second->gpuNewer ? found->second->imageStamp : std::uint64_t{0};
    if (gpuWrites.ChangedSince(key.address, key.bytes, since, owner)) {
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
    indexSurface(surface);
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
        if (found->second->gpuNewer) {
            counts.evictionsKept++;
            continue;
        }
        found->second->retired = true;
        dropStale(*found->second);
        unindexSurface(*found->second);
        heldBytes -= key.bytes;
        surfaces.erase(found);
        counts.evicted++;
    }
}

std::vector<std::shared_ptr<Surface>> SurfaceTable::TakeWork() {
    std::lock_guard lock(mutex);
    std::vector<std::shared_ptr<Surface>> taken;
    taken.swap(work);
    std::erase_if(taken, [](const std::shared_ptr<Surface>& surface) { return surface->retired || surface->gpuNewer; });
    return taken;
}

void SurfaceTable::MarkReady(Surface& surface, std::uint64_t stamp, std::uint64_t imageStamp) {
    std::lock_guard lock(mutex);
    surface.state = State::Ready;
    surface.stamp = stamp;
    surface.imageStamp = imageStamp;
    counts.detiled++;
}

std::shared_ptr<Surface> SurfaceTable::Create(const SurfaceKey& key, std::span<const std::uint32_t, 8> words, std::string& reason) {
    return createWith(key, [&](std::string& why) { return device.Create(key, words, why); }, reason);
}

std::shared_ptr<Surface> SurfaceTable::CreateResource(const SurfaceKey& key, const Graphics::GuestTextureResource& resource, std::string& reason) {
    return createWith(key, [&](std::string& why) { return device.CreateFromResource(key, resource, why); }, reason);
}

template<typename TCreate>
std::shared_ptr<Surface> SurfaceTable::createWith(const SurfaceKey& key, TCreate&& create, std::string& reason) {
    std::lock_guard lock(mutex);
    if (const auto found = surfaces.find(key); found != surfaces.end()) {
        found->second->used = ++clock;
        return found->second;
    }
    auto created = create(reason);
    if (created == nullptr) {
        counts.refused++;
        return nullptr;
    }
    auto surface = std::make_shared<Surface>();
    surface->key = key;
    surface->device = std::move(created);
    surface->used = ++clock;
    surfaces.emplace(key, surface);
    indexSurface(surface);
    heldBytes += key.bytes;
    counts.created++;
    if (budget != 0u && heldBytes > budget) evict();
    return surface;
}

void SurfaceTable::MarkGpuWritten(Surface& surface) {
    std::lock_guard lock(mutex);
    if (!surface.gpuNewer) {
        if (const auto found = surfaces.find(surface.key); found != surfaces.end() && found->second.get() == &surface) newer.push_back(found->second);
    }
    surface.state = State::Ready;
    surface.gpuNewer = true;
    surface.stamp = shared.Serial();
    surface.imageStamp = gpuWrites.Serial();
    counts.ownWrites++;
    if (surface.mirrorStale) return;
    if (const auto found = surfaces.find(surface.key); found != surfaces.end() && found->second.get() == &surface) {
        surface.mirrorStale = true;
        std::lock_guard staleLock(staleMutex);
        stale[surface.key.address] = found->second;
        staleSpan = std::max(staleSpan, surface.key.bytes);
    }
}

void SurfaceTable::ClearGpuNewer(Surface& surface) {
    std::lock_guard lock(mutex);
    surface.gpuNewer = false;
    dropStale(surface);
}

void SurfaceTable::MarkWrittenBack(Surface& surface) {
    std::lock_guard lock(mutex);
    dropStale(surface);
}

void SurfaceTable::dropStale(Surface& surface) {
    if (!surface.mirrorStale) return;
    surface.mirrorStale = false;
    std::lock_guard staleLock(staleMutex);
    if (const auto found = stale.find(surface.key.address); found != stale.end() && found->second.get() == &surface) stale.erase(found);
}

std::vector<std::shared_ptr<Surface>> SurfaceTable::StaleOverlapping(std::uint64_t address, std::uint64_t bytes) const {
    std::vector<std::shared_ptr<Surface>> found;
    std::lock_guard staleLock(staleMutex);
    if (stale.empty() || bytes == 0u) return found;
    const auto end = address + bytes;
    for (auto at = stale.lower_bound(address > staleSpan ? address - staleSpan : 0u); at != stale.end() && at->first < end; ++at) {
        const auto& surface = at->second;
        if (surface->key.address + surface->key.bytes > address) found.push_back(surface);
    }
    return found;
}

bool SurfaceTable::ChangedSinceWrite(const Surface& surface) const {
    std::lock_guard lock(mutex);
    const auto owner = OwnerOf(surface);
    if (shared.ChangedSince(surface.key.address, surface.key.bytes, surface.stamp, owner)) return true;
    if (!gpuWrites.ChangedSince(surface.key.address, surface.key.bytes, surface.imageStamp, owner)) return false;
    return gpuWrites.ChangedOwner(surface.key.address, surface.key.bytes, surface.imageStamp) != Events::TargetOwner;
}

void SurfaceTable::Touch(Surface& surface) {
    std::lock_guard lock(mutex);
    surface.used = ++clock;
}

bool SurfaceTable::RefreshIfChanged(const std::shared_ptr<Surface>& surface) {
    std::lock_guard lock(mutex);
    if (surface == nullptr || surface->retired || surface->gpuNewer || surface->state != State::Ready) return false;
    if (!shared.ChangedSince(surface->key.address, surface->key.bytes, surface->stamp, OwnerOf(*surface))) return false;
    surface->state = State::Pending;
    work.push_back(surface);
    counts.stale++;
    return true;
}

std::string SurfaceTable::ChangeOf(const Surface& surface) const {
    std::lock_guard lock(mutex);
    const auto own = OwnerOf(surface);
    const auto describe = [&](const char* log, std::uintptr_t owner) {
        std::string text = std::string(log) + " owner " + (owner == 0u ? std::string("none (CPU or untracked)") : std::to_string(owner));
        for (const auto& [key, other] : surfaces) {
            if (OwnerOf(*other) != owner || owner == 0u) continue;
            char held[160];
            std::snprintf(held, sizeof(held), ": record 0x%llx+0x%llx %ux%u depth %u mips %u format %u dimension %u%s", static_cast<unsigned long long>(key.address), static_cast<unsigned long long>(key.bytes), key.width, key.height, key.depth, key.mips, key.format, key.dimension, other->retired ? " (retired)" : "");
            return text + held;
        }
        return text + (owner == own ? ": itself" : ": not a live record");
    };
    if (shared.ChangedSince(surface.key.address, surface.key.bytes, surface.stamp, own)) return describe("shared", shared.ChangedOwner(surface.key.address, surface.key.bytes, surface.stamp));
    if (gpuWrites.ChangedSince(surface.key.address, surface.key.bytes, surface.imageStamp, own)) return describe("gpu writes", gpuWrites.ChangedOwner(surface.key.address, surface.key.bytes, surface.imageStamp));
    return "unchanged";
}

void SurfaceTable::indexSurface(const std::shared_ptr<Surface>& surface) {
    if (surface->key.bytes > IndexedSurfaceBytes) {
        large.push_back(surface);
        return;
    }
    byAddress.emplace(surface->key.address, surface);
    widest = std::max(widest, surface->key.bytes);
}

void SurfaceTable::unindexSurface(const Surface& surface) {
    std::erase_if(large, [&](const std::shared_ptr<Surface>& held) { return held.get() == &surface; });
    const auto [first, last] = byAddress.equal_range(surface.key.address);
    for (auto at = first; at != last; ++at) {
        if (at->second.get() != &surface) continue;
        byAddress.erase(at);
        break;
    }
    Events::Images().Note(surface.key.address, surface.key.bytes, Events::ImageSetOwner);
}

template<typename TVisit>
bool SurfaceTable::visitOverlapping(std::uint64_t address, std::uint64_t bytes, TVisit&& visit) const {
    const auto end = address + bytes;
    for (const auto& surface : large) {
        if (surface->key.address < end && address < surface->key.address + surface->key.bytes && visit(surface)) return true;
    }
    for (auto at = byAddress.lower_bound(address > widest ? address - widest : 0u); at != byAddress.end() && at->first < end; ++at) {
        const auto& key = at->second->key;
        if (address < key.address + key.bytes && visit(at->second)) return true;
    }
    return false;
}

std::vector<std::shared_ptr<Surface>> SurfaceTable::Overlapping(std::uint64_t address, std::uint64_t bytes) const {
    std::lock_guard lock(mutex);
    std::vector<std::shared_ptr<Surface>> overlapping;
    static_cast<void>(visitOverlapping(address, bytes, [&](const std::shared_ptr<Surface>& surface) {
        overlapping.push_back(surface);
        return false;
    }));
    return overlapping;
}

bool SurfaceTable::Holds(std::uint64_t address, std::uint64_t bytes) const {
    std::lock_guard lock(mutex);
    return visitOverlapping(address, bytes, [](const std::shared_ptr<Surface>&) { return true; });
}

void SurfaceTable::NoteRefused(std::string_view reason) {
    std::lock_guard lock(mutex);
    refusedImages++;
    const auto found = std::ranges::find(refusals, reason, &std::pair<std::string, std::uint64_t>::first);
    if (found != refusals.end()) found->second++;
    else if (refusals.size() < 16u) refusals.emplace_back(std::string(reason), 1u);
}

void SurfaceTable::Retire(const std::shared_ptr<Surface>& surface) {
    std::lock_guard lock(mutex);
    if (surface == nullptr || surface->retired) return;
    surface->retired = true;
    surface->gpuNewer = false;
    dropStale(*surface);
    const auto found = surfaces.find(surface->key);
    if (found != surfaces.end() && found->second == surface) {
        unindexSurface(*surface);
        heldBytes -= surface->key.bytes;
        surfaces.erase(found);
    }
    std::erase_if(newer, [&](const std::shared_ptr<Surface>& other) { return other == surface; });
    counts.retired++;
}

std::shared_ptr<Surface> SurfaceTable::Seed(const std::shared_ptr<Surface>& surface) {
    std::lock_guard lock(mutex);
    if (surface == nullptr || surface->seeded) return surface;
    surface->seeded = true;
    if (surface->state == State::Pending && !surface->gpuNewer) work.push_back(surface);
    return surface;
}

std::string SurfaceTable::OwnersLine() const {
    std::lock_guard lock(mutex);
    std::string line = "image records: " + std::to_string(surfaces.size()) + " holding " + std::to_string(heldBytes >> 20u) + " MiB; images the records refused " + std::to_string(refusedImages);
    for (const auto& [reason, count] : refusals) line += " [" + reason + " " + std::to_string(count) + "]";
    return line;
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
