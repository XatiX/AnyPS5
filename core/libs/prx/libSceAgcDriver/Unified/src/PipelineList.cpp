#include "prx/libSceAgcDriver/Unified/include/PipelineList.hpp"
#include "ShaderCacheDirectory.hpp"
#include "ShaderDiskCache.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace AgcDriver::Unified::PipelineList {

namespace {

constexpr std::uint64_t Magic = 0x3154534c50435241ull;
constexpr std::string_view Extension = ".pipeline";

struct Header {
    std::uint64_t magic = Magic;
    std::uint64_t keyWords = 0;
    std::uint64_t resultBytes = 0;
    std::uint64_t hash = 0;
};

std::uint64_t mix(std::uint64_t hash, const void* data, std::size_t bytes) {
    const auto* cursor = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < bytes; ++index) hash = (hash ^ cursor[index]) * 0x100000001b3ull;
    return hash;
}

std::uint64_t hashOf(std::span<const std::uint64_t> key, std::span<const std::byte> result) {
    auto hash = mix(0xcbf29ce484222325ull, key.data(), key.size_bytes());
    return mix(hash, result.data(), result.size());
}

}

std::filesystem::path Directory() {
    if (std::getenv("APS5_RECODE_NO_PIPELINE_LIST") != nullptr) return {};
    const auto root = ShaderRecompiler::ShaderCacheDirectory();
    if (root.empty()) return {};
    char version[17];
    std::snprintf(version, sizeof(version), "%016llx", static_cast<unsigned long long>(ShaderRecompiler::ShaderDiskCache::SourceVersion()));
    return root / version / "recode-compute";
}

std::string Name(std::span<const std::uint64_t> key) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hashOf(key, {})));
    return std::string(text) + std::string(Extension);
}

bool Listed(const std::filesystem::path& directory, std::span<const std::uint64_t> key) {
    std::error_code error;
    return std::filesystem::exists(directory / Name(key), error);
}

bool Store(const std::filesystem::path& directory, const Entry& entry) {
    if (directory.empty() || entry.key.empty() || entry.result.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto file = directory / Name(entry.key);
    if (std::filesystem::exists(file, error)) return false;
    auto partial = file;
    partial += ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const Header header{Magic, entry.key.size(), entry.result.size(), hashOf(entry.key, entry.result)};
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(entry.key.data()), static_cast<std::streamsize>(entry.key.size() * sizeof(std::uint64_t)));
        out.write(reinterpret_cast<const char*>(entry.result.data()), static_cast<std::streamsize>(entry.result.size()));
        if (!out) {
            out.close();
            std::filesystem::remove(partial, error);
            return false;
        }
    }
    std::filesystem::rename(partial, file, error);
    if (error) {
        std::filesystem::remove(partial, error);
        return false;
    }
    return true;
}

std::optional<Entry> Load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    Header header;
    if (!in || !in.read(reinterpret_cast<char*>(&header), sizeof(header)) || header.magic != Magic) return std::nullopt;
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || header.keyWords == 0u || header.resultBytes == 0u || header.keyWords > size / sizeof(std::uint64_t) || header.resultBytes > size || sizeof(header) + header.keyWords * sizeof(std::uint64_t) + header.resultBytes != size) return std::nullopt;
    Entry entry;
    entry.key.resize(static_cast<std::size_t>(header.keyWords));
    entry.result.resize(static_cast<std::size_t>(header.resultBytes));
    if (!in.read(reinterpret_cast<char*>(entry.key.data()), static_cast<std::streamsize>(entry.key.size() * sizeof(std::uint64_t)))) return std::nullopt;
    if (!in.read(reinterpret_cast<char*>(entry.result.data()), static_cast<std::streamsize>(entry.result.size()))) return std::nullopt;
    if (hashOf(entry.key, entry.result) != header.hash || file.filename().string() != Name(entry.key)) return std::nullopt;
    return entry;
}

std::vector<std::filesystem::path> Newest(const std::filesystem::path& directory, std::size_t limit) {
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> found;
    std::error_code error;
    for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end; entry.increment(error)) {
        if (entry->path().extension() != Extension) continue;
        const auto written = entry->last_write_time(error);
        if (error) {
            error.clear();
            continue;
        }
        found.emplace_back(written, entry->path());
    }
    std::ranges::sort(found, [](const auto& left, const auto& right) { return left.first != right.first ? left.first > right.first : left.second < right.second; });
    if (found.size() > limit) found.resize(limit);
    std::vector<std::filesystem::path> files;
    files.reserve(found.size());
    for (auto& [written, path] : found) files.push_back(std::move(path));
    return files;
}

}
