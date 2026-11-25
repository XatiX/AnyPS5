#include "prx/libSceAgcDriver/Unified/include/RecordDraw.hpp"
#include "prx/libSceAgcDriver/Unified/include/RouteDefaults.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordState.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Unified::Record {

std::uint64_t Handoff::slotOf(std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset) {
    return (static_cast<std::uint64_t>(queueId & 0xffu) << 56u) ^ (serial << 28u) ^ static_cast<std::uint64_t>(packetOffset);
}

void Handoff::Offer(const Operation& operation, const DecodeState& state) {
    if (operation.kind != OpKind::Draw && operation.kind != OpKind::DrawIndirect) return;
    auto rebuilt = QueueStateAt(state, operation.snapshots);
    if (!rebuilt) return;
    auto shared = std::make_shared<QueueState>(std::move(*rebuilt));
    {
        std::lock_guard lock(mutex);
        if (ready.size() >= MaxPending) {
            counts.dropped += ready.size();
            ready.clear();
        }
        ready.insert_or_assign(slotOf(operation.queue, operation.submission, operation.packetOffset), std::move(shared));
        ++counts.offered;
    }
    offered.notify_all();
}

std::shared_ptr<QueueState> Handoff::Take(std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset, std::chrono::milliseconds wait) {
    const auto slot = slotOf(queueId, serial, packetOffset);
    std::unique_lock lock(mutex);
    auto found = ready.find(slot);
    if (found == ready.end()) {
        ++counts.waited;
        offered.wait_for(lock, wait, [&] { return (found = ready.find(slot)) != ready.end(); });
    }
    if (found == ready.end()) {
        ++counts.missed;
        return nullptr;
    }
    auto state = std::move(found->second);
    ready.erase(found);
    ++counts.taken;
    return state;
}

HandoffCounts Handoff::Counts() const {
    std::lock_guard lock(mutex);
    auto result = counts;
    result.pending = ready.size();
    return result;
}

bool DrawActive() {
    static const bool active = [] {
        const char* value = RouteValue("APS5_RECODE_RECORD");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return active;
}

Handoff& SharedHandoff() {
    static Handoff& handoff = *new Handoff();
    return handoff;
}

std::string HandoffLine() {
    if (!DrawActive()) return {};
    const auto counts = SharedHandoff().Counts();
    char line[256];
    std::snprintf(line, sizeof(line), "[recode-record] draws from decoded state %llu of %llu offered; waited %llu, missed %llu, dropped %llu, pending %llu\n",
        static_cast<unsigned long long>(counts.taken), static_cast<unsigned long long>(counts.offered), static_cast<unsigned long long>(counts.waited),
        static_cast<unsigned long long>(counts.missed), static_cast<unsigned long long>(counts.dropped), static_cast<unsigned long long>(counts.pending));
    return line;
}

}
