#include "prx/libSceAgcDriver/Unified/include/ProofVerdict.hpp"
#include <cstdint>
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

constexpr std::uint64_t Buffer = 0x200000000ull;
constexpr std::uint64_t Image = 0x210000000ull;
constexpr std::uintptr_t ImageOwner = 0x4000;

std::vector<Proof::Span> spans() {
    return {{Buffer, Buffer + 0x1000, 0}, {Image, Image + 0x100000, ImageOwner}};
}

Proof::Guards guards(std::uint64_t first = 1) {
    return {{first, 2, 3, 4, 5}};
}

void testNothingChangedProves() {
    Events::ChangeLog log;
    const Proof::Stamp stamp{log.Serial(), guards(), true};
    log.Note(Buffer + 0x100000, 0x10);
    Expect(Proof::Verdict(log, stamp, guards(), false, true, spans()) == Proof::Miss::None, "a write outside every span keeps the proof");
}

void testEachRefusal() {
    Events::ChangeLog log;
    const auto ranges = spans();
    Expect(Proof::Verdict(log, {}, guards(), false, true, ranges) == Proof::Miss::NoStamp, "no earlier proof, no verdict");
    const Proof::Stamp stamp{log.Serial(), guards(), true};
    Expect(Proof::Verdict(log, stamp, guards(), false, false, ranges) == Proof::Miss::Unregistered, "a range outside the memory core refuses");
    Expect(Proof::Verdict(log, stamp, guards(9), false, true, ranges) == Proof::Miss::Guard, "a moved guard serial refuses");
    Expect(Proof::Verdict(log, stamp, guards(), true, true, ranges) == Proof::Miss::CpuWrites, "pending CPU writes refuse");
    log.Note(Buffer + 0x800, 4);
    Expect(Proof::Verdict(log, stamp, guards(), false, true, ranges) == Proof::Miss::Range, "a write into a buffer span refuses");
}

void testOwnImageWritesKeepTheProof() {
    Events::ChangeLog log;
    const Proof::Stamp stamp{log.Serial(), guards(), true};
    log.Note(Image, 0x100000, ImageOwner);
    Expect(Proof::Verdict(log, stamp, guards(), false, true, spans()) == Proof::Miss::None, "the image's own pending results do not refuse");
    log.Note(Image + 0x2000, 0x10, 0x9999);
    Expect(Proof::Verdict(log, stamp, guards(), false, true, spans()) == Proof::Miss::Range, "another image's results over the span refuse");
}

void testTally() {
    Proof::Tally tally;
    tally.Count(Proof::Miss::None, true);
    tally.Count(Proof::Miss::Range, true);
    tally.Count(Proof::Miss::Range, false);
    tally.Count(Proof::Miss::Guard, true);
    const auto counts = tally.Counts();
    Expect(counts.agreed == 1 && counts.unsafe == 0 && counts.bothRefused == 1, "outcomes are counted");
    Expect(counts.opportunities[static_cast<std::size_t>(Proof::Miss::Range)] == 1 && counts.opportunities[static_cast<std::size_t>(Proof::Miss::Guard)] == 1, "refusals of a proved object are counted by reason");
    tally.Count(Proof::Miss::None, false);
    Expect(tally.Counts().unsafe == 1, "a verdict the existing proof refused is unsafe");
}

}

int main() {
    testNothingChangedProves();
    testEachRefusal();
    testOwnImageWritesKeepTheProof();
    testTally();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified proof verdict tests passed\n");
    return 0;
}
