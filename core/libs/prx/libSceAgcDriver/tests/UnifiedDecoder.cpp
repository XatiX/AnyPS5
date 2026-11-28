#include "prx/libSceAgcDriver/Unified/include/Decoder.hpp"
#include "prx/libSceAgcDriver/Unified/include/Order.hpp"
#include "prx/libSceAgcDriver/Unified/include/Front.hpp"
#include "prx/libSceAgcDriver/Unified/include/Intake.hpp"
#include "prx/libSceAgcDriver/Unified/include/Replay.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordState.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordVerify.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordDraw.hpp"
#include "prx/libSceAgcDriver/Execution/include/SubmissionCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "../tools/Pm4Analysis.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

using namespace AgcDriver;
using namespace AgcDriver::Unified;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

class Recorded : public Sink {
public:
    explicit Recorded(DecodeState& state) : state(state) {}
    void Emit(const Operation& operation) override {
        operations.push_back(operation);
        if (retire) state.RetireBefore(operation.snapshots);
    }
    void MakeRoom() override { ++roomRequests; }

    DecodeState& state;
    std::vector<Operation> operations;
    bool retire = true;
    int roomRequests = 0;
};

std::uint32_t type3(std::uint32_t opcode, std::size_t words, std::uint32_t flags = 0) {
    return (3u << 30u) | (static_cast<std::uint32_t>(words - 2) << 16u) | (opcode << 8u) | flags;
}

struct Stream {
    DriverDetail::Submission submission{};

    Stream() {
        submission.serial = 9;
        submission.queue = 0;
    }
    std::size_t Add(std::initializer_list<std::uint32_t> words) {
        const auto at = submission.commands.size();
        submission.commands.insert(submission.commands.end(), words);
        return at;
    }
    std::size_t SetContext(std::uint32_t offset, std::uint32_t value) { return Add({type3(0x69, 3), offset, value}); }
    std::size_t SetShader(std::uint32_t offset, std::uint32_t value) { return Add({type3(0x76, 3), offset, value}); }
    std::size_t Draw(std::uint32_t count) { return Add({type3(0x2d, 3), count, 2}); }
    std::size_t Dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return Add({type3(0x15, 5), x, y, z, 1}); }
    std::size_t End() { return submission.commands.size(); }
};

std::vector<Operation> decode(Stream& stream, DecodeState& state) {
    Recorded sink(state);
    DecodeSubmission(stream.submission, state, sink);
    return sink.operations;
}

void testDrawsShareSnapshotsUntilWrite() {
    Stream stream;
    stream.SetContext(0x8e, 0xf);
    const auto first = stream.Draw(3);
    stream.Draw(6);
    stream.SetContext(0x8e, 0x3);
    stream.Draw(9);
    DecodeState state(0);
    const auto operations = decode(stream, state);
    Expect(operations.size() == 3, "three draws give three operations");
    if (operations.size() != 3) return;
    Expect(operations[0].kind == OpKind::Draw && operations[0].packetOffset == first && operations[0].value == 3, "a draw carries its packet offset and count");
    Expect(operations[0].snapshots == operations[1].snapshots, "draws without a register write between them share every snapshot");
    Expect(operations[2].snapshots.context == operations[1].snapshots.context + 1, "a context write makes one new context snapshot");
    Expect(operations[2].snapshots.shader == operations[1].snapshots.shader, "a context write leaves the shader snapshot alone");
    const auto* latest = state.Context(operations[2].snapshots.context);
    Expect(latest != nullptr && latest->contains(0x8e) && latest->at(0x8e) == 0x3, "the new context snapshot holds the written value");
    Expect(operations[0].sequence == 1 && operations[1].sequence == 2 && operations[2].sequence == 3, "operations are numbered in queue order");
    Expect(operations[0].submission == 9 && !operations[0].predicated, "operations carry the submission serial and are not predicated");
    Expect(state.Counts().draws == 3, "the draw count matches the packets");
}

