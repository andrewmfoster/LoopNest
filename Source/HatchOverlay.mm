// Cocoa first: JuceHeader's `using namespace juce` makes MacTypes' Point ambiguous.
#import <Cocoa/Cocoa.h>
#include "HatchOverlay.h"
#include <objc/runtime.h>
#include <objc/message.h>
#include <cmath>

// The view class is registered at runtime under a randomised name: v1, this build
// and any later LoopNest can share one host process, and two bundles defining one
// Objective-C class name make the runtime pick either at random.
namespace
{
    constexpr const char* kOwnerIvar = "lnOwner";

    // The owner pointer lives in a raw (non-object) ivar, read and written by offset.
    HatchOverlay** ownerSlot(id self)
    {
        Ivar iv = class_getInstanceVariable(object_getClass(self), kOwnerIvar);
        return iv != nullptr ? (HatchOverlay**) ((char*) (void*) self + ivar_getOffset(iv)) : nullptr;
    }

    HatchOverlay* ownerOf(id self)
    {
        auto** slot = ownerSlot(self);
        return slot != nullptr ? *slot : nullptr;
    }

    template <typename Block>
    void addMethod(Class cls, SEL sel, const char* types, Block block)
    {
        class_addMethod(cls, sel, imp_implementationWithBlock(block), types);
    }
}

struct HatchOverlay::Impl
{
    NSView* view = nil;
    NSPoint downAt {};
    bool down = false, dragging = false, hovering = false;

    static Class viewClass()
    {
        static Class cls = []
        {
            const auto name = "LoopNestHatchView_" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64());
            Class c = objc_allocateClassPair([NSView class], name.toRawUTF8(), 0);
            class_addIvar(c, kOwnerIvar, sizeof(void*), (uint8_t) log2(sizeof(void*)), "^v");

            addMethod(c, @selector(isOpaque), "c@:", ^BOOL(id) { return NO; });
            addMethod(c, @selector(isFlipped), "c@:", ^BOOL(id) { return YES; });
            addMethod(c, @selector(drawRect:), "v@:{CGRect={CGPoint=dd}{CGSize=dd}}", ^(id, NSRect) {});
            // Live's plugin windows are rarely key: the first click must act, not just focus.
            addMethod(c, @selector(acceptsFirstMouse:), "c@:@", ^BOOL(id, NSEvent*) { return YES; });
            addMethod(c, @selector(mouseDownCanMoveWindow), "c@:", ^BOOL(id) { return NO; });

            addMethod(c, @selector(updateTrackingAreas), "v@:", ^(id self)
            {
                NSView* v = self;
                for (NSTrackingArea* a in [v trackingAreas])
                    [v removeTrackingArea: a];
                [v addTrackingArea: [[[NSTrackingArea alloc]
                    initWithRect: NSZeroRect
                         options: NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
                           owner: v
                        userInfo: nil] autorelease]];
                struct objc_super sup { self, [NSView class] };
                ((void (*)(struct objc_super*, SEL)) objc_msgSendSuper)(&sup, @selector(updateTrackingAreas));
            });

            addMethod(c, @selector(mouseEntered:), "v@:@", ^(id self, NSEvent*)
            {
                if (auto* o = ownerOf(self)) { if (o->log) o->log("mouseEntered"); o->impl->setHover(*o, true); }
            });
            addMethod(c, @selector(mouseExited:), "v@:@", ^(id self, NSEvent*)
            {
                if (auto* o = ownerOf(self)) { if (o->log) o->log("mouseExited"); o->impl->setHover(*o, false); }
            });
            addMethod(c, @selector(mouseDown:), "v@:@", ^(id self, NSEvent* e)
            {
                if (auto* o = ownerOf(self))
                {
                    if (o->log) o->log("mouseDown");
                    o->impl->down = true;
                    o->impl->dragging = false;
                    o->impl->downAt = [e locationInWindow];
                    if (o->onPress) o->onPress(true);
                }
            });
            addMethod(c, @selector(mouseDragged:), "v@:@", ^(id self, NSEvent* e)
            {
                if (auto* o = ownerOf(self))
                    o->impl->dragged(*o, e);
            });
            addMethod(c, @selector(mouseUp:), "v@:@", ^(id self, NSEvent*)
            {
                if (auto* o = ownerOf(self))
                {
                    auto& im = *o->impl;
                    const bool click = im.down && ! im.dragging;
                    if (o->log) o->log(juce::String("mouseUp click=") + (click ? "yes" : "no"));
                    im.down = false;
                    if (o->onPress) o->onPress(false);
                    if (click && o->onClick) o->onClick();
                }
            });

            // NSDraggingSource
            addMethod(c, @selector(draggingSession:sourceOperationMaskForDraggingContext:), "Q@:@q",
                      ^NSDragOperation(id, NSDraggingSession*, NSDraggingContext) { return NSDragOperationCopy; });
            addMethod(c, @selector(draggingSession:endedAtPoint:operation:), "v@:@{CGPoint=dd}Q",
                      ^(id self, NSDraggingSession*, NSPoint, NSDragOperation op)
            {
                // AppKit delivers no mouseUp after a session, so the press ends here.
                if (auto* o = ownerOf(self))
                {
                    auto& im = *o->impl;
                    im.down = im.dragging = false;
                    ++o->dragsEnded;
                    if (o->onPress) o->onPress(false);
                    if (o->onDragEnd) o->onDragEnd(op != NSDragOperationNone);
                    // The pointer may have left the tile during the drag.
                    NSView* v = self;
                    const NSPoint p = [v convertPoint: [[v window] mouseLocationOutsideOfEventStream] fromView: nil];
                    im.setHover(*o, NSPointInRect(p, [v bounds]));
                }
            });
            class_addProtocol(c, @protocol(NSDraggingSource));
            objc_registerClassPair(c);
            return c;
        }();
        return cls;
    }

    void setHover(HatchOverlay& o, bool h)
    {
        if (h == hovering) return;
        hovering = h;
        if (o.onHover) o.onHover(h);
    }

    void dragged(HatchOverlay& o, NSEvent* e)
    {
        if (! down || dragging) return;
        const NSPoint p = [e locationInWindow];
        if (std::hypot(p.x - downAt.x, p.y - downAt.y) < 4.0) return;   // a click's jitter, not a drag

        const auto file = o.fileToDrag ? o.fileToDrag() : juce::File();
        if (! file.existsAsFile()) return;

        dragging = true;
        NSURL* url = [NSURL fileURLWithPath: [NSString stringWithUTF8String: file.getFullPathName().toRawUTF8()]];
        NSDraggingItem* item = [[[NSDraggingItem alloc] initWithPasteboardWriter: url] autorelease];
        const NSPoint local = [view convertPoint: p fromView: nil];
        [item setDraggingFrame: NSMakeRect(local.x - 16.0, local.y - 16.0, 32.0, 32.0)
                      contents: [[NSWorkspace sharedWorkspace] iconForFile: url.path]];
        if (NSDraggingSession* s = [view beginDraggingSessionWithItems: @[ item ]
                                                                 event: e
                                                                source: (id<NSDraggingSource>) view])
        {
            s.animatesToStartingPositionsOnCancelOrFail = YES;
            ++o.dragsBegun;
            if (o.onDragStart) o.onDragStart();
        }
        else
            dragging = false;
    }
};

