#include "prx/libSceAgcDriver/Unified/include/Downgrades.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <unordered_map>

namespace AgcDriver::Unified::Downgrades {

namespace {

constexpr std::size_t HashedCodeBytes = 4096;
constexpr std::size_t DetailBytes = 96;
constexpr std::size_t ReportRows = 200;
constexpr std::uint64_t UnreadableCode = 1ull << 63u;

struct Settings {
    bool active = false;
    bool strict = false;
    Severity stopsAt = Severity::Count;
    std::int64_t seconds = 0;
};

const Settings& settings() {
    static const Settings value = [] {
        Settings result;
        if (const char* every = std::getenv("APS5_RECODE_DOWNGRADES"); every != nullptr) {
            result.active = true;
            result.seconds = std::max<std::int64_t>(std::atoll(every), 0);
            if (result.seconds == 0) result.seconds = 10;
        }
        if (const char* strict = std::getenv("APS5_RECODE_STRICT"); strict != nullptr) {
            result.active = true;
            result.strict = true;
            if (std::strcmp(strict, "stop") == 0) result.stopsAt = Severity::Severe;
            if (std::strcmp(strict, "stop-fallback") == 0) result.stopsAt = Severity::Fallback;
            if (result.seconds == 0) result.seconds = 10;
        }
        return result;
    }();
    return value;
}

Ledger& ledger() {
    static Ledger value;
    return value;
}

std::string_view severityName(Severity severity) {
    switch (severity) {
        case Severity::Severe: return "severe";
        case Severity::Fallback: return "fallback";
        case Severity::Warn: return "warn";
        default: return "info";
    }
}

std::chrono::steady_clock::time_point started() {
    static const auto value = std::chrono::steady_clock::now();
    return value;
}

void reportWhenDue() {
    static std::atomic<std::int64_t> next{0};
    const auto& setup = settings();
    if (setup.seconds == 0) return;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started()).count();
    auto due = next.load(std::memory_order_relaxed);
    if (due == 0) {
        next.compare_exchange_strong(due, now + setup.seconds);
        return;
    }
    if (now < due || !next.compare_exchange_strong(due, now + setup.seconds)) return;
    Report();
}

}

std::string_view KindName(Kind kind) {
    switch (kind) {
        case Kind::Dropped: return "dropped";
        case Kind::ExistingPath: return "existing-path";
        case Kind::KnownRefused: return "known-refused";
        case Kind::KnownWriteRefused: return "known-write-refused";
        case Kind::KnownNull: return "known-null";
        case Kind::KnownPrefix: return "known-prefix";
        case Kind::StorageRecordMissing: return "storage-record-missing";
        case Kind::PageFault: return "page-fault";
        case Kind::UpstreamSampled: return "upstream-sampled";
        case Kind::UpstreamStorage: return "upstream-storage";
        case Kind::UpstreamTarget: return "upstream-target";
        case Kind::TemplateMismatch: return "template-mismatch";
        case Kind::KnownShort: return "known-short";
        case Kind::SampledAhead: return "sampled-ahead";
        case Kind::ViewMismatch: return "view-mismatch";
        case Kind::ImageResultsDropped: return "image-results-dropped";
        case Kind::SamplerMode: return "sampler-mode";
        case Kind::UpstreamDepth: return "upstream-depth";
        case Kind::SampledStaleRecord: return "sampled-stale-record";
        default: return "unknown";
    }
}

Severity SeverityOf(Kind kind) {
    switch (kind) {
        case Kind::Dropped:
        case Kind::KnownRefused:
        case Kind::KnownWriteRefused:
        case Kind::PageFault:
        case Kind::TemplateMismatch:
        case Kind::KnownShort:
        case Kind::SampledAhead:
        case Kind::ViewMismatch:
        case Kind::ImageResultsDropped:
        case Kind::SamplerMode:
        case Kind::SampledStaleRecord: return Severity::Severe;
        case Kind::ExistingPath:
        case Kind::StorageRecordMissing:
        case Kind::UpstreamSampled:
        case Kind::UpstreamStorage:
        case Kind::UpstreamTarget:
        case Kind::UpstreamDepth: return Severity::Fallback;
        case Kind::KnownNull:
        case Kind::KnownPrefix: return Severity::Warn;
        default: return Severity::Info;
    }
}

