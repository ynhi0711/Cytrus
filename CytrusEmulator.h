//
//  CytrusEmulator.h
//  Cytrus
//
//  Created by Jarrod Norwell on 2/7/2025.
//

#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#import <MetalKit/MetalKit.h>
#import <TargetConditionals.h>
#import <UIKit/UIKit.h>

#import "CheatsManager.h"
#import "GameInformationManager.h"
#import "MultiplayerManager.h"

#ifdef __cplusplus
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "common/file_util.h"
#include "common/string_util.h"
#include "common/dynamic_library/dynamic_library.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/hle/service/am/am.h"
#include "core/hw/unique_data.h"
#include "core/loader/loader.h"
#include "core/frontend/applets/default_applets.h"
#include "core/system_titles.h"
#include "input_common/main.h"
#include "network/network.h"

#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/fs/archive.h"
#include "core/hle/service/ptm/ptm.h"
#include "core/savestate.h"
#include "core/hle/service/nfc/nfc.h"
#include "network/network_settings.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#endif

@class CytrusGameInformation;
@class CytrusSaveState;

/// Mirrors `Core::System::SaveStateOutcome`. Separates failures worth retrying from ones that never
/// will be, so the frontend doesn't have to infer it by matching on the message.
typedef NS_ENUM(NSInteger, CytrusSaveStateOutcome) {
    /// The state was written / applied.
    CytrusSaveStateOutcomeSuccess = 0,
    /// Refused or deferred without ever being processed (another signal in flight, a prior operation
    /// still pending, the async-operations deadline). Retrying later can succeed.
    CytrusSaveStateOutcomeTransientFailure = 1,
    /// Processed and rejected — wrong title, wrong build revision, truncated payload, deserialize or
    /// disk error. Retrying the same file cannot help.
    CytrusSaveStateOutcomePermanentFailure = 2,
};

NS_ASSUME_NONNULL_BEGIN

@interface CytrusEmulator : NSObject {
#ifdef __cplusplus
    std::atomic_bool stop_run;
    std::atomic_bool pause_emulation;

    std::mutex paused_mutex;
    std::mutex running_mutex;
    std::condition_variable running_cv;

    // xappify fork: emulation-thread liveness. See `loopTicks`/`runLoopReturns` below.
    std::atomic_uint64_t loop_ticks;
    std::atomic_uint64_t run_loop_returns;

    // xappify fork: set when the `insert:` run loop has actually unwound. `stopped` used to report
    // `stop_run && pause_emulation`, which `stop` itself makes impossible (it clears
    // pause_emulation) — see `stopped`.
    std::atomic_bool emu_thread_finished;

    // xappify fork: one-shot request for a guest thread dump, consumed by the `insert:` loop.
    // See `requestGuestStateDump`.
    std::atomic_bool dump_guest_state;
    std::atomic_uint64_t guest_state_dumps;
#endif
}

@property (nonatomic, strong) void (^disk_cache_callback) (uint8_t, size_t, size_t);

+(CytrusEmulator *) sharedInstance NS_SWIFT_NAME(shared());

-(void) allocate;
-(void) deallocate;

-(void) top:(CAMetalLayer*)layer size:(CGSize)size;
-(void) bottom:(CAMetalLayer*)layer size:(CGSize)size;
-(void) deinitialize;

-(void) insert:(NSURL *)url withCallback:(void (^)())callback NS_SWIFT_NAME(insert(from:with:));
-(BOOL) installCIA:(NSURL *)url withCallback:(void (^)())callback;

-(NSURL *) bootHome:(NSInteger)region;

-(void) touchBeganAtPoint:(CGPoint)point;
-(void) touchEnded;
-(void) touchMovedAtPoint:(CGPoint)point;

-(BOOL) input:(int)slot button:(uint32_t)button pressed:(BOOL)pressed;

-(void) thumbstickMoved:(uint32_t)analog x:(CGFloat)x y:(CGFloat)y;

-(BOOL) isPaused;
-(void) pause:(BOOL)pause;
-(void) stop;

-(BOOL) running;
/// YES once the `insert:` emulation loop has actually unwound (or was never started). The render
/// surfaces must not be freed before this flips — see `tearDown` on the app side.
-(BOOL) stopped;

