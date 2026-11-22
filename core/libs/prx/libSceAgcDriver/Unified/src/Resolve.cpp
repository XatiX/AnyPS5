#include "prx/libSceAgcDriver/Unified/include/Resolve.hpp"
#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include "prx/libSceAgcDriver/Unified/include/Downgrades.hpp"
#include "prx/libSceAgcDriver/Unified/include/DrawRecorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Sampler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include "prx/libSceAgcDriver/Unified/include/Refill.hpp"
#include "prx/libSceAgcDriver/Unified/include/SlotPatch.hpp"
#include "prx/libSceAgcDriver/Unified/include/TableMirror.hpp"
#include "prx/libSceAgcDriver/Unified/include/TableResolutions.hpp"
#include "prx/libSceAgcDriver/Unified/include/Targets.hpp"
#include "prx/libSceAgcDriver/Unified/include/VulkanImages.hpp"
#include "prx/libSceAgcDriver/Unified/include/VulkanHeap.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <utility>

namespace AgcDriver::Unified::Resolve {

namespace {

constexpr std::uint32_t StorageShapes = 7u;
constexpr std::uint32_t Image2DShape = 2u;
constexpr std::uint32_t Image2DArrayShape = 3u;
constexpr std::uint32_t MultisampleShape = 4u;
constexpr std::uint32_t MultisampleArrayShape = 5u;
constexpr std::uint32_t Image3DShape = 6u;

bool storageShapeRecords() {
    static const bool records = std::getenv("APS5_RECODE_STORAGE_SHAPE_RECORDS") != nullptr;
    return records;
}

bool recordStorageShape(std::uint32_t shape) {
    return shape == Image2DShape || shape == Image2DArrayShape || (shape == Image3DShape && storageShapeRecords());
}

bool recordStorageAtomics(std::uint32_t flags) {
    if ((flags & Heap::RequestFlag::Atomic64) != 0u) return false;
    return (flags & Heap::RequestFlag::Atomic) == 0u || storageShapeRecords();
}

Images::StorageShape storageShapeOf(std::uint32_t shape) {
    return shape == Image3DShape ? Images::StorageShape::Volume : shape == Image2DArrayShape ? Images::StorageShape::Array : Images::StorageShape::Flat;
}

enum class DispatchRoute : std::uint8_t { None, Graphics, Compute, All };

DispatchRoute dispatchRoute() {
    static const DispatchRoute route = [] {
        const auto* value = std::getenv("APS5_RECODE_DISPATCH");
        if (value == nullptr) return DispatchRoute::None;
        const std::string_view text(value);
        if (text == "1") return DispatchRoute::Graphics;
        if (text == "compute") return DispatchRoute::Compute;
        if (text == "all") return DispatchRoute::All;
        return DispatchRoute::None;
    }();
    return route;
}

bool dispatchesRouted() {
    return dispatchRoute() != DispatchRoute::None;
}

bool routesQueue(std::uint32_t queue) {
    const auto route = dispatchRoute();
    if (route == DispatchRoute::All) return true;
    return queue == 0u ? route == DispatchRoute::Graphics : route == DispatchRoute::Compute;
}

bool imagesRouted() {
    static const bool enabled = [] {
        const auto* value = std::getenv("APS5_RECODE_IMAGES");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return enabled;
}

enum class SampledBind : std::uint8_t { Ahead, Record, RecordAll };

}

bool SampledRecordsAll() {
    static const bool all = [] {
        const char* value = std::getenv("APS5_RECODE_SAMPLED_RECORDS_ALL");
        return value != nullptr && std::string_view(value) != "gpu" && std::string_view(value) != "0";
    }();
    return all;
}

bool SampledRecordFor(std::uint64_t address, std::uint64_t bytes) {
    static const bool gpu = [] {
        const char* value = std::getenv("APS5_RECODE_SAMPLED_RECORDS_ALL");
        return value != nullptr && std::string_view(value) == "gpu";
    }();
    return SampledRecordsAll() || (gpu && MemoryCore::Get().Pages().GpuWrittenIn(address, bytes));
}

namespace {

SampledBind sampledBind() {
    static const SampledBind bind = [] {
        const auto* value = std::getenv("APS5_RECODE_SAMPLED_BIND");
        if (value == nullptr) return SampledBind::Record;
        const std::string_view text(value);
        if (text == "ahead") return SampledBind::Ahead;
        if (text == "record-all") return SampledBind::RecordAll;
        return SampledBind::Record;
    }();
    return bind;
}

enum SampledClass : std::size_t { SampledView, SampledCopy, SampledSnapshot, SampledClassCount };

SampledClass sampledClass(const Graphics::Texture& texture) {
    if (texture.CopyKind() != Graphics::Texture::PerUseCopy::None) return SampledCopy;
    return texture.ViewsStorageImage() ? SampledView : SampledSnapshot;
}

VkImageViewType wantedViewType(std::uint32_t kind) {
    constexpr std::array<VkImageViewType, 7> types{VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_3D};
    return types[(kind - ShaderRecompiler::RuntimeAbi::FirstImageBinding) % types.size()];
}

enum SampledChoice : std::size_t { ChoicePlain, ChoiceFirstLayer, ChoiceArray, ChoiceMismatch, ChoiceCount };

bool choosesViews() {
    static const bool chooses = std::getenv("APS5_RECODE_PLAIN_VIEWS") == nullptr;
    return chooses;
}

VkImageView sampledView(const Graphics::Texture& texture, std::uint32_t kind, SampledChoice& choice) {
    const auto wanted = wantedViewType(kind);
    choice = ChoicePlain;
    if (texture.SampledViewRange(false).type == wanted) return texture.View();
    choice = ChoiceMismatch;
    if (!choosesViews()) return texture.View();
    if (wanted == VK_IMAGE_VIEW_TYPE_2D && texture.FirstLayerView() != VK_NULL_HANDLE && texture.SampledViewRange(true).type == wanted) {
        choice = ChoiceFirstLayer;
        return texture.FirstLayerView();
    }
    if (wanted == VK_IMAGE_VIEW_TYPE_2D_ARRAY && texture.HasArrayView()) {
        choice = ChoiceArray;
        return texture.ArrayView();
    }
    return texture.View();
}

void noteViewMismatch(const Graphics::Texture& texture, const Heap::SlotRequest& request) {
    char text[96];
    std::snprintf(text, sizeof(text), "slot wants view type %d, the cache texture has %d", static_cast<int>(wantedViewType(request.kind)), static_cast<int>(texture.SampledViewRange(false).type));
    Downgrades::NoteCurrent(Downgrades::Kind::ViewMismatch, request.kind, text);
}

bool memoizesTables() {
    static const bool enabled = std::getenv("APS5_RECODE_NO_TABLE_MEMO") == nullptr;
    return enabled;
}

struct Totals {
    std::uint64_t draws = 0;
    std::uint64_t resolved = 0;
    std::uint64_t refused = 0;
    std::uint64_t noDevice = 0;
    std::uint64_t stages = 0;
    std::uint64_t slots = 0;
    std::uint64_t programNanoseconds = 0;
    std::uint64_t slotNanoseconds = 0;
    std::uint64_t bufferNanoseconds = 0;
    std::uint64_t requestNanoseconds = 0;
    std::uint64_t fillNanoseconds = 0;
    std::uint64_t tableNanoseconds = 0;
    std::uint64_t mirrorNanoseconds = 0;
    std::uint64_t tableMissNanoseconds = 0;
    std::uint64_t tableMisses = 0;
    std::uint64_t mirrorUnchangedNanoseconds = 0;
    std::uint64_t mirrorInPlaceNanoseconds = 0;
    std::uint64_t layoutNanoseconds = 0;
    std::uint64_t lockBusy = 0;
    std::uint64_t uses = 0;
    std::uint64_t incomplete = 0;
    std::uint64_t indirect = 0;
    std::uint64_t targetUses = 0;
    std::uint64_t bufferUses = 0;
    std::uint64_t imageUses = 0;
    std::uint64_t placeholderDraws = 0;
    std::uint64_t tableStages = 0;
    std::uint64_t tableUses = 0;
    std::uint64_t tableTargetHolds = 0;
    std::uint64_t tablePlaceholders = 0;
    std::uint64_t surfaceRefusals = 0;
    std::uint64_t templateMismatches = 0;
    const char* templateMismatch = nullptr;
    Bindings::SourceCounts sources;
    std::map<std::string, std::uint64_t> reasons;
    std::array<std::uint64_t, PhaseCount> phases{};
};

class Resolver {
public:
    void Attach(const std::shared_ptr<VulkanDevice>& device) {
        if (device == nullptr || device.get() == attachedDevice.load(std::memory_order_acquire)) return;
        std::lock_guard lock(mutex);
        if (device.get() == attached.get()) return;
        attached = device;
        attachedDevice.store(device.get(), std::memory_order_release);
        context = std::make_unique<Graphics::Context>(device->graphicsContext());
        target = HeapTarget(device->Target());
        computeWave32 = HeapTarget(device->ComputeTarget(32u));
        computeWave64 = HeapTarget(device->ComputeTarget(64u));
        fixedSlots = device->GraphicsPipelineLibraries();
        published.store(nullptr, std::memory_order_release);
        mirror.reset();
        heap.reset();
        table.reset();
        memo.reset();
        refill.reset();
        try {
            if (!Heap::VulkanHeapDevice::Supported(*context)) {
                std::fprintf(stderr, "[recode-resolve] descriptor heap unsupported: indexing %d, runtime arrays %d, partially bound %d, sampled update after bind %d\n", context->descriptorIndexing, context->runtimeDescriptorArray, context->partiallyBound, context->sampledImageUpdateAfterBind);
                return;
            }
            heap = std::make_unique<Heap::VulkanHeapDevice>(*context);
            table = std::make_unique<Heap::SlotTable>(*heap, heap->SampledSlotCount(), heap->SamplerSlotCount(), heap->StorageSlotCount());
            memo = std::make_unique<Heap::Memo>(Events::Shared(), Events::Images());
            refill = std::make_unique<Heap::Refill>(*table, *memo, [this](const Heap::SlotRequest& request) { return image(request); }, [this](const Heap::SlotRequest& request) { return sampler(request); });
            if (dispatchesRouted() && storageDispatches()) refill->SetDeferTest([this](const Heap::SlotRequest& request) { return storageWrittenRead(request); });
            mirror = std::make_unique<Tables::TableMirror>(*refill, Events::Shared(), Events::Images());
            if (imagesRouted() && surfaces == nullptr) {
                surfaceDevice = std::make_unique<Images::VulkanSurfaceDevice>(*context);
                surfaces = std::make_unique<Images::SurfaceTable>(*surfaceDevice, Events::Shared(), Events::GpuWrites(), surfaceBudget());
                targetViews = std::make_unique<Images::TargetViews>(*context);
                publishedSurfaces.store(surfaces.get(), std::memory_order_release);
            }
            published.store(heap.get(), std::memory_order_release);
            std::fprintf(stderr, "[recode-resolve] descriptor heap: %u sampled, %u storage, %u sampler slots\n", heap->SampledSlotCount(), heap->StorageSlotCount(), heap->SamplerSlotCount());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[recode-resolve] descriptor heap unavailable: %s\n", error.what());
            mirror.reset();
            heap.reset();
            table.reset();
            memo.reset();
            refill.reset();
        }
    }