std::uint64_t HashCode(std::span<const std::byte> code) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto value : code) hash = (hash ^ std::to_integer<std::uint8_t>(value)) * 1099511628211ull;
    return hash;
}

std::uint64_t Row::Total() const {
    std::uint64_t total = 0;
    for (const auto& [key, count] : counts) total += count;
    return total;
}

Severity Row::Worst() const {
    auto worst = Severity::Info;
    for (const auto& [key, count] : counts) worst = std::max(worst, SeverityOf(key.first));
    return worst;
}

void Ledger::NoteWork(std::uint64_t hash, Work work, std::uint64_t groups) {
    std::lock_guard lock(mutex);
    auto& row = rows[hash];
    row.hash = hash;
    if (work == Work::Draw) {
        row.draws++;
        return;
    }
    row.dispatches++;
    if (work == Work::IndirectDispatch) {
        row.indirect++;
        return;
    }
    row.groups += groups;
    row.groupsRead++;
}

void Ledger::NoteGroups(std::uint64_t hash, std::uint64_t groups) {
    std::lock_guard lock(mutex);
    auto& row = rows[hash];
    row.hash = hash;
    row.groups += groups;
    row.groupsRead++;
}

void Ledger::NoteCompile(std::uint64_t hash, std::uint64_t nanoseconds) {
    std::lock_guard lock(mutex);
    auto& row = rows[hash];
    row.hash = hash;
    row.compileNanoseconds += nanoseconds;
    row.compiles++;
}

void Ledger::NoteBuild(std::uint64_t hash, std::uint64_t nanoseconds, std::uint64_t spirvBytes, std::uint64_t module, std::uint64_t layout) {
    std::lock_guard lock(mutex);
    auto& row = rows[hash];
    row.hash = hash;
    row.buildNanoseconds += nanoseconds;
    row.builds++;
    row.spirvBytes = std::max(row.spirvBytes, spirvBytes);
    row.modules.insert(module);
    row.layouts.insert(layout);
}

std::vector<Row> Ledger::CompileRows(std::size_t limit) const {
    std::vector<Row> result;
    {
        std::lock_guard lock(mutex);
        for (const auto& [hash, row] : rows) {
            if (row.compiles != 0u || row.builds != 0u) result.push_back(row);
        }
    }
    std::ranges::sort(result, [](const Row& left, const Row& right) { return std::max(left.compileNanoseconds, left.buildNanoseconds) > std::max(right.compileNanoseconds, right.buildNanoseconds); });
    if (result.size() > limit) result.resize(limit);
    return result;
}

bool Ledger::Note(std::uint64_t hash, Kind kind, std::uint32_t binding, std::string_view detail) {
    std::lock_guard lock(mutex);
    auto& row = rows[hash];
    row.hash = hash;
    const auto key = std::make_pair(kind, binding);
    const bool first = row.counts[key]++ == 0u;
    if (first && !detail.empty()) row.details[key] = std::string(detail.substr(0, DetailBytes));
    return first;
}

std::vector<Row> Ledger::Rows() const {
    std::vector<Row> result;
    {
        std::lock_guard lock(mutex);
        for (const auto& [hash, row] : rows) {
            if (!row.counts.empty()) result.push_back(row);
        }
    }
    std::ranges::sort(result, [](const Row& left, const Row& right) {
        if (left.Worst() != right.Worst()) return left.Worst() > right.Worst();
        if (left.Total() != right.Total()) return left.Total() > right.Total();
        return left.hash < right.hash;
    });
    return result;
}

