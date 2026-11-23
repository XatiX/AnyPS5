#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DOWNGRADES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DOWNGRADES_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AgcDriver::Unified::Downgrades {

enum class Kind : std::uint32_t {
    Dropped,
    ExistingPath,
    KnownRefused,
    KnownWriteRefused,
    KnownNull,
    KnownPrefix,
    StorageRecordMissing,
    PageFault,
    UpstreamSampled,
    UpstreamStorage,
    UpstreamTarget,
    TemplateMismatch,
    KnownShort,
    SampledAhead,
    ViewMismatch,
    ImageResultsDropped,
    SamplerMode,
    UpstreamDepth,
    SampledStaleRecord,
    DepthSeedRefused,
    MetadataClearRefused,
    HtileClearRefused,
    Count,
};

enum class Severity : std::uint32_t { Info, Warn, Fallback, Severe, Count };

enum class Work : std::uint32_t { Draw, Dispatch, IndirectDispatch };

[[nodiscard]] std::string_view KindName(Kind kind);
[[nodiscard]] Severity SeverityOf(Kind kind);
[[nodiscard]] std::uint64_t HashCode(std::span<const std::byte> code);

struct Row {
    std::uint64_t hash = 0;
    std::uint64_t draws = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t indirect = 0;
    std::uint64_t groups = 0;
    std::uint64_t groupsRead = 0;
    std::uint64_t compileNanoseconds = 0;
    std::uint64_t compiles = 0;
    std::uint64_t buildNanoseconds = 0;
    std::uint64_t builds = 0;
    std::uint64_t spirvBytes = 0;
    std::set<std::uint64_t> modules;
    std::set<std::uint64_t> layouts;
    std::map<std::pair<Kind, std::uint32_t>, std::uint64_t> counts;
    std::map<std::pair<Kind, std::uint32_t>, std::string> details;
    [[nodiscard]] std::uint64_t Total() const;
    [[nodiscard]] Severity Worst() const;
};

class Ledger {
public:
    void NoteWork(std::uint64_t hash, Work work, std::uint64_t groups);
    void NoteGroups(std::uint64_t hash, std::uint64_t groups);
    void NoteCompile(std::uint64_t hash, std::uint64_t nanoseconds);
    void NoteBuild(std::uint64_t hash, std::uint64_t nanoseconds, std::uint64_t spirvBytes, std::uint64_t module, std::uint64_t layout);
    [[nodiscard]] std::vector<Row> CompileRows(std::size_t limit) const;
    bool Note(std::uint64_t hash, Kind kind, std::uint32_t binding, std::string_view detail);
    [[nodiscard]] std::vector<Row> Rows() const;
    [[nodiscard]] std::vector<std::string> Table(std::size_t limit) const;
    [[nodiscard]] static std::string Format(const Row& row);

private:
    mutable std::mutex mutex;
    std::map<std::uint64_t, Row> rows;
};

[[nodiscard]] bool Active();
[[nodiscard]] std::uint64_t ProgramHash(std::uint64_t codeAddress);
[[nodiscard]] bool ListedHash(std::string_view list, std::uint64_t hash);
[[nodiscard]] bool Excluded(std::uint64_t codeAddress);
void NoteWork(std::uint64_t codeAddress, Work work, std::uint64_t groups = 0);
void NoteGroups(std::uint64_t codeAddress, std::uint64_t groups);
void NoteCompile(std::uint64_t codeAddress, std::uint64_t nanoseconds);
void NoteBuild(std::uint64_t codeAddress, std::uint64_t nanoseconds, std::uint64_t spirvBytes, std::uint64_t module, std::uint64_t layout);
void Note(std::uint64_t codeAddress, Kind kind, std::uint32_t binding = 0, std::string_view detail = {});
void NoteHashed(std::uint64_t hash, Kind kind, std::uint32_t binding = 0, std::string_view detail = {});
void NoteCurrent(Kind kind, std::uint32_t binding = 0, std::string_view detail = {});
void NotePipeline(std::uint64_t pipeline, std::uint64_t codeAddress, std::uint64_t module);
enum class ExistingPath : std::uint8_t { Abort, Log, Allow, Drop };
[[nodiscard]] ExistingPath ExistingPathMode();
void RefuseExistingPath(std::uint64_t codeAddress, std::string_view work, std::string_view reason);
void NoteGuestWrite(const char* what, std::uint64_t address, std::uint64_t bytes);
void NoteImageResultsDropped(std::uint64_t address, std::uint64_t bytes);
[[nodiscard]] bool Watching();
void WatchProgramWrite(std::uint64_t codeAddress, std::uint64_t address, std::uint64_t bytes);
void WatchProgramRead(std::uint64_t codeAddress, std::uint64_t address, std::uint64_t bytes);
void WatchTouch(const char* what, std::uint64_t address, std::uint64_t bytes);
void WatchPoll();
void DumpModule(std::uint64_t codeAddress, std::uint64_t module, std::span<const std::uint32_t> words);
void Report();

class ProgramScope {
public:
    explicit ProgramScope(std::uint64_t codeAddress);
    ~ProgramScope();
    ProgramScope(const ProgramScope&) = delete;
    ProgramScope& operator=(const ProgramScope&) = delete;

private:
    std::uint64_t previous = 0;
};

}

#endif
