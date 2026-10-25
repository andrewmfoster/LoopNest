// droptarget <dir> <x> <y> <w> <h> <quitAfterSec>: floating window that accepts file drags, copies each file into <dir>.
import Cocoa
let a = CommandLine.arguments
let dir = URL(fileURLWithPath: a[1])
final class DropView: NSView {
    override init(frame: NSRect) { super.init(frame: frame); registerForDraggedTypes([.fileURL]) }
    required init?(coder: NSCoder) { fatalError() }
    override func draw(_ r: NSRect) { NSColor.systemTeal.setFill(); r.fill() }
    override func draggingEntered(_ s: NSDraggingInfo) -> NSDragOperation { print("entered", s.draggingSourceOperationMask.rawValue); fflush(stdout); return .copy }
    override func performDragOperation(_ s: NSDraggingInfo) -> Bool {
        let urls = s.draggingPasteboard.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
        for u in urls { print("dropped", u.path); try? FileManager.default.copyItem(at: u, to: dir.appendingPathComponent(u.lastPathComponent)) }
        fflush(stdout); return !urls.isEmpty
    }
}
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
// y is top-left screen coords like CGEvent; convert to Cocoa bottom-left.
let sh = NSScreen.main!.frame.height
let r = NSRect(x: Double(a[2])!, y: sh - Double(a[3])! - Double(a[5])!, width: Double(a[4])!, height: Double(a[5])!)
let w = NSWindow(contentRect: r, styleMask: [.borderless], backing: .buffered, defer: false)
w.level = .floating; w.contentView = DropView(frame: NSRect(origin: .zero, size: r.size)); w.orderFrontRegardless()
DispatchQueue.main.asyncAfter(deadline: .now() + Double(a[6])!) { app.terminate(nil) }
print("ready"); fflush(stdout)
app.run()
