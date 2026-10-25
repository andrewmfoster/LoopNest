// dragin <x> <y> <tx> <ty> <dwellSec> <url|legacy> <targetOwner> <file>...
// A floating 120x120 window at screen x,y (top-left coords, like CGEvent) that drags the
// files out as a real AppKit drag session, driven by posted mouse events to tx,ty, where
// it dwells (small moves) before releasing. `url` writes one public.file-url per item;
// `legacy` writes only NSFilenamesPboardType (one item, the path list).
// Refuses unless the topmost window at the target belongs to targetOwner, so no
// synthetic event lands in another app.
import Cocoa

let a = CommandLine.arguments
let p0 = CGPoint(x: Double(a[1])! + 60, y: Double(a[2])! + 60)
let p1 = CGPoint(x: Double(a[3])!, y: Double(a[4])!)
let dwell = Double(a[5])!, legacy = a[6] == "legacy", targetOwner = a[7]
let files = Array(a[8...])

func owner(at p: CGPoint) -> String {
    let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as! [[String: Any]]
    for w in list {
        guard let l = w[kCGWindowLayer as String] as? Int, l >= 0, l < 24, let b = w[kCGWindowBounds as String] as? [String: Double] else { continue }
        if CGRect(x: b["X"]!, y: b["Y"]!, width: b["Width"]!, height: b["Height"]!).contains(p) { return w[kCGWindowOwnerName as String] as? String ?? "?" }
    }
    return "none"
}

final class SourceView: NSView, NSDraggingSource {
    var began = false
    override func draw(_ r: NSRect) { NSColor.systemOrange.setFill(); r.fill() }
    override func acceptsFirstMouse(for e: NSEvent?) -> Bool { true }
    override func mouseDown(with e: NSEvent) { print("source mouseDown"); fflush(stdout) }
    override func mouseDragged(with e: NSEvent) {
        if began { return }
        began = true
        let at = convert(e.locationInWindow, from: nil)
        var items: [NSDraggingItem]
        if legacy {
            // Only the legacy filename list: one item carrying NSFilenamesPboardType under the
            // dynamic UTI AppKit maps that name to (an item refuses the bare name).
            let pi = NSPasteboardItem()
            pi.setPropertyList(files, forType: NSPasteboard.PasteboardType("dyn.ah62d4rv4gu8y6y4grf0gn5xbrzw1gydcr7u1e3cytf2gn"))
            items = [NSDraggingItem(pasteboardWriter: pi)]
        } else {
            items = files.map { NSDraggingItem(pasteboardWriter: URL(fileURLWithPath: $0) as NSURL) }
        }
        for it in items { it.setDraggingFrame(NSRect(x: at.x - 16, y: at.y - 16, width: 32, height: 32), contents: NSImage(named: NSImage.multipleDocumentsName)) }
        beginDraggingSession(with: items, event: e, source: self)
        print("session began, \(items.count) item(s), legacy=\(legacy)"); fflush(stdout)
    }
    func draggingSession(_ s: NSDraggingSession, sourceOperationMaskFor c: NSDraggingContext) -> NSDragOperation { .copy }
    func draggingSession(_ s: NSDraggingSession, endedAt p: NSPoint, operation op: NSDragOperation) {
        print("session ended, operation \(op.rawValue)"); fflush(stdout)
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let sh = NSScreen.main!.frame.height
let w = NSWindow(contentRect: NSRect(x: Double(a[1])!, y: sh - Double(a[2])! - 120, width: 120, height: 120),
                 styleMask: [.borderless], backing: .buffered, defer: false)
w.level = .floating
w.contentView = SourceView(frame: NSRect(x: 0, y: 0, width: 120, height: 120))
w.orderFrontRegardless()

DispatchQueue.global().asyncAfter(deadline: .now() + 0.8) {
    let o0 = owner(at: p0), o1 = owner(at: p1)
    print("start owner:", o0, " target owner:", o1); fflush(stdout)
    guard o1 == targetOwner, o0 == ProcessInfo.processInfo.processName || o0 == "dragin" else {
        print("REFUSED: owners don't match"); fflush(stdout); DispatchQueue.main.async { exit(2) }; return
    }
    let src = CGEventSource(stateID: .hidSystemState)
    func post(_ t: CGEventType, _ p: CGPoint) { CGEvent(mouseEventSource: src, mouseType: t, mouseCursorPosition: p, mouseButton: .left)!.post(tap: .cghidEventTap) }
    post(.mouseMoved, p0); usleep(200_000)
    post(.leftMouseDown, p0); usleep(150_000)
    let n = 40
    for i in 1...n { let t = Double(i) / Double(n); post(.leftMouseDragged, CGPoint(x: p0.x + (p1.x - p0.x) * t, y: p0.y + (p1.y - p0.y) * t)); usleep(25_000) }
    let steps = Int(dwell / 0.05)
    for i in 0..<max(1, steps) { post(.leftMouseDragged, CGPoint(x: p1.x + Double(i % 2), y: p1.y)); usleep(50_000) }
    post(.leftMouseUp, p1)
    print("posted"); fflush(stdout)
    usleep(1_500_000)
    DispatchQueue.main.async { exit(0) }
}
app.run()
