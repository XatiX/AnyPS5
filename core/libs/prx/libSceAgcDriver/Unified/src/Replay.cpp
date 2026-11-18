#include "prx/libSceAgcDriver/Unified/include/Replay.hpp"

namespace AgcDriver::Unified {

std::shared_ptr<DriverDetail::Submission> FromCapture(const CapturedSubmission& captured) {
    auto submission = std::make_shared<DriverDetail::Submission>();
    submission->serial = captured.serial;
    submission->queue = captured.queue;
    submission->commands = captured.commands;
    for (const auto& [start, end] : captured.conditionalRanges) submission->conditionalEnds.emplace(start, end);
    return submission;
}

std::vector<LaneTotals> ReplayCapture(const std::filesystem::path& directory, Front& front) {
    for (const auto& captured : SubmissionCapture::Read(directory)) {
        if (captured.kind == SubmissionKind::Suspend || captured.commands.empty()) continue;
        front.Push(FromCapture(captured));
    }
    return front.Totals();
}

}
