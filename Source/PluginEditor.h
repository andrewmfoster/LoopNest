#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <array>

// ===================================================================
// LoopNest — monochrome-blueprint UI.
// Native re-skin of the working editor (no DSP/mechanic changes here).
// Target look: mockups/v3-tape/FINAL.html, authored at 1672x941 and
// drawn here in that reference space with a global scale transform.
// ===================================================================

// Brushed-aluminium rotary knobs with a knurled skirt, engraved pointer and an
// arc of tick dots — used for the Output knob and the six character knobs.
class TapeLookAndFeel : public juce::LookAndFeel_V4
{
public:
    TapeLookAndFeel();

    // Per-INSTANCE dormant flag (BYPASS engaged → teal accents drop to grey).
    // A member, not a file-static: two open editors must not fight over one
    // process-wide global at 30 Hz. Set by the owning editor's refresh().
    bool fxDormant = false;
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h,
                          float sliderPos, float startAngle, float endAngle,
                          juce::Slider&) override;
    // Centred popup-menu items (the preset dropdown reads centred under its button).
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcutKeyText, const juce::Drawable* icon,
                           const juce::Colour* textColour) override;
};

// Blueprint transport key. PLAY/LOOP are outline keys; PRINT is the teal payoff
// key AND the drag-out source: once a loop is printed it arms, its label becomes
// "DRAG", and dragging off it performs the external file drag into Ableton.
class TransportButton : public juce::Button,
                        private juce::Timer
{
public:
    enum class Kind { play, loop, print };
    explicit TransportButton(Kind k);

    void paintButton(juce::Graphics&, bool highlighted, bool down) override;

    // PRINT-as-drag-source wiring (ignored by play/loop keys).
    std::function<juce::File()> getFile;   // the last rendered loop, or invalid
    void setArmed(bool shouldBeArmed);
    bool isArmed() const { return armed; }

    // Play key: show a pause glyph (instead of the play triangle) while the
    // transport is running, so the icon tracks the PLAY/PAUSE label.
    void setShowPauseGlyph(bool shouldShowPause)
    { if (showPause != shouldShowPause) { showPause = shouldShowPause; repaint(); } }

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp  (const juce::MouseEvent&) override;

private:
    void timerCallback() override;   // tweens armGlow toward the armed target

    Kind kind;
    bool armed = false;        // a render exists and the key is ready to drag
    bool showPause = false;    // play key currently shows the pause glyph
    bool dragLaunched = false; // gesture already started an external drag
    // PRINT only: 0 = outline (looks like PLAY), 1 = full teal fill (DRAG). Fades in
    // when the key arms after a bounce, so the payoff state glows on rather than snaps.
    float armGlow = 0.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportButton)
};

// Top-band toolbar icon: a static blueprint line-art glyph. folder = switch the
// sample folder; funnel = run the drum-loop extraction. Brightens on hover, dims
// when disabled (the funnel dims while an extraction is running).
class IconButton : public juce::Button
{
public:
    enum class Kind { folder, funnel };
    explicit IconButton(Kind k) : juce::Button({}), kind(k) {}

    void paintButton(juce::Graphics&, bool highlighted, bool down) override;

private:
    Kind kind;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IconButton)
};

// Bottom-band A/B monitor controls (iZotope-style). gainMatch + bypass form a split
// pill (gainMatch = fader glyph, left/joined; bypass = "BYPASS" text, right/joined);
// clear is a standalone "CLEAR" chip. gainMatch/bypass are toggles (teal fill when
// engaged); clear is momentary (resets every parameter to INIT).
class MonitorButton : public juce::Button,
                      private juce::Timer
{
public:
    enum class Kind { gainMatch, bypass, clear };
    explicit MonitorButton(Kind k) : juce::Button({}), kind(k) {}

    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
    void clicked() override;             // CLEAR: kick off a brief teal border flash

private:
    void timerCallback() override;       // decays clickGlow toward 0

    Kind  kind;
    float clickGlow = 0.0f;              // 1 → 0 teal-rim pulse after a CLEAR press
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MonitorButton)
};

