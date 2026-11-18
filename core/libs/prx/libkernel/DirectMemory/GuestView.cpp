#ifdef _WIN32
#include "prx/libkernel/DirectMemory/GuestView.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace GuestMemoryView {

namespace {

using AllocateFunction = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using MapFunction = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
using UnmapFunction = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

HMODULE kernelBase() {
    static const auto module = GetModuleHandleW(L"kernelbase.dll");
    return module;
}

AllocateFunction allocateFunction() {
    static const auto function = reinterpret_cast<AllocateFunction>(GetProcAddress(kernelBase(), "VirtualAlloc2"));
    return function;
}

MapFunction mapFunction() {
    static const auto function = reinterpret_cast<MapFunction>(GetProcAddress(kernelBase(), "MapViewOfFile3"));
    return function;
}

UnmapFunction unmapFunction() {
    static const auto function = reinterpret_cast<UnmapFunction>(GetProcAddress(kernelBase(), "UnmapViewOfFile2"));
    return function;
}

[[noreturn]] void fail(const char* what) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), what);
}

}

GuestView::~GuestView() {
    if (view == nullptr) return;
    const auto unmap = unmapFunction();
    for (const auto& [start, piece] : directPieces) unmap(GetCurrentProcess(), reinterpret_cast<void*>(start), 0);
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        if (chunks[index] == nullptr) continue;
        unmap(GetCurrentProcess(), view + index * chunkBytes, 0);
        CloseHandle(chunks[index]);
    }
    VirtualFree(view, 0, MEM_RELEASE);
}