void testDispatchGroupsEnterTheShaderSnapshot() {
    Stream stream;
    stream.SetShader(0x212, 0x55);
    stream.Dispatch(4, 2, 1);
    stream.Dispatch(4, 2, 1);
    stream.Dispatch(4, 2, 1);
    stream.Dispatch(8, 1, 1);
    DecodeState state(0x20);
    const auto operations = decode(stream, state);
    Expect(operations.size() == 4, "four dispatches give four operations");
    if (operations.size() != 4) return;
    Expect(operations[0].kind == OpKind::Dispatch && operations[0].value == (4u | (2ull << 32u)) && operations[0].size == 1 && operations[0].queue == 0x20, "a direct dispatch carries its group counts and queue");
    Expect(operations[1].snapshots.shader == operations[0].snapshots.shader + 1, "the first dispatch's group registers dirty the shader bank");
    Expect(operations[2].snapshots.shader == operations[1].snapshots.shader, "a repeated identical dispatch shares the shader snapshot");
    Expect(operations[3].snapshots.shader == operations[2].snapshots.shader, "a dispatch sees the group registers of the previous one");
    const auto* shader = state.Shader(operations[1].snapshots.shader);
    Expect(shader != nullptr && shader->at(0x212) == 0x55 && shader->at(0x201) == 4, "the shader snapshot holds user data and the last group counts");
}

void testWaitsLabelsAndStores() {
    Stream stream;
    stream.Add({type3(0x3c, 7), 0x3, 0x1000, 0x2, 0x7, 0xffffffffu, 0x10});
    stream.Add({type3(0x49, 8), 0x0, 1u << 29u, 0x2000, 0x3, 0x1234, 0x0, 0x0});
    stream.Add({type3(0x37, 5), 5u << 8u, 0x3000, 0x4, 0xabcd});
    stream.Add({type3(0x50, 7), 2u << 29u, 0xdeadbeefu, 0x0, 0x4000, 0x5, 64});
    DecodeState state(0);
    const auto operations = decode(stream, state);
    Expect(operations.size() == 4, "a wait, two labels and a fill give four operations");
    if (operations.size() != 4) return;
    Expect(operations[0].kind == OpKind::Wait && operations[0].address == 0x200001000ull && operations[0].value == 0x7 && operations[0].size == 4, "a WAIT_REG_MEM carries its address, reference and width");
    Expect(operations[1].kind == OpKind::LabelStore && operations[1].address == 0x300002000ull && operations[1].value == 0x1234 && operations[1].size == 4, "a RELEASE_MEM carries its destination and value");
    Expect(operations[2].kind == OpKind::LabelStore && operations[2].address == 0x400003000ull && operations[2].value == 0xabcd, "a WRITE_DATA to memory is a label store");
    Expect(operations[3].kind == OpKind::Fill && operations[3].address == 0x500004000ull && operations[3].size == 64 && operations[3].value == 0xdeadbeefu, "a DMA_DATA from data is a fill");
    Expect(operations[0].snapshots.context == 0, "operations other than draws and dispatches take no snapshot");
    Expect(operations[0].function == 3 && operations[0].mask == 0xffffffffu, "a wait carries its compare function and mask");
}

Operation labelAt(std::uint32_t queue, std::uint64_t sequence, std::uint64_t address, std::uint64_t value) {
    Operation store;
    store.kind = OpKind::LabelStore;
    store.queue = queue;
    store.sequence = sequence;
    store.address = address;
    store.size = 4;
    store.value = value;
    return store;
}

Operation waitFor(std::uint32_t queue, std::uint64_t sequence, std::uint64_t address, std::uint64_t reference) {
    Operation wait;
    wait.kind = OpKind::Wait;
    wait.queue = queue;
    wait.sequence = sequence;
    wait.address = address;
    wait.size = 4;
    wait.value = reference;
    wait.mask = 0xffffffffu;
    wait.function = 3;
    return wait;
}

