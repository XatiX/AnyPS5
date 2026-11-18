#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SPSCRING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SPSCRING_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace AgcDriver::Unified {

template <typename TValue, std::size_t TCapacity>
class SpscRing {
    static_assert(TCapacity >= 2 && (TCapacity & (TCapacity - 1)) == 0, "capacity must be a power of two");

public:
    [[nodiscard]] bool TryPush(TValue value) {
        const std::uint64_t tail = writeIndex.load(std::memory_order_relaxed);
        if (tail - cachedRead >= TCapacity) {
            cachedRead = readIndex.load(std::memory_order_acquire);
            if (tail - cachedRead >= TCapacity) return false;
        }
        (*items)[static_cast<std::size_t>(tail & (TCapacity - 1))] = std::move(value);
        writeIndex.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<TValue> TryPop() {
        const std::uint64_t head = readIndex.load(std::memory_order_relaxed);
        if (head == cachedWrite) {
            cachedWrite = writeIndex.load(std::memory_order_acquire);
            if (head == cachedWrite) return std::nullopt;
        }
        TValue value = std::move((*items)[static_cast<std::size_t>(head & (TCapacity - 1))]);
        readIndex.store(head + 1, std::memory_order_release);
        return value;
    }

    [[nodiscard]] std::size_t Size() const {
        return static_cast<std::size_t>(writeIndex.load(std::memory_order_acquire) - readIndex.load(std::memory_order_acquire));
    }

private:
    std::unique_ptr<std::array<TValue, TCapacity>> items = std::make_unique<std::array<TValue, TCapacity>>();
    alignas(64) std::atomic<std::uint64_t> writeIndex{0};
    std::uint64_t cachedRead = 0;
    alignas(64) std::atomic<std::uint64_t> readIndex{0};
    std::uint64_t cachedWrite = 0;
};

}

#endif