std::string Ledger::Format(const Row& row) {
    char text[160];
    std::snprintf(text, sizeof(text), "%016llx %s draws %llu dispatches %llu indirect %llu groups %llu/%llu |", static_cast<unsigned long long>(row.hash), std::string(severityName(row.Worst())).c_str(), static_cast<unsigned long long>(row.draws), static_cast<unsigned long long>(row.dispatches), static_cast<unsigned long long>(row.indirect), static_cast<unsigned long long>(row.groups), static_cast<unsigned long long>(row.groupsRead));
    std::string line = text;
    if (row.compiles != 0u) {
        line.pop_back();
        std::snprintf(text, sizeof(text), "compile %llu ms/%llu |", static_cast<unsigned long long>(row.compileNanoseconds / 1000000u), static_cast<unsigned long long>(row.compiles));
        line += text;
    }
    for (const auto& [key, count] : row.counts) {
        std::snprintf(text, sizeof(text), " %s[%u] %llu", std::string(KindName(key.first)).c_str(), key.second, static_cast<unsigned long long>(count));
        line += text;
        if (const auto detail = row.details.find(key); detail != row.details.end()) line += " {" + detail->second + "}";
    }
    return line;
}

std::vector<std::string> Ledger::Table(std::size_t limit) const {
    const auto all = Rows();
    std::vector<std::string> lines;
    for (std::size_t index = 0; index < all.size() && index < limit; ++index) lines.push_back(Format(all[index]));
    return lines;
}

bool Active() {
    return settings().active;
}

std::uint64_t ProgramHash(std::uint64_t codeAddress) {
    static std::mutex mutex;
    static std::unordered_map<std::uint64_t, std::uint64_t> hashes;
    {
        std::lock_guard lock(mutex);
        if (const auto found = hashes.find(codeAddress); found != hashes.end()) return found->second;
    }
    std::uint64_t hash = codeAddress | UnreadableCode;
    std::uint64_t bytesHash = 0;
    try {
        if (const auto snapshot = DriverDetail::ReadRawShader(codeAddress); snapshot != nullptr && snapshot->codeAddress == codeAddress) {
            const std::span<const std::uint32_t> words(snapshot->code);
            const auto bytes = std::as_bytes(words);
            bytesHash = HashCode(bytes.first(std::min(bytes.size(), HashedCodeBytes)));
            std::vector<std::uint32_t> leading;
            for (const auto& instruction : ShaderRecompiler::RdnaInstructionDecoder{}.Decode(words).instructions) {
                const auto at = instruction.programCounter / sizeof(std::uint32_t);
                if (at < words.size()) leading.push_back(words[at]);
            }
            hash = HashCode(std::as_bytes(std::span(leading)));
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[recode-program] code 0x%llx unreadable: %s\n", static_cast<unsigned long long>(codeAddress), error.what());
    }
    {
        std::lock_guard lock(mutex);
        if (!hashes.emplace(codeAddress, hash).second) return hash;
    }
    if (Active()) std::fprintf(stderr, "[recode-program] code 0x%llx hash %016llx bytes %016llx\n", static_cast<unsigned long long>(codeAddress), static_cast<unsigned long long>(hash), static_cast<unsigned long long>(bytesHash));
    return hash;
}

bool ListedHash(std::string_view list, std::uint64_t hash) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    const std::string_view full(text, 16);
    std::size_t at = 0;
    while (at < list.size()) {
        auto end = list.find_first_of(",; ", at);
        if (end == std::string_view::npos) end = list.size();
        const auto entry = list.substr(at, end - at);
        if (entry.size() >= 4u && entry.size() <= 16u && full.starts_with(entry)) return true;
        at = end + 1u;
    }
    return false;
}