void testWaitEdgesFollowLabelStores() {
    Order order;
    static_cast<void>(order.Note(labelAt(0x20, 5, 0x1000, 7)));
    auto met = waitFor(0, 1, 0x1000, 7);
    Expect(order.Resolve(met) == Order::Verdict::Edge && met.after.queue == 0x20 && met.after.sequence == 5, "a wait already met by a decoded store depends on that store");
    auto later = waitFor(0, 2, 0x1000, 8);
    Expect(order.Resolve(later) == Order::Verdict::Pending && order.PendingWaits() == 1, "a wait no decoded store meets is pending");
    Expect(order.Counts().crossEdges == 1 && order.Counts().crossPending == 1, "edges and pending waits behind another queue's store are counted");
    Expect(order.Note(labelAt(0, 3, 0x1000, 8)).empty(), "a store from the waiting queue itself does not release the wait");
    const auto released = order.Note(labelAt(0x20, 6, 0x1000, 8));
    Expect(released.size() == 1 && released[0].sequence == 2 && released[0].after.queue == 0x20 && released[0].after.sequence == 6, "a store from another queue releases the pending wait with an edge");
    Operation fill;
    fill.kind = OpKind::Fill;
    fill.queue = 0x20;
    fill.sequence = 7;
    fill.address = 0xff0;
    fill.size = 0x20;
    static_cast<void>(order.Note(fill));
    auto afterFill = waitFor(0, 4, 0x1000, 8);
    Expect(order.Resolve(afterFill) == Order::Verdict::Pending, "a fill over a label forgets its value");
    order.Settled(afterFill);
    Expect(order.PendingWaits() == 0, "a wait settled by polling leaves the pending list");
    auto always = waitFor(0, 5, 0x2000, 0);
    always.function = 0;
    Expect(order.Resolve(always) == Order::Verdict::Passes, "an always-true wait needs no edge");
    auto first = waitFor(0, 6, 0x3000, 1);
    auto second = waitFor(0, 7, 0x3004, 1);
    static_cast<void>(order.Resolve(first));
    static_cast<void>(order.Resolve(second));
    Expect(order.PendingWaits() == 1 && order.Counts().superseded == 1, "a queue's newer pending wait supersedes its older one");
}

void testFrontDecodesQueuesOnTheirOwnThreads() {
    constexpr std::uint32_t Rounds = 300;
    std::mutex seenMutex;
    std::map<std::uint32_t, std::uint64_t> seen;
    std::map<std::uint32_t, std::uint64_t> completed;
    {
        Front front([&](const Operation& operation, const DecodeState&) {
            std::lock_guard lock(seenMutex);
            if (operation.kind == OpKind::SubmissionEnd) ++completed[operation.queue];
            else ++seen[operation.queue];
        });
        for (std::uint32_t round = 0; round < Rounds; ++round) {
            auto compute = std::make_shared<DriverDetail::Submission>();
            compute->serial = round * 2 + 1;
            compute->queue = 0x20;
            compute->commands = {type3(0x15, 5), 1, 1, 1, 1, type3(0x49, 8), 0x0, 1u << 29u, 0x8000, 0x0, round + 1, 0x0, 0x0};
            front.Push(compute);
            auto graphics = std::make_shared<DriverDetail::Submission>();
            graphics->serial = round * 2 + 2;
            graphics->queue = 0;
            graphics->commands = {type3(0x3c, 7), 0x3, 0x8000, 0x0, round + 1, 0xffffffffu, 0x10, type3(0x2d, 3), 3, 2};
            front.Push(graphics);
        }
        const auto totals = front.Totals();
        Expect(totals.size() == 2, "one lane per queue");
        for (const auto& lane : totals) {
            Expect(lane.submissions == Rounds, "every pushed submission is decoded, past the ring depth");
            Expect(lane.counts.operations == Rounds * 2, "each submission gives two operations");
        }
        const auto counts = front.Ordering().Counts();
        Expect(counts.edges + counts.released + counts.superseded + front.Ordering().PendingWaits() == Rounds, "every wait is ordered, released, superseded or still pending");
        Expect(counts.edges + counts.released > 0, "waits across queues find the stores that meet them");
    }
    Expect(seen[0] == Rounds * 2 && seen[0x20] == Rounds * 2, "the consumer sees every operation of both queues");
    Expect(completed[0] == Rounds && completed[0x20] == Rounds, "the consumer sees a submission end for every submission");
}

