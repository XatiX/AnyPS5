#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

using namespace AgcDriver::Unified::Events;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

constexpr std::uint64_t Base = 0x200000000ull;

void testWriteMovesItsBucketOnly() {
    ChangeLog log;
    const auto stamp = log.Serial();
    log.Note(Base + 0x100, 0x10);
    Expect(log.ChangedSince(Base, BucketBytes, stamp), "a write is seen over its own bucket");
    Expect(!log.ChangedSince(Base + BucketBytes, BucketBytes, stamp), "a neighbouring bucket in the same region is unchanged");
    Expect(!log.ChangedSince(Base + RegionBytes, BucketBytes, stamp), "another region is skipped as unchanged");
    Expect(!log.ChangedSince(Base, BucketBytes, log.Serial()), "a stamp taken after the write sees no change");
    Expect(log.Counts().regionSkips >= 1, "a quiet region is answered from its region epoch");
}

void testWriteSpanningBuckets() {
    ChangeLog log;
    const auto stamp = log.Serial();
    log.Note(Base + BucketBytes - 4, 8);
    Expect(log.ChangedSince(Base + BucketBytes, 4, stamp), "a write crossing into the next bucket moves it too");
    Expect(log.ChangedSince(Base + BucketBytes - 8, 4, stamp), "and the bucket it starts in");
}

void testOwnerRule() {
    ChangeLog log;
    const std::uintptr_t image = 0x1234;
    const std::uintptr_t other = 0x5678;
    const auto stamp = log.Serial();
    log.Note(Base, 0x1000, image);
    Expect(!log.ChangedSince(Base, 0x1000, stamp, image), "an owner's own writes do not invalidate it");
    Expect(log.ChangedSince(Base, 0x1000, stamp), "a reader without an owner sees the write");
    Expect(log.ChangedSince(Base, 0x1000, stamp, other), "another owner sees the write");
    log.Note(Base, 0x1000, other);
    log.Note(Base, 0x1000, image);
    Expect(log.ChangedSince(Base, 0x1000, stamp, image), "a foreign write between the owner's writes is not hidden by the owner rule");
    const auto later = log.Serial();
    log.Note(Base, 0x1000, image);
    Expect(!log.ChangedSince(Base, 0x1000, later, image), "after the foreign write is older than the stamp, the owner is exempt again");
    log.Note(Base, 0x1000);
    Expect(log.ChangedSince(Base, 0x1000, later, image), "a CPU write is never exempt for an image");
}

void testAliasingIsConservative() {
    ChangeLog log;
    const auto stamp = log.Serial();
    const auto alias = Base + RegionBytes * RegionCount;
    log.Note(alias, 0x10);
    Expect(log.ChangedSince(alias, 0x10, stamp), "the written address itself is changed");
    Expect(log.ChangedSince(Base, 0x10, stamp), "an address sharing the written bucket and region reads as changed, never as unchanged");
    Expect(!log.ChangedSince(alias + BucketBytes, 0x10, stamp), "another bucket of the written region is unchanged");
    Expect(log.ChangedSince(Base, BucketBytes * (BucketCount + 1), stamp), "a range wider than the table is changed whenever anything moved");
}

void testConcurrentWritersAndReaders() {
    ChangeLog log;
    const auto stamp = log.Serial();
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> answers{0};
    std::vector<std::thread> writers;
    for (int writer = 0; writer < 4; ++writer) {
        writers.emplace_back([&, writer] {
            for (int round = 0; round < 20000; ++round) log.Note(Base + static_cast<std::uint64_t>(writer) * BucketBytes, 0x100, static_cast<std::uintptr_t>(writer + 1));
        });
    }
    std::thread reader([&] {
        while (!stop.load()) {
            static_cast<void>(log.ChangedSince(Base, BucketBytes * 4, stamp));
            answers.fetch_add(1);
        }
    });
    for (auto& writer : writers) writer.join();
    stop.store(true);
    reader.join();
    Expect(log.Serial() == stamp + 80000, "every note takes one serial");
    Expect(log.ChangedSince(Base, BucketBytes * 4, stamp), "all writes are visible after the writers finish");
    Expect(answers.load() > 0, "a reader runs beside the writers without blocking");
    Expect(!log.ChangedSince(Base, BucketBytes * 4, log.Serial()), "a stamp after the last write sees no change");
}

}

int main() {
    testWriteMovesItsBucketOnly();
    testWriteSpanningBuckets();
    testOwnerRule();
    testAliasingIsConservative();
    testConcurrentWritersAndReaders();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified events tests passed\n");
    return 0;
}
