// Cocoa first: JuceHeader's `using namespace juce` makes MacTypes' Point ambiguous.
#import <Cocoa/Cocoa.h>
#include "RackDrop.h"
#include <objc/runtime.h>
#include <objc/message.h>
#include <cmath>

// Registered at runtime under a randomised name, as HatchOverlay's view: two LoopNest
// bundles in one host process must never share an Objective-C class name.
namespace
{
    constexpr const char* kOwnerIvar = "lnOwner";
    NSString* const kLegacyFilenames = @"NSFilenamesPboardType";   // NSFilenamesPboardType, deprecated but still sent

    RackDropTarget** ownerSlot(id self)
    {
        Ivar iv = class_getInstanceVariable(object_getClass(self), kOwnerIvar);
        return iv != nullptr ? (RackDropTarget**) ((char*) (void*) self + ivar_getOffset(iv)) : nullptr;
    }

    RackDropTarget* ownerOf(id self)
    {
        auto** slot = ownerSlot(self);
        return slot != nullptr ? *slot : nullptr;
    }

    template <typename Block>
    void addMethod(Class cls, SEL sel, const char* types, Block block)
    {
        class_addMethod(cls, sel, imp_implementationWithBlock(block), types);
    }

    // The dragged files' paths: file URLs first (one per item), else the legacy list.
    juce::StringArray pathsOf(id<NSDraggingInfo> info)
    {
        NSPasteboard* pb = [info draggingPasteboard];
        juce::StringArray out;
        NSArray* urls = [pb readObjectsForClasses: @[ [NSURL class] ]
                                          options: @{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
        for (NSURL* u in urls)
            if (NSString* p = [[u filePathURL] path])   // resolves Finder's file-reference URLs
                out.add(juce::String::fromUTF8([p UTF8String]));
        if (out.isEmpty())
        {
            id list = [pb propertyListForType: kLegacyFilenames];
            if ([list isKindOfClass: [NSArray class]])
                for (id s in (NSArray*) list)
                    if ([s isKindOfClass: [NSString class]])
                        out.add(juce::String::fromUTF8([(NSString*) s UTF8String]));
        }
        return out;
    }

    // A HATCH print dragged from a LoopNest overlay (HatchOverlay's runtime class): the
    // rack refuses it (Andrew, 10-18), so letting it go over the plugin is a cancel.
    bool fromHatch(id<NSDraggingInfo> info)
    {
        id src = [info draggingSource];
        return src != nil && [NSStringFromClass([src class]) hasPrefix: @"LoopNestHatchView_"];
    }

    juce::String typesOf(id<NSDraggingInfo> info)
    {
        return juce::String::fromUTF8([[[[info draggingPasteboard] types] componentsJoinedByString: @", "] UTF8String]);
    }

    // We always copy. Copy when the source allows it, else the closest it offers that
    // never tells it to delete its own file.
    NSDragOperation opFor(id<NSDraggingInfo> info)
    {
        const NSDragOperation mask = [info draggingSourceOperationMask];
        if (mask & NSDragOperationCopy)    return NSDragOperationCopy;
        if (mask & NSDragOperationGeneric) return NSDragOperationGeneric;
        if (mask & NSDragOperationLink)    return NSDragOperationLink;
        return NSDragOperationNone;
    }
}

struct RackDropTarget::Impl
{
    NSView* view = nil;
    bool accepted = false, hovering = false;
    juce::StringArray paths;

    static Class viewClass()
    {
        static Class cls = []
        {
            const auto name = "LoopNestDropView_" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64());
            Class c = objc_allocateClassPair([NSView class], name.toRawUTF8(), 0);
            class_addIvar(c, kOwnerIvar, sizeof(void*), (uint8_t) log2(sizeof(void*)), "^v");

            addMethod(c, @selector(isOpaque), "c@:", ^BOOL(id) { return NO; });
            addMethod(c, @selector(isFlipped), "c@:", ^BOOL(id) { return YES; });
            addMethod(c, @selector(drawRect:), "v@:{CGRect={CGPoint=dd}{CGSize=dd}}", ^(id, NSRect) {});
            // Clicks, scroll and hover pass through to the page's rack below.
            addMethod(c, @selector(hitTest:), "@@:{CGPoint=dd}", ^id(id, NSPoint) { return nil; });

            // NSDraggingDestination
            addMethod(c, @selector(draggingEntered:), "Q@:@", ^NSDragOperation(id self, id<NSDraggingInfo> info)
            {
                auto* o = ownerOf(self);
                if (o == nullptr) return NSDragOperationNone;
                auto& im = *o->impl;
                ++o->dragsEntered;
                im.paths = pathsOf(info);
                const NSDragOperation op = opFor(info);
                const bool hatch = fromHatch(info);
                im.accepted = op != NSDragOperationNone && ! hatch && o->accepts && o->accepts(im.paths);
                if (o->log)
                    o->log("drop target: drag entered, " + juce::String(im.paths.size()) + " path(s), "
                           + (im.accepted ? "accepted" : hatch ? "refused (a HATCH print)" : "refused") + "; types: " + typesOf(info)
                           + (im.paths.isEmpty() ? juce::String() : "; first: " + im.paths[0]));
                im.setHover(*o, im.accepted);
                return im.accepted ? op : NSDragOperationNone;
            });
            addMethod(c, @selector(draggingUpdated:), "Q@:@", ^NSDragOperation(id self, id<NSDraggingInfo> info)
            {
                auto* o = ownerOf(self);
                return o != nullptr && o->impl->accepted ? opFor(info) : NSDragOperationNone;
            });
            addMethod(c, @selector(draggingExited:), "v@:@", ^(id self, id<NSDraggingInfo>)
            {
                if (auto* o = ownerOf(self)) o->impl->setHover(*o, false);
            });
            addMethod(c, @selector(prepareForDragOperation:), "c@:@", ^BOOL(id self, id<NSDraggingInfo>)
            {
                auto* o = ownerOf(self);
                return o != nullptr && o->impl->accepted;
            });
            addMethod(c, @selector(performDragOperation:), "c@:@", ^BOOL(id self, id<NSDraggingInfo> info)
            {
                auto* o = ownerOf(self);
                if (o == nullptr) return NO;
                auto& im = *o->impl;
                im.setHover(*o, false);
                if (! im.accepted) return NO;
                const auto dropped = pathsOf(info);
                const bool ok = o->onDrop && o->onDrop(dropped);
                if (ok) ++o->drops;
                if (o->log) o->log("drop target: drop of " + juce::String(dropped.size()) + " path(s) -> " + (ok ? "taken" : "refused"));
                return ok ? YES : NO;
            });
            addMethod(c, @selector(draggingEnded:), "v@:@", ^(id self, id<NSDraggingInfo>)
            {
                if (auto* o = ownerOf(self)) { o->impl->accepted = false; o->impl->setHover(*o, false); }
            });
            addMethod(c, @selector(wantsPeriodicDraggingUpdates), "c@:", ^BOOL(id) { return NO; });
            class_addProtocol(c, @protocol(NSDraggingDestination));
            objc_registerClassPair(c);
            return c;
        }();
        return cls;
    }

    void setHover(RackDropTarget& o, bool h)
    {
        if (h == hovering) return;
        hovering = h;
        if (o.onHover) o.onHover(h);
    }
};

RackDropTarget::RackDropTarget() : impl(std::make_unique<Impl>())
{
    NSView* v = [[Impl::viewClass() alloc] initWithFrame: NSMakeRect(0, 0, 10, 10)];
    if (auto** slot = ownerSlot(v)) *slot = this;
    [v registerForDraggedTypes: @[ NSPasteboardTypeFileURL, kLegacyFilenames ]];
    impl->view = v;
    setView(v);        // NSViewComponent retains it
    [v release];
}

bool RackDropTarget::bringToFront()
{
    NSView* v = impl->view;
    NSView* sup = [v superview];
    if (sup == nil) return false;
    NSArray* subs = [sup subviews];
    const NSUInteger at = [subs indexOfObjectIdenticalTo: v];
    bool buried = false;
    for (NSUInteger i = at + 1; i < [subs count]; ++i)
        if (! [NSStringFromClass([[subs objectAtIndex: i] class]) hasPrefix: @"LoopNest"])
            buried = true;
    if (! buried) return false;
    [v retain];
    [v removeFromSuperview];
    [sup addSubview: v positioned: NSWindowAbove relativeTo: nil];
    [v release];
    return true;
}

juce::String RackDropTarget::hitAtCentre() const
{
    NSView* v = impl->view;
    NSWindow* w = [v window];
    if (w == nil) return "no window";
    if ([v superview] == nil) return "no superview";
    if ([v isHidden]) return "hidden";
    const NSRect b = [v bounds];
    const NSPoint inWin = [v convertPoint: NSMakePoint(NSMidX(b), NSMidY(b)) toView: nil];
    NSView* content = [w contentView];
    NSView* root = [content superview] != nil ? [content superview] : content;
    NSView* hit = [root hitTest: inWin];
    if (hit == v) return "overlay";
    return hit == nil ? juce::String("nothing") : juce::String([NSStringFromClass([hit class]) UTF8String]);
}

RackDropTarget::~RackDropTarget()
{
    if (auto** slot = ownerSlot(impl->view)) *slot = nullptr;
    [impl->view unregisterDraggedTypes];
    setView(nullptr);
}
