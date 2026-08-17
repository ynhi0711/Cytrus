// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <stdexcept>
#include <utility>
#include <boost/serialization/array.hpp>
#include "audio_core/dsp_interface.h"
#include "audio_core/hle/hle.h"
#include "audio_core/lle/lle.h"
#include "common/arch.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/arm/arm_interface.h"
#include "core/arm/exclusive_monitor.h"
#include "core/hle/service/cam/cam.h"
#include "core/hle/service/hid/hid.h"
#include "core/hle/service/ir/ir_user.h"
#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)
#include "core/arm/dynarmic/arm_dynarmic.h"
#endif
#include "core/arm/dyncom/arm_dyncom.h"
#include "core/cheats/cheats.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/dumping/backend.h"
#include "core/file_sys/ncch_container.h"
#include "core/frontend/image_interface.h"
#include "core/gdbstub/gdbstub.h"
#include "core/global.h"
#include "core/hle/kernel/ipc_debugger/recorder.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/service/apt/applet_manager.h"
#include "core/hle/service/apt/apt.h"
#include "core/hle/service/cam/cam.h"
#include "core/hle/service/fs/archive.h"
#include "core/hle/service/gsp/gsp.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/ir/ir_rst.h"
#include "core/hle/service/mic/mic_u.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"
#include "core/hw/aes/key.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#ifdef ENABLE_SCRIPTING
#include "core/rpc/server.h"
#endif
#include "network/network.h"
#include "video_core/custom_textures/custom_tex_manager.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace Core {

/*static*/ System System::s_instance;

template <>
Core::System& Global() {
    return System::GetInstance();
}

template <>
Kernel::KernelSystem& Global() {
    return System::GetInstance().Kernel();
}

template <>
Core::Timing& Global() {
    return System::GetInstance().CoreTiming();
}

System::System() : movie{*this}, cheat_engine{*this} {}

System::~System() = default;

/// Defined below, next to `SendSignal`.
static const char* SignalName(System::Signal signal);