    void Consume(const Operation& operation, const DecodeState& state) {
        if (IsDispatch(operation.kind)) {
            if (!routesQueue(operation.queue)) return;
            std::shared_ptr<const ResolvedDraw> dispatch;
            std::shared_ptr<VulkanDevice> device;
            const Graphics::Context* bound = nullptr;
            {
                std::lock_guard lock(mutex);
                dispatch = resolveDispatch(operation, state);
                device = attached;
                bound = context.get();
            }
            if (dispatch != nullptr && device != nullptr && bound != nullptr) Record::PrebuildDispatch(*bound, *device, *dispatch);
            dispatchHandoff.Offer(operation.queue, operation.submission, operation.packetOffset, std::move(dispatch));
            return;
        }
        if (!IsDraw(operation.kind)) return;
        std::shared_ptr<const ResolvedDraw> draw;
        if (operation.resolved) {
            std::lock_guard lock(mutex);
            draw = resolve(operation, state);
        }
        handoff.Offer(operation.queue, operation.submission, operation.packetOffset, std::move(draw));
    }

    void ConsumeFailed(const Operation& operation, const char* what) {
        const std::string reason = std::string("resolve threw: ") + what;
        {
            std::lock_guard lock(refusalMutex);
            lastRefusal = reason;
            if (thrown.insert(reason.substr(0, 80)).second) std::fprintf(stderr, "[recode-resolve] first exception while resolving: %s\n", what);
        }
        if (IsDispatch(operation.kind)) dispatchHandoff.Offer(operation.queue, operation.submission, operation.packetOffset, nullptr);
        else if (IsDraw(operation.kind)) handoff.Offer(operation.queue, operation.submission, operation.packetOffset, nullptr);
    }

    PacketTable<ResolvedDraw>& Table() { return handoff; }
    PacketTable<ResolvedDraw>& DispatchTable() { return dispatchHandoff; }
    const Heap::VulkanHeapDevice* Device() const { return published.load(std::memory_order_acquire); }
    Images::SurfaceTable* SurfaceTableOf() const { return publishedSurfaces.load(std::memory_order_acquire); }

    std::optional<Heap::Span> storageWrittenRead(const Heap::SlotRequest& request) {
        if ((request.flags & Heap::RequestFlag::Storage) != 0u) return std::nullopt;
        {
            std::lock_guard lock(storageWrittenMutex);
            if (storageWritten.empty()) return std::nullopt;
        }
        try {
            const auto resource = Graphics::DecodeTextureResource(request.words);
            const auto bytes = Graphics::SurfaceGuestBytes(resource);
            const auto begin = resource.baseAddress;
            const auto end = begin + bytes;
            std::lock_guard lock(storageWrittenMutex);
            for (const auto& [writtenBegin, writtenEnd] : storageWritten) {
                if (begin < writtenEnd && writtenBegin < end) return Heap::Span{begin, bytes};
            }
        } catch (const std::exception&) {
        }
        return std::nullopt;
    }

    std::shared_ptr<const void> BindDeferredSampled(const Graphics::Context& bound, const Heap::SlotRequest& request, std::uint32_t& slot, std::string& reason) {
        if (table == nullptr) {
            reason = "no slot table";
            return nullptr;
        }
        auto texture = Graphics::CachedSampledTexture(bound, request.words);
        if (texture == nullptr || texture->Released()) {
            reason = "deferred sampled image not cached";
            return nullptr;
        }
        sampledAtRecord[sampledClass(*texture)].fetch_add(1, std::memory_order_relaxed);
        SampledChoice choice = ChoicePlain;
        const auto view = sampledView(*texture, request.kind, choice);
        viewChoices[choice].fetch_add(1, std::memory_order_relaxed);
        if (choice == ChoiceMismatch) noteViewMismatch(*texture, request);
        slot = table->ImageSlot(request.kind, {reinterpret_cast<std::uint64_t>(view), static_cast<std::uint32_t>(texture->Layout()), texture});
        if (slot == Heap::NoSlot) {
            reason = "image heap full for kind " + std::to_string(request.kind);
            return nullptr;
        }
        return texture;
    }

    std::shared_ptr<Graphics::StorageTexture> BindDeferred(const Graphics::Context& bound, const Heap::SlotRequest& request, std::uint32_t& slot, std::string& reason) {
        if (table == nullptr) {
            reason = "no slot table";
            return nullptr;
        }
        try {
            const auto resource = Graphics::DecodeTextureResource(request.words);
            auto [texture, view] = storageView(bound, request, resource, (request.flags & Heap::RequestFlag::Written) != 0u);
            if (texture == nullptr || view == VK_NULL_HANDLE) {
                reason = "written storage image without a view";
                return nullptr;
            }
            slot = table->ImageSlot(request.kind, {reinterpret_cast<std::uint64_t>(view), static_cast<std::uint32_t>(VK_IMAGE_LAYOUT_GENERAL), texture});
            if (slot == Heap::NoSlot) {
                reason = "image heap full for kind " + std::to_string(request.kind);
                return nullptr;
            }
            return texture;
        } catch (const std::exception& error) {
            reason = std::string("written storage image: ") + error.what();
            return nullptr;
        }
    }

    std::shared_ptr<Images::Surface> BindDeferredRecord(const Heap::SlotRequest& request, std::uint32_t& slot) {
        if (table == nullptr) return nullptr;
        auto record = storageRecordFor(request);
        if (record == nullptr) return nullptr;
        return BindRecordSlot(request, std::move(record), slot);
    }

    static bool StorageRecordShape(const Heap::SlotRequest& request) {
        if (!storageRecordsOn() || !recordStorageAtomics(request.flags)) return false;
        return recordStorageShape((request.kind - Heap::LastSampledKind - 1u) % StorageShapes);
    }

