# Building Cytrus for iOS (xappify fork)

Upstream (folium-app/Cytrus) ships **no build system** — this file is the reconstructed recipe.
Fork: `ynhi0711/Cytrus`, branch `xappify/ios-framework`; `upstream` remote = folium-app/Cytrus
(which tracks Azahar). Rebase routine: fetch upstream, rebase this branch, re-run the survey
greps below for new dependencies, rebuild.

## Survey findings (2026-07-06, upstream HEAD 84f59c9 "Fixed an issue with FMT")

### Settings contract (for the app-side adapter)
`CytrusEmulator.mm` reads **`NSUserDefaults` keys prefixed `cytrus.v1.38.`** (e.g.
`cytrus.v1.38.cpuClockPercentage`, `.new3DS`, `.useAsyncPresentation`). The prebuilt
ManicEMU Citra.framework's `ManicEMU.*` keys were a rebrand of exactly this layer — key names
match the app's `ThreeDSConfig` almost 1:1. `Configuration/Configuration.mm` (config.ini /
INIReader) is a parallel legacy path; the UserDefaults path is what applies at runtime.
Layout: setting a bottom layer switches `layout_option` to `SeparateWindows`; single-layer +
custom rects mirrors the prebuilt's behavior (verify in Phase 1).

### iOS-26 usage
Only two `@available(iOS 26, *)` guards (CytrusEmulator.mm:407,434) — they force
`use_cpu_jit=false` / `use_shader_jit=false` at runtime on iOS 26+. No compile-time blockers
for a 15.5 deployment target.

### Dependency map
Three buckets:

1. **Prebuilt xcframeworks** — folium-app/SharedDependencies **release 2.0**
   (published 2026-07-04; slices: ios-arm64, ios simulators, macos-arm64):
   `libboostcontext, libboostiostreams, libboostprogramoptions, libboostserialization,
   lib_cubeb, lib_dynarmic, lib_enet, lib_faad2, lib_fmt, lib_genericcodegen, lib_glslang,
   lib_machineindependent, lib_mcl, lib_openal, lib_opus, lib_sdl3, lib_sirit,
   lib_soundtouch, lib_spirv, lib_teakra`.
   ⚠️ Built with **minos 26.0** — links into a 15.5 target with warnings (pure-C++ libs are
   fine); before shipping at min iOS 15.5, rebuild these at a lower minos (all OSS) or raise
   the app deployment target (product decision). Tracked as a Phase 3 gate.
2. **Vendored source** — `SharedDependencies/Sources/` (copied from folium-app/Folium@main,
   which no longer ships the 3DS core; the copy is committed here):
   cereal, cryptopp, dds_ktx, eventbus, glib, httplib, inih, jwt, libchdr, libslirp, lodepng,
   magic_enum, metal_cpp, microprofile, miniz, nihstro, nlohmann, oaknut, stb, toml, tsl, xxhash.
3. **Headers-only fetch** — **boost 1.85.0** (matches the binary: `boost_1_85_0` string in
   libboost_serialization.a). The xcframeworks carry no boost headers. Core includes:
   algorithm, archive/serialization, asio, circular_buffer, container, crc, hana, icl,
   iostreams, locale, optional, range, regex.

`./fetch_dependencies.sh` downloads buckets 1 and 3 into git-ignored `Binaries/` and
`Dependencies/boost` — run it once after cloning.

MoltenVK: the app already bundles MoltenVK.framework 1.2.8 (the prebuilt Citra links the same
via @rpath); the package links against it at app level.