System::ResultStatus System::RunLoop(bool tight_loop) {
    status = ResultStatus::Success;
    if (!IsPoweredOn()) {
        // xappify fork: this return is BEFORE the signal block below, so a queued Save/Load used to
        // sit in `current_signal` forever with no trace and no callback — the frontend just waited
        // out its whole backstop and reported "timed out waiting for the core", which says nothing
        // about why. Resolve it here instead: the request was never processed, and the core may well
        // be powered on again by the next attempt, so it is a transient failure.
        Signal pending{Signal::None};
        {
            std::scoped_lock lock{signal_mutex};
            if (current_signal == Signal::Save || current_signal == Signal::Load) {
                pending = current_signal;
                current_signal = Signal::None;
            }
        }
        if (pending != Signal::None) {
            LOG_ERROR(Core, "Discarding queued {} — the core is not powered on", SignalName(pending));
            status_details = "Core is not powered on";
            NotifySaveStateResult(pending == Signal::Load, SaveStateOutcome::TransientFailure,
                                  status_details);
        }
        // Same hole one stage later: a request already consumed into `save_state_request_status`
        // (the resolution block below is unreachable from here) would otherwise pend forever —
        // uncancellable, unreported, and blocking every future Save/Load with "has not finished
        // yet". Resolve it the same way.
        if (save_state_request_status != SaveStateStatus::NONE) {
            const bool was_loading = save_state_request_status == SaveStateStatus::LOADING;
            save_state_request_status = SaveStateStatus::NONE;
            save_state_request_pending.store(false, std::memory_order_relaxed);
            LOG_ERROR(Core, "Discarding pending {} — the core is not powered on",
                      was_loading ? "Load" : "Save");
            status_details = "Core is not powered on";
            NotifySaveStateResult(was_loading, SaveStateOutcome::TransientFailure, status_details);
        }
        return ResultStatus::ErrorNotInitialized;
    }

    if (GDBStub::IsServerEnabled()) {
        Kernel::Thread* thread = kernel->GetCurrentThreadManager().GetCurrentThread();
        if (thread && running_core) {
            running_core->SaveContext(thread->context);
        }
        GDBStub::HandlePacket(*this);

        // If the loop is halted and we want to step, use a tiny (1) number of instructions to
        // execute. Otherwise, get out of the loop function.
        if (GDBStub::GetCpuHaltFlag()) {
            if (GDBStub::GetCpuStepFlag()) {
                tight_loop = false;
            } else {
                return ResultStatus::Success;
            }
        }
    }

    // A frontend cancel outranks a pending request: it is only ever sent after the frontend's own
    // watchdog gave up, and executing the operation anyway (possibly minutes later, mid-session)
    // is strictly worse than dropping it. Checked BEFORE the signal consume below, for two
    // reasons: a stale latch must never kill a request queued AFTER the cancel (that next request
    // is the manual pause-menu load the failure toast points the user at), and this block's early
    // return must not swallow a signal already moved into a local.
    if (save_state_cancel_requested.exchange(false, std::memory_order_relaxed) &&
        save_state_request_status != SaveStateStatus::NONE) {
        const bool was_loading = save_state_request_status == SaveStateStatus::LOADING;
        save_state_request_status = SaveStateStatus::NONE;
        save_state_request_pending.store(false, std::memory_order_relaxed);
        LOG_ERROR(Core, "Cancelled pending {} at the frontend's request",
                  was_loading ? "Load" : "Save");
        status_details = "Cancelled by the frontend";
        NotifySaveStateResult(was_loading, SaveStateOutcome::TransientFailure, status_details);
        return ResultStatus::ErrorSavestate;
    }

    Signal signal{Signal::None};
    u32 param{};
    {
        std::scoped_lock lock{signal_mutex};
        if (current_signal != Signal::None) {
            signal = current_signal;
            param = signal_param;
            current_signal = Signal::None;
            if (signal == Signal::Save || signal == Signal::Load) {
                // Published inside the same critical section that clears the signal, so a
                // concurrent `CancelPendingSaveStateOperation` always observes either the queued
                // signal or the pending mirror — never the gap between them.
                save_state_request_pending.store(true, std::memory_order_relaxed);
            }
        }
    }
    switch (signal) {
    case Signal::Reset: {
        if (app_loader && app_loader->DoingInitialSetup()) {
            // Treat reset as shutdown if we are doing the initial setup
            return ResultStatus::ShutdownRequested;
        }
        Reset();
        return ResultStatus::Success;
    }
    case Signal::Shutdown:
        return ResultStatus::ShutdownRequested;
    case Signal::Load: {
        if (save_state_request_status != SaveStateStatus::NONE) {
            LOG_ERROR(Core, "A pending save state operation has not finished yet");
            status_details = "A pending save state operation has not finished yet";
            NotifySaveStateResult(true, SaveStateOutcome::TransientFailure, status_details);
            return ResultStatus::ErrorSavestate;
        }
        save_state_slot = param;
        save_state_request_time = std::chrono::steady_clock::now();
        save_state_request_status = SaveStateStatus::LOADING;
        break;
    }
    case Signal::Save: {
        if (save_state_request_status != SaveStateStatus::NONE) {
            LOG_ERROR(Core, "A pending save state operation has not finished yet");
            status_details = "A pending save state operation has not finished yet";
            NotifySaveStateResult(false, SaveStateOutcome::TransientFailure, status_details);
            return ResultStatus::ErrorSavestate;
        }
        save_state_slot = param;
        save_state_request_time = std::chrono::steady_clock::now();
        save_state_request_status = SaveStateStatus::SAVING;
        break;
    }
    default:
        break;
    }

    // Publish the diagnostics snapshot while this thread may still touch `kernel` — the frontend
    // reads it from another thread mid-load, when `kernel` can be gone (see SaveStateDiagnostics).
    // Gated so the idle path costs one relaxed load per iteration.
    if (save_state_request_status != SaveStateStatus::NONE ||
        diag_request_status.load(std::memory_order_relaxed) != 0) {
        diag_request_status.store(static_cast<u32>(save_state_request_status),
                                  std::memory_order_relaxed);
        diag_async_pending.store(kernel ? kernel->GetPendingAsyncOperationCount() : -1,
                                 std::memory_order_relaxed);
        diag_request_age_ms.store(
            save_state_request_status == SaveStateStatus::NONE
                ? -1
                : std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - save_state_request_time)
                      .count(),
            std::memory_order_relaxed);
    }

    if (save_state_request_status == SaveStateStatus::LOADING && kernel.get() &&
        !kernel->AreAsyncOperationsPending()) {
        const u32 slot = save_state_slot;
        save_state_request_status = SaveStateStatus::NONE;
        save_state_request_pending.store(false, std::memory_order_relaxed);
        LOG_INFO(Core, "Begin load of slot {}", slot);
        // The only observable trace of the load while it runs: request status is already cleared,
        // and this thread won't return from RunLoop until the load is done (frame counters
        // freeze). The frontend watchdog reads this to keep waiting instead of declaring a
        // still-running load dead. Scope guard so a throwing deserialize can't leave it stuck.
        save_state_executing.store(true, std::memory_order_relaxed);
        SCOPE_EXIT({ save_state_executing.store(false, std::memory_order_relaxed); });
        try {
            System::LoadState(slot);
            LOG_INFO(Core, "Load completed");
        } catch (const std::exception& e) {
            LOG_ERROR(Core, "Error loading: {}", e.what());
            status_details = e.what();
            // xappify fork: the load branch of `serialize` runs Shutdown+Init BEFORE
            // deserializing, so a throw mid-deserialize leaves a rebuilt, powered-on system whose
            // pad-update events were never armed (the Init-time ScheduleEvent calls no-op'd
            // against the locked queue, and the success-path rearm below never ran) — video keeps
            // presenting, input is dead. The serialize SCOPE_EXIT has already unlocked the queue
            // by the time this catch runs, so scheduling works again. Best-effort: a throw can
            // leave service_manager partially deserialized, so swallow secondary failures.
            try {
                if (kernel.get() && service_manager) {
                    if (auto hid = Service::HID::GetModule(*this)) {
                        hid->RearmUpdateEvents();
                    }
                    if (auto ir_rst = service_manager->GetService<Service::IR::IR_RST>("ir:rst")) {
                        ir_rst->RearmUpdateEvent();
                    }
                }
            } catch (...) {
                LOG_WARNING(Core, "Input pump rearm after failed savestate load threw; skipped");
            }
            // The state was read and rejected (validation, truncated payload, deserialize) — the
            // same file will fail identically next time, so tell the frontend not to retry it.
            NotifySaveStateResult(true, SaveStateOutcome::PermanentFailure, status_details);
            return ResultStatus::ErrorSavestate;
        }
        // Success carries the phase breakdown, so a slow-but-working resume is diagnosable from the
        // frontend log alone.
        NotifySaveStateResult(true, SaveStateOutcome::Success, save_state_timings);
        frame_limiter.WaitOnce();
        return ResultStatus::Success;
    } else if (save_state_request_status == SaveStateStatus::SAVING && kernel.get() &&
               !kernel->AreAsyncOperationsPending()) {
        save_state_request_status = SaveStateStatus::NONE;
        save_state_request_pending.store(false, std::memory_order_relaxed);
        const u32 slot = save_state_slot;
        LOG_INFO(Core, "Begin save to slot {}", slot);
        save_state_executing.store(true, std::memory_order_relaxed);
        SCOPE_EXIT({ save_state_executing.store(false, std::memory_order_relaxed); });
        try {
            System::SaveState(slot);
            LOG_INFO(Core, "Save completed");
        } catch (const std::exception& e) {
            LOG_ERROR(Core, "Error saving: {}", e.what());
            status_details = e.what();
            NotifySaveStateResult(false, SaveStateOutcome::PermanentFailure, status_details);
            return ResultStatus::ErrorSavestate;
        }
        NotifySaveStateResult(false, SaveStateOutcome::Success, save_state_timings);
        frame_limiter.WaitOnce();
        return ResultStatus::Success;
    } else if (save_state_request_status != SaveStateStatus::NONE &&
               (std::chrono::steady_clock::now() - save_state_request_time) >
                   std::chrono::seconds(5)) {
        const bool was_loading = save_state_request_status == SaveStateStatus::LOADING;
        save_state_request_status = SaveStateStatus::NONE;
        save_state_request_pending.store(false, std::memory_order_relaxed);
        LOG_ERROR(Core, "Cannot perform save state operation due to pending async operations");
        status_details = "Cannot perform save state operation due to pending async operations";
        // Never processed — the async ops may well have drained by the next attempt.
        NotifySaveStateResult(was_loading, SaveStateOutcome::TransientFailure, status_details);
        return ResultStatus::ErrorSavestate;
    }

    // All cores should have executed the same amount of ticks. If this is not the case an event was
    // scheduled with a cycles_into_future smaller then the current downcount.
    // So we have to get those cores to the same global time first
    u64 global_ticks = timing->GetGlobalTicks();
    s64 max_delay = 0;
    ARM_Interface* current_core_to_execute = nullptr;
    for (auto& cpu_core : cpu_cores) {
        if (cpu_core->GetTimer().GetTicks() < global_ticks) {
            s64 delay = global_ticks - cpu_core->GetTimer().GetTicks();
            running_core = cpu_core.get();
            kernel->SetRunningCPU(running_core);
            cpu_core->GetTimer().Advance();
            cpu_core->PrepareReschedule();
            kernel->GetThreadManager(cpu_core->GetID()).Reschedule();
            cpu_core->GetTimer().SetNextSlice(delay);
            if (max_delay < delay) {
                max_delay = delay;
                current_core_to_execute = cpu_core.get();
            }
        }
    }

    // jit sometimes overshoot by a few ticks which might lead to a minimal desync in the cores.
    // This small difference shouldn't make it necessary to sync the cores and would only cost
    // performance. Thus we don't sync delays below min_delay
    static constexpr s64 min_delay = 100;
    if (max_delay > min_delay) {
        LOG_TRACE(Core_ARM11, "Core {} running (delayed) for {} ticks",
                  current_core_to_execute->GetID(),
                  current_core_to_execute->GetTimer().GetDowncount());
        if (running_core != current_core_to_execute) {
            running_core = current_core_to_execute;
            kernel->SetRunningCPU(running_core);
        }
        if (kernel->GetCurrentThreadManager().GetCurrentThread() == nullptr) {
            LOG_TRACE(Core_ARM11, "Core {} idling", current_core_to_execute->GetID());
            current_core_to_execute->GetTimer().Idle();
            PrepareReschedule();
        } else {
            if (tight_loop) {
                current_core_to_execute->Run();
            } else {
                current_core_to_execute->Step();
            }
        }
    } else {
        // Now all cores are at the same global time. So we will run them one after the other
        // with a max slice that is the minimum of all max slices of all cores
        // TODO: Make special check for idle since we can easily revert the time of idle cores
        s64 max_slice = Timing::MAX_SLICE_LENGTH;
        for (const auto& cpu_core : cpu_cores) {
            running_core = cpu_core.get();
            kernel->SetRunningCPU(running_core);
            cpu_core->GetTimer().Advance();
            cpu_core->PrepareReschedule();
            kernel->GetThreadManager(cpu_core->GetID()).Reschedule();
            max_slice = std::min(max_slice, cpu_core->GetTimer().GetMaxSliceLength());
        }
        for (auto& cpu_core : cpu_cores) {
            cpu_core->GetTimer().SetNextSlice(max_slice);
            auto start_ticks = cpu_core->GetTimer().GetTicks();
            LOG_TRACE(Core_ARM11, "Core {} running for {} ticks", cpu_core->GetID(),
                      cpu_core->GetTimer().GetDowncount());
            running_core = cpu_core.get();
            kernel->SetRunningCPU(running_core);
            // If we don't have a currently active thread then don't execute instructions,
            // instead advance to the next event and try to yield to the next thread
            if (kernel->GetCurrentThreadManager().GetCurrentThread() == nullptr) {
                LOG_TRACE(Core_ARM11, "Core {} idling", cpu_core->GetID());
                cpu_core->GetTimer().Idle();
                PrepareReschedule();
            } else {
                if (tight_loop) {
                    cpu_core->Run();
                } else {
                    cpu_core->Step();
                }
            }
            max_slice = cpu_core->GetTimer().GetTicks() - start_ticks;
        }
    }

    if (GDBStub::IsServerEnabled()) {
        GDBStub::SetCpuStepFlag(false);
    }

    Reschedule();

    return status;
}

