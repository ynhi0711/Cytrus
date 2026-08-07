// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <mutex>
#include <vector>
#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

class Swapchain {
public:
    explicit Swapchain(const Instance& instance, u32 width, u32 height, vk::SurfaceKHR surface,
                       bool low_refresh_rate);
    ~Swapchain();

    /// Creates (or recreates) the swapchain with a given size.
    void Create(u32 width, u32 height, vk::SurfaceKHR surface, bool low_refresh_rate);

    /// Acquires the next image in the swapchain.
    bool AcquireNextImage();

    /// Presents the current image and move to the next one
    void Present();

    vk::SurfaceKHR GetSurface() const {
        return surface;
    }

    vk::Image Image() const {
        return images[image_index];
    }

    vk::SurfaceFormatKHR GetSurfaceFormat() const {
        return surface_format;
    }

    vk::SwapchainKHR GetHandle() const {
        return swapchain;
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    u32 GetImageCount() const {
        return image_count;
    }

    vk::Extent2D GetExtent() const {
        return extent;
    }

    [[nodiscard]] vk::Semaphore GetImageAcquiredSemaphore() const {
        return image_acquired[frame_index];
    }

    [[nodiscard]] vk::Semaphore GetPresentReadySemaphore() const {
        return present_ready[image_index];
    }

    /// Returns true if the present mode implied by current settings (vsync / frame limit) differs
    /// from the active one — e.g. after toggling fast-forward. The caller should recreate the
    /// swapchain so the new mode takes effect. Cheap: uses cached device capabilities.
    [[nodiscard]] bool NeedsPresentModeUpdate() const;

    /// xappify fork: force a rebuild on the next acquire, for a surface that has gone stale without
    /// the driver reporting it — an iOS layer coming back to the foreground does not necessarily
    /// return eErrorOutOfDateKHR, it just fails to vend drawables. See
    /// `PresentWindow::NotifySurfaceChanged`.
    void MarkNeedsRecreation() {
        needs_recreation = true;
    }

private:
    /// Selects the best available swapchain image format
    void FindPresentFormat();

    /// Sets the best available present mode
    void SetPresentMode();

    /// Computes the present mode implied by current settings and cached device capabilities,
    /// without querying the device or mutating state.
    [[nodiscard]] vk::PresentModeKHR DesiredPresentMode() const;

    /// Sets the surface properties according to device capabilities
    void SetSurfaceProperties();

    /// Destroys current swapchain resources
    void Destroy();

    /// Performs creation of image views and framebuffers from the swapchain images
    void SetupImages();

    /// Creates the image acquired and present ready semaphores
    void RefreshSemaphores();

private:
    const Instance& instance;
    vk::SwapchainKHR swapchain{};
    vk::SurfaceKHR surface{};
    vk::SurfaceFormatKHR surface_format;
    vk::PresentModeKHR present_mode;
    vk::Extent2D extent;
    vk::SurfaceTransformFlagBitsKHR transform;
    vk::CompositeAlphaFlagBitsKHR composite_alpha;
    std::vector<vk::Image> images;
    std::vector<vk::Semaphore> image_acquired;
    std::vector<vk::Semaphore> present_ready;
    u32 width = 0;
    u32 height = 0;
    u32 image_count = 0;
    u32 image_index = 0;
    u32 frame_index = 0;
    bool needs_recreation = true;
    bool low_refresh_rate;
    // Device present-mode availability, cached by SetPresentMode() so NeedsPresentModeUpdate()
    // can recompute the desired mode without re-querying the surface every frame.
    bool supports_immediate = false;
    bool supports_mailbox = false;
};

} // namespace Vulkan
