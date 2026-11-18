#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SHADOW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SHADOW_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include <memory>

namespace AgcDriver::Unified::Shadow {

bool Active();
std::shared_ptr<DriverDetail::Submission> Copy(const DriverDetail::Submission& submission);
void Tee(const DriverDetail::Submission& submission);

}

#endif