bool Excluded(std::uint64_t codeAddress) {
    static const std::string list = [] {
        const char* value = std::getenv("APS5_RECODE_EXCLUDE");
        if (value == nullptr || value[0] != '@') return std::string(value == nullptr ? "" : value);
        std::string contents;
        if (auto* file = std::fopen(value + 1, "rb"); file != nullptr) {
            char buffer[4096];
            for (std::size_t read = 0; (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) contents.append(buffer, read);
            std::fclose(file);
        }
        for (auto& character : contents) {
            if (character == '\r' || character == '\n' || character == '\t') character = ' ';
        }
        std::fprintf(stderr, "[recode-program] exclusion list %s: %zu bytes\n", value + 1, contents.size());
        return contents;
    }();
    if (list.empty()) return false;
    return ListedHash(list, ProgramHash(codeAddress));
}

void NoteWork(std::uint64_t codeAddress, Work work, std::uint64_t groups) {
    if (!Active()) return;
    ledger().NoteWork(ProgramHash(codeAddress), work, groups);
    reportWhenDue();
}

void NoteCompile(std::uint64_t codeAddress, std::uint64_t nanoseconds) {
    if (!Active()) return;
    ledger().NoteCompile(ProgramHash(codeAddress), nanoseconds);
}

void NoteBuild(std::uint64_t codeAddress, std::uint64_t nanoseconds, std::uint64_t spirvBytes, std::uint64_t module, std::uint64_t layout) {
    if (!Active()) return;
    ledger().NoteBuild(ProgramHash(codeAddress), nanoseconds, spirvBytes, module, layout);
}

void NoteGroups(std::uint64_t codeAddress, std::uint64_t groups) {
    if (!Active()) return;
    ledger().NoteGroups(ProgramHash(codeAddress), groups);
}

void NoteHashed(std::uint64_t hash, Kind kind, std::uint32_t binding, std::string_view detail) {
    if (!Active()) return;
    const bool first = ledger().Note(hash, kind, binding, detail);
    const auto& setup = settings();
    if (!first || !setup.strict) return;
    const auto severity = SeverityOf(kind);
    const char* banner = severity == Severity::Severe ? "SEVERE" : severity == Severity::Fallback ? "FALLBACK" : severity == Severity::Warn ? "warn" : "info";
    std::fprintf(stderr, "[recode-strict] %s %016llx %s[%u]: %.*s\n", banner, static_cast<unsigned long long>(hash), std::string(KindName(kind)).c_str(), binding, static_cast<int>(detail.size()), detail.data());
    if (severity >= setup.stopsAt) {
        std::fprintf(stderr, "[recode-strict] STOPPING on the first %s (APS5_RECODE_STRICT=%s)\n", banner, setup.stopsAt == Severity::Severe ? "stop" : "stop-fallback");
        Report();
        std::fflush(stderr);
        std::_Exit(86);
    }
}

void Note(std::uint64_t codeAddress, Kind kind, std::uint32_t binding, std::string_view detail) {
    if (!Active()) return;
    NoteHashed(ProgramHash(codeAddress), kind, binding, detail);
}

namespace {

struct GuestWrite {
    const char* what = nullptr;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::chrono::steady_clock::time_point at;
};

constexpr std::size_t JournalEntries = 8192;

struct GuestWriteJournal {
    std::mutex mutex;
    std::vector<GuestWrite> entries = std::vector<GuestWrite>(JournalEntries);
    std::size_t next = 0;
};

GuestWriteJournal& journal() {
    static auto* value = new GuestWriteJournal;
    return *value;
}

}

namespace {

constexpr std::size_t WatchedRanges = 8;
constexpr std::uint64_t WatchLines = 400;
constexpr std::size_t WatchSampleBytes = 16;

struct Watched {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t hash = 0;
    std::array<std::byte, WatchSampleBytes> sample{};
    bool sampled = false;
};

struct WatchState {
    std::mutex mutex;
    std::vector<Watched> ranges;
    std::uint64_t offset = 0;
    std::uint64_t lines = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

const std::string& watchList() {
    static const std::string list = [] {
        const char* value = std::getenv("APS5_RECODE_WATCH");
        return std::string(value == nullptr ? "" : value);
    }();
    return list;
}

WatchState& watchState() {
    static auto* state = [] {
        auto* created = new WatchState;
        const char* offset = std::getenv("APS5_RECODE_WATCH_OFFSET");
        created->offset = offset != nullptr ? std::strtoull(offset, nullptr, 16) : 0xfff8u;
        return created;
    }();
    return *state;
}

long long watchMs(const WatchState& state) {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - state.start).count());
}

std::string hexOf(std::span<const std::byte> bytes) {
    std::string text;
    char digits[4];
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        std::snprintf(digits, sizeof(digits), "%02x", static_cast<unsigned>(bytes[bytes.size() - 1 - index]));
        text += digits;
        if (index == 7) text += ' ';
    }
    return text;
}

}

