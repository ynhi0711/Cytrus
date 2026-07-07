//
//  HarnessConfig.swift
//  CytrusHarness
//
//  Seeds the `cytrus.v1.38.*` UserDefaults profile CytrusEmulator.mm reads in its
//  settings-apply path. Ported from the app's ThreeDSConfig (ManicEMU's `ManicEMU.*` keys
//  were a rebrand of this same layer; key names match, but layout keys differ —
//  customTopX/Y/Width/Height here vs customTopLeft/... there).
//
//  ⚠️ The string keys MUST be registered non-nil: CytrusEmulator.mm feeds
//  `stringForKey → UTF8String` straight into std::string with no nil guard — the exact
//  unset-`webAPIURL` crash ManicEMU's build had.
//

import Foundation

enum HarnessConfig {
    private static let prefix = "cytrus.v1.38."

    static func seed() {
        let d = UserDefaults.standard

        // Crash guards — every string key the wrapper reads (grep `string(@"` in
        // CytrusEmulator.mm when rebasing).
        d.register(defaults: [
            prefix + "webAPIURL": "",
            prefix + "ppShaderName": "",
            prefix + "anaglyphShaderName": "",
        ])

        let profile: [String: Any] = [
            // Core
            "cpuJIT": false,              // jitless (App Store parity); iOS 26 forces false anyway
            "cpuClockPercentage": 100,
            "new3DS": true,
            "lleApplets": false,
            "deterministicAsyncOperations": false,
            "enableRequiredOnlineLLEModules": false,
            // Data storage
            "compressCIAInstalls": false,
            // System
            "regionValue": -1,
            "pluginLoader": false,
            "allowPluginLoader": true,
            "stepsPerHour": 0,
            "applyRegionFreePatch": true,
            // Renderer
            "spirvShaderGeneration": true,
            "disableSpirvOptimizer": false,
            "useAsyncShaderCompilation": false,
            "useAsyncPresentation": true,
            "useHardwareShaders": true,
            "useDiskShaderCache": true,
            "useShadersAccurateMul": false,
            "useNewVSync": true,
            "useShaderJIT": false,
            "resolutionFactor": 1,
            "textureFilter": 0,
            "textureSampling": 0,
            "delayGameRenderThreadUS": 0,
            "layoutOption": 0,            // default layout — no custom rects for the gate
            "aspectRatio": 0,
            "customSecondLayerOpacity": 100,
            "render3D": 0,
            "factor3D": 0,
            "monoRender": 0,
            "filterMode": 0,
            "disableRightEyeRender": true,
            "dumpTextures": false,
            "customTextures": false,
            "preloadTextures": false,
            "asyncCustomLoading": false,
            // Audio — outputType 3 = OpenAL playback (same as the prebuilt profile);
            // inputType 1 = Null mic (Cubeb unavailable → OpenAL capture crashes).
            "audioMuted": false,
            "audioEmulation": 0,
            "audioStretching": false,
            "realtimeAudio": true,
            "volume": 1.0,
            "outputType": 3,
            "inputType": 1,
            // Logging — verbose so boot failures name themselves in the console.
            "logLevel": 0,
        ]
        for (key, value) in profile { d.set(value, forKey: prefix + key) }
    }
}
