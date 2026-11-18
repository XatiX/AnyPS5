#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_REPLAY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_REPLAY_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/SubmissionCapture.hpp"
#include "prx/libSceAgcDriver/Unified/include/Front.hpp"
#include <filesystem>
#include <memory>
#include <vector>

namespace AgcDriver::Unified {

std::shared_ptr<DriverDetail::Submission> FromCapture(const CapturedSubmission& captured);
std::vector<LaneTotals> ReplayCapture(const std::filesystem::path& directory, Front& front);

}

#endif
