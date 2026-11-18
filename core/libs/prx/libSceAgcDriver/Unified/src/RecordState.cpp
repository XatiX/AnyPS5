#include "prx/libSceAgcDriver/Unified/include/RecordState.hpp"

namespace AgcDriver::Unified::Record {

std::optional<QueueState> QueueStateAt(const DecodeState& state, const SnapshotIds& ids) {
    const auto* context = state.Context(ids.context);
    const auto* userConfig = state.UserConfig(ids.userConfig);
    const auto* shader = state.Shader(ids.shader);
    const auto* scalars = state.Scalars(ids.scalars);
    if (context == nullptr || userConfig == nullptr || shader == nullptr || scalars == nullptr) return std::nullopt;
    QueueState queue;
    queue.context = *context;
    queue.userConfig = *userConfig;
    queue.shader = *shader;
    queue.indexBase = scalars->indexBase;
    queue.drawIndirectBase = scalars->drawIndirectBase;
    queue.dispatchIndirectBase = scalars->dispatchIndirectBase;
    queue.indexBufferSize = scalars->indexBufferSizeKnown ? std::optional<std::uint32_t>(scalars->indexBufferSize) : std::nullopt;
    queue.indexType = scalars->indexType;
    queue.instanceCount = scalars->instanceCount;
    return queue;
}

}