bool Watching() {
    return !watchList().empty();
}

void WatchProgramWrite(std::uint64_t codeAddress, std::uint64_t address, std::uint64_t bytes) {
    if (!Watching() || bytes == 0u) return;
    const auto hash = ProgramHash(codeAddress);
    if (!ListedHash(watchList(), hash)) return;
    auto& state = watchState();
    std::lock_guard lock(state.mutex);
    for (const auto& range : state.ranges) {
        if (range.begin == address && range.end == address + bytes) return;
    }
    if (state.ranges.size() >= WatchedRanges) state.ranges.erase(state.ranges.begin());
    state.ranges.push_back({address, address + bytes, hash});
    std::fprintf(stderr, "[recode-watch] %lld ms: %016llx writes 0x%llx+0x%llx (sampling +0x%llx)\n", watchMs(state), static_cast<unsigned long long>(hash), static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(state.offset));
}

void WatchTouch(const char* what, std::uint64_t address, std::uint64_t bytes) {
    if (!Watching() || bytes == 0u) return;
    auto& state = watchState();
    std::lock_guard lock(state.mutex);
    for (const auto& range : state.ranges) {
        if (address >= range.end || range.begin >= address + bytes) continue;
        if (state.lines++ >= WatchLines) return;
        const auto sampled = range.begin + state.offset;
        const bool covers = address <= sampled && sampled < address + bytes;
        std::fprintf(stderr, "[recode-watch] %lld ms: %s 0x%llx+0x%llx inside 0x%llx+0x%llx%s\n", watchMs(state), what, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(range.begin), static_cast<unsigned long long>(range.end - range.begin), covers ? " (covers the sampled bytes)" : "");
        return;
    }
}

void WatchPoll() {
    if (!Watching()) return;
    auto& state = watchState();
    std::lock_guard lock(state.mutex);
    for (auto& range : state.ranges) {
        const auto sampled = range.begin + state.offset;
        if (sampled + WatchSampleBytes > range.end) continue;
        const auto* view = static_cast<const std::byte*>(GuestMemory::OwnStoreTarget(sampled, WatchSampleBytes));
        if (view == nullptr || view == reinterpret_cast<const std::byte*>(sampled)) continue;
        std::array<std::byte, WatchSampleBytes> now{};
        std::memcpy(now.data(), view, WatchSampleBytes);
        if (range.sampled && now == range.sample) continue;
        if (state.lines++ < WatchLines) std::fprintf(stderr, "[recode-watch] %lld ms: bytes at 0x%llx %s -> %s\n", watchMs(state), static_cast<unsigned long long>(sampled), range.sampled ? hexOf(range.sample).c_str() : "(first)", hexOf(now).c_str());
        range.sample = now;
        range.sampled = true;
    }
}

void NoteGuestWrite(const char* what, std::uint64_t address, std::uint64_t bytes) {
    WatchTouch(what, address, bytes);
    if (!Active() || bytes == 0u) return;
    auto& log = journal();
    std::lock_guard lock(log.mutex);
    log.entries[log.next++ % JournalEntries] = {what, address, address + bytes, std::chrono::steady_clock::now()};
}

