#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RECORDSTATE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RECORDSTATE_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Unified/include/Decoder.hpp"
#include <optional>

namespace AgcDriver::Unified::Record {

std::optional<QueueState> QueueStateAt(const DecodeState& state, const SnapshotIds& ids);

}

#endif
