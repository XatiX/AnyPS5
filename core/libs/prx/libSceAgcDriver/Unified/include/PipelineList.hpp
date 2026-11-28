#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PIPELINELIST_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PIPELINELIST_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver::Unified::PipelineList {

inline constexpr std::size_t ReplayLimit = 2048;

struct Entry {
    std::vector<std::uint64_t> key;
    std::vector<std::byte> result;

    bool operator==(const Entry&) const = default;
};

[[nodiscard]] std::filesystem::path Directory();
[[nodiscard]] std::string Name(std::span<const std::uint64_t> key);
[[nodiscard]] bool Listed(const std::filesystem::path& directory, std::span<const std::uint64_t> key);
bool Store(const std::filesystem::path& directory, const Entry& entry);
[[nodiscard]] std::optional<Entry> Load(const std::filesystem::path& file);
[[nodiscard]] std::vector<std::filesystem::path> Newest(const std::filesystem::path& directory, std::size_t limit = ReplayLimit);

}

#endif
