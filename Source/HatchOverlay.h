#pragma once
#include <JuceHeader.h>

// v2 stage 2: the HATCH drag-out source. A WKWebView can't start a file drag to
// the host, so a transparent native NSView sits over the web UI's HATCH tile and
// owns every mouse event on it: hover and press go back to the page as events (so
// the tile still lights), a click hatches, and a drag, once a print is armed,
// starts an AppKit dragging session with the .wav as a file URL (the same
// pasteboard shape as JUCE's performExternalDragDropOfFiles, which v1 used).
//
// It must be an NSView, not a JUCE component: the WebBrowserComponent is itself a
// native subview of the editor's peer, so a lightweight JUCE component "over" it
// never sees the mouse. Add this AFTER the browser so its view stacks above it.
class HatchOverlay : public juce::NSViewComponent
{
public:
    HatchOverlay();
    ~HatchOverlay() override;

    std::function<void(bool)> onHover;     // pointer entered / left the tile
    std::function<void(bool)> onPress;     // mouse down / up (visual only)
    std::function<void()>     onClick;     // a press released without a drag
    // Asked when a drag begins: the file to drag, or an empty File for none (no
    // print armed, or the print is stale). Called on the message thread.
    std::function<juce::File()> fileToDrag;
    std::function<void()>     onDragStart; // a drag session actually began
    std::function<void(bool)> onDragEnd;   // the session ended; true = dropped somewhere

    // For the standalone proof and tests: the number of sessions begun / ended.
    int dragsBegun = 0, dragsEnded = 0;

    // Diagnostics (09-30: HATCH clicks did nothing in Live). Every pointer event on
    // the view is reported here when set.
    std::function<void(const juce::String&)> log;
    // Hosts can re-add the WKWebView after this view, burying it: put the view back
    // on top of its siblings. True when it had to move.
    bool bringToFront();
    // What the window's hit test finds at the view's centre: "overlay" when it is
    // this view, else the class of the view that would get the click.
    juce::String hitAtCentre() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HatchOverlay)
};
