// drag x0 y0 x1 y1 startOwner endOwner  — refuses unless the topmost window at each point belongs to the named owner.
import Foundation
import CoreGraphics
let a = CommandLine.arguments
let p0 = CGPoint(x: Double(a[1])!, y: Double(a[2])!), p1 = CGPoint(x: Double(a[3])!, y: Double(a[4])!)
func owner(at p: CGPoint) -> String {
    let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as! [[String: Any]]
    for w in list {  // front to back
        guard let l = w[kCGWindowLayer as String] as? Int, l >= 0, l < 24, let b = w[kCGWindowBounds as String] as? [String: Double] else { continue }
        let r = CGRect(x: b["X"]!, y: b["Y"]!, width: b["Width"]!, height: b["Height"]!)
        if r.contains(p) { return w[kCGWindowOwnerName as String] as? String ?? "?" }
    }
    return "none"
}
let o0 = owner(at: p0), o1 = owner(at: p1)  // layers 0-23; ignores the 2 KB layer-24 screenshot shield
print("start owner:", o0, " end owner:", o1)
guard o0 == a[5], o1 == a[6] else { print("REFUSED: owners don't match"); exit(2) }
if a.count > 7 && a[7] == "--check" { exit(0) }
let src = CGEventSource(stateID: .hidSystemState)
func post(_ t: CGEventType, _ p: CGPoint) { CGEvent(mouseEventSource: src, mouseType: t, mouseCursorPosition: p, mouseButton: .left)!.post(tap: .cghidEventTap) }
post(.mouseMoved, p0); usleep(200_000)
post(.leftMouseDown, p0); usleep(150_000)
let n = 40
for i in 1...n { let t = Double(i) / Double(n); post(.leftMouseDragged, CGPoint(x: p0.x + (p1.x - p0.x) * t, y: p0.y + (p1.y - p0.y) * t)); usleep(25_000) }
usleep(400_000)
post(.leftMouseUp, p1)
print("posted")
