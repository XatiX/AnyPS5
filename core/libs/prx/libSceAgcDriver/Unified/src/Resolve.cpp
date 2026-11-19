#include "prx/libSceAgcDriver/Unified/include/Resolve.hpp"
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
#include "prx/libSceAgcDriver/Unified/include/VulkanHeap.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <mutex>
#include <set>

namespace AgcDriver::Unified::Resolve {

namespace {

struct Totals {
    std::uint64_t draws = 0;
    std::uint64_t resolved = 0;
    std::uint64_t refused = 0;
    std::uint64_t noDevice = 0;
    std::uint64_t stages = 0;
    std::uint64_t slots = 0;
    std::uint64_t programNanoseconds = 0;
    std::uint64_t slotNanoseconds = 0;
    std::uint64_t lockBusy = 0;
    std::uint64_t uses = 0;
    std::uint64_t incomplete = 0;
    std::uint64_t indirect = 0;
    std::uint64_t targetUses = 0;
    std::uint64_t bufferUses = 0;
    std::uint64_t imageUses = 0;
    std::uint64_t placeholderDraws = 0;
    std::uint64_t templateMismatches = 0;
    const char* templateMismatch = nullptr;
    Bindings::SourceCounts sources;
    std::map<std::string, std::uint64_t> reasons;
    std::array<std::uint64_t, PhaseCount> phases{};
};

class Resolver {
public:
    void Attach(const std::shared_ptr<VulkanDevice>& device) {
        std::lock_guard lock(mutex);
        if (device == nullptr || device.get() == attached.get()) return;
        attached = device;
        context = std::make_unique<Graphics::Context>(device->graphicsContext());
        target = HeapTarget(device->Target());
        fixedSlots = device->GraphicsPipelineLibraries();
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
            std::fprintf(stderr, "[recode-resolve] descriptor heap: %u sampled, %u storage, %u sampler slots\n", heap->SampledSlotCount(), heap->StorageSlotCount(), heap->SamplerSlotCount());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[recode-resolve] descriptor heap unavailable: %s\n", error.what());
            heap.reset();
            table.reset();
            memo.reset();
            refill.reset();
        }
    }

    void Consume(const Operation& operation, const DecodeState& state) {
        if (!IsDraw(operation.kind)) return;
        std::shared_ptr<const ResolvedDraw> draw;
        if (operation.resolved) {
            std::lock_guard lock(mutex);
            draw = resolve(operation, state);
        }
        handoff.Offer(operation.queue, operation.submission, operation.packetOffset, std::move(draw));
    }