    std::shared_ptr<Images::Surface> BindRecordSlot(const Heap::SlotRequest& request, std::shared_ptr<Images::Surface> record, std::uint32_t& slot) {
        if (table == nullptr || record == nullptr) return nullptr;
        auto* vulkan = dynamic_cast<Images::VulkanSurface*>(record->device.get());
        if (vulkan == nullptr) return noteRecordRefusal("storage surface without a Vulkan image");
        std::string why;
        try {
            const auto shape = (request.kind - Heap::LastSampledKind - 1u) % StorageShapes;
            const auto resource = Graphics::DecodeTextureResource(request.words);
            const auto mip = std::min(request.flags >> Heap::RequestFlag::MipShift, std::max(resource.mipCount, 1u) - 1u);
            const auto view = vulkan->ShapedStorageView(mip, storageShapeOf(shape), (request.flags & Heap::RequestFlag::UintView) != 0u, (request.flags & Heap::RequestFlag::Atomic) != 0u, why);
            if (view == VK_NULL_HANDLE) return noteRecordRefusal(why);
            slot = table->ImageSlot(request.kind, {reinterpret_cast<std::uint64_t>(view), static_cast<std::uint32_t>(VK_IMAGE_LAYOUT_GENERAL), record});
            if (slot == Heap::NoSlot) return noteRecordRefusal("image heap full");
        } catch (const std::exception& error) {
            return noteRecordRefusal(std::string("storage record: ") + error.what());
        }
        std::lock_guard lock(recordMutex);
        storageRecords++;
        return record;
    }

private:
    static bool storageRecordsOn() {
        static const bool records = std::getenv("APS5_RECODE_STORAGE_RECORDS") != nullptr || Images::OwnersEnabled();
        return records;
    }

public:
    bool BindViewSlot(const Heap::SlotRequest& request, std::uint64_t view, std::uint32_t layout, std::shared_ptr<const void> owner, std::uint32_t& slot) {
        if (table == nullptr || view == 0u || owner == nullptr) return false;
        slot = table->ImageSlot(request.kind, {view, layout, std::move(owner)});
        return slot != Heap::NoSlot;
    }

private:

    std::shared_ptr<Images::Surface> storageRecordFor(const Heap::SlotRequest& request) {
        if (!storageRecordsOn() || surfaces == nullptr) return nullptr;
        if (!recordStorageAtomics(request.flags)) return noteRecordRefusal("atomic storage");
        if (!recordStorageShape((request.kind - Heap::LastSampledKind - 1u) % StorageShapes)) return noteRecordRefusal("storage shape");
        std::string why;
        const auto key = Images::KeyOf(request.words, why);
        if (!key.has_value()) return noteRecordRefusal("storage key: " + why);
        const auto found = surfaces->Find(key->surface, request.words, why);
        if (!found.has_value()) return noteRecordRefusal("storage surface: " + why);
        if (found->surface == nullptr) return noteRecordRefusal("storage written by another owner");
        return found->surface;
    }

    std::shared_ptr<Images::Surface> noteRecordRefusal(const std::string& why) {
        std::lock_guard lock(recordMutex);
        storageRecordRefusals++;
        if (recordRefusalReasons.size() < 24u || recordRefusalReasons.contains(why)) {
            if (recordRefusalReasons[why]++ == 0u) std::fprintf(stderr, "[recode-resolve] first storage record refusal: %s\n", why.c_str());
        }
        return nullptr;
    }

    std::array<std::atomic<std::uint64_t>, SampledClassCount> sampledAhead{};
    std::array<std::atomic<std::uint64_t>, SampledClassCount> sampledAtRecord{};
    std::array<std::atomic<std::uint64_t>, ChoiceCount> viewChoices{};
    std::mutex refusalMutex;
    std::string lastRefusal;
    std::set<std::string> thrown;
    std::mutex recordMutex;
    std::uint64_t storageRecords = 0;
    std::uint64_t storageRecordRefusals = 0;
    std::map<std::string, std::uint64_t> recordRefusalReasons;

    std::shared_ptr<const ResolvedDraw> resolve(const Operation& operation, const DecodeState& state) {
        totals.draws++;
        const auto* submission = state.CurrentSubmission();
        if (refill == nullptr || submission == nullptr || submission->shaders == nullptr) {
            totals.noDevice++;
            noteRefusal("no device");
            return nullptr;
        }
        const auto started = std::chrono::steady_clock::now();
        std::string reason;
        ProgramSet programs;
        const ProgramInputs inputs{state.Live(), *submission->shaders, *context, target, fixedSlots, &templates, verifyTemplates()};
        const bool built = BuildPrograms(inputs, operation.draw, programs, reason);
        for (std::size_t phase = 0; phase < PhaseCount; ++phase) totals.phases[phase] += programs.nanoseconds[phase];
        if (programs.templateMismatches != 0) {
            totals.templateMismatches += programs.templateMismatches;
            totals.templateMismatch = programs.templateMismatch;
        }
        if (!built) {
            std::vector<GuestUse> targets;
            TargetUses(state.Live(), targets);
            noteTargetWrites(targets);
            refuse(totals, reason);
            totals.programNanoseconds += elapsed(started);
            return nullptr;
        }
        const auto slotsStarted = std::chrono::steady_clock::now();
        totals.programNanoseconds += elapsed(started);
        if (!firstBuilt) {
            firstBuilt = true;
            std::fprintf(stderr, "[recode-resolve] first program set: %zu stages\n", programs.stages.size());
        }
        ResolvedDraw draw{operation, {}, {}, true};
        const auto& facts = operation.draw;
        if (facts.indexed && facts.indexAddress != 0u) draw.uses.push_back({facts.indexAddress, static_cast<std::uint64_t>(facts.indexCount) * facts.indexSize, false});
        if (facts.indirect) {
            totals.indirect++;
            if (facts.indirectAddress != 0u && facts.indirectBytes != 0u) draw.uses.push_back({facts.indirectAddress, facts.indirectBytes, false});
            else draw.complete = false;
            if (facts.countAddress != 0u) draw.uses.push_back({facts.countAddress, sizeof(std::uint32_t), false});
        }
        const auto beforeTargets = draw.uses.size();
        TargetUses(programs.decode.state, draw.uses);
        noteTargetWrites(std::span<const GuestUse>(draw.uses).subspan(beforeTargets));
        totals.targetUses += draw.uses.size() - beforeTargets;
        draw.targetBegin = beforeTargets;
        draw.targetEnd = draw.uses.size();
        draw.state = std::make_shared<const Graphics::State>(programs.decode.state);
        return resolveStages(totals, std::move(draw), programs, beforeTargets, slotsStarted);
    }

    std::shared_ptr<const ResolvedDraw> resolveDispatch(const Operation& operation, const DecodeState& state) {
        dispatchTotals.draws++;
        const auto* submission = state.CurrentSubmission();
        if (refill == nullptr || submission == nullptr || submission->shaders == nullptr) {
            dispatchTotals.noDevice++;
            noteRefusal("no device");
            return nullptr;
        }
        const auto started = std::chrono::steady_clock::now();
        std::string reason;
        ProgramSet programs;
        ProgramInputs inputs{state.Live(), *submission->shaders, *context, target, fixedSlots, &templates, verifyTemplates()};
        inputs.computeWave32 = &computeWave32;
        inputs.computeWave64 = &computeWave64;
        const bool built = BuildComputeProgram(inputs, operation.dispatch, programs, reason);
        for (std::size_t phase = 0; phase < PhaseCount; ++phase) dispatchTotals.phases[phase] += programs.nanoseconds[phase];
        dispatchTotals.programNanoseconds += elapsed(started);
        if (!built) {
            refuse(dispatchTotals, reason);
            return nullptr;
        }
        const auto slotsStarted = std::chrono::steady_clock::now();
        ResolvedDraw dispatch{operation, {}, {}, true};
        const auto& facts = operation.dispatch;
        if (facts.indirect) {
            dispatchTotals.indirect++;
            if (facts.indirectAddress != 0u) dispatch.uses.push_back({facts.indirectAddress, 3u * sizeof(std::uint32_t), false});
            else dispatch.complete = false;
        }
        const auto beforeTargets = dispatch.uses.size();
        return resolveStages(dispatchTotals, std::move(dispatch), programs, beforeTargets, slotsStarted);
    }

