#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_IMAGEDETILE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_IMAGEDETILE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Unified/include/Images.hpp"
#include <memory>
#include <string>

namespace AgcDriver::Graphics {
class Recorder;
class TextureDetiler;
}

namespace AgcDriver::Unified::Images {

struct DetileCounts {
    std::uint64_t surfaces = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t bytes = 0;
    std::uint64_t refused = 0;
};

class SurfaceDetiler {
public:
    explicit SurfaceDetiler(const Graphics::Context& context);
    ~SurfaceDetiler();
    SurfaceDetiler(const SurfaceDetiler&) = delete;
    SurfaceDetiler& operator=(const SurfaceDetiler&) = delete;

    [[nodiscard]] bool Record(Graphics::Recorder& recorder, SurfaceTable& table, const std::shared_ptr<Surface>& surface, std::string& reason);
    [[nodiscard]] DetileCounts TakeCounts();

private:
    const Graphics::Context& context;
    std::unique_ptr<Graphics::TextureDetiler> detiler;
    std::uint64_t batch = 0;
    DetileCounts counts;
};

}

#endif