/// xappify fork addition — see `System::SaveStateCallback` in core.h. Usually the emulation thread
/// (RunLoop resolution) but also the frontend thread (a cancel); the frontend is responsible for
/// marshalling to its own queue either way.
void System::NotifySaveStateResult(bool is_load, SaveStateOutcome outcome,
                                   const std::string& details) {
    SaveStateCallback callback;
    {
        std::scoped_lock lock{save_state_callback_mutex};
        // Logged unconditionally, including when no callback is installed: outcomes were being lost
        // between here and the frontend, and this is the first of three probes (here → the ObjC
        // trampoline → the Swift adapter) that says which hop drops one.
        LOG_INFO(Core,
                 "NotifySaveStateResult is_load={} outcome={} callback_installed={} details='{}'",
                 is_load, static_cast<int>(outcome), static_cast<bool>(save_state_callback),
                 details);
        if (!save_state_callback) {
            // No handler yet (boot-queued request resolving before the frontend wired up) — latch
            // instead of dropping; `SetSaveStateCallback` delivers it. Newest outcome wins.
            // No early return: the failure flush below must still run.
            undelivered_save_state_outcome = std::make_tuple(is_load, outcome, details);
        } else {
            callback = save_state_callback;
        }
    }
    if (outcome != SaveStateOutcome::Success) {
        // Failures are exactly what the frontend's log-tail dumps need to show — flush so the
        // line is on disk before the app reads the file. OUTSIDE the mutex: FlushBackends is a
        // handshake with the logging thread that can wait up to 2s, and holding
        // save_state_callback_mutex across it would stall the frontend thread's
        // SetSaveStateCallback/Notify calls for the duration. Queue FIFO still guarantees the
        // lines above are on disk before the callback below runs.
        Common::Log::FlushBackends();
    }
    // Outside the lock: the callback may be arbitrarily slow, and must be free to call back into
    // `SetSaveStateCallback` without deadlocking.
    if (callback) {
        callback(is_load, outcome, details);
    }
}

