// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/microprofile.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/frontend/emu_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_present_window.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

#include <vma/vk_mem_alloc.h>

MICROPROFILE_DEFINE(Vulkan_WaitPresent, "Vulkan", "Wait For Present", MP_RGB(128, 128, 128));

namespace Vulkan {

namespace {

bool CanBlitToSwapchain(const vk::PhysicalDevice& physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                          s32 swapchain_height) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = swapchain_width,
                    .y = swapchain_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageCopy MakeImageCopy(u32 frame_width, u32 frame_height, u32 swapchain_width,
                                          u32 swapchain_height) {
    return vk::ImageCopy{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffset =
            vk::Offset3D{
                .x = 0,
                .y = 0,
                .z = 0,
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffset =
            vk::Offset3D{
                .x = 0,
                .y = 0,
                .z = 0,
            },
        .extent =
            vk::Extent3D{
                .width = std::min(frame_width, swapchain_width),
                .height = std::min(frame_height, swapchain_height),
                .depth = 1,
            },
    };
}

} // Anonymous namespace

PresentWindow::PresentWindow(Frontend::EmuWindow& emu_window_, const Instance& instance_,
                             Scheduler& scheduler_, bool low_refresh_rate_)
    : emu_window{emu_window_}, instance{instance_}, scheduler{scheduler_},
      low_refresh_rate{low_refresh_rate_},
      surface{CreateSurface(instance.GetInstance(), emu_window)}, next_surface{surface},
      swapchain{instance, emu_window.GetFramebufferLayout().width,
                emu_window.GetFramebufferLayout().height, surface, low_refresh_rate_},
      graphics_queue{instance.GetGraphicsQueue()}, present_renderpass{CreateRenderpass()},
      vsync_enabled{Settings::values.use_vsync.GetValue()},
      blit_supported{
          CanBlitToSwapchain(instance.GetPhysicalDevice(), swapchain.GetSurfaceFormat().format)},
      use_present_thread{Settings::values.async_presentation.GetValue()},
      last_render_surface{emu_window.GetWindowInfo().render_surface} {

    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    const vk::CommandPoolCreateInfo pool_info = {
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer |
                 vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    };
    command_pool = device.createCommandPool(pool_info);

    const vk::CommandBufferAllocateInfo alloc_info = {
        .commandPool = command_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = num_images,
    };
    const std::vector command_buffers = device.allocateCommandBuffers(alloc_info);

    swap_chain.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = swap_chain[i];
        frame.cmdbuf = command_buffers[i];
        frame.render_ready = device.createSemaphore({});
        frame.present_done = device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled});
        free_queue.push(&frame);
    }

    if (instance.HasDebuggingToolAttached()) {
        for (u32 i = 0; i < num_images; ++i) {
            SetObjectName(device, swap_chain[i].cmdbuf, "Swapchain Command Buffer {}", i);
            SetObjectName(device, swap_chain[i].render_ready,
                          "Swapchain Semaphore: render_ready {}", i);
            SetObjectName(device, swap_chain[i].present_done, "Swapchain Fence: present_done {}",
                          i);
        }
    }

    if (use_present_thread) {
        present_thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
    }
}

PresentWindow::~PresentWindow() {
    scheduler.Finish();
    // xappify fork: a `RetirePresentedFrame` empty submit can still be in flight on the graphics
    // queue at teardown; destroying its fence/semaphore under it is a validation error at best.
    // Shutdown-only cost.
    {
        const std::scoped_lock submit_lock{scheduler.submit_mutex};
        graphics_queue.waitIdle();
    }
    const vk::Device device = instance.GetDevice();
    device.destroyCommandPool(command_pool);
    device.destroyRenderPass(present_renderpass);
    for (auto& frame : swap_chain) {
        device.destroyImageView(frame.image_view);
        device.destroyFramebuffer(frame.framebuffer);
        device.destroySemaphore(frame.render_ready);
        device.destroyFence(frame.present_done);
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
    }
}

