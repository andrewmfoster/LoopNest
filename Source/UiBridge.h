#pragma once
#include "PluginProcessor.h"
#include <deque>

// v2 stage 2: the web UI's native side, minus the WebView. Every action the page
// can call and every piece of state it renders goes through here, so the test
// target can drive the bridge headless (no WebBrowserComponent, no window).
// Parameters don't: they ride WebSliderRelay / WebToggleButtonRelay attachments in
// the editor (host automation, gestures and undo come with those for free).
// Message thread only (but drop()'s worker).
class UiBridge
{
public:
    explicit UiBridge(LoopNestProcessor&);
    // Ends any parameter gesture the page left open (C-M11) and stops the drop worker.
    ~UiBridge();

    // --- actions (the page's native functions) ---------------------------------
    // SPIN always lands in AUDITION; from LIVE the audition starts when the new loop
    // lands (so the clip on the track isn't doubled by a burst of the old loop).
    void spin();
    // PLAY: from LIVE, back to AUDITION and playing; otherwise play/pause.
    void togglePlay();
    void setLooping(bool);
    // The AUDITION | LIVE toggle. LIVE = the processor's FX mode.
    void setLive(bool live);
    void setSync(bool);          // the SYNC button; either way clears a free tempo
    void setTempo(double bpm);   // the tempo cell, dragged: SYNC off, play at this tempo (<= 0 clears)
    // The print row's FX switch: on = the chain bakes into the print (a wet hatch),
    // off = dry (the chain keeps playing live on the track).
    void setPrintFx(bool);
    // ◀ ▶ step through INIT + the factory presets, wrapping (the mock's cycle);
    // select(-1) is INIT. A preset is one host undo step (restore(kCharacter)).
    void stepPreset(int delta);
    void selectPreset(int idx);
    void seek(float norm);
    void setBpm(double bpm);   // the loop's tempo by hand; <= 0 = back to automatic
    bool fitBpm();             // ...read off the trim handles, once (1/2/4/8 bars); bar 1 to the start handle
    // Print bins: subfolders of the Prints root, scanned when asked (never cached).
    juce::var bins() const;                    // [names]
    bool setBin(const juce::String& name);     // "" = the root
    bool newBin(const juce::String& name);     // create + select
    bool showBin() const;                      // Finder: the folder HATCH prints into now
    void setBarOrigin(double norm);   // where bar 1 starts, 0..1 of the file (0 = reset)
    // Stage 3: load the folder's loop at this index (a rack click, ◀ ▶, a typed
    // number). Like SPIN it always lands in AUDITION, playing when it came from LIVE.
    bool pick(int index);
    // SPIN during a rack search: the page picks among the matches, and it is a spin
    // (the spin history records it, as ln_spin), not a rack pick.
    bool spinPick(int index);
    // Spin history ◀ ▶ (LoopNestProcessor::historyBack): the whole sound of the loop
    // before / after, landing like a spin (AUDITION; from LIVE it plays once loaded).
    // False at either end.
    bool historyStep(int delta);
    // A BANK popup recent: switch to that folder. Only paths in the recents list.
    bool openRecent(const juce::String& path);

    // --- drag-in (driven by the native RackDropTarget, not the page) -------------
    // Whether files dragged over the rack may drop: a bank, and only audio files
    // (BankDrop.h's rules). False lights nothing and refuses the drop.
    bool canDrop(const juce::StringArray& paths) const;
    // The drop: false when refused (canDrop's rules). Accepted, it queues for a worker
    // thread that copies each file into the bank (same bytes skipped, a taken name gets
    // ` 2`, ` 3`) and rescans, off the message thread (U-M3). Back on the message thread,
    // in AUDITION the first copied file is picked like a rack pick (trim reset, knobs
    // kept), unless a load was asked for since the drop; in LIVE it is only copied.
    // Drops land in order. A drop where nothing landed is logged (log).
    bool drop(const juce::StringArray& paths);
    // For the tests: wait up to timeoutMs for the queued drops to copy and rescan, then
    // land them here. True when none was left waiting.
    bool finishDrops(int timeoutMs);

