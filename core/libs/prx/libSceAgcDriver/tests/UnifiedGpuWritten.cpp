#include "prx/libSceAgcDriver/Unified/include/GpuWritten.hpp"
#include <cstdio>
#include <vector>

using namespace AgcDriver::Unified;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void testNotedBytesArePending() {
    GpuWritten written;
    written.Note(0x1000, 0x40, 7);
    Expect(written.Any(0x1000, 1) && written.Any(0x103f, 1) && !written.Any(0x1040, 0x100) && !written.Any(0xfc0, 0x40), "only the noted bytes are GPU-written");
    Expect(written.Pending(0x1000, 0x1000) == 7 && written.Pending(0x2000, 0x1000) == 0, "a range reports the newest batch that wrote into it");
    const auto counts = written.Counts();
    Expect(counts.pieces == 1 && counts.bytes == 0x40 && counts.noted == 1, "the held bytes are counted");
}

void testTakeWaitsForTheWritingBatch() {
    GpuWritten written;
    written.Note(0x1000, 0x100, 5);
    written.Note(0x1200, 0x100, 9);
    Expect(written.Take(0x1000, 0x1000, 4).empty(), "bytes of a batch that has not completed are not taken");
    const auto first = written.Take(0x1000, 0x1000, 5);
    Expect(first.size() == 1 && first[0] == WrittenRange{0x1000, 0x1100, 5}, "bytes of a completed batch are taken");
    Expect(!written.Any(0x1000, 0x100) && written.Any(0x1200, 0x100), "taken bytes leave the set, the rest stays");
    const auto second = written.Take(0x1000, 0x1000, 9);
    Expect(second.size() == 1 && second[0] == WrittenRange{0x1200, 0x1300, 9} && written.Counts().bytes == 0, "the later batch is taken once it completes");
}

void testTakeClipsToItsWindow() {
    GpuWritten written;
    written.Note(0x1000, 0x3000, 3);
    const auto middle = written.Take(0x2000, 0x1000, 3);
    Expect(middle.size() == 1 && middle[0] == WrittenRange{0x2000, 0x3000, 3}, "a take returns the part inside its window");
    const auto rest = written.In(0, 0x10000);
    Expect(rest.size() == 2 && rest[0] == WrittenRange{0x1000, 0x2000, 3} && rest[1] == WrittenRange{0x3000, 0x4000, 3}, "the parts outside the window stay");
    Expect(written.Counts().bytes == 0x2000, "held bytes follow the split");
}

void testLaterWriteSupersedesEarlier() {
    GpuWritten written;
    written.Note(0x1000, 0x100, 2);
    written.Note(0x1080, 0x100, 6);
    const auto pieces = written.In(0x1000, 0x1000);
    Expect(pieces.size() == 2 && pieces[0] == WrittenRange{0x1000, 0x1080, 2} && pieces[1] == WrittenRange{0x1080, 0x1180, 6}, "overlapped bytes carry the later batch");
    Expect(written.Counts().bytes == 0x180, "overlapping notes do not count bytes twice");
    const auto early = written.Take(0x1000, 0x1000, 2);
    Expect(early.size() == 1 && early[0].end == 0x1080, "only the bytes whose last writer completed are taken");
}

void testAdjacentNotesOfOneBatchMerge() {
    GpuWritten written;
    written.Note(0x1000, 0x40, 4);
    written.Note(0x1040, 0x40, 4);
    written.Note(0x0fc0, 0x40, 4);
    written.Note(0x1080, 0x40, 5);
    Expect(written.Counts().pieces == 2, "adjacent ranges of one batch are one piece, another batch stays apart");
    const auto pieces = written.In(0, 0x10000);
    Expect(pieces.size() == 2 && pieces[0] == WrittenRange{0x0fc0, 0x1080, 4}, "the merged piece spans the three notes");
}

void testDiscardDropsBytesTheCpuOverwrote() {
    GpuWritten written;
    written.Note(0x1000, 0x100, 8);
    Expect(written.Discard(0x1040, 0x20) == 0x20, "a discard reports the bytes it dropped");
    const auto pieces = written.In(0x1000, 0x100);
    Expect(pieces.size() == 2 && pieces[0].end == 0x1040 && pieces[1].begin == 0x1060, "the discarded bytes are no longer GPU-written");
    Expect(written.Discard(0x2000, 0x100) == 0 && written.Counts().discardedBytes == 0x20, "a discard outside the set changes nothing");
}

}

int main() {
    testNotedBytesArePending();
    testTakeWaitsForTheWritingBatch();
    testTakeClipsToItsWindow();
    testLaterWriteSupersedesEarlier();
    testAdjacentNotesOfOneBatchMerge();
    testDiscardDropsBytesTheCpuOverwrote();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified GPU-written tests passed\n");
    return 0;
}
