#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_VULKANIMAGES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_VULKANIMAGES_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Unified/include/Images.hpp"
#include <map>
#include <mutex>
#include <tuple>

namespace AgcDriver::Unified::Images {

class VulkanSurface final : public DeviceSurface {
public:
    VulkanSurface(const Graphics::Context& context, const Graphics::GuestTextureResource& resource, Graphics::SurfaceGeometry geometry, VkFormat format);
    ~VulkanSurface() override;
    VulkanSurface(const VulkanSurface&) = delete;
    VulkanSurface& operator=(const VulkanSurface&) = delete;

    [[nodiscard]] std::uint64_t View(const ViewKey& view, std::uint32_t kind, std::string& reason) override;
    [[nodiscard]] std::uint32_t Layout() const override { return VK_IMAGE_LAYOUT_GENERAL; }
    [[nodiscard]] VkImageView StorageView(std::uint32_t level, std::uint32_t layer, std::uint32_t elementBytes);
    [[nodiscard]] VkImageView ShapedStorageView(std::uint32_t level, bool array, bool uintView, std::string& reason);
    [[nodiscard]] VkImageView AttachmentView(VkFormat viewFormat, std::uint32_t level, std::uint32_t layer, std::uint32_t layers, std::string& reason);
    [[nodiscard]] bool Attachable() const { return attachable; }
    [[nodiscard]] VkImage Image() const { return image; }
    [[nodiscard]] const Graphics::GuestTextureResource& Resource() const { return resource; }
    [[nodiscard]] const Graphics::SurfaceGeometry& Geometry() const { return geometry; }
    [[nodiscard]] bool Initialized() const { return initialized; }
    [[nodiscard]] bool Compressed() const { return compressed; }
    void SetInitialized() { initialized = true; }

private:
    const Graphics::Context& context;
    Graphics::GuestTextureResource resource;
    Graphics::SurfaceGeometry geometry;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool initialized = false;
    bool compressed = false;
    bool identity = false;
    std::mutex mutex;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>, VkImageView> views;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, VkImageView> storageViews;
    std::map<std::tuple<std::uint32_t, bool, bool>, VkImageView> shapedStorageViews;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>, VkImageView> attachmentViews;
    bool attachable = false;
};

class VulkanSurfaceDevice final : public SurfaceDevice {
public:
    explicit VulkanSurfaceDevice(const Graphics::Context& context) : context(context) {}

    [[nodiscard]] std::shared_ptr<DeviceSurface> Create(const SurfaceKey& key, std::span<const std::uint32_t, 8> words, std::string& reason) override;
    [[nodiscard]] std::shared_ptr<DeviceSurface> CreateFromResource(const SurfaceKey& key, const Graphics::GuestTextureResource& resource, std::string& reason) override;

private:
    const Graphics::Context& context;
};

}

#endif
