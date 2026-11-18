#include "prx/libkernel/include/Api.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include "SceTypes.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#ifdef _WIN32
#include <windows.h>
#endif

static void Require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "Guest view check failed at %s:%u\n", location.file_name(), static_cast<unsigned>(location.line()));
        std::abort();
    }
}

int main() {
#ifdef _WIN32
    constexpr int readWrite = 0x03;
    constexpr std::size_t bytes = 4 * PS5_PAGE_SIZE;

    void* anon = nullptr;
    Require(DoMapAnon(&anon, bytes, readWrite, 0) == 0 && anon != nullptr);
    auto* guest = static_cast<volatile std::uint8_t*>(anon);
    auto* view = GuestViewAddress_nid_no_patch(reinterpret_cast<std::uintptr_t>(anon), bytes);
    Require(view != nullptr);
    guest[PS5_PAGE_SIZE + 3] = 0x5a;
    Require(view[PS5_PAGE_SIZE + 3] == 0x5a);
    DWORD old = 0;
    Require(VirtualProtect(anon, bytes, PAGE_NOACCESS, &old) != 0);
    view[2 * PS5_PAGE_SIZE] = 0x44;
    Require(view[PS5_PAGE_SIZE + 3] == 0x5a);
    Require(VirtualProtect(anon, bytes, PAGE_READWRITE, &old) != 0);
    Require(guest[2 * PS5_PAGE_SIZE] == 0x44);
    Require(DoMunmap(anon, bytes) == 0);
    Require(view[PS5_PAGE_SIZE + 3] == 0 && view[2 * PS5_PAGE_SIZE] == 0);

    int64_t phys = 0;
    Require(sceKernelAllocateDirectMemory(0, static_cast<int64_t>(sceKernelGetDirectMemorySize()), bytes, 0, 0, &phys) == 0);
    void* direct = nullptr;
    Require(DoMapDirect(&direct, bytes, readWrite, 0, phys, PS5_PAGE_SIZE) == 0 && direct != nullptr);
    auto* directGuest = static_cast<volatile std::uint8_t*>(direct);
    auto* physical = GuestViewDirectAddress_nid_no_patch(static_cast<std::uint64_t>(phys));
    Require(physical != nullptr);
    directGuest[7] = 0x21;
    Require(physical[7] == 0x21);
    Require(VirtualProtect(direct, bytes, PAGE_READONLY, &old) != 0);
    physical[9] = 0x31;
    Require(directGuest[9] == 0x31);
    Require(VirtualProtect(direct, bytes, PAGE_READWRITE, &old) != 0);
    Require(DoMunmap(direct, bytes) == 0);
    Require(sceKernelReleaseDirectMemory(phys, bytes) == 0);
#endif
    std::printf("guest view kernel tests passed\n");
    return 0;
}