void System::SetSaveStateCallback(SaveStateCallback callback) {
    SaveStateCallback installed;
    std::optional<std::tuple<bool, SaveStateOutcome, std::string>> latched;
    {
        std::scoped_lock lock{save_state_callback_mutex};
        save_state_callback = std::move(callback);
        if (save_state_callback) {
            latched.swap(undelivered_save_state_outcome);
            installed = save_state_callback;
        } else {
            // Uninstalling drops any latch — a stale outcome must not greet the NEXT session's
            // handler as if it were fresh.
            undelivered_save_state_outcome.reset();
        }
    }
    if (latched) {
        const auto& [is_load, outcome, details] = *latched;
        LOG_INFO(Core, "Delivering latched save-state outcome is_load={} outcome={}", is_load,
                 static_cast<int>(outcome));
        installed(is_load, outcome, details);
    }
}

void System::CancelPendingSaveStateOperation() {
    bool cancelled_queued_load = false;
    bool notify = false;
    {
        std::scoped_lock lock{signal_mutex};
        if (current_signal == Signal::Load || current_signal == Signal::Save) {
            cancelled_queued_load = current_signal == Signal::Load;
            current_signal = Signal::None;
            notify = true;
        } else if (save_state_request_pending.load(std::memory_order_relaxed)) {
            // Consumed but not yet executed — ask RunLoop to resolve it instead of racing the
            // emulation-thread-owned status field. The mirror is set in the same critical section
            // that clears the signal, so falling through BOTH branches genuinely means nothing is
            // in flight.
            save_state_cancel_requested.store(true, std::memory_order_relaxed);
        }
        // else: nothing queued, nothing pending. The operation either already resolved or is
        // executing right now — and an executing load cannot be cancelled. Arming the latch here
        // would only kill a FUTURE request (historically: the manual pause-menu load the resume
        // failure toast tells the user to try), so deliberately do nothing.
    }
    // Notify outside `signal_mutex`: the callback chain must stay free to call back into this
    // object without deadlocking.
    if (notify) {
        LOG_ERROR(Core, "Cancelled queued {} at the frontend's request",
                  cancelled_queued_load ? "Load" : "Save");
        NotifySaveStateResult(cancelled_queued_load, SaveStateOutcome::TransientFailure,
                              "Cancelled by the frontend");
    }
}

/// Human-readable signal name for the failure details the frontend surfaces. `Signal` has no
/// fmt formatter, so it would otherwise print as a bare integer.
static const char* SignalName(System::Signal signal) {
    switch (signal) {
    case System::Signal::None:
        return "None";
    case System::Signal::Shutdown:
        return "Shutdown";
    case System::Signal::Reset:
        return "Reset";
    case System::Signal::Save:
        return "Save";
    case System::Signal::Load:
        return "Load";
    }
    return "Unknown";
}

bool System::SendSignal(System::Signal signal, u32 param) {
    std::scoped_lock lock{signal_mutex};
    if (current_signal != signal && current_signal != Signal::None) {
        LOG_ERROR(Core, "Unable to {} as {} is ongoing", SignalName(signal),
                  SignalName(current_signal));
        // A refused Save/Load never reaches RunLoop's resolution block, so report it here or the
        // request vanishes without a trace (the caller only ever sees the bool we return, and the
        // iOS wrapper historically discarded even that).
        if (signal == Signal::Load || signal == Signal::Save) {
            NotifySaveStateResult(signal == Signal::Load, SaveStateOutcome::TransientFailure,
                                  fmt::format("Unable to {} as {} is ongoing", SignalName(signal),
                                              SignalName(current_signal)));
        }
        return false;
    }
    current_signal = signal;
    signal_param = param;
    return true;
}

void System::RestampPendingSaveStateDeadline() {
    // Emulation thread only — `save_state_request_status`/`save_state_request_time` are owned by
    // it. The iOS loop calls this right after waking from its pause wait, so a request that sat
    // parked through a backgrounding gets a fresh 5s async-ops budget instead of arriving
    // pre-expired (async ops can only drain on this thread, so the stale-deadline failure was
    // deterministic).
    if (save_state_request_status != SaveStateStatus::NONE) {
        save_state_request_time = std::chrono::steady_clock::now();
        LOG_INFO(Core, "Restamped pending {} deadline after pause",
                 save_state_request_status == SaveStateStatus::LOADING ? "Load" : "Save");
    }
}

std::string System::SaveStateDiagnostics() {
    Signal queued;
    {
        std::scoped_lock lock{signal_mutex};
        queued = current_signal;
    }
    const auto request =
        static_cast<SaveStateStatus>(diag_request_status.load(std::memory_order_relaxed));
    const char* request_name = request == SaveStateStatus::LOADING  ? "LOADING"
                               : request == SaveStateStatus::SAVING ? "SAVING"
                                                                    : "NONE";
    return fmt::format("signal={} request={} executing={} pending={} cancel_latched={} "
                       "async_ops={} request_age_ms={} powered_on={}",
                       SignalName(queued), request_name,
                       save_state_executing.load(std::memory_order_relaxed),
                       save_state_request_pending.load(std::memory_order_relaxed),
                       save_state_cancel_requested.load(std::memory_order_relaxed),
                       diag_async_pending.load(std::memory_order_relaxed),
                       diag_request_age_ms.load(std::memory_order_relaxed), IsPoweredOn());
}

System::ResultStatus System::SingleStep() {
    return RunLoop(false);
}