void PresentWindow::RecreateFrame(Frame* frame, u32 width, u32 height) {
    vk::Device device = instance.GetDevice();
    if (frame->framebuffer) {
        device.destroyFramebuffer(frame->framebuffer);
    }
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    frame->image_view = device.createImageView(view_info);

    const vk::FramebufferCreateInfo framebuffer_info = {
        .renderPass = present_renderpass,
        .attachmentCount = 1,
        .pAttachments = &frame->image_view,
        .width = width,
        .height = height,
        .layers = 1,
    };
    frame->framebuffer = instance.GetDevice().createFramebuffer(framebuffer_info);

    frame->width = width;
    frame->height = height;
}

Frame* PresentWindow::GetRenderFrame() {
    MICROPROFILE_SCOPE(Vulkan_WaitPresent);

    // Wait for free presentation frames.
    //
    // xappify fork: BOUNDED. This used to be an untimed `free_cv.wait`, and it is called from the
    // emulation thread inside `Core::System::RunLoop` — so anything that stopped the present thread
    // from recycling frames stopped the whole emulator, silently and forever. Device-measured: an
    // 85.7 second hang after one background→foreground round trip, with the app alive and the guest
    // audio starved into repeating its last buffer.
    //
    // Waiting is still the right behaviour (it is how the frame pacing works); waiting FOREVER is
    // not. Log every second so a stall names itself, and after the cap take the frame regardless: a
    // torn frame recovers, a wedged emulator does not.
    using namespace std::chrono_literals;
    constexpr auto report_interval = 1s;
    constexpr u32 max_reports = 5;

    std::unique_lock lock{free_mutex};
    for (u32 waited = 0; !free_cv.wait_for(lock, report_interval,
                                           [this] { return !free_queue.empty(); });
         ++waited) {
        LOG_CRITICAL(Render_Vulkan,
                     "No free presentation frame after {}s — the present thread is not recycling "
                     "frames (lifecycle_suspended={} starvation_suspended={})",
                     waited + 1, presentation_suspended.load(std::memory_order_relaxed),
                     starvation_suspended.load(std::memory_order_relaxed));

        // Self-defence, not a fallback frame: handing back a Frame that is still in the present
        // queue would let the emulation thread render into an image the GPU is reading. Latching
        // instead makes `CopyToSwapchain` return immediately, so every in-flight frame is recycled
        // by its caller within one pass and this wait is then satisfied legitimately.
        //
        // Its OWN flag, not the lifecycle one: when this used to set `presentation_suspended`, a
        // 5-second hitch while FOREGROUNDED (shader-compile stall, thermal throttle) killed video
        // for the rest of the session — nothing app-side knew to call ResumePresentation. The latch
        // below is cleared by this same function the moment the starvation ends.
        if (waited + 1 == max_reports) {
            LOG_CRITICAL(Render_Vulkan,
                         "Presentation starved for {}s — engaging starvation latch to unblock the "
                         "emulation thread (lifecycle suspend untouched)",
                         max_reports);
            starvation_suspended.store(true, std::memory_order_relaxed);
            last_suspend_source.store(2, std::memory_order_relaxed);
        }
    }

    // Take the frame from the queue
    Frame* frame = free_queue.front();
    free_queue.pop();

    // The latch's one job — drain in-flight frames back to `free_queue` — is done the moment this
    // wait is satisfied. If presentation is still genuinely broken the next pass re-latches after
    // 5 s: a bounded duty cycle with a named log line each way, instead of a dead-for-the-session
    // screen. (Only this thread sets and clears the latch; ResumePresentation's clear is a wipe on
    // foreground return, not a race.)
    if (starvation_suspended.load(std::memory_order_relaxed)) {
        starvation_suspended.store(false, std::memory_order_relaxed);
        LOG_CRITICAL(Render_Vulkan,
                     "Starvation latch cleared — presentation resumes on the next present");
    }

    vk::Device device = instance.GetDevice();
    vk::Result result{};

    // xappify fork: bounded per attempt. This used to pass UINT64_MAX, so a `present_done` that was
    // never signalled — a frame recycled without being presented, see `RetirePresentedFrame` —
    // blocked the EMULATION thread here forever with no log line to say so. One second per attempt
    // costs nothing on the normal path (the fence is signalled within a frame) and makes the
    // pathological case announce itself.
    static constexpr u64 fence_wait_timeout_ns = 1'000'000'000;
    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, fence_wait_timeout_ns);
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    for (u32 waited = 0; wait() != vk::Result::eSuccess; ++waited) {
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            LOG_CRITICAL(Render_Vulkan,
                         "Frame fence still unsignalled after {}s — a frame was recycled without "
                         "being presented or the GPU is not progressing",
                         waited + 1);
            continue;
        }

        // eErrorInitializationFailed occurs on Mali GPU drivers due to them
        // using the ppoll() syscall which isn't correctly restarted after a signal,
        // we need to manually retry waiting in that case
        if (result == vk::Result::eErrorInitializationFailed) {
            continue;
        }
    }

    device.resetFences(frame->present_done);
    return frame;
}

