#include "prx/libkernel/DirectMemory/GuestView.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

#ifdef _WIN32
using AllocateFunction = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using MapFunction = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
using UnmapFunction = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

constexpr std::size_t ChunkBytes = 16u << 20;
constexpr std::size_t GuestBytes = 2 * ChunkBytes;
constexpr std::size_t DirectBytes = 8u << 20;
constexpr std::size_t Page = DirectMemoryView::GuestView::PageBytes;

const auto kernelBase = GetModuleHandleW(L"kernelbase.dll");
const auto allocate = reinterpret_cast<AllocateFunction>(GetProcAddress(kernelBase, "VirtualAlloc2"));
const auto mapView = reinterpret_cast<MapFunction>(GetProcAddress(kernelBase, "MapViewOfFile3"));
const auto unmapView = reinterpret_cast<UnmapFunction>(GetProcAddress(kernelBase, "UnmapViewOfFile2"));

std::uint8_t* mapGuest(DirectMemoryView::GuestView& view, std::uint8_t* guest, std::size_t at, std::size_t bytes) {
    std::uint64_t offset = 0;
    const auto section = view.Chunk(reinterpret_cast<std::uintptr_t>(guest + at), &offset);
    Expect(offset == at % ChunkBytes, "the chunk offset is the guest offset within its chunk");
    VirtualFree(guest + at, bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    auto* mapped = static_cast<std::uint8_t*>(mapView(section, GetCurrentProcess(), guest + at, offset, bytes, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0));
    Expect(mapped == guest + at, "the chunk section maps at the guest address");
    if (mapped != nullptr) Expect(VirtualAlloc(mapped, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr, "the guest pages commit");
    view.MarkUsed(reinterpret_cast<std::uintptr_t>(guest + at), bytes);
    return mapped;
}

void testGuestPagesReachableThroughTheView() {
    auto* guest = static_cast<std::uint8_t*>(allocate(GetCurrentProcess(), nullptr, GuestBytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    Expect(guest != nullptr, "a guest placeholder is reserved");
    if (guest == nullptr) return;
    {
        DirectMemoryView::GuestView view;
        view.Reserve(reinterpret_cast<std::uintptr_t>(guest), GuestBytes, DirectBytes, ChunkBytes);
        Expect(view.Reserved() && view.Covers(reinterpret_cast<std::uintptr_t>(guest), GuestBytes), "the view covers the guest range");
        const std::size_t at = ChunkBytes + 4 * Page;
        auto* mapped = mapGuest(view, guest, at, 4 * Page);
        if (mapped != nullptr) {
            const auto address = reinterpret_cast<std::uintptr_t>(guest + at);
            mapped[16] = 0x5a;
            Expect(view.Address(address)[16] == 0x5a, "a guest write shows through the view");
            DWORD old = 0;
            Expect(VirtualProtect(mapped, 4 * Page, PAGE_NOACCESS, &old) != 0, "the guest pages are protected");
            view.Address(address)[32] = 0x33;
            Expect(view.Address(address)[16] == 0x5a && view.Address(address)[32] == 0x33, "the view reads and writes protected guest pages");
            VirtualProtect(mapped, 4 * Page, PAGE_READWRITE, &old);
            Expect(mapped[32] == 0x33, "a view write shows at the guest address");
            view.Discard(address, 4 * Page);
            Expect(mapped[16] == 0 && mapped[32] == 0, "discarding zeroes the used pages");
            unmapView(GetCurrentProcess(), mapped, MEM_PRESERVE_PLACEHOLDER);
        }
        auto* first = mapGuest(view, guest, 0, Page);
        if (first != nullptr) {
            first[0] = 7;
            Expect(view.Address(reinterpret_cast<std::uintptr_t>(guest))[0] == 7, "the first chunk maps into the view too");
            unmapView(GetCurrentProcess(), first, MEM_PRESERVE_PLACEHOLDER);
        }
    }
    VirtualFree(guest, 0, MEM_RELEASE);
}

void testDirectBackingsReplaceEachOther() {
    DirectMemoryView::GuestView view;
    view.Reserve(0x10000000, GuestBytes, DirectBytes, ChunkBytes);
    const auto make = [](std::size_t bytes) { return CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(bytes), nullptr); };
    const auto wide = make(4u << 20);
    const auto narrow = make(1u << 20);
    view.MapDirect(wide, 0, 4u << 20);
    auto* direct = view.DirectAddress(0);
    direct[(2u << 20) + 5] = 0x11;
    auto* sectionView = static_cast<std::uint8_t*>(MapViewOfFile(wide, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    Expect(sectionView != nullptr && sectionView[(2u << 20) + 5] == 0x11, "a view write lands in the backing section");
    view.MapDirect(narrow, 1u << 20, 1u << 20);
    Expect(view.DirectPieces() == 3, "a backing mapped inside another splits it in three");
    Expect(direct[(2u << 20) + 5] == 0x11, "the part of the older backing after the new one keeps its bytes");
    direct[(1u << 20) + 9] = 0x22;
    Expect(sectionView[(1u << 20) + 9] == 0, "a write over the new backing does not reach the older one");
    view.UnmapDirect(narrow, 1u << 20, 1u << 20);
    Expect(view.DirectPieces() == 2, "unmapping the new backing leaves the two older pieces");
    view.MapDirect(narrow, 1u << 20, 1u << 20);
    Expect(view.DirectPieces() == 3, "the freed range takes a backing again");
    if (sectionView != nullptr) UnmapViewOfFile(sectionView);
    view.UnmapDirect(narrow, 1u << 20, 1u << 20);
    view.UnmapDirect(wide, 0, 4u << 20);
    Expect(view.DirectPieces() == 0, "unmapping every backing empties the direct view");
    CloseHandle(narrow);
    CloseHandle(wide);
}
#endif

}

int main() {
#ifdef _WIN32
    testGuestPagesReachableThroughTheView();
    testDirectBackingsReplaceEachOther();
#endif
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("guest view tests passed\n");
    return 0;
}
