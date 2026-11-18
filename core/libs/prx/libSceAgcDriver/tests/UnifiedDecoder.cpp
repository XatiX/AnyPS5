#include "prx/libSceAgcDriver/Unified/include/Decoder.hpp"
#include "prx/libSceAgcDriver/Unified/include/Order.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include <cstdint>
#include <cstdio>
#include <initializer_list>
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
    Expect(state.Counts().conditional == 1, "the conditional count matches");
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
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified decoder tests passed\n");
    return 0;
}