System::ResultStatus System::Load(Frontend::EmuWindow& emu_window, const std::string& filepath,
                                  Frontend::EmuWindow* secondary_window) {
    Settings::ResetTemporaryFrameLimit();
    FileUtil::SetCurrentRomPath(filepath);
    if (early_app_loader) {
        app_loader = std::move(early_app_loader);
    } else {
        app_loader = Loader::GetLoader(filepath);
    }
    if (!app_loader) {
        LOG_CRITICAL(Core, "Failed to obtain loader for {}!", filepath);
        return ResultStatus::ErrorGetLoader;
    }

    u64_le program_id = 0;
    app_loader->ReadProgramId(program_id);
    if (restore_plugin_context.has_value() && restore_plugin_context->is_enabled &&
        restore_plugin_context->use_user_load_parameters) {
        if (restore_plugin_context->user_load_parameters.low_title_Id ==
                static_cast<u32_le>(program_id) &&
            restore_plugin_context->user_load_parameters.plugin_memory_strategy ==
                Service::PLGLDR::PLG_LDR::PluginMemoryStrategy::PLG_STRATEGY_MODE3) {
            app_loader->SetKernelMemoryModeOverride(Kernel::MemoryMode::Dev2);
        }
    }

    Kernel::MemoryMode app_mem_mode;
    Kernel::MemoryMode system_mem_mode;
    bool used_default_mem_mode = false;
    Kernel::New3dsHwCapabilities app_n3ds_hw_capabilities;

    if (m_mem_mode) {
        // Use memory mode set by the FIRM launch parameters
        system_mem_mode = static_cast<Kernel::MemoryMode>(m_mem_mode.value());
        m_mem_mode = {};
    } else {
        // Use default memory mode based on the n3ds setting
        system_mem_mode = Settings::values.is_new_3ds.GetValue() ? Kernel::MemoryMode::NewProd
                                                                 : Kernel::MemoryMode::Prod;
        used_default_mem_mode = true;
    }

    {
        auto memory_mode = app_loader->LoadKernelMemoryMode();
        if (memory_mode.second != Loader::ResultStatus::Success) {
            LOG_CRITICAL(Core, "Failed to determine system mode (Error {})!",
                         static_cast<int>(memory_mode.second));

            switch (memory_mode.second) {
            case Loader::ResultStatus::ErrorEncrypted:
                return ResultStatus::ErrorLoader_ErrorEncrypted;
            case Loader::ResultStatus::ErrorInvalidFormat:
                return ResultStatus::ErrorLoader_ErrorInvalidFormat;
            case Loader::ResultStatus::ErrorGbaTitle:
                return ResultStatus::ErrorLoader_ErrorGbaTitle;
            case Loader::ResultStatus::ErrorArtic:
                return ResultStatus::ErrorArticDisconnected;
            default:
                return ResultStatus::ErrorSystemMode;
            }
        }

        ASSERT(memory_mode.first);
        app_mem_mode = memory_mode.first.value();
    }

    auto n3ds_hw_caps = app_loader->LoadNew3dsHwCapabilities();
    ASSERT(n3ds_hw_caps.first);
    app_n3ds_hw_capabilities = n3ds_hw_caps.first.value();

    if (!Settings::values.is_new_3ds.GetValue() && app_loader->IsN3DSExclusive()) {
        return ResultStatus::ErrorN3DSApplication;
    }

    // If the default mem mode has been used, we do not come from a FIRM launch. On real HW
    // however, the home menu is in charge or setting the proper memory mode when launching
    // applications by doing a FIRM launch. Since we launch the application without going
    // through the home menu, we need to emulate the FIRM launch having happened and set the
    // proper memory mode.
    if (used_default_mem_mode) {

        // If we are on the Old 3DS prod mode and the application memory mode does not match, we
        // need to adjust it. We do not need adjustment if we are on the New 3DS prod mode, as that
        // one overrides all the Old 3DS memory modes.
        if (system_mem_mode == Kernel::MemoryMode::Prod && app_mem_mode != system_mem_mode) {
            system_mem_mode = app_mem_mode;
        }

        // If we are on the New 3DS prod mode, and the application needs the New 3DS extended
        // memory mode (only CTRAging is known to do this), adjust the memory mode.
        else if (system_mem_mode == Kernel::MemoryMode::NewProd &&
                 app_n3ds_hw_capabilities.memory_mode == Kernel::New3dsMemoryMode::NewDev1) {
            system_mem_mode = Kernel::MemoryMode::NewDev1;
        }
    }

    u32 num_cores = 2;
    if (Settings::values.is_new_3ds) {
        num_cores = 4;
    }
    ResultStatus init_result{Init(emu_window, secondary_window, system_mem_mode, num_cores)};
    if (init_result != ResultStatus::Success) {
        LOG_CRITICAL(Core, "Failed to initialize system (Error {})!",
                     static_cast<u32>(init_result));
        System::Shutdown();
        return init_result;
    }

    kernel->UpdateCPUAndMemoryState(program_id, app_mem_mode, app_n3ds_hw_capabilities);

    // Restore any parameters that should be carried through a reset.
    if (auto apt = Service::APT::GetModule(*this)) {
        if (restore_deliver_arg.has_value()) {
            apt->GetAppletManager()->SetDeliverArg(restore_deliver_arg);
            restore_deliver_arg.reset();
        }
        if (restore_sys_menu_arg.has_value()) {
            apt->GetAppletManager()->SetSysMenuArg(restore_sys_menu_arg.value());
            restore_sys_menu_arg.reset();
        }
        apt->SetWirelessRebootInfoBuffer(restore_wireless_reboot_info);
    }

    if (restore_plugin_context.has_value()) {
        if (auto plg_ldr = Service::PLGLDR::GetService(*this)) {
            plg_ldr->SetPluginLoaderContext(restore_plugin_context.value());
        }
        restore_plugin_context.reset();
    }

    if (restore_ipc_recorder) {
        kernel->RestoreIPCRecorder(std::move(restore_ipc_recorder));
    }

    std::shared_ptr<Kernel::Process> process;
    const Loader::ResultStatus load_result{app_loader->Load(process)};
    if (Loader::ResultStatus::Success != load_result) {
        LOG_CRITICAL(Core, "Failed to load ROM (Error {})!", load_result);
        // Release the partially-created process while the kernel and memory subsystems
        // are still alive. Shutdown() resets kernel then memory, and ~Process calls
        // kernel.memory.UnregisterPageTable() — holding this ref past Shutdown()
        // dereferences freed subsystems (EXC_BAD_ACCESS).
        process.reset();
        System::Shutdown();

        switch (load_result) {
        case Loader::ResultStatus::ErrorEncrypted:
            return ResultStatus::ErrorLoader_ErrorEncrypted;
        case Loader::ResultStatus::ErrorInvalidFormat:
            return ResultStatus::ErrorLoader_ErrorInvalidFormat;
        case Loader::ResultStatus::ErrorGbaTitle:
            return ResultStatus::ErrorLoader_ErrorGbaTitle;
        case Loader::ResultStatus::ErrorArtic:
            return ResultStatus::ErrorArticDisconnected;
        default:
            return ResultStatus::ErrorLoader;
        }
    }
    kernel->SetCurrentProcess(process);
    title_id = 0;
    if (app_loader->ReadProgramId(title_id) != Loader::ResultStatus::Success) {
        LOG_ERROR(Core, "Failed to find title id for ROM (Error {})",
                  static_cast<u32>(load_result));
    }

    cheat_engine.LoadCheatFile(title_id);
    cheat_engine.Connect(process->process_id);

    perf_stats = std::make_unique<PerfStats>(title_id);

    if (Settings::values.dump_textures) {
        custom_tex_manager->PrepareDumping(title_id);
    }
    if (Settings::values.custom_textures) {
        custom_tex_manager->FindCustomTextures();
    }

    status = ResultStatus::Success;
    m_emu_window = &emu_window;
    m_secondary_window = secondary_window;
    m_filepath = filepath;

    // Reset counters and set time origin to current frame
    [[maybe_unused]] const PerfStats::Results result = GetAndResetPerfStats();
    perf_stats->BeginSystemFrame();
    return status;
}