void NoteImageResultsDropped(std::uint64_t address, std::uint64_t bytes) {
    if (!Active()) return;
    char text[160];
    std::snprintf(text, sizeof(text), "image 0x%llx+0x%llx: no recode write to its memory in the journal", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
    {
        auto& log = journal();
        std::lock_guard lock(log.mutex);
        const auto now = std::chrono::steady_clock::now();
        for (std::size_t back = 1; back <= JournalEntries && back <= log.next; ++back) {
            const auto& entry = log.entries[(log.next - back) % JournalEntries];
            if (entry.what == nullptr || entry.begin >= address + bytes || address >= entry.end) continue;
            std::snprintf(text, sizeof(text), "image 0x%llx+0x%llx: last recode write to its memory was %s 0x%llx+0x%llx, %lld ms before", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), entry.what, static_cast<unsigned long long>(entry.begin), static_cast<unsigned long long>(entry.end - entry.begin), static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.at).count()));
            break;
        }
    }
    NoteHashed(0u, Kind::ImageResultsDropped, static_cast<std::uint32_t>(address >> 16u), text);
}

ExistingPath ExistingPathMode() {
    static const ExistingPath mode = [] {
        const char* value = std::getenv("APS5_RECODE_EXISTING_PATH");
        if (value == nullptr) return ExistingPath::Abort;
        if (std::strcmp(value, "allow") == 0) return ExistingPath::Allow;
        if (std::strcmp(value, "log") == 0) return ExistingPath::Log;
        return ExistingPath::Abort;
    }();
    return mode;
}

void RefuseExistingPath(std::uint64_t codeAddress, std::string_view work, std::string_view reason) {
    const auto mode = ExistingPathMode();
    if (mode == ExistingPath::Allow) return;
    const auto hash = codeAddress != 0u ? ProgramHash(codeAddress) : std::uint64_t{0};
    if (mode == ExistingPath::Log) {
        static std::mutex seenMutex;
        static std::set<std::string> seen;
        char key[160];
        std::snprintf(key, sizeof(key), "%016llx %.*s %.*s", static_cast<unsigned long long>(hash), static_cast<int>(work.size()), work.data(), static_cast<int>(std::min<std::size_t>(reason.size(), 100u)), reason.data());
        std::lock_guard lock(seenMutex);
        if (!seen.insert(key).second) return;
    }
    std::fprintf(stderr, "[recode-fatal] %.*s of program %016llx (code 0x%llx) cannot be recorded and the recode route has no fallback to the existing path: %.*s\n", static_cast<int>(work.size()), work.data(), static_cast<unsigned long long>(hash), static_cast<unsigned long long>(codeAddress), static_cast<int>(reason.size()), reason.data());
    if (mode == ExistingPath::Log) return;
    std::fprintf(stderr, "[recode-fatal] stopping; APS5_RECODE_EXISTING_PATH=log reports every such case and carries on, =allow restores the fallback for comparison runs\n");
    if (Active()) Report();
    std::fflush(stderr);
    std::_Exit(87);
}

namespace {

thread_local std::uint64_t currentProgram = 0;

}

ProgramScope::ProgramScope(std::uint64_t codeAddress) : previous(currentProgram) {
    currentProgram = codeAddress;
}

ProgramScope::~ProgramScope() {
    currentProgram = previous;
}

void DumpModule(std::uint64_t codeAddress, std::uint64_t module, std::span<const std::uint32_t> words) {
    static const char* list = std::getenv("APS5_RECODE_DUMP_MODULES");
    static const char* directory = std::getenv("APS5_RECODE_DUMP_DIR");
    if (list == nullptr || directory == nullptr || *list == '\0') return;
    const auto hash = ProgramHash(codeAddress);
    if (!ListedHash(list, hash)) return;
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/%016llx-%016llx.spv", directory, static_cast<unsigned long long>(hash), static_cast<unsigned long long>(module));
    auto* file = std::fopen(path, "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "[recode-program] cannot write %s\n", path);
        return;
    }
    std::fwrite(words.data(), sizeof(std::uint32_t), words.size(), file);
    std::fclose(file);
    std::fprintf(stderr, "[recode-program] wrote module %s (%zu bytes)\n", path, words.size_bytes());
}