// Two-handle wet band-pass above the ECHO / REVERB knobs (RC20 "FOCUS" style). A
// horizontal strip with a LOW-CUT handle (left, a left-pointing drag arrow) and a
// HIGH-CUT handle (right, a right-pointing arrow); the kept band between them fills
// teal. Each handle is a 0..1 position on a log frequency axis. Drives two apvts
// params directly (no SliderAttachment — a slider has one value, this has two).
class BandFilter : public juce::Component,
                   private juce::Timer
{
public:
    BandFilter();
    ~BandFilter() override;

    // Bind the low-cut + high-cut parameters (both 0..1 ranged). Call once in setup.
    void attach(juce::RangedAudioParameter* lo, juce::RangedAudioParameter* hi);

    // Per-instance dormant flag — same contract as TapeLookAndFeel::fxDormant.
    bool fxDormant = false;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp  (const juce::MouseEvent&) override;

private:
    void  timerCallback() override;       // repaint when params move (recall/automation)
    float loVal() const;                  // 0..1 low-cut handle position
    float hiVal() const;                  // 0..1 high-cut handle position
    void  setVal(juce::RangedAudioParameter*, float v01);
    int   pickHandle(float x) const;      // nearest handle to x: 1 = lo, 2 = hi

    juce::RangedAudioParameter* loP = nullptr;
    juce::RangedAudioParameter* hiP = nullptr;
    int   active = 0;                     // drag target: 0 none / 1 lo / 2 hi
    float cachedLo = -1.0f, cachedHi = -1.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BandFilter)
};

// Master DRY/WET strip in the top-right header (RC20 "MAGNITUDE" style): a thin
// horizontal track with a metal handle; teal fill from dry (left) to the handle.
class MixSlider : public juce::Slider
{
public:
    MixSlider();
    void paint(juce::Graphics&) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixSlider)
};

// Factory-preset selector (RC-20 style, blueprint-native). Centred preset name with a
// trailing '*' once the loaded preset has been tweaked off-book; a prev/next chevron
// pair on the right; a click on the name opens a scrolling dropdown of every preset.
// Pure UI — the editor owns the factory table and applies params via onSelect.
class PresetBar : public juce::Component
{
public:
    PresetBar() = default;
    std::function<void(int)> onSelect;          // fired with the chosen preset index

    void setPresets(juce::StringArray names)    { presets = std::move(names); repaint(); }
    void setCurrent(int idx, bool isDirty);     // idx < 0 = no preset loaded ("INIT")

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    void step(int delta);                       // cycle prev/next (wraps)
    juce::Rectangle<float> arrowZone() const;   // right-end chevron hit area

    juce::StringArray presets;
    int  current = -1;
    bool dirty   = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetBar)
};

// Editable value read-out under each knob. A single click SELECTS it (a teal focus
// outline appears over the box); typing a number — or a double-click — opens an inline
// editor, Return commits the typed value to the parameter, Delete resets the parameter
// to its default (neutral), and Esc / clicking away clears the focus outline.
class ValueChip : public juce::Label
{
public:
    ValueChip();

    std::function<void(const juce::String&)> onCommitText;   // parse + apply a typed value
    std::function<void()>                    onResetDefault; // reset the param to its default
    juce::Slider* linkedSlider = nullptr;                    // knob this mirrors (drag-to-adjust)

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusGained(FocusChangeType) override;
    void focusLost(FocusChangeType) override;
    void paint(juce::Graphics&) override;

private:
    void startEditing(const juce::String& initial);
    bool   selected = false;   // has focus → draw the teal outline
    bool   dragging = false;   // an Ableton-style value drag is in progress
    double dragStartProp = 0.0;// slider proportion at mouse-down
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ValueChip)
};

// The cassette bay: dark window recess with two spinning reels + a brass
// capstan. The reels ARE the spin control — clicking spins. Idle-spins gently.
class ReelBay : public juce::Component,
                private juce::Timer
{
public:
    ReelBay();
    ~ReelBay() override;

    std::function<void()> onSpin;     // clicked while a spin is allowed
    std::function<bool()> canSpin;    // gate so an empty folder doesn't react
    std::function<bool()> isPlaying;  // reels revolve only while the tape plays

    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void drawReel(juce::Graphics&, juce::Point<float> c, float r, float angle) const;
    // Static cassette shell rasterised once (see paint()) — reels draw live on top.
    void paintShell(juce::Graphics&);
    juce::Image shellCache;

    // Independent per-reel accumulators, each wrapped at 2π so the wrap is
    // seamless for both (2π is a whole multiple of each reel's symmetry period).
    // A single shared phase scaled by a non-integer factor would jump on wrap.
    float phase  = 0.0f;  // reel 1 rotation
    float phase2 = 0.0f;  // reel 2 rotation (slower, opposite sense)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReelBay)
};

