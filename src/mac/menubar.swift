// The macOS endpoint's menu bar: a visible face for a background service.
//
// The endpoint is a LaunchAgent — no Dock icon, no window — so "is it running?"
// is otherwise answerable only from the web page. This puts the state in the menu
// bar with the rate and delay, and the operational actions one click away.
//
//   aes67-srt-menubar --port 8082 [--title aes67]
//
// It polls the endpoint's own /api/status, and acts through /api/engine/* and
// /api/process/restart — the same endpoints the page uses. It owns nothing.
import AppKit
import Foundation

func argument(_ name: String, default fallback: String) -> String {
  let args = CommandLine.arguments
  if let index = args.firstIndex(of: name), index + 1 < args.count {
    return args[index + 1]
  }
  return fallback
}

let port = argument("--port", default: "8082")
let label = argument("--title", default: "aes67")
let base = URL(string: "http://127.0.0.1:\(port)")!

final class Controller: NSObject, NSApplicationDelegate {
  private var item: NSStatusItem!
  private var timer: Timer?

  func applicationDidFinishLaunching(_ notification: Notification) {
    NSApp.setActivationPolicy(.accessory)
    item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    item.button?.title = label
    item.menu = NSMenu()
    timer = Timer.scheduledTimer(withTimeInterval: 1.5, repeats: true) { [weak self] _ in
      self?.refresh()
    }
    refresh()
  }

  private func refresh() {
    var request = URLRequest(url: base.appendingPathComponent("api/status"))
    request.timeoutInterval = 2
    URLSession.shared.dataTask(with: request) { [weak self] data, _, _ in
      var state = "offline"
      var detail = "no answer from \(port)"
      if let data = data,
         let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
         let engine = root["engine"] as? [String: Any] {
        state = engine["state"] as? String ?? "unknown"
        let delay = engine["delay_ms"] as? Double ?? 0
        let refused = engine["frames_refused"] as? Int ?? 0
        let link = root["link"] as? [String: Any]
        let rate = link?["receive_rate_mbps"] as? Double ?? 0
        detail = String(format: "%.1f Mbit/s · %.0f ms · refused %d", rate, delay, refused)
      }
      DispatchQueue.main.async { self?.render(state: state, detail: detail) }
    }.resume()
  }

  private func render(state: String, detail: String) {
    switch state {
    case "running": item.button?.title = "● \(label)"
    case "offline": item.button?.title = "○ \(label)"
    default: item.button?.title = "◐ \(label)"  // starting, stopped, failed
    }
    item.button?.toolTip = "\(state): \(detail)"

    let menu = NSMenu()
    let header = NSMenuItem(title: "\(state) — \(detail)", action: nil, keyEquivalent: "")
    header.isEnabled = false
    menu.addItem(header)
    menu.addItem(.separator())
    add(menu, "Open control page", #selector(openPage))
    menu.addItem(.separator())
    add(menu, "Start audio", #selector(startAudio))
    add(menu, "Stop audio", #selector(stopAudio))
    add(menu, "Restart audio", #selector(restartAudio))
    menu.addItem(.separator())
    add(menu, "Restart process", #selector(restartProcess))
    menu.addItem(.separator())
    add(menu, "Quit the menu bar", #selector(quit))
    item.menu = menu
  }

  private func add(_ menu: NSMenu, _ title: String, _ action: Selector) {
    let entry = NSMenuItem(title: title, action: action, keyEquivalent: "")
    entry.target = self
    menu.addItem(entry)
  }

  @objc private func openPage() {
    NSWorkspace.shared.open(base)
  }

  private func post(_ path: String) {
    var request = URLRequest(url: base.appendingPathComponent(path))
    request.httpMethod = "POST"
    request.timeoutInterval = 5
    URLSession.shared.dataTask(with: request) { [weak self] _, _, _ in
      DispatchQueue.main.async { self?.refresh() }
    }.resume()
  }

  @objc private func startAudio() { post("api/engine/start") }
  @objc private func stopAudio() { post("api/engine/stop") }
  @objc private func restartAudio() { post("api/engine/restart") }
  @objc private func restartProcess() { post("api/process/restart") }
  @objc private func quit() { NSApp.terminate(nil) }
}

let application = NSApplication.shared
let controller = Controller()
application.delegate = controller
application.run()
