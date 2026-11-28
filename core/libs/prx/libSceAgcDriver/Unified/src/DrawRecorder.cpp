#include "prx/libSceAgcDriver/Unified/include/DrawRecorder.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordLoop.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libSceAgcDriver/Unified/include/RouteDefaults.hpp"
#include "prx/libkernel/include/Api.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineSpecialization.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Unified/include/BuildBudget.hpp"
#include "prx/libSceAgcDriver/Unified/include/DataRing.hpp"
#include "prx/libSceAgcDriver/Unified/include/DepthRecords.hpp"
#include "prx/libSceAgcDriver/Unified/include/LabelImports.hpp"
#include "prx/libSceAgcDriver/Unified/include/StageCosts.hpp"
#include "prx/libSceAgcDriver/Unified/include/Downgrades.hpp"
#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include "prx/libSceAgcDriver/Unified/include/ImageDetile.hpp"
#include "prx/libSceAgcDriver/Unified/include/IndirectArguments.hpp"
#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include "prx/libSceAgcDriver/Unified/include/MirrorHost.hpp"
#include "prx/libSceAgcDriver/Unified/include/PipelineList.hpp"
#include "prx/libSceAgcDriver/Unified/include/Releases.hpp"
#include "prx/libSceAgcDriver/Unified/include/UploadClash.hpp"
#include "prx/libSceAgcDriver/Unified/include/MetadataWatch.hpp"
#include "prx/libSceAgcDriver/Unified/include/HtileClears.hpp"
#include "prx/libSceAgcDriver/Unified/include/MetadataClears.hpp"
#include "prx/libSceAgcDriver/Unified/include/ResidencyService.hpp"
#include "prx/libSceAgcDriver/Unified/include/SlotPatch.hpp"
#include "prx/libSceAgcDriver/Unified/include/RouteMemory.hpp"
#include "prx/libSceAgcDriver/Unified/include/TableMirror.hpp"
#include "prx/libSceAgcDriver/Unified/include/Targets.hpp"
#include "prx/libSceAgcDriver/Unified/include/VulkanHeap.hpp"
#include "prx/libSceAgcDriver/Unified/include/VulkanImages.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include "BdaAbi.hpp"
#include "ShaderDiskCache.hpp"
#include "RuntimeAbi.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <thread>

namespace AgcDriver::Graphics {
void WriteCheckpointMarker(const Context& context, VkCommandBuffer commands, std::uint64_t token, std::uint32_t queue);
void WriteNvCheckpointToken(const Context& context, VkCommandBuffer commands, std::uint64_t token, std::uint32_t queue);
}

namespace AgcDriver::Unified::Record {

namespace {

using Graphics::Context;

constexpr std::uint64_t UploadTimingKey = 0x7ec0de02;
constexpr std::uint64_t DrawTimingKey = Graphics::Recorder::DrawTimingFlag;
constexpr std::uint64_t DispatchTimingKey = 0x7ec0de04;

bool writesFaultTokens() {
    static const bool writes = std::getenv("APS5_GPU_CHECKPOINTS") != nullptr;
    return writes;
}

thread_local std::string knownPaths;

void noteKnownPath(const char* path, std::uint64_t address, std::uint64_t bytes) {
    static const bool marking = std::getenv("APS5_RECODE_HANG_MARKS") != nullptr;
    if (!marking || knownPaths.size() > 200u) return;
    char text[64];
    std::snprintf(text, sizeof(text), " %s 0x%llx+0x%llx", path, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
    knownPaths += text;
}

struct HangMarks {
    static constexpr std::uint32_t Slots = 1u << 16u;
    static constexpr std::uint32_t Snapshots = 256u;
    static constexpr std::uint32_t Inputs = 4u;
    static constexpr std::uint32_t InputBytes = 64u;
    struct Snapshot {
        std::array<std::uint64_t, Inputs> addresses{};
        std::array<std::uint32_t, Inputs> sizes{};
        std::array<std::array<std::byte, InputBytes>, Inputs> guest{};
        std::array<std::uint32_t, Inputs> writtenPieces{};
        std::array<std::uint64_t, Inputs> writtenSerial{};
        std::array<std::uint32_t, Inputs> pageState{};
        std::uint64_t completed = 0;
        std::string known;
    };
    std::mutex mutex;
    std::shared_ptr<Graphics::Buffer> buffer;
    std::shared_ptr<Graphics::Buffer> mirrorCopies;
    std::array<std::pair<std::uint64_t, std::uint64_t>, Slots> programs{};
    std::array<std::uint64_t, Slots> batches{};
    std::array<Snapshot, Snapshots> snapshots{};
    std::uint64_t detileRecords = 0;
    std::uint64_t detilePixels = 0;
    std::uint64_t detileAddress = 0;
    std::string detileKey;
    std::uint32_t next = 0;
};

std::string hangInputs(HangMarks& marks, std::uint32_t sequence) {
    const auto slot = sequence % HangMarks::Snapshots;
    const auto& snapshot = marks.snapshots[slot];
    std::string line = " | known buffers:" + snapshot.known;
    marks.mirrorCopies->Invalidate();
    const auto* mirror = marks.mirrorCopies->Bytes().data() + static_cast<std::size_t>(slot) * HangMarks::Inputs * HangMarks::InputBytes;
    char text[64];
    for (std::uint32_t input = 0; input < HangMarks::Inputs; ++input) {
        if (snapshot.sizes[input] == 0u) continue;
        std::snprintf(text, sizeof(text), " | 0x%llx (page %u, %u written pieces, newest serial %llu of %llu done) guest", static_cast<unsigned long long>(snapshot.addresses[input]), snapshot.pageState[input], snapshot.writtenPieces[input], static_cast<unsigned long long>(snapshot.writtenSerial[input]), static_cast<unsigned long long>(snapshot.completed));
        line += text;
        for (std::uint32_t at = 0; at + 8u <= snapshot.sizes[input]; at += 8u) {
            std::uint64_t value = 0;
            std::memcpy(&value, snapshot.guest[input].data() + at, sizeof(value));
            std::snprintf(text, sizeof(text), " %llx", static_cast<unsigned long long>(value));
            line += text;
        }
        line += " mirror";
        for (std::uint32_t at = 0; at + 8u <= snapshot.sizes[input]; at += 8u) {
            std::uint64_t value = 0;
            std::memcpy(&value, mirror + input * HangMarks::InputBytes + at, sizeof(value));
            std::snprintf(text, sizeof(text), " %llx", static_cast<unsigned long long>(value));
            line += text;
        }
    }
    return line;
}

HangMarks& hangMarks() {
    static auto& marks = *new HangMarks();
    return marks;
}

void watchHangMarks() {
    auto& marks = hangMarks();
    std::uint32_t shown = 0;
    std::uint32_t still = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::uint32_t seen = 0;
        std::pair<std::uint64_t, std::uint64_t> program{};
        std::string inputs;
        {
            std::lock_guard lock(marks.mutex);
            if (marks.buffer == nullptr) continue;
            marks.buffer->Invalidate();
            std::memcpy(&seen, marks.buffer->Bytes().data(), sizeof(seen));
            program = marks.programs[seen % HangMarks::Slots];
            if (seen == shown && still == 3u) inputs = hangInputs(marks, seen);
        }
        if (seen == shown) {
            if (++still == 4u) {
                std::uint32_t finished = 0;
                std::uint32_t phase = 0;
                std::uint64_t batch = 0;
                const auto completed = MemoryCore::Get().CompletedSerial();
                {
                    std::lock_guard lock(marks.mutex);
                    batch = marks.batches[seen % HangMarks::Slots];
                    std::memcpy(&finished, marks.buffer->Bytes().data() + 4, sizeof(finished));
                    std::memcpy(&phase, marks.buffer->Bytes().data() + 8, sizeof(phase));
                }
                static constexpr std::array<const char*, 10> phases{"none", "before the store flush", "before the owned uploads", "after the stores following a dispatch", "at the next item's start (before its write-backs and hand-overs)", "after the next item's write-backs for readers", "after the next item's hand-overs for writers", "before a detile batch of new records", "after a detile batch of new records", "after the owned uploads before a detile batch"};
                std::uint64_t detileRecords = 0;
                std::uint64_t detilePixels = 0;
                std::uint64_t detileAddress = 0;
                std::string detileKey;
                {
                    std::lock_guard lock(marks.mutex);
                    detileRecords = marks.detileRecords;
                    detilePixels = marks.detilePixels;
                    detileAddress = marks.detileAddress;
                    detileKey = marks.detileKey;
                }
                std::fprintf(stderr, "[recode-hang-mark] batch %llu, GPU completed through batch %llu (%s); last detile batch: %llu records, %llu pixels, first at 0x%llx%s\n", static_cast<unsigned long long>(batch), static_cast<unsigned long long>(completed), completed >= batch ? "that batch finished: the GPU waits at a later batch's start" : "that batch did not finish: the stall is inside it", static_cast<unsigned long long>(detileRecords), static_cast<unsigned long long>(detilePixels), static_cast<unsigned long long>(detileAddress), detileKey.c_str());
                std::fprintf(stderr, "[recode-hang-mark] the GPU has not passed dispatch %u for 1 s (%s; last phase mark: item %u %s): program %016llx, %llu groups%s\n", seen, finished == seen ? "that dispatch finished, the stall is in later work" : "that dispatch did not finish", phase >> 4u, phases[std::min<std::uint32_t>(phase & 15u, 9u)], static_cast<unsigned long long>(program.first), static_cast<unsigned long long>(program.second), inputs.c_str());
            }
            continue;
        }
        shown = seen;
        still = 0;
    }
}

bool hangMarking() {
    static const bool marking = std::getenv("APS5_RECODE_HANG_MARKS") != nullptr;
    return marking;
}

struct BatchWork {
    std::string item;
    const std::byte* arguments = nullptr;
    std::shared_ptr<const void> keep;
};

struct OpenBatch {
    std::chrono::steady_clock::time_point opened;
    std::vector<BatchWork> work;
    bool shown = false;
};

struct BatchPrograms {
    std::mutex mutex;
    std::map<std::uint64_t, OpenBatch> open;
    std::array<std::pair<std::uint64_t, std::string>, 4096> recent;
    std::size_t recentNext = 0;
    bool historyShown = false;
    Graphics::Recorder* recorder = nullptr;
};

BatchPrograms& batchPrograms() {
    static BatchPrograms programs;
    return programs;
}

bool watchesBatchPrograms() {
    static const bool watching = std::getenv("APS5_RECODE_HANG_PROGRAMS") != nullptr || std::getenv("APS5_RECODE_HANG_EXIT") != nullptr;
    return watching;
}

void watchOpenBatches() {
    static const long long exitAfter = [] {
        const char* value = std::getenv("APS5_RECODE_HANG_EXIT");
        return value != nullptr ? std::strtoll(value, nullptr, 10) : 0ll;
    }();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto& programs = batchPrograms();
        std::lock_guard lock(programs.mutex);
        const auto now = std::chrono::steady_clock::now();
        if (exitAfter > 0 && !programs.open.empty() && now - programs.open.begin()->second.opened > std::chrono::seconds(exitAfter)) {
            std::fprintf(stderr, "[recode-hang-batch] batch %llu still not finished after %lld s: ending the process (APS5_RECODE_HANG_EXIT)\n", static_cast<unsigned long long>(programs.open.begin()->first), exitAfter);
            std::fflush(stderr);
            std::_Exit(3);
        }
        for (auto& [serial, batch] : programs.open) {
            if (batch.shown || now - batch.opened < std::chrono::seconds(5)) continue;
            batch.shown = true;
            std::string list;
            for (const auto& work : batch.work) {
                list += " " + work.item;
                if (work.arguments == nullptr) continue;
                std::uint32_t counts[3] = {};
                std::memcpy(counts, work.arguments, sizeof(counts));
                list += "(" + std::to_string(counts[0]) + "x" + std::to_string(counts[1]) + "x" + std::to_string(counts[2]) + ")";
            }
            std::fprintf(stderr, "[recode-hang-batch] batch %llu not finished %lld s after its first recorded work; %zu items:%s\n", static_cast<unsigned long long>(serial), static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(now - batch.opened).count()), batch.work.size(), list.c_str());
            if (programs.recorder != nullptr) std::fprintf(stderr, "[recode-hang-stamps] batch %llu, end stamps in order:%s\n", static_cast<unsigned long long>(serial), programs.recorder->StampProgress(serial).c_str());
            if (!programs.historyShown) {
                programs.historyShown = true;
                static const std::uint64_t window = [] {
                    const char* value = std::getenv("APS5_RECODE_HANG_HISTORY_BATCHES");
                    return value != nullptr ? std::strtoull(value, nullptr, 10) : 3ull;
                }();
                for (std::size_t index = 0; index < programs.recent.size(); ++index) {
                    const auto& [batchSerial, line] = programs.recent[(programs.recentNext + index) % programs.recent.size()];
                    if (!line.empty() && batchSerial + window >= serial && batchSerial <= serial) std::fprintf(stderr, "[recode-hang-history] %s\n", line.c_str());
                }
                Loop::PrintRecentWaits();
            }
        }
    }
}

void noteBatchWork(Graphics::Recorder& recorder, std::uint64_t serial, const char* kind, std::uint64_t program, std::uint64_t groups, const std::byte* arguments = nullptr, std::shared_ptr<const void> keep = nullptr, std::uint32_t queue = 0, std::uint64_t submission = 0) {
    if (!watchesBatchPrograms()) return;
    static std::once_flag started;
    std::call_once(started, [] { std::thread(watchOpenBatches).detach(); });
    auto& programs = batchPrograms();
    bool first = false;
    {
        std::lock_guard lock(programs.mutex);
        programs.recorder = &recorder;
        auto [found, inserted] = programs.open.try_emplace(serial);
        if (inserted) {
            found->second.opened = std::chrono::steady_clock::now();
            first = true;
        }
        if (found->second.work.size() < 512u) {
            char item[256];
            std::snprintf(item, sizeof(item), "%s:%016llx/%llu", kind, static_cast<unsigned long long>(program), static_cast<unsigned long long>(groups));
            found->second.work.push_back({item, arguments, std::move(keep)});
        }
        {
            char line[320];
            std::snprintf(line, sizeof(line), "batch %llu queue 0x%x submission %llu %s:%016llx/%llu", static_cast<unsigned long long>(serial), queue, static_cast<unsigned long long>(submission), kind, static_cast<unsigned long long>(program), static_cast<unsigned long long>(groups));
            programs.recent[programs.recentNext++ % programs.recent.size()] = {serial, line};
        }
    }
    if (first) recorder.OnComplete([serial] {
        auto& programs = batchPrograms();
        std::lock_guard lock(programs.mutex);
        programs.open.erase(serial);
    });
}

void noteDetileWork(std::uint64_t records, std::uint64_t pixels, std::uint64_t address) {
    auto& marks = hangMarks();
    std::lock_guard lock(marks.mutex);
    marks.detileRecords = records;
    marks.detilePixels = pixels;
    marks.detileAddress = address;
}

void noteDetileKey(const char* key) {
    auto& marks = hangMarks();
    std::lock_guard lock(marks.mutex);
    marks.detileKey = key;
}

void markHangPhase(const Context& context, Graphics::Recorder& recorder, std::uint32_t phase) {
    if (!hangMarking()) return;
    auto& marks = hangMarks();
    std::uint32_t value = 0;
    std::shared_ptr<Graphics::Buffer> buffer;
    {
        std::lock_guard lock(marks.mutex);
        value = (marks.next << 4u) | phase;
        buffer = marks.buffer;
    }
    if (buffer == nullptr) return;
    const auto commands = recorder.Commands();
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, buffer->Handle(), 8, 4, value);
}

void finishHangMark(const Context& context, VkCommandBuffer commands) {
    static const bool marking = std::getenv("APS5_RECODE_HANG_MARKS") != nullptr;
    if (!marking) return;
    auto& marks = hangMarks();
    std::uint32_t sequence = 0;
    std::shared_ptr<Graphics::Buffer> buffer;
    {
        std::lock_guard lock(marks.mutex);
        sequence = marks.next;
        buffer = marks.buffer;
    }
    if (buffer == nullptr) return;
    context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, buffer->Handle(), 4, 4, sequence);
}

void markHang(const Context& context, Graphics::Recorder& recorder, VkCommandBuffer commands, std::uint64_t hash, std::uint64_t groups, const Resolve::ResolvedDraw& work) {
    static const bool marking = std::getenv("APS5_RECODE_HANG_MARKS") != nullptr;
    if (!marking) return;
    auto& marks = hangMarks();
    std::uint32_t sequence = 0;
    std::vector<std::pair<std::uint32_t, DeviceSpan>> copies;
    {
        std::lock_guard lock(marks.mutex);
        if (marks.buffer == nullptr) {
            marks.buffer = std::make_shared<Graphics::Buffer>(context, 256u, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            std::memset(marks.buffer->Bytes().data(), 0, 256u);
            const auto snapshotBytes = static_cast<std::size_t>(HangMarks::Snapshots) * HangMarks::Inputs * HangMarks::InputBytes;
            marks.mirrorCopies = std::make_shared<Graphics::Buffer>(context, snapshotBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            std::memset(marks.mirrorCopies->Bytes().data(), 0, snapshotBytes);
            std::thread(watchHangMarks).detach();
        }
        sequence = ++marks.next;
        marks.programs[sequence % HangMarks::Slots] = {hash, groups};
        marks.batches[sequence % HangMarks::Slots] = recorder.OpenBatchSerial();
        auto& snapshot = marks.snapshots[sequence % HangMarks::Snapshots];
        snapshot = {};
        snapshot.known = std::move(knownPaths);
        knownPaths.clear();
        std::uint32_t input = 0;
        for (std::size_t index = 0; index < work.uses.size() && index < work.imageBegin && input < HangMarks::Inputs; ++index) {
            const auto& use = work.uses[index];
            if (use.write || use.size == 0u) continue;
            const auto shown = std::min<std::uint64_t>(use.size, HangMarks::InputBytes);
            snapshot.addresses[input] = use.address;
            snapshot.sizes[input] = static_cast<std::uint32_t>(shown);
            auto& core = MemoryCore::Get();
            const auto pieces = core.Written().In(use.address, use.size);
            snapshot.writtenPieces[input] = static_cast<std::uint32_t>(pieces.size());
            for (const auto& piece : pieces) snapshot.writtenSerial[input] = std::max(snapshot.writtenSerial[input], piece.serial);
            snapshot.pageState[input] = static_cast<std::uint32_t>(core.Pages().StateOf(use.address & ~(PageBytes - 1u)));
            snapshot.completed = recorder.CompletedThrough();
            if (Protector::Readable(use.address) && Protector::Readable(use.address + shown - 1u)) std::memcpy(snapshot.guest[input].data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(use.address)), static_cast<std::size_t>(shown));
            if (const auto span = ResidencyService::Get().MirrorSpan(use.address, shown); span && span->handle != 0u) copies.emplace_back(input, *span);
            ++input;
        }
    }
    recorder.Keep(marks.buffer);
    recorder.Keep(marks.mirrorCopies);
    const auto slot = sequence % HangMarks::Snapshots;
    const auto copy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    for (const auto& [input, span] : copies) {
        const VkBufferCopy region{span.offset, (static_cast<VkDeviceSize>(slot) * HangMarks::Inputs + input) * HangMarks::InputBytes, std::min<VkDeviceSize>(span.bytes, HangMarks::InputBytes)};
        copy(commands, reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span.handle)), marks.mirrorCopies->Handle(), 1, &region);
    }
    context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, marks.buffer->Handle(), 0, 4, sequence);
}

void noteFaultToken(const Context& context, Graphics::Recorder& recorder, VkCommandBuffer commands, std::span<const Resolve::ResolvedStage> stages, FaultStage stage, const char* kind, std::uint32_t count, std::uint32_t instances) {
    try {
        if (!writesFaultTokens()) return;
        constexpr std::uint32_t queue = 0;
        auto& ring = FaultTokenRing::Instance();
        const auto token = ring.Claim();
        auto* entry = ring.Mutable(token);
        if (entry == nullptr) return;
        std::uint64_t hashes[2] = {};
        std::uint64_t address = 0;
        std::uint64_t variant = 0;
        for (const auto& one : stages) {
            if (one.program.result == nullptr) continue;
            const auto hash = one.program.result->spirv.ContentHash().low;
            const bool fragment = one.program.stage == ShaderRecompiler::ShaderStage::Fragment;
            hashes[fragment ? 1 : 0] = hash;
            if (variant == 0 || fragment) {
                variant = one.program.result->variantId;
                address = one.program.codeAddress;
            }
        }
        entry->submitSerial = recorder.OpenSerial();
        entry->programAddress = address;
        entry->variantId = variant;
        entry->stage = static_cast<std::uint32_t>(stage);
        entry->argBuffer = 0;
        entry->argOffset = 0;
        entry->argStride = 0;
        entry->argCount = 0;
        std::snprintf(entry->desc, sizeof(entry->desc), "recode %s %08x/%08x c=%u i=%u", kind, static_cast<std::uint32_t>(hashes[0]), static_cast<std::uint32_t>(hashes[1]), count, instances);
        recorder.NoteFaultToken(token);
        std::atomic_ref<FaultToken>(entry->token).store(token, std::memory_order_release);
        Graphics::WriteCheckpointMarker(context, commands, token, queue);
        Graphics::WriteNvCheckpointToken(context, commands, token, queue);
    } catch (...) {
    }
}

constexpr std::uint64_t RingBytes = 64ull << 20u;
constexpr std::uint64_t RingZeroBytes = 256;
constexpr std::uint64_t KnownReadableBytes = 32;
constexpr std::uint32_t PoolSets = 512;
constexpr std::uint32_t PoolBuffers = 4096;
constexpr std::uint64_t MapBudgetBytes = 256ull << 20u;
constexpr std::size_t TableImagesBound = 4096;

struct MapBuffer {
    std::shared_ptr<Graphics::Buffer> buffer;
    std::uint64_t serial = 0;
};

struct TableImages {
    std::weak_ptr<const Tables::Mirror> base;
    std::shared_ptr<const Tables::Mirror> bound;
    std::uint64_t depths = 0;
    std::uint64_t sharedStamp = 0;
    std::uint64_t imagesStamp = 0;
    Tables::ElementIndex index;
    std::vector<Heap::DeferredImage> elements;
    std::vector<std::uint32_t> slots;
    std::vector<Heap::Span> memory;
    std::vector<std::shared_ptr<const void>> owners;
    std::vector<std::shared_ptr<Images::Surface>> records;
};

struct FaultBuffer {
    std::shared_ptr<Graphics::Buffer> buffer;
    std::uint64_t serial = 0;
};

struct KnownWrite {
    std::uint64_t serial = 0;
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    std::shared_ptr<const std::vector<std::byte>> before;
};

struct CopyBack {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    const Graphics::HostImport* import = nullptr;
};

struct Totals {
    std::uint64_t recorded = 0;
    std::uint64_t refused = 0;
    std::uint64_t existing = 0;
    std::uint64_t linearStores = 0;
    std::uint64_t indirect = 0;
    std::uint64_t snapshotArguments = 0;
    std::uint64_t pools = 0;
    std::uint64_t mapHits = 0;
    std::uint64_t mapUploads = 0;
    std::uint64_t mapUploadBytes = 0;
    std::uint64_t mapRetired = 0;
    std::uint64_t tableImageHits = 0;
    std::uint64_t tableImageBuilds = 0;
    std::uint64_t tableImageWhole = 0;
    std::uint64_t tableImageBinds = 0;
    std::uint64_t tableImageRefreshes = 0;
    std::uint64_t tableImageDepthKept = 0;
    std::uint64_t tableImageRetired = 0;
    std::uint64_t tableImageEntriesDropped = 0;
    std::uint64_t compressedAliases = 0;
    std::uint64_t surfaceFailures = 0;
    std::uint64_t faultReads = 0;
    std::uint64_t batchMoves = 0;
    std::uint64_t passesBegun = 0;
    std::uint64_t passesContinued = 0;
    std::uint64_t passFeedback = 0;
    std::uint64_t deferredReads = 0;
    std::uint64_t targetMemoHits = 0;
    std::uint64_t targetMemoMisses = 0;
    std::uint64_t residencyLands = 0;
    std::uint64_t faults = 0;
    std::uint64_t unmappedFaults = 0;
    std::uint64_t faultPages = 0;
    std::uint64_t writtenRanges = 0;
    std::uint64_t writtenLate = 0;
    std::uint64_t ownedBytes = 0;
    std::uint64_t openLandedBytes = 0;
    std::uint64_t mirrorWriteBytes = 0;
    std::uint64_t mirrorArguments = 0;
    std::uint64_t patchedArguments = 0;
    std::uint64_t depthWriterHandOvers = 0;
    std::uint64_t drawLockNanoseconds = 0;
    std::uint64_t drawBodyNanoseconds = 0;
    std::uint64_t drawLandingNanoseconds = 0;
    std::array<std::uint64_t, 9> recordSplit{};
    std::array<std::uint64_t, 3> preSplit{};
    std::map<std::string, std::uint64_t> sampledCreations;
    std::map<std::string, std::uint64_t> shapeHandOvers;
    std::uint64_t meshDraws = 0;
    std::uint64_t meshEmpty = 0;
    std::uint64_t edgeSnapshotBytes = 0;
    std::uint64_t edgeDiffBytes = 0;
    std::uint64_t writtenOverflows = 0;
    std::uint64_t writtenOnImports = 0;
    std::uint64_t importSwitches = 0;
    std::uint64_t writtenSwitches = 0;
    std::uint64_t storageWrites = 0;
    std::uint64_t drawStorageWrites = 0;
    std::uint64_t switchBytes = 0;
    std::uint64_t switchRefused = 0;
    std::uint64_t nanoseconds = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t dispatchExisting = 0;
    std::uint64_t dispatchNanoseconds = 0;
    std::uint64_t pipelineWaits = 0;
    std::uint64_t pipelineWaitNanoseconds = 0;
    std::uint64_t surfaceNanoseconds = 0;
    std::uint64_t targetNanoseconds = 0;
    std::uint64_t graphicsPipelineNanoseconds = 0;
    std::uint64_t uploadNanoseconds = 0;
    std::uint64_t knownServed = 0;
    std::uint64_t knownMapped = 0;
    std::uint64_t knownPaged = 0;
    std::uint64_t knownCopied = 0;
    std::uint64_t knownPointers = 0;
    std::uint64_t knownRefused = 0;
    std::uint64_t knownPrefixes = 0;
    std::uint64_t importExtensions = 0;
    std::uint64_t knownSwitches = 0;
    std::uint64_t knownSkipped = 0;
    std::uint64_t knownWritten = 0;
    std::uint64_t knownWriteLookups = 0;
    std::uint64_t knownWriteBytes = 0;
    std::uint64_t copyBacks = 0;
    std::uint64_t copyBackBytes = 0;
    std::uint64_t copyBackCovered = 0;
    std::uint64_t importsReturned = 0;
    std::uint64_t residencyRefreshes = 0;
    std::uint64_t refreshKeptPages = 0;
    std::uint64_t recordWrites = 0;
    std::uint64_t recordTargetDraws = 0;
    std::uint64_t recordSampled = 0;
    std::uint64_t directWrites = 0;
    std::uint64_t writeBacks = 0;
    std::uint64_t writeBackBytes = 0;
    std::uint64_t writeBackRefusals = 0;
    std::uint64_t shapesHandedOver = 0;
    std::uint64_t layerTargets = 0;
    std::uint64_t depthRecordDraws = 0;
    std::uint64_t writeBacksUnserved = 0;
    std::uint64_t depthBufferReads = 0;
    std::uint64_t depthWriteBacks = 0;
    std::uint64_t depthWriteBackRefusals = 0;
    std::uint64_t depthSeeds = 0;
    std::uint64_t depthSeedRefusals = 0;
    std::uint64_t depthHtileClears = 0;
    std::uint64_t depthHtileOther = 0;
    std::uint64_t depthReseeds = 0;
    std::uint64_t depthSupersedeSeeds = 0;
    std::uint64_t depthGpuHtileClears = 0;
    std::uint64_t depthGpuHtileRefusals = 0;
    std::uint64_t depthHtileExpanded = 0;
    std::uint64_t depthHtileExpandRefusals = 0;
    std::uint64_t metadataClears = 0;
    std::uint64_t depthRecordsSampled = 0;
    std::uint64_t recordsRefreshed = 0;
    std::uint64_t layersAbsorbed = 0;
    std::uint64_t layersAbsorbedOverChanges = 0;
    std::uint64_t staleShapes = 0;
    std::uint64_t readerWriteBacks = 0;
    std::uint64_t readerWriteBackBytes = 0;
    std::uint64_t readerWriteBackRefusals = 0;
    std::uint64_t readerStale = 0;
    std::uint64_t writerHandOvers = 0;
    std::uint64_t recordRangesNoted = 0;
    std::uint64_t sampledRecordsCreated = 0;
    std::uint64_t zeroedChains = 0;
    std::uint64_t recordTargetRefusals = 0;
    std::uint64_t indirectDispatches = 0;
    std::uint64_t gdsBindings = 0;
    std::uint64_t knownGpuWritten = 0;
    std::uint64_t inputsResident = 0;
    std::uint64_t inputsImported = 0;
    std::uint64_t inputsWritten = 0;
    std::uint64_t inputsCopied = 0;
    std::uint64_t inputsGpuWritten = 0;
    std::uint64_t inputsPending = 0;
    std::uint64_t inputsMirrored = 0;
    std::uint64_t storageRecordsSeeded = 0;
    std::uint64_t poisonedRecords = 0;
    std::uint64_t inputsAliased = 0;
    std::uint64_t inputsMerged = 0;
    std::map<std::string, std::uint64_t> reasons;
};

std::uint64_t elapsed(std::chrono::steady_clock::time_point from) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - from).count());
}

bool mergesPasses() {
    static const bool merges = std::getenv("APS5_RECODE_NO_PASS_MERGE") == nullptr;
    return merges;
}

bool memoizesTargets() {
    static const bool memoizes = std::getenv("APS5_RECODE_NO_TARGET_MEMO") == nullptr;
    return memoizes;
}

struct TargetMemo {
    bool valid = false;
    std::uint64_t recorder = 0;
    std::uint64_t generation = 0;
    std::uint64_t epoch = 0;
    std::uint64_t proof = 0;
    std::uint64_t pending = 0;
    std::uint64_t depthSerial = 0;
    std::uint64_t depthReleased = 0;
    std::vector<Graphics::ColorTarget> colors;
    std::vector<std::weak_ptr<Graphics::StorageTexture>> residents;
    std::vector<std::uint64_t> versions;
    std::vector<VkImageView> views;
    std::optional<Graphics::DepthTarget> depth;
    bool depthWrite = false;
    bool stencilTest = false;
    bool stencilWritten = false;
    VkImageView depthView = VK_NULL_HANDLE;
};

bool overlaps(std::span<const std::pair<std::uint64_t, std::uint64_t>> left, std::span<const std::pair<std::uint64_t, std::uint64_t>> right) {
    for (const auto& [begin, end] : left) {
        for (const auto& [otherBegin, otherEnd] : right) {
            if (begin < otherEnd && otherBegin < end) return true;
        }
    }
    return false;
}

bool servesFaultPages() {
    static const bool serves = std::getenv("APS5_RECODE_NO_FAULT_SERVE") == nullptr;
    return serves;
}

bool servesKnownPrefixes() {
    static const bool serves = std::getenv("APS5_RECODE_NO_KNOWN_PREFIX") == nullptr;
    return serves;
}


bool copiesBackDispatchWrites() {
    static const bool copies = std::getenv("APS5_RECODE_NO_COPY_BACK") == nullptr;
    return copies;
}

bool copiesBackSmallWrites() {
    static const bool copies = std::getenv("APS5_RECODE_COPY_BACK_SMALL_WRITES") != nullptr;
    return copies;
}

std::mutex shapeNoteMutex;
std::set<std::string> shapeNotePairs;

bool firstShapePair(const Images::SurfaceKey& asked, const Images::SurfaceKey& held) {
    char pair[160];
    std::snprintf(pair, sizeof(pair), "%ux%u/%u/%u/%u/%u-%ux%u/%u/%u/%u/%u", asked.width, asked.height, asked.depth, asked.mips, asked.format, asked.dimension, held.width, held.height, held.depth, held.mips, held.format, held.dimension);
    std::lock_guard lock(shapeNoteMutex);
    if (shapeNotePairs.size() >= 64u) return false;
    return shapeNotePairs.insert(pair).second;
}

constexpr std::uint64_t OutputProbeLimitBytes = 4ull << 20u;
constexpr std::uint32_t OutputProbePrints = 6;

bool probesOutput(std::uint64_t codeAddress) {
    static const char* list = std::getenv("APS5_RECODE_PROBE_OUTPUT");
    return list != nullptr && *list != '\0' && Downgrades::Active() && Downgrades::ListedHash(list, Downgrades::ProgramHash(codeAddress));
}

bool outputProbeDue(std::uint64_t codeAddress) {
    static std::mutex mutex;
    static std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::uint32_t>> seen;
    static const auto started = std::chrono::steady_clock::now();
    static const std::uint64_t after = [] {
        const char* value = std::getenv("APS5_RECODE_PROBE_AFTER");
        return value != nullptr ? static_cast<std::uint64_t>(std::max(std::atoi(value), 0)) : std::uint64_t{80};
    }();
    if (static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count()) < after) return false;
    std::lock_guard lock(mutex);
    auto& entry = seen[codeAddress];
    if (entry.second >= OutputProbePrints) return false;
    ++entry.second;
    return true;
}

void printOutput(std::uint64_t codeAddress, const char* route, std::uint64_t address, std::span<const std::byte> bytes) {
    std::uint64_t nonZero = 0;
    std::uint64_t sum = 0;
    std::uint32_t orAll = 0;
    std::array<std::uint64_t, 16> bits{};
    const auto words = bytes.size() / sizeof(std::uint32_t);
    for (std::size_t index = 0; index < words; ++index) {
        std::uint32_t word = 0;
        std::memcpy(&word, bytes.data() + index * sizeof(word), sizeof(word));
        if (word == 0u) continue;
        ++nonZero;
        sum += word;
        orAll |= word;
        for (std::uint32_t bit = 0; bit < bits.size(); ++bit) bits[bit] += (word >> bit) & 1u;
    }
    std::string line;
    char text[160];
    std::snprintf(text, sizeof(text), "[recode-output] %016llx %s 0x%llx+0x%llx: %llu of %llu words non-zero, sum 0x%llx, or 0x%08x, bits", static_cast<unsigned long long>(Downgrades::ProgramHash(codeAddress)), route, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes.size()), static_cast<unsigned long long>(nonZero), static_cast<unsigned long long>(words), static_cast<unsigned long long>(sum), orAll);
    line = text;
    for (const auto count : bits) line += " " + std::to_string(count);
    std::fprintf(stderr, "%s\n", line.c_str());
}
std::atomic<std::uint64_t> recordedFills{0};
std::atomic<std::uint64_t> recordedCopies{0};

std::atomic<std::uint64_t> flipsFromNewRecords{0};
std::atomic<std::uint64_t> flipsFromOldRecords{0};
std::atomic<std::uint64_t> flipsWithoutRecords{0};
std::atomic<std::uint32_t> flipNotes{0};
std::atomic<std::uint32_t> flipMemoryNotes{0};
std::atomic<std::uint64_t> sampleDumpsLanded{0};
std::atomic<std::uint64_t> storesToMirror{0};
std::atomic<std::uint64_t> storesBesideMirror{0};
std::atomic<std::uint64_t> storesAtOnce{0};
std::atomic<std::uint64_t> copiesInMirror{0};
std::atomic<std::uint64_t> copiesBesideMirror{0};
std::atomic<std::uint64_t> storesThroughImports{0};
std::atomic<std::uint64_t> storesRewrittenByCpu{0};std::atomic<std::uint64_t> storeLandings{0};
std::atomic<std::uint64_t> storeLandingNanoseconds{0};
std::atomic<std::uint64_t> storeLandingMaxNanoseconds{0};

bool argumentCensusOn() {
    static const bool on = std::getenv("APS5_RECODE_ARGUMENT_CENSUS") != nullptr;
    return on;
}

struct ArgumentTally {
    std::uint64_t draws = 0;
    std::uint64_t noCount = 0;
    std::uint64_t noElements = 0;
    std::uint64_t noInstances = 0;
    std::uint64_t elements = 0;
};

std::mutex existingPacketMutex;
std::map<std::uint64_t, std::uint64_t> existingPackets;

void printExistingPackets() {
    std::lock_guard lock(existingPacketMutex);
    if (existingPackets.empty()) return;
    std::string line = "[recode-packets] packets through the existing packet loop since the last line:";
    for (const auto& [key, count] : existingPackets) {
        char text[64];
        std::snprintf(text, sizeof(text), " q%llx:0x%02llx %llu", static_cast<unsigned long long>(key >> 32u), static_cast<unsigned long long>((key >> 8u) & 0xffu), static_cast<unsigned long long>(count));
        line += text;
    }
    char totals[320];
    std::snprintf(totals, sizeof(totals), "; recode stores into the mirror %llu, outside it %llu, at once (nothing in flight) %llu; copies in the mirror %llu, outside it %llu; occlusion dumps %llu (whole run)", static_cast<unsigned long long>(storesToMirror.load()), static_cast<unsigned long long>(storesBesideMirror.load()), static_cast<unsigned long long>(storesAtOnce.load()), static_cast<unsigned long long>(copiesInMirror.load()), static_cast<unsigned long long>(copiesBesideMirror.load()), static_cast<unsigned long long>(sampleDumpsLanded.load()));
    line += totals;
    line += "; " + LabelImports::Line() + " (" + std::to_string(storesThroughImports.load()) + " stores through them, " + std::to_string(storesRewrittenByCpu.load()) + " rewritten by the CPU before their batch landed)";
    const auto landed = storeLandings.exchange(0);
    const auto landedNanoseconds = storeLandingNanoseconds.exchange(0);
    const auto landedMax = storeLandingMaxNanoseconds.exchange(0);
    if (landed != 0u) {
        char delay[160];
        std::snprintf(delay, sizeof(delay), "; store landings since the last line %llu, %.1f us average and %.1f ms longest from recording to landing", static_cast<unsigned long long>(landed), static_cast<double>(landedNanoseconds) / static_cast<double>(landed) / 1e3, static_cast<double>(landedMax) / 1e6);
        line += delay;
    }
    std::fprintf(stderr, "%s\n", line.c_str());
    existingPackets.clear();
}

std::mutex argumentCensusMutex;
std::map<std::uint64_t, ArgumentTally> argumentCensus;

void noteArgumentCensus(std::uint64_t codeAddress, std::uint32_t elements, std::uint32_t instances, std::uint32_t count) {
    std::lock_guard lock(argumentCensusMutex);
    auto& tally = argumentCensus[codeAddress];
    tally.draws++;
    if (count == 0u) tally.noCount++;
    else if (elements == 0u) tally.noElements++;
    else if (instances == 0u) tally.noInstances++;
    else tally.elements += static_cast<std::uint64_t>(elements) * instances;
}

void printArgumentCensus() {
    if (!argumentCensusOn()) return;
    std::lock_guard lock(argumentCensusMutex);
    for (const auto& [code, tally] : argumentCensus) std::fprintf(stderr, "[recode-argument-census] %016llx indirect draws %llu: no count %llu, no elements %llu, no instances %llu, drawn elements x instances %llu\n", static_cast<unsigned long long>(Downgrades::ProgramHash(code)), static_cast<unsigned long long>(tally.draws), static_cast<unsigned long long>(tally.noCount), static_cast<unsigned long long>(tally.noElements), static_cast<unsigned long long>(tally.noInstances), static_cast<unsigned long long>(tally.elements));
    argumentCensus.clear();
}
std::atomic<std::uint64_t> flipsPresentedFromRecords{0};

struct BatchLandings {
    std::atomic<std::uint64_t> pending{0};
    std::atomic<std::uint64_t> scheduled{0};
    std::atomic<std::uint64_t> ran{0};
    std::atomic<std::uint64_t> landedBytes{0};
    std::atomic<std::uint64_t> retried{0};
    std::mutex retryMutex;
    std::vector<KnownWrite> retry;
};

BatchLandings& batchLandings() {
    static auto& landings = *new BatchLandings();
    return landings;
}

bool landsInCompletions() {
    static const bool lands = [] {
        const char* value = std::getenv("APS5_RECODE_BATCH_LANDINGS");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return lands && Mirror::WritesEnabled() && ResidencyService::Get().ReadsOffGpuMutex();
}

void landInCompletion(const std::vector<KnownWrite>& writes) {
    auto& landings = batchLandings();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(writes.size());
    for (const auto& written : writes) ranges.emplace_back(written.address, written.bytes);
    const auto pieces = ResidencyService::Get().ReadBack(ranges);
    auto& core = MemoryCore::Get();
    for (const auto& written : writes) {
        std::vector<std::byte> after(static_cast<std::size_t>(written.bytes));
        std::uint64_t covered = 0;
        for (const auto& piece : pieces) {
            const auto begin = std::max(piece.guest, written.address);
            const auto end = std::min(piece.guest + piece.bytes.size(), written.address + written.bytes);
            if (begin >= end) continue;
            std::memcpy(after.data() + (begin - written.address), piece.bytes.data() + (begin - piece.guest), static_cast<std::size_t>(end - begin));
            covered += end - begin;
        }
        if (covered < written.bytes) {
            std::lock_guard lock(landings.retryMutex);
            landings.retry.push_back(written);
            landings.retried.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        core.LandDiff(written.address, *written.before, after, written.serial);
        landings.landedBytes.fetch_add(written.bytes, std::memory_order_relaxed);
    }
    core.LandAhead();
    landings.ran.fetch_add(1, std::memory_order_relaxed);
}

bool ownsImages() {
    return Images::OwnersEnabled();
}

bool ownsTargets() {
    static const bool owns = Images::OwnsClass("targets");
    return owns;
}

bool ownsStorage() {
    static const bool owns = Images::OwnsClass("storage");
    return owns;
}

bool recordsTargets() {
    static const bool records = std::getenv("APS5_RECODE_TARGET_RECORDS") != nullptr || ownsImages();
    return records;
}

bool refreshesBeforeCopyBack() {
    static const bool refreshes = std::getenv("APS5_RECODE_NO_COPY_BACK_REFRESH") == nullptr;
    return refreshes;
}

bool keepsGpuWrittenPages() {
    static const bool keeps = std::getenv("APS5_RECODE_REFRESH_GPU_WRITTEN") == nullptr;
    return keeps;
}

bool bindsGds() {
    static const bool binds = std::getenv("APS5_RECODE_NO_GDS") == nullptr;
    return binds;
}

bool notesRecordRanges() {
    static const bool notes = std::getenv("APS5_RECODE_NOTE_RECORD_RANGES") != nullptr;
    return notes;
}

bool depthHandOverForWriters() {
    static const bool hands = std::getenv("APS5_RECODE_NO_DEPTH_WRITER_HANDOVER") == nullptr;
    return hands;
}

bool handsOverForWriters() {
    static const bool hands = std::getenv("APS5_RECODE_WRITER_HANDOVER") == nullptr || std::string_view(std::getenv("APS5_RECODE_WRITER_HANDOVER")) != "0";
    return hands;
}

struct RecordDumpRequest {
    std::uint32_t width = 0;
    std::int64_t seconds = 0;
    std::string directory;
    bool trace = false;
    std::string program;
    std::uint32_t every = 1;
};

std::chrono::steady_clock::time_point processStart() {
    static const auto start = std::chrono::steady_clock::now();
    return start;
}

const RecordDumpRequest& recordDumpRequest() {
    static const RecordDumpRequest request = [] {
        RecordDumpRequest parsed;
        const char* value = std::getenv("APS5_RECODE_DUMP_RECORDS");
        const char* directory = std::getenv("APS5_RECODE_DUMP_DIR");
        if (value == nullptr || directory == nullptr) return parsed;
        char* end = nullptr;
        parsed.width = static_cast<std::uint32_t>(std::strtoul(value, &end, 10));
        if (end != nullptr && *end == ':') parsed.seconds = std::strtoll(end + 1, nullptr, 10);
        parsed.directory = directory;
        parsed.trace = std::getenv("APS5_RECODE_DUMP_DRAWS") != nullptr;
        if (const char* program = std::getenv("APS5_RECODE_DUMP_PROGRAM")) parsed.program = program;
        if (const char* every = std::getenv("APS5_RECODE_DUMP_EVERY")) parsed.every = std::max<std::uint32_t>(static_cast<std::uint32_t>(std::strtoul(every, nullptr, 10)), 1u);
        return parsed;
    }();
    return request;
}

struct NanCensus {
    std::int64_t seconds = 0;
    std::uint32_t draws = 0;
};

struct CensusDump {
    std::string directory;
    std::uint32_t count = 0;
};

const CensusDump& censusDump() {
    static const CensusDump dump = [] {
        CensusDump parsed;
        const char* count = std::getenv("APS5_RECODE_NAN_CENSUS_DUMP");
        const char* directory = std::getenv("APS5_RECODE_DUMP_DIR");
        if (count == nullptr || directory == nullptr) return parsed;
        parsed.count = static_cast<std::uint32_t>(std::strtoul(count, nullptr, 10));
        parsed.directory = directory;
        return parsed;
    }();
    return dump;
}

const std::string& targetTraceList() {
    static const std::string list = [] {
        const char* value = std::getenv("APS5_RECODE_TRACE_TARGETS");
        return std::string(value == nullptr ? "" : value);
    }();
    return list;
}

std::atomic<std::uint32_t> targetTraceLines{0};

std::uint64_t targetTraceAddress() {
    static const std::uint64_t address = [] {
        const char* value = std::getenv("APS5_RECODE_TRACE_TARGET_ADDRESS");
        return value == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(std::strtoull(value, nullptr, 16));
    }();
    return address;
}

bool targetDetailOn() {
    static const bool on = std::getenv("APS5_RECODE_TRACE_TARGET_DETAIL") != nullptr;
    return on;
}

std::atomic<std::uint32_t> targetDetailLines{0};
std::atomic<std::uint32_t> meshTraceLines{0};
std::atomic<std::uint32_t> layeredTraceLines{0};
std::atomic<std::uint32_t> inputRefusalLines{0};

bool meshDrawsSkipped() {
    static const bool skipped = std::getenv("APS5_RECODE_MESH_SKIP_DRAW") != nullptr;
    return skipped;
}

std::uint64_t sourcesOfProgram() {
    static const std::uint64_t program = [] {
        const char* value = std::getenv("APS5_RECODE_TRACE_SOURCES_OF");
        return value == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(std::strtoull(value, nullptr, 16));
    }();
    return program;
}

std::mutex sourcesMutex;
std::vector<std::pair<std::uint64_t, std::uint64_t>> sourceRecords;
std::atomic<std::uint32_t> sourceTargetLines{0};

void noteSampledSource(std::uint64_t program, std::uint64_t record, std::uint64_t bytes) {
    if (sourcesOfProgram() == 0 || program != sourcesOfProgram()) return;
    std::lock_guard lock(sourcesMutex);
    if (std::ranges::find(sourceRecords, std::pair{record, bytes}) == sourceRecords.end() && sourceRecords.size() < 64u) {
        sourceRecords.emplace_back(record, bytes);
        std::fprintf(stderr, "[recode-source] program %016llx samples record 0x%llx+0x%llx\n", static_cast<unsigned long long>(program), static_cast<unsigned long long>(record), static_cast<unsigned long long>(bytes));
    }
}

bool sourceTarget(std::uint64_t record, std::uint64_t bytes) {
    if (sourcesOfProgram() == 0) return false;
    std::lock_guard lock(sourcesMutex);
    return std::ranges::any_of(sourceRecords, [&](const auto& source) { return record < source.first + source.second && source.first < record + bytes; }) && sourceTargetLines.fetch_add(1, std::memory_order_relaxed) < 4000u;
}

bool traceTargetRecord(std::uint64_t address, std::uint64_t bytes) {
    if (sourcesOfProgram() != 0) return sourceTarget(address, bytes);
    const auto wanted = targetTraceAddress();
    if (wanted == 0) return targetTraceLines.fetch_add(1, std::memory_order_relaxed) < (targetTraceList() == "all" ? 20000u : 400u);
    return wanted >= address && wanted < address + bytes;
}

bool tracedProgram(std::uint64_t hash) {
    return targetTraceList() == "all" || Downgrades::ListedHash(targetTraceList(), hash);
}

const std::vector<std::pair<std::uint32_t, std::uint32_t>>& censusSizes() {
    static const std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes = [] {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> parsed;
        const char* value = std::getenv("APS5_RECODE_NAN_CENSUS_SIZE");
        while (value != nullptr && *value != 0) {
            char* end = nullptr;
            const auto width = static_cast<std::uint32_t>(std::strtoul(value, &end, 10));
            if (end == nullptr || *end != 0x78) break;
            const auto height = static_cast<std::uint32_t>(std::strtoul(end + 1, &end, 10));
            parsed.emplace_back(width, height);
            if (end == nullptr || *end != 0x2c) break;
            value = end + 1;
        }
        return parsed;
    }();
    return sizes;
}

const std::string& censusReads() {
    static const std::string list = [] {
        const char* value = std::getenv("APS5_RECODE_NAN_CENSUS_READS");
        return std::string(value == nullptr ? "" : value);
    }();
    return list;
}

bool censusReadsOf(std::uint64_t program) {
    return !censusReads().empty() && (censusReads() == "all" || Downgrades::ListedHash(censusReads(), program));
}

const NanCensus& nanCensus() {
    static const NanCensus census = [] {
        NanCensus parsed;
        const char* value = std::getenv("APS5_RECODE_NAN_CENSUS");
        if (value == nullptr) return parsed;
        char* end = nullptr;
        parsed.seconds = std::strtoll(value, &end, 10);
        parsed.draws = end != nullptr && *end == ':' ? static_cast<std::uint32_t>(std::strtoul(end + 1, nullptr, 10)) : 200u;
        return parsed;
    }();
    return census;
}

bool refreshesRecords() {
    static const bool refreshes = [] {
        const char* value = std::getenv("APS5_RECODE_RECORD_REFRESH");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return refreshes;
}

bool presentsRecords() {
    static const bool presents = [] {
        const char* value = std::getenv("APS5_RECODE_PRESENT_RECORDS");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return presents;
}

bool seedsStorageRecords() {
    static const bool seeds = [] {
        const char* value = std::getenv("APS5_RECODE_SEED_STORAGE");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return seeds;
}

bool zeroesNewChains() {
    static const bool zeroes = std::getenv("APS5_RECODE_ZERO_NEW_CHAINS") != nullptr;
    return zeroes;
}

std::optional<VkClearColorValue> poisonValue() {
    static const auto value = []() -> std::optional<VkClearColorValue> {
        const char* text = std::getenv("APS5_RECODE_POISON_NEW");
        if (text == nullptr) return std::nullopt;
        const std::string_view name(text);
        std::uint32_t word = 0;
        if (name == "nan") word = 0x7fc00000u;
        else if (name == "big") word = 0x47000000u;
        else if (name == "one") word = 0x3f800000u;
        else if (name != "zero") return std::nullopt;
        VkClearColorValue clear{};
        for (auto& channel : clear.uint32) channel = word;
        return clear;
    }();
    return value;
}

bool writesBackDepth() {
    static const bool writes = [] {
        const char* value = std::getenv("APS5_RECODE_DEPTH_WRITE_BACK");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return writes;
}

bool seedsDepth() {
    static const bool seeds = [] {
        const char* value = std::getenv("APS5_RECODE_DEPTH_SEED");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return seeds;
}

bool zeroesAllNewTargets() {
    static const bool all = [] {
        const char* value = std::getenv("APS5_RECODE_ZERO_NEW_CHAINS");
        return value != nullptr && std::string_view(value) == "all";
    }();
    return all;
}

bool dumpsRecords() {
    return recordDumpRequest().width != 0u;
}

bool recordsMipChains() {
    static const bool records = [] {
        const char* value = std::getenv("APS5_RECODE_MIP_CHAIN_RECORDS");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return records;
}

std::uint32_t mipChainMinWidth() {
    static const std::uint32_t width = [] {
        const char* value = std::getenv("APS5_RECODE_MIP_CHAIN_RECORDS");
        if (value == nullptr || std::string_view(value).rfind("min:", 0) != 0) return 0u;
        return static_cast<std::uint32_t>(std::strtoul(value + 4, nullptr, 10));
    }();
    return width;
}

bool countsSamples() {
    static const bool counts = std::getenv("APS5_RECODE_NO_SAMPLE_COUNTS") == nullptr;
    return counts;
}

bool recordsLayeredTargets() {
    static const bool records = std::getenv("APS5_RECODE_NO_LAYERED_TARGETS") == nullptr;
    return records;
}

bool recordsMipTailTargets() {
    static const bool records = std::getenv("APS5_RECODE_NO_MIP_TAIL_TARGETS") == nullptr;
    return records;
}

bool marksDispatchWrites() {
    static const bool marks = std::getenv("APS5_RECODE_NO_DISPATCH_WRITTEN_PAGES") == nullptr;
    return marks;
}

bool buildsPipelinesAhead() {
    static const bool ahead = std::getenv("APS5_RECODE_ASYNC_COMPUTE_PIPELINES") != nullptr;
    return ahead;
}

bool recordsIndirectDispatches() {
    static const bool records = std::getenv("APS5_RECODE_NO_INDIRECT_DISPATCHES") == nullptr;
    return records;
}

bool refusesGpuWrittenCopies() {
    static const bool refuses = std::getenv("APS5_RECODE_COPY_GPU_WRITTEN_KNOWN") == nullptr;
    return refuses;
}

struct DrawInput {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
};

enum class InputSource : std::uint8_t { Imported, Resident, Written, GpuWritten, Pending, Mirrored, Aliased };

std::optional<DrawInput> importedInput(const Context& context, std::uint64_t address, std::uint64_t bytes) {
    const auto* import = Graphics::HostImportFor(context, address, static_cast<std::size_t>(bytes));
    if (import == nullptr) return std::nullopt;
    return DrawInput{import->buffer, Graphics::HostImportOffset(*import, address)};
}

bool mirrorMode() {
    return Mirror::WritesEnabled();
}

bool mirrorPart(const char* part) {
    static const char* const chosen = std::getenv("APS5_RECODE_MIRROR_PARTS");
    static const char* const parts = chosen != nullptr ? chosen : "writes,args";
    return mirrorMode() && std::strstr(parts, part) != nullptr;
}

bool argumentsFromMirror() {
    static const bool fromMirror = std::getenv("APS5_RECODE_ARGS_SNAPSHOT") == nullptr;
    return fromMirror;
}

bool gpuPending(std::uint64_t address, std::uint64_t bytes) {
    return mirrorMode() && MemoryCore::Get().Written().Any(address, bytes);
}

bool pendingInputsFromMirror() {
    static const bool enabled = std::getenv("APS5_RECODE_PENDING_INPUTS") == nullptr || std::string_view(std::getenv("APS5_RECODE_PENDING_INPUTS")) != "0";
    return enabled && mirrorMode();
}

std::optional<DrawInput> mirrorInput(std::uint64_t address, std::uint64_t bytes) {
    static const std::uint64_t margin = [] {
        const char* value = std::getenv("APS5_RECODE_MIRROR_INPUTS_MARGIN");
        return value != nullptr ? std::strtoull(value, nullptr, 16) : 0ull;
    }();
    if (margin != 0u) static_cast<void>(ResidencyService::Get().MirrorSpan(address, bytes + margin));
    const auto span = ResidencyService::Get().MirrorSpan(address, bytes);
    if (!span) return std::nullopt;
    return DrawInput{reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span->handle)), static_cast<VkDeviceSize>(span->offset)};
}

constexpr std::uint64_t InputCopyLimitBytes = 64ull << 10u;

bool copiesInputs() {
    static const bool copies = std::getenv("APS5_RECODE_NO_INPUT_COPIES") == nullptr;
    return copies;
}

std::atomic<std::uint64_t>& inputConsumes() {
    static std::atomic<std::uint64_t> count{0};
    return count;
}

bool shadowInputs() {
    static const bool shadow = [] {
        const char* value = RouteValue("APS5_RECODE_MIRROR_INPUTS");
        return value == nullptr || std::string_view(value) == "0";
    }();
    return shadow;
}

std::optional<DrawInput> drawInput(const Context& context, std::uint64_t address, std::uint64_t bytes, InputSource& source) {
    source = InputSource::Imported;
    auto& pages = MemoryCore::Get().Pages();
    static const bool landedFromImports = std::getenv("APS5_RECODE_MIRROR_INPUTS_LANDED_IMPORTS") != nullptr;
    const bool landedGpu = landedFromImports && !gpuPending(address, bytes) && pages.GpuOwnedIn(address, bytes).empty() && pages.GpuWrittenIn(address, bytes);
    static const std::pair<std::uint64_t, std::uint64_t> oldRange = [] {
        const char* value = std::getenv("APS5_RECODE_MIRROR_INPUTS_OLD_RANGE");
        if (value == nullptr) return std::pair<std::uint64_t, std::uint64_t>{0u, 0u};
        const char* dash = std::strchr(value, '-');
        return std::pair<std::uint64_t, std::uint64_t>{std::strtoull(value, nullptr, 16), dash != nullptr ? std::strtoull(dash + 1, nullptr, 16) : 0u};
    }();
    const bool inOldRange = address >= oldRange.first && address < oldRange.second;
    if (!shadowInputs() && mirrorMode() && !landedGpu && !inOldRange && !DirectMemoryAliased_nid_postfix(static_cast<std::uintptr_t>(address), static_cast<std::size_t>(bytes))) {
        static const bool servesOwned = std::getenv("APS5_RECODE_MIRROR_INPUTS_SERVE_OWNED") == nullptr || std::string_view(std::getenv("APS5_RECODE_MIRROR_INPUTS_SERVE_OWNED")) != "0";
        if (servesOwned) {
            RouteMemory::OwnedSpan span;
            static_cast<void>(RouteMemory::ServeOwned(context, address, bytes, &span));
        }
        if (auto input = mirrorInput(address, bytes)) {
            const bool gpu = pages.GpuWrittenIn(address, bytes) || !pages.GpuOwnedIn(address, bytes).empty() || gpuPending(address, bytes);
            static const bool consumes = std::getenv("APS5_RECODE_NO_INPUT_CONSUME") == nullptr;
            static const bool globalConsume = std::getenv("APS5_RECODE_INPUT_CONSUME") != nullptr && std::string_view(std::getenv("APS5_RECODE_INPUT_CONSUME")) == "global";
            static const bool copiesSmall = std::getenv("APS5_RECODE_MIRROR_INPUTS_COPY_SMALL") != nullptr;
            if (consumes && !gpu && (copiesSmall || !pages.CleanIn(address, bytes)) && copiesInputs() && bytes <= InputCopyLimitBytes && !globalConsume) {
                source = InputSource::Written;
                return input;
            }
            if (consumes && !gpu && !pages.CleanIn(address, bytes)) {
                if (globalConsume) {
                    RouteMemory::ConsumeNow();
                } else {
                    const std::array range{std::pair<std::uint64_t, std::uint64_t>{address, bytes}};
                    static_cast<void>(RouteMemory::ConsumeIn(range));
                }
                inputConsumes().fetch_add(1, std::memory_order_relaxed);
            }
            source = InputSource::Mirrored;
            return input;
        }
    }
    if (!RouteMemory::ResidentDrawInputs()) return importedInput(context, address, bytes);
    if (pages.GpuWrittenIn(address, bytes) || !pages.GpuOwnedIn(address, bytes).empty() || gpuPending(address, bytes)) {
        source = InputSource::GpuWritten;
        if (pendingInputsFromMirror() && (gpuPending(address, bytes) || !pages.GpuOwnedIn(address, bytes).empty())) {
            if (auto input = mirrorInput(address, bytes)) {
                source = InputSource::Pending;
                return input;
            }
        }
    } else if (DirectMemoryAliased_nid_postfix(static_cast<std::uintptr_t>(address), static_cast<std::size_t>(bytes))) {
        source = InputSource::Aliased;
    }
    std::optional<DrawInput> written;
    if (source == InputSource::Imported) {
        RouteMemory::OwnedSpan span;
        if (RouteMemory::ServeOwned(context, address, bytes, &span) && span.handle != 0u) {
            const DrawInput resident{reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span.handle)), static_cast<VkDeviceSize>(address - span.origin)};
            if (pages.CleanIn(address, bytes)) {
                source = InputSource::Resident;
                return resident;
            }
            source = InputSource::Written;
            if (copiesInputs() && bytes <= InputCopyLimitBytes) written = resident;
        }
    }
    if (mirrorPart("inputs") && source != InputSource::Aliased) {
        if (auto input = mirrorInput(address, bytes)) return input;
        if (gpuPending(address, bytes)) return std::nullopt;
        source = InputSource::Imported;
    }
    auto imported = importedInput(context, address, bytes);
    if (!imported && source == InputSource::GpuWritten && mirrorMode()) {
        if (auto input = mirrorInput(address, bytes)) {
            source = InputSource::Mirrored;
            return input;
        }
    }
    if (!imported && written) {
        source = InputSource::Written;
        return written;
    }
    if (!imported && source == InputSource::Written && mirrorMode()) {
        if (auto input = mirrorInput(address, bytes)) {
            source = InputSource::Mirrored;
            return input;
        }
    }
    return imported;
}

struct InputRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::size_t merged = 0;
};

std::uint64_t unclampedInputEnd(const ShaderRecompiler::VertexAttribute& attribute, std::uint64_t address, std::uint64_t end) {
    constexpr std::uint64_t Cap = 64ull << 20u;
    static const bool extends = std::getenv("APS5_RECODE_NO_UNCLAMPED_INPUT_RANGES") == nullptr;
    if (!extends || shadowInputs() || !mirrorMode() || Graphics::NullVertexDescriptor(attribute)) return end;
    const auto stride = (attribute.resource.fields[1] >> 16u) & 0x3fffu;
    if (stride == 0u || Graphics::VertexBufferOutOfBoundsSelect(attribute) > 1u) return end;
    const auto found = GuestAllocations::GuestAllocationsLookup_nid_postfix(static_cast<std::uintptr_t>(address), false);
    if (!found || found->bytes == 0u || found->address > address) return end;
    return std::max(end, std::min<std::uint64_t>(found->address + found->bytes, address + Cap));
}

bool mergesInputs() {
    static const bool merges = std::getenv("APS5_RECODE_NO_INPUT_MERGE") == nullptr;
    return merges;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> mergeInputs(std::vector<InputRange>& ranges) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> unions;
    if (!mergesInputs()) {
        for (auto& range : ranges) {
            range.merged = unions.size();
            unions.emplace_back(range.begin, range.end);
        }
        return unions;
    }
    std::vector<std::size_t> order(ranges.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::sort(order, {}, [&](std::size_t index) { return ranges[index].begin; });
    for (const auto index : order) {
        auto& range = ranges[index];
        if (!unions.empty() && range.begin < unions.back().second) unions.back().second = std::max(unions.back().second, range.end);
        else unions.emplace_back(range.begin, range.end);
        range.merged = unions.size() - 1;
    }
    return unions;
}

void countInput(Totals& totals, InputSource source) {
    switch (source) {
        case InputSource::Resident: ++totals.inputsResident; break;
        case InputSource::Written: ++totals.inputsWritten; break;
        case InputSource::GpuWritten: ++totals.inputsGpuWritten; break;
        case InputSource::Pending: ++totals.inputsPending; break;
        case InputSource::Mirrored: ++totals.inputsMirrored; break;
        case InputSource::Aliased: ++totals.inputsAliased; break;
        case InputSource::Imported: ++totals.inputsImported; break;
    }
}

constexpr std::uint64_t KnownBufferLimitBytes = 1024ull << 20u;
constexpr std::uint64_t KnownCopyLimitBytes = 64ull << 10u;
constexpr std::uint64_t LargeKnownBytes = 16ull << 20u;
constexpr std::uint64_t KnownWriteLimitBytes = 1ull << 20u;
constexpr std::size_t ExtentsShown = 3;
std::uint32_t coldSmallWrites() {
    static const std::uint32_t limit = [] {
        const char* value = std::getenv("APS5_RECODE_DIRECT_SMALL_WRITES");
        if (value == nullptr) return 4u;
        if (std::string_view(value) == "all") return 0xffffffffu;
        return static_cast<std::uint32_t>(std::max(std::atoi(value), 0));
    }();
    return limit;
}
constexpr std::uint32_t PagesPerChunk = static_cast<std::uint32_t>(ChunkBytes / PageBytes);

bool shaderDataBinding(const Resolve::ResolvedStage& stage, std::uint32_t index) {
    const auto& bindings = stage.program.result->bindings;
    return index < bindings.size() && bindings[index].role == ShaderRecompiler::DescriptorRole::ShaderData;
}

bool writesStencil(const Graphics::State& state) {
    const auto keeps = [](const VkStencilOpState& face) {
        return face.writeMask == 0u || (face.failOp == VK_STENCIL_OP_KEEP && face.passOp == VK_STENCIL_OP_KEEP && face.depthFailOp == VK_STENCIL_OP_KEEP);
    };
    return state.stencilTest && (!keeps(state.stencilFront) || !keeps(state.stencilBack));
}

std::optional<Graphics::State> maskedOutputs(const Graphics::State& state, std::uint32_t outputs) {
    std::optional<Graphics::State> masked;
    for (std::size_t index = 0; index < state.blends.size(); ++index) {
        if ((index < 32u && ((outputs >> index) & 1u) != 0u) || state.blends[index].colorWriteMask == 0u) continue;
        if (!masked.has_value()) masked = state;
        masked->blends[index].colorWriteMask = 0;
    }
    return masked;
}

std::uint64_t vertexAddress(const ShaderRecompiler::VertexAttribute& attribute) {
    const auto& fields = attribute.resource.fields;
    return fields[0] | (static_cast<std::uint64_t>(fields[1] & 0xffffu) << 32u);
}

bool precheck(const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::vector<std::pair<std::uint64_t, std::uint64_t>>* reads, std::string& reason) {
    if (draw.state == nullptr || draw.stages.empty()) {
        reason = "no state or stages";
        return false;
    }
    if (!draw.complete) {
        reason = "placeholder images";
        return false;
    }
    const auto& state = *draw.state;
    if (state.stages.tessellation || state.tessellationCapture != nullptr) {
        reason = "tessellation";
        return false;
    }
    if (state.stages.mesh) {
        const auto& mesh = *state.stages.mesh;
        if (parameters.indirect) {
            reason = "mesh: indirect draw";
            return false;
        }
        if (mesh.primitivesPerGroup == 0u) {
            reason = "mesh: no primitives per group";
            return false;
        }
        if (parameters.indexed && ((state.stages.registerValue >> 19u) & 3u) != 0u) {
            reason = "mesh: indexed fast launch";
            return false;
        }
        if (parameters.indexed && state.primitiveRestart && (mesh.inputPrimitive == 5u || mesh.inputPrimitive == 6u)) {
            reason = "mesh: strip with primitive restart";
            return false;
        }
        if (std::ranges::any_of(draw.stages, [](const Resolve::ResolvedStage& stage) { return stage.program.result->barycentricEmulation.active; })) {
            reason = "mesh: emulated barycentrics";
            return false;
        }
    }
    for (const auto& stage : draw.stages) {
        if (stage.program.result->readsDepthRemapPush) {
            reason = "depth remap push";
            return false;
        }
    }
    for (const auto& color : state.colors) {
        if (color.samples > 1u) {
            reason = "multisampled color target";
            return false;
        }
        if (color.mipTail && !recordsMipTailTargets()) {
            reason = "color target in a mip tail";
            return false;
        }
        if (color.layers == 0u || (color.layers != 1u && !recordsLayeredTargets())) {
            reason = color.layers == 0u ? "colour target without layers" : "layered color target";
            return false;
        }
    }
    if (state.depth && state.depth->samples > 1u) {
        reason = "multisampled depth";
        return false;
    }
    for (const auto& binding : draw.layout.bindings) {
        if (binding.count != 1u) {
            reason = "buffer array binding";
            return false;
        }
        if (binding.source == Bindings::Source::Guest || (binding.source == Bindings::Source::Gds && !bindsGds())) {
            reason = binding.source == Bindings::Source::Gds ? "GDS buffer binding" : "guest buffer binding";
            return false;
        }
        if (binding.source == Bindings::Source::TableMap && draw.stages.at(binding.stage).tableMap == nullptr) {
            reason = "table map without a mirror";
            return false;
        }
    }
    for (const auto& attribute : draw.stages.front().program.vertexAttributes) {
        if (Graphics::NullVertexDescriptor(attribute) || Graphics::VertexFetchOutOfRange(attribute)) {
            reason = "null or out-of-range vertex descriptor";
            return false;
        }
        if (attribute.fetchIndex > 1u) {
            reason = "vertex fetch index";
            return false;
        }
        const auto address = vertexAddress(attribute);
        if (reads != nullptr) reads->emplace_back(address, address + Graphics::VertexBufferExtent(attribute));
    }
    if (parameters.indexed && parameters.indexSize != 2u && parameters.indexSize != 4u) {
        reason = "index size";
        return false;
    }
    if (parameters.indirect) {
        const auto& args = *parameters.indirect;
        if (args.drawIndexEnabled || args.baseVertexSgpr >= 0 || args.startInstanceSgpr >= 0 || args.drawIndexSgpr >= 0) {
            reason = "indirect draw feeds user data";
            return false;
        }
        if (args.RangeBytes() == 0u) {
            reason = "empty indirect draw";
            return false;
        }
    }
    return true;
}

struct ComputePipeline {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkDestroyPipelineLayout destroyLayout = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineCreationFeedbackFlags feedback = 0;
    std::uint64_t lockNanoseconds = 0;

    ComputePipeline() = default;
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ~ComputePipeline() {
        if (pipeline != VK_NULL_HANDLE) destroyPipeline(device, pipeline, nullptr);
        if (layout != VK_NULL_HANDLE) destroyLayout(device, layout, nullptr);
    }
};

struct PipelineJob {
    std::vector<std::uint64_t> key;
    Graphics::Context context;
    std::shared_ptr<const ShaderRecompiler::RecompileResult> result;
    VkDescriptorSetLayout main = VK_NULL_HANDLE;
    VkDescriptorSetLayout heap = VK_NULL_HANDLE;
    VkDescriptorSetLayout empty = VK_NULL_HANDLE;
    VkPipelineCreateFlags flags = 0;
    VkPipelineShaderStageCreateFlags stageFlags = 0;
    std::uint32_t subgroupSize = 0;
    VulkanDevice* owner = nullptr;
    std::uint64_t codeAddress = 0;
    std::uint64_t spirvBytes = 0;
    bool listed = false;
};

constexpr std::uint64_t UnsavedBuildNanoseconds = 5'000'000'000u;

bool dispatchPrecheck(const Resolve::ResolvedDraw& dispatch, std::string& reason) {
    if (dispatch.stages.size() != 1u || dispatch.stages.front().program.stage != ShaderRecompiler::ShaderStage::Compute) {
        reason = "not one compute stage";
        return false;
    }
    if (!dispatch.complete) {
        reason = "placeholder images";
        return false;
    }
    if (dispatch.operation.dispatch.indirect && (dispatch.operation.dispatch.indirectAddress == 0u || !recordsIndirectDispatches())) {
        reason = "indirect dispatch";
        return false;
    }
    const auto& result = *dispatch.stages.front().program.result;
    if (result.workgroupMemoryDwords != 0u) {
        reason = "workgroup memory buffer";
        return false;
    }
    if (result.scratchPoolSlotDwords != 0u) {
        reason = "scratch pool";
        return false;
    }
    if (dispatch.stages.front().program.words.pushConstants.size() > Graphics::ComputePushConstantBytes) {
        reason = "push data outside the block";
        return false;
    }
    for (const auto& binding : dispatch.layout.bindings) {
        if (binding.count != 1u) {
            reason = "buffer array binding";
            return false;
        }
        if (binding.source == Bindings::Source::Guest || (binding.source == Bindings::Source::Gds && !bindsGds())) {
            reason = binding.source == Bindings::Source::Gds ? "GDS buffer binding" : "guest buffer binding";
            return false;
        }
        if (binding.source == Bindings::Source::TableMap && dispatch.stages.front().tableMap == nullptr) {
            reason = "table map without a mirror";
            return false;
        }
    }
    return true;
}

std::span<const std::uint32_t> wordsOf(const Resolve::ResolvedStage& stage, std::uint32_t index) {
    for (const auto& [position, words] : stage.program.words.bindings) {
        if (position == index) return words;
    }
    return stage.program.result->bindings.at(index).guestDescriptor;
}

void noteStages(const Resolve::ResolvedDraw& draw, Downgrades::Kind kind, std::string_view detail) {
    if (!Downgrades::Active()) return;
    for (const auto& stage : draw.stages) Downgrades::Note(stage.program.codeAddress, kind, 0, detail);
}

void noteStagesWork(const Resolve::ResolvedDraw& draw, Downgrades::Work work, std::uint64_t groups) {
    if (!Downgrades::Active()) return;
    for (const auto& stage : draw.stages) Downgrades::NoteWork(stage.program.codeAddress, work, groups);
}

bool excludedStages(const Resolve::ResolvedDraw& draw, std::string& reason) {
    if (!std::ranges::any_of(draw.stages, [](const Resolve::ResolvedStage& stage) { return Downgrades::Excluded(stage.program.codeAddress); })) return false;
    reason = "excluded by APS5_RECODE_EXCLUDE";
    return true;
}

class DrawRecorder {
public:
    bool Precheck(const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::vector<std::pair<std::uint64_t, std::uint64_t>>& reads, std::string& reason) {
        const bool excluded = excludedStages(draw, reason);
        if (!excluded && precheck(draw, parameters, &reads, reason)) return true;
        noteStages(draw, Downgrades::Kind::ExistingPath, reason);
        if (!excluded) Downgrades::RefuseExistingPath(draw.stages.empty() ? 0u : draw.stages.back().program.codeAddress, "draw", reason);
        std::lock_guard lock(mutex);
        totals.existing++;
        const auto key = "existing path: " + reason.substr(0, 45);
        if (totals.reasons.size() < 24u || totals.reasons.contains(key)) totals.reasons[key]++;
        return false;
    }

    void PrebuildDraw(const Context& context, const Resolve::ResolvedDraw& draw) {
        const auto* heap = Resolve::HeapDevice();
        if (heap == nullptr || draw.state == nullptr || draw.stages.empty()) return;
        std::vector<Graphics::CompiledShader> shaders;
        for (const auto& stage : draw.stages) {
            if (stage.program.result == nullptr) return;
            shaders.push_back({stage.program.stage, stage.program.result.get(), stage.program.pushOffset});
        }
        Graphics::VertexInputLayout vertexInput;
        const auto& attributes = draw.stages.front().program.vertexAttributes;
        if (!attributes.empty()) {
            vertexInput = Graphics::BuildVertexInputLayout(context, attributes);
            for (std::size_t index = 0; index < attributes.size(); ++index) {
                if (vertexInput.attributes[index].format != Graphics::DecodeVertexFormat(attributes[index]).format) return;
            }
        }
        Graphics::PipelineSetLayouts sets;
        {
            std::lock_guard lock(mutex);
            sets.main = setLayout(context, draw, 0);
            sets.split = draw.layout.split;
            if (sets.split) sets.fragment = setLayout(context, draw, 1);
        }
        sets.mainKey = draw.layout.keys[0];
        sets.fragmentKey = draw.layout.keys[1];
        sets.heap = heap->Layout();
        const auto started = std::chrono::steady_clock::now();
        static_cast<void>(Graphics::CachedPipeline(context, *draw.state, vertexInput, sets, shaders, VK_IMAGE_LAYOUT_GENERAL));
        drawPrebuildNanoseconds.fetch_add(elapsed(started), std::memory_order_relaxed);
    }

    bool Record(const Context& context, const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::string& reason) {
        const auto waitStarted = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex);
        const auto started = std::chrono::steady_clock::now();
        totals.drawLockNanoseconds += elapsed(waitStarted) - elapsed(started);
        if (hangMarking() && context.recorder != nullptr) markHangPhase(context, *context.recorder, 4u);
        bool recorded = false;
        const auto openSerial = context.recorder != nullptr ? context.recorder->OpenSerial() : 0u;
        try {
            recorded = record(context, draw, parameters, reason);
            if (recorded && context.recorder != nullptr && !draw.stages.empty()) noteBatchWork(*context.recorder, openSerial, "draw", Downgrades::ProgramHash(draw.stages.back().program.codeAddress), 0u, nullptr, nullptr, draw.operation.queue, draw.operation.submission);
            const auto landStarted = std::chrono::steady_clock::now();
            totals.drawBodyNanoseconds += elapsed(started) - elapsed(landStarted);
            landWrites();
            scheduleLandings(context);
            totals.drawLandingNanoseconds += elapsed(landStarted);
        } catch (const std::exception& error) {
            reason = std::string("record failed: ") + error.what();
        }
        const auto recordSpent = elapsed(started);
        totals.nanoseconds += recordSpent;
        StageCosts::Note(StageCosts::Stage::Record, recordSpent);
        if (recorded) {
            totals.recorded++;
            noteExtent(draw);
            noteStagesWork(draw, Downgrades::Work::Draw, 0);
            noteUses(draw, "draw");
            return true;
        }
        noteStages(draw, Downgrades::Kind::Dropped, reason);
        refuse(reason);
        Downgrades::RefuseExistingPath(draw.stages.empty() ? 0u : draw.stages.back().program.codeAddress, "draw", "dropped while recording: " + reason);
        return false;
    }

    bool DispatchPrecheck(const Context& context, VulkanDevice& owner, const Resolve::ResolvedDraw& dispatch, std::string& reason) {
        std::unique_lock lock(mutex);
        const bool excluded = excludedStages(dispatch, reason);
        if (!excluded && dispatchPrecheck(dispatch, reason) && pipelineReady(context, owner, dispatch, lock, reason)) return true;
        if (excluded && !dispatch.stages.empty() && context.recorder != nullptr && probesOutput(dispatch.stages.front().program.codeAddress) && outputProbeDue(dispatch.stages.front().program.codeAddress)) {
            const auto codeAddress = dispatch.stages.front().program.codeAddress;
            for (std::size_t index = 0; index < dispatch.uses.size() && index < dispatch.imageBegin; ++index) {
                const auto& use = dispatch.uses[index];
                if (!use.write || use.size == 0u || use.size > OutputProbeLimitBytes) continue;
                context.recorder->OnComplete([codeAddress, address = use.address, size = use.size] {
                    if (!GuestMemory::Accessible(reinterpret_cast<const std::byte*>(address), static_cast<std::size_t>(size), false)) return;
                    printOutput(codeAddress, "existing-path", address, std::span(reinterpret_cast<const std::byte*>(address), static_cast<std::size_t>(size)));
                });
            }
        }
        noteStages(dispatch, Downgrades::Kind::ExistingPath, reason);
        if (!excluded) Downgrades::RefuseExistingPath(dispatch.stages.empty() ? 0u : dispatch.stages.front().program.codeAddress, "dispatch", reason);
        totals.dispatchExisting++;
        const auto key = "existing path dispatch: " + reason.substr(0, 45);
        if (totals.reasons.size() < 24u || totals.reasons.contains(key)) totals.reasons[key]++;
        return false;
    }

    bool RecordDispatch(const Context& context, const Resolve::ResolvedDraw& dispatch, const std::array<std::uint32_t, 3>& groups, std::string& reason) {
        std::lock_guard lock(mutex);
        const auto started = std::chrono::steady_clock::now();
        if (hangMarking() && context.recorder != nullptr) markHangPhase(context, *context.recorder, 4u);
        bool recorded = false;
        const auto openSerial = context.recorder != nullptr ? context.recorder->OpenSerial() : 0u;
        lastHangArguments = nullptr;
        try {
            recorded = recordDispatch(context, dispatch, groups, reason);
            if (recorded && context.recorder != nullptr && !dispatch.stages.empty()) {
                char kind[160];
                if (dispatch.operation.dispatch.indirect) std::snprintf(kind, sizeof(kind), "indirect@%llx[record %08x %08x %08x %08x %08x %08x %08x %08x%s]", static_cast<unsigned long long>(lastArgumentsAddress), lastArgumentsCpu[0], lastArgumentsCpu[1], lastArgumentsCpu[2], lastArgumentsCpu[3], lastArgumentsCpu[4], lastArgumentsCpu[5], lastArgumentsCpu[6], lastArgumentsCpu[7], lastArgumentsGpu ? " gpu" : "");
                else std::snprintf(kind, sizeof(kind), "dispatch");
                noteBatchWork(*context.recorder, openSerial, kind, Downgrades::ProgramHash(dispatch.stages.front().program.codeAddress), static_cast<std::uint64_t>(groups[0]) * groups[1] * groups[2], lastHangArguments, lastHangArguments != nullptr ? hangArguments : nullptr, dispatch.operation.queue, dispatch.operation.submission);
            }
            landWrites();
            scheduleLandings(context);
        } catch (const std::exception& error) {
            reason = std::string("record failed: ") + error.what();
        }
        totals.dispatchNanoseconds += elapsed(started);
        if (recorded) {
            totals.dispatches++;
            const bool indirect = dispatch.operation.dispatch.indirect;
            noteStagesWork(dispatch, indirect ? Downgrades::Work::IndirectDispatch : Downgrades::Work::Dispatch, indirect ? 0u : static_cast<std::uint64_t>(groups[0]) * groups[1] * groups[2]);
            noteUses(dispatch, "dispatch");
            return true;
        }
        noteStages(dispatch, Downgrades::Kind::Dropped, reason);
        refuse("dispatch " + reason);
        Downgrades::RefuseExistingPath(dispatch.stages.empty() ? 0u : dispatch.stages.front().program.codeAddress, "dispatch", "dropped while recording: " + reason);
        return false;
    }

    void probeOutputs(const Context& context, Graphics::Recorder& recorder, VkCommandBuffer commands, const Resolve::ResolvedDraw& dispatch, std::uint64_t codeAddress) {
        auto& residency = ResidencyService::Get();
        const auto copy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
        for (std::size_t index = 0; index < dispatch.uses.size() && index < dispatch.imageBegin; ++index) {
            const auto& use = dispatch.uses[index];
            if (!use.write || use.size == 0u || use.size > OutputProbeLimitBytes) continue;
            VkBuffer source = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            const char* where = "residency";
            if (const auto span = residency.Translate(use.address, use.size)) {
                source = reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span->handle));
                offset = span->offset;
            } else if (const auto* import = Graphics::HostImportFor(context, use.address, static_cast<std::size_t>(use.size)); import != nullptr) {
                source = import->buffer;
                offset = Graphics::HostImportOffset(*import, use.address);
                where = "import";
            }
            if (source == VK_NULL_HANDLE) continue;
            auto buffer = std::make_shared<Graphics::Buffer>(context, static_cast<std::size_t>(use.size), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            const VkBufferCopy region{offset, 0, use.size};
            copy(commands, source, buffer->Handle(), 1, &region);
            Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT);
            recorder.Keep(buffer);
            recorder.OnComplete([buffer, codeAddress, address = use.address, where] { printOutput(codeAddress, where, address, buffer->Bytes()); });
        }
    }

    void probeArguments(const Context& context, Graphics::Recorder& recorder, VkCommandBuffer commands, std::uint64_t address, VkBuffer used, VkDeviceSize usedOffset, std::uint64_t codeAddress) {
        constexpr VkDeviceSize bytes = 3u * sizeof(std::uint32_t);
        auto& residency = ResidencyService::Get();
        const auto copy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
        const auto read = [&](VkBuffer source, VkDeviceSize offset, const char* where) {
            auto buffer = std::make_shared<Graphics::Buffer>(context, static_cast<std::size_t>(bytes), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            const VkBufferCopy region{offset, 0, bytes};
            copy(commands, source, buffer->Handle(), 1, &region);
            Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT);
            recorder.Keep(buffer);
            recorder.OnComplete([buffer, codeAddress, address, where] { printOutput(codeAddress, where, address, buffer->Bytes()); });
        };
        const auto* import = Graphics::HostImportFor(context, address, static_cast<std::size_t>(bytes));
        read(used, usedOffset, ring != nullptr && used == ring->Handle() ? "arguments-used-snapshot" : import != nullptr && used == import->buffer ? "arguments-used-import" : "arguments-used-residency");
        if (const auto span = residency.Translate(address, bytes)) read(reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span->handle)), span->offset, "arguments-in-residency");
        if (import != nullptr) read(import->buffer, Graphics::HostImportOffset(*import, address), "arguments-in-import");
    }

    void traceSmallReads(const Resolve::ResolvedDraw& work) {
        static const char* listed = std::getenv("APS5_RECODE_TRACE_READS");
        if (listed == nullptr || work.stages.empty()) return;
        const auto hash = Downgrades::ProgramHash(work.stages.back().program.codeAddress);
        if (!Downgrades::ListedHash(listed, hash)) return;
        static std::atomic<std::uint32_t> lines{0};
        if (lines.fetch_add(1, std::memory_order_relaxed) >= 20000u) return;
        std::string line;
        char text[160];
        std::snprintf(text, sizeof(text), "[recode-reads] %.3f s %016llx:", static_cast<double>(elapsed(processStart())) / 1e9, static_cast<unsigned long long>(hash));
        line += text;
        for (std::size_t index = 0; index < work.uses.size() && index < work.imageBegin; ++index) {
            const auto& use = work.uses[index];
            if (use.write || use.size == 0u || use.size > 64u) continue;
            std::snprintf(text, sizeof(text), " 0x%llx:", static_cast<unsigned long long>(use.address));
            line += text;
            if (!Protector::Readable(use.address) || !Protector::Readable(use.address + use.size - 1u)) {
                line += " unreadable";
                continue;
            }
            for (std::uint64_t at = 0; at + 8u <= use.size; at += 8u) {
                std::uint64_t value = 0;
                std::memcpy(&value, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(use.address + at)), sizeof(value));
                std::snprintf(text, sizeof(text), " %llx", static_cast<unsigned long long>(value));
                line += text;
            }
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }

    void noteUses(const Resolve::ResolvedDraw& work, const char* what) {
        traceSmallReads(work);
        traceSourceWrites(work);
        if (!Downgrades::Active() || work.stages.empty()) return;
        const auto code = work.stages.back().program.codeAddress;
        if (!usesNoted.insert(code).second) return;
        std::string line = "[recode-uses] ";
        char text[96];
        std::snprintf(text, sizeof(text), "%016llx %s:", static_cast<unsigned long long>(Downgrades::ProgramHash(code)), what);
        line += text;
        constexpr std::size_t Shown = 28;
        for (std::size_t index = 0; index < work.uses.size() && index < Shown; ++index) {
            const auto& use = work.uses[index];
            const char* kind = index >= work.imageBegin ? "image" : index >= work.targetBegin && index < work.targetEnd ? "target" : "memory";
            std::snprintf(text, sizeof(text), " %s %s 0x%llx+0x%llx", use.write ? "WRITE" : "read", kind, static_cast<unsigned long long>(use.address), static_cast<unsigned long long>(use.size));
            line += text;
        }
        if (work.uses.size() > Shown) line += " ... " + std::to_string(work.uses.size() - Shown) + " more";
        for (const auto& stage : work.stages) {
            if (stage.program.variant == nullptr) continue;
            const auto& infos = stage.program.variant->info.info.knownBuffers;
            const auto written = static_cast<std::size_t>(std::ranges::count_if(infos, [](const ShaderRecompiler::KnownBuffer& known) { return known.written; }));
            line += "; known buffers " + std::to_string(infos.size()) + " (" + std::to_string(written) + " written), stores through addresses " + (stage.program.result != nullptr && Graphics::ShaderStoresThroughAddresses(stage.program.result->spirv) ? "yes" : "no");
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }

    std::string Line() {
        std::lock_guard lock(mutex);
        printArgumentCensus();
        printExistingPackets();
        std::fprintf(stderr, "%s\n", StageCosts::Line(ResidencyService::Get().Frame()).c_str());
        if (!totals.sampledCreations.empty()) {
            std::vector<std::pair<std::uint64_t, std::string>> top;
            for (const auto& [shape, count] : totals.sampledCreations) top.emplace_back(count, shape);
            std::ranges::sort(top, std::greater<>());
            std::string line = "[recode-sampled-creations] records made from memory for a sampled read, most repeated:";
            for (std::size_t index = 0; index < top.size() && index < 12u; ++index) line += " [" + std::to_string(top[index].first) + "x " + top[index].second + "]";
            std::fprintf(stderr, "%s; %zu shapes\n", line.c_str(), top.size());
            totals.sampledCreations.clear();
        }
        if (!totals.shapeHandOvers.empty()) {
            std::vector<std::pair<std::uint64_t, std::string>> top;
            for (const auto& [pair, count] : totals.shapeHandOvers) top.emplace_back(count, pair);
            std::ranges::sort(top, std::greater<>());
            std::string line = "[recode-shape-handovers] records retired for another shape, most repeated:";
            for (std::size_t index = 0; index < top.size() && index < 8u; ++index) line += " [" + std::to_string(top[index].first) + "x asked " + top[index].second + "]";
            std::fprintf(stderr, "%s\n", line.c_str());
            totals.shapeHandOvers.clear();
        }
        if (Downgrades::Active()) std::fprintf(stderr, "[recode-record-split] ms: write-backs for readers %.1f, hand-overs for writers %.1f, deferred images %.1f; inputs %.1f, targets and pipeline %.1f, open batch %.1f, input copies and sets %.1f, push and draw inputs %.1f, uploads and stores %.1f, pass access %.1f, start pass %.1f, binds and draw %.1f\n", totals.preSplit[0] / 1e6, totals.preSplit[1] / 1e6, totals.preSplit[2] / 1e6, totals.recordSplit[0] / 1e6, totals.recordSplit[1] / 1e6, totals.recordSplit[2] / 1e6, totals.recordSplit[3] / 1e6, totals.recordSplit[4] / 1e6, totals.recordSplit[5] / 1e6, totals.recordSplit[6] / 1e6, totals.recordSplit[7] / 1e6, totals.recordSplit[8] / 1e6);
        const auto pipelineCounts = takePipelineCounts();
        char text[384];
        const auto draws = totals.recorded + totals.refused;
        const auto ring = space.has_value() ? space->TakeCounts() : RingCounts{};
        std::snprintf(text, sizeof(text), "[recode-record] %llu recorded, %llu dropped, %llu to the existing path, %.1f us per draw; %llu indirect (%llu argument snapshots); ring %llu allocations, %.1f MiB, %llu wraps, %llu full; %llu descriptor pools", static_cast<unsigned long long>(totals.recorded), static_cast<unsigned long long>(totals.refused), static_cast<unsigned long long>(totals.existing), draws == 0 ? 0.0 : static_cast<double>(totals.nanoseconds) / 1000.0 / static_cast<double>(draws), static_cast<unsigned long long>(totals.indirect), static_cast<unsigned long long>(totals.snapshotArguments), static_cast<unsigned long long>(ring.allocations), static_cast<double>(ring.bytes) / 1048576.0, static_cast<unsigned long long>(ring.wraps), static_cast<unsigned long long>(ring.full), static_cast<unsigned long long>(totals.pools));
        std::string line = text;
        line += "; linear target stores " + std::to_string(totals.linearStores);
        line += "; batch moved before the pass " + std::to_string(totals.batchMoves);
        line += "; passes " + std::to_string(totals.passesBegun) + " begun, " + std::to_string(totals.passesContinued) + " draws continued, " + std::to_string(totals.passFeedback) + " broken by a target read";
        line += "; storage images written by draws " + std::to_string(totals.drawStorageWrites);
        line += "; small known writes stored straight through their import " + std::to_string(totals.directWrites) + " (" + std::to_string(smallWrites.size()) + " addresses seen)";
        line += "; target memo " + std::to_string(totals.targetMemoHits) + " hits, " + std::to_string(totals.targetMemoMisses) + " misses; " + std::to_string(totals.deferredReads) + " draws with reads bound at record";
        line += "; residency updates landed " + std::to_string(totals.residencyLands);
        line += "; known buffers " + std::to_string(totals.knownServed) + " served, " + std::to_string(totals.knownMapped) + " mapped (" + std::to_string(totals.knownPaged) + " paged in), " + std::to_string(totals.knownCopied) + " copied (" + std::to_string(totals.knownGpuWritten) + " GPU-written ranges not copied), " + std::to_string(totals.knownRefused) + " unresolved, " + std::to_string(totals.knownPrefixes) + " bound to their mapped prefix (" + std::to_string(totals.importExtensions) + " chunks moved to their written import to make a known buffer contiguous, " + std::to_string(totals.knownSwitches) + " of them from residency), " + std::to_string(totals.knownSkipped) + " skipped (" + std::to_string(totals.knownPointers) + " of them pointers); known writes " + std::to_string(totals.knownWritten) + " direct (" + std::to_string(totals.knownWriteBytes >> 10u) + " KiB landed), " + std::to_string(totals.knownWriteLookups) + " through the page table, " + std::to_string(totals.copyBacks) + " copied back to their import after the dispatch (" + std::to_string(totals.copyBackBytes >> 20u) + " MiB, " + std::to_string(totals.copyBackCovered >> 10u) + " KiB of written pieces not landed, " + std::to_string(totals.importsReturned) + " chunks returned from their import, " + std::to_string(totals.residencyRefreshes) + " residency refreshes before the dispatch, " + std::to_string(totals.refreshKeptPages) + " GPU-written pages kept from them)";
        line += "; draw inputs " + std::to_string(totals.inputsResident) + " resident, " + std::to_string(totals.inputsImported) + " imported, " + std::to_string(totals.inputsWritten) + " after CPU writes (" + std::to_string(totals.inputsCopied) + " copied into the ring), " + std::to_string(totals.inputsGpuWritten) + " GPU-written, " + std::to_string(totals.inputsPending) + " with GPU bytes pending read from the mirror, " + std::to_string(totals.inputsMirrored) + " GPU-written without an import read from the mirror, " + std::to_string(totals.inputsAliased) + " aliased, " + std::to_string(totals.inputsMerged) + " served with an overlapping input";
        std::snprintf(text, sizeof(text), "; draw phases surfaces %.1f ms, targets %.1f ms, pipelines %.1f ms, owned uploads %.1f ms", static_cast<double>(totals.surfaceNanoseconds) / 1e6, static_cast<double>(totals.targetNanoseconds) / 1e6, static_cast<double>(totals.graphicsPipelineNanoseconds) / 1e6, static_cast<double>(totals.uploadNanoseconds) / 1e6);
        line += text;
        std::snprintf(text, sizeof(text), "; dispatches %llu recorded (%llu indirect, %llu storage images written), %llu to the existing path, %.1f us per dispatch; compute pipelines %zu held, %llu queued (%llu ahead of recording), %llu built in %.1f ms, %zu building, %zu failed", static_cast<unsigned long long>(totals.dispatches), static_cast<unsigned long long>(totals.indirectDispatches), static_cast<unsigned long long>(totals.storageWrites), static_cast<unsigned long long>(totals.dispatchExisting), totals.dispatches == 0 ? 0.0 : static_cast<double>(totals.dispatchNanoseconds) / 1000.0 / static_cast<double>(totals.dispatches), pipelineCounts.held, static_cast<unsigned long long>(pipelinesQueued.load(std::memory_order_relaxed)), static_cast<unsigned long long>(pipelinesPrebuilt.load(std::memory_order_relaxed)), static_cast<unsigned long long>(pipelineCounts.built), static_cast<double>(pipelineCounts.nanoseconds) / 1e6, pipelineCounts.building, pipelineCounts.failed);
        line += text;
        line += "; pipeline builders " + std::to_string(pipelineCounts.builders) + " threads, at most " + std::to_string(pipelineCounts.mostAtOnce) + " builds at once, " + std::to_string(pipelinesReplayed.load(std::memory_order_relaxed)) + " built from the list";
        std::snprintf(text, sizeof(text), "; table maps %llu hits, %llu uploads (%.1f MiB), %llu retired, %zu held (%.1f MiB); table images %llu reused, %llu bound (%llu whole), %llu elements bound, %llu checked for a refresh, %llu depth kept, %llu retired, %zu held; compressed records beside another format %llu; consumes for CPU-written mirror inputs %llu",static_cast<unsigned long long>(totals.mapHits), static_cast<unsigned long long>(totals.mapUploads), static_cast<double>(totals.mapUploadBytes) / 1048576.0, static_cast<unsigned long long>(totals.mapRetired), maps.size(), static_cast<double>(mapBytes) / 1048576.0, static_cast<unsigned long long>(totals.tableImageHits), static_cast<unsigned long long>(totals.tableImageBuilds), static_cast<unsigned long long>(totals.tableImageWhole), static_cast<unsigned long long>(totals.tableImageBinds), static_cast<unsigned long long>(totals.tableImageRefreshes), static_cast<unsigned long long>(totals.tableImageDepthKept), static_cast<unsigned long long>(totals.tableImageRetired), tableImages.size(), static_cast<unsigned long long>(totals.compressedAliases), static_cast<unsigned long long>(inputConsumes().load(std::memory_order_relaxed)));
        line += text;
        line += "; table image entries dropped for retired records " + std::to_string(totals.tableImageEntriesDropped);
        line += "; record ranges noted GPU-written for the memory core " + std::to_string(totals.recordRangesNoted) + "; records made for a sampled shape after a hand-over " + std::to_string(totals.sampledRecordsCreated) + ", new storage records started from their memory " + std::to_string(totals.storageRecordsSeeded) + "; new chains zeroed " + std::to_string(totals.zeroedChains) + ", new records poisoned " + std::to_string(totals.poisonedRecords);
        if (metadataClears != nullptr) {
            const auto metadata = metadataClears->TakeCounts();
            line += "; metadata clears: DCC writes " + std::to_string(metadata.pendingDcc) + ", CMASK writes " + std::to_string(metadata.pendingCmask) + ", applied to records " + std::to_string(totals.metadataClears) + " (DCC " + std::to_string(metadata.dccApplied) + ", CMASK " + std::to_string(metadata.cmaskApplied) + "), refused " + std::to_string(metadata.refused) + ", DCC checks that found keys that disagree (not applied) " + std::to_string(metadata.dccMixed) + ", metadata set to expanded after a check " + std::to_string(metadata.expanded);
        }
        if (htileClears != nullptr) {
            const auto outcomes = htileClears->TakeOutcomes();
            line += "; HTILE checks finished: every tile cleared " + std::to_string(outcomes.cleared) + ", no tile cleared " + std::to_string(outcomes.untouched) + ", tiles disagree (not applied) " + std::to_string(outcomes.mixed);
        }
        line += "; records written back for buffer readers " + std::to_string(totals.readerWriteBacks) + " (" + std::to_string(totals.readerWriteBackBytes >> 20u) + " MiB, " + std::to_string(totals.readerWriteBackRefusals) + " refused, " + std::to_string(totals.readerStale) + " skipped: memory changed since their GPU write), handed over before a buffer writer " + std::to_string(totals.writerHandOvers);
        line += "; records handed to another shape " + std::to_string(totals.shapesHandedOver) + " (" + std::to_string(totals.writeBacks) + " written back into the mirror, " + std::to_string(totals.writeBackBytes >> 20u) + " MiB, " + std::to_string(totals.writeBackRefusals) + " refused, " + std::to_string(totals.staleShapes) + " not written back: memory changed since their GPU write, " + std::to_string(totals.writeBacksUnserved) + " retired unwritten: the write-back was refused); draws with a recode depth record " + std::to_string(totals.depthRecordDraws) + " (" + std::to_string(Images::Depths().Size()) + " records, " + std::to_string(Images::Depths().Counts().cleared) + " started from their clear value, " + std::to_string(Images::Depths().Counts().refused) + " refused, " + std::to_string(Images::Depths().Counts().superseded) + " superseded by a record over their memory, " + std::to_string(Images::Depths().Counts().htileWrites) + " driver writes over HTILE, " + std::to_string(totals.depthRecordsSampled) + " sampled reads of them, " + std::to_string(totals.depthBufferReads) + " buffer reads of their memory, " + std::to_string(totals.depthWriteBacks) + " depth write-backs into the mirror for them, " + std::to_string(totals.depthWriteBackRefusals) + " refused; " + std::to_string(totals.depthSeeds) + " started from memory (" + std::to_string(totals.depthSeedRefusals) + " refused), HTILE writes re-cleared " + std::to_string(totals.depthHtileClears) + ", HTILE writes without a clear " + std::to_string(totals.depthHtileOther) + "; CPU writes over their planes " + std::to_string(Images::Depths().Counts().cpuPlaneWrites) + ", seeded again from memory " + std::to_string(totals.depthReseeds) + ", seeded from a superseded record " + std::to_string(totals.depthSupersedeSeeds) + "; GPU writes over HTILE " + std::to_string(Images::Depths().Counts().gpuHtileWrites) +", checked for a clear on the GPU " + std::to_string(totals.depthGpuHtileClears) + " (" + std::to_string(totals.depthGpuHtileRefusals) + " refused), HTILE set to expanded after a clear " + std::to_string(totals.depthHtileExpanded) + " (" + std::to_string(totals.depthHtileExpandRefusals) + " refused)); targets and sampled views served as layers of a containing record " + std::to_string(totals.layerTargets) + ", records copied in as layers of a new record " + std::to_string(totals.layersAbsorbed) + " (" + std::to_string(totals.layersAbsorbedOverChanges) + " over a change note); CPU-content records detiled again after their memory changed " + std::to_string(totals.recordsRefreshed);
        line += "; flips from a GPU-written record " + std::to_string(flipsFromNewRecords.load(std::memory_order_relaxed)) + ", over a record not GPU-written " + std::to_string(flipsFromOldRecords.load(std::memory_order_relaxed)) + ", with no record " + std::to_string(flipsWithoutRecords.load(std::memory_order_relaxed)) + ", presented straight from the record " + std::to_string(flipsPresentedFromRecords.load(std::memory_order_relaxed));
        {
            auto& landings = batchLandings();
            line += "; landings after their batch " + std::to_string(landings.scheduled.load(std::memory_order_relaxed)) + " scheduled, " + std::to_string(landings.ran.load(std::memory_order_relaxed)) + " ran (" + std::to_string(landings.landedBytes.load(std::memory_order_relaxed) >> 10u) + " KiB), " + std::to_string(landings.pending.load(std::memory_order_relaxed)) + " pending, " + std::to_string(landings.retried.load(std::memory_order_relaxed)) + " retried later";
        }
        line += "; storage writes to recode surfaces " + std::to_string(totals.recordWrites) + "; draws into recode surface targets " + std::to_string(totals.recordTargetDraws) + " (" + std::to_string(totals.recordTargetRefusals) + " targets refused";
        for (const auto& [why, count] : recordTargetReasons) line += " [" + why + " " + std::to_string(count) + "]";
        line += ")";
        if (ownsImages()) {
            if (auto* ownerTable = Resolve::Surfaces()) line += "; " + ownerTable->OwnersLine() + "; sampled reads served by recode records " + std::to_string(totals.recordSampled);
        }
        line += "; fill and copy kernels recorded " + std::to_string(recordedFills.load(std::memory_order_relaxed)) + " and " + std::to_string(recordedCopies.load(std::memory_order_relaxed));
        line += "; GDS bound " + std::to_string(totals.gdsBindings) + " times;dispatches waited for their pipeline " + std::to_string(totals.pipelineWaits) + " times (" + std::to_string(totals.pipelineWaitNanoseconds / 1000000u) + " ms)";
        std::snprintf(text, sizeof(text), "; fault buffers %zu in flight, %llu read: %llu faults (%llu unmapped), %llu fault pages served, %llu written ranges (%llu pieces landed late), %llu written overflows", faults.size(), static_cast<unsigned long long>(totals.faultReads), static_cast<unsigned long long>(totals.faults), static_cast<unsigned long long>(totals.unmappedFaults), static_cast<unsigned long long>(totals.faultPages), static_cast<unsigned long long>(totals.writtenRanges), static_cast<unsigned long long>(totals.writtenLate), static_cast<unsigned long long>(totals.writtenOverflows));
        line += text;
        if (Mirror::WritesEnabled()) {
            std::snprintf(text, sizeof(text), "; mirror writes %.1f MiB, pages left GPU-owned %.1f MiB, edges landed whole %.1f MiB, edges snapshot %.1f MiB and landed by diff %.1f MiB, indirect arguments from the mirror %llu", static_cast<double>(totals.mirrorWriteBytes) / 1048576.0, static_cast<double>(totals.ownedBytes) / 1048576.0, static_cast<double>(totals.openLandedBytes) / 1048576.0, static_cast<double>(totals.edgeSnapshotBytes) / 1048576.0, static_cast<double>(totals.edgeDiffBytes) / 1048576.0, static_cast<unsigned long long>(totals.mirrorArguments));
            line += text;
        }
        std::snprintf(text, sizeof(text), "; indirect draw arguments patched on the GPU for a constant rule %llu; mesh draws %llu (%llu empty); depth records written back before a buffer writer %llu; draw record split: lock wait %.1f ms, body %.1f ms, landings %.1f ms", static_cast<unsigned long long>(totals.patchedArguments), static_cast<unsigned long long>(totals.meshDraws), static_cast<unsigned long long>(totals.meshEmpty), static_cast<unsigned long long>(totals.depthWriterHandOvers), static_cast<double>(totals.drawLockNanoseconds) / 1e6, static_cast<double>(totals.drawBodyNanoseconds) / 1e6, static_cast<double>(totals.drawLandingNanoseconds) / 1e6);
        line += text;
        std::snprintf(text, sizeof(text), "; GPU-written chunks %llu switched to their import from the written bitmap (%.1f MiB copied over), %llu from other GPU writes, %llu refused, %llu written pieces already on an import", static_cast<unsigned long long>(totals.importSwitches), static_cast<double>(totals.switchBytes) / 1048576.0, static_cast<unsigned long long>(totals.writtenSwitches), static_cast<unsigned long long>(totals.switchRefused), static_cast<unsigned long long>(totals.writtenOnImports));
        line += text;
        for (const auto& [reasonCode, count] : faultReasons) line += "; fault reason " + std::to_string(reasonCode) + " " + std::to_string(count);
        faultReasons.clear();
        for (const auto& [why, count] : totals.reasons) line += "; " + why + " " + std::to_string(count);
        line += "\n" + extentLine();
        if (Releases::FlipsFollowWork() || landsInCompletions()) line += "\n" + Releases::Line();
        const auto pools = totals.pools;
        totals = {};
        totals.pools = pools;
        return line;
    }

private:
    void noteExtent(const Resolve::ResolvedDraw& draw) {
        if (draw.state == nullptr) return;
        const auto width = static_cast<std::uint32_t>(std::lround(std::fabs(draw.state->viewport.width)));
        const auto height = static_cast<std::uint32_t>(std::lround(std::fabs(draw.state->viewport.height)));
        ++extents[{width, height}];
    }

    std::string extentLine() {
        std::vector<std::pair<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>>> ranked;
        std::uint64_t total = 0;
        std::uint64_t draws = 0;
        for (const auto& [size, count] : extents) {
            const auto pixels = count * size.first * size.second;
            ranked.emplace_back(pixels, size);
            total += pixels;
            draws += count;
        }
        extents.clear();
        if (total == 0u) return "[recode-extent] none";
        std::ranges::sort(ranked, std::greater{});
        std::string line = "[recode-extent]";
        std::uint64_t shown = 0;
        char part[64];
        for (std::size_t index = 0; index < ranked.size() && index < ExtentsShown; ++index) {
            const auto& [count, size] = ranked[index];
            std::snprintf(part, sizeof(part), " %ux%u %llu%%", size.first, size.second, static_cast<unsigned long long>(count * 100u / total));
            line += part;
            shown += count;
        }
        if (shown < total) {
            std::snprintf(part, sizeof(part), " other %llu%%", static_cast<unsigned long long>((total - shown) * 100u / total));
            line += part;
        }
        std::snprintf(part, sizeof(part), " of the pixels drawn; %llu draws", static_cast<unsigned long long>(draws));
        return line + part;
    }

public:
    std::uint64_t WriteBackAll(const Context& context, Graphics::Recorder& recorder) {
        std::lock_guard lock(mutex);
        std::uint64_t written = 0;
        if (device != context.device) return 0u;
        if (surfaceDetiler == nullptr) surfaceDetiler = std::make_unique<Images::SurfaceDetiler>(context);
        if (auto* table = Resolve::Surfaces(); table != nullptr && ownsImages()) {
            for (const auto& record : table->StaleOverlapping(0u, MirrorWindow())) {
                if (record->retired || table->ChangedSinceWrite(*record)) continue;
                std::string reason;
                if (!surfaceDetiler->WriteBack(recorder, record, reason)) continue;
                noteWrittenBack(recorder, record->key.address, record->key.bytes);
                table->MarkWrittenBack(*record);
                ++written;
            }
        }
        if (Images::DepthRecordsOn()) {
            for (const auto& record : Images::Depths().Overlapping(0u, MirrorWindow())) {
                if (!record->Initialized() || !record->TakeDrawn()) continue;
                std::vector<std::pair<std::uint64_t, std::uint64_t>> planes;
                std::string reason;
                if (!surfaceDetiler->WriteBackDepth(recorder, record, planes, reason)) continue;
                for (const auto& [begin, size] : planes) noteWrittenBack(recorder, begin, size);
                ++written;
            }
        }
        return written;
    }

    void Detach(VkDevice replaced) {
        listReplayStops.store(true, std::memory_order_release);
        while (listReplaying.load(std::memory_order_acquire)) std::this_thread::yield();
        {
            std::unique_lock jobs(pipelineMutex);
            ++framesWaiting;
            jobsReady.notify_all();
            pipelinesDone.wait(jobs, [&] { return pipelineJobs.empty() && buildingPipelines.empty(); });
            --framesWaiting;
        }
        std::lock_guard lock(mutex);
        if (device != replaced && device != VK_NULL_HANDLE) return;
        device = VK_NULL_HANDLE;
        attachedDevice.store(VK_NULL_HANDLE, std::memory_order_release);
        listReplayStops.store(false, std::memory_order_release);
        listReplayStarted.store(false, std::memory_order_release);
        ring.reset();
        ringAddress = 0;
        space.reset();
        faults.clear();
        spareFaults.clear();
        knownWrites.clear();
        copyBacks.clear();
        writtenRecords.clear();
        stageStorageImages.clear();
        groupReads.reset();
        argumentReads.reset();
        {
            std::lock_guard layoutLock(layoutMutex);
            layouts.clear();
            emptyLayout = VK_NULL_HANDLE;
        }
        pools.clear();
        spare.clear();
        maps.clear();
        mapBytes = 0;
        tableImages.clear();
        surfaceDetiler.reset();
        metadataClears.reset();
        htileClears.reset();
        {
            std::lock_guard jobs(pipelineMutex);
            computePipelines.clear();
            failedPipelines.clear();
        }
        landing.clear();
        landingSerials.clear();
        pendingDiffs.clear();
        targetMemo = {};
        std::fprintf(stderr, "[recode-record] detached from the replaced device; the next recorded work attaches to the new one\n");
    }

private:
    bool attach(const Context& context, std::string& reason) {
        if (device == context.device && ring != nullptr) return true;
        if (device != VK_NULL_HANDLE) {
            reason = "a second device";
            return false;
        }
        if (ResidencyService::MirrorAddressing() && !ResidencyService::Get().Attach(context)) {
            reason = "residency service did not attach";
            return false;
        }
        device = context.device;
        alignment = std::max<std::uint64_t>(context.limits.minStorageBufferOffsetAlignment, 16u);
        const auto ringMemory = ResidencyService::ShaderReadMemory(context, RingBytes);
        const VkBufferUsageFlags addressable = context.bufferDeviceAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0u;
        ring = std::make_shared<Graphics::Buffer>(context, RingBytes + RingZeroBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | addressable, ringMemory);
        std::memset(ring->Bytes().data() + RingBytes, 0, RingZeroBytes);
        ringAddress = addressable != 0u ? ring->DeviceAddress() : 0u;
        space.emplace(RingBytes, alignment);
        attachedDevice.store(device, std::memory_order_release);
        std::fprintf(stderr, "[recode-record] ring %llu MiB, alignment %llu, %s memory\n", static_cast<unsigned long long>(RingBytes >> 20u), static_cast<unsigned long long>(alignment), (ringMemory & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 ? "device" : "host");
        return true;
    }

    void refuse(const std::string& reason) {
        totals.refused++;
        auto key = "dropped: " + reason.substr(0, 50);
        if (seen.insert(key).second) std::fprintf(stderr, "[recode-record] first drop: %s\n", reason.c_str());
        if (totals.reasons.size() < 24u || totals.reasons.contains(key)) totals.reasons[key]++;
    }

    void drainSurfaces(const Context& context, Graphics::Recorder& recorder) {
        auto* table = Resolve::Surfaces();
        if (table == nullptr) return;
        auto work = table->TakeWork();
        if (work.empty()) return;
        if (surfaceDetiler == nullptr) surfaceDetiler = std::make_unique<Images::SurfaceDetiler>(context);
        markHangPhase(context, recorder, 7u);
        if (hangMarking() && RouteMemory::UploadsPending()) {
            std::vector<std::shared_ptr<void>> kept;
            static_cast<void>(RouteMemory::RecordUploads(recorder.Commands(), kept));
            for (auto& object : kept) recorder.Keep(std::move(object));
            markHangPhase(context, recorder, 9u);
        }
        surfaceDetiler->Record(recorder, *table, work, [&](const std::string& why) {
            totals.surfaceFailures++;
            auto key = "surface: " + why.substr(0, 50);
            if (seen.insert(key).second) std::fprintf(stderr, "[recode-record] first surface failure: %s\n", why.c_str());
            if (totals.reasons.size() < 24u || totals.reasons.contains(key)) totals.reasons[key]++;
        });
        markHangPhase(context, recorder, 8u);
        if (hangMarking()) {
            std::uint64_t pixels = 0;
            for (const auto& surface : work) pixels += static_cast<std::uint64_t>(surface->key.width) * surface->key.height;
            noteDetileWork(work.size(), pixels, work.empty() ? 0u : work.front()->key.address);
            if (!work.empty()) {
                const auto& key = work.front()->key;
                char text[200];
                std::snprintf(text, sizeof(text), "+0x%llx %ux%ux%u format %u tile %u dimension %u mips %u pitch %u xor %u", static_cast<unsigned long long>(key.bytes), key.width, key.height, key.depth, key.format, key.tileMode, key.dimension, key.mips, key.pitch, key.pipeBankXor);
                noteDetileKey(text);
            }
        }
    }

    void scheduleLandings(const Context& context) {
        if (!landsInCompletions() || context.recorder == nullptr) return;
        auto& landings = batchLandings();
        {
            std::lock_guard lock(landings.retryMutex);
            for (auto& written : landings.retry) pendingDiffs.push_back(std::move(written));
            landings.retry.clear();
        }
        std::vector<KnownWrite> due;
        for (auto it = knownWrites.begin(); it != knownWrites.end();) {
            if (it->before == nullptr) {
                ++it;
                continue;
            }
            due.push_back(std::move(*it));
            it = knownWrites.erase(it);
        }
        if (due.empty()) return;
        auto held = std::make_shared<std::vector<KnownWrite>>(std::move(due));
        LandAfterBatch(*context.recorder, [held] { landInCompletion(*held); });
    }

    void landWrites() {
        if (!pendingDiffs.empty()) {
            const auto diffs = std::move(pendingDiffs);
            pendingDiffs.clear();
            landDiffs(diffs);
        }
        if (landing.empty()) return;
        const auto written = std::move(landing);
        const auto serials = std::move(landingSerials);
        landing.clear();
        landingSerials.clear();
        if (Mirror::WritesEnabled()) {
            landOwned(written, serials);
        } else {
            for (const auto& [begin, size] : written) Downgrades::NoteGuestWrite("a landed GPU write", begin, size);
            RouteMemory::LandGpuWrites(written);
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ends;
        ends.reserve(written.size());
        for (const auto& [begin, size] : written) ends.emplace_back(begin, begin + size);
        Events::NoteRanges(ends);
    }

    void landOwned(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& written, const std::vector<std::uint64_t>& serials) {
        auto& core = MemoryCore::Get();
        std::vector<std::pair<std::uint64_t, std::uint64_t>> eager;
        std::vector<std::uint64_t> eagerSerials;
        for (std::size_t index = 0; index < written.size(); ++index) {
            const auto [begin, size] = written[index];
            const auto serial = index < serials.size() ? serials[index] : 0u;
            UploadClash::NoteTracked(begin, size);
            const auto mark = core.NoteGpuWrite(begin, size, serial, false);
            for (const auto& range : mark.owned) totals.ownedBytes += range.size;
            for (const auto& range : mark.open) {
                eager.emplace_back(range.address, range.size);
                eagerSerials.push_back(serial);
            }
        }
        if (eager.empty()) return;
        for (const auto& [begin, size] : eager) {
            Downgrades::NoteGuestWrite("a landed GPU write", begin, size);
            totals.openLandedBytes += size;
        }
        RouteMemory::LandGpuWrites(eager);
        for (std::size_t index = 0; index < eager.size(); ++index) core.NoteLandedGpuWrite(eager[index].first, eager[index].second, eagerSerials[index]);
    }

    void landWritten(std::uint64_t begin, std::uint64_t size, std::uint64_t serial) {
        for (auto at = begin; at < begin + size;) {
            const auto chunk = at & ~(ChunkBytes - 1u);
            const auto next = std::min(begin + size, chunk + ChunkBytes);
            landing.emplace_back(at, next - at);
            landingSerials.push_back(serial);
            totals.writtenLate++;
            for (auto page = (at - chunk) / PageBytes; page <= (next - 1u - chunk) / PageBytes; ++page) writtenPages[chunk] |= 1u << page;
            at = next;
        }
    }

    std::uint64_t directSmallWrite(const Context& context, std::uint64_t address, std::uint64_t bytes) {
        if (copiesBackSmallWrites() || bytes == 0u || bytes >= PageBytes) return 0u;
        auto& count = smallWrites[address];
        if (count >= coldSmallWrites()) return 0u;
        const auto* import = Graphics::HostImportFor(context, address, static_cast<std::size_t>(bytes));
        if (import == nullptr || import->address == 0u || address < import->base || address + bytes > import->base + import->bytes) return 0u;
        ++count;
        directWrites.emplace_back(address, address + bytes);
        if (context.recorder != nullptr) context.recorder->NotePendingWrite(address, static_cast<std::size_t>(bytes));
        totals.directWrites++;
        if (Downgrades::Active() && context.recorder != nullptr && count == 1u) {
            context.recorder->OnComplete([address, bytes] {
                if (!GuestMemory::Accessible(reinterpret_cast<const std::byte*>(address), static_cast<std::size_t>(bytes), false)) return;
                std::array<float, 6> values{};
                std::memcpy(values.data(), reinterpret_cast<const void*>(address), std::min<std::size_t>(sizeof(values), static_cast<std::size_t>(bytes)));
                std::uint32_t zeros = 0;
                for (std::uint64_t at = 0; at + 4u <= bytes; at += 4u) {
                    std::uint32_t word = 0;
                    std::memcpy(&word, reinterpret_cast<const void*>(address + at), sizeof(word));
                    if (word == 0u) ++zeros;
                }
                std::fprintf(stderr, "[recode-probe] small write 0x%llx+0x%llx after its batch: %u of %llu words zero; first floats %g %g %g %g %g %g\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), zeros, static_cast<unsigned long long>(bytes / 4u), values[0], values[1], values[2], values[3], values[4], values[5]);
            });
        }
        return import->address + (address - import->base);
    }

    bool directlyWritten(std::uint64_t begin, std::uint64_t end) const {
        return std::ranges::any_of(directWrites, [&](const std::pair<std::uint64_t, std::uint64_t>& range) { return range.first <= begin && end <= range.second; });
    }

    void landKnownWrites(std::uint64_t completed) {
        while (!knownWrites.empty() && knownWrites.front().serial <= completed) {
            auto written = std::move(knownWrites.front());
            knownWrites.pop_front();
            if (written.before != nullptr) {
                pendingDiffs.push_back(std::move(written));
                continue;
            }
            landWritten(written.address, written.bytes, written.serial);
            totals.knownWriteBytes += written.bytes;
        }
    }

    void landDiffs(const std::vector<KnownWrite>& diffs) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        ranges.reserve(diffs.size());
        for (const auto& written : diffs) ranges.emplace_back(written.address, written.bytes);
        std::vector<PendingUpload> pieces;
        {
            std::unique_lock<GuestMemory::GpuMutexType> gpu(GuestMemory::GpuMutex(), std::defer_lock);
            if (!GuestMemory::GpuMutex().HeldByThisThread()) gpu.lock();
            pieces = ResidencyService::Get().ReadBack(ranges);
        }
        auto& core = MemoryCore::Get();
        for (const auto& written : diffs) {
            std::vector<std::byte> after(static_cast<std::size_t>(written.bytes));
            std::uint64_t covered = 0;
            for (const auto& piece : pieces) {
                const auto begin = std::max(piece.guest, written.address);
                const auto end = std::min(piece.guest + piece.bytes.size(), written.address + written.bytes);
                if (begin >= end) continue;
                std::memcpy(after.data() + (begin - written.address), piece.bytes.data() + (begin - piece.guest), static_cast<std::size_t>(end - begin));
                covered += end - begin;
            }
            if (covered < written.bytes) {
                landWritten(written.address, written.bytes, written.serial);
                totals.knownWriteBytes += written.bytes;
                continue;
            }
            core.LandDiff(written.address, *written.before, after, written.serial);
            totals.edgeDiffBytes += written.bytes;
        }
    }

    bool queueCopyBack(const Context& context, std::uint64_t address, std::uint64_t bytes) {
        if (!copyingBack || bytes > KnownBufferLimitBytes) return false;
        const auto* import = Graphics::HostImportFor(context, address, static_cast<std::size_t>(bytes));
        if (import == nullptr || address < import->base || address + bytes > import->base + import->bytes) return false;
        if (refreshesBeforeCopyBack()) refreshResidency(context, address, bytes, *import);
        copyBacks.push_back({address, bytes, import});
        return true;
    }

    void refreshResidency(const Context& context, std::uint64_t address, std::uint64_t bytes, const Graphics::HostImport& import) {
        auto& residency = ResidencyService::Get();
        std::vector<std::pair<std::uint64_t, VkBufferCopy>> regions;
        const auto end = address + bytes;
        const auto add = [&](std::uint64_t from, std::uint64_t to) {
            const auto span = residency.Translate(from, to - from);
            if (!span) return;
            const auto source = Graphics::HostImportOffset(import, from);
            if (!regions.empty() && regions.back().first == span->handle && regions.back().second.srcOffset + regions.back().second.size == source && regions.back().second.dstOffset + regions.back().second.size == span->offset) regions.back().second.size += to - from;
            else regions.push_back({span->handle, VkBufferCopy{source, span->offset, to - from}});
        };
        for (auto at = address; at < end;) {
            const auto chunk = at & ~(ChunkBytes - 1u);
            const auto next = std::min(end, chunk + ChunkBytes);
            {
                const auto found = keepsGpuWrittenPages() ? writtenPages.find(chunk) : writtenPages.end();
                const std::uint32_t newer = found != writtenPages.end() ? found->second : 0u;
                if (newer == 0u) {
                    add(at, next);
                } else {
                    for (auto from = at; from < next;) {
                        const auto page = static_cast<std::uint32_t>((from - chunk) / PageBytes);
                        const auto to = std::min(next, chunk + (page + 1u) * PageBytes);
                        if ((newer & (1u << page)) != 0u) totals.refreshKeptPages++;
                        else add(from, to);
                        from = to;
                    }
                }
            }
            at = next;
        }
        if (regions.empty()) return;
        const auto commands = context.recorder->Commands();
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        const auto copy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
        for (const auto& [handle, region] : regions) copy(commands, import.buffer, reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(handle)), 1, &region);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        totals.residencyRefreshes += regions.size();
    }

    void recordCopyBacks(const Context& context, VkCommandBuffer commands, std::uint64_t serial) {
        if (copyBacks.empty()) return;
        auto& residency = ResidencyService::Get();
        const auto copy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
        auto& covered = copiedBack[serial];
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ends;
        std::vector<VkBufferCopy> regions;
        std::uint64_t handle = 0;
        VkBuffer target = VK_NULL_HANDLE;
        const auto flush = [&] {
            if (!regions.empty()) copy(commands, reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(handle)), target, static_cast<std::uint32_t>(regions.size()), regions.data());
            regions.clear();
        };
        for (const auto& back : copyBacks) {
            const auto end = back.address + back.bytes;
            const auto whole = residency.Translate(back.address, back.bytes);
            if (whole) {
                if (whole->handle != handle || back.import->buffer != target) {
                    flush();
                    handle = whole->handle;
                    target = back.import->buffer;
                }
                regions.push_back({whole->offset, Graphics::HostImportOffset(*back.import, back.address), back.bytes});
                totals.copyBackBytes += back.bytes;
            }
            for (auto at = whole ? end : back.address; at < end;) {
                const auto chunk = at & ~(ChunkBytes - 1u);
                const auto next = std::min(end, chunk + ChunkBytes);
                const auto span = residency.Translate(at, next - at);
                if (span) {
                    if (span->handle != handle || back.import->buffer != target) {
                        flush();
                        handle = span->handle;
                        target = back.import->buffer;
                    }
                    const auto destination = Graphics::HostImportOffset(*back.import, at);
                    if (!regions.empty() && regions.back().srcOffset + regions.back().size == span->offset && regions.back().dstOffset + regions.back().size == destination) regions.back().size += next - at;
                    else regions.push_back({span->offset, destination, next - at});
                    totals.copyBackBytes += next - at;
                }
                at = next;
            }
            covered.emplace_back(back.address, end);
            ends.emplace_back(back.address, end);
            Downgrades::NoteGuestWrite("a copy-back to the import", back.address, back.bytes);
            if (keepsGpuWrittenPages()) clearResidencyNewer(back.address, end);
            totals.copyBacks++;
        }
        flush();
        copyBacks.clear();
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT);
        for (const auto& [begin, end] : ends) {
            Events::Shared().Note(begin, end - begin);
            MemoryCore::Get().Pages().NoteGpuWritten(begin, end - begin);
        }
    }

    void landUncovered(std::uint64_t begin, std::uint64_t size, std::uint64_t serial, const std::vector<std::pair<std::uint64_t, std::uint64_t>>* covered) {
        const auto end = begin + size;
        if (covered == nullptr) {
            landWritten(begin, size, serial);
            return;
        }
        for (auto at = begin; at < end;) {
            const auto hit = std::ranges::find_if(*covered, [&](const auto& range) { return range.first <= at && at < range.second; });
            if (hit != covered->end()) {
                const auto stop = std::min(end, hit->second);
                totals.copyBackCovered += stop - at;
                at = stop;
                continue;
            }
            auto stop = end;
            for (const auto& range : *covered) {
                if (range.first > at && range.first < stop) stop = range.first;
            }
            landWritten(at, stop - at, serial);
            at = stop;
        }
    }

    std::uint64_t mirrorKnownWrite(std::uint64_t address, std::uint64_t bytes, std::uint64_t serial) {
        if (bytes > KnownBufferLimitBytes) return 0u;
        auto& residency = ResidencyService::Get();
        const auto span = residency.MirrorSpan(address, bytes);
        if (!span) return 0u;
        UploadClash::NoteTracked(address, bytes);
        const auto mark = MemoryCore::Get().NoteGpuWrite(address, bytes, serial);
        if (!mark.upload.empty()) residency.NoteDirty(mark.upload);
        for (const auto& range : mark.owned) totals.ownedBytes += range.size;
        for (const auto& range : mark.open) {
            if (!knownWrites.empty() && knownWrites.back().serial == serial && knownWrites.back().address == range.address && knownWrites.back().bytes == range.size) continue;
            std::shared_ptr<const std::vector<std::byte>> before;
            const auto* view = Mirror::PatientView(range.address, static_cast<std::size_t>(range.size));
            if (view != nullptr && range.size <= KnownBufferLimitBytes) {
                before = std::make_shared<const std::vector<std::byte>>(view, view + range.size);
                totals.edgeSnapshotBytes += range.size;
            }
            knownWrites.push_back({serial, range.address, range.size, std::move(before)});
        }
        totals.mirrorWriteBytes += bytes;
        return span->address;
    }

    std::uint64_t serveKnownWrite(const Context& context, std::uint64_t address, std::uint64_t& bytes, std::uint64_t serial, bool copiedBackWrite) {
        static_cast<void>(context);
        auto& residency = ResidencyService::Get();
        if (bytes > (copiedBackWrite ? KnownBufferLimitBytes : KnownWriteLimitBytes)) return 0u;
        auto mapped = residency.TranslateMapped(address, bytes);
        if (mapped == 0u && bytes <= KnownWriteLimitBytes && servesFaultPages()) {
            if (const auto pages = residency.UnmappedPages(address, bytes); !pages.empty()) {
                RouteMemory::ServeFaultPages(pages);
                mapped = residency.TranslateMapped(address, bytes);
            }
        }
        if (mapped == 0u && servesKnownPrefixes()) {
            const auto [device, length] = residency.MappedPrefix(address, bytes);
            if (device != 0u) {
                totals.knownPrefixes++;
                mapped = device;
                bytes = length;
            }
        }
        if (mapped == 0u || copiedBackWrite) return mapped;
        if (knownWrites.empty() || knownWrites.back().serial != serial || knownWrites.back().address != address || knownWrites.back().bytes != bytes) knownWrites.push_back({serial, address, bytes});
        return mapped;
    }

    void readFaults(Graphics::Buffer& buffer, std::uint64_t serial) {
        namespace Abi = ShaderRecompiler::BdaAbi;
        auto bytes = buffer.Bytes();
        auto* words = reinterpret_cast<std::uint32_t*>(bytes.data());
        totals.faultReads++;
        if (words[Abi::WrittenOverflowWord] != 0u) totals.writtenOverflows++;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
        if (Abi::ForEachWrittenRange(words, [&](std::uint64_t begin, std::size_t size) { written.emplace_back(begin, size); })) {
            totals.writtenRanges += written.size();
            const auto found = copiedBack.find(serial);
            for (const auto& [begin, size] : written) landUncovered(begin, size, serial, found == copiedBack.end() ? nullptr : &found->second);
        }
        copiedBack.erase(copiedBack.begin(), copiedBack.upper_bound(serial));
        Abi::Fault report{};
        std::memcpy(&report, bytes.data(), sizeof(report));
        if (report.state == Abi::FaultState::Ready) {
            totals.faults++;
            const auto code = static_cast<std::uint32_t>(report.reason);
            faultReasons[code]++;
            if (report.reason == Abi::FaultReason::Unmapped) totals.unmappedFaults++;
            if (Downgrades::Active()) {
                char text[96];
                std::snprintf(text, sizeof(text), "pc 0x%x stage %u at 0x%llx+%u", report.instruction, report.stage, static_cast<unsigned long long>(report.address), report.bytes);
                Downgrades::NoteHashed(0u, Downgrades::Kind::PageFault, code, text);
            }
            if (seenFaults.insert(code).second) std::fprintf(stderr, "[recode-record] first fault: reason %u at 0x%llx, %u bytes, stage %u, pc 0x%x\n", code, static_cast<unsigned long long>(report.address), report.bytes, report.stage, report.instruction);
            std::vector<std::uint64_t> pages;
            if (Abi::ForEachFaultPage(words, [&](std::uint64_t page) { pages.push_back(page); }) && !pages.empty()) {
                totals.faultPages += pages.size();
                if (servesFaultPages()) RouteMemory::ServeFaultPages(pages);
            }
        }
        std::memset(bytes.data(), 0, bytes.size());
    }

    std::shared_ptr<Graphics::Buffer> faultFor(const Context& context, std::uint64_t serial, std::uint64_t completed) {
        while (!faults.empty() && faults.front().serial <= completed && faults.front().serial != serial) {
            readFaults(*faults.front().buffer, faults.front().serial);
            spareFaults.push_back(std::move(faults.front().buffer));
            faults.pop_front();
        }
        if (!faults.empty() && faults.back().serial == serial) return faults.back().buffer;
        std::shared_ptr<Graphics::Buffer> buffer;
        if (!spareFaults.empty()) {
            buffer = std::move(spareFaults.back());
            spareFaults.pop_back();
        } else {
            buffer = std::make_shared<Graphics::Buffer>(context, ShaderRecompiler::BdaAbi::FaultBufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            std::memset(buffer->Bytes().data(), 0, buffer->Bytes().size());
        }
        faults.push_back({buffer, serial});
        return buffer;
    }

    std::shared_ptr<Graphics::Buffer> mapBuffer(const Context& context, const Tables::Mirror& mirror, std::uint64_t serial, std::uint64_t completed) {
        if (const auto found = maps.find(mirror.id); found != maps.end()) {
            found->second.serial = serial;
            totals.mapHits++;
            return found->second.buffer;
        }
        if (mapBytes > MapBudgetBytes) {
            for (auto at = maps.begin(); at != maps.end() && mapBytes > MapBudgetBytes / 2u;) {
                if (at->second.serial > completed) {
                    ++at;
                    continue;
                }
                mapBytes -= at->second.buffer->Bytes().size();
                at = maps.erase(at);
                totals.mapRetired++;
            }
        }
        const auto bytes = std::max<std::size_t>(mirror.map.size() * sizeof(std::uint32_t), 16u);
        auto buffer = std::make_shared<Graphics::Buffer>(context, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ResidencyService::ShaderReadMemory(context, bytes));
        std::memcpy(buffer->Bytes().data(), mirror.map.data(), mirror.map.size() * sizeof(std::uint32_t));
        mapBytes += buffer->Bytes().size();
        maps[mirror.id] = {buffer, serial};
        totals.mapUploads++;
        totals.mapUploadBytes += bytes;
        return buffer;
    }

    std::optional<std::uint64_t> ringWrite(std::span<const std::byte> bytes, std::uint64_t serial, std::uint64_t completed) {
        const auto offset = space->Allocate(bytes.size(), serial, completed);
        if (!offset.has_value()) return std::nullopt;
        std::memcpy(ring->Bytes().data() + *offset, bytes.data(), bytes.size());
        return offset;
    }

    VkDescriptorSetLayout setLayout(const Context& context, const Resolve::ResolvedDraw& draw, std::uint32_t set) {
        std::lock_guard layoutLock(layoutMutex);
        const auto& key = draw.layout.keys[set];
        if (const auto found = layouts.find(key); found != layouts.end()) return found->second;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const auto& binding : draw.layout.bindings) {
            if (binding.set != set) continue;
            VkShaderStageFlags stages = 0;
            for (const auto& stage : draw.stages) {
                if ((binding.stages & Bindings::StageBit(stage.program.stage)) != 0u) stages |= Graphics::VulkanStage(stage.program.stage);
            }
            bindings.push_back({binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding.count, stages, nullptr});
        }
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        Graphics::Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &info, nullptr, &layout), "vkCreateDescriptorSetLayout recode draw");
        layouts.emplace(key, layout);
        return layout;
    }

    VkDescriptorPool createPool(const Context& context) {
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, PoolBuffers};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = PoolSets;
        info.poolSizeCount = 1;
        info.pPoolSizes = &size;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        Graphics::Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &info, nullptr, &pool), "vkCreateDescriptorPool recode draw");
        totals.pools++;
        return pool;
    }

    VkDescriptorPool poolFor(const Context& context, std::uint64_t serial, std::uint64_t completed, bool fresh) {
        while (!pools.empty() && pools.front().second <= completed) {
            context.Function<PFN_vkResetDescriptorPool>("vkResetDescriptorPool")(context.device, pools.front().first, 0);
            spare.push_back(pools.front().first);
            pools.pop_front();
        }
        if (!fresh && !pools.empty() && pools.back().second == serial) return pools.back().first;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (!spare.empty()) {
            pool = spare.back();
            spare.pop_back();
        } else {
            pool = createPool(context);
        }
        pools.emplace_back(pool, serial);
        return pool;
    }

    VkDescriptorSet allocateSet(const Context& context, VkDescriptorSetLayout layout, std::uint64_t serial, std::uint64_t completed) {
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        info.descriptorPool = poolFor(context, serial, completed, false);
        if (context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &info, &set) == VK_SUCCESS) return set;
        info.descriptorPool = poolFor(context, serial, completed, true);
        Graphics::Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &info, &set), "vkAllocateDescriptorSets recode draw");
        return set;
    }

    template<typename TPut>
    void serveKnownBuffers(const Context& context, const Resolve::ResolvedStage& stage, std::uint64_t serial, std::uint64_t completed, TPut&& put) {
        if (stage.program.variant == nullptr) return;
        static const std::vector<ShaderRecompiler::DescriptorValue> none;
        const auto& layout = stage.program.variant->info.bindings;
        const auto& known = stage.program.capture != nullptr ? stage.program.capture->snapshot.knownBuffers : none;
        const auto& infos = stage.program.variant->info.info.knownBuffers;
        const bool preferOwnedReads = (stage.program.stage == ShaderRecompiler::ShaderStage::Compute || stage.program.stage == ShaderRecompiler::ShaderStage::Mesh) &&
            RouteMemory::ServesOwned() && stage.program.result != nullptr &&
            std::ranges::any_of(stage.program.result->bindings, [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; });
        landKnownWrites(completed);
        for (std::uint32_t index = 0; index < layout.knownBufferCount; ++index) {
            const auto dword = layout.KnownBufferDword() + index * ShaderRecompiler::KnownBufferDwords;
            const auto write = [&](std::uint64_t device, std::uint64_t bytes) {
                put(dword, static_cast<std::uint32_t>(device));
                put(dword + 1u, static_cast<std::uint32_t>(device >> 32u));
                put(dword + 2u, static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes, 0xffffffffu)));
            };
            const auto [address, bytes] = index < known.size() ? Resolve::KnownBufferSpan(index < infos.size() ? infos[index] : ShaderRecompiler::KnownBuffer{}, known[index]) : std::pair<std::uint64_t, std::uint64_t>{0u, 0u};
            if (index < infos.size() && infos[index].Pointer()) totals.knownPointers++;
            const auto zero = ringAddress != 0u ? ringAddress + RingBytes : 0u;
            if (address == 0u || bytes == 0u) {
                totals.knownSkipped++;
                if (Downgrades::Active()) {
                    char detail[160] = "not captured";
                    if (index < known.size()) {
                        const auto& value = known[index];
                        std::snprintf(detail, sizeof(detail), "%s %u words, available mask %02x, words %08x %08x %08x %08x", index < infos.size() && infos[index].Pointer() ? "pointer" : "descriptor", value.dwordCount, static_cast<unsigned>(value.availableDwordMask), value.dwords[0], value.dwords[1], value.dwords[2], value.dwords[3]);
                    }
                    Downgrades::Note(stage.program.codeAddress, Downgrades::Kind::KnownNull, index, detail);
                }
                write(zero, 0u);
                continue;
            }
            if (index < infos.size() && infos[index].written && mirrorPart("writes")) {
                auto served = bytes;
                auto mapped = mirrorKnownWrite(address, bytes, serial);
                if (mapped == 0u) {
                    const auto committed = GuestMemory::CommittedRanges(address, static_cast<std::size_t>(bytes));
                    if (!committed.empty() && committed.front().first == address && committed.front().second > address && committed.front().second - address < bytes) {
                        served = committed.front().second - address;
                        mapped = mirrorKnownWrite(address, served, serial);
                    }
                }
                noteKnownPath(mapped != 0u ? "write-mirror" : "write-refused", address, served);
                if (mapped != 0u) {
                    totals.knownWritten++;
                    if (served < bytes) noteKnown(stage, Downgrades::Kind::KnownPrefix, index, address, bytes, served);
                } else {
                    totals.knownWriteLookups++;
                    noteKnown(stage, Downgrades::Kind::KnownWriteRefused, index, address, bytes, 0u);
                }
                write(mapped, mapped != 0u ? served : 0u);
                continue;
            }
            if (index < infos.size() && infos[index].written) {
                auto served = bytes;
                if (const auto direct = directSmallWrite(context, address, bytes); direct != 0u) {
                    noteKnownPath("write-direct", address, bytes);
                    totals.knownWritten++;
                    write(direct, bytes);
                    continue;
                }
                const auto mapped = serveKnownWrite(context, address, served, serial, queueCopyBack(context, address, bytes));
                noteKnownPath(mapped != 0u ? "write-served" : "write-refused", address, served);
                if (mapped != 0u) totals.knownWritten++;
                else totals.knownWriteLookups++;
                if (mapped == 0u) noteKnown(stage, Downgrades::Kind::KnownWriteRefused, index, address, bytes, 0u);
                else if (served < bytes) noteKnown(stage, Downgrades::Kind::KnownPrefix, index, address, bytes, served);
                write(mapped, mapped != 0u ? served : 0u);
                continue;
            }
            const bool preferOwned = preferOwnedReads && index < infos.size() && !infos[index].Pointer();
            const auto copyReadOnly = [&] {
                const auto copied = copyKnownBuffer(address, bytes, serial, completed);
                if (copied == 0u) return false;
                totals.knownCopied++;
                noteKnownPath("copied", address, bytes);
                write(copied, bytes);
                return true;
            };
            if (!preferOwned && copyReadOnly()) continue;
            const bool large = bytes >= LargeKnownBytes;
            const auto removals = large ? ResidencyService::Get().Removals() : 0u;
            if (large) {
                if (const auto found = largeKnown.find(address); found != largeKnown.end() && found->second.bytes == bytes && found->second.removals == removals) {
                    totals.knownMapped++;
                    noteKnownPath("mapped", address, bytes);
                    write(found->second.mapped, bytes);
                    continue;
                }
            }
            const auto reach = std::max(bytes, KnownReadableBytes);
            RouteMemory::OwnedSpan span;
            if (bytes <= KnownBufferLimitBytes && !MemoryCore::Get().Pages().GpuWrittenIn(address, reach) && ((reach != bytes && RouteMemory::ServeOwned(context, address, reach, &span)) || RouteMemory::ServeOwned(context, address, bytes, &span)) && span.address != 0u) {
                totals.knownServed++;
                noteKnownPath("owned", address, bytes);
                write(span.address, bytes);
                continue;
            }
            if (preferOwned && copyReadOnly()) continue;
            auto mapped = ResidencyService::Get().TranslateMapped(address, reach);
            if (mapped == 0u && reach != bytes) mapped = ResidencyService::Get().TranslateMapped(address, bytes);
            if (mapped == 0u && bytes <= KnownBufferLimitBytes && servesFaultPages()) {
                if (const auto pages = ResidencyService::Get().UnmappedPages(address, bytes); !pages.empty()) {
                    RouteMemory::ServeFaultPages(pages);
                    mapped = ResidencyService::Get().TranslateMapped(address, bytes);
                    if (mapped != 0u) totals.knownPaged++;
                }
            }
            if (mapped != 0u) {
                totals.knownMapped++;
                noteKnownPath("mapped", address, bytes);
                if (large) largeKnown[address] = {bytes, mapped, removals};
                write(mapped, bytes);
                continue;
            }
            if (servesKnownPrefixes()) {
                if (const auto [device, length] = ResidencyService::Get().MappedPrefix(address, bytes); device != 0u) {
                    totals.knownPrefixes++;
                    noteKnown(stage, Downgrades::Kind::KnownPrefix, index, address, bytes, length);
                    write(device, length);
                    continue;
                }
            }
            totals.knownRefused++;
            noteKnown(stage, Downgrades::Kind::KnownRefused, index, address, bytes, 0u);
            write(zero, 0u);
        }
    }

    struct GpuWatch {
        std::mutex mutex;
        std::vector<std::byte> last;
        std::uint32_t lines = 0;
    };

    static GpuWatch& gpuWatch() {
        static GpuWatch watch;
        return watch;
    }

    void watchOnGpu(const Context& context, Graphics::Recorder& recorder, VkCommandBuffer commands, std::uint64_t program, std::uint32_t queue, std::uint64_t submission) {
        static const std::pair<std::uint64_t, std::uint64_t> range = [] {
            const char* value = std::getenv("APS5_RECODE_GPU_WATCH");
            if (value == nullptr) return std::pair<std::uint64_t, std::uint64_t>{0u, 0u};
            const char* plus = std::strchr(value, '+');
            return std::pair<std::uint64_t, std::uint64_t>{std::strtoull(value, nullptr, 16), plus != nullptr ? std::min<std::uint64_t>(std::strtoull(plus + 1, nullptr, 16), 65536u) : 0u};
        }();
        constexpr std::uint32_t Slots = 256;
        if (range.second == 0u) return;
        const auto span = ResidencyService::Get().Translate(range.first, range.second);
        if (!span || span->handle == 0u) return;
        if (gpuWatchBuffer == nullptr) gpuWatchBuffer = std::make_shared<Graphics::Buffer>(context, Slots * range.second, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto slot = nextGpuWatch++ % Slots;
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy region{span->offset, slot * range.second, range.second};
        context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span->handle)), gpuWatchBuffer->Handle(), 1, &region);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Keep(gpuWatchBuffer);
        recorder.OnComplete([buffer = gpuWatchBuffer, slot, bytes = range.second, base = range.first, program, queue, submission] {
            const auto* now = buffer->Bytes().data() + slot * bytes;
            auto& watch = gpuWatch();
            std::lock_guard lock(watch.mutex);
            if (watch.last.size() != bytes) {
                watch.last.assign(now, now + bytes);
                return;
            }
            std::uint64_t first = bytes;
            std::uint64_t lastChanged = 0;
            std::uint64_t changed = 0;
            for (std::uint64_t index = 0; index < bytes; ++index) {
                if (now[index] == watch.last[index]) continue;
                if (first == bytes) first = index;
                lastChanged = index;
                ++changed;
            }
            if (changed != 0u && watch.lines < 400u) {
                ++watch.lines;
                const auto at = first & ~std::uint64_t{3};
                std::uint32_t before = 0;
                std::uint32_t after = 0;
                std::memcpy(&before, watch.last.data() + at, 4u);
                std::memcpy(&after, now + at, 4u);
                std::fprintf(stderr, "[recode-gpu-watch] queue 0x%x submission %llu program %016llx changed %llu bytes, first at 0x%llx: %08x -> %08x, last at 0x%llx\n", queue, static_cast<unsigned long long>(submission), static_cast<unsigned long long>(program), static_cast<unsigned long long>(changed), static_cast<unsigned long long>(base + first), before, after, static_cast<unsigned long long>(base + lastChanged));
            }
            watch.last.assign(now, now + bytes);
        });
    }

    struct InputCompareTotals {
        std::mutex mutex;
        std::map<std::pair<std::uint64_t, int>, std::pair<std::uint64_t, std::uint64_t>> counts;
        std::uint64_t compared = 0;
        std::uint32_t lines = 0;
    };

    static InputCompareTotals& inputCompareTotals() {
        static InputCompareTotals totals;
        return totals;
    }

    void compareInput(const Context& context, Graphics::Recorder& recorder, std::uint64_t address, std::uint64_t bytes, const DrawInput& input) {
        static const std::uint64_t every = [] {
            const char* value = std::getenv("APS5_RECODE_INPUT_COMPARE");
            return value != nullptr ? std::max<std::uint64_t>(1u, std::strtoull(value, nullptr, 10)) : 0u;
        }();
        constexpr std::uint32_t Slots = 512;
        constexpr VkDeviceSize SlotBytes = 65536;
        if (every == 0u || bytes == 0u || ++inputCompareTick % every != 0u) return;
        bytes = std::min<std::uint64_t>(bytes, SlotBytes);
        const auto* guest = reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(address));
        if (!GuestMemory::Accessible(guest, static_cast<std::size_t>(bytes), false)) return;
        auto& pages = MemoryCore::Get().Pages();
        const int state = gpuPending(address, bytes) ? 3 : !pages.GpuOwnedIn(address, bytes).empty() ? 2 : pages.GpuWrittenIn(address, bytes) ? 1 : pages.CleanIn(address, bytes) ? 0 : 4;
        std::vector<std::byte> before(guest, guest + bytes);
        if (inputCompares == nullptr) inputCompares = std::make_shared<Graphics::Buffer>(context, Slots * SlotBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto slot = nextInputCompare++ % Slots;
        const auto commands = recorder.Commands();
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy region{input.offset, slot * SlotBytes, bytes};
        context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, input.buffer, inputCompares->Handle(), 1, &region);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Keep(inputCompares);
        recorder.OnComplete([buffer = inputCompares, slot, address, bytes, state, before = std::move(before)] {
            const auto* mirrored = buffer->Bytes().data() + slot * SlotBytes;
            std::uint64_t differing = 0;
            for (std::uint64_t index = 0; index < bytes; ++index) {
                if (mirrored[index] != before[index]) ++differing;
            }
            auto& totals = inputCompareTotals();
            std::lock_guard lock(totals.mutex);
            auto& entry = totals.counts[{address >> 28u, state}];
            ++entry.first;
            if (differing != 0u) ++entry.second;
            static const char* const states[] = {"clean", "gpu-written", "gpu-owned", "gpu-pending", "cpu-dirty"};
            if (differing != 0u && totals.lines < 60u) {
                ++totals.lines;
                std::uint32_t mirrorWord = 0;
                std::uint32_t guestWord = 0;
                std::memcpy(&mirrorWord, mirrored, std::min<std::size_t>(4u, static_cast<std::size_t>(bytes)));
                std::memcpy(&guestWord, before.data(), std::min<std::size_t>(4u, static_cast<std::size_t>(bytes)));
                std::fprintf(stderr, "[recode-input-compare] 0x%llx+0x%llx %s: %llu bytes differ (mirror %08x, guest at record %08x)\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), states[state], static_cast<unsigned long long>(differing), mirrorWord, guestWord);
            }
            if (++totals.compared % 5000u == 0u) {
                std::string line;
                for (const auto& [key, value] : totals.counts) {
                    char region[32];
                    std::snprintf(region, sizeof(region), " 0x%llx", static_cast<unsigned long long>(key.first << 28u));
                    line += region + std::string("/") + states[key.second] + " " + std::to_string(value.second) + "/" + std::to_string(value.first);
                }
                std::fprintf(stderr, "[recode-input-compare] %llu compared; differing/compared by 256 MiB region and page state:%s\n", static_cast<unsigned long long>(totals.compared), line.c_str());
            }
        });
    }

    void readIndirectGroups(const Context& context, Graphics::Recorder& recorder, VkCommandBuffer commands, VkBuffer arguments, VkDeviceSize offset, std::uint64_t codeAddress) {
        constexpr std::uint32_t Slots = 4096;
        constexpr VkDeviceSize SlotBytes = 16;
        if (!Downgrades::Active()) return;
        if (groupReads == nullptr) groupReads = std::make_shared<Graphics::Buffer>(context, Slots * SlotBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto slot = nextGroupRead++ % Slots;
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy region{offset, slot * SlotBytes, 3u * sizeof(std::uint32_t)};
        context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, arguments, groupReads->Handle(), 1, &region);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Keep(groupReads);
        recorder.OnComplete([buffer = groupReads, slot, codeAddress] {
            std::uint32_t counts[3] = {};
            std::memcpy(counts, buffer->Bytes().data() + slot * SlotBytes, sizeof(counts));
            Downgrades::NoteGroups(codeAddress, static_cast<std::uint64_t>(counts[0]) * counts[1] * counts[2]);
        });
    }

    void probeDrawArguments(const Context& context, Graphics::Recorder& recorder, VkCommandBuffer commands, const Pm4::DrawParameters::IndirectDraw& args, bool indexed, const char* source, VkBuffer arguments, VkDeviceSize offset, const std::optional<DrawInput>& count, std::uint64_t codeAddress) {
        constexpr std::uint32_t Slots = 1024;
        constexpr VkDeviceSize SlotBytes = 32;
        constexpr VkDeviceSize CountOffset = 24;
        if (!Downgrades::Active() || args.count == 0u || args.recordBytes > CountOffset) return;
        const auto seen = ++argumentProbes[codeAddress];
        const bool census = argumentCensusOn();
        if (!census && seen > 2u && seen % 200u != 0u) return;
        if (argumentReads == nullptr) argumentReads = std::make_shared<Graphics::Buffer>(context, Slots * SlotBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto slot = nextArgumentRead++ % Slots;
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        Graphics::CopyBuffer(context, commands, arguments, offset, argumentReads->Handle(), slot * SlotBytes, args.recordBytes);
        if (count) Graphics::CopyBuffer(context, commands, count->buffer, count->offset, argumentReads->Handle(), slot * SlotBytes + CountOffset, sizeof(std::uint32_t));
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Keep(argumentReads);
        recorder.OnComplete([buffer = argumentReads, slot, codeAddress, indexed, source, seen, records = args.count, guest = args.arguments, counted = count.has_value(), census] {
            std::uint32_t words[8] = {};
            std::memcpy(words, buffer->Bytes().data() + slot * SlotBytes, sizeof(words));
            if (census) noteArgumentCensus(codeAddress, words[0], words[1], counted ? words[6] : 1u);
            if (census && seen > 2u && seen % 200u != 0u) return;
            char counter[48] = "";
            if (counted) std::snprintf(counter, sizeof(counter), ", count buffer holds %u", words[6]);
            if (indexed) std::fprintf(stderr, "[recode-arguments] %016llx indirect draw %u (%s, %u records at 0x%llx%s): first record indices %u instances %u first index %u vertex offset %d first instance %u\n", static_cast<unsigned long long>(Downgrades::ProgramHash(codeAddress)), seen, source, records, static_cast<unsigned long long>(guest), counter, words[0], words[1], words[2], static_cast<std::int32_t>(words[3]), words[4]);
            else std::fprintf(stderr, "[recode-arguments] %016llx indirect draw %u (%s, %u records at 0x%llx%s): first record vertices %u instances %u first vertex %u first instance %u\n", static_cast<unsigned long long>(Downgrades::ProgramHash(codeAddress)), seen, source, records, static_cast<unsigned long long>(guest), counter, words[0], words[1], words[2], words[3]);
        });
    }

    static void noteImage(const Resolve::ResolvedStage& stage, Downgrades::Kind kind, std::uint32_t index, const Heap::SlotRequest& request, const char* what) {
        if (!Downgrades::Active()) return;
        char text[256];
        try {
            const auto resource = Graphics::DecodeTextureResource(request.words);
            std::snprintf(text, sizeof(text), "%s 0x%llx %ux%u fmt %u mips %u", what, static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, resource.mipCount);
        } catch (const std::exception&) {
            std::snprintf(text, sizeof(text), "%s (undecodable)", what);
        }
        Downgrades::Note(stage.program.codeAddress, kind, index, text);
    }

    static void noteKnown(const Resolve::ResolvedStage& stage, Downgrades::Kind kind, std::uint32_t index, std::uint64_t address, std::uint64_t bytes, std::uint64_t served) {
        if (!Downgrades::Active()) return;
        char text[96];
        std::snprintf(text, sizeof(text), "0x%llx+0x%llx served 0x%llx", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(served));
        if (kind == Downgrades::Kind::KnownPrefix && served * 2u < bytes) kind = Downgrades::Kind::KnownShort;
        Downgrades::Note(stage.program.codeAddress, kind, index, text);
    }

    std::optional<std::uint64_t> ringCopy(std::uint64_t address, std::uint64_t bytes, std::uint64_t serial, std::uint64_t completed) {
        if (!GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(address), static_cast<std::size_t>(bytes))) return std::nullopt;
        const auto committed = GuestMemory::CommittedRanges(address, static_cast<std::size_t>(bytes));
        if (committed.size() != 1u || committed.front().first > address || committed.front().second < address + bytes) return std::nullopt;
        if (!MemoryCore::Get().Pages().GpuOwnedIn(address, bytes).empty() || gpuPending(address, bytes)) return std::nullopt;
        return ringWrite(std::span(reinterpret_cast<const std::byte*>(address), static_cast<std::size_t>(bytes)), serial, completed);
    }

    std::uint64_t copyKnownBuffer(std::uint64_t address, std::uint64_t bytes, std::uint64_t serial, std::uint64_t completed) {
        if (ringAddress == 0u || bytes > KnownCopyLimitBytes) return 0u;
        if ((refusesGpuWrittenCopies() && MemoryCore::Get().Pages().GpuWrittenIn(address, bytes)) || gpuPending(address, bytes)) {
            totals.knownGpuWritten++;
            return 0u;
        }
        const auto offset = ringCopy(address, bytes, serial, completed);
        return offset.has_value() ? ringAddress + *offset : 0u;
    }

    struct InputCopy {
        std::uint64_t address = 0;
        std::uint64_t bytes = 0;
        std::size_t merged = 0;
    };

    struct CopiedSpan {
        std::uint64_t address = 0;
        std::uint64_t bytes = 0;
        VkDeviceSize offset = 0;
    };

    struct ArgumentPatch {
        VkBuffer source = VK_NULL_HANDLE;
        VkDeviceSize sourceOffset = 0;
        VkDeviceSize ringOffset = 0;
    };

    std::optional<DrawInput> copiedInput(std::vector<CopiedSpan>& done, std::uint64_t address, std::uint64_t bytes, std::uint64_t serial, std::uint64_t completed) {
        for (const auto& copy : done) {
            if (copy.address <= address && address + bytes <= copy.address + copy.bytes) return DrawInput{ring->Handle(), copy.offset + (address - copy.address)};
        }
        const auto offset = ringCopy(address, bytes, serial, completed);
        if (!offset.has_value()) return std::nullopt;
        done.push_back({address, bytes, *offset});
        return DrawInput{ring->Handle(), *offset};
    }

    void servePushKnownBuffers(const Context& context, const Resolve::ResolvedStage& stage, std::span<std::byte> block, std::uint64_t serial, std::uint64_t completed) {
        if (stage.program.variant == nullptr || !stage.program.variant->info.bindings.UsesPushData()) return;
        serveKnownBuffers(context, stage, serial, completed, [&](std::uint32_t dword, std::uint32_t value) {
            const auto offset = static_cast<std::size_t>(dword) * sizeof(std::uint32_t);
            if (offset + sizeof(value) <= block.size()) std::memcpy(block.data() + offset, &value, sizeof(value));
        });
    }

    bool writeSet(const Context& context, const Resolve::ResolvedDraw& draw, std::uint32_t set, VkDescriptorSet target, std::uint64_t serial, std::uint64_t completed, std::shared_ptr<Graphics::Buffer>& pageTable, std::string& reason) {
        std::vector<VkDescriptorBufferInfo> infos;
        std::vector<VkWriteDescriptorSet> writes;
        infos.reserve(draw.layout.bindings.size());
        for (const auto& binding : draw.layout.bindings) {
            if (binding.set != set) continue;
            VkDescriptorBufferInfo info{};
            switch (binding.source) {
                case Bindings::Source::Words: {
                    const auto& stage = draw.stages.at(binding.stage);
                    const auto words = wordsOf(stage, binding.index);
                    const bool shaderData = shaderDataBinding(stage, binding.index) && stage.program.variant != nullptr;
                    const auto dataBytes = shaderData ? static_cast<std::size_t>(stage.program.variant->info.bindings.ShaderDataDwords()) * sizeof(std::uint32_t) : 0u;
                    std::vector<std::byte> bytes(std::max<std::size_t>({words.size_bytes(), dataBytes, 16u}));
                    std::memcpy(bytes.data(), words.data(), words.size_bytes());
                    if (shaderData) {
                        serveKnownBuffers(context, stage, serial, completed, [&](std::uint32_t dword, std::uint32_t value) {
                            const auto offset = static_cast<std::size_t>(dword) * sizeof(std::uint32_t);
                            if (offset + sizeof(value) <= bytes.size()) std::memcpy(bytes.data() + offset, &value, sizeof(value));
                        });
                    }
                    const auto offset = ringWrite(bytes, serial, completed);
                    if (!offset.has_value()) {
                        reason = "ring full";
                        return false;
                    }
                    info = {ring->Handle(), *offset, bytes.size()};
                    break;
                }
                case Bindings::Source::PageTable:
                    if (pageTable == nullptr) pageTable = ResidencyService::Get().MirrorTable();
                    if (pageTable == nullptr) {
                        reason = "no GPU page table";
                        return false;
                    }
                    info = {pageTable->Handle(), 0, VK_WHOLE_SIZE};
                    break;
                case Bindings::Source::Fault: info = {faultFor(context, serial, completed)->Handle(), 0, VK_WHOLE_SIZE}; break;
                case Bindings::Source::TableMap: {
                    const auto& mirror = draw.stages.at(binding.stage).tableMap;
                    if (mirror == nullptr) {
                        reason = "table map without a mirror";
                        return false;
                    }
                    info = {mapBuffer(context, *mirror, serial, completed)->Handle(), 0, VK_WHOLE_SIZE};
                    break;
                }
                case Bindings::Source::Gds: {
                    const auto gds = Pm4::GdsAddress();
                    MemoryCore::Get().Protection().NeverArm(gds, Pm4::GdsBytes);
                    static_cast<void>(MemoryCore::Get().OpenCleanForImport(gds, Pm4::GdsBytes));
                    const auto* import = Graphics::HostImportFor(context, gds, Pm4::GdsBytes);
                    if (import == nullptr) {
                        reason = "GDS not imported";
                        return false;
                    }
                    info = {import->buffer, Graphics::HostImportOffset(*import, gds), Pm4::GdsBytes};
                    const std::array written{std::pair{gds, gds + Pm4::GdsBytes}};
                    Events::NoteRanges(written);
                    if (context.recorder != nullptr) context.recorder->NotePendingWrite(gds, Pm4::GdsBytes);
                    totals.gdsBindings++;
                    break;
                }
                default: reason = "guest buffer binding"; return false;
            }
            infos.push_back(info);
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = target;
            write.dstBinding = binding.binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &infos.back();
            writes.push_back(write);
        }
        if (!writes.empty()) context.Resolved(&Graphics::DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0u, nullptr);
        return true;
    }

    VkDescriptorSetLayout emptySetLayout(const Context& context) {
        std::lock_guard layoutLock(layoutMutex);
        if (emptyLayout != VK_NULL_HANDLE) return emptyLayout;
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        Graphics::Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &info, nullptr, &emptyLayout), "vkCreateDescriptorSetLayout recode dispatch");
        return emptyLayout;
    }

    static std::vector<std::uint64_t> pipelineKey(const Resolve::ResolvedDraw& dispatch) {
        const auto hash = dispatch.stages.front().program.result->spirv.ContentHash();
        std::vector<std::uint64_t> key{hash.low, hash.high};
        key.insert(key.end(), dispatch.layout.keys[0].begin(), dispatch.layout.keys[0].end());
        return key;
    }

    static std::shared_ptr<ComputePipeline> buildComputePipeline(const PipelineJob& job) {
        const auto& context = job.context;
        const auto& result = *job.result;
        auto made = std::make_shared<ComputePipeline>();
        made->device = context.device;
        made->destroyPipeline = context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        made->destroyLayout = context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
        const std::array<VkDescriptorSetLayout, ShaderRecompiler::RuntimeAbi::DirectHeapSet + 1u> setLayouts{job.main, job.empty, job.empty, job.empty, job.heap};
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, Graphics::ComputePushConstantBytes};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
        layoutInfo.pSetLayouts = setLayouts.data();
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        Graphics::Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &made->layout), "vkCreatePipelineLayout recode dispatch");
        const Graphics::PipelineSpecialization specialization(result, context.atomicLockTable);
        const auto module = Graphics::CreateShaderModule(context, result.spirv.data(), result.spirv.size() * sizeof(std::uint32_t), "vkCreateShaderModule recode dispatch");
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.flags = job.flags;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.flags = job.stageFlags;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.stage.pSpecializationInfo = specialization.Info();
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT subgroup{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT};
        subgroup.requiredSubgroupSize = job.subgroupSize;
        if (job.subgroupSize != 0u) info.stage.pNext = &subgroup;
        info.layout = made->layout;
        VkPipelineCreationFeedback whole{};
        VkPipelineCreationFeedback stage{};
        VkPipelineCreationFeedbackCreateInfo feedback{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
        feedback.pPipelineCreationFeedback = &whole;
        feedback.pipelineStageCreationFeedbackCount = 1;
        feedback.pPipelineStageCreationFeedbacks = &stage;
        if (Downgrades::Active()) info.pNext = &feedback;
        VkResult created = VK_SUCCESS;
        const auto lockStarted = std::chrono::steady_clock::now();
        {
            const auto cacheLock = context.LockPipelineCacheForUse();
            made->lockNanoseconds = elapsed(lockStarted);
            created = context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, context.pipelineCache, 1, &info, nullptr, &made->pipeline);
        }
        made->feedback = whole.flags;
        context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        Graphics::Check(created, "vkCreateComputePipelines recode dispatch");
        return made;
    }

public:
    void Prebuild(const Context& context, VulkanDevice& owner, const Resolve::ResolvedDraw& dispatch) {
        std::string reason;
        if (attachedDevice.load(std::memory_order_acquire) != context.device || dispatch.stages.size() != 1u || dispatch.stages.front().program.result == nullptr || !dispatchPrecheck(dispatch, reason)) return;
        const auto key = pipelineKey(dispatch);
        startListReplay(context, owner);
        std::lock_guard pipelineLock(pipelineMutex);
        if (computePipelines.contains(key) || buildingPipelines.contains(key) || failedPipelines.contains(key)) return;
        if (queuePipeline(context, owner, dispatch, key, reason)) pipelinesPrebuilt.fetch_add(1, std::memory_order_relaxed);
    }

private:

    bool pipelineReady(const Context& context, VulkanDevice& owner, const Resolve::ResolvedDraw& dispatch, std::unique_lock<std::mutex>& lock, std::string& reason) {
        const auto key = pipelineKey(dispatch);
        startListReplay(context, owner);
        std::unique_lock pipelineLock(pipelineMutex);
        if (computePipelines.contains(key)) return true;
        if (!queuePipeline(context, owner, dispatch, key, reason)) return false;
        if (buildsPipelinesAhead()) return false;
        const auto queued = std::ranges::find_if(pipelineJobs, [&](const PipelineJob& job) { return job.key == key; });
        if (queued != pipelineJobs.end() && queued != pipelineJobs.begin()) {
            const auto ahead = std::distance(pipelineJobs.begin(), queued);
            std::rotate(pipelineJobs.begin(), queued, std::next(queued));
            if (Downgrades::Active()) std::fprintf(stderr, "[recode-pipeline-priority] requested compute pipeline moved ahead of %lld queued builds\n", static_cast<long long>(ahead));
        }
        const auto started = std::chrono::steady_clock::now();
        lock.unlock();
        ++framesWaiting;
        jobsReady.notify_all();
        pipelinesDone.wait(pipelineLock, [&] { return !buildingPipelines.contains(key); });
        --framesWaiting;
        pipelineLock.unlock();
        lock.lock();
        pipelineLock.lock();
        totals.pipelineWaits++;
        const auto waited = elapsed(started);
        totals.pipelineWaitNanoseconds += waited;
        if (Downgrades::Active() && !dispatch.stages.empty()) Downgrades::NoteCompile(dispatch.stages.front().program.codeAddress, waited);
        if (computePipelines.contains(key)) return true;
        if (const auto failed = failedPipelines.find(key); failed != failedPipelines.end()) reason = "compute pipeline failed: " + failed->second;
        return false;
    }

    static const std::filesystem::path& pipelineListDirectory() {
        static const std::filesystem::path directory = PipelineList::Directory();
        return directory;
    }

    static void listPipeline(const PipelineJob& job) {
        const auto& directory = pipelineListDirectory();
        if (directory.empty() || PipelineList::Listed(directory, job.key)) return;
        PipelineList::Entry entry{job.key, {}};
        ShaderRecompiler::ShaderDiskCache::EncodeResult(*job.result, entry.result);
        PipelineList::Store(directory, entry);
    }

    void startListReplay(const Context& context, VulkanDevice& owner) {
        if (listReplayStarted.exchange(true, std::memory_order_acq_rel) || pipelineListDirectory().empty()) return;
        listReplaying.store(true, std::memory_order_release);
        std::thread([this, context, &owner] {
            replayList(context, owner);
            listReplaying.store(false, std::memory_order_release);
        }).detach();
    }

    VkDescriptorSetLayout listedSetLayout(const Context& context, std::span<const std::uint64_t> triplets) {
        std::vector<std::uint32_t> key(triplets.begin(), triplets.end());
        std::lock_guard layoutLock(layoutMutex);
        if (const auto found = layouts.find(key); found != layouts.end()) return found->second;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (std::size_t index = 0; index + 3u <= key.size(); index += 3u) {
            const VkShaderStageFlags stages = (key[index + 2u] & Bindings::StageBit(ShaderRecompiler::ShaderStage::Compute)) != 0u ? VK_SHADER_STAGE_COMPUTE_BIT : 0u;
            bindings.push_back({key[index], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, key[index + 1u], stages, nullptr});
        }
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        Graphics::Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &info, nullptr, &layout), "vkCreateDescriptorSetLayout recode pipeline list");
        layouts.emplace(std::move(key), layout);
        return layout;
    }

    void replayList(const Context& context, VulkanDevice& owner) {
        const auto started = std::chrono::steady_clock::now();
        const auto files = PipelineList::Newest(pipelineListDirectory());
        std::uint32_t queued = 0;
        for (const auto& file : files) {
            auto loaded = PipelineList::Load(file);
            if (!loaded.has_value() || loaded->key.size() < 2u || (loaded->key.size() - 2u) % 3u != 0u) continue;
            {
                std::lock_guard lock(pipelineMutex);
                if (computePipelines.contains(loaded->key) || buildingPipelines.contains(loaded->key) || failedPipelines.contains(loaded->key)) continue;
            }
            const auto held = std::make_shared<const PipelineList::Entry>(std::move(*loaded));
            auto result = std::make_shared<ShaderRecompiler::RecompileResult>();
            if (!ShaderRecompiler::ShaderDiskCache::DecodeResult(held->result, *result, held)) continue;
            const auto hash = result->spirv.ContentHash();
            if (hash.low != held->key[0] || hash.high != held->key[1]) continue;
            const auto* heap = Resolve::HeapDevice();
            if (heap == nullptr || attachedDevice.load(std::memory_order_acquire) != context.device || listReplayStops.load(std::memory_order_acquire)) break;
            try {
                const auto main = listedSetLayout(context, std::span(held->key).subspan(2));
                std::lock_guard lock(pipelineMutex);
                if (computePipelines.contains(held->key) || buildingPipelines.contains(held->key) || failedPipelines.contains(held->key)) continue;
                PipelineJob job{held->key, context, result, main, heap->Layout(), emptySetLayout(context), owner.computePipelineFlags(), owner.computeStageFlags(*result), owner.computeSubgroupSize(*result), &owner, 0u};
                job.spirvBytes = result->spirv.size() * sizeof(std::uint32_t);
                job.listed = true;
                buildingPipelines.insert(job.key);
                pipelineJobs.push_back(std::move(job));
                listedJobs++;
                queued++;
                const std::size_t wanted = std::min<std::size_t>(BuildBudget::Threads(BuildBudget::Host()), pipelineJobs.size() + runningBuilds.builds);
                while (builders.size() < wanted) builders.emplace_back([this] { buildPipelines(); });
                jobsReady.notify_all();
            } catch (const std::exception&) {
                continue;
            }
        }
        std::fprintf(stderr, "[recode-pipeline-list] %u of %zu listed compute pipelines queued in %.0f ms\n", queued, files.size(), static_cast<double>(elapsed(started)) / 1e6);
    }

    struct PipelineCounts {
        std::size_t held = 0;
        std::size_t building = 0;
        std::size_t failed = 0;
        std::uint64_t built = 0;
        std::uint64_t nanoseconds = 0;
        std::uint32_t builders = 0;
        std::uint32_t mostAtOnce = 0;
    };

    PipelineCounts takePipelineCounts() {
        std::lock_guard pipelineLock(pipelineMutex);
        PipelineCounts counts{computePipelines.size(), buildingPipelines.size(), failedPipelines.size(), builtPipelines, buildNanoseconds, static_cast<std::uint32_t>(builders.size()), mostBuildsAtOnce};
        builtPipelines = 0;
        buildNanoseconds = 0;
        mostBuildsAtOnce = runningBuilds.builds;
        return counts;
    }

    std::shared_ptr<ComputePipeline> builtPipeline(const std::vector<std::uint64_t>& key) {
        std::lock_guard pipelineLock(pipelineMutex);
        const auto found = computePipelines.find(key);
        return found != computePipelines.end() ? found->second : nullptr;
    }

    bool queuePipeline(const Context& context, VulkanDevice& owner, const Resolve::ResolvedDraw& dispatch, std::vector<std::uint64_t> key, std::string& reason) {
        if (const auto failed = failedPipelines.find(key); failed != failedPipelines.end()) {
            reason = "compute pipeline failed: " + failed->second;
            return false;
        }
        reason = "compute pipeline building";
        if (buildingPipelines.contains(key)) return true;
        const auto* heap = Resolve::HeapDevice();
        if (heap == nullptr) {
            reason = "no heap";
            return false;
        }
        const auto& result = dispatch.stages.front().program.result;
        PipelineJob job{key, context, result, setLayout(context, dispatch, 0), heap->Layout(), emptySetLayout(context), owner.computePipelineFlags(), 0u, owner.computeSubgroupSize(*result), &owner, dispatch.stages.front().program.codeAddress};
        try {
            job.stageFlags = owner.computeStageFlags(*result);
        } catch (const std::exception& error) {
            failedPipelines.emplace(std::move(key), error.what());
            reason = std::string("compute pipeline failed: ") + error.what();
            return false;
        }
        job.spirvBytes = result->spirv.size() * sizeof(std::uint32_t);
        buildingPipelines.insert(key);
        pipelineJobs.push_back(std::move(job));
        pipelinesQueued.fetch_add(1, std::memory_order_relaxed);
        const std::size_t wanted = std::min<std::size_t>(BuildBudget::Threads(BuildBudget::Host()), pipelineJobs.size() + runningBuilds.builds);
        while (builders.size() < wanted) builders.emplace_back([this] { buildPipelines(); });
        jobsReady.notify_all();
        return true;
    }

    void buildPipelines() {
        for (;;) {
            std::optional<PipelineJob> job;
            {
                std::unique_lock lock(pipelineMutex);
                jobsReady.wait(lock, [&] { return !pipelineJobs.empty() && BuildBudget::Admits(BuildBudget::Host(), framesWaiting != 0u || listedJobs != 0u, runningBuilds, pipelineJobs.front().spirvBytes); });
                job.emplace(std::move(pipelineJobs.front()));
                pipelineJobs.pop_front();
                runningBuilds.builds++;
                runningBuilds.memoryBytes += BuildBudget::MemoryBytes(job->spirvBytes);
                mostBuildsAtOnce = std::max(mostBuildsAtOnce, runningBuilds.builds);
            }
            const auto started = std::chrono::steady_clock::now();
            std::shared_ptr<ComputePipeline> made;
            std::string failure;
            try {
                made = buildComputePipeline(*job);
            } catch (const std::exception& error) {
                failure = error.what();
            }
            const auto built = elapsed(started);
            if (made != nullptr && Downgrades::Active() && job->result != nullptr) std::fprintf(stderr, "[recode-build] %016llx module %016llx %s %.0f ms (cache lock %.0f ms)\n", static_cast<unsigned long long>(Downgrades::ProgramHash(job->codeAddress)), static_cast<unsigned long long>(job->result->spirv.ContentHash().low), (made->feedback & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) == 0u ? "no-feedback" : (made->feedback & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0u ? "cache-hit" : "cache-miss", static_cast<double>(built) / 1e6, static_cast<double>(made->lockNanoseconds) / 1e6);
            if (job->result != nullptr) Downgrades::DumpModule(job->codeAddress, job->result->spirv.ContentHash().low, std::span<const std::uint32_t>(job->result->spirv.data(), job->result->spirv.size()));
            if (made != nullptr && !job->listed && job->result != nullptr) listPipeline(*job);
            Downgrades::NoteBuild(job->codeAddress, built, job->result != nullptr ? job->result->spirv.size() * sizeof(std::uint32_t) : 0u, job->result != nullptr ? job->result->spirv.ContentHash().low : 0u, job->key.size() > 2u ? Downgrades::HashCode(std::as_bytes(std::span(job->key).subspan(2))) : 0u);
            {
                std::lock_guard lock(pipelineMutex);
                runningBuilds.builds--;
                runningBuilds.memoryBytes -= BuildBudget::MemoryBytes(job->spirvBytes);
                if (job->listed) {
                    listedJobs--;
                    if (made != nullptr) pipelinesReplayed.fetch_add(1, std::memory_order_relaxed);
                }
                buildingPipelines.erase(job->key);
                buildNanoseconds += built;
                unsavedBuildNanoseconds += made != nullptr ? built : 0u;
                if (unsavedBuildNanoseconds >= UnsavedBuildNanoseconds && job->owner != nullptr) {
                    job->owner->RequestPipelineCacheSave();
                    unsavedBuildNanoseconds = 0;
                }
                if (made != nullptr) {
                    computePipelines.emplace(std::move(job->key), std::move(made));
                    builtPipelines++;
                } else {
                    if (pipelineFailuresSeen.insert(failure.substr(0, 50)).second) std::fprintf(stderr, "[recode-record] first compute pipeline failure: %s\n", failure.c_str());
                    failedPipelines.emplace(std::move(job->key), std::move(failure));
                }
                pipelinesDone.notify_all();
                jobsReady.notify_all();
            }
        }
    }

    static void traceSourceWrites(const Resolve::ResolvedDraw& work) {
        if (sourcesOfProgram() == 0 || work.stages.empty()) return;
        static const long long after = [] {
            const char* value = std::getenv("APS5_RECODE_TRACE_SOURCES_AFTER");
            return value == nullptr ? 0ll : std::strtoll(value, nullptr, 10);
        }();
        if (std::chrono::steady_clock::now() - processStart() < std::chrono::seconds(after)) return;
        const auto program = Downgrades::ProgramHash(work.stages.back().program.codeAddress);
        for (std::size_t index = 0; index < work.uses.size(); ++index) {
            const auto& use = work.uses[index];
            static std::atomic<std::uint32_t> lines{0};
            if (!use.write || !sourceTarget(use.address, use.size) || lines.fetch_add(1, std::memory_order_relaxed) >= 3000u) continue;
            const char* kind = index >= work.imageBegin ? "image" : index >= work.targetBegin && index < work.targetEnd ? "target" : "memory";
            std::fprintf(stderr, "[recode-source-write] %lld s: program %016llx writes %s 0x%llx+0x%llx\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - processStart()).count()), static_cast<unsigned long long>(program), kind, static_cast<unsigned long long>(use.address), static_cast<unsigned long long>(use.size));
        }
    }

    void dumpImageReads(const Context& context, const Resolve::ResolvedDraw& work) {
        static const bool on = std::getenv("APS5_RECODE_DUMP_SAMPLED_BY") != nullptr;
        auto* table = Resolve::Surfaces();
        if (!on || table == nullptr || context.recorder == nullptr || work.stages.empty()) return;
        const auto program = Downgrades::ProgramHash(work.stages.back().program.codeAddress);
        std::size_t reads = 0;
        std::size_t matched = 0;
        for (std::size_t index = work.imageBegin; index < work.uses.size(); ++index) {
            const auto& use = work.uses[index];
            if (use.write) continue;
            ++reads;
            for (const auto& record : table->Overlapping(use.address, use.size)) {
                if (record->key.address != use.address) continue;
                ++matched;
                maybeDumpSampled(context, *context.recorder, record, program);
            }
        }
        if (seen.insert("image reads " + std::to_string(program)).second && program == std::strtoull(std::getenv("APS5_RECODE_DUMP_SAMPLED_BY"), nullptr, 16)) std::fprintf(stderr, "[recode-dump] dispatch %016llx: %zu image reads, %zu with a record at their address\n", static_cast<unsigned long long>(program), reads, matched);
    }

    static void consumeForUses(const Resolve::ResolvedDraw& operation) {
        static const char* const mode = std::getenv("APS5_RECODE_CONSUME_USES");
        static const bool global = mode != nullptr && std::string_view(mode) == "global";
        static const bool quick = mode != nullptr && std::string_view(mode) == "quick";
        static const bool every = mode != nullptr && std::string_view(mode) == "every";
        if (mode == nullptr || std::string_view(mode) == "0") return;
        if (every) {
            if (RouteMemory::ConsumeIn({}) != 0u) inputConsumes().fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (quick) {
            for (const auto& use : operation.uses) {
                if (use.size == 0u || use.write || MemoryCore::Get().Pages().CleanIn(use.address, use.size)) continue;
                static_cast<void>(RouteMemory::ConsumeIn({}));
                inputConsumes().fetch_add(1, std::memory_order_relaxed);
                return;
            }
            return;
        }
        auto& pages = MemoryCore::Get().Pages();
        thread_local std::vector<std::pair<std::uint64_t, std::uint64_t>> dirty;
        dirty.clear();
        for (const auto& use : operation.uses) {
            if (use.size == 0u || use.write || pages.CleanIn(use.address, use.size)) continue;
            if (global) {
                RouteMemory::ConsumeNow();
                inputConsumes().fetch_add(1, std::memory_order_relaxed);
                return;
            }
            dirty.emplace_back(use.address, use.size);
        }
        if (dirty.empty()) return;
        static_cast<void>(RouteMemory::ConsumeIn(dirty));
        inputConsumes().fetch_add(1, std::memory_order_relaxed);
    }

    bool recordDispatch(const Context& context, const Resolve::ResolvedDraw& dispatch, const std::array<std::uint32_t, 3>& groups, std::string& reason) {
        dumpImageReads(context, dispatch);
        writtenRecords.clear();
        stageStorageImages.clear();
        consumeForUses(dispatch);
        writeBackForReaders(context, dispatch);
        if (hangMarking() && context.recorder != nullptr) markHangPhase(context, *context.recorder, 5u);
        handOverForWriters(context, dispatch);
        if (hangMarking() && context.recorder != nullptr) markHangPhase(context, *context.recorder, 6u);
        if (dispatch.stages.size() != 1u || (dispatch.stages.front().deferredImages.empty() && !tableImagesDeferred(dispatch.stages.front()))) return recordBound(context, dispatch, groups, {}, reason);
        auto bound = dispatch;
        std::vector<std::shared_ptr<Graphics::StorageTexture>> written;
        if (!bindTableImages(context, bound.stages.front(), reason)) return false;
        if (!bound.stages.front().deferredImages.empty() && !bindDeferredImages(context, bound.stages.front(), written, reason)) return false;
        return recordBound(context, bound, groups, written, reason);
    }

    bool bindDeferredImages(const Context& context, Resolve::ResolvedStage& stage, std::vector<std::shared_ptr<Graphics::StorageTexture>>& written, std::string& reason) {
        written.reserve(written.size() + stage.deferredImages.size());
        std::vector<const Heap::DeferredImage*> sampled;
        for (const auto& deferred : stage.deferredImages) {
            std::uint32_t slot = 0;
            if ((deferred.request.flags & Heap::RequestFlag::Storage) == 0u) {
                sampled.push_back(&deferred);
                continue;
            }
            auto record = ownsImages() ? ownedStorage(context, deferred.request, slot, stage.program.codeAddress) : Resolve::BindDeferredRecord(deferred.request, slot);
            if (record == nullptr && Downgrades::Active() && Resolve::StorageRecordShape(deferred.request)) Downgrades::Note(stage.program.codeAddress, Downgrades::Kind::StorageRecordMissing, deferred.index, "bound from the existing cache");
            const bool storageWritten = (deferred.request.flags & Heap::RequestFlag::Written) != 0u;
            if (record != nullptr && storageWritten && sourceTarget(record->key.address, record->key.bytes)) std::fprintf(stderr, "[recode-target] program %016llx storage -> record 0x%llx+0x%llx %ux%u mips %u dimension %u\n", static_cast<unsigned long long>(Downgrades::ProgramHash(stage.program.codeAddress)), static_cast<unsigned long long>(record->key.address), static_cast<unsigned long long>(record->key.bytes), record->key.width, record->key.height, record->key.mips, record->key.dimension);
            if (record != nullptr) {
                stage.slots[deferred.index] = slot;
                if (context.recorder != nullptr) context.recorder->Keep(record);
                if (context.recorder != nullptr && nanCensus().draws != 0u && censusReadsOf(Downgrades::ProgramHash(stage.program.codeAddress))) censusRecord(context, *context.recorder, record, Downgrades::ProgramHash(stage.program.codeAddress), true);
                if (const auto* vulkan = dynamic_cast<const Images::VulkanSurface*>(record->device.get())) stageStorageImages.emplace_back(vulkan->Image(), storageWritten);
                if (storageWritten) writtenRecords.push_back(std::move(record));
                continue;
            }
            noteImage(stage, Downgrades::Kind::UpstreamStorage, deferred.index, deferred.request, storageWritten ? "written storage image without a record" : "storage image without a record");
            reason = "storage image without a record";
            return false;
        }
        const Downgrades::ProgramScope programScope(stage.program.codeAddress);
        for (const auto* deferred : sampled) {
            lastRefusedImage().clear();
            if (ownsImages() && bindOwnedSampled(context, stage, *deferred)) continue;
            if (Downgrades::Active()) noteStaleRecordRead(stage, *deferred);
            const auto why = "sampled image without a record (" + (lastRefusedImage().empty() ? std::string(Resolve::Surfaces() == nullptr ? "no surface table" : "not held") : lastRefusedImage()) + ")";
            noteImage(stage, Downgrades::Kind::UpstreamSampled, deferred->index, deferred->request, why.c_str());
            reason = "sampled image without a record";
            return false;
        }
        return Resolve::PatchSlots(*stage.program.result, stage.slots, stage.program.words, reason);
    }

    static bool tableImagesDeferred(const Resolve::ResolvedStage& stage) {
        return stage.tableMap != nullptr && !stage.tableMap->deferred.empty();
    }

    bool bindTableImages(const Context& context, Resolve::ResolvedStage& stage, std::string& reason) {
        const auto base = stage.tableMap;
        if (base == nullptr || base->deferred.empty()) return true;
        const auto sharedStamp = Events::Shared().Serial();
        const auto imagesStamp = Events::Images().Serial();
        const auto depthGeneration = Images::DepthRecordsOn() ? Images::Depths().Generation() : std::uint64_t{0};
        if (tableImages.size() >= TableImagesBound) std::erase_if(tableImages, [](const auto& held) { return held.second.base.expired(); });
        if (const auto* surfaces = Resolve::Surfaces(); surfaces != nullptr && surfaces->Retirements() != tableRetirements) {
            tableRetirements = surfaces->Retirements();
            for (auto& [lineage, held] : tableImages) {
                bool released = false;
                for (std::size_t position = 0; position < held.records.size(); ++position) {
                    if (held.records[position] == nullptr || !held.records[position]->retired) continue;
                    held.records[position].reset();
                    if (position < held.owners.size()) held.owners[position].reset();
                    released = true;
                }
                if (!released) continue;
                held.bound.reset();
                totals.tableImageEntriesDropped++;
            }
        }
        auto& entry = tableImages[base->lineage];
        const bool fresh = (entry.bound == nullptr && entry.elements.empty()) || entry.depths != depthGeneration;
        const bool rebased = !fresh && entry.base.lock() != base;
        std::vector<bool> stale(fresh ? 0u : entry.elements.size(), false);
        bool anyStale = false;
        if (!fresh && entry.bound == nullptr) {
            for (std::size_t position = 0; position < stale.size() && position < entry.records.size() && position < entry.owners.size(); ++position) {
                if (entry.records[position] != nullptr || entry.owners[position] != nullptr) continue;
                stale[position] = true;
            }
            anyStale = true;
        }
        std::vector<std::uint32_t> touched;
        if (!fresh) entry.index.Changed(Events::Shared(), entry.sharedStamp, Events::Images(), entry.imagesStamp, touched);
        if (!touched.empty()) {
            auto* table = Resolve::Surfaces();
            bool refreshed = false;
            for (const auto position : touched) {
                const auto& record = entry.records[position];
                if (record == nullptr && entry.owners[position] != nullptr) {
                    totals.tableImageDepthKept++;
                    continue;
                }
                if (record != nullptr && !record->retired && table != nullptr) {
                    if (refreshesRecords() && table->RefreshIfChanged(record)) {
                        totals.recordsRefreshed++;
                        refreshed = true;
                    }
                    totals.tableImageRefreshes++;
                    continue;
                }
                if (record != nullptr) totals.tableImageRetired++;
                stale[position] = true;
                anyStale = true;
            }
            if (refreshed && context.recorder != nullptr) drainSurfaces(context, *context.recorder);
        }
        entry.sharedStamp = sharedStamp;
        entry.imagesStamp = imagesStamp;
        if (!fresh && !rebased && !anyStale) {
            totals.tableImageHits++;
            stage.tableMap = entry.bound;
            noteTableStorage(context, entry);
            return true;
        }
        std::vector<std::size_t> rebind;
        if (fresh || rebased) {
            std::unordered_map<std::uint32_t, std::size_t> previous;
            if (rebased) {
                previous.reserve(entry.elements.size());
                for (std::size_t position = 0; position < entry.elements.size(); ++position) previous.emplace(entry.elements[position].index, position);
            }
            const auto count = base->deferred.size();
            std::vector<std::uint32_t> slots(count, 0u);
            std::vector<Heap::Span> memory(count);
            std::vector<std::shared_ptr<const void>> owners(count);
            std::vector<std::shared_ptr<Images::Surface>> records(count);
            for (std::size_t position = 0; position < count; ++position) {
                const auto& element = base->deferred[position];
                const auto found = rebased ? previous.find(element.index) : previous.end();
                if (found != previous.end() && !stale[found->second] && sameRequest(entry.elements[found->second].request, element.request)) {
                    slots[position] = entry.slots[found->second];
                    memory[position] = entry.memory[found->second];
                    owners[position] = std::move(entry.owners[found->second]);
                    records[position] = std::move(entry.records[found->second]);
                    continue;
                }
                rebind.push_back(position);
            }
            entry.elements = base->deferred;
            entry.slots = std::move(slots);
            entry.memory = std::move(memory);
            entry.owners = std::move(owners);
            entry.records = std::move(records);
            entry.base = base;
            if (fresh) totals.tableImageWhole++;
        } else {
            for (std::size_t position = 0; position < stale.size(); ++position) {
                if (stale[position]) rebind.push_back(position);
            }
        }
        for (const auto position : rebind) {
            const auto& deferred = entry.elements[position];
            lastRefusedImage().clear();
            if ((deferred.request.flags & Heap::RequestFlag::Storage) != 0u) {
                auto record = ownedStorage(context, deferred.request, entry.slots[position], stage.program.codeAddress);
                if (record == nullptr) {
                    entry.bound.reset();
                    const auto why = "table storage image without a record (" + (lastRefusedImage().empty() ? std::string("not held") : lastRefusedImage()) + ")";
                    noteImage(stage, Downgrades::Kind::UpstreamStorage, deferred.index, deferred.request, why.c_str());
                    reason = why;
                    return false;
                }
                entry.memory[position] = {record->key.address, record->key.bytes};
                entry.owners[position] = record;
                entry.records[position] = std::move(record);
                continue;
            }
            if (!ownedSampledSlot(context, stage.program, deferred, entry.slots[position], &entry.owners[position], &entry.records[position])) {
                entry.bound.reset();
                const auto why = "table image without a record (" + (lastRefusedImage().empty() ? std::string(Resolve::Surfaces() == nullptr ? "no surface table" : "not held") : lastRefusedImage()) + ")";
                noteImage(stage, Downgrades::Kind::UpstreamSampled, deferred.index, deferred.request, why.c_str());
                reason = why;
                return false;
            }
            try {
                const auto resource = Graphics::DecodeTextureResource(deferred.request.words);
                entry.memory[position] = {resource.baseAddress, Graphics::SurfaceGuestBytes(resource)};
            } catch (const std::exception&) {
                entry.memory[position] = {};
            }
        }
        auto bound = std::make_shared<Tables::Mirror>();
        bound->id = nextTableImagesId++;
        bound->lineage = base->lineage;
        bound->map = base->map;
        bound->slotBase = base->slotBase;
        for (std::size_t position = 0; position < entry.elements.size(); ++position) bound->map[base->slotBase + entry.elements[position].index] = entry.slots[position];
        bound->owners = base->owners;
        for (const auto& owner : entry.owners) {
            if (owner != nullptr) bound->owners.push_back(owner);
        }
        if (fresh || rebased) entry.index.Build(entry.memory);
        entry.depths = Images::DepthRecordsOn() ? Images::Depths().Generation() : std::uint64_t{0};
        entry.bound = bound;
        stage.tableMap = std::move(bound);
        totals.tableImageBuilds++;
        totals.tableImageBinds += rebind.size();
        noteTableStorage(context, entry);
        return true;
    }

    void noteTableStorage(const Context& context, const TableImages& entry) {
        for (std::size_t position = 0; position < entry.elements.size() && position < entry.records.size(); ++position) {
            const auto& request = entry.elements[position].request;
            const auto& record = entry.records[position];
            if ((request.flags & Heap::RequestFlag::Storage) == 0u || record == nullptr) continue;
            const bool written = (request.flags & Heap::RequestFlag::Written) != 0u;
            if (context.recorder != nullptr) context.recorder->Keep(record);
            if (const auto* vulkan = dynamic_cast<const Images::VulkanSurface*>(record->device.get())) stageStorageImages.emplace_back(vulkan->Image(), written);
            if (written) writtenRecords.push_back(record);
        }
    }

    static bool sameRequest(const Heap::SlotRequest& left, const Heap::SlotRequest& right) {
        return left.kind == right.kind && left.flags == right.flags && left.words == right.words;
    }

    void noteStorageWrites(const Context& context, Graphics::Recorder& recorder, std::span<const std::shared_ptr<Graphics::StorageTexture>> written) {
        for (const auto& texture : written) {
            recorder.Keep(texture);
            texture->MarkDirty();
            static_cast<void>(Graphics::DepthSurfaceStoreBack(context, *texture));
            totals.storageWrites++;
        }
        if (!written.empty() || !writtenRecords.empty()) GuestMemory::BreakProofs();
        auto* surfaceTable = Resolve::Surfaces();
        for (const auto& record : writtenRecords) {
            const auto owner = Images::OwnerOf(*record);
            static_cast<void>(Events::GpuWrites().Note(record->key.address, record->key.bytes, owner));
            static_cast<void>(Events::Shared().Note(record->key.address, record->key.bytes, owner));
            if (surfaceTable != nullptr) markRecordWritten(recorder, *surfaceTable, *record);
            totals.recordWrites++;
        }
    }

    void censusRecord(const Context& context, Graphics::Recorder& recorder, const std::shared_ptr<Images::Surface>& record, std::uint64_t program, bool read = false) {
        const auto& census = nanCensus();
        const auto now = std::chrono::steady_clock::now();
        if (!censusSizes().empty() && std::ranges::find(censusSizes(), std::pair<std::uint32_t, std::uint32_t>{record->key.width, record->key.height}) == censusSizes().end()) return;
        if (now - censusClock < std::chrono::seconds(census.seconds) || censusDraws >= census.draws) return;
        auto* vulkan = dynamic_cast<Images::VulkanSurface*>(record->device.get());
        if (vulkan == nullptr || vulkan->Compressed()) return;
        const auto format = vulkan->Format();
        if (format != VK_FORMAT_R16G16B16A16_SFLOAT && format != VK_FORMAT_R32G32B32A32_SFLOAT && format != VK_FORMAT_R32G32_SFLOAT && format != VK_FORMAT_R32_SFLOAT && format != VK_FORMAT_R16G16_SFLOAT && format != VK_FORMAT_R16_SFLOAT && format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && !read && censusDump().count == 0u) return;
        const auto sequence = censusDraws++;
        const auto width = record->key.width;
        const auto height = record->key.height;
        const auto elementBytes = Graphics::BytesPerElement(vulkan->Resource().format);
        const std::uint32_t layers = read ? std::max(vulkan->Geometry().imageLayers, 1u) : 1u;
        const std::size_t layerBytes = static_cast<std::size_t>(width) * height * elementBytes;
        auto buffer = std::make_shared<Graphics::Buffer>(context, layerBytes * layers, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto commands = recorder.Commands();
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, layers};
        region.imageExtent = {width, height, 1u};
        context.Resolved(&Graphics::DeviceFunctions::cmdCopyImageToBuffer, "vkCmdCopyImageToBuffer")(commands, vulkan->Image(), VK_IMAGE_LAYOUT_GENERAL, buffer->Handle(), 1, &region);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        recorder.Keep(buffer);
        const auto address = record->key.address;
        const auto mips = record->key.mips;
        std::uint64_t guestNonzero = 0;
        if (read) {
            if (const auto* view = static_cast<const std::byte*>(GuestMemory::OwnStoreTarget(address, static_cast<std::size_t>(record->key.bytes))); view != nullptr && view != reinterpret_cast<const std::byte*>(address)) {
                for (std::uint64_t at = 0; at < record->key.bytes; at += 64u) guestNonzero += view[at] != std::byte{0} ? 1u : 0u;
            }
        }
        recorder.OnComplete([buffer, sequence, program, address, width, height, mips, format, read, layers, layerBytes, guestNonzero] {
            buffer->Invalidate();
            const auto bytes = buffer->Bytes();
            if (const auto& dump = censusDump(); sequence < dump.count) {
                char name[160];
                std::snprintf(name, sizeof(name), "\\census%04u-%s-p%016llx-record-%llx-L0-%ux%u-vk%u.raw", sequence, read ? "read" : "write", static_cast<unsigned long long>(program), static_cast<unsigned long long>(address), width, height, static_cast<unsigned>(format));
                const auto file = dump.directory + name;
                if (auto* out = std::fopen(file.c_str(), "wb")) {
                    std::fwrite(bytes.data(), 1, std::min(layerBytes, bytes.size()), out);
                    std::fclose(out);
                } else {
                    std::fprintf(stderr, "[recode-dump] cannot write %s\n", file.c_str());
                }
            }
            std::uint32_t filledLayers = 0;
            for (std::uint32_t layer = 0; layer < layers; ++layer) {
                const auto chunk = bytes.subspan(layer * layerBytes, std::min(layerBytes, bytes.size() - std::min(bytes.size(), layer * layerBytes)));
                if (std::ranges::any_of(chunk, [](std::byte value) { return value != std::byte{0}; })) ++filledLayers;
            }
            std::uint64_t bad = 0;
            std::uint64_t huge = 0;
            double sum = 0.0;
            std::uint64_t counted = 0;
            const bool floating = format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R32G32B32A32_SFLOAT || format == VK_FORMAT_R32G32_SFLOAT || format == VK_FORMAT_R32_SFLOAT || format == VK_FORMAT_R16G16_SFLOAT || format == VK_FORMAT_R16_SFLOAT || format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            if (!floating) {
                for (const auto value : bytes) sum += static_cast<double>(std::to_integer<std::uint8_t>(value));
                counted = bytes.size();
            } else if (format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
                for (std::size_t at = 0; at + 4u <= bytes.size(); at += 4u) {
                    std::uint32_t word = 0;
                    std::memcpy(&word, bytes.data() + at, 4u);
                    if (((word >> 6u) & 0x1fu) == 0x1fu || ((word >> 17u) & 0x1fu) == 0x1fu || ((word >> 27u) & 0x1fu) == 0x1fu) {
                        ++bad;
                        continue;
                    }
                    if (((word >> 6u) & 0x1fu) == 0x1eu || ((word >> 17u) & 0x1fu) == 0x1eu || ((word >> 27u) & 0x1fu) == 0x1eu) ++huge;
                    const auto unpacked = [](std::uint32_t exponent, std::uint32_t mantissa, double steps) { return exponent == 0u ? mantissa / steps * std::ldexp(1.0, -14) : (1.0 + mantissa / steps) * std::ldexp(1.0, static_cast<int>(exponent) - 15); };
                    sum += unpacked((word >> 6u) & 0x1fu, word & 0x3fu, 64.0) + unpacked((word >> 17u) & 0x1fu, (word >> 11u) & 0x3fu, 64.0) + unpacked((word >> 27u) & 0x1fu, (word >> 22u) & 0x1fu, 32.0);
                    counted += 3u;
                }
            } else if (format == VK_FORMAT_R32G32B32A32_SFLOAT || format == VK_FORMAT_R32G32_SFLOAT || format == VK_FORMAT_R32_SFLOAT) {
                for (std::size_t at = 0; at + 4u <= bytes.size(); at += 4u) {
                    std::uint32_t word = 0;
                    std::memcpy(&word, bytes.data() + at, 4u);
                    if ((word & 0x7f800000u) == 0x7f800000u) ++bad;
                    else {
                        if (((word >> 23u) & 0xffu) >= 142u) ++huge;
                        float value = 0.0f;
                        std::memcpy(&value, &word, 4u);
                        sum += std::fabs(static_cast<double>(value));
                        ++counted;
                    }
                }
            } else {
                for (std::size_t at = 0; at + 2u <= bytes.size(); at += 2u) {
                    std::uint16_t half = 0;
                    std::memcpy(&half, bytes.data() + at, 2u);
                    if ((half & 0x7c00u) == 0x7c00u) ++bad;
                    else {
                        if ((half & 0x7c00u) == 0x7800u) ++huge;
                        const auto exponent = static_cast<int>((half >> 10u) & 0x1fu);
                        const auto mantissa = static_cast<double>(half & 0x3ffu);
                        sum += exponent == 0 ? mantissa * std::ldexp(1.0, -24) : (1.0 + mantissa / 1024.0) * std::ldexp(1.0, exponent - 15);
                        ++counted;
                    }
                }
            }
            const double mean = counted == 0u ? 0.0 : sum / static_cast<double>(counted);
            if (read) std::fprintf(stderr, "[recode-nan-read] %u program %016llx reads record 0x%llx %ux%u mips %u vk%u: %llu non-finite values, %llu at or above 32768, mean %.4f", sequence, static_cast<unsigned long long>(program), static_cast<unsigned long long>(address), width, height, mips, static_cast<unsigned>(format), static_cast<unsigned long long>(bad), static_cast<unsigned long long>(huge), mean);
            if (read) std::fprintf(stderr, ", layers with content %u of %u, guest memory samples non-zero %llu", filledLayers, layers, static_cast<unsigned long long>(guestNonzero));
            if (read) std::fputc(10, stderr);
            else if (!censusSizes().empty()) {
                std::fprintf(stderr, "[recode-nan] %u program %016llx record 0x%llx %ux%u mips %u vk%u: %llu non-finite values, %llu at or above 32768, mean %.4f", sequence, static_cast<unsigned long long>(program), static_cast<unsigned long long>(address), width, height, mips, static_cast<unsigned>(format), static_cast<unsigned long long>(bad), static_cast<unsigned long long>(huge), mean);
                std::fprintf(stderr, ", content %s", filledLayers != 0u ? "yes" : "none");
                std::fputc(10, stderr);
            } else if (bad != 0u || huge != 0u) std::fprintf(stderr, read ? "[recode-nan-read] %u program %016llx reads record 0x%llx %ux%u mips %u vk%u: %llu non-finite values, %llu at or above 32768\n" : "[recode-nan] %u program %016llx record 0x%llx %ux%u mips %u vk%u: %llu non-finite values, %llu at or above 32768\n", sequence, static_cast<unsigned long long>(program), static_cast<unsigned long long>(address), width, height, mips, static_cast<unsigned>(format), static_cast<unsigned long long>(bad), static_cast<unsigned long long>(huge));
        });
    }

    void zeroRecord(const Context& context, Graphics::Recorder& recorder, Images::SurfaceTable& table, Images::Surface& record) {
        auto* vulkan = dynamic_cast<Images::VulkanSurface*>(record.device.get());
        if (vulkan == nullptr) return;
        clearRecord(context, recorder, *vulkan, VkClearColorValue{});
        table.MarkReady(record, Events::Shared().Serial(), Events::GpuWrites().Serial());
        totals.zeroedChains++;
    }

    void poisonRecord(const Context& context, Images::Surface* record) {
        const auto value = poisonValue();
        if (!value.has_value() || record == nullptr || context.recorder == nullptr) return;
        auto* vulkan = dynamic_cast<Images::VulkanSurface*>(record->device.get());
        if (vulkan == nullptr || vulkan->Initialized() || vulkan->Compressed()) return;
        clearRecord(context, *context.recorder, *vulkan, *value);
        totals.poisonedRecords++;
    }

    static void clearRecord(const Context& context, Graphics::Recorder& recorder, Images::VulkanSurface& surface, const VkClearColorValue& value) {
        auto* vulkan = &surface;
        const auto commands = recorder.Commands();
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = 0u;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = vulkan->Image();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, VK_REMAINING_MIP_LEVELS, 0u, VK_REMAINING_ARRAY_LAYERS};
        context.Resolved(&Graphics::DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, vulkan->Image(), VK_IMAGE_LAYOUT_GENERAL, &value, 1, &barrier.subresourceRange);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        context.Resolved(&Graphics::DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vulkan->SetInitialized();
    }

    void maybeDumpRecord(const Context& context, Graphics::Recorder& recorder, const std::shared_ptr<Images::Surface>& record, std::uint64_t program, std::uint32_t writtenLevel = 0) {
        const auto& request = recordDumpRequest();
        static const bool single = std::getenv("APS5_RECODE_DUMP_SINGLE") != nullptr;
        if (record->key.width < request.width || (record->key.mips <= 1u && !single) || (request.trace && record->key.width != request.width)) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - dumpClock < std::chrono::seconds(request.seconds)) return;
        std::string prefix;
        if (request.trace) {
            if (!request.program.empty()) {
                char hash[20];
                std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(program));
                if (std::string_view(hash).rfind(request.program, 0) != 0) return;
            }
            if (traceAddress == 0u) traceAddress = record->key.address;
            if (record->key.address != traceAddress || traceDumps >= 40u) return;
            if (request.every > 1u && traceSeen++ % request.every != 0u) return;
            char tag[48];
            std::snprintf(tag, sizeof(tag), "trace%02u-p%016llx-", traceDumps++, static_cast<unsigned long long>(program));
            prefix = tag;
        } else if (dumpedRecords.size() >= 8u || dumpedRecords.contains(record->key.address)) {
            return;
        }
        auto* vulkan = dynamic_cast<Images::VulkanSurface*>(record->device.get());
        if (vulkan == nullptr || vulkan->Compressed()) {
            if (seen.insert("dump skipped").second) std::fprintf(stderr, "[recode-dump] record 0x%llx skipped: %s\n", static_cast<unsigned long long>(record->key.address), vulkan == nullptr ? "not a Vulkan surface" : "block-compressed format");
            return;
        }
        dumpedRecords.insert(record->key.address);
        const auto elementBytes = Graphics::BytesPerElement(vulkan->Resource().format);
        const auto levels = static_cast<std::uint32_t>(std::max<std::size_t>(vulkan->Geometry().mips.size(), 1u));
        const auto commands = recorder.Commands();
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        for (std::uint32_t level = 0; level < levels; ++level) {
            if (request.trace && level != writtenLevel) continue;
            const auto width = std::max(record->key.width >> level, 1u);
            const auto height = std::max(record->key.height >> level, 1u);
            auto buffer = std::make_shared<Graphics::Buffer>(context, static_cast<std::size_t>(width) * height * elementBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0u, 1u};
            region.imageExtent = {width, height, 1u};
            context.Resolved(&Graphics::DeviceFunctions::cmdCopyImageToBuffer, "vkCmdCopyImageToBuffer")(commands, vulkan->Image(), VK_IMAGE_LAYOUT_GENERAL, buffer->Handle(), 1, &region);
            recorder.Keep(buffer);
            char name[192];
            std::snprintf(name, sizeof(name), "%srecord-%llx-L%u-%ux%u-vk%u.raw", prefix.c_str(), static_cast<unsigned long long>(record->key.address), level, width, height, static_cast<unsigned>(vulkan->Format()));
            recorder.OnComplete([buffer, file = request.directory + "\\" + name] {
                auto* out = std::fopen(file.c_str(), "wb");
                if (out == nullptr) {
                    std::fprintf(stderr, "[recode-dump] cannot write %s\n", file.c_str());
                    return;
                }
                buffer->Invalidate();
                const auto bytes = buffer->Bytes();
                std::fwrite(bytes.data(), 1, bytes.size(), out);
                std::fclose(out);
                std::fprintf(stderr, "[recode-dump] wrote %s (%zu bytes)\n", file.c_str(), bytes.size());
            });
            if (levels == 1u) break;
        }
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        std::fprintf(stderr, "[recode-dump] record 0x%llx %ux%u %u levels format %u written at %lld s\n", static_cast<unsigned long long>(record->key.address), record->key.width, record->key.height, levels, record->key.format, static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(now - dumpClock).count()));
    }

    void maybeDumpDepth(const Context& context, Graphics::Recorder& recorder, const std::shared_ptr<Images::DepthRecord>& record, std::uint64_t program) {
        static const std::uint64_t wanted = [] {
            const char* value = std::getenv("APS5_RECODE_DUMP_DEPTH_BEFORE");
            return value == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(std::strtoull(value, nullptr, 16));
        }();
        const auto& request = recordDumpRequest();
        if (wanted == 0 || program != wanted || request.directory.empty() || !record->Initialized()) return;
        if (std::chrono::steady_clock::now() - dumpClock < std::chrono::seconds(request.seconds)) return;
        if (depthDumps >= 6u || depthSeen++ % std::max<std::uint32_t>(request.every, 1u) != 0u) return;
        const auto& key = record->Key();
        const auto commands = recorder.Commands();
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        auto buffer = std::make_shared<Graphics::Buffer>(context, static_cast<std::size_t>(key.width) * key.height * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 0u, 1u};
        region.imageExtent = {key.width, key.height, 1u};
        context.Resolved(&Graphics::DeviceFunctions::cmdCopyImageToBuffer, "vkCmdCopyImageToBuffer")(commands, record->Image(), VK_IMAGE_LAYOUT_GENERAL, buffer->Handle(), 1, &region);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        recorder.Keep(buffer);
        char name[192];
        std::snprintf(name, sizeof(name), "depth%02u-p%016llx-record-%llx-%ux%u.f32", depthDumps++, static_cast<unsigned long long>(program), static_cast<unsigned long long>(key.address), key.width, key.height);
        recorder.OnComplete([buffer, file = request.directory + "/" + name] {
            auto* out = std::fopen(file.c_str(), "wb");
            if (out == nullptr) return;
            buffer->Invalidate();
            const auto bytes = buffer->Bytes();
            std::fwrite(bytes.data(), 1, bytes.size(), out);
            std::fclose(out);
        });
    }

    void maybeDumpSampled(const Context& context, Graphics::Recorder& recorder, const std::shared_ptr<Images::Surface>& record, std::uint64_t program) {
        static const std::uint64_t wanted = [] {
            const char* value = std::getenv("APS5_RECODE_DUMP_SAMPLED_BY");
            return value == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(std::strtoull(value, nullptr, 16));
        }();
        const auto& request = recordDumpRequest();
        if (wanted == 0 || program != wanted || request.directory.empty()) return;
        if (std::chrono::steady_clock::now() - dumpClock < std::chrono::seconds(request.seconds)) return;
        if (sampledDumps.contains(record->key.address) || sampledDumps.size() >= 16u) return;
        sampledDumps.insert(record->key.address);
        auto* vulkan = dynamic_cast<Images::VulkanSurface*>(record->device.get());
        if (vulkan == nullptr) {
            std::fprintf(stderr, "[recode-dump] sampled record 0x%llx skipped: not a Vulkan surface\n", static_cast<unsigned long long>(record->key.address));
            return;
        }
        const auto elementBytes = Graphics::BytesPerElement(vulkan->Resource().format);
        const auto block = std::max(Graphics::BlockWidth(vulkan->Resource().format), 1u);
        const auto width = record->key.width;
        const auto height = record->key.height;
        if (Downgrades::Active()) std::fprintf(stderr, "[recode-dump] sampled record 0x%llx %ux%u guest format %u vk %u, %u bytes per element, block %u, by %016llx\n", static_cast<unsigned long long>(record->key.address), width, height, static_cast<unsigned>(vulkan->Resource().format), static_cast<unsigned>(vulkan->Format()), elementBytes, block, static_cast<unsigned long long>(program));
        const auto commands = recorder.Commands();
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        auto buffer = std::make_shared<Graphics::Buffer>(context, static_cast<std::size_t>((width + block - 1u) / block) * ((height + block - 1u) / block) * elementBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
        region.imageExtent = {width, height, 1u};
        context.Resolved(&Graphics::DeviceFunctions::cmdCopyImageToBuffer, "vkCmdCopyImageToBuffer")(commands, vulkan->Image(), VK_IMAGE_LAYOUT_GENERAL, buffer->Handle(), 1, &region);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        recorder.Keep(buffer);
        char name[192];
        std::snprintf(name, sizeof(name), "sampled-p%016llx-record-%llx-L0-%ux%u-vk%u.raw", static_cast<unsigned long long>(program), static_cast<unsigned long long>(record->key.address), width, height, static_cast<unsigned>(vulkan->Format()));
        recorder.OnComplete([buffer, file = request.directory + "/" + name] {
            auto* out = std::fopen(file.c_str(), "wb");
            if (out == nullptr) return;
            buffer->Invalidate();
            const auto bytes = buffer->Bytes();
            std::fwrite(bytes.data(), 1, bytes.size(), out);
            std::fclose(out);
        });
        std::fprintf(stderr, "[recode-dump] sampled record 0x%llx %ux%u format %u by %016llx\n", static_cast<unsigned long long>(record->key.address), width, height, record->key.format, static_cast<unsigned long long>(program));
    }

    void maybeDumpUpstream(const Context& context, Graphics::Recorder& recorder, const std::shared_ptr<Graphics::StorageTexture>& resident, std::uint64_t program, std::uint32_t level) {
        const auto& request = recordDumpRequest();
        const auto& descriptor = resident->Descriptor();
        if (!request.trace || descriptor.width != request.width || descriptor.mipCount <= 1u || level >= descriptor.mipCount) return;
        if (std::chrono::steady_clock::now() - dumpClock < std::chrono::seconds(request.seconds)) return;
        if (traceAddress == 0u) traceAddress = descriptor.baseAddress;
        if (descriptor.baseAddress != traceAddress || traceDumps >= 40u) return;
        if (!request.program.empty()) {
            char hash[20];
            std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(program));
            if (std::string_view(hash).rfind(request.program, 0) != 0) return;
        }
        if (request.every > 1u && traceSeen++ % request.every != 0u) return;
        const auto elementBytes = Graphics::BytesPerElement(descriptor.format);
        const auto width = std::max(descriptor.width >> level, 1u);
        const auto height = std::max(descriptor.height >> level, 1u);
        const auto commands = recorder.Commands();
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        auto buffer = std::make_shared<Graphics::Buffer>(context, static_cast<std::size_t>(width) * height * elementBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0u, 1u};
        region.imageExtent = {width, height, 1u};
        context.Resolved(&Graphics::DeviceFunctions::cmdCopyImageToBuffer, "vkCmdCopyImageToBuffer")(commands, resident->Image(), VK_IMAGE_LAYOUT_GENERAL, buffer->Handle(), 1, &region);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        recorder.Keep(buffer);
        char name[192];
        std::snprintf(name, sizeof(name), "trace%02u-p%016llx-upstream-%llx-L%u-%ux%u-vk%u.raw", traceDumps++, static_cast<unsigned long long>(program), static_cast<unsigned long long>(descriptor.baseAddress), level, width, height, static_cast<unsigned>(resident->ImageFormat()));
        recorder.OnComplete([buffer, file = request.directory + "/" + name] {
            auto* out = std::fopen(file.c_str(), "wb");
            if (out == nullptr) return;
            buffer->Invalidate();
            const auto bytes = buffer->Bytes();
            std::fwrite(bytes.data(), 1, bytes.size(), out);
            std::fclose(out);
        });
    }

    void markRecordWritten(Graphics::Recorder& recorder, Images::SurfaceTable& table, Images::Surface& record) {
        const bool wasStale = record.mirrorStale;
        table.MarkGpuWritten(record);
        if (!wasStale && record.mirrorStale) Downgrades::WatchTouch("a record's first GPU write", record.key.address, record.key.bytes);
        if (wasStale || !record.mirrorStale || !Mirror::WritesEnabled() || !notesRecordRanges()) return;
        UploadClash::NoteTracked(record.key.address, record.key.bytes);
        const auto mark = MemoryCore::Get().NoteGpuWrite(record.key.address, record.key.bytes, recorder.OpenBatchSerial());
        if (!mark.upload.empty()) ResidencyService::Get().NoteDirty(mark.upload);
        totals.recordRangesNoted++;
    }


    bool checkDispatchArguments(Graphics::Recorder& recorder, VkCommandBuffer commands, std::uint64_t serial, std::uint64_t completed, VkDeviceAddress source, VkBuffer& buffer, VkDeviceSize& offset) {
        static const bool unchecked = std::getenv("APS5_RECODE_UNCHECKED_DISPATCH_ARGUMENTS") != nullptr;
        if (unchecked || ringAddress == 0u || source == 0u) return false;
        constexpr std::array<std::byte, 3u * sizeof(std::uint32_t)> cleared{};
        const auto placed = ringWrite(std::span<const std::byte>(cleared), serial, completed);
        if (!placed.has_value() || !recorder.RecordDispatchArguments(commands, source, ringAddress + *placed)) return false;
        buffer = ring->Handle();
        offset = *placed;
        return true;
    }

    bool placeDispatchArguments(const Context& context, Graphics::Recorder& recorder, std::uint64_t address, std::uint64_t serial, std::uint64_t completed, VkBuffer& buffer, VkDeviceSize& offset, VkDeviceAddress& device, std::string& reason) {
        constexpr std::size_t bytes = 3u * sizeof(std::uint32_t);
        const auto* source = reinterpret_cast<const std::byte*>(address);
        device = 0u;
        const auto mirrorDevice = [&] {
            const auto span = ResidencyService::Get().MirrorSpan(address, bytes);
            return span.has_value() && span->handle == reinterpret_cast<std::uintptr_t>(buffer) && span->offset == offset ? span->address : 0u;
        };
        if (watchesBatchPrograms()) {
            lastArgumentsAddress = address;
            lastArgumentsCpu = {};
            const auto* record = reinterpret_cast<const std::byte*>(address & ~std::uint64_t{31});
            if (GuestMemory::Accessible(record, 32u, false)) std::memcpy(lastArgumentsCpu.data(), record, 32u);
            lastArgumentsGpu = MemoryCore::Get().Pages().GpuWrittenIn(address, bytes) || gpuPending(address, bytes);
        }
        if (mirrorPart("args") && argumentsFromMirror()) {
            if (const auto input = mirrorInput(address, bytes)) {
                buffer = input->buffer;
                offset = input->offset;
                device = mirrorDevice();
                totals.mirrorArguments++;
                return true;
            }
        }
        if (!Graphics::Recorder::SnapshotWriteOverlaps(address, bytes) && recorder.PendingLeaseSerial(address, bytes) == 0u && !MemoryCore::Get().Pages().GpuWrittenIn(address, bytes) && !gpuPending(address, bytes) && MemoryCore::Get().Pages().GpuOwnedIn(address, bytes).empty() && GuestMemory::Accessible(source, bytes, false)) {
            const auto placed = ringWrite(std::span(source, bytes), serial, completed);
            if (!placed.has_value()) {
                reason = "ring full";
                return false;
            }
            buffer = ring->Handle();
            offset = *placed;
            device = ringAddress != 0u ? ringAddress + *placed : 0u;
            totals.snapshotArguments++;
            return true;
        }
        if (mirrorPart("args")) {
            if (const auto input = mirrorInput(address, bytes)) {
                buffer = input->buffer;
                offset = input->offset;
                device = mirrorDevice();
                totals.mirrorArguments++;
                return true;
            }
            if (gpuPending(address, bytes)) {
                reason = "indirect arguments with pending GPU bytes not in the mirror";
                return false;
            }
        }
        const auto* import = Graphics::HostImportFor(context, address, bytes);
        if (import == nullptr) {
            reason = "indirect arguments not imported";
            return false;
        }
        recorder.NotePendingRead(address, bytes, Graphics::Recorder::ReadKind::Indirect);
        buffer = import->buffer;
        offset = Graphics::HostImportOffset(*import, address);
        device = import->address != 0u ? import->address + (address - import->base) : 0u;
        return true;
    }

    bool recordBound(const Context& context, const Resolve::ResolvedDraw& dispatch, const std::array<std::uint32_t, 3>& groups, std::span<const std::shared_ptr<Graphics::StorageTexture>> written, std::string& reason) {
        auto* recorder = context.recorder;
        const auto* heap = Resolve::HeapDevice();
        if (recorder == nullptr || heap == nullptr) {
            reason = "no recorder or heap";
            return false;
        }
        if (!attach(context, reason) || !dispatchPrecheck(dispatch, reason)) return false;
        const bool indirect = dispatch.operation.dispatch.indirect;
        for (std::uint32_t axis = 0; axis < 3 && !indirect; ++axis) {
            if (groups[axis] > context.limits.maxComputeWorkGroupCount[axis]) {
                reason = "group count past the device limit";
                return false;
            }
        }
        drainSurfaces(context, *recorder);
        const auto& stage = dispatch.stages.front();
        const auto main = setLayout(context, dispatch, 0);
        const auto pipeline = builtPipeline(pipelineKey(dispatch));
        if (pipeline == nullptr) {
            reason = "compute pipeline not ready";
            return false;
        }

        recorder->OpenBatch();
        const auto serial = recorder->OpenBatchSerial();
        const auto completed = recorder->CompletedThrough();
        std::shared_ptr<Graphics::Buffer> pageTable;
        const auto set = allocateSet(context, main, serial, completed);
        copyBacks.clear();
        directWrites.clear();
        copyingBack = copiesBackDispatchWrites();
        std::array<std::byte, Graphics::ComputePushConstantBytes> block{};
        const auto& push = stage.program.words.pushConstants;
        const bool setWritten = writeSet(context, dispatch, 0, set, serial, completed, pageTable, reason);
        if (setWritten) {
            if (!push.empty()) std::memcpy(block.data(), push.data(), push.size());
            servePushKnownBuffers(context, stage, block, serial, completed);
        }
        copyingBack = false;
        if (!setWritten) {
            copyBacks.clear();
            return false;
        }
        VkBuffer argumentBuffer = VK_NULL_HANDLE;
        VkDeviceSize argumentOffset = 0;
        VkDeviceAddress argumentDevice = 0u;
        if (indirect && !placeDispatchArguments(context, *recorder, dispatch.operation.dispatch.indirectAddress, serial, completed, argumentBuffer, argumentOffset, argumentDevice, reason)) {
            copyBacks.clear();
            return false;
        }

        markHangPhase(context, *recorder, 1u);
        if (recorder->HasQueuedKeyStores()) recorder->FlushKeyStores();
        if (recorder->HasQueuedStores()) recorder->FlushStores();
        markHangPhase(context, *recorder, 2u);
        if (pageTable != nullptr && !dispatch.decodeReads.empty()) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> dirtyDecodeReads;
            auto& pageState = MemoryCore::Get().Pages();
            for (const auto& [address, size] : dispatch.decodeReads) {
                if (address == 0u || size == 0u) continue;
                if (servesFaultPages()) {
                    if (const auto pages = ResidencyService::Get().UnmappedPages(address, size); !pages.empty()) RouteMemory::ServeFaultPages(pages);
                }
                if (!pageState.CleanIn(address, size)) dirtyDecodeReads.emplace_back(address, size);
            }
            if (!dirtyDecodeReads.empty()) static_cast<void>(RouteMemory::ConsumeIn(dirtyDecodeReads));
        }
        if (RouteMemory::UploadsPending()) {
            std::vector<std::shared_ptr<void>> kept;
            if (RouteMemory::RecordUploads(recorder->Commands(), kept) != 0u) totals.residencyLands++;
            for (auto& object : kept) recorder->Keep(std::move(object));
        }
        const auto commands = recorder->Commands();
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | (indirect ? VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT : 0u), VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | (indirect ? VK_ACCESS_INDIRECT_COMMAND_READ_BIT : 0u));
        if (indirect) static_cast<void>(checkDispatchArguments(*recorder, commands, serial, completed, argumentDevice, argumentBuffer, argumentOffset));
        markHang(context, *recorder, commands, Downgrades::ProgramHash(stage.program.codeAddress), groups[0] * groups[1] * groups[2], dispatch);
        context.BindComputePipeline(commands, pipeline->pipeline);
        Downgrades::NotePipeline(reinterpret_cast<std::uint64_t>(pipeline->pipeline), stage.program.codeAddress, stage.program.result != nullptr ? stage.program.result->spirv.ContentHash().low : 0u);
        const auto bind = context.Resolved(&Graphics::DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets");
        bind(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout, ShaderRecompiler::RuntimeAbi::DescriptorSet, 1, &set, 0, nullptr);
        const VkDescriptorSet heapSet = heap->Set();
        bind(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout, ShaderRecompiler::RuntimeAbi::DirectHeapSet, 1, &heapSet, 0, nullptr);
        if (!push.empty()) context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, pipeline->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, Graphics::ComputePushConstantBytes, block.data());
        noteFaultToken(context, *recorder, commands, dispatch.stages, FaultStage::compute, indirect ? "dispatch indirect" : "dispatch", groups[0], groups[1] * groups[2]);
        const bool probed = probesOutput(stage.program.codeAddress) && outputProbeDue(stage.program.codeAddress);
        if (probed && indirect) probeArguments(context, *recorder, commands, dispatch.operation.dispatch.indirectAddress, argumentBuffer, argumentOffset, stage.program.codeAddress);
        const auto dispatchTiming = recorder->BeginGpuTiming(DispatchTimingKey);
        if (indirect) {
            if (watchesBatchPrograms()) {
                constexpr std::uint32_t Slots = 4096;
                constexpr VkDeviceSize SlotBytes = 16;
                if (hangArguments == nullptr) hangArguments = std::make_shared<Graphics::Buffer>(context, Slots * SlotBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                const auto slot = nextHangArgument++ % Slots;
                const VkBufferCopy region{argumentOffset, slot * SlotBytes, 3u * sizeof(std::uint32_t)};
                Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, argumentBuffer, hangArguments->Handle(), 1, &region);
                Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
                recorder->Keep(hangArguments);
                lastHangArguments = hangArguments->Bytes().data() + slot * SlotBytes;
            }
            context.Resolved(&Graphics::DeviceFunctions::cmdDispatchIndirect, "vkCmdDispatchIndirect")(commands, argumentBuffer, argumentOffset);
            totals.indirectDispatches++;
            readIndirectGroups(context, *recorder, commands, argumentBuffer, argumentOffset, stage.program.codeAddress);
        } else {
            context.Resolved(&Graphics::DeviceFunctions::cmdDispatch, "vkCmdDispatch")(commands, groups[0], groups[1], groups[2]);
        }
        recorder->EndGpuTiming(dispatchTiming);
        watchOnGpu(context, *recorder, commands, Downgrades::ProgramHash(stage.program.codeAddress), dispatch.operation.queue, dispatch.operation.submission);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_HOST_READ_BIT);
        finishHangMark(context, commands);
        recordCopyBacks(context, commands, serial);
        if (hangMarking()) {
            if (recorder->HasQueuedKeyStores()) recorder->FlushKeyStores();
            if (recorder->HasQueuedStores()) recorder->FlushStores();
            markHangPhase(context, *recorder, 3u);
        }
        if (probed) probeOutputs(context, *recorder, commands, dispatch, stage.program.codeAddress);

        recorder->Keep(pipeline);
        for (const auto& owned : stage.owners) recorder->Keep(std::const_pointer_cast<void>(owned));
        if (stage.tableMap != nullptr) recorder->Keep(std::const_pointer_cast<Tables::Mirror>(stage.tableMap));
        if (pageTable != nullptr) recorder->Keep(pageTable);
        noteStorageWrites(context, *recorder, written);
        if (nanCensus().draws != 0u && censusReads().empty()) {
            for (const auto& record : writtenRecords) censusRecord(context, *recorder, record, Downgrades::ProgramHash(stage.program.codeAddress));
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> writtenRanges;
        for (std::size_t index = 0; index < dispatch.uses.size(); ++index) {
            const auto& use = dispatch.uses[index];
            if (!use.write) continue;
            if (Downgrades::Watching()) Downgrades::WatchProgramWrite(stage.program.codeAddress, use.address, use.size);
            if (std::ranges::any_of(writtenRecords, [&](const std::shared_ptr<Images::Surface>& record) { return record->key.address <= use.address && use.address + use.size <= record->key.address + record->key.bytes; })) continue;
            writtenRanges.emplace_back(use.address, use.address + use.size);
        }
        writtenRecords.clear();
        if (Downgrades::Watching()) {
            for (const auto& [begin, end] : writtenRanges) Downgrades::WatchTouch("a dispatch write outside its records", begin, end - begin);
        }
        if (!writtenRanges.empty()) Events::NoteRanges(writtenRanges);
        if (marksDispatchWrites()) {
            for (const auto& [begin, end] : writtenRanges) {
                if (!directlyWritten(begin, end)) MemoryCore::Get().Pages().NoteGpuWritten(begin, end - begin);
            }
        }
        if (keepsGpuWrittenPages()) {
            for (const auto& [begin, end] : writtenRanges) {
                if (!directlyWritten(begin, end)) markResidencyNewer(begin, end);
            }
            if (const auto covered = copiedBack.find(serial); covered != copiedBack.end()) {
                for (const auto& [begin, end] : covered->second) clearResidencyNewer(begin, end);
            }
        }
        return true;
    }

    void markResidencyNewer(std::uint64_t begin, std::uint64_t end) {
        for (auto at = begin; at < end;) {
            const auto chunk = at & ~(ChunkBytes - 1u);
            const auto next = std::min(end, chunk + ChunkBytes);
            auto& mask = writtenPages[chunk];
            for (auto page = (at - chunk) / PageBytes; page <= (next - 1u - chunk) / PageBytes; ++page) mask |= 1u << page;
            at = next;
        }
    }

    void clearResidencyNewer(std::uint64_t begin, std::uint64_t end) {
        for (auto page = (begin + PageBytes - 1u) & ~(PageBytes - 1u); page + PageBytes <= end; page += PageBytes) {
            const auto chunk = page & ~(ChunkBytes - 1u);
            const auto found = writtenPages.find(chunk);
            if (found == writtenPages.end()) continue;
            found->second &= ~(1u << ((page - chunk) / PageBytes));
            if (found->second == 0u) writtenPages.erase(found);
        }
    }

    std::uint32_t fragmentOutputs(std::span<const Graphics::CompiledShader> shaders) {
        std::uint32_t mask = 0;
        for (const auto& shader : shaders) {
            if (shader.stage != ShaderRecompiler::ShaderStage::Fragment) continue;
            const auto hash = shader.program->spirv.ContentHash();
            const std::pair key{hash.low, hash.high};
            auto found = outputs.find(key);
            if (found == outputs.end()) found = outputs.emplace(key, Bindings::OutputLocations(shader.program->spirv.Words())).first;
            mask |= found->second;
        }
        return mask;
    }

    void prepareDepth(const Context& context, Graphics::Recorder& recorder, const std::shared_ptr<Images::DepthRecord>& record, const Graphics::DepthTarget& target, std::uint64_t program) {
        startDepth(context, recorder, record, target, program);
        if (target.htileAddress == 0u || !Images::HtileClearsOn()) return;
        const bool gpuWritten = record->TakeGpuHtileWrite();
        if (!gpuWritten && !record->HtileSaysCleared()) return;
        const auto bytes = record->HtileBytes();
        RouteMemory::OwnedSpan span;
        const bool served = RouteMemory::ServeOwned(context, target.htileAddress, bytes, &span) && span.handle != 0u && target.htileAddress >= span.origin;
        std::string reason = served ? "" : "HTILE memory not resident";
        if (served) {
            const auto commands = recorder.Commands();
            std::vector<std::shared_ptr<void>> kept;
            if (RouteMemory::RecordUploads(commands, kept) != 0u) {
                for (auto& object : kept) recorder.Keep(std::move(object));
            }
        }
        const auto buffer = reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span.handle));
        if (gpuWritten && served) {
            if (htileClears == nullptr) htileClears = std::make_unique<Images::HtileClears>(context);
            const Images::HtileClear clear{buffer, target.htileAddress - span.origin, bytes, target.htileStencil, target.clearDepth, target.clearStencil};
            if (htileClears->Apply(recorder, record, clear, reason)) {
                static const bool traced = std::getenv("APS5_RECODE_TRACE_HTILE_CHECKS") != nullptr;
                if (traced && totals.depthGpuHtileClears < 60u) std::fprintf(stderr, "[recode-htile-check] %llu: before program %016llx on record 0x%llx\n", static_cast<unsigned long long>(totals.depthGpuHtileClears), static_cast<unsigned long long>(Downgrades::ProgramHash(program)), static_cast<unsigned long long>(record->Key().address));
                totals.depthGpuHtileClears++;
                if (seen.insert("htile clear checked").second) std::fprintf(stderr, "[recode-depth] first GPU write over HTILE checked for a clear: record 0x%llx htile 0x%llx+0x%llx stencil in tiles %d clear %g/%u\n", static_cast<unsigned long long>(record->Key().address), static_cast<unsigned long long>(target.htileAddress), static_cast<unsigned long long>(bytes), target.htileStencil ? 1 : 0, static_cast<double>(target.clearDepth), static_cast<unsigned>(target.clearStencil));
                record->SetHtileSaysCleared(true);
            }
        }
        if (!reason.empty()) {
            if (gpuWritten) totals.depthGpuHtileRefusals++;
            else totals.depthHtileExpandRefusals++;
            if (seen.insert("htile clear: " + reason).second) std::fprintf(stderr, "[recode-depth] first refused HTILE %s: %s (record 0x%llx htile 0x%llx+0x%llx)\n", gpuWritten ? "clear" : "expansion", reason.c_str(), static_cast<unsigned long long>(record->Key().address), static_cast<unsigned long long>(target.htileAddress), static_cast<unsigned long long>(bytes));
            if (gpuWritten && Downgrades::Active() && program != 0u) {
                char detail[160];
                std::snprintf(detail, sizeof(detail), "GPU write over HTILE 0x%llx not applied to its depth record: %s", static_cast<unsigned long long>(target.htileAddress), reason.c_str());
                Downgrades::Note(program, Downgrades::Kind::HtileClearRefused, 8u, detail);
            }
        }
        if (!served || !record->HtileSaysCleared()) return;
        const auto commands = recorder.Commands();
        context.Resolved(&Graphics::DeviceFunctions::cmdFillBuffer, "vkCmdFillBuffer")(commands, buffer, target.htileAddress - span.origin, bytes, Images::ExpandedHtileWord(target.htileStencil));
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        noteWrittenBack(recorder, target.htileAddress, bytes);
        record->SetHtileSaysCleared(false);
        totals.depthHtileExpanded++;
    }

    bool seedFromPredecessors(const Context& context, Graphics::Recorder& recorder, const std::shared_ptr<Images::DepthRecord>& record) {
        const auto predecessors = record->TakePredecessors();
        if (predecessors.empty() || !seedsDepth() || !writesBackDepth()) return false;
        if (surfaceDetiler == nullptr) surfaceDetiler = std::make_unique<Images::SurfaceDetiler>(context);
        std::string reason;
        for (const auto& previous : predecessors) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
            if (!surfaceDetiler->WriteBackDepth(recorder, previous, written, reason)) {
                reason = "writing back the superseded record: " + reason;
                break;
            }
            for (const auto& [begin, size] : written) noteWrittenBack(recorder, begin, size);
        }
        if (reason.empty() && surfaceDetiler->SeedDepth(recorder, record, reason)) {
            if (totals.depthSupersedeSeeds++ == 0u) std::fprintf(stderr, "[recode-depth] first record seeded from the record it superseded: 0x%llx from %zu records\n", static_cast<unsigned long long>(record->Key().address), predecessors.size());
            static_cast<void>(record->TakeHtileWrite());
            static_cast<void>(record->TakePlaneWrite());
            record->SetHtileSaysCleared(false);
            return true;
        }
        totals.depthSeedRefusals++;
        if (seen.insert("supersede seed: " + reason).second) std::fprintf(stderr, "[recode-depth] first refused seed from a superseded record: %s (record 0x%llx)\n", reason.c_str(), static_cast<unsigned long long>(record->Key().address));
        return false;
    }

    void startDepth(const Context& context, Graphics::Recorder& recorder, const std::shared_ptr<Images::DepthRecord>& record, const Graphics::DepthTarget& target, std::uint64_t program) {
        const bool fresh = !record->Initialized();
        if (fresh && seedFromPredecessors(context, recorder, record)) return;
        const bool htileWritten = record->TakeHtileWrite();
        const bool planeWritten = record->TakePlaneWrite();
        if (!fresh && !htileWritten && !planeWritten) return;
        const bool stencil = (record->Aspects() & VK_IMAGE_ASPECT_STENCIL_BIT) != 0u;
        const auto htile = target.htileAddress != 0u ? Images::ReadHtile(target.htileAddress, record->HtileBytes(), target.htileDepthOnly, stencil, target.clearDepth) : Images::HtileState::Written;
        if (htileWritten) {
            if (htile == Images::HtileState::Cleared) {
                record->Initialize(recorder.Commands(), target.clearDepth, target.clearStencil);
                record->SetHtileSaysCleared(true);
                Images::Depths().NoteCleared();
                totals.depthHtileClears++;
            } else {
                totals.depthHtileOther++;
            }
        }
        if (!fresh) {
            if (!planeWritten || !seedsDepth() || (htileWritten && htile == Images::HtileState::Cleared)) return;
            if (surfaceDetiler == nullptr) surfaceDetiler = std::make_unique<Images::SurfaceDetiler>(context);
            std::string refusal;
            if (surfaceDetiler->SeedDepth(recorder, record, refusal)) {
                if (totals.depthReseeds++ == 0u && seen.insert("depth reseed").second) std::fprintf(stderr, "[recode-depth] first record seeded again after a CPU write into its planes: 0x%llx %ux%u format %d\n", static_cast<unsigned long long>(record->Key().address), record->Key().width, record->Key().height, static_cast<int>(record->Key().format));
                return;
            }
            totals.depthSeedRefusals++;
            if (seen.insert("depth reseed: " + refusal).second) std::fprintf(stderr, "[recode-depth] first refused seed after a CPU write: %s (record 0x%llx %ux%u format %d)\n", refusal.c_str(), static_cast<unsigned long long>(record->Key().address), record->Key().width, record->Key().height, static_cast<int>(record->Key().format));
            if (Downgrades::Active() && program != 0u) {
                char detail[160];
                std::snprintf(detail, sizeof(detail), "depth record 0x%llx kept its content after a CPU write into its planes: %s", static_cast<unsigned long long>(record->Key().address), refusal.c_str());
                Downgrades::Note(program, Downgrades::Kind::DepthSeedRefused, 8u, detail);
            }
            return;
        }
        if (htile != Images::HtileState::Written || !seedsDepth()) {
            record->Initialize(recorder.Commands(), target.clearDepth, target.clearStencil);
            record->SetHtileSaysCleared(htile == Images::HtileState::Cleared);
            Images::Depths().NoteCleared();
            return;
        }
        if (surfaceDetiler == nullptr) surfaceDetiler = std::make_unique<Images::SurfaceDetiler>(context);
        std::string reason;
        if (surfaceDetiler->SeedDepth(recorder, record, reason)) {
            totals.depthSeeds++;
            return;
        }
        totals.depthSeedRefusals++;
        if (seen.insert("depth seed: " + reason).second) std::fprintf(stderr, "[recode-depth] first refused seed from memory: %s (record 0x%llx %ux%u format %d)\n", reason.c_str(), static_cast<unsigned long long>(record->Key().address), record->Key().width, record->Key().height, static_cast<int>(record->Key().format));
        if (Downgrades::Active() && program != 0u) {
            char detail[160];
            std::snprintf(detail, sizeof(detail), "depth record 0x%llx started from its clear value, not memory: %s", static_cast<unsigned long long>(record->Key().address), reason.c_str());
            Downgrades::Note(program, Downgrades::Kind::DepthSeedRefused, 8u, detail);
        }
        record->Initialize(recorder.Commands(), target.clearDepth, target.clearStencil);
        Images::Depths().NoteCleared();
    }

    void writeBackDepthForReader(const Context& context, std::uint64_t address, std::uint64_t bytes) {
        const auto records = Images::Depths().Overlapping(address, bytes);
        if (records.empty()) return;
        totals.depthBufferReads++;
        if (seen.insert("depth buffer read").second) std::fprintf(stderr, "[recode-depth] first buffer read of depth record memory: 0x%llx+0x%llx\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
        if (!writesBackDepth()) return;
        for (const auto& record : records) {
            if (!record->Initialized() || !record->TakeDrawn()) continue;
            if (surfaceDetiler == nullptr) surfaceDetiler = std::make_unique<Images::SurfaceDetiler>(context);
            std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
            std::string reason;
            if (!surfaceDetiler->WriteBackDepth(*context.recorder, record, written, reason)) {
                record->MarkDrawn();
                totals.depthWriteBackRefusals++;
                if (seen.insert("depth write-back: " + reason).second) std::fprintf(stderr, "[recode-depth] first refused depth write-back: %s (record 0x%llx %ux%u format %d swizzle %u)\n", reason.c_str(), static_cast<unsigned long long>(record->Key().address), record->Key().width, record->Key().height, static_cast<int>(record->Key().format), record->Swizzle());
                continue;
            }
            if (totals.depthWriteBacks++ == 0u) std::fprintf(stderr, "[recode-depth] first depth write-back: record 0x%llx %ux%u format %d pipe-bank xor %u, %zu planes\n", static_cast<unsigned long long>(record->Key().address), record->Key().width, record->Key().height, static_cast<int>(record->Key().format), record->PipeBankXor(), written.size());
            for (const auto& [begin, size] : written) noteWrittenBack(*context.recorder, begin, size);
        }
    }

    static void noteClashWrites(const Resolve::ResolvedDraw& work) {
        if ((!UploadClash::Active() && !MetadataWatch::Active()) || work.stages.empty()) return;
        const auto program = Downgrades::ProgramHash(work.stages.back().program.codeAddress);
        for (std::size_t index = 0; index < work.uses.size() && index < work.imageBegin; ++index) {
            const auto& use = work.uses[index];
            if (!use.write || use.size == 0u || (index >= work.targetBegin && index < work.targetEnd)) continue;
            UploadClash::NoteWrite(use.address, use.size, program);
            MetadataWatch::CheckWrite(use.address, use.size, false, program);
        }
    }

    void noteMetadataWrites(const Resolve::ResolvedDraw& work) {
        const bool depth = Images::DepthRecordsOn() && Images::HtileClearsOn();
        if (metadataClears == nullptr && !depth) return;
        for (std::size_t index = 0; index < work.uses.size() && index < work.imageBegin; ++index) {
            const auto& use = work.uses[index];
            if (!use.write || use.size == 0u || (index >= work.targetBegin && index < work.targetEnd)) continue;
            if (metadataClears != nullptr) metadataClears->NoteWrite(use.address, use.size);
            if (depth && Images::Depths().NoteGpuHtileWrites(use.address, use.size) != 0u) {
                static const bool traced = std::getenv("APS5_RECODE_TRACE_HTILE_CHECKS") != nullptr;
                static std::atomic<std::uint32_t> lines{0};
                if (traced && !work.stages.empty() && lines.fetch_add(1, std::memory_order_relaxed) < 60u) std::fprintf(stderr, "[recode-htile-write] program %016llx writes 0x%llx+0x%llx over a depth record's HTILE\n", static_cast<unsigned long long>(Downgrades::ProgramHash(work.stages.back().program.codeAddress)), static_cast<unsigned long long>(use.address), static_cast<unsigned long long>(use.size));
            }
        }
    }

    void applyMetadataClear(const Context& context, const std::shared_ptr<Images::Surface>& record, std::uint64_t program) {
        if (metadataClears == nullptr || record == nullptr || context.recorder == nullptr || !metadataClears->Pending(record->key.address, record->key.bytes)) return;
        auto* vulkan = dynamic_cast<Images::VulkanSurface*>(record->device.get());
        auto* table = Resolve::Surfaces();
        if (vulkan == nullptr || table == nullptr) return;
        drainSurfaces(context, *context.recorder);
        std::string reason;
        const bool applied = metadataClears->Apply(*context.recorder, record->key.address, *vulkan, reason);
        for (const auto& [begin, size] : metadataClears->TakeExpanded()) noteWrittenBack(*context.recorder, begin, size);
        if (applied || !reason.empty()) context.recorder->Keep(record);
        if (applied) {
            markRecordWritten(*context.recorder, *table, *record);
            totals.metadataClears++;
            return;
        }
        if (reason.empty()) return;
        if (seen.insert("metadata clear: " + reason).second) std::fprintf(stderr, "[recode-metadata] first refused clear: %s (record 0x%llx %ux%u format %u)\n", reason.c_str(), static_cast<unsigned long long>(record->key.address), record->key.width, record->key.height, record->key.format);
        if (Downgrades::Active() && program != 0u) {
            char detail[160];
            std::snprintf(detail, sizeof(detail), "fast clear of record 0x%llx not applied: %s", static_cast<unsigned long long>(record->key.address), reason.c_str());
            Downgrades::Note(program, Downgrades::Kind::MetadataClearRefused, 0u, detail);
        }
    }

    void writeBackForReaders(const Context& context, const Resolve::ResolvedDraw& work) {
        noteClashWrites(work);
        noteMetadataWrites(work);
        auto* table = Resolve::Surfaces();
        if (table == nullptr || context.recorder == nullptr || !ownsImages()) return;
        for (std::size_t index = 0; index < work.uses.size() && index < work.imageBegin; ++index) {
            const auto& use = work.uses[index];
            if (use.write || use.size == 0u || (index >= work.targetBegin && index < work.targetEnd)) continue;
            if (Images::DepthRecordsOn()) writeBackDepthForReader(context, use.address, use.size);
            for (const auto& record : table->StaleOverlapping(use.address, use.size)) {
                if (record->retired) continue;
                if (table->ChangedSinceWrite(*record)) {
                    table->MarkWrittenBack(*record);
                    totals.readerStale++;
                    continue;
                }
                applyMetadataClear(context, record, 0u);
                if (surfaceDetiler == nullptr) surfaceDetiler = std::make_unique<Images::SurfaceDetiler>(context);
                std::string reason;
                if (!surfaceDetiler->WriteBack(*context.recorder, record, reason)) {
                    totals.readerWriteBackRefusals++;
                    if (seen.insert("reader write-back: " + reason).second) std::fprintf(stderr, "[recode-record] first refused write-back for a buffer reader: %s\n", reason.c_str());
                    continue;
                }
                noteWrittenBack(*context.recorder, record->key.address, record->key.bytes);
                table->MarkWrittenBack(*record);
                totals.readerWriteBacks++;
                totals.readerWriteBackBytes += record->key.bytes;
            }
        }
    }

    void handOverForWriters(const Context& context, const Resolve::ResolvedDraw& work) {
        auto* table = Resolve::Surfaces();
        if (table == nullptr || context.recorder == nullptr || !ownsImages() || !handsOverForWriters()) return;
        for (std::size_t index = 0; index < work.uses.size() && index < work.imageBegin; ++index) {
            const auto& use = work.uses[index];
            if (!use.write || use.size == 0u || (index >= work.targetBegin && index < work.targetEnd)) continue;
            if (Images::DepthRecordsOn() && depthHandOverForWriters()) {
                const auto before = totals.depthWriteBacks;
                writeBackDepthForReader(context, use.address, use.size);
                totals.depthWriterHandOvers += totals.depthWriteBacks - before;
            }
            for (const auto& record : table->Overlapping(use.address, use.size)) {
                if (!record->seeded || record->retired || !record->gpuNewer) continue;
                std::string why;
                if (record->mirrorStale) static_cast<void>(handOverShape(context, *table, record, why));
                else table->Retire(record);
                totals.writerHandOvers++;
            }
        }
    }

    bool record(const Context& context, const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::string& reason) {
        writtenRecords.clear();
        stageStorageImages.clear();
        directWrites.clear();
        consumeForUses(draw);
        auto preMark = std::chrono::steady_clock::now();
        writeBackForReaders(context, draw);
        if (hangMarking() && context.recorder != nullptr) markHangPhase(context, *context.recorder, 5u);
        totals.preSplit[0] += elapsed(preMark);
        preMark = std::chrono::steady_clock::now();
        handOverForWriters(context, draw);
        if (hangMarking() && context.recorder != nullptr) markHangPhase(context, *context.recorder, 6u);
        totals.preSplit[1] += elapsed(preMark);
        if (std::ranges::all_of(draw.stages, [](const Resolve::ResolvedStage& stage) { return stage.deferredImages.empty() && !tableImagesDeferred(stage); })) return recordWithSlots(context, draw, parameters, reason);
        preMark = std::chrono::steady_clock::now();
        auto bound = draw;
        std::vector<std::shared_ptr<Graphics::StorageTexture>> written;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> writtenRanges;
        constexpr auto storageWrite = Heap::RequestFlag::Storage | Heap::RequestFlag::Written;
        for (auto& stage : bound.stages) {
            if (!bindTableImages(context, stage, reason)) return false;
            if (stage.deferredImages.empty()) continue;
            if (!bindDeferredImages(context, stage, written, reason)) return false;
            for (const auto& deferred : stage.deferredImages) {
                if ((deferred.request.flags & storageWrite) != storageWrite) continue;
                try {
                    const auto resource = Graphics::DecodeTextureResource(deferred.request.words);
                    writtenRanges.emplace_back(resource.baseAddress, resource.baseAddress + Graphics::SurfaceGuestBytes(resource));
                } catch (const std::exception&) {
                }
            }
        }
        totals.deferredReads++;
        totals.preSplit[2] += elapsed(preMark);
        if (!recordWithSlots(context, bound, parameters, reason)) return false;
        if (!written.empty() || !writtenRecords.empty()) {
            noteStorageWrites(context, *context.recorder, written);
            totals.drawStorageWrites += written.size() + writtenRecords.size();
        }
        writtenRecords.clear();
        if (Downgrades::Watching()) {
            for (const auto& [begin, end] : writtenRanges) Downgrades::WatchTouch("a draw storage write outside its records", begin, end - begin);
        }
        if (!writtenRanges.empty()) Events::NoteRanges(writtenRanges);
        return true;
    }

    bool recordWithSlots(const Context& context, const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::string& reason) {
        auto* recorder = context.recorder;
        const auto* heap = Resolve::HeapDevice();
        if (recorder == nullptr || heap == nullptr || draw.state == nullptr) {
            reason = "no recorder or heap";
            return false;
        }
        if (!attach(context, reason) || !precheck(draw, parameters, nullptr, reason)) return false;
        const auto& state = *draw.state;
        std::optional<Graphics::MeshGrid> meshGrid;
        if (state.stages.mesh) {
            const auto& mesh = *state.stages.mesh;
            const auto inputSize = mesh.inputPrimitive == 1u ? 1u : mesh.inputPrimitive == 2u ? 2u : 3u;
            if (parameters.indexCount < inputSize || parameters.instanceCount == 0u) {
                totals.meshEmpty++;
                return true;
            }
            const auto step = mesh.inputPrimitive == 5u || mesh.inputPrimitive == 6u ? 1u : inputSize;
            const auto primitives = (parameters.indexCount - inputSize) / step + 1u;
            meshGrid = Graphics::MeshGridFor(context, (primitives - 1u) / mesh.primitivesPerGroup + 1u, parameters.instanceCount);
            if (!meshGrid) {
                reason = "mesh: workgroup count past the device limits";
                return false;
            }
        }
        if (Downgrades::Active() && std::ranges::any_of(state.colors, [](const Graphics::ColorTarget& color) { return color.layers > 1u; }) && layeredTraceLines.fetch_add(1, std::memory_order_relaxed) < 8u) {
            std::string targets;
            for (const auto& color : state.colors) {
                char text[160];
                std::snprintf(text, sizeof(text), " [0x%llx %ux%u slice %u layers %u format %d]", static_cast<unsigned long long>(color.address), color.extent.width, color.extent.height, color.arraySlice, color.layers, static_cast<int>(color.format));
                targets += text;
            }
            std::fprintf(stderr, "[recode-layered] program %016llx render layers %u layer exports 0x%x depth %d:%s\n", static_cast<unsigned long long>(Downgrades::ProgramHash(draw.stages.back().program.codeAddress)), state.renderLayers, state.layerExports, state.depth ? 1 : 0, targets.c_str());
        }
        auto phase = std::chrono::steady_clock::now();
        drainSurfaces(context, *recorder);
        totals.surfaceNanoseconds += elapsed(phase);
        std::vector<Graphics::CompiledShader> shaders;
        for (const auto& stage : draw.stages) shaders.push_back({stage.program.stage, stage.program.result.get(), stage.program.pushOffset});
        auto splitMark = std::chrono::steady_clock::now();
        const auto split = [&](std::size_t index) {
            const auto now = std::chrono::steady_clock::now();
            totals.recordSplit[index] += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - splitMark).count());
            splitMark = now;
        };
        const auto& attributes = draw.stages.front().program.vertexAttributes;
        Graphics::VertexInputLayout vertexInput;
        std::vector<VkBuffer> vertexBuffers;
        std::vector<VkDeviceSize> vertexOffsets;
        std::vector<InputCopy> inputCopies;
        std::vector<InputRange> inputRanges;
        if (!attributes.empty()) {
            vertexInput = Graphics::BuildVertexInputLayout(context, attributes);
            for (std::size_t index = 0; index < attributes.size(); ++index) {
                const auto& attribute = attributes[index];
                if (vertexInput.attributes[index].format != Graphics::DecodeVertexFormat(attribute).format) {
                    reason = "widened vertex format";
                    return false;
                }
                const auto address = vertexAddress(attribute);
                inputRanges.push_back({address, unclampedInputEnd(attribute, address, address + Graphics::VertexBufferExtent(attribute)), 0});
            }
            const auto unions = mergeInputs(inputRanges);
            std::vector<DrawInput> unionInputs;
            std::vector<InputSource> unionSources;
            for (std::size_t index = 0; index < unions.size(); ++index) {
                const auto& [begin, end] = unions[index];
                InputSource source = InputSource::Imported;
                const auto input = drawInput(context, begin, end - begin, source);
                if (!input) {
                    std::uint64_t committed = 0;
                    for (const auto& [from, to] : GuestMemory::CommittedRanges(begin, static_cast<std::size_t>(end - begin))) committed += to - from;
                    auto& pages = MemoryCore::Get().Pages();
                    char range[192];
                    std::snprintf(range, sizeof(range), " 0x%llx+0x%llx (gpu pending %d, gpu owned %d, gpu written %d, committed 0x%llx, residency %d)", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin), gpuPending(begin, end - begin) ? 1 : 0, pages.GpuOwnedIn(begin, end - begin).empty() ? 0 : 1, pages.GpuWrittenIn(begin, end - begin) ? 1 : 0, static_cast<unsigned long long>(committed), ResidencyService::Get().Attached() ? 1 : 0);
                    reason = std::string("vertex buffer not imported") + range;
                    if (Downgrades::Active() && inputRefusalLines.fetch_add(1, std::memory_order_relaxed) < 16u) std::fprintf(stderr, "[recode-input] vertex buffer 0x%llx+0x%llx not imported: source %d, gpu pending %d, GPU-owned pages %zu, aliased %d, program %016llx\n", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin), static_cast<int>(source), gpuPending(begin, end - begin) ? 1 : 0, MemoryCore::Get().Pages().GpuOwnedIn(begin, end - begin).size(), DirectMemoryAliased_nid_postfix(static_cast<std::uintptr_t>(begin), static_cast<std::size_t>(end - begin)) ? 1 : 0, static_cast<unsigned long long>(Downgrades::ProgramHash(draw.stages.back().program.codeAddress)));
                    return false;
                }
                if (source == InputSource::Written && copiesInputs() && end - begin <= InputCopyLimitBytes) inputCopies.push_back({begin, end - begin, index});
                if (source == InputSource::Mirrored) compareInput(context, *recorder, begin, end - begin, *input);
                recorder->NotePendingRead(begin, end - begin, Graphics::Recorder::ReadKind::DrawInput);
                unionInputs.push_back(*input);
                unionSources.push_back(source);
            }
            for (const auto& range : inputRanges) {
                countInput(totals, unionSources[range.merged]);
                vertexBuffers.push_back(unionInputs[range.merged].buffer);
                vertexOffsets.push_back(unionInputs[range.merged].offset + (range.begin - unions[range.merged].first));
            }
            totals.inputsMerged += inputRanges.size() - unions.size();
        }
        split(0);
        std::vector<VkImageView> views;
        std::vector<std::shared_ptr<void>> owners;
        std::vector<std::shared_ptr<Graphics::StorageTexture>> residents;
        phase = std::chrono::steady_clock::now();
        recordingProgram = draw.stages.empty() ? 0u : draw.stages.back().program.codeAddress;
        const bool memoUsable = std::exchange(targetMemo.valid, false) && !recordsTargets();
        std::vector<std::shared_ptr<Images::Surface>> targetRecords;
        if (memoUsable && targetsFromMemo(context, *recorder, state, views, owners, residents)) {
            totals.targetMemoHits++;
        } else {
            if (memoizesTargets()) totals.targetMemoMisses++;
            views.clear();
            owners.clear();
            residents.clear();
            if (recordsTargets() && !state.colors.empty()) {
                std::vector<VkImageView> recordViews;
                for (const auto& color : state.colors) {
                    MetadataWatch::NoteTarget(color);
                    if (Images::MetadataClearsOn()) {
                        if (metadataClears == nullptr) metadataClears = std::make_unique<Images::MetadataClears>(context);
                        metadataClears->NoteTarget(color);
                    }
                    VkImageView view = VK_NULL_HANDLE;
                    auto record = targetRecord(context, color, view);
                    if (targetDetailOn() && !draw.stages.empty() && tracedProgram(Downgrades::ProgramHash(draw.stages.back().program.codeAddress)) && targetDetailLines.fetch_add(1, std::memory_order_relaxed) < 200u) {
                        const auto resource = Graphics::SurfaceForTarget(color);
                        std::fprintf(stderr, "[recode-target-detail] program %016llx slot %zu cb 0x%llx surface 0x%llx resource 0x%llx %ux%u bytes 0x%llx format %d tile %d element %u dcc 0x%llx cmask 0x%llx mip %u slice %u -> %s 0x%llx+0x%llx\n", static_cast<unsigned long long>(Downgrades::ProgramHash(draw.stages.back().program.codeAddress)), static_cast<std::size_t>(&color - state.colors.data()), static_cast<unsigned long long>(color.address), static_cast<unsigned long long>(color.surfaceAddress), static_cast<unsigned long long>(resource.baseAddress), color.extent.width, color.extent.height, static_cast<unsigned long long>(color.bytes), static_cast<int>(color.format), static_cast<int>(color.tileMode), color.elementBytes, static_cast<unsigned long long>(color.dccAddress), static_cast<unsigned long long>(color.cmaskAddress), color.mip, color.arraySlice, record != nullptr ? "record" : "no record", static_cast<unsigned long long>(record != nullptr ? record->key.address : 0u), static_cast<unsigned long long>(record != nullptr ? record->key.bytes : 0u));
                    }
                    if (record != nullptr && !targetTraceList().empty() && !draw.stages.empty() && tracedProgram(Downgrades::ProgramHash(draw.stages.back().program.codeAddress)) && traceTargetRecord(record->key.address, record->key.bytes)) {
                        const auto resource = Graphics::SurfaceForTarget(color);
                        std::fprintf(stderr, "[recode-target] program %016llx color 0x%llx %ux%u mip %u tail %u slice %u extent %ux%u -> record 0x%llx+0x%llx %ux%u mips %u dimension %u\n", static_cast<unsigned long long>(Downgrades::ProgramHash(draw.stages.back().program.codeAddress)), static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, color.mip, color.mipTail ? 1u : 0u, color.arraySlice, color.extent.width, color.extent.height, static_cast<unsigned long long>(record->key.address), static_cast<unsigned long long>(record->key.bytes), record->key.width, record->key.height, record->key.mips, record->key.dimension);
                    }
                    if (record == nullptr) break;
                    recordViews.push_back(view);
                    targetRecords.push_back(std::move(record));
                }
                if (targetRecords.size() == state.colors.size()) {
                    views = std::move(recordViews);
                    for (const auto& record : targetRecords) owners.push_back(record);
                    totals.recordTargetDraws++;
                } else {
                    targetRecords.clear();
                }
            }
            for (const auto& color : state.colors) {
                if (!targetRecords.empty()) break;
                if (Graphics::AttachmentProxyFormat(context, color.format) != VK_FORMAT_UNDEFINED) {
                    reason = "proxied color format";
                    return false;
                }
                noteRefusedImage(lastTargetRefusal.empty() ? std::string("colour target: not offered to records") : "colour target: " + lastTargetRefusal);
                reason = "colour target without a record: " + (lastTargetRefusal.empty() ? std::string("not offered to records") : lastTargetRefusal);
                return false;
            }
            std::shared_ptr<Images::DepthRecord> depthRecord;
            if (state.depth && Images::DepthRecordsOn()) {
                std::string why;
                depthRecord = Images::Depths().Find(context, *state.depth, why);
                if (depthRecord == nullptr && seen.insert("depth: " + why).second) std::fprintf(stderr, "[recode-depth] first refusal: %s (0x%llx %ux%u format %d samples %u slices %u-%u)", why.c_str(), static_cast<unsigned long long>(state.depth->address), state.depth->extent.width, state.depth->extent.height, static_cast<int>(state.depth->format), state.depth->samples, state.depth->firstSlice, state.depth->lastSlice), std::fputc(10, stderr);
                if (depthRecord != nullptr) prepareDepth(context, *recorder, depthRecord, *state.depth, recordingProgram);
                if (depthRecord != nullptr) maybeDumpDepth(context, *recorder, depthRecord, Downgrades::ProgramHash(recordingProgram));
            }
            if (depthRecord != nullptr) {
                depthRecord->MarkDrawn();
                views.push_back(depthRecord->View());
                recorder->Keep(depthRecord);
                totals.depthRecordDraws++;
            } else if (state.depth) {
                reason = Images::DepthRecordsOn() ? "depth target without a record" : "depth records are off";
                return false;
            }
        }
        if (!targetRecords.empty()) drainSurfaces(context, *recorder);
        Graphics::PipelineSetLayouts sets;
        sets.main = setLayout(context, draw, 0);
        sets.split = draw.layout.split;
        if (sets.split) sets.fragment = setLayout(context, draw, 1);
        sets.mainKey = draw.layout.keys[0];
        sets.fragmentKey = draw.layout.keys[1];
        sets.heap = heap->Layout();
        totals.targetNanoseconds += elapsed(phase);
        phase = std::chrono::steady_clock::now();
        auto pipeline = Graphics::CachedPipeline(context, state, vertexInput, sets, shaders, VK_IMAGE_LAYOUT_GENERAL);
        const auto pipelineSpent = elapsed(phase);
        totals.graphicsPipelineNanoseconds += pipelineSpent;
        if (pipelineSpent >= 1000000u && Downgrades::Active()) {
            for (const auto& stage : draw.stages) Downgrades::NoteCompile(stage.program.codeAddress, pipelineSpent);
        }
        if (pipeline == nullptr) {
            reason = "no pipeline";
            return false;
        }
        const bool meshWindow = Graphics::UsesMeshWindow(shaders);
        if (meshWindow) Graphics::PrepareMeshWindow(context);
        auto framebuffer = pipeline->AcquireFramebuffer(views, owners, state.renderExtent, state.renderLayers);
        const auto masked = maskedOutputs(state, fragmentOutputs(shaders));
        split(1);

        recorder->OpenBatch();
        const auto serial = recorder->OpenBatchSerial();
        const auto completed = recorder->CompletedThrough();
        split(2);
        std::vector<CopiedSpan> copiedInputs;
        for (const auto& copy : inputCopies) {
            const auto input = copiedInput(copiedInputs, copy.address, copy.bytes, serial, completed);
            if (!input) continue;
            for (std::size_t index = 0; index < inputRanges.size(); ++index) {
                if (inputRanges[index].merged != copy.merged) continue;
                vertexBuffers[index] = input->buffer;
                vertexOffsets[index] = input->offset + (inputRanges[index].begin - copy.address);
                totals.inputsCopied++;
            }
        }
        std::array<VkDescriptorSet, 2> descriptorSets{};
        std::shared_ptr<Graphics::Buffer> pageTable;
        for (std::uint32_t set = 0; set < (sets.split ? 2u : 1u); ++set) {
            descriptorSets[set] = allocateSet(context, set == 0u ? sets.main : sets.fragment, serial, completed);
            if (!writeSet(context, draw, set, descriptorSets[set], serial, completed, pageTable, reason)) return false;
        }
        split(3);

        std::array<std::byte, Graphics::PipelinePushConstantBytes> block{};
        for (std::size_t index = 0; index < draw.stages.size(); ++index) {
            const auto& push = draw.stages[index].program.words.pushConstants;
            if (push.empty()) continue;
            if (shaders[index].pushConstantOffset + push.size() > block.size()) {
                reason = "push data outside the block";
                return false;
            }
            std::memcpy(block.data() + shaders[index].pushConstantOffset, push.data(), push.size());
            servePushKnownBuffers(context, draw.stages[index], std::span(block).subspan(shaders[index].pushConstantOffset), serial, completed);
        }
        VkShaderStageFlags pushStages = Graphics::PushConstantStages(shaders);
        if (meshGrid) {
            for (const auto& shader : shaders) {
                const auto bytes = shader.program->pushConstants.size();
                if (bytes != 0u && shader.pushConstantOffset < Graphics::PipelinePushSlotBytes && shader.pushConstantOffset + bytes > ShaderRecompiler::MeshDrawPushOffsetBytes) {
                    reason = "mesh: stage push data overlaps the mesh draw words";
                    return false;
                }
            }
            const std::array<std::uint32_t, ShaderRecompiler::MeshDrawPushBytes / 4> words{parameters.indexCount, parameters.firstVertex, parameters.firstInstance, (parameters.indexed ? parameters.indexSize : 0u) | meshGrid->layerStride << ShaderRecompiler::MeshLayerStrideShift, 0u, 0u};
            std::memcpy(block.data() + ShaderRecompiler::MeshDrawPushOffsetBytes, words.data(), sizeof(words));
            pushStages |= VK_SHADER_STAGE_MESH_BIT_EXT;
            totals.meshDraws++;
        }

        VkBuffer argumentBuffer = VK_NULL_HANDLE;
        VkDeviceSize argumentOffset = 0;
        std::optional<DrawInput> indirectCount;
        std::optional<ArgumentPatch> argumentPatch;
        if (parameters.indirect) {
            using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
            const auto& args = *parameters.indirect;
            const auto bytes = static_cast<std::size_t>(args.RangeBytes());
            const bool rewritten = args.vertexRule == Rule::Constant || args.instanceRule == Rule::Constant;
            const auto* source = reinterpret_cast<const std::byte*>(args.arguments);
            if (!args.countIndirect && !Graphics::Recorder::SnapshotWriteOverlaps(args.arguments, bytes) && recorder->PendingLeaseSerial(args.arguments, bytes) == 0u && !gpuPending(args.arguments, bytes) && MemoryCore::Get().Pages().GpuOwnedIn(args.arguments, bytes).empty() && GuestMemory::Accessible(source, bytes, false)) {
                std::vector<std::byte> copy(source, source + bytes);
                for (std::uint32_t record = 0; record < args.count; ++record) {
                    const auto base = static_cast<std::size_t>(record) * args.stride;
                    if (args.vertexRule == Rule::Constant) std::memcpy(copy.data() + base + args.VertexDwordOffset(), &args.vertexConstant, 4);
                    if (args.instanceRule == Rule::Constant) std::memcpy(copy.data() + base + args.InstanceDwordOffset(), &args.instanceConstant, 4);
                    if (argumentCensusOn() && Downgrades::Active() && base + 8u <= copy.size()) {
                        std::uint32_t elements = 0;
                        std::uint32_t instances = 0;
                        std::memcpy(&elements, copy.data() + base, 4);
                        std::memcpy(&instances, copy.data() + base + 4, 4);
                        noteArgumentCensus(draw.stages.empty() ? 0u : draw.stages.back().program.codeAddress, elements, instances, 1u);
                    }
                }
                const auto offset = ringWrite(copy, serial, completed);
                if (!offset.has_value()) {
                    reason = "ring full";
                    return false;
                }
                argumentBuffer = ring->Handle();
                argumentOffset = *offset;
                totals.snapshotArguments++;
            } else {
                if (rewritten && !PatchableArguments(args)) {
                    reason = "constant rule on GPU-written arguments the patch does not take";
                    return false;
                }
                const auto readInput = [&](std::uint64_t address, std::uint64_t size, bool& mirrored) -> std::optional<DrawInput> {
                    mirrored = false;
                    if (mirrorPart("args")) {
                        if (auto input = mirrorInput(address, size)) {
                            mirrored = true;
                            return input;
                        }
                        if (gpuPending(address, size)) return std::nullopt;
                    }
                    return importedInput(context, address, size);
                };
                bool argumentsMirrored = false;
                const auto argumentInput = readInput(args.arguments, bytes, argumentsMirrored);
                if (!argumentInput) {
                    reason = "indirect arguments neither in the mirror nor imported";
                    return false;
                }
                if (args.countIndirect) {
                    if (!context.drawIndirectCount) {
                        reason = "no indirect count";
                        return false;
                    }
                    bool countMirrored = false;
                    indirectCount = readInput(args.countAddress, sizeof(std::uint32_t), countMirrored);
                    if (!indirectCount) {
                        reason = "indirect count neither in the mirror nor imported";
                        return false;
                    }
                    if (!countMirrored) recorder->NotePendingRead(args.countAddress, sizeof(std::uint32_t), Graphics::Recorder::ReadKind::Indirect);
                }
                if (argumentsMirrored) totals.mirrorArguments++;
                else recorder->NotePendingRead(args.arguments, bytes, Graphics::Recorder::ReadKind::Indirect);
                argumentBuffer = argumentInput->buffer;
                argumentOffset = argumentInput->offset;
                if (rewritten && bytes != 0u) {
                    const auto placed = space->Allocate(bytes, serial, completed);
                    if (!placed.has_value()) {
                        reason = "ring full";
                        return false;
                    }
                    argumentPatch = ArgumentPatch{argumentBuffer, argumentOffset, *placed};
                    argumentBuffer = ring->Handle();
                    argumentOffset = *placed;
                }
            }
            totals.indirect++;
        }
        std::optional<DrawInput> indexInput;
        if (parameters.indexed && !meshGrid) {
            const auto bytes = static_cast<std::size_t>(parameters.indexCount) * parameters.indexSize;
            InputSource source = InputSource::Imported;
            if (bytes != 0u) indexInput = drawInput(context, parameters.indexAddress, bytes, source);
            if (!indexInput) {
                reason = bytes == 0u ? "indexed draw without indices" : "indices not imported";
                if (Downgrades::Active() && inputRefusalLines.fetch_add(1, std::memory_order_relaxed) < 16u) std::fprintf(stderr, "[recode-input] indices 0x%llx+0x%zx (%u of %u bytes) not imported: source %d, gpu pending %d, GPU-owned pages %zu, aliased %d, program %016llx\n", static_cast<unsigned long long>(parameters.indexAddress), bytes, parameters.indexCount, parameters.indexSize, static_cast<int>(source), bytes != 0u && gpuPending(parameters.indexAddress, bytes) ? 1 : 0, bytes != 0u ? MemoryCore::Get().Pages().GpuOwnedIn(parameters.indexAddress, bytes).size() : 0u, bytes != 0u && DirectMemoryAliased_nid_postfix(static_cast<std::uintptr_t>(parameters.indexAddress), bytes) ? 1 : 0, static_cast<unsigned long long>(Downgrades::ProgramHash(draw.stages.back().program.codeAddress)));
                return false;
            }
            countInput(totals, source);
            if (source == InputSource::Written && copiesInputs() && bytes <= InputCopyLimitBytes) {
                if (const auto copied = copiedInput(copiedInputs, parameters.indexAddress, bytes, serial, completed)) {
                    indexInput = copied;
                    totals.inputsCopied++;
                }
            }
            recorder->NotePendingRead(parameters.indexAddress, bytes, Graphics::Recorder::ReadKind::DrawInput);
        }
        split(4);

        markHangPhase(context, *recorder, 1u);
        if (recorder->HasQueuedKeyStores()) recorder->FlushKeyStores();
        if (recorder->HasQueuedStores()) recorder->FlushStores();
        markHangPhase(context, *recorder, 2u);
        if (pageTable != nullptr && !draw.decodeReads.empty()) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> dirtyDecodeReads;
            auto& pageState = MemoryCore::Get().Pages();
            for (const auto& [address, size] : draw.decodeReads) {
                if (address == 0u || size == 0u) continue;
                if (servesFaultPages()) {
                    if (const auto pages = ResidencyService::Get().UnmappedPages(address, size); !pages.empty()) RouteMemory::ServeFaultPages(pages);
                }
                if (!pageState.CleanIn(address, size)) dirtyDecodeReads.emplace_back(address, size);
            }
            if (!dirtyDecodeReads.empty()) static_cast<void>(RouteMemory::ConsumeIn(dirtyDecodeReads));
        }
        if (RouteMemory::UploadsPending()) {
            phase = std::chrono::steady_clock::now();
            std::vector<std::shared_ptr<void>> kept;
            const auto uploadCommands = recorder->Commands();
            const auto uploadTiming = recorder->BeginGpuTiming(UploadTimingKey);
            if (RouteMemory::RecordUploads(uploadCommands, kept) != 0u) totals.residencyLands++;
            recorder->EndGpuTiming(uploadTiming);
            for (auto& object : kept) recorder->Keep(std::move(object));
            const auto uploadSpent = elapsed(phase);
            totals.uploadNanoseconds += uploadSpent;
            StageCosts::Note(StageCosts::Stage::HostCopy, uploadSpent);
        }
        split(5);
        const auto key = Graphics::DrawRenderPassKey(context, state, views);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> passReads;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> passWrites;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> drawTargets;
        for (std::size_t index = 0; index < draw.uses.size(); ++index) {
            const auto& use = draw.uses[index];
            if (use.size == 0u) continue;
            const std::pair range{use.address, use.address + use.size};
            if (use.write && Downgrades::Watching() && !draw.stages.empty()) Downgrades::WatchProgramWrite(draw.stages.back().program.codeAddress, use.address, use.size);
            if (index >= draw.targetBegin && index < draw.targetEnd) drawTargets.push_back(range);
            else if (use.write) passWrites.push_back(range);
            else passReads.push_back(range);
        }
        if (!passWrites.empty()) RouteMemory::NoteGpuWrites(passWrites);
        std::vector<Graphics::PassAttachment> attachments;
        for (const auto& resident : residents) attachments.push_back({resident->Image(), true});
        Graphics::PassAccess access{passReads, passWrites, stageStorageImages, attachments};
        const bool storesImages = std::ranges::any_of(stageStorageImages, [](const std::pair<VkImage, bool>& image) { return image.second; });
        access.anyReads = pageTable != nullptr;
        access.anyWrites = pageTable != nullptr && std::ranges::any_of(draw.stages, [](const Resolve::ResolvedStage& stage) { return Graphics::ShaderStoresThroughAddresses(stage.program.result->spirv); });
        const bool feedback = recorder->RenderPassOpen() && overlaps(passReads, passTargets);
        if (feedback) totals.passFeedback++;
        const bool snapshotArguments = parameters.indirect && ring != nullptr && argumentBuffer == ring->Handle() && !argumentPatch.has_value();
        const bool forced = !mergesPasses() || !draw.complete || feedback || storesImages || (parameters.indirect && !snapshotArguments) || indirectCount.has_value();
        const bool fenced = state.depth.has_value();
        split(6);
        if (hangMarking()) {
            const auto marked = recorder->Commands();
            Graphics::RecordMemoryBarrier(context, marked, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            markHang(context, *recorder, marked, draw.stages.empty() ? 0u : Downgrades::ProgramHash(draw.stages.back().program.codeAddress), 0u, draw);
        }
        const auto start = recorder->StartDrawPass(key, forced, fenced, access);
        split(7);
        if (recorder->OpenBatchSerial() != serial) totals.batchMoves++;
        const auto commands = start.commands;
        auto& recording = recorder->GraphicsState();
        if (start.continued) {
            totals.passesContinued++;
            for (const auto& range : drawTargets) {
                if (std::ranges::find(passTargets, range) == passTargets.end()) passTargets.push_back(range);
            }
        } else {
            totals.passesBegun++;
            passTargets = std::move(drawTargets);
            recorder->PrepareSampleSlot();
        }
        if (start.barrier) {
            Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT);
        }
        const auto* maskedState = masked.has_value() ? &*masked : nullptr;
        if (start.continued) {
            pipeline->Continue(recording, state, maskedState);
        } else {
            if (argumentPatch) {
                PatchIndirectArguments(context, commands, *parameters.indirect, argumentPatch->source, argumentPatch->sourceOffset, ring->Handle(), argumentPatch->ringOffset);
                totals.patchedArguments++;
            }
            if (parameters.indirect && !snapshotArguments) probeDrawArguments(context, *recorder, commands, *parameters.indirect, parameters.indexed, argumentPatch ? "patched copy" : "read in place", argumentBuffer, argumentOffset, indirectCount, draw.stages.empty() ? 0u : draw.stages.back().program.codeAddress);
            if (parameters.indirect) Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
            pipeline->Begin(recording, *framebuffer, state.renderExtent, state, maskedState);
        }

        const auto bind = context.Resolved(&Graphics::DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets");
        bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Layout(), ShaderRecompiler::RuntimeAbi::DescriptorSet, 1, &descriptorSets[0], 0, nullptr);
        if (sets.split) bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Layout(), ShaderRecompiler::RuntimeAbi::FragmentDescriptorSet, 1, &descriptorSets[1], 0, nullptr);
        const VkDescriptorSet heapSet = heap->Set();
        bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Layout(), ShaderRecompiler::RuntimeAbi::DirectHeapSet, 1, &heapSet, 0, nullptr);
        if (meshWindow) Graphics::BindMeshWindow(context, commands, pipeline->Layout());
        pipeline->PushConstants(commands, pushStages, block);

        if (!vertexBuffers.empty()) context.Resolved(&Graphics::DeviceFunctions::cmdBindVertexBuffers, "vkCmdBindVertexBuffers")(commands, 0, static_cast<std::uint32_t>(vertexBuffers.size()), vertexBuffers.data(), vertexOffsets.data());
        if (indexInput) context.Resolved(&Graphics::DeviceFunctions::cmdBindIndexBuffer, "vkCmdBindIndexBuffer")(commands, indexInput->buffer, indexInput->offset, parameters.indexSize == 2u ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        noteFaultToken(context, *recorder, commands, draw.stages, FaultStage::graphics, parameters.indirect ? "indirect" : (parameters.indexed ? "indexed" : "draw"), parameters.indexCount, parameters.instanceCount);
        std::uint64_t timedProgram = 0;
        for (const auto& one : draw.stages) {
            if (timedProgram == 0 || one.program.stage == ShaderRecompiler::ShaderStage::Fragment) timedProgram = one.program.codeAddress;
        }
        if (countsSamples()) recorder->NoteSampledDraw(commands);
        const auto drawTiming = recorder->BeginInPassGpuTiming(DrawTimingKey | (std::uint64_t{0x7ec} << 48u) | (timedProgram & 0xffffffffffffull));
        if (meshGrid) {
            if (Downgrades::Active() && meshTraceLines.fetch_add(1, std::memory_order_relaxed) < 40u) {
                std::string stagesText;
                for (std::size_t index = 0; index < draw.stages.size(); ++index) {
                    const auto& stage = draw.stages[index];
                    char text[200];
                    std::snprintf(text, sizeof(text), " [%016llx stage %d push %u+%zu attributes %zu known %zu window %d]", static_cast<unsigned long long>(Downgrades::ProgramHash(stage.program.codeAddress)), static_cast<int>(stage.program.stage), shaders[index].pushConstantOffset, stage.program.result->pushConstants.size(), stage.program.vertexAttributes.size(), stage.program.variant != nullptr ? stage.program.variant->info.info.knownBuffers.size() : 0u, stage.program.result->meshWindowBuffer ? 1 : 0);
                    stagesText += text;
                }
                std::size_t meshBindings = 0;
                for (const auto& binding : draw.layout.bindings) meshBindings += binding.stage == 0u ? 1u : 0u;
                std::fprintf(stderr, "[recode-mesh] groups %u layers %u stride %u instances %u indices %u size %u indexed %d at 0x%llx first vertex %u input %u per group %u; bindings %zu (%zu mesh), uses %zu; push stages 0x%x;%s\n", meshGrid->groups, meshGrid->layers, meshGrid->layerStride, parameters.instanceCount, parameters.indexCount, parameters.indexSize, parameters.indexed ? 1 : 0, static_cast<unsigned long long>(parameters.indexAddress), parameters.firstVertex, state.stages.mesh->inputPrimitive, state.stages.mesh->primitivesPerGroup, draw.layout.bindings.size(), meshBindings, draw.uses.size(), static_cast<unsigned>(pushStages), stagesText.c_str());
            }
            if (!meshDrawsSkipped()) context.Function<PFN_vkCmdDrawMeshTasksEXT>("vkCmdDrawMeshTasksEXT")(commands, meshGrid->groups, parameters.instanceCount, meshGrid->layers);
        } else if (parameters.indirect) {
            const auto& args = *parameters.indirect;
            if (indirectCount) {
                if (parameters.indexed) context.Function<PFN_vkCmdDrawIndexedIndirectCountKHR>("vkCmdDrawIndexedIndirectCountKHR")(commands, argumentBuffer, argumentOffset, indirectCount->buffer, indirectCount->offset, args.count, args.stride);
                else context.Function<PFN_vkCmdDrawIndirectCountKHR>("vkCmdDrawIndirectCountKHR")(commands, argumentBuffer, argumentOffset, indirectCount->buffer, indirectCount->offset, args.count, args.stride);
            } else if (args.count <= 1u || context.multiDrawIndirect) {
                if (parameters.indexed) context.Resolved(&Graphics::DeviceFunctions::cmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect")(commands, argumentBuffer, argumentOffset, args.count, args.stride);
                else context.Resolved(&Graphics::DeviceFunctions::cmdDrawIndirect, "vkCmdDrawIndirect")(commands, argumentBuffer, argumentOffset, args.count, args.stride);
            } else {
                for (std::uint32_t record = 0; record < args.count; ++record) {
                    const auto offset = argumentOffset + static_cast<VkDeviceSize>(record) * args.stride;
                    if (parameters.indexed) context.Resolved(&Graphics::DeviceFunctions::cmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect")(commands, argumentBuffer, offset, 1, args.stride);
                    else context.Resolved(&Graphics::DeviceFunctions::cmdDrawIndirect, "vkCmdDrawIndirect")(commands, argumentBuffer, offset, 1, args.stride);
                }
            }
        } else if (parameters.indexed) {
            context.Resolved(&Graphics::DeviceFunctions::cmdDrawIndexed, "vkCmdDrawIndexed")(commands, parameters.indexCount, parameters.instanceCount, 0, static_cast<std::int32_t>(parameters.firstVertex), parameters.firstInstance);
        } else {
            context.Resolved(&Graphics::DeviceFunctions::cmdDraw, "vkCmdDraw")(commands, parameters.indexCount, parameters.instanceCount, parameters.firstVertex, parameters.firstInstance);
        }
        recorder->EndGpuTiming(drawTiming);
        split(8);

        recorder->LeaveRenderPassOpen(key, 0, fenced, access);
        recorder->Keep(pipeline);
        recorder->Keep(framebuffer);
        for (const auto& stage : draw.stages) {
            for (const auto& owner : stage.owners) recorder->Keep(std::const_pointer_cast<void>(owner));
            if (stage.tableMap != nullptr) recorder->Keep(std::const_pointer_cast<Tables::Mirror>(stage.tableMap));
        }
        if (pageTable != nullptr) recorder->Keep(pageTable);
        if (!targetRecords.empty()) {
            auto* surfaceTable = Resolve::Surfaces();
            for (std::size_t index = 0; index < targetRecords.size(); ++index) {
                const auto& record = targetRecords[index];
                const auto owner = Images::OwnerOf(*record);
                static_cast<void>(Events::GpuWrites().Note(record->key.address, record->key.bytes, owner));
                static_cast<void>(Events::Shared().Note(record->key.address, record->key.bytes, owner));
                if (surfaceTable != nullptr) markRecordWritten(*recorder, *surfaceTable, *record);
                if (nanCensus().draws != 0u && censusReads().empty()) censusRecord(context, *recorder, record, draw.stages.empty() ? std::uint64_t{0} : Downgrades::ProgramHash(draw.stages.back().program.codeAddress));
                if (dumpsRecords()) maybeDumpRecord(context, *recorder, record, draw.stages.empty() ? std::uint64_t{0} : Downgrades::ProgramHash(draw.stages.back().program.codeAddress), index < state.colors.size() ? state.colors[index].mip : 0u);
                recorder->Keep(record);
            }
        }
        std::vector<bool> storeNow(residents.size(), false);
        for (std::size_t index = 0; index < residents.size(); ++index) {
            auto& resident = residents[index];
            if (!resident->Cached()) recorder->KeepBytes(resident.get(), static_cast<std::size_t>(resident->BudgetBytes()));
            if (resident->Descriptor().tileMode == Graphics::TextureTileMode::kLinear) storeNow[index] = !resident->MarkDrawn(state.colors[index].arraySlice, 1u, state.scissor) || !resident->Cached();
            else resident->MarkSliceDirty(state.colors[index].arraySlice, 1u);
            recorder->Keep(resident);
            if (dumpsRecords()) maybeDumpUpstream(context, *recorder, resident, draw.stages.empty() ? std::uint64_t{0} : Downgrades::ProgramHash(draw.stages.back().program.codeAddress), state.colors[index].mip);
        }
        for (std::size_t index = 0; index < residents.size(); ++index) {
            if (storeNow[index]) Graphics::StorageTexture::FlushPending(residents[index]->Descriptor().baseAddress, static_cast<std::size_t>(residents[index]->GuestBytes()), nullptr, "linear target");
        }
        totals.linearStores += static_cast<std::uint64_t>(std::ranges::count(storeNow, true));
        if (memoizesTargets()) rememberTargets(*recorder, state, views, residents);
        return true;
    }

    std::shared_ptr<Images::Surface> targetRecord(const Context& context, const Graphics::ColorTarget& color, VkImageView& view) {
        const auto refuse = [&](const std::string& why) {
            lastTargetRefusal = why;
            totals.recordTargetRefusals++;
            if (recordTargetReasons.size() < 24u || recordTargetReasons.contains(why)) {
                if (recordTargetReasons[why]++ == 0u) std::fprintf(stderr, "[recode-record] first target record refusal: %s\n", why.c_str());
            }
            return std::shared_ptr<Images::Surface>{};
        };
        auto* table = Resolve::Surfaces();
        if (table == nullptr) return refuse("no surface table");
        if (ownsImages()) {
            if (color.samples != 1u) return refuse("multisampled target");
            if ((color.dccAddress != 0u || color.cmaskAddress != 0u) && !Images::MetadataRecords()) return refuse("target with DCC or CMASK metadata");
            if (color.staged) return refuse("staged target");
            if (color.depthSlice != 0u) return refuse("target slice of a volume");
        } else if (color.samples != 1u || color.mipTail || color.dccAddress != 0u || color.cmaskAddress != 0u || color.staged || color.depthSlice != 0u) {
            return refuse("target needs metadata, samples, a mip tail or a slice");
        }
        if (ownsImages()) {
            if (!ownsTargets()) return refuse("target records are off");
            std::string ownedWhy;
            const auto ownedResource = Graphics::SurfaceForTarget(color);
            const auto ownedKey = Images::KeyOf(ownedResource, ownedWhy);
            if (!ownedKey.has_value()) return refuse("target key: " + ownedWhy);
            if ((ownedKey->surface.mips > 1u || color.mipTail) && (!recordsMipChains() || ownedKey->surface.width < mipChainMinWidth())) return refuse("target with a mip chain");
            std::uint32_t containingLayer = 0;
            Absorbed absorbed;
            auto owned = ownedRecord(context, *table, ownedKey->surface, ownedWhy, &containingLayer, &absorbed);
            if (owned == nullptr && ownedWhy.empty()) {
                auto adopted = table->CreateResource(ownedKey->surface, ownedResource, ownedWhy);
                poisonRecord(context, adopted.get());
                if (adopted != nullptr && (ownedKey->surface.mips > 1u || zeroesAllNewTargets()) && zeroesNewChains() && context.recorder != nullptr) zeroRecord(context, *context.recorder, *table, *adopted);
                owned = table->Seed(adopted);
                absorbLayers(context, *table, owned, absorbed);
            }
            if (owned == nullptr) return refuse("target surface: " + ownedWhy);
            applyMetadataClear(context, owned, recordingProgram);
            auto* ownedVulkan = dynamic_cast<Images::VulkanSurface*>(owned->device.get());
            if (ownedVulkan == nullptr) return refuse("target surface without a Vulkan image");
            view = ownedVulkan->AttachmentView(color.format, color.mip, containingLayer + ownedResource.baseArray + color.arraySlice, std::max(color.layers, 1u), ownedWhy);
            if (view == VK_NULL_HANDLE) return refuse(ownedWhy);
            return owned;
        }
        std::string why;
        const auto resource = Graphics::SurfaceForTarget(color);
        const auto key = Images::KeyOf(resource, why);
        if (!key.has_value()) return refuse("target key: " + why);
        const auto found = table->FindResource(key->surface, resource, why);
        if (!found.has_value()) return refuse("target surface: " + why);
        auto record = found->surface;
        if (record == nullptr) return refuse("target written by another owner");
        auto* vulkan = dynamic_cast<Images::VulkanSurface*>(record->device.get());
        if (vulkan == nullptr) return refuse("target surface without a Vulkan image");
        view = vulkan->AttachmentView(color.format, color.mip, resource.baseArray + color.arraySlice, std::max(color.layers, 1u), why);
        if (view == VK_NULL_HANDLE) return refuse(why);
        return record;
    }

    static bool layered(std::uint32_t dimension) {
        return dimension == static_cast<std::uint32_t>(Graphics::TextureDimension::k2D) || dimension == static_cast<std::uint32_t>(Graphics::TextureDimension::k2DArray) || dimension == static_cast<std::uint32_t>(Graphics::TextureDimension::kCube);
    }

    static std::shared_ptr<Images::Surface> layersOfRecord(std::span<const std::shared_ptr<Images::Surface>> overlapping, const Images::SurfaceKey& key, std::uint32_t& layer) {
        if (!layered(key.dimension) || key.bytes == 0u) return nullptr;
        for (const auto& record : overlapping) {
            if (!record->seeded || record->retired) continue;
            const auto& held = record->key;
            if (!layered(held.dimension) || held.format != key.format || held.tileMode != key.tileMode || held.width != key.width || held.height != key.height || held.mips != key.mips || held.pitch != key.pitch) continue;
            auto* vulkan = dynamic_cast<Images::VulkanSurface*>(record->device.get());
            if (vulkan == nullptr) continue;
            const auto layers = std::max(vulkan->Geometry().imageLayers, 1u);
            if (layers < 2u || held.bytes % layers != 0u) continue;
            const auto stride = held.bytes / layers;
            if (key.address < held.address || (key.address - held.address) % stride != 0u) continue;
            const auto first = (key.address - held.address) / stride;
            const auto asked = (key.bytes + stride - 1u) / stride;
            if (first + asked > layers) continue;
            layer = static_cast<std::uint32_t>(first);
            return record;
        }
        return nullptr;
    }

    using Absorbed = std::vector<std::pair<std::shared_ptr<Images::Surface>, std::uint32_t>>;

    static bool layerRunIn(const Images::Surface& record, const Images::SurfaceKey& key, std::uint32_t& first) {
        const auto& held = record.key;
        if (!layered(key.dimension) || !layered(held.dimension) || held.format != key.format || held.tileMode != key.tileMode || held.width != key.width || held.height != key.height || held.mips != key.mips || held.pitch != key.pitch) return false;
        auto* vulkan = dynamic_cast<Images::VulkanSurface*>(record.device.get());
        if (vulkan == nullptr) return false;
        const auto layers = std::max(vulkan->Geometry().imageLayers, 1u);
        if (held.bytes % layers != 0u || held.address < key.address || held.address + held.bytes > key.address + key.bytes) return false;
        const auto stride = held.bytes / layers;
        if ((held.address - key.address) % stride != 0u) return false;
        first = static_cast<std::uint32_t>((held.address - key.address) / stride);
        return true;
    }

    void absorbLayers(const Context& context, Images::SurfaceTable& table, const std::shared_ptr<Images::Surface>& record, const Absorbed& absorbed) {
        if (absorbed.empty() || record == nullptr) return;
        auto* target = dynamic_cast<Images::VulkanSurface*>(record->device.get());
        bool copied = false;
        if (target != nullptr && context.recorder != nullptr) {
            drainSurfaces(context, *context.recorder);
            const auto targetLevels = static_cast<std::uint32_t>(std::max<std::size_t>(target->Geometry().mips.size(), 1u));
            const auto targetLayers = std::max(target->Geometry().imageLayers, 1u);
            for (const auto& [held, first] : absorbed) {
                auto* source = dynamic_cast<Images::VulkanSurface*>(held->device.get());
                if (!held->gpuNewer || source == nullptr) continue;
                const auto layers = std::max(source->Geometry().imageLayers, 1u);
                if (first + layers > targetLayers) continue;
                const auto commands = context.recorder->Commands();
                source->EnsureGeneral(commands);
                target->EnsureGeneral(commands);
                const auto levels = std::min(targetLevels, static_cast<std::uint32_t>(std::max<std::size_t>(source->Geometry().mips.size(), 1u)));
                Images::CopyImageContent(context, commands, source->Image(), target->Image(), record->key.width, record->key.height, levels, layers, first);
                context.recorder->Keep(held);
                if (table.ChangedSinceWrite(*held)) totals.layersAbsorbedOverChanges++;
                totals.layersAbsorbed++;
                copied = true;
            }
        }
        for (const auto& [held, first] : absorbed) table.Retire(held);
        if (copied) markRecordWritten(*context.recorder, table, *record);
    }

    std::shared_ptr<Images::Surface> ownedRecord(const Context& context, Images::SurfaceTable& table, const Images::SurfaceKey& key, std::string& why, std::uint32_t* layer = nullptr, Absorbed* absorbed = nullptr) {
        const auto overlapping = table.Overlapping(key.address, key.bytes);
        for (const auto& record : overlapping) {
            if (!record->seeded || record->retired || !Images::SameBacking(record->key, key)) continue;
            table.Touch(*record);
            if (refreshesRecords() && table.RefreshIfChanged(record)) {
                totals.recordsRefreshed++;
                if (context.recorder != nullptr) drainSurfaces(context, *context.recorder);
            }
            return record;
        }
        if (layer != nullptr) {
            if (auto containing = layersOfRecord(overlapping, key, *layer)) {
                table.Touch(*containing);
                totals.layerTargets++;
                return containing;
            }
        }
        for (const auto& record : overlapping) {
            if (!record->seeded || record->retired) continue;
            std::uint32_t first = 0;
            if (absorbed != nullptr && layerRunIn(*record, key, first)) {
                absorbed->emplace_back(record, first);
                continue;
            }
            if (!record->gpuNewer && Graphics::IsBlockCompressed(key.format) && Graphics::IsBlockCompressed(record->key.format)) {
                totals.compressedAliases++;
                continue;
            }
            if (Downgrades::Active() && firstShapePair(key, record->key)) {
                const auto& held = record->key;
                std::fprintf(stderr, "[recode-images] another shape: asked 0x%llx+0x%llx %ux%u depth %u mips %u format %u tile %u dimension %u pitch %u xor %u; record 0x%llx+0x%llx %ux%u depth %u mips %u format %u tile %u dimension %u pitch %u xor %u layers %u%s\n", static_cast<unsigned long long>(key.address), static_cast<unsigned long long>(key.bytes), key.width, key.height, key.depth, key.mips, key.format, key.tileMode, key.dimension, key.pitch, key.pipeBankXor, static_cast<unsigned long long>(held.address), static_cast<unsigned long long>(held.bytes), held.width, held.height, held.depth, held.mips, held.format, held.tileMode, held.dimension, held.pitch, held.pipeBankXor, record->device != nullptr && dynamic_cast<Images::VulkanSurface*>(record->device.get()) != nullptr ? dynamic_cast<Images::VulkanSurface*>(record->device.get())->Geometry().imageLayers : 0u, record->gpuNewer ? " (GPU-written: written back)" : "");
            }
            if (Downgrades::Active()) {
                char pair[256];
                std::snprintf(pair, sizeof(pair), "%ux%ux%u mips %u format %u dim %u by %016llx retired %ux%ux%u mips %u format %u dim %u", key.width, key.height, key.depth, key.mips, key.format, key.dimension, static_cast<unsigned long long>(Downgrades::ProgramHash(recordingProgram)), record->key.width, record->key.height, record->key.depth, record->key.mips, record->key.format, record->key.dimension);
                totals.shapeHandOvers[pair]++;
            }
            if (!handOverShape(context, table, record, why)) return nullptr;
        }
        return nullptr;
    }

    bool handOverShape(const Context& context, Images::SurfaceTable& table, const std::shared_ptr<Images::Surface>& record, std::string& why) {
        applyMetadataClear(context, record, recordingProgram);
        if (record->gpuNewer && table.ChangedSinceWrite(*record)) {
            if (Downgrades::Active() && totals.staleShapes < 16u) {
                const auto& held = record->key;
                std::fprintf(stderr, "[recode-images] not written back: record 0x%llx+0x%llx %ux%u depth %u mips %u format %u dimension %u; %s\n", static_cast<unsigned long long>(held.address), static_cast<unsigned long long>(held.bytes), held.width, held.height, held.depth, held.mips, held.format, held.dimension, table.ChangeOf(*record).c_str());
            }
            totals.staleShapes++;
            if (Downgrades::Watching()) Downgrades::WatchTouch(("a GPU-written record dropped at a hand-over, " + table.ChangeOf(*record)).c_str(), record->key.address, record->key.bytes);
        } else if (record->gpuNewer) {
            std::string reason = "no recorder";
            if (context.recorder != nullptr) {
                if (surfaceDetiler == nullptr) surfaceDetiler = std::make_unique<Images::SurfaceDetiler>(context);
                if (surfaceDetiler->WriteBack(*context.recorder, record, reason)) reason.clear();
            }
            if (!reason.empty()) {
                totals.writeBacksUnserved++;
                if (seen.insert("unwritten hand-over: " + reason).second) std::fprintf(stderr, "[recode-images] first record retired unwritten at a hand-over: %s\n", reason.c_str());
                if (Downgrades::Watching()) Downgrades::WatchTouch(("a GPU-written record retired unwritten at a hand-over, " + reason).c_str(), record->key.address, record->key.bytes);
            } else {
                noteWrittenBack(*context.recorder, record->key.address, record->key.bytes);
                totals.writeBacks++;
                totals.writeBackBytes += record->key.bytes;
            }
        }
        table.Retire(record);
        totals.shapesHandedOver++;
        return true;
    }

    void noteWrittenBack(Graphics::Recorder& recorder, std::uint64_t address, std::uint64_t bytes) {
        if (!Mirror::WritesEnabled()) return;
        if (notesRecordRanges()) {
            UploadClash::NoteTracked(address, bytes);
            const auto mark = MemoryCore::Get().NoteGpuWrite(address, bytes, recorder.OpenBatchSerial());
            if (!mark.upload.empty()) ResidencyService::Get().NoteDirty(mark.upload);
        }
        Downgrades::WatchTouch("a record written back", address, bytes);
        const std::array written{std::pair{address, address + bytes}};
        Events::NoteRanges(written);
    }

    static std::string& lastRefusedImage() {
        thread_local std::string why;
        return why;
    }

    static void noteRefusedImage(std::string_view why) {
        lastRefusedImage() = why;
        if (auto* table = Resolve::Surfaces(); table != nullptr && ownsImages()) table->NoteRefused(why);
    }

    std::shared_ptr<Images::Surface> ownedStorage(const Context& context, const Heap::SlotRequest& request, std::uint32_t& slot, std::uint64_t program) {
        auto* table = Resolve::Surfaces();
        if (table == nullptr) return nullptr;
        std::string why;
        const auto key = Images::KeyOf(request.words, why);
        if (!key.has_value()) {
            noteRefusedImage("storage: " + why);
            return nullptr;
        }
        const auto& surface = key->surface;
        if (!ownsStorage()) {
            noteRefusedImage("storage records are off");
            return nullptr;
        }
        if (!Resolve::StorageRecordShape(request)) {
            noteRefusedImage("storage with a typed view, atomics or another shape");
            return nullptr;
        }
        std::uint32_t layer = 0;
        Absorbed absorbed;
        auto record = ownedRecord(context, *table, surface, why, &layer, &absorbed);
        if (record == nullptr && why.empty()) {
            layer = 0;
            record = table->Create(surface, request.words, why);
            poisonRecord(context, record.get());
            record = table->Seed(record);
            if (record != nullptr) {
                if (seedsStorageRecords() && context.recorder != nullptr) {
                    drainSurfaces(context, *context.recorder);
                    totals.storageRecordsSeeded++;
                }
                absorbLayers(context, *table, record, absorbed);
            }
        }
        applyMetadataClear(context, record, program);
        if (record != nullptr) record = Resolve::BindRecordSlot(request, std::move(record), slot, layer);
        if (record == nullptr) {
            noteRefusedImage("storage: " + (why.empty() ? std::string("no view") : why));
            return nullptr;
        }
        return record;
    }

    void noteStaleRecordRead(const Resolve::ResolvedStage& stage, const Heap::DeferredImage& deferred) {
        auto* table = Resolve::Surfaces();
        if (table == nullptr) return;
        try {
            const auto resource = Graphics::DecodeTextureResource(deferred.request.words);
            const auto bytes = Graphics::SurfaceGuestBytes(resource);
            for (const auto& record : table->StaleOverlapping(resource.baseAddress, bytes)) {
                char detail[160];
                const auto& held = record->key;
                std::snprintf(detail, sizeof(detail), "existing cache reads 0x%llx %ux%u fmt %u over record 0x%llx+0x%llx %ux%u fmt %u", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, static_cast<unsigned long long>(held.address), static_cast<unsigned long long>(held.bytes), held.width, held.height, held.format);
                Downgrades::Note(stage.program.codeAddress, Downgrades::Kind::SampledStaleRecord, deferred.index, detail);
                break;
            }
        } catch (const std::exception&) {
        }
    }

    bool bindOwnedSampled(const Context& context, Resolve::ResolvedStage& stage, const Heap::DeferredImage& deferred) {
        std::uint32_t slot = 0;
        if (!ownedSampledSlot(context, stage.program, deferred, slot)) return false;
        stage.slots[deferred.index] = slot;
        return true;
    }

    bool ownedSampledSlot(const Context& context, const Resolve::StageProgram& program, const Heap::DeferredImage& deferred, std::uint32_t& boundSlot, std::shared_ptr<const void>* bound = nullptr, std::shared_ptr<Images::Surface>* boundRecord = nullptr) {
        const auto codeAddress = program.codeAddress;
        auto* table = Resolve::Surfaces();
        if (table == nullptr) return false;
        std::string why;
        const auto key = Images::KeyOf(deferred.request.words, why);
        if (!key.has_value()) {
            noteRefusedImage("sampled: " + why);
            return false;
        }
        const auto& surface = key->surface;
        if (!Resolve::SampledRecordsAll() && !table->Holds(surface.address, surface.bytes)) return false;
        if (Images::DepthRecordsOn()) {
            bool stencilPlane = false;
            if (auto depth = Images::Depths().At(surface.address, stencilPlane)) {
                const auto depthView = depth->SampledView(stencilPlane);
                std::uint32_t depthSlot = 0;
                if (depthView != VK_NULL_HANDLE && Resolve::BindViewSlot(deferred.request, reinterpret_cast<std::uint64_t>(depthView), VK_IMAGE_LAYOUT_GENERAL, depth, depthSlot)) {
                    boundSlot = depthSlot;
                    if (bound != nullptr) *bound = depth;
                    if (boundRecord != nullptr) boundRecord->reset();
                    if (context.recorder != nullptr) context.recorder->Keep(depth);
                    totals.depthRecordsSampled++;
                    return true;
                }
            }
        }
        std::uint32_t layer = 0;
        Absorbed absorbed;
        auto record = ownedRecord(context, *table, surface, why, &layer, &absorbed);
        if (record == nullptr && why.empty()) {
            layer = 0;
            record = table->Create(surface, deferred.request.words, why);
            poisonRecord(context, record.get());
            record = table->Seed(record);
            if (record != nullptr) {
                if (context.recorder != nullptr) drainSurfaces(context, *context.recorder);
                absorbLayers(context, *table, record, absorbed);
                totals.sampledRecordsCreated++;
                if (Downgrades::Active()) {
                    char shape[200];
                    std::snprintf(shape, sizeof(shape), "0x%llx+0x%llx %ux%u depth %u mips %u format %u dimension %u tile %u pitch %u xor %u by %016llx", static_cast<unsigned long long>(surface.address), static_cast<unsigned long long>(surface.bytes), surface.width, surface.height, surface.depth, surface.mips, surface.format, surface.dimension, surface.tileMode, surface.pitch, surface.pipeBankXor, static_cast<unsigned long long>(Downgrades::ProgramHash(codeAddress)));
                    totals.sampledCreations[shape]++;
                }
                MemoryCore::Get().OpenWhole(surface.address, surface.bytes);
                Downgrades::WatchTouch("a record made from memory for a sampled read", surface.address, surface.bytes);
            }
        }
        if (Downgrades::Watching()) Downgrades::WatchProgramRead(codeAddress, surface.address, surface.bytes);
        applyMetadataClear(context, record, codeAddress);
        auto viewKey = key->view;
        viewKey.baseArray += layer;
        viewKey.lastArray += layer;
        if (record != nullptr) noteSampledSource(Downgrades::ProgramHash(codeAddress), record->key.address, record->key.bytes);
        if (record != nullptr && context.recorder != nullptr) maybeDumpSampled(context, *context.recorder, record, Downgrades::ProgramHash(codeAddress));
        if (record != nullptr && sourcesOfProgram() == 0 && !targetTraceList().empty() && tracedProgram(Downgrades::ProgramHash(codeAddress)) && targetTraceLines.fetch_add(1, std::memory_order_relaxed) < (targetTraceList() == "all" ? 20000u : 400u)) {
            std::fprintf(stderr, "[recode-sampled] program %016llx slot %u kind %u 0x%llx %ux%u mips %u view levels %u-%u layers %u-%u format %u -> record 0x%llx mips %u\n", static_cast<unsigned long long>(Downgrades::ProgramHash(codeAddress)), deferred.index, deferred.request.kind, static_cast<unsigned long long>(surface.address), surface.width, surface.height, surface.mips, viewKey.baseLevel, viewKey.lastLevel, viewKey.baseArray, viewKey.lastArray, viewKey.format, static_cast<unsigned long long>(record->key.address), record->key.mips);
        }
        const auto view = record != nullptr ? record->device->View(viewKey, deferred.request.kind, why) : std::uint64_t{0};
        std::uint32_t slot = 0;
        if (view == 0u || !Resolve::BindViewSlot(deferred.request, view, record->device->Layout(), record, slot)) {
            noteRefusedImage("sampled: " + (why.empty() ? std::string("another shape of a recode record") : why));
            return false;
        }
        boundSlot = slot;
        if (bound != nullptr) *bound = record;
        if (boundRecord != nullptr) *boundRecord = record;
        if (context.recorder != nullptr) context.recorder->Keep(record);
        if (context.recorder != nullptr && nanCensus().draws != 0u && censusReadsOf(Downgrades::ProgramHash(codeAddress))) censusRecord(context, *context.recorder, record, Downgrades::ProgramHash(codeAddress), true);
        totals.recordSampled++;
        return true;
    }

    bool targetsFromMemo(const Context& context, const Graphics::Recorder& recorder, const Graphics::State& state, std::vector<VkImageView>& views, std::vector<std::shared_ptr<void>>& owners, std::vector<std::shared_ptr<Graphics::StorageTexture>>& residents) {
        const auto& memo = targetMemo;
        if (recorder.Id() != memo.recorder || !recorder.RenderPassOpen() || recorder.RenderPassGeneration() != memo.generation) return false;
        if (GuestMemory::CollectEpoch() != memo.epoch || GuestMemory::ProofSerial() != memo.proof || Graphics::StorageTexture::PendingSerial() != memo.pending) return false;
        if (state.colors.size() != memo.colors.size() || !std::equal(state.colors.begin(), state.colors.end(), memo.colors.begin())) return false;
        if (state.depth.has_value() != memo.depth.has_value()) return false;
        if (state.depth && (!(*state.depth == *memo.depth) || state.depthWrite != memo.depthWrite || state.stencilTest != memo.stencilTest || writesStencil(state) != memo.stencilWritten || Graphics::DepthSurfacesSerial() != memo.depthSerial || Graphics::DepthSurfacesReleased() != memo.depthReleased)) return false;
        for (std::size_t index = 0; index < memo.residents.size(); ++index) {
            auto resident = memo.residents[index].lock();
            if (resident == nullptr || resident->Version() != memo.versions[index] || !Graphics::StorageImageCached(context, resident.get()) || !Graphics::StorageImageServesKeys(*resident, Graphics::SurfaceForTarget(state.colors[index]).dccAddress)) return false;
            views.push_back(memo.views[index]);
            owners.push_back(resident);
            residents.push_back(std::move(resident));
        }
        if (state.depth) views.push_back(memo.depthView);
        return true;
    }

    void rememberTargets(const Graphics::Recorder& recorder, const Graphics::State& state, std::span<const VkImageView> views, std::span<const std::shared_ptr<Graphics::StorageTexture>> residents) {
        auto& memo = targetMemo;
        const auto epoch = GuestMemory::CollectEpoch();
        const bool eligible = epoch != 0 && recorder.RenderPassOpen() && views.size() == residents.size() + (state.depth ? 1u : 0u) && (!state.depth || state.depth->samples == 1u) && std::ranges::all_of(state.colors, [](const Graphics::ColorTarget& color) { return color.samples == 1u && !color.staged && color.partialWriteMask == 0u; }) && std::ranges::all_of(residents, [](const std::shared_ptr<Graphics::StorageTexture>& resident) { return resident->Cached() && resident->Descriptor().tileMode != Graphics::TextureTileMode::kLinear; });
        if (!eligible) return;
        memo.valid = true;
        memo.recorder = recorder.Id();
        memo.generation = recorder.RenderPassGeneration();
        memo.epoch = epoch;
        memo.proof = GuestMemory::ProofSerial();
        memo.pending = Graphics::StorageTexture::PendingSerial();
        memo.depthSerial = Graphics::DepthSurfacesSerial();
        memo.depthReleased = Graphics::DepthSurfacesReleased();
        memo.colors.assign(state.colors.begin(), state.colors.end());
        memo.residents.assign(residents.begin(), residents.end());
        memo.versions.clear();
        for (const auto& resident : residents) memo.versions.push_back(resident->Version());
        memo.views.assign(views.begin(), views.begin() + static_cast<std::ptrdiff_t>(residents.size()));
        memo.depth = state.depth;
        memo.depthWrite = state.depthWrite;
        memo.stencilTest = state.stencilTest;
        memo.stencilWritten = state.depth ? writesStencil(state) : false;
        memo.depthView = state.depth ? views.back() : VK_NULL_HANDLE;
    }

    std::mutex mutex;
    std::atomic<std::uint64_t> drawPrebuildNanoseconds{0};
    struct LargeKnown {
        std::uint64_t bytes = 0;
        std::uint64_t mapped = 0;
        std::uint64_t removals = 0;
    };
    std::unordered_map<std::uint64_t, LargeKnown> largeKnown;
    VkDevice device = VK_NULL_HANDLE;
    std::uint64_t alignment = 16;
    std::shared_ptr<Graphics::Buffer> ring;
    std::uint64_t ringAddress = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> passTargets;
    TargetMemo targetMemo;
    std::deque<FaultBuffer> faults;
    std::deque<KnownWrite> knownWrites;
    std::vector<CopyBack> copyBacks;
    std::vector<std::shared_ptr<Images::Surface>> writtenRecords;
    std::vector<std::pair<VkImage, bool>> stageStorageImages;
    std::string lastTargetRefusal;
    std::set<std::uint64_t> usesNoted;
    std::map<std::string, std::uint64_t> recordTargetReasons;
    std::map<std::uint64_t, std::vector<std::pair<std::uint64_t, std::uint64_t>>> copiedBack;
    bool copyingBack = false;
    std::unordered_map<std::uint64_t, std::uint32_t> smallWrites;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> directWrites;
    std::unordered_map<std::uint64_t, std::uint32_t> writtenPages;
    std::vector<std::shared_ptr<Graphics::Buffer>> spareFaults;
    std::shared_ptr<Graphics::Buffer> groupReads;
    std::uint32_t nextGroupRead = 0;
    std::shared_ptr<Graphics::Buffer> argumentReads;
    std::uint32_t nextArgumentRead = 0;
    std::unordered_map<std::uint64_t, std::uint32_t> argumentProbes;
    std::map<std::uint32_t, std::uint64_t> faultReasons;
    std::optional<RingSpace> space;
    std::map<std::vector<std::uint32_t>, VkDescriptorSetLayout> layouts;
    std::deque<std::pair<VkDescriptorPool, std::uint64_t>> pools;
    std::vector<VkDescriptorPool> spare;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint32_t> outputs;
    std::map<std::uint64_t, MapBuffer> maps;
    std::uint64_t mapBytes = 0;
    std::unordered_map<std::uint64_t, TableImages> tableImages;
    std::uint64_t tableRetirements = 0;
    std::shared_ptr<Graphics::Buffer> gpuWatchBuffer;
    std::uint32_t nextGpuWatch = 0;
    std::shared_ptr<Graphics::Buffer> inputCompares;
    std::uint32_t nextInputCompare = 0;
    std::uint64_t inputCompareTick = 0;
    std::shared_ptr<Graphics::Buffer> hangArguments;
    std::uint32_t nextHangArgument = 0;
    const std::byte* lastHangArguments = nullptr;
    std::uint64_t lastArgumentsAddress = 0;
    std::array<std::uint32_t, 8> lastArgumentsCpu{};
    bool lastArgumentsGpu = false;
    std::uint64_t nextTableImagesId = std::uint64_t{1} << 62u;
    std::set<std::string> seen;
    std::chrono::steady_clock::time_point dumpClock = processStart();
    std::set<std::uint64_t> dumpedRecords;
    std::set<std::uint64_t> sampledDumps;
    std::uint32_t depthDumps = 0;
    std::uint32_t depthSeen = 0;
    std::uint64_t traceAddress = 0;
    std::uint32_t traceDumps = 0;
    std::uint32_t traceSeen = 0;
    std::chrono::steady_clock::time_point censusClock = std::chrono::steady_clock::now();
    std::uint32_t censusDraws = 0;
    std::set<std::uint32_t> seenFaults;
    std::unique_ptr<Images::SurfaceDetiler> surfaceDetiler;
    std::unique_ptr<Images::MetadataClears> metadataClears;
    std::unique_ptr<Images::HtileClears> htileClears;
    std::uint64_t recordingProgram = 0;
    std::map<std::vector<std::uint64_t>, std::shared_ptr<ComputePipeline>> computePipelines;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> landing;
    std::vector<std::uint64_t> landingSerials;
    std::vector<KnownWrite> pendingDiffs;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t> extents;
    std::set<std::vector<std::uint64_t>> buildingPipelines;
    std::map<std::vector<std::uint64_t>, std::string> failedPipelines;
    std::deque<PipelineJob> pipelineJobs;
    std::set<std::string> pipelineFailuresSeen;
    std::uint64_t builtPipelines = 0;
    std::uint64_t buildNanoseconds = 0;
    std::uint64_t unsavedBuildNanoseconds = 0;
    BuildBudget::Running runningBuilds;
    std::uint32_t framesWaiting = 0;
    std::uint32_t listedJobs = 0;
    std::atomic<bool> listReplayStarted{false};
    std::atomic<bool> listReplaying{false};
    std::atomic<bool> listReplayStops{false};
    std::atomic<std::uint64_t> pipelinesReplayed{0};
    std::uint32_t mostBuildsAtOnce = 0;
    std::mutex pipelineMutex;
    std::mutex layoutMutex;
    std::atomic<VkDevice> attachedDevice{VK_NULL_HANDLE};
    std::atomic<std::uint64_t> pipelinesQueued{0};
    std::atomic<std::uint64_t> pipelinesPrebuilt{0};
    std::condition_variable jobsReady;
    std::condition_variable pipelinesDone;
    std::vector<std::thread> builders;
    VkDescriptorSetLayout emptyLayout = VK_NULL_HANDLE;
    Totals totals;
};

DrawRecorder& shared() {
    static auto* recorder = new DrawRecorder();
    return *recorder;
}

}

bool RecordActive() {
    static const bool enabled = [] {
        const auto* value = RouteValue("APS5_RECODE_DRAW");
        return value != nullptr && std::string_view(value) == "record";
    }();
    return enabled;
}

bool DropsUnrecorded() {
    return RecordActive() && Downgrades::ExistingPathMode() == Downgrades::ExistingPath::Drop;
}

bool RecordsKernels(std::uint32_t queue) {
    static const bool records = std::getenv("APS5_RECODE_NO_KERNEL_RECORD") == nullptr;
    return records && RecordActive() && Resolve::DispatchesRoutedOn(queue);
}

bool LandingsPending() {
    if (batchLandings().pending.load(std::memory_order_acquire) == 0u) return false;
    Releases::NoteLabelBehindLandings();
    return true;
}

bool RecordsHtileClears() {
    return Images::DepthRecordsOn() && Images::HtileClearsOn();
}

void runLanding(const std::function<void()>& landing) {
    const auto started = std::chrono::steady_clock::now();
    try {
        landing();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[recode-landings] a landing after its batch failed: %s\n", error.what());
    }
    StageCosts::Note(StageCosts::Stage::HostCopy, elapsed(started));
    batchLandings().pending.fetch_sub(1, std::memory_order_acq_rel);
}

struct HostCopies {
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable drained;
    std::deque<std::function<void()>> queue;
    bool running = false;
    bool started = false;
    std::uint64_t landings = 0;
    std::uint64_t busyNanoseconds = 0;
    std::size_t deepest = 0;
    std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
};

HostCopies& hostCopies() {
    static auto& copies = *new HostCopies();
    return copies;
}

bool usesHostCopyThread() {
    static const bool uses = [] {
        const char* value = std::getenv("APS5_RECODE_HOST_COPY_THREAD");
        return value != nullptr ? std::string_view(value) != "0" : RecordActive();
    }();
    return uses;
}

void hostCopyLoop() {
    auto& copies = hostCopies();
    std::unique_lock lock(copies.mutex);
    for (;;) {
        copies.wake.wait(lock, [&] { return !copies.queue.empty(); });
        auto landing = std::move(copies.queue.front());
        copies.queue.pop_front();
        copies.running = true;
        copies.deepest = std::max(copies.deepest, copies.queue.size() + 1u);
        lock.unlock();
        const auto started = std::chrono::steady_clock::now();
        runLanding(landing);
        const auto finished = std::chrono::steady_clock::now();
        lock.lock();
        copies.running = false;
        copies.landings++;
        copies.busyNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
        if (finished - copies.reported >= std::chrono::seconds(10)) {
            std::fprintf(stderr, "[recode-host-copy] stage (10 s): %llu landings, busy %.0f ms, deepest queue %zu\n", static_cast<unsigned long long>(copies.landings), static_cast<double>(copies.busyNanoseconds) / 1e6, copies.deepest);
            copies.reported = finished;
            copies.landings = 0;
            copies.busyNanoseconds = 0;
            copies.deepest = 0;
        }
        if (copies.queue.empty()) copies.drained.notify_all();
    }
}

void queueHostCopy(std::function<void()> landing) {
    auto& copies = hostCopies();
    {
        std::lock_guard lock(copies.mutex);
        copies.queue.push_back(std::move(landing));
        if (!copies.started) {
            copies.started = true;
            std::thread(hostCopyLoop).detach();
        }
    }
    copies.wake.notify_one();
}

void AfterLandings(std::function<void()> action) {
    if (!usesHostCopyThread()) {
        action();
        return;
    }
    batchLandings().pending.fetch_add(1, std::memory_order_acq_rel);
    queueHostCopy(std::move(action));
}

void DrainHostCopies() {
    if (!usesHostCopyThread()) return;
    auto& copies = hostCopies();
    std::unique_lock lock(copies.mutex);
    copies.drained.wait(lock, [&] { return copies.queue.empty() && !copies.running; });
}

void LandAfterBatch(Graphics::Recorder& recorder, std::function<void()> landing) {
    auto& landings = batchLandings();
    landings.pending.fetch_add(1, std::memory_order_acq_rel);
    landings.scheduled.fetch_add(1, std::memory_order_relaxed);
    if (usesHostCopyThread()) {
        recorder.OnComplete([landing = std::move(landing)]() mutable { queueHostCopy(std::move(landing)); });
        return;
    }
    recorder.OnComplete([landing = std::move(landing)] { runLanding(landing); });
}

std::optional<DisplaySource> DisplayRecord(std::uint64_t address, std::uint64_t bytes) {
    auto* table = Resolve::Surfaces();
    if (table == nullptr || !RecordActive() || !presentsRecords()) return std::nullopt;
    for (const auto& record : table->Overlapping(address, bytes)) {
        if (!record->gpuNewer || record->retired || record->key.address != address) continue;
        auto* vulkan = dynamic_cast<Images::VulkanSurface*>(record->device.get());
        if (vulkan == nullptr || vulkan->Compressed() || vulkan->Geometry().mips.size() > 1u || vulkan->Geometry().imageLayers > 1u) continue;
        if (flipsPresentedFromRecords.fetch_add(1, std::memory_order_relaxed) < 6u && Downgrades::Active()) std::fprintf(stderr, "[recode-flip] presented from record 0x%llx+0x%llx %ux%u format %u at %.2f s\n", static_cast<unsigned long long>(record->key.address), static_cast<unsigned long long>(record->key.bytes), record->key.width, record->key.height, record->key.format, static_cast<double>(elapsed(processStart())) / 1e9);
        return DisplaySource{vulkan->Image(), vulkan->Format(), vulkan->Resource(), record};
    }
    return std::nullopt;
}

struct SampleLandingSlots {
    static constexpr std::uint64_t SlotBytes = 256;
    static constexpr std::uint64_t Slots = 4096;
    std::shared_ptr<Graphics::Buffer> buffer;
    std::uint64_t next = 0;
    std::mutex mutex;
};

SampleLandingSlots& sampleLandingSlots() {
    static SampleLandingSlots slots;
    return slots;
}

bool DumpSamplesToMirror(const Context& context, Graphics::Recorder& recorder, std::uint64_t address, std::uint64_t bytes, std::uint32_t blocks) {
    const auto span = ResidencyService::Get().MirrorSpan(address, bytes);
    if (!span || span->address == 0u || bytes > SampleLandingSlots::SlotBytes || !context.bufferDeviceAddress) {
        static std::atomic<std::uint32_t> missing{0};
        if (missing.fetch_add(1, std::memory_order_relaxed) < 8u) std::fprintf(stderr, "[recode-fatal] occlusion dump at 0x%llx+0x%llx dropped: no mirror span or landing slot\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
        return false;
    }
    auto& slots = sampleLandingSlots();
    std::shared_ptr<Graphics::Buffer> buffer;
    std::uint64_t slot = 0;
    {
        std::lock_guard lock(slots.mutex);
        if (slots.buffer == nullptr) slots.buffer = std::make_shared<Graphics::Buffer>(context, static_cast<std::size_t>(SampleLandingSlots::SlotBytes * SampleLandingSlots::Slots), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        buffer = slots.buffer;
        slot = slots.next++ % SampleLandingSlots::Slots;
    }
    const auto offset = slot * SampleLandingSlots::SlotBytes;
    if (!recorder.DumpSamples(span->address, address, blocks)) return false;
    if (!recorder.DumpSamples(buffer->DeviceAddress() + offset, address, blocks)) return false;
    recorder.Keep(buffer);
    const std::array written{std::pair{address, address + bytes}};
    RouteMemory::NoteGpuWrites(written);
    Events::NoteRanges(written);
    LandAfterBatch(recorder, [buffer, offset, address, bytes] {
        buffer->Invalidate();
        MemoryCore::Get().LandGpuWrite(address, std::span<const std::byte>(buffer->Bytes().data() + offset, static_cast<std::size_t>(bytes)));
        static const long traced = [] {
            const char* value = std::getenv("APS5_RECODE_TRACE_DUMP_VALUES");
            return value != nullptr ? std::strtol(value, nullptr, 0) : 0L;
        }();
        static std::atomic<long> lines{0};
        if (traced > 0 && elapsed(processStart()) > 60'000'000'000ull && lines.fetch_add(1, std::memory_order_relaxed) < traced) {
            std::string line;
            const auto* values = reinterpret_cast<const std::uint64_t*>(buffer->Bytes().data() + offset);
            for (std::uint64_t db = 0; db < bytes / 16u + 1u && db < 16u; ++db) {
                char text[24];
                std::snprintf(text, sizeof(text), " %llx", static_cast<unsigned long long>(values[db * 2u]));
                line += text;
            }
            std::fprintf(stderr, "[recode-dump-values] 0x%llx per DB:%s\n", static_cast<unsigned long long>(address), line.c_str());
        }
    });
    sampleDumpsLanded.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void DeviceReplaced(const Context& context, Graphics::Recorder& recorder) {
    if (!RecordActive()) return;
    const auto written = shared().WriteBackAll(context, recorder);
    recorder.Sync();
    DrainHostCopies();
    auto& core = MemoryCore::Get();
    std::uint64_t landed = 0;
    for (const auto& piece : core.Written().In(0u, MirrorWindow())) {
        core.FlushGpuWrites(piece.begin, piece.end - piece.begin);
        landed += piece.end - piece.begin;
    }
    shared().Detach(context.device);
    Images::Depths().Clear();
    ResidencyService::Get().Detach();
    std::fprintf(stderr, "[recode-device] replaced: %llu records written back, %llu GPU-written bytes landed\n", static_cast<unsigned long long>(written), static_cast<unsigned long long>(landed));
}

int StoreToMirror(const Context& context, Graphics::Recorder& recorder, std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue) {
    if (recorder.Idle()) {
        storesAtOnce.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    const auto span = ResidencyService::Get().MirrorSpan(address, bytes.size());
    const auto imported = LabelImports::Enabled() ? LabelImports::For(context, address, bytes.size()) : std::nullopt;
    if (imported) {
        if (span && span->handle != 0u) {
            recorder.RecordStore(reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span->handle)), static_cast<VkDeviceSize>(span->offset), bytes, address);
            std::vector<std::byte> before(bytes.size());
            const bool readable = Protector::Readable(address) && Protector::Readable(address + bytes.size() - 1u);
            if (readable) std::memcpy(before.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), bytes.size());
            const auto sequence = readable ? MemoryCore::Get().NoteStoredBytes(address, before, recorder.OpenBatchSerial()) : MemoryCore::Get().NoteWrittenBytes(address, bytes.size(), recorder.OpenBatchSerial());
            LandAfterBatch(recorder, [address, sequence, stored = std::vector<std::byte>(bytes.begin(), bytes.end())] {
                MemoryCore::Get().ReleaseStoredBytes(address, stored.size(), sequence);
                if (!GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), stored.size(), false)) return;
                if (std::memcmp(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), stored.data(), stored.size()) == 0) return;
                const std::array changed{DirtyRange{address, stored.size()}};
                ResidencyService::Get().NoteDirty(changed);
                storesRewrittenByCpu.fetch_add(1, std::memory_order_relaxed);
            });
        }
        recorder.RecordStore(imported->buffer, imported->offset, bytes, address);
        recorder.NoteLabel(address, bytes, stamp, queue);
        storesThroughImports.fetch_add(1, std::memory_order_relaxed);
        static const std::pair<std::uint64_t, std::uint64_t> tracedRange = [] {
            const char* value = std::getenv("APS5_RECODE_TRACE_STORE_RANGE");
            if (value == nullptr) return std::pair<std::uint64_t, std::uint64_t>{0u, 0u};
            const char* colon = std::strchr(value, ':');
            return std::pair<std::uint64_t, std::uint64_t>{std::strtoull(value, nullptr, 16), colon != nullptr ? std::strtoull(colon + 1, nullptr, 16) : ~0ull};
        }();
        if (address >= tracedRange.first && address < tracedRange.second) {
            static std::atomic<std::uint32_t> lines{0};
            if (lines.fetch_add(1, std::memory_order_relaxed) < 20000u) {
                std::uint64_t value = 0;
                std::uint64_t previous = 0;
                std::memcpy(&value, bytes.data(), std::min<std::size_t>(bytes.size(), sizeof(value)));
                if (Protector::Readable(address)) std::memcpy(&previous, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), std::min<std::size_t>(bytes.size(), sizeof(previous)));
                std::fprintf(stderr, "[recode-store] %.3f s q%u 0x%llx+0x%llx value %llx over guest %llx\n", static_cast<double>(elapsed(processStart())) / 1e9, queue, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes.size()), static_cast<unsigned long long>(value), static_cast<unsigned long long>(previous));
            }
        }
        return 0;
    }
    if (!span || span->handle == 0u) {
        storesBesideMirror.fetch_add(1, std::memory_order_relaxed);
        recorder.NoteLabel(address, bytes, stamp, queue);
        LandAfterBatch(recorder, [address, stored = std::vector<std::byte>(bytes.begin(), bytes.end())] { MemoryCore::Get().LandGpuWrite(address, stored); });
        return 0;
    }
    static const char* tracing = std::getenv("APS5_RECODE_TRACE_MIRROR_STORES");
    static const bool traced = tracing != nullptr;
    static const bool everyAddress = traced && std::strcmp(tracing, "all") == 0;
    if (everyAddress) {
        static std::mutex seenMutex;
        static std::set<std::uint64_t> seen;
        std::lock_guard seenLock(seenMutex);
        if (seen.size() < 50000u && seen.insert(address).second) {
            std::uint64_t value = 0;
            std::memcpy(&value, bytes.data(), std::min<std::size_t>(bytes.size(), sizeof(value)));
            std::fprintf(stderr, "[recode-mirror-store] %.2f s q%u 0x%llx+0x%llx first value 0x%llx\n", static_cast<double>(elapsed(processStart())) / 1e9, queue, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes.size()), static_cast<unsigned long long>(value));
        }
    } else if (traced && bytes.size() >= 16u) {
        static std::atomic<std::uint32_t> lines{0};
        if (lines.fetch_add(1, std::memory_order_relaxed) < 300u) {
            std::uint32_t words[4] = {};
            std::memcpy(words, bytes.data(), sizeof(words));
            std::fprintf(stderr, "[recode-mirror-store] %.2f s q%u 0x%llx+0x%llx words %08x %08x %08x %08x\n", static_cast<double>(elapsed(processStart())) / 1e9, queue, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes.size()), words[0], words[1], words[2], words[3]);
        }
    }
    recorder.RecordStore(reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span->handle)), static_cast<VkDeviceSize>(span->offset), bytes, address);
    const std::array written{std::pair{address, address + bytes.size()}};
    static const bool unmarked = std::getenv("APS5_RECODE_MIRROR_STORES_UNMARKED") != nullptr;
    std::uint64_t sequence = 0;
    if (!unmarked) {
        RouteMemory::NoteGpuWrites(written);
        static const bool takesDirty = std::getenv("APS5_RECODE_MIRROR_STORES_KEEP_DIRTY") == nullptr;
        const auto mark = MemoryCore::Get().NoteGpuWrite(address, bytes.size(), recorder.OpenBatchSerial(), takesDirty);
        if (!mark.upload.empty()) ResidencyService::Get().NoteDirty(mark.upload);
        sequence = mark.sequence;
    }
    Events::NoteRanges(written);
    recorder.NoteLabel(address, bytes, stamp, queue);
    LandAfterBatch(recorder, [address, sequence, recorded = std::chrono::steady_clock::now(), stored = std::vector<std::byte>(bytes.begin(), bytes.end())] {
        auto& core = MemoryCore::Get();
        core.LandGpuWrite(address, stored);
        core.ReleaseLandedWrite(address, stored.size(), sequence);
        const auto waited = elapsed(recorded);
        storeLandingNanoseconds.fetch_add(waited, std::memory_order_relaxed);
        storeLandings.fetch_add(1, std::memory_order_relaxed);
        auto seen = storeLandingMaxNanoseconds.load(std::memory_order_relaxed);
        while (waited > seen && !storeLandingMaxNanoseconds.compare_exchange_weak(seen, waited, std::memory_order_relaxed)) {
        }
    });
    storesToMirror.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

std::optional<Pm4::MemoryCopy> MirrorCopyOf(std::span<const std::uint32_t> packet) {
    if (packet.empty() || !StoresToMirror(0, 0)) return std::nullopt;
    const auto opcode = (packet[0] >> 8u) & 0xffu;
    if (opcode == 0x50) {
        const auto copy = Pm4::DecodeMemoryCopy(packet);
        if (!copy || !StoresToMirror(copy->bytes, copy->destination)) return std::nullopt;
        return copy;
    }
    if (opcode != 0x40 || packet.size() < 6 || Pm4::CopyDataToRegister(packet)) return std::nullopt;
    const auto source = Pm4::CopyDataSource(packet);
    const auto destination = Pm4::CopyDataDestination(packet);
    if ((source != 2 && source != 4) || (destination != 2 && destination != 4)) return std::nullopt;
    const Pm4::MemoryCopy copy{packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u), packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u), Pm4::CopyDataBytes(packet)};
    if (!StoresToMirror(copy.bytes, copy.destination)) return std::nullopt;
    return copy;
}

std::shared_ptr<Graphics::Buffer> ReadMirrorAfterBatch(const Context& context, Graphics::Recorder& recorder, std::uint64_t address, std::uint64_t bytes) {
    const auto from = ResidencyService::Get().MirrorSpan(address, bytes);
    if (!from || from->handle == 0u) return nullptr;
    auto landing = std::make_shared<Graphics::Buffer>(context, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto commands = recorder.Commands();
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferCopy region{from->offset, 0, bytes};
    context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(from->handle)), landing->Handle(), 1, &region);
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    recorder.Keep(landing);
    return landing;
}

bool CopyInMirror(const Context& context, Graphics::Recorder& recorder, const Pm4::MemoryCopy& copy) {
    auto& residency = ResidencyService::Get();
    const auto from = residency.MirrorSpan(copy.source, copy.bytes);
    const auto to = residency.MirrorSpan(copy.destination, copy.bytes);
    if (!from || !to || from->handle == 0u || to->handle == 0u) {
        copiesBesideMirror.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (recorder.HasQueuedKeyStores()) recorder.FlushKeyStores();
    if (recorder.HasQueuedStores()) recorder.FlushStores();
    if (RouteMemory::UploadsPending()) {
        std::vector<std::shared_ptr<void>> kept;
        static_cast<void>(RouteMemory::RecordUploads(recorder.Commands(), kept));
        for (auto& object : kept) recorder.Keep(std::move(object));
    }
    auto landing = std::make_shared<Graphics::Buffer>(context, copy.bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto commands = recorder.Commands();
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    const auto record = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    const auto source = reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(from->handle));
    const auto destination = reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(to->handle));
    const VkBufferCopy inMirror{from->offset, to->offset, copy.bytes};
    record(commands, source, destination, 1, &inMirror);
    const VkBufferCopy toLanding{from->offset, 0, copy.bytes};
    record(commands, source, landing->Handle(), 1, &toLanding);
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT);
    recorder.Keep(landing);
    const std::array written{std::pair{copy.destination, copy.destination + copy.bytes}};
    RouteMemory::NoteGpuWrites(written);
    const auto mark = MemoryCore::Get().NoteGpuWrite(copy.destination, copy.bytes, recorder.OpenBatchSerial());
    if (!mark.upload.empty()) ResidencyService::Get().NoteDirty(mark.upload);
    Events::NoteRanges(written);
    LandAfterBatch(recorder, [landing, sequence = mark.sequence, address = copy.destination, bytes = copy.bytes] {
        landing->Invalidate();
        auto& core = MemoryCore::Get();
        core.LandGpuWrite(address, std::span<const std::byte>(landing->Bytes().data(), bytes));
        core.ReleaseLandedWrite(address, bytes, sequence);
    });
    copiesInMirror.fetch_add(1, std::memory_order_relaxed);
    return true;
}

namespace {

struct GdsLanding {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    std::shared_ptr<Graphics::Buffer> buffer;
};

struct GdsLandings {
    std::mutex mutex;
    std::vector<GdsLanding> pending;
};

GdsLandings& gdsLandings() {
    static auto& landings = *new GdsLandings();
    return landings;
}

std::atomic<std::uint64_t> gdsTransfers{0};
std::atomic<std::uint64_t> gdsFills{0};
std::atomic<std::uint64_t> gdsFromLandings{0};
std::atomic<std::uint64_t> gdsFromMirror{0};
std::atomic<std::uint64_t> gdsRefused{0};

}

std::string GdsTransferLine() {
    char text[256];
    std::snprintf(text, sizeof(text), "GDS transfers recorded in stream order %llu (%llu fills, %llu read from a pending GDS copy, %llu read from the mirror), left to the CPU %llu", static_cast<unsigned long long>(gdsTransfers.load(std::memory_order_relaxed)), static_cast<unsigned long long>(gdsFills.load(std::memory_order_relaxed)), static_cast<unsigned long long>(gdsFromLandings.load(std::memory_order_relaxed)), static_cast<unsigned long long>(gdsFromMirror.load(std::memory_order_relaxed)), static_cast<unsigned long long>(gdsRefused.load(std::memory_order_relaxed)));
    return text;
}

bool TransferGds(const Context& context, Graphics::Recorder& recorder, std::span<const std::uint32_t> packet, const GdsTransferHooks& hooks) {
    static const bool enabled = std::getenv("APS5_RECODE_GDS_ON_CPU") == nullptr;
    if (!enabled || packet.size() != 7u || ((packet[0] >> 8u) & 0xffu) != 0x50u || recorder.Idle()) return false;
    const auto sourceSelect = Pm4::DmaSource(packet);
    const auto destinationSelect = Pm4::DmaDestination(packet);
    const std::uint64_t bytes = Pm4::DmaBytes(packet);
    const bool fromGds = sourceSelect == Pm4::DmaSelectGds;
    const bool toGds = destinationSelect == Pm4::DmaSelectGds;
    const auto memory = [](std::uint32_t select) { return select == 0u || select == 3u; };
    const auto inGds = [&](std::uint32_t low, std::uint32_t high) { return high == 0u && low <= Pm4::GdsBytes && bytes <= Pm4::GdsBytes - low; };
    if ((!fromGds && !toGds) || bytes == 0u) return false;
    const auto refuse = [] {
        gdsRefused.fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    if (fromGds && !inGds(packet[2], packet[3])) return refuse();
    if (toGds && !inGds(packet[4], packet[5])) return refuse();
    if (fromGds && !toGds && !memory(destinationSelect)) return refuse();
    if (toGds && !fromGds && !memory(sourceSelect) && sourceSelect != Pm4::DmaSourceData) return refuse();
    const auto gds = Pm4::GdsAddress();
    const auto sourceAddress = fromGds ? gds + packet[2] : packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
    const auto destinationAddress = toGds ? gds + packet[4] : packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u);
    if (fromGds && toGds && sourceAddress < destinationAddress + bytes && destinationAddress < sourceAddress + bytes) return refuse();
    if (toGds && sourceSelect == Pm4::DmaSourceData && (bytes % 4u != 0u || packet[4] % 4u != 0u)) return refuse();

    std::shared_ptr<Graphics::Buffer> landed;
    std::uint64_t landedOffset = 0;
    std::optional<DeviceSpan> mirrored;
    std::vector<std::byte> current;
    if (toGds && memory(sourceSelect)) {
        bool overlapped = false;
        {
            auto& landings = gdsLandings();
            std::lock_guard lock(landings.mutex);
            for (auto entry = landings.pending.rbegin(); entry != landings.pending.rend(); ++entry) {
                if (sourceAddress >= entry->address + entry->bytes || entry->address >= sourceAddress + bytes) continue;
                overlapped = true;
                if (sourceAddress >= entry->address && sourceAddress + bytes <= entry->address + entry->bytes) {
                    landed = entry->buffer;
                    landedOffset = sourceAddress - entry->address;
                }
                break;
            }
        }
        if (overlapped && landed == nullptr) return refuse();
        if (landed == nullptr) {
            if (MemoryCore::Get().GpuWritesPending(sourceAddress, bytes)) {
                const std::array range{DirtyRange{sourceAddress, bytes}};
                if (!MemoryCore::Get().WithoutGpuWritten(range).empty()) return refuse();
                mirrored = ResidencyService::Get().MirrorSpan(sourceAddress, bytes);
                if (!mirrored || mirrored->handle == 0u) return refuse();
            } else {
                if (hooks.busy != nullptr && hooks.busy(sourceAddress, bytes)) return refuse();
                if (bytes > 0x10000u || !GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(sourceAddress)), static_cast<std::size_t>(bytes), false)) return refuse();
                current.resize(static_cast<std::size_t>(bytes));
                std::memcpy(current.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(sourceAddress)), current.size());
            }
        }
    }

    MemoryCore::Get().Protection().NeverArm(gds, Pm4::GdsBytes);
    static_cast<void>(MemoryCore::Get().OpenCleanForImport(gds, Pm4::GdsBytes));
    const auto* import = Graphics::HostImportFor(context, gds, Pm4::GdsBytes);
    if (import == nullptr) return refuse();
    const auto gdsBuffer = import->buffer;
    const auto gdsBase = Graphics::HostImportOffset(*import, gds);

    if (recorder.HasQueuedKeyStores()) recorder.FlushKeyStores();
    if (recorder.HasQueuedStores()) recorder.FlushStores();
    if (mirrored && RouteMemory::UploadsPending()) {
        std::vector<std::shared_ptr<void>> kept;
        static_cast<void>(RouteMemory::RecordUploads(recorder.Commands(), kept));
        for (auto& object : kept) recorder.Keep(std::move(object));
    }
    if (!current.empty()) {
        recorder.RecordStore(gdsBuffer, gdsBase + (destinationAddress - gds), current, destinationAddress);
        recorder.FlushStores();
        const std::array written{std::pair{destinationAddress, destinationAddress + bytes}};
        Events::NoteRanges(written);
        recorder.NotePendingWrite(destinationAddress, static_cast<std::size_t>(bytes));
        gdsTransfers.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const auto commands = recorder.Commands();
    const auto copy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    const auto close = [&] { Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT); };
    if (toGds) {
        const auto destinationOffset = gdsBase + (destinationAddress - gds);
        if (fromGds) {
            const VkBufferCopy region{gdsBase + (sourceAddress - gds), destinationOffset, bytes};
            copy(commands, gdsBuffer, gdsBuffer, 1, &region);
        } else if (sourceSelect == Pm4::DmaSourceData) {
            context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, gdsBuffer, destinationOffset, bytes, packet[2]);
            gdsFills.fetch_add(1, std::memory_order_relaxed);
        } else if (landed != nullptr) {
            const VkBufferCopy region{landedOffset, destinationOffset, bytes};
            copy(commands, landed->Handle(), gdsBuffer, 1, &region);
            recorder.Keep(landed);
            gdsFromLandings.fetch_add(1, std::memory_order_relaxed);
        } else {
            const VkBufferCopy region{mirrored->offset, destinationOffset, bytes};
            copy(commands, reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(mirrored->handle)), gdsBuffer, 1, &region);
            gdsFromMirror.fetch_add(1, std::memory_order_relaxed);
        }
        close();
        const std::array written{std::pair{destinationAddress, destinationAddress + bytes}};
        Events::NoteRanges(written);
        recorder.NotePendingWrite(destinationAddress, static_cast<std::size_t>(bytes));
        gdsTransfers.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    auto landing = std::make_shared<Graphics::Buffer>(context, static_cast<std::size_t>(bytes), VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const VkBufferCopy toLanding{gdsBase + (sourceAddress - gds), 0, bytes};
    copy(commands, gdsBuffer, landing->Handle(), 1, &toLanding);
    const auto to = ResidencyService::Get().MirrorSpan(destinationAddress, bytes);
    const bool inMirror = to && to->handle != 0u;
    if (inMirror) {
        const VkBufferCopy toMirror{gdsBase + (sourceAddress - gds), to->offset, bytes};
        copy(commands, gdsBuffer, reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(to->handle)), 1, &toMirror);
    }
    close();
    recorder.Keep(landing);
    std::uint64_t sequence = 0;
    if (inMirror) {
        const std::array written{std::pair{destinationAddress, destinationAddress + bytes}};
        RouteMemory::NoteGpuWrites(written);
        const auto mark = MemoryCore::Get().NoteGpuWrite(destinationAddress, bytes, recorder.OpenBatchSerial());
        if (!mark.upload.empty()) ResidencyService::Get().NoteDirty(mark.upload);
        sequence = mark.sequence;
        Events::NoteRanges(written);
    }
    {
        auto& landings = gdsLandings();
        std::lock_guard lock(landings.mutex);
        landings.pending.push_back({destinationAddress, bytes, landing});
    }
    if (hooks.pending != nullptr) hooks.pending(destinationAddress, bytes);
    LandAfterBatch(recorder, [landing, inMirror, sequence, address = destinationAddress, bytes, done = hooks.landed] {
        landing->Invalidate();
        auto& core = MemoryCore::Get();
        const std::span<const std::byte> stored(landing->Bytes().data(), static_cast<std::size_t>(bytes));
        if (inMirror) {
            core.LandGpuWrite(address, stored);
            core.ReleaseLandedWrite(address, bytes, sequence);
        } else if (GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), stored.size(), true)) {
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), stored.data(), stored.size());
            RouteMemory::NoteDriverStore(address, stored.size());
        } else {
            GuestMemory::Write(address, stored, 1);
        }
        {
            auto& landings = gdsLandings();
            std::lock_guard lock(landings.mutex);
            std::erase_if(landings.pending, [&](const GdsLanding& entry) { return entry.buffer == landing; });
        }
        if (done != nullptr) done(address, bytes);
    });
    gdsTransfers.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool StoresToMirror(std::size_t bytes, std::uint64_t address) {
    static const bool stores = RecordActive() && std::getenv("APS5_RECODE_MIRROR_STORES") != nullptr;
    static const std::size_t smallest = [] {
        const char* value = std::getenv("APS5_RECODE_MIRROR_STORES_FROM");
        return value != nullptr ? static_cast<std::size_t>(std::strtoull(value, nullptr, 0)) : std::size_t{0};
    }();
    static const std::pair<std::uint64_t, std::uint64_t> range = [] {
        const char* value = std::getenv("APS5_RECODE_MIRROR_STORES_RANGE");
        if (value == nullptr) return std::pair<std::uint64_t, std::uint64_t>{0u, ~0ull};
        const char* colon = std::strchr(value, ':');
        return std::pair<std::uint64_t, std::uint64_t>{std::strtoull(value, nullptr, 16), colon != nullptr ? std::strtoull(colon + 1, nullptr, 16) : ~0ull};
    }();
    static const std::pair<std::uint64_t, std::uint64_t> skipped = [] {
        const char* value = std::getenv("APS5_RECODE_MIRROR_STORES_SKIP");
        if (value == nullptr) return std::pair<std::uint64_t, std::uint64_t>{0u, 0u};
        const char* colon = std::strchr(value, ':');
        return std::pair<std::uint64_t, std::uint64_t>{std::strtoull(value, nullptr, 16), colon != nullptr ? std::strtoull(colon + 1, nullptr, 16) : ~0ull};
    }();
    const bool skip = address != 0u && address >= skipped.first && address < skipped.second;
    return stores && !skip && bytes >= smallest && (address == 0u || (address >= range.first && address < range.second));
}

static void censusMemoryPacket(std::span<const std::uint32_t> packet, std::uint32_t queue, std::uint32_t opcode) {
    static const char* filter = std::getenv("APS5_RECODE_PACKET_CENSUS");
    if (filter == nullptr) return;
    std::uint64_t source = 0;
    std::uint64_t destination = 0;
    std::uint64_t bytes = 0;
    std::uint32_t sourceSelect = 0;
    std::uint32_t destinationSelect = 0;
    if (opcode == 0x40 && packet.size() >= 6) {
        sourceSelect = Pm4::CopyDataSource(packet);
        destinationSelect = Pm4::CopyDataDestination(packet);
        if (Pm4::CopyDataToRegister(packet)) return;
        source = packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
        destination = packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u);
        bytes = Pm4::CopyDataBytes(packet);
    } else if (opcode == 0x37 && packet.size() >= 5) {
        destinationSelect = (packet[1] >> 8u) & 0xfu;
        destination = packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
        bytes = (packet.size() - 4u) * 4u;
    } else if (opcode == 0x50 && packet.size() >= 7) {
        sourceSelect = (packet[1] >> 29u) & 3u;
        destinationSelect = (packet[1] >> 20u) & 3u;
        source = packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
        destination = packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u);
        bytes = packet[6] & 0x3ffffffu;
    } else {
        return;
    }
    if (*filter != '\0') {
        const auto low = std::strtoull(filter, nullptr, 16);
        const char* colon = std::strchr(filter, ':');
        const auto high = colon != nullptr ? std::strtoull(colon + 1, nullptr, 16) : low + 0x100000u;
        const bool touches = (destination < high && destination + bytes > low) || (source != 0 && source < high && source + bytes > low);
        if (!touches) return;
    }
    static std::mutex mutex;
    static std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint64_t, std::uint64_t>, std::uint64_t> seen;
    static std::uint64_t lines = 0;
    const auto key = std::tuple{opcode, sourceSelect, destinationSelect, source >> 8u, destination >> 8u};
    std::lock_guard lock(mutex);
    if (seen[key]++ != 0 || lines >= 400) return;
    ++lines;
    const auto& pages = MemoryCore::Get().Pages();
    const bool sourceGpu = source != 0 && bytes != 0 && pages.GpuWrittenIn(source, bytes);
    const bool destinationGpu = destination != 0 && bytes != 0 && pages.GpuWrittenIn(destination, bytes);
    std::uint64_t sourceValue = 0;
    if (source != 0 && GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(source)), 8, false)) std::memcpy(&sourceValue, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(source)), 8);
    std::fprintf(stderr, "[recode-packet-census] q%u %s src sel %u 0x%llx (gpu-written %d, guest 0x%016llx) dst sel %u 0x%llx+0x%llx (gpu-written %d) word1 0x%08x data0 0x%08x\n", queue, opcode == 0x40 ? "COPY_DATA" : opcode == 0x37 ? "WRITE_DATA" : "DMA_DATA", sourceSelect, static_cast<unsigned long long>(source), sourceGpu ? 1 : 0, static_cast<unsigned long long>(sourceValue), destinationSelect, static_cast<unsigned long long>(destination), static_cast<unsigned long long>(bytes), destinationGpu ? 1 : 0, packet[1], packet.size() > 4 ? packet[4] : 0u);
}

void NoteExistingPacket(std::span<const std::uint32_t> packet, std::uint32_t queue) {
    if (!RecordActive() || packet.empty()) return;
    const auto header = packet[0];
    const auto opcode = (header >> 8u) & 0xffu;
    censusMemoryPacket(packet, queue, opcode);
    if (!Downgrades::Active()) return;
    const auto key = (static_cast<std::uint64_t>(queue) << 32u) | (header & 0x0000ff00u);
    const char* served = nullptr;
    switch (opcode) {
        case 0x37: served = "WRITE_DATA"; break;
        case 0x3c: served = "WAIT_REG_MEM"; break;
        case 0x40: served = "COPY_DATA"; break;
        case 0x46: served = "EVENT_WRITE"; break;
        case 0x49: served = "RELEASE_MEM"; break;
        case 0x50: served = "DMA_DATA"; break;
        case 0x58: served = "ACQUIRE_MEM"; break;
        case 0x93: served = "WAIT_REG_MEM_64"; break;
        default: break;
    }
    if (served != nullptr) Downgrades::NoteHashed(0u, Downgrades::Kind::ExistingPacket, opcode | (queue << 8u), served);
    std::lock_guard lock(existingPacketMutex);
    existingPackets[key]++;
}

void NoteSampleDumpBefore(std::uint64_t address) {
    static const bool traced = std::getenv("APS5_RECODE_TRACE_SAMPLE_DUMPS") != nullptr;
    if (!traced) return;
    static std::atomic<std::uint64_t> seen{0};
    static std::atomic<std::uint64_t> zero{0};
    static std::atomic<std::uint64_t> notReady{0};
    static std::atomic<std::uint64_t> counted{0};
    std::uint64_t value = 0;
    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), sizeof(value), false)) return;
    std::memcpy(&value, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), sizeof(value));
    const auto samples = value & ~(1ull << 63u);
    if ((value >> 63u) == 0u) notReady.fetch_add(1, std::memory_order_relaxed);
    else if (samples == 0u) zero.fetch_add(1, std::memory_order_relaxed);
    else counted.fetch_add(1, std::memory_order_relaxed);
    const auto total = seen.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (total % 2000u == 0u) std::fprintf(stderr, "[recode-sample-values] %llu dumps: block held before the dump: not ready %llu, ready with 0 samples %llu, ready with samples %llu; last 0x%llx = 0x%016llx\n", static_cast<unsigned long long>(total), static_cast<unsigned long long>(notReady.load()), static_cast<unsigned long long>(zero.load()), static_cast<unsigned long long>(counted.load()), static_cast<unsigned long long>(address), static_cast<unsigned long long>(value));
}

bool PresentsExistingResidents() {
    static const bool presents = !RecordActive();
    return presents;
}

void FlipSource(const Graphics::Context& context, std::uint64_t address, std::uint64_t bytes) {
    auto* table = Resolve::Surfaces();
    if (table != nullptr && RecordActive()) {
        const auto overlapping = table->Overlapping(address, bytes);
        const bool newer = std::ranges::any_of(overlapping, [](const std::shared_ptr<Images::Surface>& record) { return record->gpuNewer; });
        auto& counter = overlapping.empty() ? flipsWithoutRecords : newer ? flipsFromNewRecords : flipsFromOldRecords;
        if (counter.fetch_add(1, std::memory_order_relaxed) == 0u && flipNotes.fetch_add(1, std::memory_order_relaxed) < 8u) {
            std::fprintf(stderr, "[recode-flip] first flip of 0x%llx+0x%llx %s", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), overlapping.empty() ? "with no record" : newer ? "from a GPU-written record" : "over a record that is not GPU-written");
            for (const auto& record : overlapping) std::fprintf(stderr, " [0x%llx+0x%llx %ux%u format %u]", static_cast<unsigned long long>(record->key.address), static_cast<unsigned long long>(record->key.bytes), record->key.width, record->key.height, record->key.format);
            std::fputc(10, stderr);
            if (newer) std::fprintf(stderr, "[recode-flip] SEVERE: this flip is not presented from its GPU-written record; the existing presenter shows guest memory\n");
        }
        if (overlapping.empty() && Downgrades::Active() && flipMemoryNotes.fetch_add(1, std::memory_order_relaxed) < 6u) {
            std::array<std::uint32_t, 4> head{};
            std::array<std::uint32_t, 4> middle{};
            const auto* base = reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(address));
            const bool readable = GuestMemory::Accessible(base, sizeof(head), false) && GuestMemory::Accessible(base + bytes / 2u, sizeof(middle), false);
            if (readable) {
                std::memcpy(head.data(), base, sizeof(head));
                std::memcpy(middle.data(), base + bytes / 2u, sizeof(middle));
            }
            auto& pages = MemoryCore::Get().Pages();
            std::fprintf(stderr, "[recode-flip] flip without a record at %.1f s: memory %s head %08x %08x %08x %08x middle %08x %08x %08x %08x; GPU-written %d, GPU-owned pages %zu\n", static_cast<double>(elapsed(processStart())) / 1e9, readable ? "readable" : "unreadable", head[0], head[1], head[2], head[3], middle[0], middle[1], middle[2], middle[3], pages.GpuWrittenIn(address, bytes) ? 1 : 0, pages.GpuOwnedIn(address, bytes).size());
        }
    }
    static_cast<void>(context);
}

void NoteKernel(Kernel kernel) {
    (kernel == Kernel::Fill ? recordedFills : recordedCopies).fetch_add(1, std::memory_order_relaxed);
}

bool IdleStores() {
    static const bool idle = !RecordActive() || std::getenv("APS5_RECODE_IDLE_STORE") != nullptr;
    return idle;
}

bool RecordPrecheck(const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::vector<std::pair<std::uint64_t, std::uint64_t>>& reads, std::string& reason) {
    return shared().Precheck(draw, parameters, reads, reason);
}

bool RecordResolved(const Context& context, const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::string& reason) {
    return shared().Record(context, draw, parameters, reason);
}

void PrebuildDraw(const Context& context, const Resolve::ResolvedDraw& draw) {
    if (!RecordActive()) return;
    shared().PrebuildDraw(context, draw);
}

bool RecordDispatchPrecheck(const Context& context, VulkanDevice& owner, const Resolve::ResolvedDraw& dispatch, std::string& reason) {
    return shared().DispatchPrecheck(context, owner, dispatch, reason);
}

bool RecordDispatch(const Context& context, const Resolve::ResolvedDraw& dispatch, const std::array<std::uint32_t, 3>& groups, std::string& reason) {
    const auto started = std::chrono::steady_clock::now();
    const bool recorded = shared().RecordDispatch(context, dispatch, groups, reason);
    StageCosts::Note(StageCosts::Stage::Record, elapsed(started));
    return recorded;
}

void PrebuildDispatch(const Context& context, VulkanDevice& owner, const Resolve::ResolvedDraw& dispatch) {
    static const bool prebuilds = std::getenv("APS5_RECODE_NO_PIPELINE_PREBUILD") == nullptr;
    if (!prebuilds || !RecordActive()) return;
    shared().Prebuild(context, owner, dispatch);
}

void RefuseUnrecorded(std::string_view work, std::uint64_t codeAddress, std::string_view reason) {
    if (!RecordActive()) return;
    if (!reason.empty()) {
        Downgrades::RefuseExistingPath(codeAddress, work, reason);
        return;
    }
    const auto refusal = Resolve::TakeRefusal();
    if (refusal == "no device" || (refusal.empty() && Resolve::Surfaces() == nullptr)) return;
    Downgrades::RefuseExistingPath(codeAddress, work, refusal.empty() ? std::string("not resolved in time") : "resolve refused it: " + refusal);
}

void ProbeExistingWrites(Graphics::Recorder& recorder, std::uint64_t codeAddress, std::span<const std::pair<std::uint64_t, std::uint64_t>> writes) {
    if (RecordActive() || writes.empty() || !probesOutput(codeAddress) || !outputProbeDue(codeAddress)) return;
    for (const auto& [begin, end] : writes) {
        if (end <= begin || end - begin > OutputProbeLimitBytes) continue;
        recorder.OnComplete([codeAddress, address = begin, size = end - begin] {
            if (!GuestMemory::Accessible(reinterpret_cast<const std::byte*>(address), static_cast<std::size_t>(size), false)) return;
            printOutput(codeAddress, "old-path", address, std::span(reinterpret_cast<const std::byte*>(address), static_cast<std::size_t>(size)));
        });
    }
}

void NoteExistingDispatch(std::uint64_t codeAddress, std::uint64_t arguments, std::uint64_t groups) {
    if (!Downgrades::Active()) return;
    Downgrades::Note(codeAddress, Downgrades::Kind::ExistingPath, 0, "dispatch on the existing path");
    if (arguments == 0u) {
        Downgrades::NoteWork(codeAddress, Downgrades::Work::Dispatch, groups);
        return;
    }
    if (!GuestMemory::Accessible(reinterpret_cast<const std::byte*>(arguments), 3u * sizeof(std::uint32_t), false)) return;
    std::uint32_t counts[3] = {};
    std::memcpy(counts, reinterpret_cast<const void*>(arguments), sizeof(counts));
    Downgrades::NoteWork(codeAddress, Downgrades::Work::IndirectDispatch, 0);
    Downgrades::NoteGroups(codeAddress, static_cast<std::uint64_t>(counts[0]) * counts[1] * counts[2]);
}

std::string RecordLine() {
    return shared().Line();
}

}
