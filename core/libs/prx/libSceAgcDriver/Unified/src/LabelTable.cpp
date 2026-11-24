#include "prx/libSceAgcDriver/Unified/include/LabelTable.hpp"
#include <algorithm>
#include <cstring>

namespace AgcDriver::Unified::Labels {

std::uint64_t LabelTable::Note(std::uint64_t address, std::span<const std::byte> bytes, std::uint32_t queue) {
    std::lock_guard lock(mutex);
    const auto stamp = serial.fetch_add(1, std::memory_order_acq_rel) + 1u;
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= bytes.size(); offset += sizeof(std::uint32_t)) {
        auto& history = words[address + offset];
        std::shift_right(history.begin(), history.end(), 1);
        history[0].stamp = stamp;
        history[0].queue = queue;
        std::memcpy(&history[0].value, bytes.data() + offset, sizeof(std::uint32_t));
    }
    return stamp;
}

std::optional<Issued> LabelTable::IssuedAfter(std::uint64_t address, std::size_t bytes, std::uint64_t after, const std::function<bool(std::uint64_t value)>& compares) const {
    std::lock_guard lock(mutex);
    const auto low = words.find(address);
    if (low == words.end()) return std::nullopt;
    const auto high = bytes == 8u ? words.find(address + 4u) : words.end();
    if (bytes == 8u && high == words.end()) return std::nullopt;
    for (const auto& entry : low->second) {
        if (entry.stamp == 0u || entry.stamp <= after) continue;
        std::uint64_t value = entry.value;
        if (bytes == 8u) {
            const auto upper = std::find_if(high->second.begin(), high->second.end(), [&](const Entry& other) { return other.stamp == entry.stamp; });
            if (upper == high->second.end()) continue;
            value |= static_cast<std::uint64_t>(upper->value) << 32u;
        }
        if (compares(value)) return Issued{entry.stamp, entry.queue, value};
    }
    return std::nullopt;
}

void LabelTable::Clear() {
    std::lock_guard lock(mutex);
    words.clear();
}

LabelTable& Table() {
    static LabelTable table;
    return table;
}

}