void testShadowCopyKeepsWhatTheDecoderReads() {
    Stream stream;
    const auto condition = stream.Add({type3(0x22, 5), 0x1000, 0x2, 0x0, 3});
    stream.Draw(3);
    stream.submission.conditionalEnds[condition] = stream.End();
    const auto registers = stream.Add({type3(0x63, 5), 0x0, 0x0, 0x0, 1});
    stream.submission.registerLists[registers] = {0x240, 0x1};
    const auto flip = stream.Add({FlipPacketHeader, 0, 0, 0, 0, 0});
    stream.submission.flips[flip] = nullptr;
    stream.submission.flipHandles[flip] = 3;
    stream.submission.labelWrites = {1, 2};
    const auto copy = Intake::Copy(stream.submission);
    Expect(copy->serial == 9 && copy->queue == 0 && copy->commands == stream.submission.commands, "the copy keeps serial, queue and commands");
    Expect(copy->conditionalEnds == stream.submission.conditionalEnds && copy->registerLists == stream.submission.registerLists && copy->flipHandles == stream.submission.flipHandles, "the copy keeps conditional ends, register lists and flip handles");
    Expect(copy->flips.size() == 1 && copy->flips.begin()->first == flip && copy->flips.begin()->second == nullptr, "the copy marks flips without holding the flip request");
    Expect(copy->labelWrites.empty(), "the copy leaves out the worker's bookkeeping");
}

void testReplayMatchesThePacketAnalysis() {
    const auto directory = std::filesystem::temp_directory_path() / "aps5-unified-replay";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> planned;
    {
        Stream graphics;
        graphics.SetContext(0x8e, 0xf);
        graphics.Draw(3);
        const auto condition = graphics.Add({type3(0x22, 5), 0x1000, 0x2, 0x0, 3});
        graphics.Draw(6);
        graphics.Add({type3(0x49, 8), 0x0, 1u << 29u, 0x9000, 0x0, 1, 0x0, 0x0});
        planned.emplace_back(0, graphics.submission.commands);
        SubmissionCapture capture(directory);
        auto first = SubmissionCapture::Prepare(0, SubmissionKind::Submit, graphics.submission.commands, {{condition, condition + 8}});
        capture.Take(first, 1, 1000, {});
        capture.Write(first);
        Stream compute;
        compute.Add({type3(0x3c, 7), 0x13, 0x9000, 0x0, 1, 0xffffffffu, 0x10});
        compute.Dispatch(2, 2, 1);
        compute.Dispatch(4, 1, 1);
        auto second = SubmissionCapture::Prepare(0x20, SubmissionKind::Submit, compute.submission.commands, {});
        capture.Take(second, 2, 2000, {});
        capture.Write(second);
    }
    std::uint64_t analysedDraws = 0, analysedDispatches = 0;
    for (const auto& captured : SubmissionCapture::Read(directory)) {
        for (const auto& packet : Pm4Analysis::Decode(captured)) {
            const auto opcode = (packet.header >> 8u) & 0xffu;
            if (Pm4::DrawOpcode(opcode)) ++analysedDraws;
            if (opcode == 0x15 || opcode == 0x16) ++analysedDispatches;
        }
    }
    std::uint64_t draws = 0, dispatches = 0, conditionalDraws = 0;
    {
        Front front;
        for (const auto& lane : ReplayCapture(directory, front)) {
            draws += lane.counts.draws;
            dispatches += lane.counts.dispatches;
            conditionalDraws += lane.counts.conditionalDraws;
        }
        const auto order = front.Ordering().Counts();
        Expect(order.edges + order.released == 1, "the replayed cross-queue wait finds its release");
    }
    Expect(analysedDraws == 2 && analysedDispatches == 2, "the packet analysis sees two draws and two dispatches");
    Expect(draws == analysedDraws && dispatches == analysedDispatches, "the replayed decode counts match the packet analysis");
    Expect(conditionalDraws == 1, "the captured conditional range marks the guarded draw");
    std::filesystem::remove_all(directory);
}

void testResolveFillsDrawParametersAndKeys() {
    Stream stream;
    stream.Add({type3(0x2f, 2), 4});
    stream.Draw(12);
    stream.Draw(12);
    stream.SetContext(0x8e, 0x3);
    stream.Draw(12);
    DecodeState state(0);
    state.SetResolve(true);
    const auto operations = decode(stream, state);
    Expect(operations.size() == 3, "three resolved draws");
    if (operations.size() != 3) return;
    Expect(state.Counts().resolved == 3 && state.Counts().resolveErrors == 0, "every draw resolves");
    Expect(operations[0].resolved && operations[0].size == 12 && (operations[0].value & 0xffffffffu) == 4, "a resolved auto draw carries its vertex count and instance count");
    Expect(!operations[0].draw.indexed && !operations[0].draw.indirect && operations[0].draw.indexCount == 12 && operations[0].draw.instanceCount == 4, "a resolved auto draw keeps its draw facts");
    Expect(operations[0].key == operations[1].key, "draws without a register write between them share a key");
    Expect(operations[2].key != operations[1].key, "a context write changes the key");
    DecodeState plain(0);
    const auto unresolved = decode(stream, plain);
    Expect(!unresolved.empty() && !unresolved[0].resolved && plain.Counts().resolved == 0, "without resolve nothing is resolved");
}