void System::PrepareReschedule() {
    running_core->PrepareReschedule();
    reschedule_pending = true;
}

PerfStats::Results System::GetAndResetPerfStats() {
    return (perf_stats && timing) ? perf_stats->GetAndResetStats(timing->GetGlobalTimeUs())
                                  : PerfStats::Results{};
}

PerfStats::Results System::GetLastPerfStats() {
    return perf_stats ? perf_stats->GetLastStats() : PerfStats::Results{};
}

double System::GetStableFrameTimeScale() {
    return perf_stats->GetStableFrameTimeScale();
}

void System::Reschedule() {
    if (!reschedule_pending) {
        return;
    }

    reschedule_pending = false;
    for (const auto& core : cpu_cores) {
        LOG_TRACE(Core_ARM11, "Reschedule core {}", core->GetID());
        kernel->GetThreadManager(core->GetID()).Reschedule();
    }
}

System::ResultStatus System::Init(Frontend::EmuWindow& emu_window,
                                  Frontend::EmuWindow* secondary_window,
                                  Kernel::MemoryMode memory_mode, u32 num_cores) {
    LOG_DEBUG(HW_Memory, "initialized OK");

    memory = std::make_unique<Memory::MemorySystem>(*this);

    timing = std::make_unique<Timing>(num_cores, Settings::values.cpu_clock_percentage.GetValue(),
                                      movie.GetOverrideBaseTicks());

    kernel = std::make_unique<Kernel::KernelSystem>(
        *memory, *timing, [this] { PrepareReschedule(); }, memory_mode, num_cores,
        movie.GetOverrideInitTime());

    exclusive_monitor = MakeExclusiveMonitor(*memory, num_cores);
    cpu_cores.reserve(num_cores);
    if (Settings::values.use_cpu_jit) {
#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)
        for (u32 i = 0; i < num_cores; ++i) {
            cpu_cores.push_back(std::make_shared<ARM_Dynarmic>(
                *this, *memory, i, timing->GetTimer(i), *exclusive_monitor));
        }
#else
        for (u32 i = 0; i < num_cores; ++i) {
            cpu_cores.push_back(
                std::make_shared<ARM_DynCom>(*this, *memory, USER32MODE, i, timing->GetTimer(i)));
        }
        LOG_WARNING(Core, "CPU JIT requested, but Dynarmic not available");
#endif
    } else {
        for (u32 i = 0; i < num_cores; ++i) {
            cpu_cores.push_back(
                std::make_shared<ARM_DynCom>(*this, *memory, USER32MODE, i, timing->GetTimer(i)));
        }
    }
    running_core = cpu_cores[0].get();

    kernel->SetCPUs(cpu_cores);
    kernel->SetRunningCPU(cpu_cores[0].get());

    const auto audio_emulation = Settings::values.audio_emulation.GetValue();
    if (audio_emulation == Settings::AudioEmulation::HLE) {
        dsp_core = std::make_unique<AudioCore::DspHle>(*this);
    } else {
        const bool multithread = audio_emulation == Settings::AudioEmulation::LLEMultithreaded;
        dsp_core = std::make_unique<AudioCore::DspLle>(*this, multithread);
    }

    dsp_core->SetSink(Settings::values.output_type.GetValue(),
                      Settings::values.output_device.GetValue());
    dsp_core->EnableStretching(Settings::values.enable_audio_stretching.GetValue());

#ifdef ENABLE_SCRIPTING
    if (Settings::values.enable_rpc_server.GetValue()) {
        rpc_server = std::make_unique<RPC::Server>(*this);
    }
#endif

    service_manager = std::make_unique<Service::SM::ServiceManager>(*this);
    archive_manager = std::make_unique<Service::FS::ArchiveManager>(*this);

    u64 loading_title_id = 0;
    app_loader->ReadProgramId(loading_title_id);
    HW::AES::InitKeys();
    Service::Init(*this, loading_title_id, lle_modules, !app_loader->DoingInitialSetup());
    GDBStub::DeferStart();

    if (!registered_image_interface) {
        registered_image_interface = std::make_shared<Frontend::ImageInterface>();
    }

    custom_tex_manager = std::make_unique<VideoCore::CustomTexManager>(*this);

    auto gsp = service_manager->GetService<Service::GSP::GSP_GPU>("gsp::Gpu");
    gpu = std::make_unique<VideoCore::GPU>(*this, emu_window, secondary_window);
    gpu->SetInterruptHandler(
        [gsp](Service::GSP::InterruptId interrupt_id) { gsp->SignalInterrupt(interrupt_id); });

    auto plg_ldr = Service::PLGLDR::GetService(*this);
    if (plg_ldr) {
        plg_ldr->SetEnabled(Settings::values.plugin_loader_enabled.GetValue());
        plg_ldr->SetAllowGameChangeState(Settings::values.allow_plugin_loader.GetValue());
    }

    LOG_DEBUG(Core, "Initialized OK");

    is_powered_on = true;

    return ResultStatus::Success;
}

VideoCore::GPU& System::GPU() {
    return *gpu;
}

Service::SM::ServiceManager& System::ServiceManager() {
    return *service_manager;
}

const Service::SM::ServiceManager& System::ServiceManager() const {
    return *service_manager;
}

Service::FS::ArchiveManager& System::ArchiveManager() {
    return *archive_manager;
}

const Service::FS::ArchiveManager& System::ArchiveManager() const {
    return *archive_manager;
}

Kernel::KernelSystem& System::Kernel() {
    return *kernel;
}