-(void) orientationChanged:(UIInterfaceOrientation)orientation metalView:(UIView *)metalView secondary:(BOOL)secondary;

-(NSMutableArray<NSURL *> *) installedGamePaths;
-(NSMutableArray<NSURL *> *) systemGamePaths;

-(void) updateSettings;

-(uint16_t) stepsPerHour;
-(void) setStepsPerHour:(uint16_t)stepsPerHour;

// xappify fork additions (the ManicEMU prebuilt exposed both; the app's pause menu needs them)
-(void) reset;                              // Restart Game — thread-safe RequestReset
-(void) setFrameLimit:(uint16_t)limit NS_SWIFT_NAME(setFrameLimit(_:)); // Fast Forward — percent (100 = normal)

-(BOOL) loadState;
-(BOOL) saveState;

-(BOOL) stateExists:(uint64_t)identifier forSlot:(NSInteger)slot;
// Return value is `SendSignal`'s: YES only means the request was ACCEPTED INTO THE QUEUE, never
// that the state applied. The emulation loop resolves it frames later — observe
// `saveStateHandler` for the actual outcome.
-(BOOL) load:(NSInteger)slot;
-(BOOL) save:(NSInteger)slot;

// xappify fork additions — the truthful save-state channel.
//
// `load:`/`save:` are fire-and-forget, so the app used to report "resumed" the moment the signal
// was posted while the load was silently rejected inside the core (stale build revision, wrong
// title, pending async ops, bad_alloc in the deserialize). This block is invoked once per request
// when it actually resolves, ON THE MAIN QUEUE. `details` is empty on success.
//
// It is also how a SAVE is known to be finished: the compressed payload is written incrementally,
// so the file's existence/size says nothing about completeness until the core publishes it.
// Main-thread only: the emulation thread never reads this property — the install-time capture
// inside `setSaveStateHandler:` is what resolution uses, so cross-thread reads cannot tear.
@property (nonatomic, copy, nullable) void (^saveStateHandler) (BOOL isLoad, CytrusSaveStateOutcome outcome, NSString *details);

/// xappify fork addition. Call when the app's own watchdog gives up waiting on a queued
/// `load:`/`save:`: a still-pending request is dropped (resolving as a transient failure through
/// `saveStateHandler`) so it can never execute arbitrarily late — a load landing minutes after the
/// player moved on warps the session, and one that fails mid-deserialize corrupts it. A request
/// already executing inside the core cannot be cancelled; the call is then a no-op. Thread-safe.
-(void) cancelPendingSaveStateOperation;

/// Title ID of the running application — the value save states are keyed and validated against.
/// 0 when nothing is booted. Lets the app pre-check a .cst before requesting a load.
-(uint64_t) runningTitleID;

/// This build's save-state revision (`Common::g_scm_rev`), as it appears hex-encoded in a .cst
/// header. A state whose embedded revision differs is rejected by the core.
-(NSString *) saveStateRevision;

// xappify fork additions — emulation-thread liveness.
//
// `load:`/`save:` only queue a signal; the emulation loop consumes it on a later frame. When no
// outcome ever arrives, the frontend cannot tell WHY without knowing whether that loop is even
// running. Sampling these two across the wait separates the three failure shapes:
//
//   loopTicks frozen, isPaused == YES  → the unpause never took / something re-paused the core
//   loopTicks frozen, isPaused == NO   → blocked INSIDE RunLoop (presentation, most likely)
//   both advancing, still no outcome   → RunLoop is short-circuiting before the signal switch
//
/// Iterations of the `insert:` run loop — advances while paused too (the pause branch is inside it).
-(uint64_t) loopTicks;
/// Completed `Core::System::RunLoop` calls — advances only while actually emulating.
-(uint64_t) runLoopReturns;

