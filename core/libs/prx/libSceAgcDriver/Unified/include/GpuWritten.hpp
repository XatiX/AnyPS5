#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_GPUWRITTEN_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_GPUWRITTEN_HPP

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace AgcDriver::Unified {

struct WrittenRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t serial = 0;

    bool operator==(const WrittenRange&) const = default;
};

struct GpuWrittenCounts {
    std::uint64_t noted = 0;
    std::uint64_t notedBytes = 0;
    std::uint64_t taken = 0;
    std::uint64_t takenBytes = 0;
    std::uint64_t discardedBytes = 0;
    std::uint64_t pieces = 0;
    std::uint64_t bytes = 0;
};

class GpuWritten {
public:
    std::uint64_t Note(std::uint64_t address, std::uint64_t size, std::uint64_t serial);
    std::uint64_t TakeNoted(std::uint64_t address, std::uint64_t size, std::uint64_t sequence);
    [[nodiscard]] std::uint64_t Pending(std::uint64_t address, std::uint64_t size) const;
    [[nodiscard]] bool Any(std::uint64_t address, std::uint64_t size) const;
    [[nodiscard]] std::vector<WrittenRange> In(std::uint64_t address, std::uint64_t size) const;
    std::vector<WrittenRange> Take(std::uint64_t address, std::uint64_t size, std::uint64_t completed);
    std::uint64_t Discard(std::uint64_t address, std::uint64_t size);
    [[nodiscard]] GpuWrittenCounts Counts() const;

private:
    struct Piece {
        std::uint64_t end = 0;
        std::uint64_t serial = 0;
        std::uint64_t sequence = 0;
    };
    using Pieces = std::map<std::uint64_t, Piece>;

    Pieces::iterator firstOverlapping(std::uint64_t begin);
    Pieces::const_iterator firstOverlapping(std::uint64_t begin) const;
    std::uint64_t carve(std::uint64_t begin, std::uint64_t end, std::uint64_t completed, std::vector<WrittenRange>* removed, std::uint64_t onlySequence = 0);

    mutable std::mutex mutex;
    Pieces pieces;
    std::uint64_t nextSequence = 0;
    std::uint64_t heldBytes = 0;
    GpuWrittenCounts counts;
};

}

#endif
