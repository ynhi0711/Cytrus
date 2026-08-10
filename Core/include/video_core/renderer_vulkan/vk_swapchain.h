// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
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
        last_acquire_failure = "marked";
    }

    /// Why the last `AcquireNextImage` returned false: `out-of-date`, `surface-lost` or `marked`.
    /// The caller puts it in the recreation log — "acquire-failed" alone cannot distinguish a stale
    /// surface from MoltenVK refusing to vend a drawable, and those need different fixes.
    [[nodiscard]] const char* LastAcquireFailure() const {
        return last_acquire_failure;
    }

    /// xappify fork: answers "did the driver's `eSuboptimalKHR` actually mean anything?", and clears
    /// the flag either way.
    ///
    /// Suboptimal is a SUCCESS code — the image was acquired and its semaphore signalled — so unlike
    /// `eErrorOutOfDateKHR` it does not oblige us to rebuild. MoltenVK returns it routinely for
    /// conditions a rebuild cannot fix, and the old code treated the two identically: every
    /// Suboptimal dropped the frame and cost a full `graphics_queue.waitIdle()` + teardown +
    /// re-create. On device that latched into a rebuild roughly every second for minutes at a time
    /// and took the emulation loop from ~3600 to ~500 iterations/s. Only a genuine extent change
    /// justifies the rebuild, and that is the one thing worth re-querying the surface for.
    [[nodiscard]] bool ConsumeSuboptimal();

    /// Total `Create()` calls this session, including the first. Polled by the frontend's liveness
    /// heartbeat — recreation thrash is invisible from outside otherwise (its only trace was
    /// `[mvk-info]` console spam, which does not survive a detached debugger).
    [[nodiscard]] u64 GetRecreations() const {
        return recreations.load(std::memory_order_relaxed);
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
    // Set by an acquire or present that returned eSuboptimalKHR; drained by ConsumeSuboptimal().
    bool suboptimal = false;
    // Static string literal, so it is safe to hand out and cheap to store.
    const char* last_acquire_failure = "none";
    std::atomic<u64> recreations{0};
    bool low_refresh_rate;
    // Device present-mode availability, cached by SetPresentMode() so NeedsPresentModeUpdate()
    // can recompute the desired mode without re-querying the surface every frame.
    bool supports_immediate = false;
    bool supports_mailbox = false;
};

} // namespace Vulkan