void testUsesReportIndexAndIndirectRanges() {
    std::vector<std::uint16_t> indices(64, 0);
    std::vector<std::uint32_t> arguments(4, 1);
    const auto indexAddress = reinterpret_cast<std::uint64_t>(indices.data());
    const auto argumentAddress = reinterpret_cast<std::uint64_t>(arguments.data());
    Stream stream;
    stream.Add({type3(0x27, 6), 64, static_cast<std::uint32_t>(indexAddress), static_cast<std::uint32_t>(indexAddress >> 32u), 48, 0});
    stream.Add({type3(0x16, 4), static_cast<std::uint32_t>(argumentAddress), static_cast<std::uint32_t>(argumentAddress >> 32u), 1});
    DecodeState state(0);
    state.SetResolve(true);
    std::vector<GuestUse> seen;
    state.SetUses([&seen](std::span<const GuestUse> uses) { seen.insert(seen.end(), uses.begin(), uses.end()); });
    decode(stream, state);
    const GuestUse index{indexAddress, 96, false};
    const GuestUse dispatch{argumentAddress, 12, false};
    Expect(state.Counts().resolveErrors == 0, "the indexed draw resolves");
    Expect(std::find(seen.begin(), seen.end(), index) != seen.end(), "an indexed draw reports its index range");
    Expect(std::find(seen.begin(), seen.end(), dispatch) != seen.end(), "an indirect dispatch reports its argument range");
    Expect(state.Counts().useReports == 2, "one report per operation");
}

void testRebuiltStateResolvesLikeTheDecoder() {
    std::vector<std::uint32_t> indices(256, 0);
    const auto base = reinterpret_cast<std::uint64_t>(indices.data());
    const auto second = base + 256;
    Stream stream;
    stream.Add({type3(0x26, 3), static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base >> 32u)});
    stream.Add({type3(0x2f, 2), 3});
    stream.Add({type3(0x35, 5), 64, 4, 16, 0});
    stream.SetContext(0x8e, 0x7);
    stream.Add({type3(0x2a, 2), 1});
    stream.Add({type3(0x35, 5), 64, 8, 32, 0});
    stream.Add({type3(0x26, 3), static_cast<std::uint32_t>(second), static_cast<std::uint32_t>(second >> 32u)});
    stream.Add({type3(0x2f, 2), 1});
    stream.Add({type3(0x35, 5), 64, 0, 12, 0});
    DecodeState state(0);
    state.SetResolve(true);
    Recorded sink(state);
    sink.retire = false;
    DecodeSubmission(stream.submission, state, sink);
    Expect(sink.operations.size() == 3 && state.Counts().resolved == 3, "three indexed draws resolve");
    for (const auto& operation : sink.operations) Expect(operation.draw.indexed && operation.draw.indexSize != 0u && operation.draw.indexAddress == operation.address, "a resolved indexed draw keeps its index facts");
    int matched = 0;
    for (const auto& operation : sink.operations) {
        const auto queue = Record::QueueStateAt(state, operation.snapshots);
        if (!queue) continue;
        const auto packet = std::span<const std::uint32_t>(stream.submission.commands).subspan(operation.packetOffset, 5);
        const auto draw = Pm4::ResolveDraw(packet, *queue);
        const bool same = draw.indexAddress == operation.address && draw.indexCount == operation.size && draw.instanceCount == (operation.value & 0xffffffffu) && draw.indexSize == (operation.value >> 32u);
        if (same) ++matched;
    }
    Expect(matched == 3, "the state rebuilt from each draw's snapshots resolves the draw as the decoder did");
    const auto* context = state.Context(sink.operations.back().snapshots.context);
    const auto rebuilt = Record::QueueStateAt(state, sink.operations.back().snapshots);
    Expect(rebuilt && context != nullptr && rebuilt->context.at(0x8e) == 0x7, "the rebuilt state carries the context snapshot");
}

