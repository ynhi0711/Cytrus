// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include "common/polyfill_thread.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace Frontend {
class EmuWindow;
}

namespace Vulkan {

class Instance;
class Swapchain;
class Scheduler;
class RenderManager;

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Framebuffer framebuffer;
    vk::Image image;
    vk::ImageView image_view;
    vk::Semaphore render_ready;
    vk::Fence present_done;
    vk::CommandBuffer cmdbuf;
};

class PresentWindow final {
public:
    explicit PresentWindow(Frontend::EmuWindow& emu_window, const Instance& instance,
                           Scheduler& scheduler, bool low_refresh_rate);
    ~PresentWindow();

    /// Waits for all queued frames to finish presenting.
    void WaitPresent();

    /// Returns the last used render frame.
    Frame* GetRenderFrame();

    /// Recreates the render frame to match provided parameters.
    void RecreateFrame(Frame* frame, u32 width, u32 height);

    /// Queues the provided frame for presentation.
    void Present(Frame* frame);

    /// This is called to notify the rendering backend of a surface change
    void NotifySurfaceChanged();

    /**
     * xappify fork: stop / restart actually touching the presentation surface.
     *
     * On iOS a backgrounded `CAMetalLayer` cannot vend a drawable, so `AcquireNextImage` fails
     * forever and `CopyToSwapchain` spins recreating the swapchain while holding `swapchain_mutex`
     * — frames never return to `free_queue`, and the emulation thread blocks in `GetRenderFrame`
     * inside `Core::System::RunLoop`. Device-measured: 85.7 seconds of a completely dead emulator
     * after one background→foreground round trip.
     *
     * While suspended, `CopyToSwapchain` recycles each frame immediately without touching the
     * surface, so the producer never starves however long the app stays away. Pausing the emulator
     * is NOT a substitute: the pause flag is only read at the top of the run loop, so it cannot
     * unblock a present that is already in flight.
     */
    void SuspendPresentation();
    void ResumePresentation();

    [[nodiscard]] vk::RenderPass Renderpass() const noexcept {
        return present_renderpass;
    }

    u32 ImageCount() const noexcept {
        return swapchain.GetImageCount();
    }

private:
    void PresentThread(std::stop_token token);

    void CopyToSwapchain(Frame* frame);

    vk::RenderPass CreateRenderpass();

private:
    Frontend::EmuWindow& emu_window;
    const Instance& instance;
    Scheduler& scheduler;
    bool low_refresh_rate;
    vk::SurfaceKHR surface;
    vk::SurfaceKHR next_surface{};
    Swapchain swapchain;
    vk::CommandPool command_pool;
    vk::Queue graphics_queue;
    vk::RenderPass present_renderpass;
    std::vector<Frame> swap_chain;
    std::queue<Frame*> free_queue;
    std::queue<Frame*> present_queue;
    std::condition_variable free_cv;
    std::condition_variable recreate_surface_cv;
    std::condition_variable_any frame_cv;
    std::mutex swapchain_mutex;
    std::mutex recreate_surface_mutex;
    std::mutex queue_mutex;
    std::mutex free_mutex;
    std::jthread present_thread;
    bool vsync_enabled{};
    bool blit_supported;
    bool use_present_thread{true};
    void* last_render_surface{};
    /// xappify fork: see SuspendPresentation. Atomic because the present thread reads it while the
    /// UI thread writes it, with no lock in common.
    std::atomic_bool presentation_suspended{false};
};

} // namespace Vulkan
