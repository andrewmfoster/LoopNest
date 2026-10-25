#pragma once
#include "PluginProcessor.h"
#include "UiBridge.h"
#include "HatchOverlay.h"
#include "RackDrop.h"

// v2 stage 2: the editor is a WebView (JUCE 8 WebBrowserComponent) showing the
// locked mock (ui/index.html, bundled into BinaryData, served by a resource
// provider; nothing loads from the network), plus the native HATCH drag overlay and
// the rack's drag-in drop target.
//
// The page talks to the plugin three ways:
//   params   WebSliderRelay / WebToggleButtonRelay / WebComboBoxRelay (reverbType) per
//            apvts parameter, relay name = parameter id (host automation, gestures and
//            undo ride the attachments)
//   actions  native functions: ln_ready, ln_spin, ln_spinPick, ln_back, ln_forward, ln_play, ln_loop, ln_live, ln_sync,
//            ln_printFx, ln_preset, ln_presetStep, ln_seek, ln_bank, ln_extract,
//            ln_extractCancel, ln_overlay, ln_pick, ln_openRecent, ln_resize, ln_bpm,
//            ln_bpmFit, ln_barOrigin, ln_bins, ln_bin, ln_binNew, ln_binShow, ln_tempo
//   events   ln_state (30 Hz), ln_files, ln_peaks, ln_hatch (overlay hover/press),
//            ln_hatched (a print finished), ln_drop (a drag-in over the rack)
// The full contract is in ui/BRIDGE.md.
class LoopNestWebEditor : public juce::AudioProcessorEditor,
                          private juce::Timer
{
public:
    explicit LoopNestWebEditor(LoopNestProcessor&);
    ~LoopNestWebEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // The window is the reference space x scale, resizable at a fixed aspect from
    // kMinScale to kMaxScale (the host's frame, or the page's corner grip).
    static constexpr float kRefW = 1672.0f, kRefH = 941.0f, kScale = 0.8f,
                           kMinScale = 0.6f, kMaxScale = 1.25f;
    // The HATCH tile in the mock's reference space (#hatch: left 1452, top 454, 196 x 66).
    static juce::Rectangle<float> hatchTileRef() { return { 1452.0f, 454.0f, 196.0f, 66.0f }; }
    // The tape rack in the same space (#nest 24, 76 + #n-rack 14, 58; 282 x 372): the drag-in target.
    static juce::Rectangle<float> rackRef() { return { 38.0f, 134.0f, 282.0f, 372.0f }; }

    // For the UI harness: the overlay, the bridge and the browser.
    HatchOverlay& getOverlay() { return overlay; }
    RackDropTarget& getRackDrop() { return rackDrop; }
    UiBridge& getBridge() { return bridge; }
    juce::WebBrowserComponent& getBrowser() { return *browser; }

private:
    void timerCallback() override;
    void emit(const char* id, const juce::var& v);
    void sendAll();              // on ln_ready: files, peaks, state
    void scanPeaks(const juce::File&);
    void chooseFolder();
    void startExtraction();
    void updateOverlay();
    void setScale(float);
    // Diagnostics for the HATCH overlay: ~/Library/Logs/LoopNest/hatch.log, written only
    // in a build with the CMake option LOOPNEST_DIAG_LOG=ON (a no-op otherwise).
    void hatchLog(const juce::String&);
    std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url);

    LoopNestProcessor& processor;
    UiBridge bridge;

    struct SliderLink
    {
        std::unique_ptr<juce::WebSliderRelay> relay;
        std::unique_ptr<juce::WebSliderParameterAttachment> attachment;
    };
    struct ToggleLink
    {
        std::unique_ptr<juce::WebToggleButtonRelay> relay;
        std::unique_ptr<juce::WebToggleButtonParameterAttachment> attachment;
    };
    struct ComboLink
    {
        std::unique_ptr<juce::WebComboBoxRelay> relay;
        std::unique_ptr<juce::WebComboBoxParameterAttachment> attachment;
    };
    std::vector<SliderLink> sliders;
    std::vector<ToggleLink> toggles;
    std::vector<ComboLink> combos;

    std::unique_ptr<juce::WebBrowserComponent> browser;
    RackDropTarget rackDrop;            // declared (and added) before the overlay: see the constructor
    HatchOverlay overlay;
    bool overlayEnabled = true, pageReady = false;
    juce::String lastHit;               // the last hit-test result logged
    float scaleToSave = 0.0f;           // a resize to remember, saved once it settles
    int scaleSaveTicks = 0;
    juce::File logFile;

    // Peak scan of the loaded sample (a loader thread of its own; the processor's
    // buffer is audio-thread property). Newest request wins.
    juce::var lastPeaks, lastFiles;
    std::atomic<int> peakGen { 0 };
    std::unique_ptr<juce::Thread> peakThread;
    juce::File peakRequest;
    juce::CriticalSection peakLock;
    juce::WaitableEvent peakWake;

    std::unique_ptr<juce::FileChooser> chooser, destChooser;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopNestWebEditor)
};