void testVerifierPairsDecodedAndDriverDraws() {
    Stream stream;
    stream.Draw(3);
    stream.SetContext(0x8e, 0x5);
    stream.Draw(6);
    stream.Draw(9);
    DecodeState state(0);
    state.SetResolve(true);
    Recorded sink(state);
    sink.retire = false;
    DecodeSubmission(stream.submission, state, sink);
    Expect(sink.operations.size() == 3 && state.Counts().resolved == 3, "three resolved draws to verify");
    if (sink.operations.size() != 3) return;
    Record::Verifier verifier;
    const auto& first = sink.operations[0];
    const auto& second = sink.operations[1];
    const auto& third = sink.operations[2];
    verifier.NoteDecoded(first, state);
    verifier.NoteDriverKey(first.key, 0, first.submission, first.packetOffset);
    verifier.NoteDriverKey(second.key, 0, second.submission, second.packetOffset);
    verifier.NoteDecoded(second, state);
    verifier.NoteDecoded(third, state);
    verifier.NoteDriverKey(third.key ^ 1u, 0, third.submission, third.packetOffset);
    const auto counts = verifier.Counts();
    Expect(counts.matched == 2, "equal keys match whichever side arrives first");
    Expect(counts.mismatched == 1, "a different driver key is a mismatch");
    Expect(counts.bridgeMismatched == 0 && counts.bridgeMissing == 0, "the rebuilt state gives the decoder's key at every draw");
    Expect(counts.pending == 0, "every pair met");
}

void testSuspendResetsTheGraphicsState() {
    const auto run = [](bool suspend) {
        std::mutex seenMutex;
        std::vector<bool> written;
        {
            Front front([&](const Operation& operation, const DecodeState& state) {
                if (operation.kind != OpKind::Draw) return;
                const auto rebuilt = Record::QueueStateAt(state, operation.snapshots);
                std::lock_guard lock(seenMutex);
                written.push_back(rebuilt && rebuilt->shader.contains(0x212));
            });
            auto first = std::make_shared<DriverDetail::Submission>();
            first->serial = 1;
            first->commands = {type3(0x76, 3), 0x212, 0x55, type3(0x2d, 3), 3, 2};
            front.Push(first);
            if (suspend) front.NoteSuspend();
            auto second = std::make_shared<DriverDetail::Submission>();
            second->serial = 2;
            second->commands = {type3(0x2d, 3), 3, 2};
            front.Push(second);
            const auto totals = front.Totals();
            Expect(totals.size() == 1 && totals[0].counts.resets == (suspend ? 1u : 0u), "a suspend resets the queue 0 lane once");
        }
        return written;
    };
    const auto kept = run(false);
    const auto reset = run(true);
    Expect(kept.size() == 2 && kept[0] && kept[1], "without a suspend the shader write carries into the next submission");
    Expect(reset.size() == 2 && reset[0] && !reset[1], "after a suspend the next queue 0 submission starts from a fresh state, as the driver's does");
}

void testHandoffGivesEachDrawItsDecodedState() {
    Stream stream;
    stream.SetContext(0x8e, 0x3);
    stream.Draw(3);
    stream.SetContext(0x8e, 0x9);
    stream.Draw(6);
    DecodeState state(0);
    Recorded sink(state);
    sink.retire = false;
    DecodeSubmission(stream.submission, state, sink);
    if (sink.operations.size() != 2) {
        Expect(false, "two draws to hand off");
        return;
    }
    Record::Handoff handoff;
    for (const auto& operation : sink.operations) handoff.Offer(operation, state);
    const auto second = handoff.Take(0, sink.operations[1].submission, sink.operations[1].packetOffset, std::chrono::milliseconds(0));
    const auto first = handoff.Take(0, sink.operations[0].submission, sink.operations[0].packetOffset, std::chrono::milliseconds(0));
    Expect(first != nullptr && first->context.at(0x8e) == 0x3, "the first draw gets the state decoded at it");
    Expect(second != nullptr && second->context.at(0x8e) == 0x9, "the second draw gets its own state, taken in any order");
    Expect(handoff.Take(0, sink.operations[0].submission, sink.operations[0].packetOffset, std::chrono::milliseconds(1)) == nullptr, "a state is taken once");
    const auto counts = handoff.Counts();
    Expect(counts.offered == 2 && counts.taken == 2 && counts.missed == 1 && counts.pending == 0, "handoff counts");
}