const Kernel::KernelSystem& System::Kernel() const {
    return *kernel;
}

bool System::KernelRunning() {
    return kernel != nullptr;
}

Timing& System::CoreTiming() {
    return *timing;
}

const Timing& System::CoreTiming() const {
    return *timing;
}

Memory::MemorySystem& System::Memory() {
    return *memory;
}

const Memory::MemorySystem& System::Memory() const {
    return *memory;
}

Cheats::CheatEngine& System::CheatEngine() {
    return cheat_engine;
}

const Cheats::CheatEngine& System::CheatEngine() const {
    return cheat_engine;
}

void System::RegisterVideoDumper(std::shared_ptr<VideoDumper::Backend> dumper) {
    video_dumper = std::move(dumper);
}

VideoCore::CustomTexManager& System::CustomTexManager() {
    return *custom_tex_manager;
}

const VideoCore::CustomTexManager& System::CustomTexManager() const {
    return *custom_tex_manager;
}

Core::Movie& System::Movie() {
    return movie;
}

const Core::Movie& System::Movie() const {
    return movie;
}

void System::RegisterMiiSelector(std::shared_ptr<Frontend::MiiSelector> mii_selector) {
    registered_mii_selector = std::move(mii_selector);
}

void System::RegisterSoftwareKeyboard(std::shared_ptr<Frontend::SoftwareKeyboard> swkbd) {
    registered_swkbd = std::move(swkbd);
}

void System::RegisterImageInterface(std::shared_ptr<Frontend::ImageInterface> image_interface) {
    registered_image_interface = std::move(image_interface);
}

void System::Shutdown(bool is_deserializing) {

    // Shutdown emulation session
    is_powered_on = false;

    gpu.reset();
    if (!is_deserializing) {
        lle_modules.clear();
        GDBStub::Shutdown();
        perf_stats.reset();
        app_loader.reset();
    }
    custom_tex_manager.reset();
#ifdef ENABLE_SCRIPTING
    rpc_server.reset();
#endif
    archive_manager.reset();
    service_manager.reset();
    dsp_core.reset();
    kernel.reset();
    cpu_cores.clear();
    exclusive_monitor.reset();
    timing.reset();

    if (video_dumper && video_dumper->IsDumping()) {
        video_dumper->StopDumping();
    }

    if (auto room_member = Network::GetRoomMember().lock()) {
        Network::GameInfo game_info{};
        room_member->SendGameInfo(game_info);
    }

    memory.reset();

    LOG_DEBUG(Core, "Shutdown OK");
}

void System::Reset() {
    // This is NOT a proper reset, but a temporary workaround by shutting down the system and
    // reloading.
    // TODO: Properly implement the reset

    // Save the APT deliver arg and plugin loader context across resets.
    // This is needed as we don't currently support proper app jumping.
    if (auto apt = Service::APT::GetModule(*this)) {
        restore_deliver_arg = apt->GetAppletManager()->ReceiveDeliverArg();
        restore_sys_menu_arg = apt->GetAppletManager()->GetSysMenuArg();
        restore_wireless_reboot_info = apt->GetWirelessRebootInfoBuffer();
    }
    if (auto plg_ldr = Service::PLGLDR::GetService(*this)) {
        restore_plugin_context = plg_ldr->GetPluginLoaderContext();
    }

    restore_ipc_recorder = std::move(kernel->BackupIPCRecorder());

    Shutdown();

    if (!m_chainloadpath.empty()) {
        m_filepath = m_chainloadpath;
        m_chainloadpath.clear();
    }

    // Reload the system with the same setting
    [[maybe_unused]] const System::ResultStatus result =
        Load(*m_emu_window, m_filepath, m_secondary_window);
}

void System::ApplySettings() {
    GDBStub::SetServerPort(Settings::values.gdbstub_port.GetValue());
    GDBStub::ToggleServer(Settings::values.use_gdbstub.GetValue());

    if (gpu) {
#ifndef ANDROID
        gpu->Renderer().UpdateCurrentFramebufferLayout();
#endif
        auto& settings = gpu->Renderer().Settings();
        settings.bg_color_update_requested = true;
        settings.shader_update_requested = true;
    }

    if (IsPoweredOn()) {
        CoreTiming().UpdateClockSpeed(Settings::values.cpu_clock_percentage.GetValue());
        dsp_core->SetSink(Settings::values.output_type.GetValue(),
                          Settings::values.output_device.GetValue());
        dsp_core->EnableStretching(Settings::values.enable_audio_stretching.GetValue());

        auto hid = Service::HID::GetModule(*this);
        if (hid) {
            hid->ReloadInputDevices();
        }

        auto apt = Service::APT::GetModule(*this);
        if (apt) {
            apt->GetAppletManager()->ReloadInputDevices();
        }

        auto ir_user = service_manager->GetService<Service::IR::IR_USER>("ir:USER");
        if (ir_user)
            ir_user->ReloadInputDevices();
        auto ir_rst = service_manager->GetService<Service::IR::IR_RST>("ir:rst");
        if (ir_rst)
            ir_rst->ReloadInputDevices();

        auto cam = Service::CAM::GetModule(*this);
        if (cam) {
            cam->ReloadCameraDevices();
        }

        Service::MIC::ReloadMic(*this);
    }

    auto plg_ldr = Service::PLGLDR::GetService(*this);
    if (plg_ldr) {
        plg_ldr->SetEnabled(Settings::values.plugin_loader_enabled.GetValue());
        plg_ldr->SetAllowGameChangeState(Settings::values.allow_plugin_loader.GetValue());
    }
}

void System::RegisterAppLoaderEarly(std::unique_ptr<Loader::AppLoader>& loader) {
    early_app_loader = std::move(loader);
}

void System::InsertCartridge(const std::string& path) {
    FileSys::NCCHContainer cartridge_container(path);
    if (cartridge_container.LoadHeader() == Loader::ResultStatus::Success &&
        cartridge_container.IsNCSD()) {
        inserted_cartridge = path;
    }
}

void System::EjectCartridge() {
    inserted_cartridge.clear();
}

bool System::IsInitialSetup() {
    return app_loader && app_loader->DoingInitialSetup();
}