    std::shared_ptr<const ResolvedDraw> resolveStages(Totals& counts, ResolvedDraw draw, ProgramSet& programs, std::size_t beforeTargets, std::chrono::steady_clock::time_point slotsStarted) {
        std::string reason;
        const auto beforeBuffers = draw.uses.size();
        const auto placeholdersBefore = refill->Counts().placeholders;
        std::vector<Heap::ImageUse> imageUses;
        std::vector<Heap::ImageUse> tableUses;
        std::uint64_t tablePlaceholdersHere = 0;
        for (auto& stage : programs.stages) {
            const Downgrades::ProgramScope programScope(stage.codeAddress);
            auto partStarted = std::chrono::steady_clock::now();
            bufferUses(*stage.variant, stage.capture->snapshot, draw.uses);
            counts.bufferNanoseconds += elapsed(partStarted);
            partStarted = std::chrono::steady_clock::now();
            std::vector<Heap::SlotRequest> requests;
            if (stage.result->directHeap) {
                Heap::SlotStatistics statistics;
                if (!Heap::BuildSlotRequests(stage.variant->info, stage.capture->snapshot, requests, statistics, reason, storageDispatches() && (&counts == &dispatchTotals || storageDraws()))) {
                    refuse(counts, reason);
                    counts.slotNanoseconds += elapsed(slotsStarted);
                    return nullptr;
                }
            }
            counts.requestNanoseconds += elapsed(partStarted);
            if (Downgrades::Active()) {
                for (std::size_t index = 0; index < requests.size(); ++index) {
                    const auto& request = requests[index];
                    if ((request.flags & Heap::RequestFlag::Sampler) == 0u) continue;
                    if (((request.words[0] >> 20u) & 1u) != 0u) Downgrades::NoteCurrent(Downgrades::Kind::SamplerMode, static_cast<std::uint32_t>(index), "sampler forces sRGB decoding; its texture is bound without an sRGB view");
                    if (((request.words[0] >> 15u) & 1u) != 0u) Downgrades::NoteCurrent(Downgrades::Kind::SamplerMode, static_cast<std::uint32_t>(index), "sampler uses unnormalized coordinates; bound as normalized");
                }
                std::uint32_t bindingIndex = 0;
                for (const auto& binding : stage.result->bindings) {
                    const auto any = [](const ShaderRecompiler::ElementFlags& flags) {
                        for (std::size_t element = 0; element < flags.size(); ++element) {
                            if (flags[element]) return true;
                        }
                        return false;
                    };
                    if (any(binding.imageUnnormalized)) Downgrades::NoteCurrent(Downgrades::Kind::SamplerMode, 0x100u + bindingIndex, any(binding.imageUnnormalizedBaseLevel) ? "image fetched with unnormalized coordinates at its base level; no base-level view is chosen" : "image fetched with unnormalized coordinates");
                    if (any(binding.samplerCoordinatesIgnored)) Downgrades::NoteCurrent(Downgrades::Kind::SamplerMode, 0x200u + bindingIndex, "sampler whose coordinates are ignored; its unnormalized bit is not cleared");
                    if (any(binding.samplerIntegerBorder)) Downgrades::NoteCurrent(Downgrades::Kind::SamplerMode, 0x300u + bindingIndex, "sampler with an integer border colour; bound with a float border");
                    if (any(binding.samplerUnnormalized)) Downgrades::NoteCurrent(Downgrades::Kind::SamplerMode, 0x400u + bindingIndex, "sampler proven unnormalized; bound as normalized");
                    ++bindingIndex;
                }
            }
            partStarted = std::chrono::steady_clock::now();
            ResolvedStage resolved{std::move(stage), {}};
            if (!refill->Fill(requests, resolved.slots, reason, &imageUses, &resolved.owners, &resolved.deferredImages) ||!PatchSlots(*resolved.program.result, resolved.slots, resolved.program.words, reason)) {
                refuse(counts, reason);
                counts.slotNanoseconds += elapsed(slotsStarted);
                return nullptr;
            }
            counts.fillNanoseconds += elapsed(partStarted);
            counts.slots += resolved.slots.size();
            if (std::ranges::any_of(resolved.program.result->bindings, [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ImageTableMap; })) {
                if (!resolved.program.result->directHeap) {
                    refuse(counts, "image tables outside heap mode");
                    counts.slotNanoseconds += elapsed(slotsStarted);
                    return nullptr;
                }
                std::shared_ptr<const ShaderRecompiler::ResolvedImageTables> tables;
                partStarted = std::chrono::steady_clock::now();
                const auto resolutionsBefore = ShaderRecompiler::ResourceMaterializer::TableReuse().resolutions;
                try {
                    const auto& info = resolved.program.variant->info.info;
                    const auto& snapshot = resolved.program.capture->snapshot;
                    const auto memoKey = resolved.program.variant->artifact.variantId;
                    const auto resolveShared = [&] { return ShaderRecompiler::ResourceMaterializer::ResolveImageTablesShared(info, snapshot, memoKey); };
                    tables = memoizesTables() && memoKey != 0u && !snapshot.tables.tables.empty() ? resolutions.Find(info, snapshot, memoKey, resolveShared) : resolveShared();
                } catch (const std::exception& error) {
                    reason = std::string("image tables: ") + error.what();
                }
                const auto tableSpent = elapsed(partStarted);
                counts.tableNanoseconds += tableSpent;
                if (ShaderRecompiler::ResourceMaterializer::TableReuse().resolutions != resolutionsBefore) {
                    counts.tableMisses++;
                    counts.tableMissNanoseconds += tableSpent;
                }
                partStarted = std::chrono::steady_clock::now();
                const auto tablePlaceholders = refill->Counts().placeholders;
                const auto mirrorsBefore = mirror->Counts();
                if (tables != nullptr) resolved.tableMap = mirror->Build(tables, reason, &tableUses);
                const auto mirrorSpent = elapsed(partStarted);
                counts.mirrorNanoseconds += mirrorSpent;
                const auto mirrorsAfter = mirror->Counts();
                if (mirrorsAfter.unchanged != mirrorsBefore.unchanged) counts.mirrorUnchangedNanoseconds += mirrorSpent;
                else if (mirrorsAfter.inPlace != mirrorsBefore.inPlace) counts.mirrorInPlaceNanoseconds += mirrorSpent;
                if (resolved.tableMap == nullptr) {
                    refuse(counts, reason);
                    counts.slotNanoseconds += elapsed(slotsStarted);
                    return nullptr;
                }
                tablePlaceholdersHere += refill->Counts().placeholders - tablePlaceholders;
                counts.tableStages++;
            }
            draw.stages.push_back(std::move(resolved));
        }
        const auto layoutStarted = std::chrono::steady_clock::now();
        draw.imageBegin = draw.uses.size();
        std::vector<std::pair<std::uint64_t, std::uint64_t>> held;
        for (auto index = beforeTargets; index < beforeBuffers; ++index) held.emplace_back(draw.uses[index].address, draw.uses[index].address + draw.uses[index].size);
        for (const auto& use : tableUses) {
            const auto pieces = Tables::Outside({use.memory.address, use.memory.address + use.memory.bytes}, held);
            if (pieces.size() != 1u || pieces.front().second - pieces.front().first != use.memory.bytes) counts.tableTargetHolds++;
            for (const auto& [begin, end] : pieces) {
                draw.uses.push_back({begin, end - begin, use.write});
                counts.tableUses++;
            }
        }
        counts.tablePlaceholders += tablePlaceholdersHere;
        std::vector<Bindings::StageInput> inputsOfLayout;
        inputsOfLayout.reserve(draw.stages.size());
        for (const auto& stage : draw.stages) inputsOfLayout.push_back({stage.program.stage, stage.program.result.get()});
        if (!Bindings::Describe(inputsOfLayout, draw.layout, reason)) {
            refuse(counts, reason);
            counts.slotNanoseconds += elapsed(slotsStarted);
            return nullptr;
        }
        Bindings::CountSources(draw.layout, counts.sources);
        counts.bufferUses += draw.uses.size() - beforeBuffers;
        counts.imageUses += imageUses.size();
        for (const auto& use : imageUses) draw.uses.push_back({use.memory.address, use.memory.bytes, use.write});
        if (refill->Counts().placeholders - placeholdersBefore != tablePlaceholdersHere) {
            draw.complete = false;
            counts.placeholderDraws++;
        }
        counts.layoutNanoseconds += elapsed(layoutStarted);
        counts.slotNanoseconds += elapsed(slotsStarted);
        counts.stages += draw.stages.size();
        counts.uses += draw.uses.size();
        if (!draw.complete) counts.incomplete++;
        counts.resolved++;
        return std::make_shared<const ResolvedDraw>(std::move(draw));
    }

public:
    std::string Line() {
        std::lock_guard lock(mutex);
        char text[512];
        const auto perDraw = [&](std::uint64_t nanoseconds) { return totals.draws == 0 ? 0.0 : static_cast<double>(nanoseconds) / 1000.0 / static_cast<double>(totals.draws); };
        std::snprintf(text, sizeof(text), "[recode-resolve] %llu draws, %llu resolved, %llu refused, %llu without device; %llu stages, %llu slots; programs %.1f us, slots %.1f us per draw", static_cast<unsigned long long>(totals.draws), static_cast<unsigned long long>(totals.resolved), static_cast<unsigned long long>(totals.refused), static_cast<unsigned long long>(totals.noDevice), static_cast<unsigned long long>(totals.stages), static_cast<unsigned long long>(totals.slots), perDraw(totals.programNanoseconds), perDraw(totals.slotNanoseconds));
        std::string line = text;
        std::snprintf(text, sizeof(text), " (buffers %.1f, requests %.1f, fill %.1f, image tables %.1f [%llu misses %.1f], mirror %.1f [unchanged %.1f, in place %.1f], layout %.1f)", perDraw(totals.bufferNanoseconds), perDraw(totals.requestNanoseconds), perDraw(totals.fillNanoseconds), perDraw(totals.tableNanoseconds), static_cast<unsigned long long>(totals.tableMisses), perDraw(totals.tableMissNanoseconds), perDraw(totals.mirrorNanoseconds), perDraw(totals.mirrorUnchangedNanoseconds), perDraw(totals.mirrorInPlaceNanoseconds), perDraw(totals.layoutNanoseconds));
        line += text;
        if (refill != nullptr) {
            const auto refills = refill->Counts();
            const auto slotCounts = table->Counts();
            std::snprintf(text, sizeof(text), "; memo %llu hits, %llu lookups, %llu placeholders, %llu deferred to record; heap %llu writes, %llu recycled, %llu full", static_cast<unsigned long long>(refills.memoHits), static_cast<unsigned long long>(refills.lookups), static_cast<unsigned long long>(refills.placeholders), static_cast<unsigned long long>(refills.deferred), static_cast<unsigned long long>(slotCounts.writes), static_cast<unsigned long long>(slotCounts.recycled), static_cast<unsigned long long>(slotCounts.full));
            line += text;
        }
        if (mirror != nullptr) {
            const auto mirrors = mirror->Counts();
            std::snprintf(text, sizeof(text), "; tables %llu stages, mirrors %llu built, %llu reused, %llu unchanged, %llu refilled in place, %llu inherited, %zu held, %llu dropped, %llu elements filled, %llu kept, %llu placeholders, %llu reads, %llu target holds", static_cast<unsigned long long>(totals.tableStages), static_cast<unsigned long long>(mirrors.built), static_cast<unsigned long long>(mirrors.reused), static_cast<unsigned long long>(mirrors.unchanged), static_cast<unsigned long long>(mirrors.inPlace), static_cast<unsigned long long>(mirrors.inherited), mirror->Size(), static_cast<unsigned long long>(mirrors.dropped),static_cast<unsigned long long>(mirrors.elements), static_cast<unsigned long long>(mirrors.kept), static_cast<unsigned long long>(totals.tablePlaceholders), static_cast<unsigned long long>(totals.tableUses), static_cast<unsigned long long>(totals.tableTargetHolds));
            line += text;
            if (surfaces != nullptr) {
                const auto surfaceCounts = surfaces->TakeCounts();
                std::snprintf(text, sizeof(text), "; surfaces %zu held (%llu MiB), %llu evicted (%llu GPU-written kept), %llu lookups, %llu created, %llu ready, %llu pending, %llu stale, %llu detiled, %llu GPU-written to the existing cache, %llu refused", surfaces->Size(), static_cast<unsigned long long>(surfaces->HeldBytes() >> 20u), static_cast<unsigned long long>(surfaceCounts.evicted), static_cast<unsigned long long>(surfaceCounts.evictionsKept), static_cast<unsigned long long>(surfaceCounts.lookups), static_cast<unsigned long long>(surfaceCounts.created), static_cast<unsigned long long>(surfaceCounts.ready), static_cast<unsigned long long>(surfaceCounts.pending), static_cast<unsigned long long>(surfaceCounts.stale), static_cast<unsigned long long>(surfaceCounts.detiled), static_cast<unsigned long long>(surfaceCounts.gpuWritten), static_cast<unsigned long long>(totals.surfaceRefusals));
                line += text;
                for (const auto& [why, count] : surfaceReasons) line += "; surface " + why + " " + std::to_string(count);
                surfaceReasons.clear();
                const auto targetCounts = Images::Targets().TakeCounts();
                std::snprintf(text, sizeof(text), "; targets %llu noted, %llu read, %llu missed, %llu mismatched, %llu CPU-written", static_cast<unsigned long long>(targetCounts.noted), static_cast<unsigned long long>(targetCounts.hits), static_cast<unsigned long long>(targetCounts.misses), static_cast<unsigned long long>(targetCounts.mismatched), static_cast<unsigned long long>(targetCounts.cpuWritten));
                line += text;
            }
            const auto reuse = ShaderRecompiler::ResourceMaterializer::TableReuse();
            std::snprintf(text, sizeof(text), " (columns %llu, reused %llu unwritten, %llu compared, %llu words read, %llu resolved, %llu shared)", static_cast<unsigned long long>(reuse.columns), static_cast<unsigned long long>(reuse.unwritten), static_cast<unsigned long long>(reuse.compared), static_cast<unsigned long long>(reuse.wordsRead), static_cast<unsigned long long>(reuse.resolutions), static_cast<unsigned long long>(reuse.resolutionsShared));
            line += text;
            const auto memoCounts = resolutions.Counts();
            std::snprintf(text, sizeof(text), "; table memo %llu hits, %llu by content, %llu new, %llu changed, %llu samplers changed, %llu evicted, %llu held (%llu MiB)", static_cast<unsigned long long>(memoCounts.hits), static_cast<unsigned long long>(memoCounts.contentHits),static_cast<unsigned long long>(memoCounts.fresh), static_cast<unsigned long long>(memoCounts.changed), static_cast<unsigned long long>(memoCounts.samplersChanged), static_cast<unsigned long long>(memoCounts.evicted), static_cast<unsigned long long>(memoCounts.held), static_cast<unsigned long long>(memoCounts.bytes >> 20u));
            line += text;
        }
        std::snprintf(text, sizeof(text), "; per draw: decode %.1f, prepare %.1f, capture %.1f, materialize %.1f, variant %.1f us", perDraw(totals.phases[PhaseDecode]), perDraw(totals.phases[PhasePrepare]), perDraw(totals.phases[PhaseCapture]), perDraw(totals.phases[PhaseMaterialize]), perDraw(totals.phases[PhaseVariant]));
        line += text;
        line += "; gpu lock busy " + std::to_string(totals.lockBusy);
        const auto taken = [](std::array<std::atomic<std::uint64_t>, SampledClassCount>& counts) { return std::to_string(counts[SampledView].exchange(0, std::memory_order_relaxed)) + " views, " + std::to_string(counts[SampledCopy].exchange(0, std::memory_order_relaxed)) + " per-use copies, " + std::to_string(counts[SampledSnapshot].exchange(0, std::memory_order_relaxed)) + " snapshots"; };
        line += "; existing-cache sampled textures bound when resolved " + taken(sampledAhead) + "; bound when recorded " + taken(sampledAtRecord);
        line += "; their views " + std::to_string(viewChoices[ChoicePlain].exchange(0, std::memory_order_relaxed)) + " plain, " + std::to_string(viewChoices[ChoiceFirstLayer].exchange(0, std::memory_order_relaxed)) + " first layer, " + std::to_string(viewChoices[ChoiceArray].exchange(0, std::memory_order_relaxed)) + " array, " + std::to_string(viewChoices[ChoiceMismatch].exchange(0, std::memory_order_relaxed)) + " of another type than the slot";
        {
            std::lock_guard recordLock(recordMutex);
            line += "; storage images on recode surfaces " + std::to_string(storageRecords) + ", " + std::to_string(storageRecordRefusals) + " refused";
            for (const auto& [why, count] : recordRefusalReasons) line += " [" + why + " " + std::to_string(count) + "]";
        }
        const auto packets = handoff.TakeCounts();
        std::snprintf(text, sizeof(text), "; hand-off %llu offered (%llu refused), %llu found, %llu refused, %llu waited, %llu missed, %llu released, %llu dropped, %llu pending", static_cast<unsigned long long>(packets.offered), static_cast<unsigned long long>(packets.refusedOffers), static_cast<unsigned long long>(packets.found), static_cast<unsigned long long>(packets.refused), static_cast<unsigned long long>(packets.waited), static_cast<unsigned long long>(packets.missed), static_cast<unsigned long long>(packets.released), static_cast<unsigned long long>(packets.dropped), static_cast<unsigned long long>(packets.pending));
        line += text;
        std::snprintf(text, sizeof(text), "; access %.1f ranges per resolved draw, %llu incomplete (%llu indirect, %llu placeholder)", totals.resolved == 0 ? 0.0 : static_cast<double>(totals.uses) / static_cast<double>(totals.resolved), static_cast<unsigned long long>(totals.incomplete), static_cast<unsigned long long>(totals.indirect), static_cast<unsigned long long>(totals.placeholderDraws));
        line += text;
        std::snprintf(text, sizeof(text), " [targets %llu, buffers %llu, images %llu]", static_cast<unsigned long long>(totals.targetUses), static_cast<unsigned long long>(totals.bufferUses), static_cast<unsigned long long>(totals.imageUses));
        line += text;
        std::snprintf(text, sizeof(text), "; bindings words %llu, guest %llu, page table %llu, fault %llu, gds %llu, table map %llu", static_cast<unsigned long long>(totals.sources.words), static_cast<unsigned long long>(totals.sources.guest), static_cast<unsigned long long>(totals.sources.pageTable), static_cast<unsigned long long>(totals.sources.fault), static_cast<unsigned long long>(totals.sources.gds), static_cast<unsigned long long>(totals.sources.tableMap));
        line += text;
        const auto shapes = templates.TakeCounts();
        std::snprintf(text, sizeof(text), "; templates %llu hits, %llu builds (%llu stale), %llu evicted, %zu held, %.1f MiB", static_cast<unsigned long long>(shapes.hits), static_cast<unsigned long long>(shapes.builds), static_cast<unsigned long long>(shapes.rebuilds), static_cast<unsigned long long>(shapes.evictions), templates.Size(), static_cast<double>(templates.HeldBytes()) / 1048576.0);
        line += text;
        if (verifyTemplates()) line += "; template mismatches " + std::to_string(totals.templateMismatches) + (totals.templateMismatch != nullptr ? std::string(" (") + totals.templateMismatch + ")" : std::string());
        for (const auto& [reason, count] : totals.reasons) line += "; " + reason + " " + std::to_string(count);
        if (dispatchesRouted()) {
            const auto& d = dispatchTotals;
            const auto perDispatch = [&](std::uint64_t nanoseconds) { return d.draws == 0 ? 0.0 : static_cast<double>(nanoseconds) / 1000.0 / static_cast<double>(d.draws); };
            std::snprintf(text, sizeof(text), "\n[recode-resolve] %llu dispatches, %llu resolved, %llu refused, %llu without device, %llu incomplete (%llu indirect, %llu placeholder); %llu slots, %.1f ranges per resolved dispatch [buffers %llu, images %llu]; programs %.1f us (capture %.1f, materialize %.1f), slots %.1f us per dispatch", static_cast<unsigned long long>(d.draws), static_cast<unsigned long long>(d.resolved), static_cast<unsigned long long>(d.refused), static_cast<unsigned long long>(d.noDevice), static_cast<unsigned long long>(d.incomplete), static_cast<unsigned long long>(d.indirect), static_cast<unsigned long long>(d.placeholderDraws), static_cast<unsigned long long>(d.slots), d.resolved == 0 ? 0.0 : static_cast<double>(d.uses) / static_cast<double>(d.resolved), static_cast<unsigned long long>(d.bufferUses), static_cast<unsigned long long>(d.imageUses), perDispatch(d.programNanoseconds), perDispatch(d.phases[PhaseCapture]), perDispatch(d.phases[PhaseMaterialize]), perDispatch(d.slotNanoseconds));
            line += text;
            const auto handed = dispatchHandoff.TakeCounts();
            std::snprintf(text, sizeof(text), "; hand-off %llu offered, %llu found, %llu waited, %llu missed, %llu released, %llu dropped, %llu pending", static_cast<unsigned long long>(handed.offered), static_cast<unsigned long long>(handed.found), static_cast<unsigned long long>(handed.waited), static_cast<unsigned long long>(handed.missed), static_cast<unsigned long long>(handed.released), static_cast<unsigned long long>(handed.dropped), static_cast<unsigned long long>(handed.pending));
            line += text;
            for (const auto& [reason, count] : d.reasons) line += "; " + reason + " " + std::to_string(count);
            dispatchTotals = {};
        }
        totals = {};
        return line;
    }

private:
    static void bufferUses(const ShaderRecompiler::CompiledVariant& variant, const ShaderRecompiler::ResourceSnapshot& snapshot, std::vector<GuestUse>& uses) {
        const auto& buffers = variant.info.info.buffers;
        for (std::size_t index = 0; index < buffers.size() && index < snapshot.buffers.size(); ++index) {
            const auto& value = snapshot.buffers[index];
            if (value.dwordCount < 4u) continue;
            const ShaderRecompiler::ShaderBufferResource descriptor{{value.dwords[0], value.dwords[1] & ~0x40000000u, value.dwords[2], value.dwords[3]}};
            const auto address = descriptor.Base48();
            const auto bytes = static_cast<std::uint64_t>(descriptor.GetSize());
            if (address == 0u || bytes == 0u) continue;
            uses.push_back({address, bytes, buffers[index].written || buffers[index].atomic});
        }
        const auto& known = variant.info.info.knownBuffers;
        for (std::size_t index = 0; index < snapshot.knownBuffers.size(); ++index) {
            const auto& info = index < known.size() ? known[index] : ShaderRecompiler::KnownBuffer{};
            const auto [address, bytes] = KnownBufferSpan(info, snapshot.knownBuffers[index]);
            if (address == 0u || bytes == 0u) continue;
            uses.push_back({address, bytes, info.written});
        }
    }