HatchOverlay::HatchOverlay() : impl(std::make_unique<Impl>())
{
    NSView* v = [[Impl::viewClass() alloc] initWithFrame: NSMakeRect(0, 0, 10, 10)];
    if (auto** slot = ownerSlot(v)) *slot = this;
    impl->view = v;
    setView(v);        // NSViewComponent retains it
    [v release];
}

bool HatchOverlay::bringToFront()
{
    NSView* v = impl->view;
    NSView* sup = [v superview];
    if (sup == nil || [[sup subviews] lastObject] == v) return false;
    [v retain];
    [v removeFromSuperview];
    [sup addSubview: v positioned: NSWindowAbove relativeTo: nil];
    [v release];
    return true;
}

juce::String HatchOverlay::hitAtCentre() const
{
    NSView* v = impl->view;
    NSWindow* w = [v window];
    if (w == nil) return "no window";
    if ([v superview] == nil) return "no superview";
    if ([v isHidden]) return "hidden";
    const NSRect b = [v bounds];
    const NSPoint inWin = [v convertPoint: NSMakePoint(NSMidX(b), NSMidY(b)) toView: nil];
    // hitTest: takes the point in the receiver's superview's space; the frame view has
    // none, so window coordinates.
    NSView* content = [w contentView];
    NSView* root = [content superview] != nil ? [content superview] : content;
    NSView* hit = [root hitTest: inWin];
    if (hit == v) return "overlay";
    return hit == nil ? juce::String("nothing") : juce::String([NSStringFromClass([hit class]) UTF8String]);
}

HatchOverlay::~HatchOverlay()
{
    if (auto** slot = ownerSlot(impl->view)) *slot = nullptr;
    setView(nullptr);
}