void PresentWindow::Present(Frame* frame) {
    if (!use_present_thread) {
        scheduler.WaitWorker();
        CopyToSwapchain(frame);
        free_queue.push(frame);
        return;
    }

    scheduler.Record([this, frame](vk::CommandBuffer) {
        std::unique_lock lock{queue_mutex};
        present_queue.push(frame);
        frame_cv.notify_one();
    });
}

void PresentWindow::WaitPresent() {
    if (!use_present_thread) {
        return;
    }

    // Wait for the present queue to be empty
    //
    // xappify fork: BOUNDED per attempt. This is called from the emulation thread (the resize path
    // in `RenderToWindow`), and it used to be an untimed wait — the last silent way for a stuck
    // present thread to hang the emulator. Still waits as long as it takes; it just says so.
    {
        using namespace std::chrono_literals;
        std::unique_lock queue_lock{queue_mutex};
        for (u32 waited = 0;
             !frame_cv.wait_for(queue_lock, 1s, [this] { return present_queue.empty(); });
             ++waited) {
            LOG_CRITICAL(Render_Vulkan,
                         "WaitPresent blocked for {}s — present queue still holds {} frame(s)",
                         waited + 1, present_queue.size());
        }
    }

    // The above condition will be satisfied when the last frame is taken from the queue.
    // To ensure that frame has been presented as well take hold of the swapchain
    // mutex.
    std::scoped_lock swapchain_lock{swapchain_mutex};
}

void PresentWindow::PresentThread(std::stop_token token) {
    Common::SetCurrentThreadName("VulkanPresent");
    while (!token.stop_requested()) {
        std::unique_lock lock{queue_mutex};

        // Wait for presentation frames
        Common::CondvarWait(frame_cv, lock, token, [this] { return !present_queue.empty(); });
        if (token.stop_requested()) {
            return;
        }

        // Take the frame and notify anyone waiting
        Frame* frame = present_queue.front();
        present_queue.pop();
        frame_cv.notify_one();

        // By exchanging the lock ownership we take the swapchain lock
        // before the queue lock goes out of scope. This way the swapchain
        // lock in WaitPresent is guaranteed to occur after here.
        std::exchange(lock, std::unique_lock{swapchain_mutex});

        CopyToSwapchain(frame);

        // Free the frame for reuse
        std::scoped_lock fl{free_mutex};
        free_queue.push(frame);
        free_cv.notify_one();
    }
}

void PresentWindow::NotifySurfaceChanged() {
#ifdef ANDROID
    std::scoped_lock lock{recreate_surface_mutex};
    next_surface = CreateSurface(instance.GetInstance(), emu_window);
    recreate_surface_cv.notify_one();
#else
    // xappify fork: this used to be Android-only, so on iOS there was NO way to tell the renderer
    // its surface had gone stale — the swapchain built at boot was used forever, including across
    // background→foreground transitions that invalidate the layer's drawables.
    //
    // The surface handle itself is still valid here (the CAMetalLayer outlives the transition), so
    // rather than recreating it we mark the swapchain out-of-date; `CopyToSwapchain`'s acquire path
    // then rebuilds it against the layer's current state on the next present.
    swapchain.MarkNeedsRecreation();
#endif
}