    PacketTable<ResolvedDraw>& Table() { return handoff; }

private:
    std::shared_ptr<const ResolvedDraw> resolve(const Operation& operation, const DecodeState& state) {
        totals.draws++;
        const auto* submission = state.CurrentSubmission();
        if (refill == nullptr || submission == nullptr || submission->shaders == nullptr) {
            totals.noDevice++;
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
            refuse(reason);
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
        totals.targetUses += draw.uses.size() - beforeTargets;
        const auto beforeBuffers = draw.uses.size();
        const auto placeholdersBefore = refill->Counts().placeholders;
        std::vector<Heap::ImageUse> imageUses;
        for (auto& stage : programs.stages) {
            bufferUses(*stage.variant, stage.capture->snapshot, draw.uses);
            std::vector<Heap::SlotRequest> requests;
            if (stage.result->directHeap) {
                Heap::SlotStatistics statistics;
                if (!Heap::BuildSlotRequests(stage.variant->info, stage.capture->snapshot, requests, statistics, reason, false)) {
                    refuse(reason);
                    totals.slotNanoseconds += elapsed(slotsStarted);
                    return nullptr;
                }
            }
            ResolvedStage resolved{std::move(stage), {}};
            if (!refill->Fill(requests, resolved.slots, reason, &imageUses) || !PatchSlots(*resolved.program.result, resolved.slots, resolved.program.words, reason)) {
                refuse(reason);
                totals.slotNanoseconds += elapsed(slotsStarted);
                return nullptr;
            }
            totals.slots += resolved.slots.size();
            draw.stages.push_back(std::move(resolved));
        }
        std::vector<Bindings::StageInput> inputsOfLayout;
        inputsOfLayout.reserve(draw.stages.size());
        for (const auto& stage : draw.stages) inputsOfLayout.push_back({stage.program.stage, stage.program.result.get()});
        if (!Bindings::Describe(inputsOfLayout, draw.layout, reason)) {
            refuse(reason);
            totals.slotNanoseconds += elapsed(slotsStarted);
            return nullptr;
        }
        Bindings::CountSources(draw.layout, totals.sources);
        draw.state = std::make_shared<const Graphics::State>(programs.decode.state);
        totals.bufferUses += draw.uses.size() - beforeBuffers;
        totals.imageUses += imageUses.size();
        for (const auto& use : imageUses) draw.uses.push_back({use.memory.address, use.memory.bytes, use.write});
        if (refill->Counts().placeholders != placeholdersBefore) {
            draw.complete = false;
            totals.placeholderDraws++;
        }
        totals.slotNanoseconds += elapsed(slotsStarted);
        totals.stages += draw.stages.size();
        totals.uses += draw.uses.size();
        if (!draw.complete) totals.incomplete++;
        totals.resolved++;
        return std::make_shared<const ResolvedDraw>(std::move(draw));
    }

public:
    std::string Line() {
        std::lock_guard lock(mutex);
        char text[512];
        const auto perDraw = [&](std::uint64_t nanoseconds) { return totals.draws == 0 ? 0.0 : static_cast<double>(nanoseconds) / 1000.0 / static_cast<double>(totals.draws); };
        std::snprintf(text, sizeof(text), "[recode-resolve] %llu draws, %llu resolved, %llu refused, %llu without device; %llu stages, %llu slots; programs %.1f us, slots %.1f us per draw", static_cast<unsigned long long>(totals.draws), static_cast<unsigned long long>(totals.resolved), static_cast<unsigned long long>(totals.refused), static_cast<unsigned long long>(totals.noDevice), static_cast<unsigned long long>(totals.stages), static_cast<unsigned long long>(totals.slots), perDraw(totals.programNanoseconds), perDraw(totals.slotNanoseconds));
        std::string line = text;
        if (refill != nullptr) {
            const auto refills = refill->Counts();
            const auto slotCounts = table->Counts();
            std::snprintf(text, sizeof(text), "; memo %llu hits, %llu lookups, %llu placeholders; heap %llu writes, %llu recycled, %llu full", static_cast<unsigned long long>(refills.memoHits), static_cast<unsigned long long>(refills.lookups), static_cast<unsigned long long>(refills.placeholders), static_cast<unsigned long long>(slotCounts.writes), static_cast<unsigned long long>(slotCounts.recycled), static_cast<unsigned long long>(slotCounts.full));
            line += text;
        }
        std::snprintf(text, sizeof(text), "; per draw: decode %.1f, prepare %.1f, capture %.1f, materialize %.1f, variant %.1f us", perDraw(totals.phases[PhaseDecode]), perDraw(totals.phases[PhasePrepare]), perDraw(totals.phases[PhaseCapture]), perDraw(totals.phases[PhaseMaterialize]), perDraw(totals.phases[PhaseVariant]));
        line += text;
        line += "; gpu lock busy " + std::to_string(totals.lockBusy);
        const auto packets = handoff.TakeCounts();
        std::snprintf(text, sizeof(text), "; hand-off %llu offered (%llu refused), %llu found, %llu refused, %llu waited, %llu missed, %llu released, %llu dropped, %llu pending", static_cast<unsigned long long>(packets.offered), static_cast<unsigned long long>(packets.refusedOffers), static_cast<unsigned long long>(packets.found), static_cast<unsigned long long>(packets.refused), static_cast<unsigned long long>(packets.waited), static_cast<unsigned long long>(packets.missed), static_cast<unsigned long long>(packets.released), static_cast<unsigned long long>(packets.dropped), static_cast<unsigned long long>(packets.pending));
        line += text;
        std::snprintf(text, sizeof(text), "; access %.1f ranges per resolved draw, %llu incomplete (%llu indirect, %llu placeholder)", totals.resolved == 0 ? 0.0 : static_cast<double>(totals.uses) / static_cast<double>(totals.resolved), static_cast<unsigned long long>(totals.incomplete), static_cast<unsigned long long>(totals.indirect), static_cast<unsigned long long>(totals.placeholderDraws));
        line += text;
        std::snprintf(text, sizeof(text), " [targets %llu, buffers %llu, images %llu]", static_cast<unsigned long long>(totals.targetUses), static_cast<unsigned long long>(totals.bufferUses), static_cast<unsigned long long>(totals.imageUses));
        line += text;
        std::snprintf(text, sizeof(text), "; bindings words %llu, guest %llu, page table %llu, fault %llu, gds %llu", static_cast<unsigned long long>(totals.sources.words), static_cast<unsigned long long>(totals.sources.guest), static_cast<unsigned long long>(totals.sources.pageTable), static_cast<unsigned long long>(totals.sources.fault), static_cast<unsigned long long>(totals.sources.gds));
        line += text;
        const auto shapes = templates.TakeCounts();
        std::snprintf(text, sizeof(text), "; templates %llu hits, %llu builds (%llu stale), %llu evicted, %zu held, %.1f MiB", static_cast<unsigned long long>(shapes.hits), static_cast<unsigned long long>(shapes.builds), static_cast<unsigned long long>(shapes.rebuilds), static_cast<unsigned long long>(shapes.evictions), templates.Size(), static_cast<double>(templates.HeldBytes()) / 1048576.0);
        line += text;
        if (verifyTemplates()) line += "; template mismatches " + std::to_string(totals.templateMismatches) + (totals.templateMismatch != nullptr ? std::string(" (") + totals.templateMismatch + ")" : std::string());
        for (const auto& [reason, count] : totals.reasons) line += "; " + reason + " " + std::to_string(count);
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
    }

    static bool verifyTemplates() {
        static const bool enabled = [] {
            const auto* value = std::getenv("APS5_RECODE_TEMPLATE_VERIFY");
            return value != nullptr && value[0] == '1';
        }();
        return enabled;
    }

    static std::uint64_t templateBudget() {
        const auto* value = std::getenv("APS5_RECODE_TEMPLATE_MB");
        const std::uint64_t megabytes = value == nullptr || *value == '\0' ? 512u : std::strtoull(value, nullptr, 10);
        return megabytes << 20u;
    }

    static std::uint64_t elapsed(std::chrono::steady_clock::time_point from) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - from).count());
    }

    void refuse(const std::string& reason) {
        totals.refused++;
        auto key = reason.substr(0, 60);
        if (seenReasons.insert(key).second) std::fprintf(stderr, "[recode-resolve] first refusal: %s\n", reason.c_str());
        if (totals.reasons.size() < 16u || totals.reasons.contains(key)) totals.reasons[key]++;
    }

    std::optional<Heap::ResolvedImage> image(const Heap::SlotRequest& request) {
        if ((request.flags & Heap::RequestFlag::Storage) != 0u) return std::nullopt;
        if (!firstImage) {
            firstImage = true;
            std::fprintf(stderr, "[recode-resolve] first image lookup\n");
        }
        std::shared_ptr<Graphics::Texture> texture;
        {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Try);
            std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::try_to_lock);
            if (!gpuLock.owns_lock()) {
                totals.lockBusy++;
                return std::nullopt;
            }
            texture = Graphics::CachedSampledTexture(*context, request.words);
        }
        if (texture == nullptr || texture->Released()) return std::nullopt;
        const auto resource = Graphics::DecodeTextureResource(request.words);
        Heap::ResolvedImage resolved;
        resolved.image = {reinterpret_cast<std::uint64_t>(texture->View()), static_cast<std::uint32_t>(texture->Layout()), texture};
        resolved.memory = {resource.baseAddress, Graphics::SurfaceGuestBytes(resource)};
        return resolved;
    }

    std::optional<Heap::ResolvedSampler> sampler(const Heap::SlotRequest& request) {
        if (context->samplerCache == nullptr) return std::nullopt;
        std::shared_ptr<Graphics::Sampler> found;
        {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Try);
            std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::try_to_lock);
            if (!gpuLock.owns_lock()) {
                totals.lockBusy++;
                return std::nullopt;
            }
            found =context->samplerCache->Get(*context, std::span<const std::uint32_t>(request.words).first(4), false, false, true, false);
        }
        if (found == nullptr) return std::nullopt;
        return Heap::ResolvedSampler{reinterpret_cast<std::uint64_t>(found->Handle()), found};
    }

    std::mutex mutex;
    std::shared_ptr<VulkanDevice> attached;
    std::unique_ptr<Graphics::Context> context;
    ShaderRecompiler::SpirvTarget target;
    bool fixedSlots = false;
    std::unique_ptr<Heap::VulkanHeapDevice> heap;
    std::unique_ptr<Heap::SlotTable> table;
    std::unique_ptr<Heap::Memo> memo;
    std::unique_ptr<Heap::Refill> refill;
    TemplateTable templates{templateBudget()};
    PacketTable<ResolvedDraw> handoff;
    Totals totals;
    std::set<std::string> seenReasons;
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
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void Attach(const std::shared_ptr<VulkanDevice>& device) {
    if (Active()) shared().Attach(device);
}

void Consume(const Operation& operation, const DecodeState& state) {
    shared().Consume(operation, state);
}

std::string Line() {
    return shared().Line();
}

PacketTable<ResolvedDraw>& ResolvedDraws() {
    return shared().Table();
}

}
