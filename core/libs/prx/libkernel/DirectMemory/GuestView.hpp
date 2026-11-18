#ifndef CORE_LIBS_PRX_LIBKERNEL_DIRECTMEMORY_GUESTVIEW_HPP
#define CORE_LIBS_PRX_LIBKERNEL_DIRECTMEMORY_GUESTVIEW_HPP

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace DirectMemoryView {

class GuestView {
public:
    static constexpr std::size_t DefaultChunkBytes = std::size_t{1} << 30;
    static constexpr std::size_t PageBytes = 0x4000;

    GuestView() = default;
    GuestView(const GuestView&) = delete;
    GuestView& operator=(const GuestView&) = delete;
    ~GuestView();

    void Reserve(std::uintptr_t guestBase, std::size_t guestBytes, std::size_t directBytes, std::size_t chunkBytes = DefaultChunkBytes);
    bool Reserved() const { return view != nullptr; }
    bool Covers(std::uintptr_t guest, std::size_t bytes) const;
    std::uint8_t* Address(std::uintptr_t guest) const { return view + (guest - guestBase); }
    std::uint8_t* DirectAddress(std::uint64_t physical) const { return view + guestBytes + physical; }
    HANDLE Chunk(std::uintptr_t guest, std::uint64_t* offset);
    void MarkUsed(std::uintptr_t guest, std::size_t bytes);
    void Discard(std::uintptr_t guest, std::size_t bytes);
    void MapDirect(HANDLE section, std::uint64_t physical, std::size_t bytes);
    void UnmapDirect(HANDLE section, std::uint64_t physical, std::size_t bytes);
    std::size_t DirectPieces() const;

private:
    struct DirectPiece {
        std::uintptr_t end;
        HANDLE section;
        std::uint64_t offset;
    };

    void coalesceDirect(std::uintptr_t start, std::uintptr_t end);
    void placeDirect(std::uintptr_t start, std::uintptr_t end, HANDLE section, std::uint64_t offset);

    std::uintptr_t guestBase = 0;
    std::size_t guestBytes = 0;
    std::size_t viewBytes = 0;
    std::size_t chunkBytes = DefaultChunkBytes;
    std::uint8_t* view = nullptr;
    std::vector<HANDLE> chunks;
    std::vector<std::uint64_t> used;
    std::map<std::uintptr_t, DirectPiece> directPieces;
    mutable std::mutex lock;
};

}
#endif

#endif
