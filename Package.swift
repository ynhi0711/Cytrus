// swift-tools-version: 5.9
//
// xappify build of Cytrus (folium-app/Cytrus) — upstream ships no build system; this
// package is the reconstructed recipe. See BUILDING.md. Run ./fetch_dependencies.sh once
// after cloning (populates git-ignored Binaries/ and Dependencies/).
//
// NOTE: the manifest is deliberately split into small typed statements — a single big
// Package(...) expression blows Swift's type-checker time limit.

import PackageDescription

// Prebuilt device/simulator xcframeworks from folium-app/SharedDependencies release 2.0.
// ⚠️ built with minos 26.0 — links into lower deployment targets with warnings; rebuild
// at lower minos before shipping (BUILDING.md "Dependency map").
let binaryNames: [String] = [
    "libboostcontext", "libboostiostreams", "libboostprogramoptions", "libboostserialization",
    "libcubeb", "libdynarmic", "libenet", "libfaad2", "libfmt", "libgenericcodegen",
    "libglslang", "libmachineindependent", "libmcl", "libopenal", "libopus", "SDL3",
    "libsirit", "libsoundtouch", "libspirv", "libteakra",
]

let binaryTargets: [Target] = binaryNames.map {
    Target.binaryTarget(name: $0, path: "Binaries/\($0).xcframework")
}

// Vendored third-party include dirs (SharedDependencies/Sources/<lib>/include layout).
let vendoredIncludeNames: [String] = [
    "cereal", "dds_ktx", "eventbus", "httplib", "inih", "jwt", "lodepng", "magic_enum",
    "metal_cpp", "microprofile", "miniz", "nihstro", "nlohmann", "oaknut", "stb", "toml",
    "tsl", "xxhash",
]
let vendoredIncludes: [CXXSetting] = vendoredIncludeNames.map {
    CXXSetting.headerSearchPath("SharedDependencies/Sources/\($0)/include")
}

var cytrusDependencies: [Target.Dependency] = binaryNames.map {
    Target.Dependency.target(name: $0)
}
cytrusDependencies.append(.product(name: "OpenSSL", package: "OpenSSL"))
cytrusDependencies.append(.target(name: "cryptopp"))
cytrusDependencies.append(.target(name: "inih"))

let cytrusExcludes: [String] = [
    // Non-source / metadata
    "BUILDING.md", "LICENSE.md", "README.md", "fetch_dependencies.sh",
    "SharedDependencies/Package.swift", "SharedDependencies/README.md",
    // Folium's Swift sugar — the app's CytrusAdapter talks ObjC directly.
    "Cytrus.swift",
    // Multiplayer impl depends on Swift types from Cytrus.swift (Cytrus-Swift.h) and is
    // unused by this app; the header still compiles for CytrusEmulator.h's import.
    "Managers/MultiplayerManager/MultiplayerManager.mm",
    // FFmpeg video-dumper dlopen wrapper — needs libav* headers (dropped from the 2.0
    // dependency set); usage is guarded by ENABLE_FFMPEG_VIDEO_DUMPER (off).
    "Core/common/dynamic_library/ffmpeg.cpp",
    "Core/core/dumping/ffmpeg_backend.cpp",
    // zstd contrib ships examples/tests alongside the sources.
    "Dependencies/zstd/contrib/seekable_format/examples",
    "Dependencies/zstd/contrib/seekable_format/tests",
]

// Grown module-by-module (compile loop, BUILDING.md log). Everything compiles into this
// ONE C-family target; isolated vendored deps (cryptopp, inih) are separate targets.
let cytrusSources: [String] = [
    "Core/common",
    "Core/core",
    "Core/video_core",
    "Core/audio_core",
    "Core/input_common",
    "Core/network",
    "Core/web_service",
    // ObjC++ wrapper layer (the CytrusEmulator API the app consumes).
    "CytrusEmulator.mm",
    "BuildStrings",
    "Camera",
    "Configuration",
    "EmulationWindow",
    "InputManager",
    "Managers",
    "SoftwareKeyboard",
    // zstd from source (lib + seekable_format contrib) — not in the prebuilt set.
    "Dependencies/zstd/lib/common",
    "Dependencies/zstd/lib/compress",
    "Dependencies/zstd/lib/decompress",
    "Dependencies/zstd/contrib/seekable_format",
]

let cytrusCSettings: [CSetting] = [
    // Plain-C sources (zstd, vendored C libs). SPM applies cSettings/cxxSettings
    // per-language — keep both blocks in sync where it matters.
    .headerSearchPath("Dependencies/zstd/lib"),
    .headerSearchPath("Dependencies/zstd/contrib"),
    .define("ZSTD_STATIC_LINKING_ONLY"), // internal zstd + seekable contrib API
]