    static bool verifyTemplates() {
        static const bool enabled = [] {
            const auto* value = std::getenv("APS5_RECODE_TEMPLATE_VERIFY");
            return value != nullptr && value[0] == '1';
        }();
        return enabled;
    }

    static std::uint64_t surfaceBudget() {
        const auto* value = std::getenv("APS5_RECODE_SURFACE_MB");
        const std::uint64_t megabytes = value == nullptr || *value == '\0' ? 4096u : std::strtoull(value, nullptr, 10);
        return megabytes << 20u;
    }

    static std::uint64_t templateBudget() {
        const auto* value = std::getenv("APS5_RECODE_TEMPLATE_MB");
        const std::uint64_t megabytes = value == nullptr || *value == '\0' ? 512u : std::strtoull(value, nullptr, 10);
        return megabytes << 20u;
    }

    static std::uint64_t elapsed(std::chrono::steady_clock::time_point from) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - from).count());
    }

    void noteRefusal(const std::string& reason) {
        std::lock_guard lock(refusalMutex);
        lastRefusal = reason;
    }

public:
    std::string TakeRefusal() {
        std::lock_guard lock(refusalMutex);
        return std::exchange(lastRefusal, {});
    }

private:
    void refuse(Totals& counts, const std::string& reason) {
        counts.refused++;
        noteRefusal(reason);
        Downgrades::NoteCurrent(Downgrades::Kind::ExistingPath, 0u, "resolve: " + reason);
        auto key = reason.substr(0, 60);
        if (seenReasons.insert((&counts == &dispatchTotals ? "dispatch " : "") + key).second) std::fprintf(stderr, "[recode-resolve] first %srefusal: %s\n", &counts == &dispatchTotals ? "dispatch " : "", reason.c_str());
        if (counts.reasons.size() < 16u || counts.reasons.contains(key)) counts.reasons[key]++;
    }

