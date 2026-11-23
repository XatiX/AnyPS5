#include "prx/libSceAgcDriver/Unified/include/DepthRecords.hpp"
#include "prx/libSceAgcDriver/Unified/include/HtileClears.hpp"
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace AgcDriver::Unified::Images;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

HtileState read(const std::vector<std::uint32_t>& words, bool depthOnly, bool stencil, float clearDepth) {
    return ReadHtile(reinterpret_cast<std::uint64_t>(words.data()), words.size() * 4u, depthOnly, stencil, clearDepth);
}

void testZeroWordsProveAClearToZero() {
    const std::vector<std::uint32_t> words(1024, 0u);
    Expect(read(words, false, true, 0.0f) == HtileState::Cleared, "zero HTILE words prove a clear to depth 0");
    Expect(read(words, false, true, 1.0f) == HtileState::Written, "zero HTILE words do not prove a clear to depth 1");
}

void testFullRangeWordsProveAClearToOne() {
    const std::vector<std::uint32_t> words(1024, 0xfffc0000u);
    Expect(read(words, false, false, 1.0f) == HtileState::Cleared, "0xfffc0000 proves a clear to depth 1");
    const std::vector<std::uint32_t> depthOnly(1024, 0xfffffff0u);
    Expect(read(depthOnly, true, false, 1.0f) == HtileState::Cleared, "a depth-only 0xfffffff0 proves a clear to depth 1");
}

void testOneExpandedTileMeansWritten() {
    std::vector<std::uint32_t> words(1024, 0u);
    words[700] = 0x0000000fu;
    Expect(read(words, false, false, 0.0f) == HtileState::Written, "one expanded tile means the plane holds drawn depth");
    words[700] = 0x00012340u;
    Expect(read(words, false, false, 0.0f) == HtileState::Written, "one compressed tile means the plane holds drawn depth");
}

void testStencilBitsCountOnlyWithStencil() {
    const std::vector<std::uint32_t> words(1024, 0x00000100u);
    Expect(read(words, false, true, 0.0f) == HtileState::Written, "stencil not cleared when the record has stencil");
    Expect(read(words, false, false, 0.0f) == HtileState::Cleared, "stencil bits ignored without stencil");
}

void testNoAddressIsUnknown() {
    Expect(ReadHtile(0u, 4096u, false, false, 0.0f) == HtileState::Unknown, "no HTILE address is unknown");
    const std::vector<std::uint32_t> words(1, 0u);
    Expect(ReadHtile(reinterpret_cast<std::uint64_t>(words.data()), 2u, false, false, 0.0f) == HtileState::Unknown, "fewer than four bytes is unknown");
}

void testMetadataSizeFollowsBlocks() {
    Expect(HtileBlockBytes(1920u, 1080u) == 0x30000u, "a 1920x1080 target has six HTILE blocks");
    Expect(HtileBlockBytes(256u, 256u) == 0x8000u, "a 256x256 target has one HTILE block");
    Expect(HtileBlockBytes(3840u, 2160u) == 0xa0000u, "a 3840x2160 target has twenty HTILE blocks");
}

void testExpandedWordsNeverProveAClear() {
    for (const bool stencilInTiles : {false, true}) {
        const std::vector<std::uint32_t> words(1024, ExpandedHtileWord(stencilInTiles));
        Expect((words[0] & 15u) == 15u, "an expanded word has every sample mask bit set");
        Expect(read(words, false, stencilInTiles, 0.0f) == HtileState::Written, "expanded words are no clear to depth 0");
        Expect(read(words, false, stencilInTiles, 1.0f) == HtileState::Written, "expanded words are no clear to depth 1");
    }
}

}

int main() {
    testMetadataSizeFollowsBlocks();
    testExpandedWordsNeverProveAClear();
    testZeroWordsProveAClearToZero();
    testFullRangeWordsProveAClearToOne();
    testOneExpandedTileMeansWritten();
    testStencilBitsCountOnlyWithStencil();
    testNoAddressIsUnknown();
    if (failures != 0) return 1;
    std::puts("agc_unified_depth_htile_tests passed");
    return 0;
}
