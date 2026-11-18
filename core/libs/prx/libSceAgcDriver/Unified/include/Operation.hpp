#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_OPERATION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_OPERATION_HPP

#include <cstdint>

namespace AgcDriver::Unified {

enum class OpKind : std::uint8_t {
    Draw,
    DrawIndirect,
    Dispatch,
    DispatchIndirect,
    LabelStore,
    Wait,
    Copy,
    Fill,
    Flip,
    Marker,
};

struct SnapshotIds {
    std::uint64_t context = 0;
    std::uint64_t userConfig = 0;
    std::uint64_t shader = 0;
    std::uint64_t scalars = 0;

    bool operator==(const SnapshotIds&) const = default;
};

struct WaitEdge {
    std::uint32_t queue = 0;
    std::uint64_t sequence = 0;

    [[nodiscard]] bool Present() const { return sequence != 0; }
};

struct Operation {
    OpKind kind = OpKind::Marker;
    std::uint8_t opcode = 0;
    bool predicated = false;
    std::uint32_t queue = 0;
    std::uint64_t sequence = 0;
    std::uint64_t submission = 0;
    std::uint32_t packetOffset = 0;
    SnapshotIds snapshots;
    WaitEdge after;
    std::uint64_t address = 0;
    std::uint64_t size = 0;
    std::uint64_t value = 0;
    std::uint64_t mask = ~std::uint64_t{0};
    std::uint8_t function = 0;
};

[[nodiscard]] constexpr bool IsDraw(OpKind kind) {
    return kind == OpKind::Draw || kind == OpKind::DrawIndirect;
}

[[nodiscard]] constexpr bool IsDispatch(OpKind kind) {
    return kind == OpKind::Dispatch || kind == OpKind::DispatchIndirect;
}

}

#endif
