#include "prx/libSceAgcDriver/Unified/include/RecordVerify.hpp"
#include "prx/libSceAgcDriver/Unified/include/RouteDefaults.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordState.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRegisterKey.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Unified::Record {

namespace {

std::uint64_t hashRegisters(const Registers& registers) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    registers.ForEach([&](std::uint32_t offset, std::uint32_t value) { hash = (hash ^ ((static_cast<std::uint64_t>(offset) << 32u) | value)) * 0x100000001b3ull; });
    return hash;
}

}

BankHashes HashBanks(const QueueState& queue) {
    std::uint64_t scalars = 0xcbf29ce484222325ull;
    for (const auto value : {queue.indexBase, queue.drawIndirectBase, queue.dispatchIndirectBase, static_cast<std::uint64_t>(queue.indexBufferSize.value_or(0xffffffffu)), static_cast<std::uint64_t>(queue.indexType), static_cast<std::uint64_t>(queue.instanceCount)}) scalars = (scalars ^ value) * 0x100000001b3ull;
    return {hashRegisters(queue.context), hashRegisters(queue.userConfig), hashRegisters(queue.shader), scalars};
}

std::uint64_t Verifier::slotOf(std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset) {
    return (static_cast<std::uint64_t>(queueId & 0xffu) << 56u) ^ (serial << 28u) ^ static_cast<std::uint64_t>(packetOffset);
}

void Verifier::NoteDecoded(const Operation& operation, const DecodeState& state) {
    if (operation.kind != OpKind::Draw && operation.kind != OpKind::DrawIndirect) return;
    if (operation.predicated) return;
    BankHashes banks{};
    std::shared_ptr<const QueueState> kept;
    if (operation.resolved) {
        const auto rebuilt = QueueStateAt(state, operation.snapshots);
        if (rebuilt) banks = HashBanks(*rebuilt);
        if (rebuilt && detailing.load(std::memory_order_relaxed)) kept = std::make_shared<const QueueState>(*rebuilt);
        std::lock_guard lock(mutex);
        if (!rebuilt) ++counts.bridgeMissing;
        else if (DriverDetail::RegisterStateKey(*rebuilt) != operation.key) ++counts.bridgeMismatched;
    }
    meet(slotOf(operation.queue, operation.submission, operation.packetOffset), {operation.key, true, operation.resolved, banks, operation.queue, operation.submission, operation.packetOffset, kept});
}

void Verifier::NoteDriverDraw(const QueueState& queue, std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset) {
    const auto key = DriverDetail::RegisterStateKey(queue);
    const auto kept = detailing.load(std::memory_order_relaxed) ? std::make_shared<const QueueState>(queue) : nullptr;
    meet(slotOf(queueId, serial, packetOffset), {key, false, true, HashBanks(queue), queueId, serial, packetOffset, kept});
}

void Verifier::NoteDriverKey(std::uint64_t key, std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset, const BankHashes& banks) {
    meet(slotOf(queueId, serial, packetOffset), {key, false, true, banks, queueId, serial, packetOffset});
}

