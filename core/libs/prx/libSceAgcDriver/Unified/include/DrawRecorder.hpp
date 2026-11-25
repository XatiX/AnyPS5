#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DRAWRECORDER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DRAWRECORDER_HPP

#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Unified/include/Resolve.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace AgcDriver {
class VulkanDevice;
}

namespace AgcDriver::Graphics {
class Buffer;
class Recorder;
}

namespace AgcDriver::Unified::Record {

[[nodiscard]] bool RecordActive();
[[nodiscard]] bool DropsUnrecorded();
[[nodiscard]] bool RecordsKernels(std::uint32_t queue);
enum class Kernel : std::uint8_t { Fill, Copy };
void NoteKernel(Kernel kernel);
void FlipSource(const Graphics::Context& context, std::uint64_t address, std::uint64_t bytes);
struct DisplaySource {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    Graphics::GuestTextureResource resource;
    std::shared_ptr<void> owner;
};
[[nodiscard]] std::optional<DisplaySource> DisplayRecord(std::uint64_t address, std::uint64_t bytes);
[[nodiscard]] bool PresentsExistingResidents();
void NoteSampleDumpBefore(std::uint64_t address);
[[nodiscard]] bool DumpSamplesToMirror(const Graphics::Context& context, Graphics::Recorder& recorder, std::uint64_t address, std::uint64_t bytes, std::uint32_t blocks);
void DeviceReplaced(const Graphics::Context& context, Graphics::Recorder& recorder);
[[nodiscard]] bool StoresToMirror(std::size_t bytes, std::uint64_t address);
[[nodiscard]] std::optional<Pm4::MemoryCopy> MirrorCopyOf(std::span<const std::uint32_t> packet);
[[nodiscard]] std::shared_ptr<Graphics::Buffer> ReadMirrorAfterBatch(const Graphics::Context& context, Graphics::Recorder& recorder, std::uint64_t address, std::uint64_t bytes);
[[nodiscard]] bool CopyInMirror(const Graphics::Context& context, Graphics::Recorder& recorder, const Pm4::MemoryCopy& copy);
[[nodiscard]] int StoreToMirror(const Graphics::Context& context, Graphics::Recorder& recorder, std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue);
void NoteExistingPacket(std::span<const std::uint32_t> packet, std::uint32_t queue);
[[nodiscard]] bool LandingsPending();
[[nodiscard]] bool RecordsHtileClears();
void LandAfterBatch(Graphics::Recorder& recorder, std::function<void()> landing);
void AfterLandings(std::function<void()> action);
void DrainHostCopies();
[[nodiscard]] bool IdleStores();
bool RecordPrecheck(const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::vector<std::pair<std::uint64_t, std::uint64_t>>& reads, std::string& reason);
bool RecordResolved(const Graphics::Context& context, const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::string& reason);
bool RecordDispatchPrecheck(const Graphics::Context& context, VulkanDevice& owner, const Resolve::ResolvedDraw& dispatch, std::string& reason);
bool RecordDispatch(const Graphics::Context& context, const Resolve::ResolvedDraw& dispatch, const std::array<std::uint32_t, 3>& groups, std::string& reason);
void PrebuildDispatch(const Graphics::Context& context, VulkanDevice& owner, const Resolve::ResolvedDraw& dispatch);
void RefuseUnrecorded(std::string_view work, std::uint64_t codeAddress, std::string_view reason);
void NoteExistingDispatch(std::uint64_t codeAddress, std::uint64_t arguments, std::uint64_t groups);
void ProbeExistingWrites(Graphics::Recorder& recorder, std::uint64_t codeAddress, std::span<const std::pair<std::uint64_t, std::uint64_t>> writes);
[[nodiscard]] std::string RecordLine();

}

#endif