    std::optional<Heap::ResolvedImage> image(const Heap::SlotRequest& request) {
        if ((request.flags & Heap::RequestFlag::Storage) != 0u) return storageImage(request);
        if (!firstImage) {
            firstImage = true;
            std::fprintf(stderr, "[recode-resolve] first image lookup\n");
        }
        if (surfaces != nullptr && ownedByRecord(request)) return deferredImage(request);
        if (surfaces != nullptr) {
            bool fallback = false;
            auto resolved = surfaceImage(request, fallback);
            if (!fallback) return resolved;
            if (sampledBind() != SampledBind::RecordAll) {
                if (auto target = targetImage(request)) {
                    std::string detail = "render target from the existing target cache";
                    try {
                        const auto resource = Graphics::DecodeTextureResource(request.words);
                        char where[96];
                        std::snprintf(where, sizeof(where), " 0x%llx %ux%u fmt %u mips %u", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, static_cast<unsigned>(resource.format), resource.mipCount);
                        detail += where;
                        for (const auto& record : surfaces->StaleOverlapping(resource.baseAddress, Graphics::SurfaceGuestBytes(resource))) {
                            char stale[160];
                            std::snprintf(stale, sizeof(stale), "existing target cache reads 0x%llx %ux%u over record 0x%llx+0x%llx %ux%u mips %u", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, static_cast<unsigned long long>(record->key.address), static_cast<unsigned long long>(record->key.bytes), record->key.width, record->key.height, record->key.mips);
                            Downgrades::NoteCurrent(Downgrades::Kind::SampledStaleRecord, request.kind, stale);
                            break;
                        }
                    } catch (const std::exception&) {
                    }
                    Downgrades::NoteCurrent(Downgrades::Kind::UpstreamSampled, request.kind, detail);
                    return target;
                }
            }
        }
        if (sampledBind() != SampledBind::Ahead) return deferredImage(request);
        std::shared_ptr<Graphics::Texture> texture;
        {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Try);
            std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::try_to_lock);
            if (!gpuLock.owns_lock()) {
                totals.lockBusy++;
                static const bool defersBusy = std::getenv("APS5_RECODE_NO_BUSY_DEFER") == nullptr;
                if (!defersBusy) return std::nullopt;
                const auto resource = Graphics::DecodeTextureResource(request.words);
                Heap::ResolvedImage deferred;
                deferred.memory = {resource.baseAddress, Graphics::SurfaceGuestBytes(resource)};
                deferred.deferred = true;
                return deferred;
            }
            texture = Graphics::CachedSampledTexture(*context, request.words);
        }
        if (texture == nullptr || texture->Released()) return std::nullopt;
        const auto resource = Graphics::DecodeTextureResource(request.words);
        const auto bound = sampledClass(*texture);
        sampledAhead[bound].fetch_add(1, std::memory_order_relaxed);
        if (bound == SampledCopy) {
            Downgrades::NoteCurrent(Downgrades::Kind::SampledAhead, request.kind, "per-use copy refreshed when resolved, ahead of its draw");
        } else if (bound == SampledSnapshot && Events::GpuWrites().ChangedSince(resource.baseAddress, Graphics::SurfaceGuestBytes(resource), 0u)) {
            Downgrades::NoteCurrent(Downgrades::Kind::SampledAhead, request.kind, "snapshot of GPU-written memory read when resolved, ahead of its draw");
        } else {
            Downgrades::NoteCurrent(Downgrades::Kind::UpstreamSampled, request.kind, "existing sampled texture cache");
        }
        SampledChoice choice = ChoicePlain;
        const auto view = sampledView(*texture, request.kind, choice);
        viewChoices[choice].fetch_add(1, std::memory_order_relaxed);
        if (choice == ChoiceMismatch) noteViewMismatch(*texture, request);
        Heap::ResolvedImage resolved;
        resolved.image = {reinterpret_cast<std::uint64_t>(view), static_cast<std::uint32_t>(texture->Layout()), texture};
        resolved.memory = {resource.baseAddress, Graphics::SurfaceGuestBytes(resource)};
        return resolved;
    }

    std::optional<Heap::ResolvedImage> deferredImage(const Heap::SlotRequest& request) {
        try {
            const auto resource = Graphics::DecodeTextureResource(request.words);
            Heap::ResolvedImage deferred;
            deferred.memory = {resource.baseAddress, Graphics::SurfaceGuestBytes(resource)};
            deferred.deferred = true;
            return deferred;
        } catch (const std::exception& error) {
            noteSurfaceReason(std::string("deferred image: ") + error.what());
            return std::nullopt;
        }
    }

    bool ownedByRecord(const Heap::SlotRequest& request) const {
        if (!Images::OwnersEnabled()) return false;
        try {
            const auto resource = Graphics::DecodeTextureResource(request.words);
            const auto side = surfaces->OwnerOfRange(resource.baseAddress, Graphics::SurfaceGuestBytes(resource));
            return side == Images::Side::Record || (side == Images::Side::None && SampledRecordFor(resource.baseAddress, Graphics::SurfaceGuestBytes(resource)));
        } catch (const std::exception&) {
            return false;
        }
    }

    void noteTargetWrites(std::span<const GuestUse> uses) {
        std::lock_guard lock(writtenTargetsMutex);
        for (const auto& use : uses) {
            if (!use.write || use.size == 0u || !writtenTargets.emplace(use.address, use.size).second) continue;
            Events::GpuWrites().Note(use.address, use.size, Events::TargetOwner);
        }
    }

    void noteSurfaceReason(const std::string& why) {
        if (seenSurfaceReasons.insert(why).second) std::fprintf(stderr, "[recode-resolve] first surface refusal: %s\n", why.c_str());
        if (surfaceReasons.size() < 16u || surfaceReasons.contains(why)) surfaceReasons[why]++;
    }

    static bool storageDispatches() {
        static const bool storage = std::getenv("APS5_RECODE_NO_STORAGE_DISPATCHES") == nullptr;
        return storage;
    }

    static bool storageDraws() {
        static const bool storage = std::getenv("APS5_RECODE_NO_STORAGE_DRAWS") == nullptr;
        return storage;
    }

    static std::pair<std::shared_ptr<Graphics::StorageTexture>, VkImageView> storageView(const Graphics::Context& context, const Heap::SlotRequest& request, const Graphics::GuestTextureResource& resource, bool written) {
        const auto shape = (request.kind - Heap::LastSampledKind - 1u) % StorageShapes;
        const auto mip = std::min(request.flags >> Heap::RequestFlag::MipShift, resource.mipCount - 1u);
        Graphics::CompletePendingCpuWrites(context, resource.baseAddress, static_cast<std::size_t>(Graphics::SurfaceGuestBytes(resource)));
        auto texture = Graphics::CachedStorageImage(context, request.words, resource, mip, written);
        if (texture == nullptr) return {nullptr, VK_NULL_HANDLE};
        const bool firstLayer = shape == Image2DShape && resource.dimension == Graphics::TextureDimension::k2DArray;
        VkImageView view = VK_NULL_HANDLE;
        if ((request.flags & Heap::RequestFlag::Atomic64) != 0u) view = texture->Atomic64View(mip, firstLayer);
        else if ((request.flags & Heap::RequestFlag::Atomic) != 0u) view = texture->AtomicView(mip, firstLayer);
        else view = texture->StorageView(mip, firstLayer, (request.flags & Heap::RequestFlag::UintView) != 0u);
        return {std::move(texture), view};
    }

    std::optional<Heap::ResolvedImage> storageImage(const Heap::SlotRequest& request) {
        const auto shape = (request.kind - Heap::LastSampledKind - 1u) % StorageShapes;
        if (shape == MultisampleShape || shape == MultisampleArrayShape) {
            noteSurfaceReason("storage: multisampled");
            return std::nullopt;
        }
        try {
            const auto resource = Graphics::DecodeTextureResource(request.words);
            if (resource.mipCount == 0u) return std::nullopt;
            if (resource.dimension == Graphics::TextureDimension::k2DMsaa) {
                noteSurfaceReason("storage: multisampled surface");
                return std::nullopt;
            }
            {
                Heap::ResolvedImage deferred;
                deferred.memory = {resource.baseAddress, Graphics::SurfaceGuestBytes(resource)};
                deferred.deferred = true;
                if ((request.flags & Heap::RequestFlag::Written) != 0u) {
                    const GuestUse use{deferred.memory.address, deferred.memory.bytes, true};
                    if (const auto record = storageRecordFor(request)) static_cast<void>(Events::GpuWrites().Note(record->key.address, record->key.bytes, Images::OwnerOf(*record)));
                    else noteTargetWrites(std::span<const GuestUse>(&use, 1));
                    std::lock_guard lock(storageWrittenMutex);
                    const std::pair range{deferred.memory.address, deferred.memory.address + deferred.memory.bytes};
                    if (std::ranges::find(storageWritten, range) == storageWritten.end()) storageWritten.push_back(range);
                }
                return deferred;
            }
            std::shared_ptr<Graphics::StorageTexture> texture;
            VkImageView view = VK_NULL_HANDLE;
            {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Try);
                std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::try_to_lock);
                if (!gpuLock.owns_lock()) {
                    totals.lockBusy++;
                    return std::nullopt;
                }
                std::tie(texture, view) = storageView(*context, request, resource, false);
            }
            if (texture == nullptr || view == VK_NULL_HANDLE) return std::nullopt;
            Heap::ResolvedImage resolved;
            resolved.image = {reinterpret_cast<std::uint64_t>(view), static_cast<std::uint32_t>(VK_IMAGE_LAYOUT_GENERAL), texture};
            resolved.memory = {resource.baseAddress, Graphics::SurfaceGuestBytes(resource)};
            return resolved;
        } catch (const std::exception& error) {
            noteSurfaceReason(std::string("storage: ") + error.what());
            return std::nullopt;
        }
    }

    std::optional<Heap::ResolvedImage> targetImage(const Heap::SlotRequest& request) {
        std::string why;
        try {
            const auto resource = Graphics::DecodeTextureResource(request.words);
            const auto bytes = Graphics::SurfaceGuestBytes(resource);
            auto texture = Images::Targets().Find(resource.baseAddress, bytes, request.words, why);
            std::shared_ptr<Images::TargetView> view;
            if (texture != nullptr) view = targetViews->View(texture, request.words, request.kind, why);
            if (view == nullptr) {
                if (texture != nullptr) noteSurfaceReason(why);
                if (texture == nullptr && missedTargets.size() < 12u && missedTargets.insert(resource.baseAddress).second) {
                    std::fprintf(stderr, "[recode-resolve] first target miss: address 0x%llx format %u %ux%u tile %u dimension %u mips %u (%s)\n", static_cast<unsigned long long>(resource.baseAddress), resource.format, resource.width, resource.height, static_cast<unsigned>(resource.tileMode), static_cast<unsigned>(resource.dimension), resource.mipCount, why.c_str());
                }
                return std::nullopt;
            }
            Heap::ResolvedImage resolved;
            resolved.image = {reinterpret_cast<std::uint64_t>(view->View()), static_cast<std::uint32_t>(VK_IMAGE_LAYOUT_GENERAL), view};
            resolved.memory = {resource.baseAddress, bytes};
            return resolved;
        } catch (const std::exception& error) {
            noteSurfaceReason(std::string("target: ") + error.what());
            return std::nullopt;
        }
    }

    std::optional<Heap::ResolvedImage> surfaceImage(const Heap::SlotRequest& request, bool& fallback) {
        std::string why;
        const auto key = Images::KeyOf(request.words, why);
        std::optional<Images::Found> found;
        std::uint64_t view = 0;
        if (key.has_value()) found = surfaces->Find(key->surface, request.words, why);
        if (found.has_value() && found->gpuWritten) {
            fallback = true;
            return std::nullopt;
        }
        if (found.has_value()) view = found->surface->device->View(key->view, request.kind, why);
        if (view == 0u) {
            totals.surfaceRefusals++;
            noteSurfaceReason(why);
            fallback = true;
            return std::nullopt;
        }
        Heap::ResolvedImage resolved;
        resolved.image = {view, found->surface->device->Layout(), found->surface};
        resolved.memory = {key->surface.address, key->surface.bytes};
        return resolved;
    }

    std::optional<Heap::ResolvedSampler> sampler(const Heap::SlotRequest& request) {
        if (context->samplerCache == nullptr) return std::nullopt;
        try {
            const auto found = context->samplerCache->Get(*context, std::span<const std::uint32_t>(request.words).first(4), (request.flags & Heap::RequestFlag::Compare) != 0u, false, true, false);
            if (found == nullptr) return std::nullopt;
            return Heap::ResolvedSampler{reinterpret_cast<std::uint64_t>(found->Handle()), found};
        } catch (const std::exception& error) {
            noteSurfaceReason(std::string("sampler: ") + error.what());
            return std::nullopt;
        }
    }

    std::mutex mutex;
    std::shared_ptr<VulkanDevice> attached;
    std::atomic<const VulkanDevice*> attachedDevice{nullptr};
    std::unique_ptr<Graphics::Context> context;
    ShaderRecompiler::SpirvTarget target;
    ShaderRecompiler::SpirvTarget computeWave32;
    ShaderRecompiler::SpirvTarget computeWave64;
    bool fixedSlots = false;
    std::unique_ptr<Heap::VulkanHeapDevice> heap;
    std::atomic<const Heap::VulkanHeapDevice*> published{nullptr};
    std::unique_ptr<Images::VulkanSurfaceDevice> surfaceDevice;
    std::unique_ptr<Images::SurfaceTable> surfaces;
    std::atomic<Images::SurfaceTable*> publishedSurfaces{nullptr};
    std::unique_ptr<Images::TargetViews> targetViews;
    std::unique_ptr<Heap::SlotTable> table;
    std::unique_ptr<Heap::Memo> memo;
    std::unique_ptr<Heap::Refill> refill;
    std::unique_ptr<Tables::TableMirror> mirror;
    Tables::TableResolutions resolutions{Tables::ResolutionBytes, std::getenv("APS5_RECODE_TABLE_MEMO_BY_STORAGE") == nullptr};
    TemplateTable templates{templateBudget()};
    PacketTable<ResolvedDraw> handoff;
    PacketTable<ResolvedDraw> dispatchHandoff;
    Totals totals;
    Totals dispatchTotals;
    std::set<std::string> seenReasons;
    std::set<std::string> seenSurfaceReasons;
    std::set<std::uint64_t> missedTargets;
    std::mutex writtenTargetsMutex;
    std::set<std::pair<std::uint64_t, std::uint64_t>> writtenTargets;
    std::mutex storageWrittenMutex;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> storageWritten;
    std::map<std::string, std::uint64_t> surfaceReasons;
    bool firstBuilt = false;
    bool firstImage = false;
};

