#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_LABELTABLE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_LABELTABLE_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>

namespace AgcDriver::Unified::Labels {

inline constexpr std::size_t History = 4;

struct Entry {
    std::uint64_t stamp = 0;
    std::uint32_t value = 0;
    std::uint32_t queue = ~0u;
};

struct Issued {
    std::uint64_t stamp = 0;
    std::uint32_t queue = 0;
    std::uint64_t value = 0;
};

class LabelTable {
public:
    std::uint64_t Note(std::uint64_t address, std::span<const std::byte> bytes, std::uint32_t queue);
    [[nodiscard]] std::optional<Issued> IssuedAfter(std::uint64_t address, std::size_t bytes, std::uint64_t after, const std::function<bool(std::uint64_t value)>& compares) const;
    [[nodiscard]] std::uint64_t Serial() const { return serial.load(std::memory_order_acquire); }
    void Clear();

private:
    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t, std::array<Entry, History>> words;
    std::atomic<std::uint64_t> serial{0};
};

LabelTable& Table();

}

#endif
