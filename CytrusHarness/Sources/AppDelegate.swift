//
//  AppDelegate.swift
//  CytrusHarness
//
//  Phase-0 device gate for the self-built Cytrus framework: boots a .3ds ROM from the
//  app's Documents into a full-screen MTKView. See BUILDING.md ("Status").
//

import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        let window = UIWindow(frame: UIScreen.main.bounds)
        window.rootViewController = HarnessViewController()
        window.makeKeyAndVisible()
        self.window = window
        return true
    }
}
