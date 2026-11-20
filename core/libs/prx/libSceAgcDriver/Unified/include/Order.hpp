#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_ORDER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_ORDER_HPP

#include "prx/libSceAgcDriver/Unified/include/Operation.hpp"
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace AgcDriver::Unified {

struct OrderCounts {
    std::uint64_t edges = 0;
    std::uint64_t crossEdges = 0;
    std::uint64_t crossPending = 0;
    std::uint64_t alwaysPass = 0;
    std::uint64_t pending = 0;
    std::uint64_t released = 0;
    std::uint64_t overwritten = 0;
    std::uint64_t superseded = 0;
};

class Order {
public:
    enum class Verdict : std::uint8_t { Passes, Edge, Pending };

    Verdict Resolve(Operation& wait);
    std::vector<Operation> Note(const Operation& operation);
    void Settled(const Operation& wait);
    [[nodiscard]] std::size_t PendingWaits() const;
    [[nodiscard]] OrderCounts Counts() const;

private:
    struct Store {
        std::uint32_t queue = 0;
        std::uint64_t sequence = 0;
        std::uint64_t value = 0;
        std::uint64_t size = 0;
    };

    void forget(std::uint64_t address, std::uint64_t size);
    [[nodiscard]] bool satisfies(const Store& store, const Operation& wait) const;

    mutable std::mutex mutex;
    std::map<std::uint64_t, Store> stores;
    std::vector<Operation> pending;
    OrderCounts counts;
};

}

#endif
