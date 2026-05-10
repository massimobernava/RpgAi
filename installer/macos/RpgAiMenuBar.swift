import AppKit
import Foundation

// MARK: - Config
private let kBinary    = "/usr/local/bin/rpgai"
private let kShare     = "/usr/local/share/rpgai"
private let kBaseURL   = "http://localhost:8080"
private let kWorkspace = FileManager.default
    .homeDirectoryForCurrentUser
    .appendingPathComponent("Documents/RpgAi").path

// MARK: - AppDelegate
class AppDelegate: NSObject, NSApplicationDelegate {

    var statusItem: NSStatusItem!
    var engineTask: Process?

    func applicationDidFinishLaunching(_ notification: Notification) {
        prepareWorkspace()
        startEngine()
        buildStatusItem()
        openWhenReady()
    }

    func applicationWillTerminate(_ notification: Notification) {
        engineTask?.terminate()
    }

    // MARK: Workspace

    private func prepareWorkspace() {
        let fm = FileManager.default
        for sub in ["scripts", "saves"] {
            try? fm.createDirectory(atPath: "\(kWorkspace)/\(sub)",
                                    withIntermediateDirectories: true,
                                    attributes: nil)
        }
        let demo = "\(kWorkspace)/scripts/fantasy_demo.lua"
        if !fm.fileExists(atPath: demo) {
            try? fm.copyItem(atPath: "\(kShare)/scripts",
                             toPath: "\(kWorkspace)/scripts")
        }
    }

    // MARK: Engine

    private func startEngine() {
        guard FileManager.default.fileExists(atPath: kBinary) else {
            showAlert("RpgAi",
                      "Binary not found at \(kBinary).\nPlease reinstall RpgAi.")
            return
        }
        let task = Process()
        task.executableURL    = URL(fileURLWithPath: kBinary)
        task.arguments        = ["--web",
                                 "--path",      "\(kWorkspace)/scripts/",
                                 "--save-path", "\(kWorkspace)/saves/"]
        task.currentDirectoryURL = URL(fileURLWithPath: kWorkspace)
        try? task.run()
        engineTask = task
    }

    // MARK: Status bar

    private func buildStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        statusItem.button?.title = "🎲"

        let menu = NSMenu()
        menu.addItem(withTitle: "Open RpgAi",
                     action: #selector(openBrowser),
                     keyEquivalent: "o")
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit RpgAi",
                     action: #selector(quitApp),
                     keyEquivalent: "q")
        statusItem.menu = menu
    }

    @objc private func openBrowser() {
        NSWorkspace.shared.open(URL(string: kBaseURL)!)
    }

    @objc private func quitApp() {
        engineTask?.terminate()
        NSApp.terminate(nil)
    }

    // MARK: Open browser when server is ready

    private func openWhenReady() {
        DispatchQueue.global(qos: .background).async {
            for _ in 0..<15 {
                Thread.sleep(forTimeInterval: 1)
                if let url = URL(string: kBaseURL),
                   (try? Data(contentsOf: url)) != nil { break }
            }
            DispatchQueue.main.async {
                let settings  = "\(kWorkspace)/rpgai_settings.json"
                let gs        = "\(kShare)/getting_started.html"
                let firstRun  = !FileManager.default.fileExists(atPath: settings)
                if firstRun, FileManager.default.fileExists(atPath: gs) {
                    NSWorkspace.shared.open(URL(fileURLWithPath: gs))
                } else {
                    NSWorkspace.shared.open(URL(string: kBaseURL)!)
                }
            }
        }
    }

    // MARK: Helper

    private func showAlert(_ title: String, _ message: String) {
        DispatchQueue.main.async {
            let a = NSAlert()
            a.messageText     = title
            a.informativeText = message
            a.alertStyle      = .critical
            a.runModal()
            NSApp.terminate(nil)
        }
    }
}

// MARK: - Entry point
let app = NSApplication.shared
app.setActivationPolicy(.accessory)   // no dock icon, no bounce
let delegate = AppDelegate()
app.delegate = delegate
app.run()