### Core tree layout
Compile sources live under `Core/{common,core,video_core,audio_core,input_common,network,
web_service,romfs}` with public headers duplicated under `Core/include/...` — header search
paths need BOTH `Core` and `Core/include`. Wrapper: `CytrusEmulator.h/.mm` + `Camera/`,
`Configuration/`, `EmulationWindow/`, `InputManager/`, `Managers/`, `SoftwareKeyboard/`,
`MultiplayerManager`, `BuildStrings/`. `Cytrus.swift` (Folium's Swift sugar) is NOT built —
the app's `CytrusAdapter` calls `CytrusEmulator` directly.

## Build vehicle
Local Swift package (`Package.swift`, this directory): binaryTargets (bucket 1, path-based
into `Binaries/`), source targets (bucket 2), and the `Cytrus` ObjC++ target (Core + wrapper,
`publicHeadersPath` exposing `CytrusEmulator.h` & friends so `import Cytrus` /
`canImport(Cytrus)` works). Built by Xcode as a dependency of the consuming app/harness —
`swift build` alone won't work (needs the iOS SDK via xcodebuild).

Compile-loop log (defines, exclusions, workarounds) is maintained below as they are
discovered.

## Compile-loop log

Build command (from this directory):
`xcodebuild -scheme Cytrus -destination generic/platform=iOS -derivedDataPath .build/dd build`

- **Sibling-style includes**: a handful of sources include their own header bare
  (`#include "math_util.h"`). Patched to full `"module/path.h"` form (fork patches —
  math_util, cityhash, hacks/hack_manager, dynamic_library/dynamic_library,
  file_sys/secure_value_backend, dlp/dlp_base, dlp/dlp_clt_base, file_sys/archive_artic,
  file_sys/artic_cache, http/ctr-common-1-{cert,key}). Do NOT solve this with
  `-I Core/include/<module>` — Citra's `assert.h` etc. shadow the C system headers.
- **zstd**: built from source (1.5.6, fetched) — lib/{common,compress,decompress} +
  contrib/seekable_format; `ZSTD_STATIC_LINKING_ONLY` defined for BOTH C and C++;
  seekable contrib's bare `#include "mem.h"` patched by the fetch script; contrib
  examples/tests excluded.
- **Renderer define**: `ENABLE_VULKAN` (settings.h `#error`s with no renderer).
- **fmt**: `FMT_HEADER_ONLY` — without it fmt's headers emit strong per-TU definitions →
  ~300 duplicate symbols at link. libfmt.a is effectively unused but harmless.
- **FFmpeg video dumper**: excluded (`common/dynamic_library/ffmpeg.cpp`,
  `core/dumping/ffmpeg_backend.cpp`) — needs libav* headers, dropped from the 2.0 dep set;
  usage is guarded by `ENABLE_FFMPEG_VIDEO_DUMPER` (off).
- **OpenSSL**: `krzyzanowskim/OpenSSL` SPM package (same as Folium);
  `CPPHTTPLIB_OPENSSL_SUPPORT` so httplib exposes `SSLClient` (http_c.cpp).
- **BuildStrings/** on the header path (core/scm_rev.cpp includes `BuildStrings.h`).
- **SPM executable heuristic**: `Core/input_common/main.cpp` renamed to
  `input_common_main.cpp` — SPM treats a target containing `main.cpp` as an executable and
  refuses to put it in a library product.
- **Vulkan headers**: KhronosGroup Vulkan-Headers v1.4.304 (fetched) — compatible with the
  current tree; MoltenVK provides the runtime ICD.
- **VulkanMemoryAllocator v3.1.0** + **SPIRV-Headers `spirv.hpp11`** fetched (neither ships
  in the 2.0 xcframework headers; sirit.h includes the latter).
- **libglslang.xcframework header layout**: its Headers dir is the glslang tree CONTENT —
  the fetch script nests everything under a `glslang/` prefix dir so `glslang/Include/…`
  includes resolve.
- **cryptopp / inih as separate SPM targets**: their internal bare includes need
  `-I include/<lib>` dirs full of shadow-prone names (cryptopp `config.h`) that must not
  leak onto the core's path. cryptopp: `CRYPTOPP_DISABLE_ARM_CRC32` + `_ARM_PMULL` (the
  intrinsics need `-march=armv8-a+crc+crypto`, unavailable without unsafeFlags; AES/SHA
  NEON paths unaffected, CRC falls back to table impl).
- **Wrapper**: each wrapper dir added to header search paths (bare imports).
  `Cytrus.swift` is NOT compiled — its model classes (`CytrusGameInformation`,
  `CytrusCheat`, `CytrusSaveState` + kernel memory-mode enums) are ported to ObjC in
  `Managers/CytrusTypes.{h,mm}` (same approach as ManicEMU's prebuilt), and the three
  `#import "Cytrus-Swift.h"` sites now import `CytrusTypes.h`.
  `Managers/MultiplayerManager/MultiplayerManager.mm` is excluded — it hard-depends on
  Swift-side room/chat classes and the app doesn't use 3DS multiplayer (its header still
  satisfies CytrusEmulator.h's import).
- **`SDL_MAIN_HANDLED`**: SDL3's SDL_main.h otherwise injects `main()` into every includer
  (duplicate `_main` between sdl3_sink.o and EmulationWindow_Vulkan.o).
- **Public module surface**: `Public/` shim headers (umbrella) — CytrusEmulator.h,
  CheatsManager.h, GameInformationManager.h, MultiplayerManager.h, CytrusTypes.h — so
  `import Cytrus` / `canImport(Cytrus)` works from Swift.

## Status
- ✅ Library builds for `generic/platform=iOS` (Debug) — full core + wrapper.
- ⏳ Next: device harness app (boot a title on hardware), then app integration (Phase 1).
