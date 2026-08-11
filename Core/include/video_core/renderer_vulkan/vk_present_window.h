// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <chrono>
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

    /// xappify fork: total swapchain rebuilds this session. See `Swapchain::GetRecreations`.
    [[nodiscard]] u64 SwapchainRecreations() const noexcept {
        return swapchain.GetRecreations();
    }

    /// xappify fork: one `[present-state]` log line — queue depths, both suspend flags, who last
    /// suspended, retired-frame count. Called from the emulation-thread wedge dump; takes
    /// `free_mutex` then `queue_mutex` sequentially (never nested) and touches neither
    /// `swapchain_mutex` nor `submit_mutex`, so it cannot deadlock against the present thread.
    void LogPresentState(const char* marker);

private:
    void PresentThread(std::stop_token token);

    void CopyToSwapchain(Frame* frame);

    /// xappify fork: settles a frame that is being recycled WITHOUT having been presented — signals
    /// its `present_done` fence AND consumes its pending `render_ready` signal. See the
    /// implementation for the invariant; skipping this hangs the emulation thread forever, and
    /// skipping only the semaphore half wedges MoltenVK slowly (leaked binary-semaphore signals).
    void RetirePresentedFrame(Frame* frame);

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
    /// UI thread writes it, with no lock in common. LIFECYCLE ownership only — the app sets it on
    /// resign-active and clears it on become-active. The starvation self-defence has its own flag
    /// below; folding them into one bit is what once left presentation dead for a whole session
    /// after a foreground hitch (nothing ever called ResumePresentation to clear it).
    std::atomic_bool presentation_suspended{false};
    /// xappify fork: set by `GetRenderFrame` after 5 s of frame starvation so `CopyToSwapchain`
    /// drains in-flight frames straight back to `free_queue`; cleared by `GetRenderFrame` itself the
    /// moment the starvation is over (and by ResumePresentation, as a clean slate). Distinct from
    /// the lifecycle flag above — different owner, different clear condition.
    std::atomic_bool starvation_suspended{false};
    /// xappify fork: who last suspended presentation — 0 none, 1 lifecycle, 2 starvation. Purely for
    /// the `[present-state]` dump line.
    std::atomic<u8> last_suspend_source{0};
    /// xappify fork: frames settled by `RetirePresentedFrame` instead of being presented. A healthy
    /// session shows a few per background trip; a climb during foreground play means frames are
    /// being dropped on the floor.
    std::atomic<u64> retired_frames{0};
    /// Rate limiter for the swapchain-recreation log. Present-thread only.
    std::chrono::steady_clock::time_point last_recreate_log{};
};

} // namespace Vulkan