void PresentWindow::SuspendPresentation() {
    presentation_suspended.store(true, std::memory_order_relaxed);
    last_suspend_source.store(1, std::memory_order_relaxed);
    // Anything already parked in GetRenderFrame should re-evaluate now rather than burn its timeout.
    free_cv.notify_all();
}

void PresentWindow::ResumePresentation() {
    presentation_suspended.store(false, std::memory_order_relaxed);
    // Clean slate: a starvation latch from before the background trip is stale by definition — the
    // in-flight frames it existed to drain were recycled while suspended.
    starvation_suspended.store(false, std::memory_order_relaxed);
    last_suspend_source.store(0, std::memory_order_relaxed);
    // The surface may have been torn down and rebuilt underneath us while suspended (an iOS app
    // returning to the foreground gets a fresh drawable pool), so force a swapchain rebuild on the
    // next present rather than trusting the one we had.
    NotifySurfaceChanged();
}

void PresentWindow::RetirePresentedFrame(Frame* frame) {
    // xappify fork. `GetRenderFrame` hands out a frame only after `waitForFences(present_done)`
    // succeeds, then resets the fence — so whoever takes the frame OWES it a signal. On the normal
    // path that comes from `graphics_queue.submit(submit_info, frame->present_done)`.
    //
    // `CopyToSwapchain`'s two bail-out paths (presentation suspended; the acquire exhausted its
    // retries) return before that submit, while their callers still push the frame back onto
    // `free_queue`. Without this, the next `GetRenderFrame` to draw that frame blocks in
    // `waitForFences(..., UINT64_MAX)` on a fence nothing will ever signal — a permanent hang of the
    // EMULATION thread, which is the exact failure the bounded waits elsewhere in this file exist to
    // prevent. Signal it with an empty submit so the frame is genuinely reusable.
    // Same lock order as the normal path and as `recreate_swapchain` (swapchain_mutex, held by our
    // caller, then submit_mutex).
    //
    // INVARIANT: only call this on a frame that reached `CopyToSwapchain` through `Present()`. Such
    // a frame carries a pending `render_ready` signal — `RenderToWindow` does
    // `scheduler.Flush(frame->render_ready)` before every `Present()`, and the only thing that
    // consumes the signal is the present submit this function is standing in for. The empty submit
    // below therefore WAITS `render_ready` (consuming the signal) as well as signalling the fence.
    // Both halves matter:
    //   - skip the fence and the next `GetRenderFrame` on this frame blocks forever (see above);
    //   - skip the semaphore and the frame's NEXT pass re-signals an already-signalled binary
    //     semaphore — a VUID-vkQueueSubmit-pSignalSemaphores-00067 violation that MoltenVK's
    //     MTLEvent emulation turns into cumulative queue-wait skew. Device symptom: the game slows
    //     down over successive background trips, then wedges in a GPU wait that never returns.
    // A caller that violates the invariant (retiring a frame with NO pending signal) deadlocks the
    // graphics queue on this wait — the bounded fence wait in `GetRenderFrame` names that within 1 s.
    const std::scoped_lock submit_lock{scheduler.submit_mutex};
    try {
        // No command buffers — `vkQueueSubmit` purely for its synchronisation effects.
        static constexpr vk::PipelineStageFlags retire_wait_stage =
            vk::PipelineStageFlagBits::eAllCommands;
        const vk::SubmitInfo retire_info = {
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &frame->render_ready,
            .pWaitDstStageMask = &retire_wait_stage,
        };
        graphics_queue.submit(retire_info, frame->present_done);
        retired_frames.fetch_add(1, std::memory_order_relaxed);
    } catch (const vk::SystemError& err) {
        // Nothing left to try — but say so, because the symptom downstream would be a silent hang
        // (unsignalled fence) plus a leaked render_ready signal on this frame's next pass.
        LOG_CRITICAL(Render_Vulkan, "Failed to retire an unpresented frame's fence: {}", err.what());
    }
}

