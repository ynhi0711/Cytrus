# Cytrus
Cytrus is the Nintendo 3DS emulation core used in Folium

Cytrus is built on top of the **Citra** emulator formally developed by the **Citra Team** and later picked up by the **Azahar Team**


## Building for iOS

This is the xappify fork, packaged as a **local Swift Package** (`Package.swift`) for the
Delta iOS app. Xcode resolves and compiles it automatically as a dependency of the `Delta`
target — there is **no CocoaPods pod** and no manual `swift build` (SPM alone can't build it;
it needs the iOS SDK via `xcodebuild`).

### One-time setup: fetch dependencies

The prebuilt frameworks and vendored headers live in **git-ignored** `Binaries/` and
`Dependencies/` folders, so they are absent on a fresh clone. Fetch them once:

```bash
./fetch_dependencies.sh
```

This downloads (~1 GB total; needs `git`, `curl`, `unzip`):

- prebuilt dependency xcframeworks (folium-app/SharedDependencies 2.0) → `Binaries/`
- **MoltenVK v1.4.1** xcframework (the runtime Vulkan ICD the app embeds) → `Binaries/`
- Vulkan-Headers v1.4.304, VulkanMemoryAllocator v3.1.0, SPIRV headers, zstd 1.5.6, and
  boost 1.85.0 headers (~140 MB) → `Dependencies/`

The script is idempotent — re-running skips anything already fetched. **If you skip this
step, the `Delta` build fails** with a missing `MoltenVK.xcframework` and unresolved Swift
Package binary targets.


