#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DECODER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DECODER_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Unified/include/Operation.hpp"
#include "prx/libSceAgcDriver/Unified/include/SnapshotRing.hpp"
#include "prx/libSceAgcDriver/Unified/include/Ranges.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace AgcDriver::Unified {

inline constexpr std::size_t SnapshotSlots = 256;

struct DrawScalars {
    std::uint64_t indexBase = 0;
    std::uint64_t drawIndirectBase = 0;
    std::uint64_t dispatchIndirectBase = 0;
    std::uint32_t indexBufferSize = 0;
    bool indexBufferSizeKnown = false;
    std::uint32_t indexType = 0;
    std::uint32_t instanceCount = 1;
};

struct DecodeCounts {
    std::uint64_t operations = 0;
    std::uint64_t draws = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t flips = 0;
    std::uint64_t labelStores = 0;
    std::uint64_t waits = 0;
    std::uint64_t conditional = 0;
    std::uint64_t conditionalDraws = 0;
    std::uint64_t conditionalDispatches = 0;
    std::uint64_t stateFromMemory = 0;
    std::uint64_t stateErrors = 0;
    std::uint64_t snapshotStalls = 0;
    std::uint64_t resolved = 0;
    std::uint64_t resolveErrors = 0;
    std::uint64_t truncated = 0;
    std::uint64_t useReports = 0;
    std::uint64_t useErrors = 0;
    std::uint64_t resets = 0;
};

class Sink {
public:
    virtual ~Sink() = default;
    virtual void Emit(const Operation& operation) = 0;
    virtual void MakeRoom() = 0;
};

using UseSink = std::function<void(std::span<const GuestUse> uses)>;

class DecodeState {
public:
    explicit DecodeState(std::uint32_t queue) : queueId(queue) {}

    void RetireBefore(const SnapshotIds& ids);
    void SetResolve(bool enabled) { resolve = enabled; }
    void SetUses(UseSink sink) { uses = std::move(sink); }
    void ResetQueueState() {
        state = QueueState{};
        contextDirty = userConfigDirty = shaderDirty = scalarsDirty = true;
        targetContext = ~std::uint64_t{0};
        ++counts.resets;
    }
    [[nodiscard]] const Registers* Context(std::uint64_t id) const { return context.Find(id); }
    [[nodiscard]] const Registers* UserConfig(std::uint64_t id) const { return userConfig.Find(id); }
    [[nodiscard]] const Registers* Shader(std::uint64_t id) const { return shader.Find(id); }
    [[nodiscard]] const DrawScalars* Scalars(std::uint64_t id) const { return scalars.Find(id); }
    [[nodiscard]] const DecodeCounts& Counts() const { return counts; }
    [[nodiscard]] std::uint32_t Queue() const { return queueId; }
    [[nodiscard]] std::uint64_t Sequence() const { return sequence; }
    [[nodiscard]] const QueueState& Live() const { return state; }
    [[nodiscard]] const DriverDetail::Submission* CurrentSubmission() const { return submission; }
    void Hold(std::shared_ptr<const DriverDetail::Submission> current) { held = std::move(current); }
    [[nodiscard]] const std::shared_ptr<const DriverDetail::Submission>& Held() const { return held; }

private:
    friend class Decoder;
    friend void DecodeSubmission(const DriverDetail::Submission& submission, DecodeState& state, Sink& sink);

    std::uint32_t queueId;
    const DriverDetail::Submission* submission = nullptr;
    std::shared_ptr<const DriverDetail::Submission> held;
    QueueState state;
    SnapshotRing<Registers, SnapshotSlots> context;
    SnapshotRing<Registers, SnapshotSlots> userConfig;
    SnapshotRing<Registers, SnapshotSlots> shader;
    SnapshotRing<DrawScalars, SnapshotSlots> scalars;
    bool contextDirty = true;
    bool userConfigDirty = true;
    bool shaderDirty = true;
    bool scalarsDirty = true;
    bool resolve = false;
    UseSink uses;
    std::vector<GuestUse> useBuffer;
    std::uint64_t targetContext = ~std::uint64_t{0};
    std::uint64_t sequence = 0;
    DecodeCounts counts;
};

void DecodeSubmission(const DriverDetail::Submission& submission, DecodeState& state, Sink& sink);

}

#endif
