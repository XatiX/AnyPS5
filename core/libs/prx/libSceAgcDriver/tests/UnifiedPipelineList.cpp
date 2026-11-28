#include "prx/libSceAgcDriver/Unified/include/PipelineList.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace AgcDriver::Unified;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

std::filesystem::path Scratch() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("agc-pipeline-list-" + std::to_string(stamp));
}

PipelineList::Entry Sample(std::uint64_t seed, std::size_t bytes) {
    PipelineList::Entry entry;
    entry.key = {seed, seed ^ 0x9e3779b97f4a7c15ull, 0u, 1u, 32u, 5u, 64u, 32u};
    entry.result.resize(bytes);
    for (std::size_t index = 0; index < bytes; ++index) entry.result[index] = static_cast<std::byte>((seed + index * 7u) & 0xffu);
    return entry;
}

void testAnEntryComesBackAsStored(const std::filesystem::path& directory) {
    const auto entry = Sample(1, 4096);
    Expect(!PipelineList::Listed(directory, entry.key), "an empty folder lists nothing");
    Expect(PipelineList::Store(directory, entry), "a first entry is stored");
    Expect(PipelineList::Listed(directory, entry.key), "a stored entry is listed");
    Expect(!PipelineList::Store(directory, entry), "an entry is stored once");
    const auto loaded = PipelineList::Load(directory / PipelineList::Name(entry.key));
    Expect(loaded.has_value() && *loaded == entry, "a stored entry loads with its key and its bytes");
    Expect(PipelineList::Name(entry.key) != PipelineList::Name(Sample(2, 16).key), "two keys have two names");
}

void testADamagedFileIsRefused(const std::filesystem::path& directory) {
    const auto entry = Sample(3, 2048);
    Expect(PipelineList::Store(directory, entry), "the entry to damage is stored");
    const auto file = directory / PipelineList::Name(entry.key);
    {
        std::fstream damaged(file, std::ios::binary | std::ios::in | std::ios::out);
        damaged.seekp(200);
        damaged.put('\x55');
    }
    Expect(!PipelineList::Load(file).has_value(), "a file with a changed byte is refused");
    std::error_code error;
    std::filesystem::resize_file(file, 100, error);
    Expect(!PipelineList::Load(file).has_value(), "a cut file is refused");
    const auto other = Sample(4, 512);
    Expect(PipelineList::Store(directory, other), "another entry is stored");
    std::filesystem::copy_file(directory / PipelineList::Name(other.key), directory / PipelineList::Name(Sample(5, 8).key), error);
    Expect(!PipelineList::Load(directory / PipelineList::Name(Sample(5, 8).key)).has_value(), "an entry under another key's name is refused");
    Expect(!PipelineList::Store(directory, PipelineList::Entry{}), "an empty entry is not stored");
    Expect(!PipelineList::Store({}, other), "nothing is stored without a folder");
}

void testTheNewestEntriesComeFirst(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::vector<std::filesystem::path> stored;
    for (std::uint64_t seed = 10; seed < 15; ++seed) {
        const auto entry = Sample(seed, 64);
        Expect(PipelineList::Store(directory, entry), "an entry of the ordered set is stored");
        stored.push_back(directory / PipelineList::Name(entry.key));
        std::filesystem::last_write_time(stored.back(), std::filesystem::file_time_type::clock::now() - std::chrono::hours(20 - seed), error);
    }
    std::ofstream(directory / "note.txt") << "not an entry";
    const auto every = PipelineList::Newest(directory);
    Expect(every.size() == 5 && every.front() == stored.back() && every.back() == stored.front(), "every entry is returned, newest first, and nothing else");
    const auto two = PipelineList::Newest(directory, 2);
    Expect(two.size() == 2 && two[0] == stored[4] && two[1] == stored[3], "a limit keeps the newest entries");
    Expect(PipelineList::Newest(directory / "missing").empty(), "a missing folder has no entries");
}

}

int main() {
    const auto directory = Scratch();
    testAnEntryComesBackAsStored(directory);
    testADamagedFileIsRefused(directory);
    testTheNewestEntriesComeFirst(directory);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    if (failures != 0) return 1;
    std::puts("unified pipeline list tests passed");
    return 0;
}