void Verifier::report(const Entry& decoded, const Entry& driver) {
    static const char* const names[] = {"context", "uconfig", "shader", "scalars"};
    std::string differing;
    for (std::size_t bank = 0; bank < decoded.banks.size(); ++bank) {
        if (decoded.banks[bank] == driver.banks[bank]) continue;
        ++counts.bankDifferences[bank];
        differing += ' ';
        differing += names[bank];
    }
    if (counts.mismatched >= DetailedMismatches) detailing.store(false, std::memory_order_relaxed);
    if (counts.mismatched > 16) return;
    std::fprintf(stderr, "[recode-verify] mismatch q0x%x serial %llu offset %zu banks:%s\n", decoded.queue, static_cast<unsigned long long>(decoded.serial), decoded.packetOffset, differing.empty() ? " none" : differing.c_str());
    if (decoded.state == nullptr || driver.state == nullptr) return;
    const auto compare = [](const char* bank, const Registers& left, const Registers& right) {
        std::string line;
        int shown = 0;
        const auto add = [&](std::uint32_t offset, long long ours, long long theirs) {
            if (++shown > 12) return;
            char part[64];
            std::snprintf(part, sizeof(part), " %x:%llx/%llx", offset, ours, theirs);
            line += part;
        };
        left.ForEach([&](std::uint32_t offset, std::uint32_t value) {
            if (!right.contains(offset)) add(offset, value, -1);
            else if (right.at(offset) != value) add(offset, value, right.at(offset));
        });
        right.ForEach([&](std::uint32_t offset, std::uint32_t value) {
            if (!left.contains(offset)) add(offset, -1, value);
        });
        if (shown != 0) std::fprintf(stderr, "[recode-verify]   %s (decoded/driver, %d differ):%s\n", bank, shown, line.c_str());
    };
    compare("uconfig", decoded.state->userConfig, driver.state->userConfig);
    compare("shader", decoded.state->shader, driver.state->shader);
    std::fprintf(stderr, "[recode-verify]   scalars decoded index %llx type %u instances %u size %lld; driver index %llx type %u instances %u size %lld\n",
        static_cast<unsigned long long>(decoded.state->indexBase), decoded.state->indexType, decoded.state->instanceCount, decoded.state->indexBufferSize ? static_cast<long long>(*decoded.state->indexBufferSize) : -1LL,
        static_cast<unsigned long long>(driver.state->indexBase), driver.state->indexType, driver.state->instanceCount, driver.state->indexBufferSize ? static_cast<long long>(*driver.state->indexBufferSize) : -1LL);
}

void Verifier::meet(std::uint64_t at, const Entry& entry) {
    std::lock_guard lock(mutex);
    const auto found = waiting.find(at);
    if (found == waiting.end() || found->second.decoded == entry.decoded) {
        if (waiting.size() >= MaxPending) {
            counts.dropped += waiting.size();
            waiting.clear();
        }
        waiting.insert_or_assign(at, entry);
        return;
    }
    const auto& decoded = entry.decoded ? entry : found->second;
    const auto& driver = entry.decoded ? found->second : entry;
    if (!decoded.resolved) ++counts.unresolved;
    else if (decoded.key == driver.key) ++counts.matched;
    else {
        ++counts.mismatched;
        report(decoded, driver);
    }
    waiting.erase(found);
}

VerifyCounts Verifier::Counts() const {
    std::lock_guard lock(mutex);
    auto result = counts;
    result.pending = waiting.size();
    return result;
}

bool VerifyActive() {
    static const bool active = [] {
        const char* value = RouteValue("APS5_RECODE_RECORD");
        return value != nullptr && std::strcmp(value, "verify") == 0;
    }();
    return active;
}

Verifier& SharedVerifier() {
    static Verifier& verifier = *new Verifier();
    return verifier;
}

std::string VerifyLine() {
    if (!VerifyActive()) return {};
    const auto counts = SharedVerifier().Counts();
    char line[448];
    std::snprintf(line, sizeof(line), "[recode-verify] draws matched %llu, mismatched %llu (context %llu, uconfig %llu, shader %llu, scalars %llu), unresolved %llu; bridge mismatched %llu, missing %llu; pending %llu, dropped %llu\n",
        static_cast<unsigned long long>(counts.matched), static_cast<unsigned long long>(counts.mismatched),
        static_cast<unsigned long long>(counts.bankDifferences[0]), static_cast<unsigned long long>(counts.bankDifferences[1]), static_cast<unsigned long long>(counts.bankDifferences[2]), static_cast<unsigned long long>(counts.bankDifferences[3]),
        static_cast<unsigned long long>(counts.unresolved),
        static_cast<unsigned long long>(counts.bridgeMismatched), static_cast<unsigned long long>(counts.bridgeMissing),
        static_cast<unsigned long long>(counts.pending), static_cast<unsigned long long>(counts.dropped));
    return line;
}

}
