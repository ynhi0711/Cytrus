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
@property (nonatomic, copy, nullable) void (^saveStateHandler) (BOOL isLoad, CytrusSaveStateOutcome outcome, NSString *details);

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
