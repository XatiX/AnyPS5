#include "prx/libSceAgcDriver/Unified/include/Downgrades.hpp"
#include <array>
#include <cstdio>
#include <string>

using namespace AgcDriver::Unified::Downgrades;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void testHashKeepsEarlierNames() {
    Expect(HashCode({}) == 1469598103934665603ull, "empty code hashes to the basis earlier program names used");
    const std::array<std::byte, 1> a{std::byte{'a'}};
    Expect(HashCode(a) == 0x44bd8ad473cd9906ull, "one byte hashes with the FNV-1a prime");
}

void testFirstOccurrence() {
    Ledger ledger;
    Expect(ledger.Note(1u, Kind::KnownRefused, 2u, "first"), "first note of a binding is reported");
    Expect(!ledger.Note(1u, Kind::KnownRefused, 2u, "second"), "repeat note is not reported again");
    Expect(ledger.Note(1u, Kind::KnownRefused, 8u, {}), "another binding is a first occurrence");
    const auto rows = ledger.Rows();
    Expect(rows.size() == 1u && rows.front().counts.at({Kind::KnownRefused, 2u}) == 2u, "counts accumulate per binding");
    Expect(rows.front().details.at({Kind::KnownRefused, 2u}) == "first", "the first detail is kept");
}

void testWorkAndGroups() {
    Ledger ledger;
    ledger.NoteWork(5u, Work::IndirectDispatch, 0u);
    ledger.NoteWork(5u, Work::IndirectDispatch, 0u);
    ledger.NoteGroups(5u, 17424u);
    ledger.NoteWork(5u, Work::Dispatch, 6u);
    ledger.Note(5u, Kind::KnownPrefix, 9u, {});
    const auto rows = ledger.Rows();
    Expect(rows.size() == 1u, "a program with work and a downgrade is listed");
    Expect(rows.front().dispatches == 3u && rows.front().indirect == 2u, "dispatches and indirect dispatches are counted");
    Expect(rows.front().groups == 17430u && rows.front().groupsRead == 2u, "direct and read-back groups add up");
    ledger.NoteWork(6u, Work::Draw, 0u);
    Expect(ledger.Rows().size() == 1u, "programs without downgrades are left out");
}

void testSevereRowsComeFirst() {
    Ledger ledger;
    for (int count = 0; count < 5; ++count) ledger.Note(1u, Kind::ExistingPath, 0u, {});
    ledger.Note(2u, Kind::Dropped, 0u, "ring full");
    ledger.Note(3u, Kind::KnownNull, 0u, {});
    const auto rows = ledger.Rows();
    Expect(rows.size() == 3u && rows[0].hash == 2u && rows[1].hash == 1u && rows[2].hash == 3u, "rows sort severe, then fallback to the existing path, then warnings");
    Expect(SeverityOf(Kind::UpstreamTarget) == Severity::Fallback && SeverityOf(Kind::ExistingPath) == Severity::Fallback, "existing-path pieces are fallbacks");
    Expect(Ledger::Format(rows[1]).find(" fallback ") != std::string::npos, "a fallback row is labelled fallback");
    const auto line = Ledger::Format(rows[0]);
    Expect(line.find("0000000000000002 severe") == 0u, "a row starts with the hash and its severity");
    Expect(line.find("dropped[0] 1 {ring full}") != std::string::npos, "a row lists each kind, binding, count and first detail");
    Expect(ledger.Table(2u).size() == 2u, "the table honours its row limit");
}

void testListedHash() {
    const auto hash = 0xb1b7bb2506bdcb92ull;
    Expect(ListedHash("b1b7bb2506bdcb92", hash), "a full hash is listed");
    Expect(ListedHash("a26bc518824f851f,b1b7", hash), "a prefix of four or more digits is listed");
    Expect(ListedHash("e377; b1b7bb25", hash), "entries may be separated by semicolons and spaces");
    Expect(!ListedHash("b1b", hash), "prefixes shorter than four digits are ignored");
    Expect(!ListedHash("a26bc518824f851f", hash), "another hash is not listed");
    Expect(!ListedHash("", hash), "an empty list lists nothing");
}

}

int main() {
    testListedHash();
    testHashKeepsEarlierNames();
    testFirstOccurrence();
    testWorkAndGroups();
    testSevereRowsComeFirst();
    if (failures != 0) return 1;
    std::printf("unified downgrades tests passed\n");
    return 0;
}
