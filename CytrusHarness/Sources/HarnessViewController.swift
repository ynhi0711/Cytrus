//
//  HarnessViewController.swift
//  CytrusHarness
//
//  Full-screen MTKView + Boot/Pause/Stop. Boot flow mirrors the app's
//  ThreeDSEmulatorBridge.boot: seed config → allocate → hand the layer over → +1s →
//  insert(from:) on a detached max-priority thread (ROM decrypt on main would
//  watchdog-kill the app).
//
//  Usage on device: drop a .3ds ROM into the app via the Files app (file sharing is
//  enabled), then tap Boot. aes_keys.txt installs automatically from the bundle.
//

import MetalKit
import UIKit

#if canImport(Cytrus)
import Cytrus
#endif

final class HarnessViewController: UIViewController {

    private let metalView = MTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
    private let logLabel = UILabel()
    private var isPaused = false

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black

        metalView.framebufferOnly = false
        view.addSubview(metalView)

        logLabel.textColor = .green
        logLabel.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        logLabel.numberOfLines = 0
        view.addSubview(logLabel)

        let boot = button("Boot", #selector(bootTapped))
        let pause = button("Pause", #selector(pauseTapped))
        let stop = button("Stop", #selector(stopTapped))
        let stack = UIStackView(arrangedSubviews: [boot, pause, stop])
        stack.axis = .horizontal
        stack.distribution = .fillEqually
        stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -16),
            stack.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor, constant: -12),
            stack.heightAnchor.constraint(equalToConstant: 44),
        ])
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        metalView.frame = view.bounds
        logLabel.frame = CGRect(x: 16, y: view.safeAreaInsets.top + 8,
                                width: view.bounds.width - 32, height: 160)
        view.bringSubviewToFront(logLabel)
    }

    private func button(_ title: String, _ action: Selector) -> UIButton {
        let b = UIButton(type: .system)
        b.setTitle(title, for: .normal)
        b.backgroundColor = .darkGray
        b.setTitleColor(.white, for: .normal)
        b.layer.cornerRadius = 8
        b.addTarget(self, action: action, for: .touchUpInside)
        return b
    }

    private func log(_ message: String) {
        print("[harness] \(message)")
        DispatchQueue.main.async {
            self.logLabel.text = ((self.logLabel.text ?? "") + "\n" + message)
                .split(separator: "\n").suffix(9).joined(separator: "\n")
        }
    }

    // MARK: Emulation

    #if canImport(Cytrus)
    private let core = CytrusEmulator.shared()
    private var didBoot = false

    @objc private func bootTapped() {
        guard !didBoot else { return log("already booted") }

        guard let romURL = firstROM() else {
            return log("no .3ds in Documents — drop one in via the Files app")
        }

        installAESKeys()
        HarnessConfig.seed()

        log("allocate()")
        core.allocate()

        guard let layer = metalView.layer as? CAMetalLayer else { return log("no CAMetalLayer") }
        log("top(layer, size: \(metalView.bounds.size))")
        core.top(layer, size: metalView.bounds.size)

        didBoot = true
        log("boot in 1s: \(romURL.lastPathComponent)")
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
            Thread.detachNewThread { [weak self] in
                Thread.setThreadPriority(1.0)
                self?.core.insert(from: romURL) {
                    self?.log("insert callback — running=\(self?.core.running() ?? false)")
                }
                self?.log("insert returned — running=\(self?.core.running() ?? false) stopped=\(self?.core.stopped() ?? true)")
            }
        }

        DispatchQueue.main.asyncAfter(deadline: .now() + 8.0) { [weak self] in
            guard let self else { return }
            self.log("@8s running=\(self.core.running()) paused=\(self.core.isPaused())")
        }
    }

    @objc private func pauseTapped() {
        guard didBoot else { return }
        isPaused.toggle()
        core.pause(isPaused)
        log(isPaused ? "paused" : "resumed")
    }

    @objc private func stopTapped() {
        guard didBoot else { return }
        log("stop()")
        core.stop()
        log("stopped=\(core.stopped())")
    }
    #else
    @objc private func bootTapped() { log("Cytrus module not linked") }
    @objc private func pauseTapped() {}
    @objc private func stopTapped() {}
    #endif

    // MARK: Files

    private var documentsURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    private func firstROM() -> URL? {
        let contents = (try? FileManager.default.contentsOfDirectory(
            at: documentsURL, includingPropertiesForKeys: nil)) ?? []
        return contents.first { ["3ds", "cci", "cxi", "app"].contains($0.pathExtension.lowercased()) }
    }

    /// This fork's iOS user dir is Documents/Cytrus (EMU_APPLE_DATA_DIR in
    /// common_paths.h — "Documents/3DS" was ManicEMU's rebrand in the prebuilt). Keys must
    /// be in place before the core's HW::AES::InitKeys latches them (once per process).
    private func installAESKeys() {
        guard let bundled = Bundle.main.url(forResource: "aes_keys", withExtension: "txt") else {
            return log("aes_keys.txt missing from bundle")
        }
        let sysdata = documentsURL.appendingPathComponent("Cytrus/sysdata", isDirectory: true)
        let target = sysdata.appendingPathComponent("aes_keys.txt")
        do {
            try FileManager.default.createDirectory(at: sysdata, withIntermediateDirectories: true)
            if !FileManager.default.fileExists(atPath: target.path) {
                try FileManager.default.copyItem(at: bundled, to: target)
            }
            log("aes_keys installed")
        } catch {
            log("aes_keys install failed: \(error.localizedDescription)")
        }
    }
}
