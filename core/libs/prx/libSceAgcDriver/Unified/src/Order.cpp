#include "prx/libSceAgcDriver/Unified/include/Order.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <algorithm>

namespace AgcDriver::Unified {

bool Order::satisfies(const Store& store, const Operation& wait) const {
    return Pm4::WaitCompares(wait.function, wait.value, wait.mask, store.value);
}

Order::Verdict Order::Resolve(Operation& wait) {
    std::lock_guard lock(mutex);
    if (wait.function == 0) {
        ++counts.alwaysPass;
        return Verdict::Passes;
    }
    const auto found = stores.find(wait.address);
    if (found != stores.end() && satisfies(found->second, wait)) {
        wait.after = {found->second.queue, found->second.sequence};
        ++counts.edges;
        return Verdict::Edge;
    }
    counts.superseded += std::erase_if(pending, [&](const Operation& held) { return held.queue == wait.queue; });
    pending.push_back(wait);
    ++counts.pending;
    return Verdict::Pending;
}

void Order::forget(std::uint64_t address, std::uint64_t size) {
    auto first = stores.lower_bound(address >= 7 ? address - 7 : 0);
    while (first != stores.end() && first->first < address + size) {
        if (first->first + std::max<std::uint64_t>(first->second.size, 1) > address) {
            first = stores.erase(first);
            ++counts.overwritten;
        } else {
            ++first;
        }
    }
}

std::vector<Operation> Order::Note(const Operation& operation) {
    std::vector<Operation> released;
    if (operation.kind != OpKind::LabelStore && operation.kind != OpKind::Copy && operation.kind != OpKind::Fill) return released;
    std::lock_guard lock(mutex);
    if (operation.size != 0) forget(operation.address, operation.size);
    if (operation.kind != OpKind::LabelStore || operation.size > 8) return released;
    const Store store{operation.queue, operation.sequence, operation.value, operation.size};
    stores[operation.address] = store;
    for (auto it = pending.begin(); it != pending.end();) {
        if (it->address == operation.address && it->queue != operation.queue && satisfies(store, *it)) {
            it->after = {store.queue, store.sequence};
            released.push_back(*it);
            it = pending.erase(it);
            ++counts.released;
        } else {
            ++it;
        }
    }
    return released;
}

void Order::Settled(const Operation& wait) {
    std::lock_guard lock(mutex);
    std::erase_if(pending, [&](const Operation& held) { return held.queue == wait.queue && held.sequence == wait.sequence; });
}

std::size_t Order::PendingWaits() const {
    std::lock_guard lock(mutex);
    return pending.size();
}

OrderCounts Order::Counts() const {
    std::lock_guard lock(mutex);
    return counts;
}

}
