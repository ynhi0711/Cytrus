//
//  BuildStrings.mm
//  Cytrus
//
//  Created by Jarrod Norwell on 15/3/2025.
//  Copyright © 2025 Jarrod Norwell. All rights reserved.
//

#import <Foundation/Foundation.h>

#import <string.h>

#import "BuildStrings.h"

// The consuming app (Delta) does not ship the GIT_REV / GIT_DATE Info.plist keys that Folium's own
// app does, so those lookups return nil and -[NSString UTF8String] yields NULL. A null g_scm_rev
// aborts savestate validation (`std::string == (const char*)nullptr`) and breaks saving and other
// consumers (svc.cpp, vk_shader_disk_cache, zstd). Never return null here. Also strdup the dynamic
// values: the globals in scm_rev.cpp are process-lifetime, but -UTF8String's buffer belongs to the
// static-init autorelease pool and would dangle — a latent bug independent of the null case.

// Stable 40-hex-char revision for Cytrus builds hosted inside Delta. Fixed (NOT the changing git
// SHA) so every Cytrus savestate embeds the same revision and validates as current across app
// updates. savestate.cpp hex-decodes this into the 20-byte CSTHeader.revision, so it MUST be
// exactly 40 hex characters.
//
// This ALSO doubles as the SAVESTATE-LAYOUT VERSION. ValidateSaveState (savestate.cpp) only rejects
// a save when its embedded revision != this value; since every Cytrus build shares this constant, a
// savestate written by an OLDER build with a DIFFERENT serialized System/GPU/Pica/kernel layout
// otherwise passes validation and then ABORTS in the boost deserialize (misaligned stream →
// `basic_binary_iprimitive::load` assert). The abort is not catchable, so validation-before-load is
// the only safe guard. Therefore: BUMP the middle version field below (…0000000000000001…) whenever
// you change anything in the serialized state graph. Stale saves then fail validation and are
// rejected (LoadState throws "Invalid savestate", RunLoop catches it, the title boots fresh) instead
// of crashing. v1: 2026-07 — first bump; establishes a clean baseline after the boot-path work.
// v2: 2026-07 — serialized System/GPU/Pica layout drifted from v1 builds; an old-build resume save
// aborted in boost deserialize (GeometryEmitter bool trap, basic_binary_iprimitive.hpp:105). Bump to
// reject stale v1 saves at validation so the freshly-booted title keeps running instead of crashing.
static const char* const kCytrusFallbackRevision = "c17c05c0dec0ffee0000000000000002deadbeef";

const char* gitDate(void) {
    NSDate *date = [[[NSBundle mainBundle] infoDictionary] objectForKey:@"GIT_DATE"];
    if (!date) return "";
    NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
    [formatter setDateStyle:NSDateFormatterMediumStyle];
    [formatter setTimeStyle:NSDateFormatterShortStyle];
    const char *utf8 = [[formatter stringFromDate:date] UTF8String];
    return utf8 ? strdup(utf8) : "";
}

const char* buildRevision(void) {
    NSString *revision = [[[NSBundle mainBundle] infoDictionary] objectForKey:@"GIT_REV"];
    const char *utf8 = [revision UTF8String];
    return utf8 ? strdup(utf8) : kCytrusFallbackRevision;
}

const char* buildFullName(void) {
    NSString *version = [[[NSBundle mainBundle] infoDictionary] objectForKey:@"CFBundleShortVersionString"];
    NSString *build = [[[NSBundle mainBundle] infoDictionary] objectForKey:(NSString *)kCFBundleVersionKey];
    if (!version || !build) return "";
    const char *utf8 = [[NSString stringWithFormat:@"%@.%@", version, build] UTF8String];
    return utf8 ? strdup(utf8) : "";
}