// xappify fork additions — GUEST liveness, which the two counters above cannot report.
//
// Both of those measure the EMULATOR. A wedged title (a guest thread blocked forever on a GPU
// interrupt or a service reply that never arrives) leaves the emulation loop running normally, so
// they keep climbing while the picture is frozen and the audio sink starves into repeating its last
// buffer. Sampling these two separates the cases:
//
//   systemFrames climbing, gameFrames FROZEN → the emulated TITLE is wedged
//   both frozen                              → the emulator stopped (also visible in runLoopReturns)
//   both climbing                            → frames are produced; a freeze is downstream (present)
//
// Monotonic and side-effect free, unlike `Core::System::GetAndResetPerfStats` which clears the
// accumulators it reports. 0 until a title is loaded.

/// LCD VBlanks presented by the emulated system.
-(uint64_t) systemFrames;
/// GSP frame submissions by the guest — freezes the moment the title stops drawing.
-(uint64_t) gameFrames;
/// HID pad-update event firings — the input pump. The pump is a self-rescheduling core-timing
/// event, so it climbs at a fixed rate whenever the core runs, independent of whether the player
/// touches anything:
///   runLoopReturns climbing, padUpdates FROZEN → the input pump is dead (event re-schedule was
///   dropped); buttons/touch go unhandled while everything already running continues.
/// Restarts at 0 when a save-state load rebuilds the HID module — treat a DECREASE as a reanchor.
-(uint64_t) padUpdates;

/// Ask the core to log every guest thread's status and what it is blocked on (see
/// `Kernel::KernelSystem::LogGuestThreadState`). Use when `gameFrames` has gone flat while
/// `systemFrames` keeps climbing — the emulator is fine and the emulated title is wedged, and the
/// guest thread states are the only thing that says why.
///
/// Asynchronous BY DESIGN: the scheduler mutates the thread list on the emulation thread, so
/// walking it from the UI thread would be a data race. This only sets a flag; the `insert:` run
/// loop performs the dump on its next iteration (sub-millisecond in practice). Output goes to the
/// core's log file, not the console — read it back with the frontend's log tail dump.
-(void) requestGuestStateDump;

/// Completed guest-state dumps. `requestGuestStateDump` is asynchronous, so poll this to know the
/// dump actually ran before reading the log back — otherwise "the dump didn't happen" and "the log
/// was read too early" are indistinguishable, which has already cost a debugging round.
-(uint64_t) guestStateDumps;

/// Total swapchain rebuilds this session (see `RendererBase::GetSwapchainRecreations`).
///
/// Sample it alongside `runLoopReturns`: a rebuild takes a queue `waitIdle` under the swapchain
/// lock, so the emulation thread stalls in `GetRenderFrame` for the duration. A run where this
/// climbs steadily has a PRESENTATION problem, and no frame counter says so on its own — the only
/// previous evidence was `[mvk-info]` console spam, which is invisible without Xcode attached.
/// Expect single digits for a whole session: boot, rotations, and foreground returns.
-(uint64_t) swapchainRecreations;

// xappify fork — app lifecycle. Call `suspendPresentation` BEFORE backgrounding and
// `resumePresentation` after returning to the foreground.
//
// A backgrounded `CAMetalLayer` cannot vend a drawable, and the Vulkan backend's present path used
// to wait for one forever — blocking the EMULATION thread inside `Core::System::RunLoop` for as
// long as the app stayed away (device-measured: 85.7s, with the app alive and audio starved into
// repeating its last buffer). Pausing is NOT a substitute: `pause:` only sets a flag the run loop
// reads at the top of its next iteration, so it cannot unblock a present already in flight.
//
// `resumePresentation` also marks the swapchain for recreation, because the layer's drawables do
// not survive the transition.
-(void) suspendPresentation;
-(void) resumePresentation;

-(BOOL) insertAmiibo:(NSURL *)url;
-(void) removeAbiibo;

-(void) loadConfig;
-(int) getSystemLanguage NS_SWIFT_NAME(systemLanguage());
-(void) setSystemLanguage:(int)systemLanguage NS_SWIFT_NAME(set(systemLanguage:));
-(NSString *) getUsername NS_SWIFT_NAME(username());
-(void) setUsername:(NSString *)username NS_SWIFT_NAME(set(username:));

-(NSArray *) saveStates:(uint64_t)identifier;
-(NSString *) saveStatePath:(uint64_t)identifier slot:(NSInteger)slot NS_SWIFT_NAME(saveStatePath(_:_:));
@end

NS_ASSUME_NONNULL_END