Resolver& shared() {
    static auto* resolver = new Resolver();
    return *resolver;
}

}

bool Active() {
    static const bool enabled = [] {
        const auto* value = std::getenv("APS5_RECODE_DRAW");
        return value != nullptr && (value[0] == '1' || std::string_view(value) == "record");
    }();
    return enabled;
}

void Attach(const std::shared_ptr<VulkanDevice>& device) {
    if (Active()) shared().Attach(device);
}

void Consume(const Operation& operation, const DecodeState& state) {
    try {
        shared().Consume(operation, state);
    } catch (const std::exception& error) {
        shared().ConsumeFailed(operation, error.what());
    }
}

std::string Line() {
    return shared().Line();
}

std::string TakeRefusal() {
    return shared().TakeRefusal();
}

std::chrono::milliseconds HandoffWait() {
    return Downgrades::ExistingPathMode() == Downgrades::ExistingPath::Allow ? std::chrono::milliseconds(50) : std::chrono::milliseconds(60000);
}

PacketTable<ResolvedDraw>& ResolvedDraws() {
    return shared().Table();
}

bool DispatchesActive() {
    return Active() && dispatchesRouted();
}

bool DispatchesRoutedOn(std::uint32_t queue) {
    return Active() && routesQueue(queue);
}

PacketTable<ResolvedDraw>& ResolvedDispatches() {
    return shared().DispatchTable();
}

