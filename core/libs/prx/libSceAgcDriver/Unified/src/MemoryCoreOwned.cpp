#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include "prx/libSceAgcDriver/Unified/include/Downgrades.hpp"
#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace AgcDriver::Unified {

namespace {

constexpr std::uint64_t pageOf(std::uint64_t address) {
    return address & ~(PageBytes - 1);
}

bool chainsLandings() {
    static const bool chains = [] {
        const char* value = std::getenv("APS5_RECODE_CHAINED_LANDINGS");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return chains;
}

}

MemoryCore::~MemoryCore() {
    {
        std::lock_guard lock(parkMutex);
        resolverStop = true;
    }
    parkSignal.notify_all();
    if (resolver.joinable()) resolver.join();
}

void MemoryCore::SetHost(MemoryHost* next) {
    host.store(next, std::memory_order_release);
    if (next == nullptr) return;
    std::lock_guard lock(parkMutex);
    if (resolverStarted) return;
    resolverStarted = true;
    resolver = std::thread([this] { resolverLoop(); });
    resolverThread = resolver.get_id();
}

GpuWriteMark MemoryCore::NoteGpuWrite(std::uint64_t address, std::uint64_t size, std::uint64_t serial, bool takeDirty) {
    GpuWriteMark mark;
    if (size == 0) return mark;
    written.Note(address, size, serial);
    auto* current = host.load(std::memory_order_acquire);
    if (current != nullptr && current->PatientView(address, static_cast<std::size_t>(size)) != nullptr) {
        takeViewed(mark, address, size, takeDirty);
        return mark;
    }
    const auto viewed = current != nullptr ? current->ViewedPieces(address, size) : std::vector<DirtyRange>{};
    auto cursor = address;
    for (const auto& piece : viewed) {
        if (piece.address > cursor) leaveUnviewed(mark, cursor, piece.address - cursor, current);
        takeViewed(mark, piece.address, piece.size, takeDirty);
        cursor = piece.address + piece.size;
    }
    if (cursor < address + size) leaveUnviewed(mark, cursor, address + size - cursor, current);
    if (!viewed.empty()) pieceWrites.fetch_add(1, std::memory_order_relaxed);
    return mark;
}

void MemoryCore::takeViewed(GpuWriteMark& mark, std::uint64_t address, std::uint64_t size, bool takeDirty) {
    std::lock_guard lock(mutex);
    auto take = pages.TakeForGpu(address, size, takeDirty);
    protector.Apply(take.changes);
    protector.Apply(pages.Register(address, size));
    std::uint64_t owned = 0;
    for (const auto& range : take.owned) owned += range.size / PageBytes;
    std::uint64_t open = 0;
    for (const auto& range : take.open) open += range.size;
    ownedPages.fetch_add(owned, std::memory_order_relaxed);
    openBytes.fetch_add(open, std::memory_order_relaxed);
    mark.owned.insert(mark.owned.end(), take.owned.begin(), take.owned.end());
    mark.open.insert(mark.open.end(), take.open.begin(), take.open.end());
    mark.upload.insert(mark.upload.end(), take.dirty.begin(), take.dirty.end());
}

void MemoryCore::leaveUnviewed(GpuWriteMark& mark, std::uint64_t address, std::uint64_t size, MemoryHost* current) {
    if (current != nullptr && unviewedReports.fetch_add(1, std::memory_order_relaxed) < UnviewedReports) {
        const auto firstPage = pageOf(address);
        const auto lastPage = pageOf(address + size - 1);
        std::fprintf(stderr, "[recode-gpu-owned] write outside the second view: 0x%llx+0x%llx, %s memory; first page %s, last page %s\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(size), Protector::RegionKind(address), current->View(firstPage, PageBytes) != nullptr ? "viewed" : "not viewed", current->View(lastPage, PageBytes) != nullptr ? "viewed" : "not viewed");
    }
    const auto drop = [&](std::uint64_t begin, std::uint64_t stop) {
        std::lock_guard lock(mutex);
        for (auto at = begin; at < stop;) {
            const auto next = std::min(stop, pageOf(at) + PageBytes);
            if (pages.StateOf(pageOf(at)) == PageState::GpuOwned) {
                ownedUnviewedBytes.fetch_add(next - at, std::memory_order_relaxed);
            } else {
                written.Discard(at, next - at);
                gpuOnlyBytes.fetch_add(next - at, std::memory_order_relaxed);
            }
            at = next;
        }
    };
    auto cursor = address;
    for (const auto& range : Protector::ReadableRanges(address, size)) {
        if (range.address > cursor) drop(cursor, range.address);
        mark.open.push_back(range);
        unviewedBytes.fetch_add(range.size, std::memory_order_relaxed);
        cursor = range.address + range.size;
    }
    if (cursor < address + size) drop(cursor, address + size);
}

void MemoryCore::NoteLandedGpuWrite(std::uint64_t address, std::uint64_t size, std::uint64_t serial) {
    if (size == 0) return;
    written.Take(address, size, serial);
}

void MemoryCore::LandDiff(std::uint64_t address, std::span<const std::byte> before, std::span<const std::byte> after, std::uint64_t serial) {
    const auto bytes = std::min(before.size(), after.size());
    if (bytes == 0) return;
    auto* current = host.load(std::memory_order_acquire);
    if (current == nullptr) return;
    if (auto* view = current->PatientView(address, bytes); view != nullptr) {
        landDiffThrough(view, address, before.first(bytes), after.first(bytes), serial, *current);
        return;
    }
    for (const auto& piece : current->ViewedPieces(address, bytes)) {
        auto* view = current->View(piece.address, static_cast<std::size_t>(piece.size));
        if (view == nullptr) continue;
        const auto offset = static_cast<std::size_t>(piece.address - address);
        landDiffThrough(view, piece.address, before.subspan(offset, static_cast<std::size_t>(piece.size)), after.subspan(offset, static_cast<std::size_t>(piece.size)), serial, *current);
    }
}

void MemoryCore::landDiffThrough(std::byte* view, std::uint64_t address, std::span<const std::byte> before, std::span<const std::byte> after, std::uint64_t serial, MemoryHost& current) {
    const auto bytes = before.size();
    std::unique_lock lock(mutex);
    Downgrades::WatchTouch("a diff landing", address, bytes);
    std::uint64_t changed = 0;
    for (std::size_t at = 0; at < bytes;) {
        if (before[at] == after[at]) {
            ++at;
            continue;
        }
        auto stop = at + 1;
        while (stop < bytes && before[stop] != after[stop]) ++stop;
        std::memcpy(view + at, after.data() + at, stop - at);
        changed += stop - at;
        at = stop;
    }
    const auto last = landedDiffs.find(address);
    const std::byte* previous = last != landedDiffs.end() && last->second.size() == bytes ? last->second.data() : nullptr;
    std::uint64_t chained = 0;
    std::uint64_t foreign = 0;
    for (std::size_t at = 0; at < bytes; ++at) {
        if (view[at] == after[at]) continue;
        if (previous == nullptr || view[at] != previous[at] || before[at] == previous[at]) {
            ++foreign;
            continue;
        }
        ++chained;
        if (chainsLandings()) view[at] = after[at];
    }
    if (last != landedDiffs.end()) landedDiffBytes -= last->second.size();
    if (landedDiffBytes + bytes > LandedDiffBudgetBytes) {
        landedDiffs.clear();
        landedDiffBytes = 0;
    }
    landedDiffs[address].assign(after.begin(), after.end());
    landedDiffBytes += bytes;
    if ((chained != 0 || foreign != 0) && diffReports.fetch_add(1, std::memory_order_relaxed) < DiffReports) {
        std::fprintf(stderr, "[recode-gpu-owned] diff landing 0x%llx+0x%llx (batch %llu): %llu bytes behind an older landing%s, %llu bytes left unlike the mirror\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(serial), static_cast<unsigned long long>(chained), chainsLandings() ? " brought up" : " left", static_cast<unsigned long long>(foreign));
    }
    diffChainedBytes.fetch_add(chained, std::memory_order_relaxed);
    diffForeignBytes.fetch_add(foreign, std::memory_order_relaxed);
    if (chainsLandings()) changed += chained;
    written.Take(address, bytes, serial);
    diffLandedBytes.fetch_add(changed, std::memory_order_relaxed);
    diffUnchangedBytes.fetch_add(bytes - changed, std::memory_order_relaxed);
    if (changed != 0) Events::Shared().Note(address, bytes);
    const bool stale = std::memcmp(view, after.data(), bytes) != 0;
    lock.unlock();
    if (stale) current.Upload(address, bytes);
}

void MemoryCore::openWithPreImage(std::uint64_t page) {
    auto* current = host.load(std::memory_order_acquire);
    auto* view = current != nullptr ? current->View(page, PageBytes) : nullptr;
    if (view != nullptr) {
        auto& image = preImages[page];
        image.resize(PageBytes);
        std::memcpy(image.data(), view, PageBytes);
        preImageCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        written.Discard(page, PageBytes);
    }
    protector.Apply(pages.Reclaim(page, PageBytes, true));
    Events::Shared().Note(page, PageBytes);
}

std::uint64_t MemoryCore::landOpenPieces(std::uint64_t page, std::uint64_t completed, std::uint64_t begin, std::uint64_t end) {
    auto* current = host.load(std::memory_order_acquire);
    if (current == nullptr) return 0;
    begin = std::max(begin, page);
    end = std::min(end, page + PageBytes);
    if (begin >= end) return 0;
    const auto span = end - begin;
    {
        std::lock_guard lock(mutex);
        const auto found = preImages.find(page);
        if (found == preImages.end()) return 0;
        if (!written.Any(page, PageBytes)) {
            preImages.erase(found);
            return 0;
        }
        const auto pieces = written.In(begin, span);
        if (std::ranges::none_of(pieces, [&](const WrittenRange& piece) { return piece.serial <= completed; })) return 0;
    }
    std::vector<std::byte> mirror(static_cast<std::size_t>(span));
    if (!current->ReadMirror(begin, mirror)) return 0;
    std::unique_lock lock(mutex);
    const auto found = preImages.find(page);
    auto* view = current->View(begin, static_cast<std::size_t>(span));
    if (found == preImages.end() || view == nullptr) return 0;
    const auto pieces = written.In(begin, span);
    const auto& before = found->second;
    std::uint64_t changed = 0;
    for (const auto& piece : pieces) {
        if (piece.serial > completed) continue;
        for (auto at = piece.begin; at < piece.end; ++at) {
            const auto byte = mirror[at - begin];
            if (byte == before[at - page]) continue;
            view[at - begin] = byte;
            ++changed;
        }
    }
    if (changed != 0) Downgrades::WatchTouch("an opened page landing", begin, span);
    written.Take(begin, span, completed);
    if (!written.Any(page, PageBytes)) preImages.erase(found);
    diffLandedBytes.fetch_add(changed, std::memory_order_relaxed);
    if (span < PageBytes) {
        smallReads.fetch_add(1, std::memory_order_relaxed);
        smallReadBytes.fetch_add(span, std::memory_order_relaxed);
    }
    if (changed != 0) Events::Shared().Note(begin, span);
    lock.unlock();
    current->Upload(begin, span);
    return changed;
}

void MemoryCore::noteHot(std::uint64_t page) {
    for (auto& entry : hot) {
        if (entry.page != page) continue;
        entry.life = HotLifetime;
        return;
    }
    if (hot.size() < HotPages) {
        hot.push_back({page, HotLifetime});
        return;
    }
    *std::ranges::min_element(hot, {}, &Hot::life) = {page, HotLifetime};
}

bool MemoryCore::resolvePage(std::uint64_t page, bool write) {
    auto* current = host.load(std::memory_order_acquire);
    for (int attempt = 0; current != nullptr && attempt < ResolveAttempts; ++attempt) {
        std::uint64_t count = 0;
        std::uint64_t serial = 0;
        {
            std::lock_guard lock(mutex);
            if (pages.StateOf(page) != PageState::GpuOwned) return true;
            count = 1;
            while (count < ReadAheadPages && pages.StateOf(page + count * PageBytes) == PageState::GpuOwned) ++count;
            serial = written.Pending(page, PageBytes);
        }
        if (serial > current->Completed() && !current->WaitFor(serial)) break;
        std::vector<std::byte> mirror(static_cast<std::size_t>(count * PageBytes));
        if (!current->ReadMirror(page, mirror)) break;
        std::lock_guard lock(mutex);
        const auto completed = current->Completed();
        std::vector<ProtectionChange> changes;
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto at = page + index * PageBytes;
            if (pages.StateOf(at) != PageState::GpuOwned || written.Pending(at, PageBytes) > completed) continue;
            auto* view = current->View(at, PageBytes);
            if (view == nullptr) continue;
            Downgrades::WatchTouch("a GPU-owned page landing on a fault", at, PageBytes);
            std::memcpy(view, mirror.data() + index * PageBytes, PageBytes);
            written.Take(at, PageBytes, completed);
            const auto reclaimed = pages.Reclaim(at, PageBytes, index == 0 && write);
            changes.insert(changes.end(), reclaimed.begin(), reclaimed.end());
            ownedLandedBytes.fetch_add(PageBytes, std::memory_order_relaxed);
            if (index != 0) readAheadPages.fetch_add(1, std::memory_order_relaxed);
        }
        protector.Apply(changes);
        if (pages.StateOf(page) != PageState::GpuOwned) {
            noteHot(page);
            return true;
        }
    }
    std::lock_guard lock(mutex);
    if (pages.StateOf(page) == PageState::GpuOwned) {
        openWithPreImage(page);
        faultsOpenedStale.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool MemoryCore::resolveOwnedFault(std::uint64_t address, bool write) {
    const auto page = pageOf(address);
    auto* current = host.load(std::memory_order_acquire);
    if (current == nullptr) return false;
    if (current->ImagePage(page)) (write ? imageWriteFaults : imageFaults).fetch_add(1, std::memory_order_relaxed);
    if (current->Bypass() || std::this_thread::get_id() == resolverThread) {
        if (std::this_thread::get_id() != resolverThread && written.Pending(page, PageBytes) <= current->Completed()) {
            bypassLanded.fetch_add(1, std::memory_order_relaxed);
            return resolvePage(page, write);
        }
        std::lock_guard lock(mutex);
        if (pages.StateOf(page) == PageState::GpuOwned) openWithPreImage(page);
        faultsBypassed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    Parked request{page, write};
    std::unique_lock lock(parkMutex);
    if (!resolverStarted || resolverStop) {
        lock.unlock();
        return resolvePage(page, write);
    }
    parked.push_back(&request);
    faultsParked.fetch_add(1, std::memory_order_relaxed);
    parkSignal.notify_all();
    parkSignal.wait(lock, [&] { return request.done; });
    return true;
}

void MemoryCore::resolverLoop() {
    std::unique_lock lock(parkMutex);
    for (;;) {
        parkSignal.wait(lock, [&] { return resolverStop || !parked.empty(); });
        if (resolverStop) {
            for (auto* request : parked) request->done = true;
            parked.clear();
            parkSignal.notify_all();
            return;
        }
        auto* request = parked.front();
        parked.pop_front();
        lock.unlock();
        resolvePage(request->page, request->write);
        faultsResolved.fetch_add(1, std::memory_order_relaxed);
        lock.lock();
        request->done = true;
        parkSignal.notify_all();
    }
}

void MemoryCore::FlushGpuWrites(std::uint64_t address, std::uint64_t size) {
    if (size == 0 || !written.Any(address, size)) return;
    auto* current = host.load(std::memory_order_acquire);
    if (current == nullptr) return;
    flushes.fetch_add(1, std::memory_order_relaxed);
    const auto end = address + size;
    for (auto page = pageOf(address); page < end; page += PageBytes) {
        if (!written.Any(page, PageBytes)) continue;
        const auto completed = current->Completed();
        bool owned = false;
        {
            std::lock_guard lock(mutex);
            owned = pages.StateOf(page) == PageState::GpuOwned;
            if (owned && written.Pending(page, PageBytes) > completed) {
                flushPendingBytes.fetch_add(PageBytes, std::memory_order_relaxed);
                continue;
            }
        }
        if (!owned) {
            const bool small = size <= SmallReadBytes;
            const auto begin = small ? std::max(address, page) : page;
            const auto stop = small ? std::min(end, page + PageBytes) : page + PageBytes;
            if (landOpenPieces(page, completed, begin, stop) == 0 && written.Pending(begin, stop - begin) > completed) flushPendingBytes.fetch_add(stop - begin, std::memory_order_relaxed);
            continue;
        }
        std::vector<std::byte> mirror(PageBytes);
        if (!current->ReadMirror(page, mirror)) continue;
        std::lock_guard lock(mutex);
        if (pages.StateOf(page) != PageState::GpuOwned || written.Pending(page, PageBytes) > current->Completed()) continue;
        auto* view = current->View(page, PageBytes);
        if (view == nullptr) continue;
        Downgrades::WatchTouch("a GPU-owned page landing on a flush", page, PageBytes);
        std::memcpy(view, mirror.data(), PageBytes);
        written.Take(page, PageBytes, current->Completed());
        protector.Apply(pages.Reclaim(page, PageBytes, false));
        ownedLandedBytes.fetch_add(PageBytes, std::memory_order_relaxed);
    }
}

void MemoryCore::LandAhead() {
    std::vector<std::uint64_t> due;
    {
        std::lock_guard lock(mutex);
        std::erase_if(hot, [](const Hot& entry) { return entry.life == 0; });
        for (auto& entry : hot) {
            --entry.life;
            if (pages.StateOf(entry.page) == PageState::GpuOwned) due.push_back(entry.page);
        }
    }
    for (const auto page : due) {
        const auto before = ownedLandedBytes.load(std::memory_order_relaxed);
        FlushGpuWrites(page, PageBytes);
        if (ownedLandedBytes.load(std::memory_order_relaxed) != before) hotLandedPages.fetch_add(1, std::memory_order_relaxed);
    }
}

void MemoryCore::LandOpened() {
    auto* current = host.load(std::memory_order_acquire);
    if (current == nullptr) return;
    std::vector<std::uint64_t> opened;
    {
        std::lock_guard lock(mutex);
        opened.reserve(preImages.size());
        for (const auto& [page, image] : preImages) opened.push_back(page);
    }
    const auto completed = current->Completed();
    for (const auto page : opened) landOpenPieces(page, completed, page, page + PageBytes);
}

std::vector<DirtyRange> MemoryCore::WithoutGpuWritten(std::span<const DirtyRange> ranges) const {
    std::vector<DirtyRange> kept;
    for (const auto& range : ranges) {
        auto cursor = range.address;
        const auto end = range.address + range.size;
        for (const auto& piece : written.In(range.address, range.size)) {
            if (piece.begin > cursor) kept.push_back({cursor, piece.begin - cursor});
            cursor = std::max(cursor, piece.end);
        }
        if (cursor < end) kept.push_back({cursor, end - cursor});
    }
    return kept;
}

}
