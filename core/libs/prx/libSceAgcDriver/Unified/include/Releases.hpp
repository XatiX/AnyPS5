#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RELEASES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RELEASES_HPP

#include <cstdint>
#include <functional>
#include <string>

namespace AgcDriver::Unified::Releases {

struct Counts {
    std::uint64_t queued = 0;
    std::uint64_t released = 0;
    std::uint64_t direct = 0;
    std::uint64_t labelsBehindLandings = 0;
};

[[nodiscard]] bool FlipsFollowWork();
void Queue(std::function<void()> release);
void NoteDirect();
void NoteLabelBehindLandings();
void Drain();
[[nodiscard]] Counts Totals();
[[nodiscard]] std::string Line();

}

#endif