template <class Archive>
void System::serialize(Archive& ar, const unsigned int file_version) {

    if (Archive::is_loading::value) {
        save_state_status = SaveStateStatus::LOADING;
    } else {
        save_state_status = SaveStateStatus::SAVING;
    }

    u32 num_cores;
    if (Archive::is_saving::value) {
        num_cores = this->GetNumCores();
    }
    ar & num_cores;

    // TODO(PabloMK7): Figure out why this is the case
    if (!lle_modules.empty()) {
        throw std::runtime_error("Savestates are not supported with LLE modules enabled");
    }

    ar & lle_modules;
    Kernel::MemoryMode mem_mode{};
    if (!Archive::is_loading::value) {
        mem_mode = kernel->GetMemoryMode();
    }
    ar & mem_mode;

    if (Archive::is_loading::value) {
        // When loading, we want to make sure any lingering state gets cleared out before we begin.
        // Shutdown, but persist a few things between loads...
        //
        // These two lines are the bulk of a savestate load's wall-clock: Shutdown destroys the whole
        // Vulkan device (blocking on the scheduler, the present thread and the pipeline workers) and
        // Init rebuilds it from scratch — new VkInstance/VkDevice, surface, swapchain, glslang
        // compiles, pipeline builds. Timed separately so a slow load can be attributed instead of
        // guessed at.
        const auto shutdown_start = std::chrono::steady_clock::now();
        Shutdown(true);
        const auto shutdown_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - shutdown_start)
                                     .count();

        const auto init_start = std::chrono::steady_clock::now();
        [[maybe_unused]] const System::ResultStatus result =
            Init(*m_emu_window, m_secondary_window, mem_mode, num_cores);
        const auto init_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - init_start)
                                 .count();

        save_state_timings = fmt::format("shutdown {}ms, init {}ms", shutdown_ms, init_ms);
        LOG_INFO(Core, "Savestate load: shutdown {}ms, init {}ms", shutdown_ms, init_ms);
    }

    // Flush on save, don't flush on load
    const bool should_flush = !Archive::is_loading::value;
    gpu->ClearAll(should_flush);
    ar&* timing.get();
    // xappify fork: `Timing::serialize` latches `event_queue_locked` on load, and until now the
    // ONLY unlock was the explicit call far below, after every other subsystem deserialized. A
    // throw anywhere in between (truncated payload, bad_alloc under memory pressure) stranded the
    // lock for the rest of the process: ScheduleEvent becomes a silent no-op, the HID pad-update
    // event never re-arms, and input dies while the already-running render threads keep presenting
    // — the "buttons dead, video alive" wedge. Guarantee the unlock on every exit; it is
    // idempotent, so running after the explicit call on the success path is harmless.
    SCOPE_EXIT({
        if (Archive::is_loading::value) {
            timing->UnlockEventQueue();
        }
    });
    for (u32 i = 0; i < num_cores; i++) {
        ar&* cpu_cores[i].get();
    }
    ar&* service_manager.get();
    ar&* archive_manager.get();

    // NOTE: DSP doesn't like being destroyed and recreated. So instead we do an inline
    // serialization; this means that the DSP Settings need to match for loading to work.
    auto dsp_hle = dynamic_cast<AudioCore::DspHle*>(dsp_core.get());
    if (dsp_hle) {
        ar&* dsp_hle;
    } else {
        throw std::runtime_error("LLE audio not supported for save states");
    }

    ar&* memory.get();
    ar&* kernel.get();
    ar&* gpu.get();
    ar & movie;

    // This needs to be set from somewhere - might as well be here!
    if (Archive::is_loading::value) {
        u32 cheats_pid;
        ar & cheats_pid;
        timing->UnlockEventQueue();
        cheat_engine.Connect(cheats_pid);

        if (Settings::values.custom_textures) {
            custom_tex_manager->FindCustomTextures();
        }

        // Re-register gpu callback, because gsp service changed after service_manager got
        // serialized
        auto gsp = service_manager->GetService<Service::GSP::GSP_GPU>("gsp::Gpu");
        gpu->SetInterruptHandler(
            [gsp](Service::GSP::InterruptId interrupt_id) { gsp->SignalInterrupt(interrupt_id); });

        // xappify fork: re-arm the input pumps, for the same reason the GSP handler above needs
        // re-registering. Their events are scheduled only in the module constructors, and during
        // the load those ScheduleEvent calls were silent no-ops (the queue above deserialized
        // LOCKED, and `ar & timing` had already destroyed the Init-time entries) — so whether
        // input worked after a load used to depend on whether the SAVED queue happened to carry
        // the events. A state saved after a session's pump died is "poisoned": it loads
        // successfully with buttons/touch dead forever, and every auto-save it seeds inherits
        // that. The re-arm makes the pumps unconditional and heals poisoned states on disk.
        if (auto hid = Service::HID::GetModule(*this)) {
            hid->RearmUpdateEvents();
        }
        if (auto ir_rst = service_manager->GetService<Service::IR::IR_RST>("ir:rst")) {
            ir_rst->RearmUpdateEvent();
        }

        // Apply per program settings and switch the shader cache to the title running when the
        // savestate was created.
        // TODO(PabloMK7): Find better way to obtain the program ID.
        const u32 thread_id = gsp->GetActiveClientThreadId();
        if (thread_id != std::numeric_limits<u32>::max()) {
            const auto thread = kernel->GetThreadByID(thread_id);
            if (thread) {
                const std::shared_ptr<Kernel::Process> process = thread->owner_process.lock();
                if (process) {
                    gpu->ApplyPerProgramSettings(process->codeset->program_id);
                    // Rebuilds the per-title disk shader cache from scratch — the same work the BOOT
                    // path shows a progress bar for (`disk_cache_callback`), here with no callback
                    // wired. Frequently the single largest term in a resume.
                    const auto resources_start = std::chrono::steady_clock::now();
                    gpu->Renderer().Rasterizer()->SwitchDiskResources(process->codeset->program_id);
                    const auto resources_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now() - resources_start)
                                                  .count();
                    save_state_timings += fmt::format(", disk-resources {}ms", resources_ms);
                    LOG_INFO(Core, "Savestate load: disk-resources {}ms", resources_ms);
                }
            }
        }
    } else {
        u32 cheats_pid = cheat_engine.GetConnectedPID();
        ar & cheats_pid;
    }

    save_state_status = SaveStateStatus::NONE;
}

SERIALIZE_IMPL(System)

} // namespace Core
