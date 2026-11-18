#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SNAPSHOTRING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SNAPSHOTRING_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace AgcDriver::Unified {

template <typename TValue, std::size_t TSlots>
class SnapshotRing {
    static_assert(TSlots >= 2 && (TSlots & (TSlots - 1)) == 0, "slot count must be a power of two");

public:
    struct View {
        const TValue* value = nullptr;
        std::uint64_t id = 0;
    };

    [[nodiscard]] bool TryTake(const TValue& live, bool& dirty, View& view) {
        if (!dirty && current != 0) {
            view = {&slots->at(slotOf(current)).value, current};
            return true;
        }
        const std::uint64_t next = current + 1;
        if (next > TSlots && next - TSlots > retired.load(std::memory_order_acquire)) {
            return false;
        }
        Slot& slot = slots->at(slotOf(next));
        slot.value = live;
        slot.id = next;
        current = next;
        dirty = false;
        view = {&slot.value, next};
        return true;
    }

    void Retire(std::uint64_t id) {
        std::uint64_t seen = retired.load(std::memory_order_relaxed);
        while (id > seen && !retired.compare_exchange_weak(seen, id, std::memory_order_release, std::memory_order_relaxed)) {
        }
    }

    [[nodiscard]] const TValue* Find(std::uint64_t id) const {
        if (id == 0 || id > current || current - id >= TSlots) return nullptr;
        const Slot& slot = slots->at(slotOf(id));
        return slot.id == id ? &slot.value : nullptr;
    }

    [[nodiscard]] std::uint64_t Current() const { return current; }

private:
    struct Slot {
        TValue value{};
        std::uint64_t id = 0;
    };

    [[nodiscard]] static std::size_t slotOf(std::uint64_t id) { return static_cast<std::size_t>(id & (TSlots - 1)); }

    std::unique_ptr<std::array<Slot, TSlots>> slots = std::make_unique<std::array<Slot, TSlots>>();
    std::uint64_t current = 0;
    alignas(64) std::atomic<std::uint64_t> retired{0};
};

}

#endif
