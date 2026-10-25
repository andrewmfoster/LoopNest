#pragma once
#include <JuceHeader.h>

// Drag-in (Andrew, 10-17): the drop target over the web UI's tape rack. The
// WKWebView takes every drag over it for itself, so a transparent native NSView
// sits above the rack (as HatchOverlay sits above HATCH) and registers for file
// drags: `public.file-url` and the legacy NSFilenamesPboardType. It returns nil from
// hitTest:, so clicks and scroll fall through to the page's rack; only a drag
// session finds it. Hover goes back to the page as an event (the rack's teal light).
// Add this AFTER the browser so its view stacks above it.
class RackDropTarget : public juce::NSViewComponent
{
public:
    RackDropTarget();
    ~RackDropTarget() override;

    // Asked once as a drag enters, with the dragged paths: may these drop? False
    // refuses the drag (no copy cursor, no hover). A HATCH print from a LoopNest overlay
    // is refused before this is asked.
    std::function<bool(const juce::StringArray&)> accepts;
    std::function<void(bool)> onHover;                       // an accepted drag entered / left
    std::function<bool(const juce::StringArray&)> onDrop;    // the drop; true = taken

    // For the harness and tests: drags seen entering, and drops taken.
    int dragsEntered = 0, drops = 0;

    std::function<void(const juce::String&)> log;   // diagnostics, as HatchOverlay's
    // Keep the view above the WKWebView when a host re-adds it. Only another
    // LoopNest overlay may sit above (HatchOverlay keeps itself last), so the two
    // never trade places every frame. True when it had to move.
    bool bringToFront();
    // What a click at the view's centre reaches: the view below ("WKWebView"), never
    // "overlay", while hitTest: passes clicks through.
    juce::String hitAtCentre() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RackDropTarget)
};