// The big "RE-SPIN" dial under the cassette: a large line-art rotor ringed by two
// curved spin-arrows. It is a TRIGGER, not a parameter — clicking re-spins.
// Brightens on hover; dims when the folder is empty.
class SpinDial : public juce::Component,
                 private juce::Timer
{
public:
    SpinDial();
    ~SpinDial() override;

    std::function<void()> onSpin;
    std::function<bool()> canSpin;

    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override { hovered = true;  repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }

private:
    void timerCallback() override;
    float spin     = 0.0f; // current draw angle (0 == rest); one click = a full eased tween home
    float spinProg = 0.0f; // 0..1 progress through the current spin tween
    bool  spinning = false;// a tween is in flight
    bool  hovered  = false;
    static constexpr int   kSpinTurns = 3;       // whole revolutions per spin (lands home)
    static constexpr float kSpinStep  = 0.022f;  // progress/frame @30Hz → ~1.5 s tween
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpinDial)
};

// Teal "VFD/scope" waveform: near-black screen, teal scanlines + trace, a
// folder/filename header band, a timecode footer, flag-pennant trim handles and
// a bright scrubber line. Clicking the header band chooses a folder.
class WaveformDisplay : public juce::Component,
                        private juce::ChangeListener
{
public:
    explicit WaveformDisplay(LoopNestProcessor&);
    ~WaveformDisplay() override;

    void setFile(const juce::File&);

    std::function<void()> onChooseFolder;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify(const juce::MouseEvent&, float scaleFactor) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    float startNorm() const;
    float endNorm()   const;
    void  setTrim(const juce::String& paramID, float value01);

    // Static-layer cache: screen chrome + header text + centre axis + thumbnail
    // trace + zoom rail, rasterised once into an image (at physical-pixel scale,
    // so retina stays sharp). paint() blits it and draws only the cheap dynamic
    // overlays (trim, playhead, footer) — re-rasterising the thumbnail at every
    // 30 Hz playhead tick was the UI's single biggest recurring paint cost.
    void paintChrome(juce::Graphics&, const juce::String& folder,
                     const juce::String& name, bool missing);
    juce::Image  waveCache;
    bool         waveCacheDirty = true;
    float        cacheViewStart = -1.0f, cacheViewEnd = -1.0f;
    juce::String cacheHeaderKey;

    enum class Handle { none, start, end };
    Handle dragging = Handle::none;

    // Horizontal zoom: the [viewStart, viewEnd] window (0..1 of the sample) the
    // scope currently shows. Full file at rest; trackpad pinch (mouseMagnify)
    // zooms it, anchored at the cursor; two-finger scroll / Alt-drag pans.
    // Display only — never touches the trim params or the export.
    float viewStart = 0.0f, viewEnd = 1.0f;
    bool  panning   = false;   // Alt-drag pan in progress
    float lastPanX  = 0.0f;    // previous mouse x while panning
    void  setView(float start, float end);   // clamps + enforces a min span

    // Pixel x <-> 0..1 sample position, mapped through the wave area.
    float xToNorm(float x) const;
    float normToX(float n) const;
    // Is point p inside the top-triangle grab zone of the given bracket?
    bool  overHandle(juce::Point<float> p, Handle which) const;

    juce::Rectangle<float> waveArea()   const;  // region the trace + handles live in
    juce::Rectangle<float> headerArea() const;  // clickable folder/filename band

    LoopNestProcessor& processor;

    juce::AudioFormatManager  formatManager;
    juce::AudioThumbnailCache thumbnailCache { 8 };
    juce::AudioThumbnail      thumbnail { 512, formatManager, thumbnailCache };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformDisplay)
};