var cytrusCXXSettings: [CXXSetting] = [
    .headerSearchPath("."),
    .headerSearchPath("BuildStrings"),
    // Wrapper-layer dirs (CytrusEmulator.mm imports their headers bare).
    .headerSearchPath("Camera"),
    .headerSearchPath("Configuration"),
    .headerSearchPath("EmulationWindow"),
    .headerSearchPath("InputManager"),
    .headerSearchPath("SoftwareKeyboard"),
    .headerSearchPath("Managers"),
    .headerSearchPath("Managers/CheatsManager"),
    .headerSearchPath("Managers/GameInformationManager"),
    .headerSearchPath("Managers/MultiplayerManager"),
    .headerSearchPath("Core"),
    .headerSearchPath("Core/include"),
    // NOTE: never add Core/include/<module> dirs here — Citra's assert.h etc. would
    // shadow the C system headers. The few sibling-style includes are patched to full
    // "common/..." paths instead (see BUILDING.md log).
    .headerSearchPath("SharedDependencies/Sources/cryptopp/include"),
    .headerSearchPath("Dependencies/boost"),
    .headerSearchPath("Dependencies/vulkan-headers/include"),
    .headerSearchPath("Dependencies/vma"),
    .headerSearchPath("Dependencies/spirv-headers"),
    .headerSearchPath("Dependencies/zstd/lib"),
    .headerSearchPath("Dependencies/zstd/contrib"),
    // Citra-lineage arch define (aarch64 JIT/shader paths key off it).
    .define("ARCHITECTURE_arm64"),
    // iOS renders via Vulkan→MoltenVK only (settings.h #errors without one).
    .define("ENABLE_VULKAN"),
    .define("ZSTD_STATIC_LINKING_ONLY"), // zstd_compression.cpp uses advanced API
    // Without this, fmt's headers (shipped in libfmt.xcframework) emit strong
    // definitions per-TU → hundreds of duplicate symbols at link.
    .define("FMT_HEADER_ONLY"),
    // httplib SSL mode — http_c.cpp uses httplib::SSLClient + raw OpenSSL types.
    .define("CPPHTTPLIB_OPENSSL_SUPPORT"),
    // The tree only ships the SDL3 input backend (no sdl/sdl.h legacy pair).
    .define("ENABLE_SDL3"),
    // Embedding SDL: without this, SDL_main.h injects a main() into every includer
    // (duplicate _main at link between sdl3_sink.o and EmulationWindow_Vulkan.o).
    .define("SDL_MAIN_HANDLED"),
]
cytrusCXXSettings.append(contentsOf: vendoredIncludes)

let cytrusLinkerSettings: [LinkerSetting] = [
    .linkedFramework("Metal"),
    .linkedFramework("MetalKit"),
    .linkedFramework("QuartzCore"),
    .linkedFramework("UIKit"),
    .linkedFramework("AVFoundation"),
    .linkedFramework("AudioToolbox"),
    .linkedLibrary("z"),
    .linkedLibrary("bz2"),
]

// Separate targets: their internal bare includes need dirs full of shadow-prone names
// (cryptopp's config.h etc.) that must NOT leak onto the core's include path.
let cryptoppTarget: Target = .target(
    name: "cryptopp",
    path: "SharedDependencies/Sources/cryptopp",
    publicHeadersPath: "include",
    cxxSettings: [
        .headerSearchPath("include/cryptopp"),
        // The ARM CRC32/PMULL intrinsic paths need -march=armv8-a+crc+crypto, which SPM
        // can't pass without unsafeFlags. Disable them — Citra uses cryptopp for AES/SHA
        // (NEON paths unaffected); CRC32 falls back to the table implementation.
        .define("CRYPTOPP_DISABLE_ARM_CRC32"),
        .define("CRYPTOPP_DISABLE_ARM_PMULL"),
    ]
)

let inihTarget: Target = .target(
    name: "inih",
    path: "SharedDependencies/Sources/inih",
    publicHeadersPath: "include",
    cSettings: [.headerSearchPath("include/inih")],
    cxxSettings: [.headerSearchPath("include/inih")]
)

let cytrusTarget: Target = .target(
    name: "Cytrus",
    dependencies: cytrusDependencies,
    path: ".",
    exclude: cytrusExcludes,
    sources: cytrusSources,
    publicHeadersPath: "Public",
    cSettings: cytrusCSettings,
    cxxSettings: cytrusCXXSettings,
    linkerSettings: cytrusLinkerSettings
)

var allTargets: [Target] = binaryTargets
allTargets.append(cryptoppTarget)
allTargets.append(inihTarget)
allTargets.append(cytrusTarget)

let package = Package(
    name: "Cytrus",
    platforms: [.iOS(.v15)],
    products: [
        .library(name: "Cytrus", targets: ["Cytrus"])
    ],
    dependencies: [
        // OpenSSL binary xcframework (Folium uses the same for ssl_c / web_service).
        .package(url: "https://github.com/krzyzanowskim/OpenSSL", from: "3.0.0"),
    ],
    targets: allTargets,
    cLanguageStandard: .gnu2x,
    cxxLanguageStandard: .gnucxx2b
)