const Heap::VulkanHeapDevice* HeapDevice() {
    return shared().Device();
}

Images::SurfaceTable* Surfaces() {
    return shared().SurfaceTableOf();
}

std::shared_ptr<Graphics::StorageTexture> BindDeferredImage(const Graphics::Context& context, const Heap::SlotRequest& request, std::uint32_t& slot, std::string& reason) {
    return shared().BindDeferred(context, request, slot, reason);
}

std::shared_ptr<Images::Surface> BindDeferredRecord(const Heap::SlotRequest& request, std::uint32_t& slot) {
    return shared().BindDeferredRecord(request, slot);
}

std::shared_ptr<Images::Surface> BindRecordSlot(const Heap::SlotRequest& request, std::shared_ptr<Images::Surface> record, std::uint32_t& slot) {
    return shared().BindRecordSlot(request, std::move(record), slot);
}

bool StorageRecordShape(const Heap::SlotRequest& request) {
    return Resolver::StorageRecordShape(request);
}

bool BindViewSlot(const Heap::SlotRequest& request, std::uint64_t view, std::uint32_t layout, std::shared_ptr<const void> owner, std::uint32_t& slot) {
    return shared().BindViewSlot(request, view, layout, std::move(owner), slot);
}

std::shared_ptr<const void> BindDeferredSampled(const Graphics::Context& context, const Heap::SlotRequest& request, std::uint32_t& slot, std::string& reason) {
    return shared().BindDeferredSampled(context, request, slot, reason);
}

}