class LoopNestEditor : public juce::AudioProcessorEditor,
                           public  juce::DragAndDropContainer,
                           private juce::Timer
{
public:
    explicit LoopNestEditor(LoopNestProcessor&);
    ~LoopNestEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;   // click-away clears a focused value chip

private:
    void timerCallback() override;
    void refresh();
    void chooseFolder();
    void cashOut();

    // Drum-loop curation flow: pick source folder → pick/name destination →
    // confirm if it already holds files → kick off the background extraction.
    void startExtraction();
    void chooseExtractDest(juce::File src);
    void confirmAndExtract(juce::File src, juce::File dest);

    // Reference-space (1672x941) -> window-pixel rectangle for laying out kids.
    juce::Rectangle<int> R(float x, float y, float w, float h) const;

    LoopNestProcessor& processor;
    TapeLookAndFeel lnf;

    ReelBay         reels;
    SpinDial        respin;
    WaveformDisplay waveform;

    TransportButton playButton  { TransportButton::Kind::play };
    TransportButton loopButton  { TransportButton::Kind::loop };
    TransportButton printButton { TransportButton::Kind::print };

    // Top-band toolbar: switch the sample folder, or run drum-loop extraction.
    IconButton folderButton  { IconButton::Kind::folder };
    IconButton extractButton { IconButton::Kind::funnel };

    // Six character knobs in the right-hand band: WARP · PITCH · DRIVE ·
    // FLUTTER · ECHO · REVERB. OUTPUT lives separately, tucked in the cassette
    // column's bottom-right corner (it's a playback level, not a character knob).
    static constexpr int kNumKnobs = 6;
    struct KnobControl
    {
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    // The six character knobs show their value as an RC-20-style pill that pops on
    // turn/hover (Slider popup display) — no value chips, names sit under each knob.
    std::array<KnobControl, kNumKnobs> knobs;
    KnobControl inputKnob;    // INPUT level (pre-rack gain stage), above OUTPUT
    ValueChip   inputValue;   // INPUT read-out chip (mirrors OUTPUT)
    KnobControl outputKnob;   // OUTPUT level, in the cassette-column corner
    ValueChip   outputValue;  // OUTPUT keeps its editable read-out chip (no pill)
    KnobControl pitchKnob;    // PITCH (−12..+12 st) — clean transpose, in the SPIN-column
    ValueChip   pitchValue;   // row beside INPUT/OUTPUT. Out of presets (key is the user's).

    // One secondary control per knob (same column index), in the slot above the main
    // knob. idx0-3 are SMALL rotary knobs (the rate/glide/tone partner axes); idx4-5
    // keep the bipolar echo/reverb wet-filter bands. No value read-outs (intentional).
    juce::Slider warpRateKnob;    // idx0 WARP  — rate  (unipolar)
    juce::Slider flutterRateKnob; // idx1 FLUT  — rate  (unipolar)
    juce::Slider driveToneKnob;   // idx2 DRIVE — tone  (bipolar)
    BandFilter   echoBand;        // idx3 ECHO  — two-handle wet band-pass
    BandFilter   reverbBand;      // idx4 REVERB— two-handle wet band-pass
    BandFilter   widthBand;       // idx5 WIDTH — two-handle stereo-width band
    BandFilter   eqBand;          // master EQ (pre-rack) — two-handle band, bottom band
    // Tertiary rotaries for the three band cells (idx3/4/5), in the slot below the band.
    juce::Slider echoTimeKnob;    // idx3 ECHO   — TIME   (free slapback ms)
    juce::Slider reverbDecayKnob; // idx4 REVERB — DECAY  (room/tail length)
    juce::Slider widthMidKnob;    // idx5 WIDTH  — CENTER (mid ±dB)
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        warpRateAttach, driveToneAttach, flutterRateAttach,
        echoTimeAttach, reverbDecayAttach, widthMidAttach;

    // Master DRY/WET strip in the top-right header.
    MixSlider mixSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttach;

    // Factory presets — the bottom-band selector (between EQ and MIX). A preset
    // overwrites ONLY the CHARACTER panel knobs (+ 2nd axes), the echo/reverb wet
    // bands, and the master EQ; pitch, levels, mix and trim are left to the user.
    PresetBar presetBar;
    int       currentPreset = -1;          // -1 = none loaded ("INIT"); else table idx
    void      applyPreset(int idx);        // push the preset's values to the params
    bool      presetMatches(int idx) const;// do the live params equal preset idx?

    // A/B monitor cluster (bottom band, right of the shrunk preset bar). gainMatch +
    // bypass attach to the bool params; clear is momentary → resets all params.
    MonitorButton gainMatchButton { MonitorButton::Kind::gainMatch };
    MonitorButton bypassButton    { MonitorButton::Kind::bypass };
    MonitorButton clearButton     { MonitorButton::Kind::clear };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        bypassAttach, gainMatchAttach;
    void clearAllParams();                 // reset every param to its INIT default

    juce::File   renderedFile;
    // Keyed on the FULL path, not the basename: the recursive multi-pack scan makes
    // duplicate basenames realistic, and a same-name spin must still refresh the
    // scope and drop the stale armed render.
    juce::String lastSamplePath;
    juce::String lastStatusSig;       // gate faceplate repaints to real state changes

    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::FileChooser> extractChooser;  // source/dest pickers for curation

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopNestEditor)
};