void testConditionalRangesArePredicated() {
    Stream stream;
    const auto condition = stream.Add({type3(0x22, 5), 0x1000, 0x2, 0x0, 3});
    stream.Draw(3);
    const auto end = stream.End();
    stream.submission.conditionalEnds[condition] = end;
    stream.Draw(4);
    DecodeState state(0);
    const auto operations = decode(stream, state);
    Expect(operations.size() == 2, "both draws are emitted");
    if (operations.size() != 2) return;
    Expect(operations[0].predicated, "a draw inside COND_EXEC is predicated");
    Expect(!operations[1].predicated, "a draw after the conditional range is not");
    Expect(state.Counts().conditional == 1 && state.Counts().conditionalDraws == 1 && state.Counts().conditionalDispatches == 0, "the conditional counts match");
}

void testFlipsAndIndirectRegisters() {
    Stream stream;
    const auto registers = stream.Add({type3(0x63, 5), 0x0, 0x0, 0x0, 2});
    stream.submission.registerLists[registers] = {0x240, 0x77, 0x241, 0x88};
    stream.Dispatch(1, 1, 1);
    const auto flip = stream.Add({FlipPacketHeader, 0, 0, 0, 0, 0});
    stream.submission.flips[flip] = nullptr;
    stream.submission.flipHandles[flip] = 7;
    DecodeState state(0);
    const auto operations = decode(stream, state);
    Expect(operations.size() == 2, "a dispatch and a flip give two operations");
    if (operations.size() != 2) return;
    const auto* shader = state.Shader(operations[0].snapshots.shader);
    Expect(shader != nullptr && shader->contains(0x240) && shader->at(0x240) == 0x77 && shader->at(0x241) == 0x88, "an indirect SH register list reaches the shader snapshot");
    Expect(operations[1].kind == OpKind::Flip && operations[1].value == 7 && operations[1].packetOffset == flip, "a flip carries its handle and offset");
    Expect(state.Counts().flips == 1 && state.Counts().stateFromMemory == 0, "flip counted, no state read from memory");
}

void testFullRingAsksForRoom() {
    Stream stream;
    for (std::uint32_t i = 0; i < SnapshotSlots + 4; ++i) {
        stream.SetContext(0x8e, i);
        stream.Draw(3);
    }
    DecodeState state(0);
    Recorded sink(state);
    sink.retire = false;
    struct Retiring : Sink {
        Recorded& inner;
        explicit Retiring(Recorded& recorded) : inner(recorded) {}
        void Emit(const Operation& operation) override { inner.Emit(operation); }
        void MakeRoom() override {
            ++inner.roomRequests;
            inner.state.RetireBefore(inner.operations.back().snapshots);
        }
    } retiring(sink);
    DecodeSubmission(stream.submission, state, retiring);
    Expect(sink.operations.size() == SnapshotSlots + 4, "every draw is emitted when the ring fills");
    Expect(sink.roomRequests > 0 && state.Counts().snapshotStalls > 0, "a full snapshot ring asks the consumer for room");
    const auto* last = state.Context(sink.operations.back().snapshots.context);
    Expect(last != nullptr && last->at(0x8e) == SnapshotSlots + 3, "the last snapshot holds the last write");
}

}

int main() {
    testDrawsShareSnapshotsUntilWrite();
    testDispatchGroupsEnterTheShaderSnapshot();
    testWaitsLabelsAndStores();
    testConditionalRangesArePredicated();
    testFlipsAndIndirectRegisters();
    testFullRingAsksForRoom();
    testWaitEdgesFollowLabelStores();
    testFrontDecodesQueuesOnTheirOwnThreads();
    testShadowCopyKeepsWhatTheDecoderReads();
    testReplayMatchesThePacketAnalysis();
    testResolveFillsDrawParametersAndKeys();
    testUsesReportIndexAndIndirectRanges();
    testRebuiltStateResolvesLikeTheDecoder();
    testVerifierPairsDecodedAndDriverDraws();
    testSuspendResetsTheGraphicsState();
    testHandoffGivesEachDrawItsDecodedState();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified decoder tests passed\n");
    return 0;
}