void NotePipeline(std::uint64_t pipeline, std::uint64_t codeAddress, std::uint64_t module) {
    if (!Active()) return;
    static std::mutex mutex;
    static std::unordered_map<std::uint64_t, std::uint64_t> seen;
    {
        std::lock_guard lock(mutex);
        if (!seen.emplace(pipeline, codeAddress).second) return;
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started()).count();
    std::fprintf(stderr, "[recode-pipeline] 0x%llx hash %016llx module %016llx first bound at %lld s\n", static_cast<unsigned long long>(pipeline), static_cast<unsigned long long>(ProgramHash(codeAddress)), static_cast<unsigned long long>(module), static_cast<long long>(seconds));
}

void NoteCurrent(Kind kind, std::uint32_t binding, std::string_view detail) {
    if (!Active() || currentProgram == 0u) return;
    Note(currentProgram, kind, binding, detail);
}

void Report() {
    if (!Active()) return;
    const auto lines = ledger().Table(ReportRows);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started()).count();
    std::fprintf(stderr, "[recode-downgrades] table at %lld s: %zu programs with downgrades\n", static_cast<long long>(seconds), lines.size());
    for (const auto& line : lines) std::fprintf(stderr, "[recode-downgrades] %s\n", line.c_str());
    const auto compiled = ledger().CompileRows(SIZE_MAX);
    std::uint64_t compileTotal = 0;
    std::uint64_t compileCount = 0;
    std::uint64_t buildTotal = 0;
    std::uint64_t buildCount = 0;
    for (const auto& row : compiled) {
        compileTotal += row.compileNanoseconds;
        compileCount += row.compiles;
        buildTotal += row.buildNanoseconds;
        buildCount += row.builds;
    }
    std::fprintf(stderr, "[recode-compile] at %lld s: %llu ms waiting on %llu pipeline builds, %llu ms building %llu compute pipelines (summed over the builder threads), %zu programs\n", static_cast<long long>(seconds), static_cast<unsigned long long>(compileTotal / 1000000u), static_cast<unsigned long long>(compileCount), static_cast<unsigned long long>(buildTotal / 1000000u), static_cast<unsigned long long>(buildCount), compiled.size());
    for (std::size_t index = 0; index < compiled.size() && index < 12u; ++index) std::fprintf(stderr, "[recode-compile] %016llx waited %llu ms/%llu built %llu ms/%llu (%zu modules, %zu layouts) spirv %llu KiB draws %llu dispatches %llu\n", static_cast<unsigned long long>(compiled[index].hash), static_cast<unsigned long long>(compiled[index].compileNanoseconds / 1000000u), static_cast<unsigned long long>(compiled[index].compiles), static_cast<unsigned long long>(compiled[index].buildNanoseconds / 1000000u), static_cast<unsigned long long>(compiled[index].builds), compiled[index].modules.size(), compiled[index].layouts.size(), static_cast<unsigned long long>(compiled[index].spirvBytes >> 10u), static_cast<unsigned long long>(compiled[index].draws), static_cast<unsigned long long>(compiled[index].dispatches));
    if (!settings().strict) return;
    std::array<std::size_t, static_cast<std::size_t>(Severity::Count)> worst{};
    for (const auto& row : ledger().Rows()) worst[static_cast<std::size_t>(row.Worst())]++;
    std::fprintf(stderr, "[recode-strict] at %lld s: %zu programs with SEVERE downgrades, %zu using FALLBACKS to the existing path, %zu with warnings\n", static_cast<long long>(seconds), worst[static_cast<std::size_t>(Severity::Severe)], worst[static_cast<std::size_t>(Severity::Fallback)], worst[static_cast<std::size_t>(Severity::Warn)]);
}

}
