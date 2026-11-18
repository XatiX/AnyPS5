#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RECORDVERIFY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RECORDVERIFY_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Unified/include/Decoder.hpp"
#include "prx/libSceAgcDriver/Unified/include/Operation.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace AgcDriver::Unified::Record {

struct VerifyCounts {
    std::uint64_t matched = 0;
    std::uint64_t mismatched = 0;
    std::uint64_t unresolved = 0;
    std::uint64_t bridgeMismatched = 0;
    std::uint64_t bridgeMissing = 0;
    std::uint64_t pending = 0;
    std::uint64_t dropped = 0;
    std::array<std::uint64_t, 4> bankDifferences{};
};

using BankHashes = std::array<std::uint64_t, 4>;

BankHashes HashBanks(const QueueState& queue);

class Verifier {
public:
    static constexpr std::size_t MaxPending = 1u << 16u;
    static constexpr std::uint64_t DetailedMismatches = 4;

    void NoteDecoded(const Operation& operation, const DecodeState& state);
    void NoteDriverDraw(const QueueState& queue, std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset);
    void NoteDriverKey(std::uint64_t key, std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset, const BankHashes& banks = {});
    [[nodiscard]] VerifyCounts Counts() const;

private:
    struct Entry {
        std::uint64_t key = 0;
        bool decoded = false;
        bool resolved = false;
        BankHashes banks{};
        std::uint32_t queue = 0;
        std::uint64_t serial = 0;
        std::size_t packetOffset = 0;
        std::shared_ptr<const QueueState> state;
    };
    void meet(std::uint64_t at, const Entry& entry);
    void report(const Entry& decoded, const Entry& driver);
    static std::uint64_t slotOf(std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset);

    std::atomic<bool> detailing{true};
    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t, Entry> waiting;
    VerifyCounts counts;
};

bool VerifyActive();
Verifier& SharedVerifier();
std::string VerifyLine();

}

#endif