void GuestView::Reserve(std::uintptr_t base, std::size_t bytes, std::size_t directBytes, std::size_t chunk) {
    std::lock_guard guard(lock);
    if (view != nullptr) return;
    if (allocateFunction() == nullptr || mapFunction() == nullptr || unmapFunction() == nullptr) throw std::runtime_error("VirtualAlloc2, MapViewOfFile3 and UnmapViewOfFile2 are needed for the guest memory view");
    if (chunk == 0 || chunk % PageBytes != 0) throw std::invalid_argument("the guest view chunk is not a whole number of pages");
    const auto guestEnd = (bytes + PageBytes - 1) & ~(PageBytes - 1);
    const auto total = guestEnd + directBytes;
    auto* reserved = static_cast<std::uint8_t*>(allocateFunction()(GetCurrentProcess(), nullptr, total, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    if (reserved == nullptr) fail("reserve the guest memory view");
    if (directBytes != 0 && !VirtualFree(reserved, guestEnd, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        const auto error = static_cast<int>(GetLastError());
        VirtualFree(reserved, 0, MEM_RELEASE);
        throw std::system_error(error, std::system_category(), "split the guest memory view");
    }
    guestBase = base;
    guestBytes = guestEnd;
    viewBytes = total;
    chunkBytes = chunk;
    chunks.assign((guestEnd + chunk - 1) / chunk, nullptr);
    used.assign((guestEnd / PageBytes + 63) / 64, 0);
    view = reserved;
}

bool GuestView::Covers(std::uintptr_t guest, std::size_t bytes) const {
    return view != nullptr && guest >= guestBase && guest - guestBase <= guestBytes && bytes <= guestBytes - (guest - guestBase);
}

HANDLE GuestView::Chunk(std::uintptr_t guest, std::uint64_t* offset) {
    std::lock_guard guard(lock);
    const auto relative = guest - guestBase;
    const auto index = relative / chunkBytes;
    *offset = relative % chunkBytes;
    if (chunks[index] != nullptr) return chunks[index];
    const auto chunkStart = index * chunkBytes;
    const auto bytes = std::min<std::uint64_t>(chunkBytes, guestBytes - chunkStart);
    const HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
    if (section == nullptr) fail("create a guest memory section");
    auto* target = view + chunkStart;
    MEMORY_BASIC_INFORMATION memory{};
    const bool queried = VirtualQuery(target, &memory, sizeof(memory)) == sizeof(memory);
    const bool whole = queried && reinterpret_cast<std::uint8_t*>(memory.AllocationBase) == target && memory.RegionSize == bytes;
    if (queried && memory.State == MEM_RESERVE && !whole && !VirtualFree(target, bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        const auto error = static_cast<int>(GetLastError());
        CloseHandle(section);
        throw std::system_error(error, std::system_category(), "split the guest memory view for a chunk");
    }
    if (!queried || memory.State != MEM_RESERVE || mapFunction()(section, GetCurrentProcess(), target, 0, bytes, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) == nullptr) {
        const auto error = static_cast<int>(GetLastError());
        CloseHandle(section);
        throw std::system_error(error, std::system_category(), "map a guest memory section into the view");
    }
    chunks[index] = section;
    return section;
}

void GuestView::MarkUsed(std::uintptr_t guest, std::size_t bytes) {
    std::lock_guard guard(lock);
    const auto first = (guest - guestBase) / PageBytes;
    const auto last = (guest - guestBase + bytes + PageBytes - 1) / PageBytes;
    for (auto page = first; page < last; ++page) used[page / 64] |= std::uint64_t{1} << (page % 64);
}

void GuestView::Discard(std::uintptr_t guest, std::size_t bytes) {
    if (bytes == 0 || !Covers(guest, bytes)) return;
    std::lock_guard guard(lock);
    const auto first = (guest - guestBase) / PageBytes;
    const auto last = (guest - guestBase + bytes + PageBytes - 1) / PageBytes;
    for (auto page = first; page < last; ++page) {
        auto& word = used[page / 64];
        const auto bit = std::uint64_t{1} << (page % 64);
        if ((word & bit) == 0) continue;
        std::memset(view + page * PageBytes, 0, PageBytes);
        word &= ~bit;
    }
}

void GuestView::coalesceDirect(std::uintptr_t start, std::uintptr_t end) {
    const auto regionStart = reinterpret_cast<std::uintptr_t>(DirectAddress(0));
    const auto after = directPieces.lower_bound(end);
    const auto before = directPieces.lower_bound(start);
    const auto freeStart = before == directPieces.begin() ? regionStart : std::prev(before)->second.end;
    const auto freeEnd = after == directPieces.end() ? regionStart + (viewBytes - guestBytes) : after->first;
    VirtualFree(reinterpret_cast<void*>(freeStart), freeEnd - freeStart, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
}

void GuestView::placeDirect(std::uintptr_t start, std::uintptr_t end, HANDLE section, std::uint64_t offset) {
    auto* address = reinterpret_cast<void*>(start);
    const auto bytes = static_cast<std::size_t>(end - start);
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory)) fail("query the direct memory view");
    if (memory.State != MEM_RESERVE || reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize < end) throw std::runtime_error("the direct memory view has no free range for a backing");
    if ((reinterpret_cast<std::uintptr_t>(memory.AllocationBase) != start || memory.RegionSize != bytes) && !VirtualFree(address, bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split the direct memory view for a backing");
    if (mapFunction()(section, GetCurrentProcess(), address, offset, bytes, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) == nullptr) {
        const auto error = static_cast<int>(GetLastError());
        char message[160];
        std::snprintf(message, sizeof(message), "map a direct memory backing into the view at physical 0x%llx+0x%zx, section offset 0x%llx", static_cast<unsigned long long>(start - reinterpret_cast<std::uintptr_t>(DirectAddress(0))), bytes, static_cast<unsigned long long>(offset));
        throw std::system_error(error, std::system_category(), message);
    }
    directPieces.emplace(start, DirectPiece{end, section, offset});
}

void GuestView::MapDirect(HANDLE section, std::uint64_t physical, std::size_t bytes) {
    if (view == nullptr || bytes == 0) return;
    const auto start = reinterpret_cast<std::uintptr_t>(DirectAddress(physical));
    const auto end = start + bytes;
    std::lock_guard guard(lock);
    struct Kept {
        std::uintptr_t start;
        std::uintptr_t end;
        HANDLE section;
        std::uint64_t offset;
    };
    std::vector<Kept> kept;
    auto spanStart = start;
    auto spanEnd = end;
    bool replaced = false;
    for (auto older = directPieces.begin(); older != directPieces.end();) {
        if (older->second.end <= start || older->first >= end) {
            ++older;
            continue;
        }
        if (!unmapFunction()(GetCurrentProcess(), reinterpret_cast<void*>(older->first), MEM_PRESERVE_PLACEHOLDER)) fail("replace a direct memory backing in the view");
        if (older->first < start) kept.push_back({older->first, start, older->second.section, older->second.offset});
        if (older->second.end > end) kept.push_back({end, older->second.end, older->second.section, older->second.offset + (end - older->first)});
        spanStart = std::min(spanStart, older->first);
        spanEnd = std::max(spanEnd, older->second.end);
        older = directPieces.erase(older);
        replaced = true;
    }
    if (replaced) coalesceDirect(spanStart, spanEnd);
    for (const auto& piece : kept) placeDirect(piece.start, piece.end, piece.section, piece.offset);
    placeDirect(start, end, section, 0);
}

void GuestView::UnmapDirect(HANDLE section, std::uint64_t physical, std::size_t bytes) {
    if (view == nullptr || bytes == 0) return;
    const auto start = reinterpret_cast<std::uintptr_t>(DirectAddress(physical));
    const auto end = start + bytes;
    std::lock_guard guard(lock);
    for (auto piece = directPieces.lower_bound(start); piece != directPieces.end() && piece->first < end;) {
        if (piece->second.section != section || !unmapFunction()(GetCurrentProcess(), reinterpret_cast<void*>(piece->first), MEM_PRESERVE_PLACEHOLDER)) {
            ++piece;
            continue;
        }
        const auto pieceStart = piece->first;
        const auto pieceEnd = piece->second.end;
        piece = directPieces.erase(piece);
        coalesceDirect(pieceStart, pieceEnd);
    }
}

std::size_t GuestView::DirectPieces() const {
    std::lock_guard guard(lock);
    return directPieces.size();
}

}
#endif