    // --- HATCH (driven by the native overlay, not the page) ---------------------
    enum class Hatch { none, idle, armed, stale };   // none = no sample loaded
    Hatch hatchState() const;
    // Stops a free audition (one the host's transport owns plays on) and prints.
    // True when a print is armed afterwards.
    bool hatch();
    // The file a drag may carry: the armed print when it's current, else {}.
    juce::File dragFile() const;
    // A drag began: the track takes over from the audition (AUDITION -> LIVE).
    void dragStarted();
    // The drag ended. Not dropped (Esc, or let go over the plugin): back to AUDITION,
    // playing if it was, when the start flipped it from there (U-M5). Dropped: LIVE stays.
    void dragEnded(bool dropped);

    std::function<void(const juce::String&)> log;   // diagnostics (the editor's HATCH log)
    juce::File lastPrint() const { return printed; }

    // --- state for the page ----------------------------------------------------
    // Everything non-parameter the page draws, polled at 30 Hz. Also re-resolves
    // the preset name after a host undo, like the native editor's refresh().
    juce::var state();
    // The rack's file list (plus the recent folders), when the processor's list
    // generation moved since the last call (folder switch, rescan, extraction adopt, a
    // drop) or the recents' order did; else nothing is built (U-M2).
    std::optional<juce::var> takeFilesIfChanged();
    // The loaded sample's path whenever it changes, for the peak scan. The armed
    // print is dropped on a change (it no longer matches what's loaded).
    std::optional<juce::File> takeSampleIfChanged();

    int  currentPreset() const { return proc.getPresetIndex(); }   // lives on the processor (U-M4)
    bool presetDirty() const;
    static juce::String presetName(int idx);   // "INIT" for -1

    // Peak envelope of a buffer, n columns of max |sample| over all channels, 0..1.
    static std::vector<float> computePeaks(const juce::AudioBuffer<float>&, int n);
    // The drum hits in a buffer: the frame each attack starts on, ascending. What the
    // page's SNAP lands the trim handles on. Energy rises in the full band and in a
    // high band (so a hat over a ringing kick still counts), at least kHitGapMs apart.
    static std::vector<int> computeHits(const juce::AudioBuffer<float>&, double sampleRate);
    // Both give up early (a partial or empty result) when run on a juce::Thread that
    // has been asked to exit: the editor's peak worker, which then discards it.
    static constexpr double kHitGapMs = 45.0;

private:
    bool presetMatches(int idx) const;
    bool characterAtDefaults() const;
    bool allPanelsOn() const;           // every panel power switch ON (presets and INIT have all ON)

    LoopNestProcessor& proc;
    juce::File printed;          // the armed print, empty = none
    juce::String samplePath;
    bool filesSent = false;      // takeFilesIfChanged sent filesGenSent's list
    juce::uint32 filesGenSent = 0;
    int sampleIndex = -1;        // state's "index", found again only when its keys move
    juce::uint32 indexGen = 0;
    juce::String indexPath;
    bool indexValid = false;
    int binTick = -1;            // state() calls since the bin's folder was last checked (-1 = never)
    bool binMissing = false;
    bool sampleSeen = false;
    bool dragFlipped = false;    // the drag in flight took AUDITION to LIVE
    bool dragWasPlaying = false; // ...and the audition was playing

    // Drag-in's worker (U-M3). Drops queue under dropLock for one thread, which copies
    // and rescans; the newest drop's pick (or its failures) waits for landDrops() on
    // the message thread. The destructor stops it between files and joins it, so it never
    // outlives the bridge (nor the processor, which outlives the editor). The worker
    // touches only the queue, BankDrop and proc.rescanFolder() (any thread).
    struct DropJob { juce::Array<juce::File> items; juce::File bank; };
    void landDrops();
    std::unique_ptr<juce::Thread> dropThread;
    juce::CriticalSection dropLock;
    std::deque<DropJob> dropQueue;
    int dropsBusy = 0;           // queued or copying
    juce::File dropPick;         // empty = nothing to pick
    int dropFailed = 0;          // drops where nothing landed, by failed files, since the last landDrops
    juce::uint32 dropLoadGen = 0;   // the processor's load generation at the last drop or drop pick
    juce::WaitableEvent dropWake;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};