// xappify fork — see the header. Emulation-thread safe by lock discipline: the two queue mutexes
// are taken one after the other, and the swapchain/submit mutexes are never touched.
void PresentWindow::LogPresentState(const char* marker) {
    std::size_t free_count = 0;
    {
        std::scoped_lock lock{free_mutex};
        free_count = free_queue.size();
    }
    std::size_t queued_count = 0;
    {
        std::scoped_lock lock{queue_mutex};
        queued_count = present_queue.size();
    }
    const u8 source = last_suspend_source.load(std::memory_order_relaxed);
    LOG_CRITICAL(Render_Vulkan,
                 "[present-state] marker={} free={} queued={} lifecycle_suspended={} "
                 "starvation_suspended={} last_suspend_source={} retired={} recreations={}",
                 marker, free_count, queued_count,
                 presentation_suspended.load(std::memory_order_relaxed),
                 starvation_suspended.load(std::memory_order_relaxed),
                 source == 1 ? "lifecycle"
                 : source == 2 ? "starvation"
                               : "none",
                 retired_frames.load(std::memory_order_relaxed), swapchain.GetRecreations());
}

void PresentWindow::CopyToSwapchain(Frame* frame) {
    // xappify fork: while suspended, do not touch the surface at all. Returning immediately is all
    // that is needed — BOTH callers (`PresentThread` and the `!use_present_thread` branch of
    // `Present`) push the frame back onto `free_queue` themselves, so recycling it here as well
    // would enqueue the same frame twice. See SuspendPresentation.
    if (presentation_suspended.load(std::memory_order_relaxed) ||
        starvation_suspended.load(std::memory_order_relaxed)) [[unlikely]] {
        RetirePresentedFrame(frame);
        return;
    }

    // xappify fork: every rebuild is a `graphics_queue.waitIdle()` plus a full teardown/create, taken
    // on the present thread while `swapchain_mutex` is held — i.e. it stalls the emulation thread in
    // `GetRenderFrame`. It used to happen roughly once a second on device with nothing in the app
    // able to see it. Name the reason and count them so the next capture measures the thrash instead
    // of us inferring it from `[mvk-info]` console spam.
    const auto recreate_swapchain = [&](const char* reason) {
#ifdef ANDROID
        {
            std::unique_lock lock{recreate_surface_mutex};
            recreate_surface_cv.wait(lock, [this]() { return surface != next_surface; });
            surface = next_surface;
        }
#endif
        {
            std::scoped_lock submit_lock{scheduler.submit_mutex};
            graphics_queue.waitIdle();
            swapchain.Create(frame->width, frame->height, surface, low_refresh_rate);
        }

        // Rate-limited: if the thrash ever comes back, this must not become the thing that causes it.
        const auto now = std::chrono::steady_clock::now();
        if (now - last_recreate_log >= std::chrono::seconds{1}) {
            last_recreate_log = now;
            LOG_INFO(Render_Vulkan, "Swapchain recreated (reason={}, total={})", reason,
                     swapchain.GetRecreations());
        }
    };

#ifndef ANDROID
    const bool use_vsync = Settings::values.use_vsync.GetValue();
    const bool size_changed =
        swapchain.GetWidth() != frame->width || swapchain.GetHeight() != frame->height;
    const bool vsync_changed = vsync_enabled != use_vsync;
    // A runtime frame-limit change (e.g. toggling fast-forward) can flip the desired present mode
    // between FIFO and Mailbox/Immediate; recreate so the emulation loop unlocks past vblank.
    const bool present_mode_changed = swapchain.NeedsPresentModeUpdate();
    // Drained unconditionally (not short-circuited by the checks above) so the flag never survives
    // into a later frame and triggers a rebuild that has nothing to do with it.
    const bool suboptimal_extent = swapchain.ConsumeSuboptimal();
    if (vsync_changed || size_changed || present_mode_changed || suboptimal_extent) [[unlikely]] {
        vsync_enabled = use_vsync;
        recreate_swapchain(size_changed            ? "size"
                           : vsync_changed         ? "vsync"
                           : present_mode_changed  ? "present-mode"
                                                   : "suboptimal-extent");
    }
#endif

    // xappify fork: BOUNDED. This used to be `while (!AcquireNextImage()) recreate_swapchain();`.
    // When the surface cannot vend a drawable at all — an iOS layer whose app is backgrounded, most
    // reliably — the condition never becomes true, so this span the present thread forever while
    // holding `swapchain_mutex`, and `free_queue` was never refilled. That is the other half of the
    // 85-second emulation-thread block; capping it means a lost drawable costs one dropped frame.
    constexpr u32 max_acquire_attempts = 3;
    bool acquired = false;
    for (u32 attempt = 0; attempt < max_acquire_attempts; ++attempt) {
        if (swapchain.AcquireNextImage()) {
            acquired = true;
            break;
        }
        // Only eErrorOutOfDateKHR / eErrorSurfaceLostKHR / an explicit MarkNeedsRecreation reach
        // here now — eSuboptimalKHR acquires successfully and is handled above. Which of the three
        // it was matters: they have different fixes, and a plain "acquire-failed" cost a round.
        recreate_swapchain(swapchain.LastAcquireFailure());
    }
    if (!acquired) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan,
                     "Could not acquire a swapchain image in {} attempts — dropping this frame",
                     max_acquire_attempts);
        // Return WITHOUT presenting; the caller recycles the frame, so the emulation thread keeps
        // getting frames and the next pass retries the acquire from scratch.
        RetirePresentedFrame(frame);
        return;
    }

    const vk::Image swapchain_image = swapchain.Image();

    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    const vk::CommandBuffer cmdbuf = frame->cmdbuf;
    cmdbuf.begin(begin_info);

    const vk::Extent2D extent = swapchain.GetExtent();
    const std::array pre_barriers{
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        },
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = frame->image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        },
    };
    const vk::ImageMemoryBarrier post_barrier{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::ePresentSrcKHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapchain_image,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };

    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                           {}, {}, pre_barriers);

    if (blit_supported) {
        cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlit(frame->width, frame->height, extent.width, extent.height),
                         vk::Filter::eLinear);
    } else {
        cmdbuf.copyImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageCopy(frame->width, frame->height, extent.width, extent.height));
    }

    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                           vk::PipelineStageFlagBits::eAllCommands,
                           vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);

    cmdbuf.end();

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eAllGraphics,
    };

    const vk::Semaphore present_ready = swapchain.GetPresentReadySemaphore();
    const vk::Semaphore image_acquired = swapchain.GetImageAcquiredSemaphore();
    const std::array wait_semaphores = {image_acquired, frame->render_ready};

    vk::SubmitInfo submit_info = {
        .waitSemaphoreCount = static_cast<u32>(wait_semaphores.size()),
        .pWaitSemaphores = wait_semaphores.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1u,
        .pCommandBuffers = &cmdbuf,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &present_ready,
    };

    std::scoped_lock submit_lock{scheduler.submit_mutex, recreate_surface_mutex};

    try {
        graphics_queue.submit(submit_info, frame->present_done);
    } catch (vk::DeviceLostError& err) {
        LOG_CRITICAL(Render_Vulkan, "Device lost during present submit: {}", err.what());
        UNREACHABLE();
    }

    swapchain.Present();
}

vk::RenderPass PresentWindow::CreateRenderpass() {
    const vk::AttachmentReference color_ref = {
        .attachment = 0,
        .layout = vk::ImageLayout::eGeneral,
    };

    const vk::SubpassDescription subpass = {
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &color_ref,
        .pResolveAttachments = 0,
        .pDepthStencilAttachment = nullptr,
    };

    const vk::AttachmentDescription color_attachment = {
        .format = swapchain.GetSurfaceFormat().format,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
        .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
        .initialLayout = vk::ImageLayout::eUndefined,
        .finalLayout = vk::ImageLayout::eTransferSrcOptimal,
    };

    const vk::RenderPassCreateInfo renderpass_info = {
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 0,
        .pDependencies = nullptr,
    };

    return instance.GetDevice().createRenderPass(renderpass_info);
}

} // namespace Vulkan
