#pragma once
#include <JuceHeader.h>
#include "CharacterChain.h"
#include "MonitorStage.h"
#include <optional>

// The SYNC stretcher lives behind a pointer. Its Signalsmith Linear FFT
// pulls in <Accelerate/Accelerate.h>, whose MacTypes `Point` collides with
// juce::Point once JuceHeader's `using namespace juce` is in scope — so
// TimeStretch.h is included only at the top of PluginProcessor.cpp, before JUCE.
namespace loopnest { struct SyncStretcher; }

// LoopNest: spin for a random loop from your curated folder, shape it, cash it
// out as a .wav you drag onto an Ableton track. Not a MIDI instrument — it
// ignores MIDI and auditions the trimmed region on a loop via a UI Play button.
// v2: an audio EFFECT with two modes — AUDITION (the spun loop, host input
// discarded) and FX (host input through the chain) — plus tempo sync of the
// audition to the host BPM (pitch-preserving time-stretch, PITCH independent).
// Proven by the FX/sync spike (docs/spike-fx-sync.md).
class LoopNestProcessor : public juce::AudioProcessor,
                          public juce::VST3ClientExtensions,
                          private juce::AsyncUpdater,
                          private juce::AudioProcessorListener,
                          private juce::AudioProcessorValueTreeState::Listener,
                          private juce::Timer
{
public:
    LoopNestProcessor();
    ~LoopNestProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    // The host's own bypass (AU BypassEffect, VST3's): the track's audio on the same
    // reported latency, never early (H2); the chain keeps running, unheard, so a toggle
    // either way is a short crossfade between two live signals. Not the `bypass` param.
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    // An audio effect, no MIDI bus (NEEDS_MIDI_INPUT FALSE). Playback is
    // never triggered by MIDI anyway — the audition runs off the UI Play key.
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    // An effect that generates sound on an empty track. Infinite tail (VST3
    // kInfiniteTail) tells the host never to suspend it on silent input.
    double getTailLengthSeconds() const override { return std::numeric_limits<double>::infinity(); }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // --- Spin (called from the editor / message thread) ---
    void setSampleFolder(const juce::File& folder);
    juce::File getSampleFolder() const { const juce::ScopedLock sl(stateLock); return sampleFolder; }
    // Re-walk the current folder into the rack's list (a drag-in copied files there).
    // Any thread. Of two walks of one folder, the later-started one's list stands.
    void rescanFolder();
    // For the tests: runs in rescanFolder between its walk and its publish.
    std::function<void()> afterScanWalk;
    // A drop's copies landed but its rescan was skipped (the editor closed mid-drop):
    // the next editor's bridge rescans. take = read and clear. Any thread.
    void markRescanOwed()  { rescanOwed.store(true); }
    bool takeRescanOwed()  { return rescanOwed.exchange(false); }
    // A restored set's missing sample (B4), looked for again: setSampleFolder calls it,
    // and so does a recent folder picked again (the drive remounted under the same bank).
    void relinkMissingSample();

    // Pick a new random loop from the folder (unlimited). The decode runs on the
    // loader thread; the file, and its trim reset to 0..1, land together a moment
    // later (see restore()). A spin from FX leaves FX as it lands (SYNC goes on then
    // too), and playWhenLoaded starts the audition then, so a spin from FX never plays a
    // burst of the previous loop first. The same for a pick and a history step.
    void spin(bool playWhenLoaded = false);
    // A spin that lands on this file of the folder: SPIN among a rack search's matches
    // (the page picks the row). Recorded by the spin history as any spin is. False when
    // the file isn't in the current folder's list.
    bool spinTo(const juce::File&, bool playWhenLoaded = false);
    // v2 stage 3: load one named loop (a rack click, the stepper, a typed number),
    // with the same trim reset and async landing as a spin. False when the file
    // isn't in the current folder's list.
    bool loadSampleFile(const juce::File&, bool playWhenLoaded = false);

    // --- Spin history: BACK / forward (Andrew, 10-11) --------------------------------
    // Only SPIN records (Andrew, 10-11): a spin keeps the sound it replaces (whatever
    // is current, a rack-picked loop with its knobs included) and the loop it brings,
    // each as the whole sound (capture(), plus its tempo and bar-1 anchor):
    // kHistoryBack back steps (kHistoryBack + 1 entries with the current one). A rack
    // pick (row, ◀ ▶, a typed number) adds nothing and drops nothing.
    // historyBack() restores the previous entry and historyForward() the next, skipping
    // any whose file is gone; a spin after stepping back drops the forward ones. A step
    // loads through the same path as a spin (even for the loaded file), so it lands
    // async and, under the bar-grid lock, swaps on the next bar line. Like a spin it
    // turns SYNC on. A spin or step whose file won't decode takes its entry back out.
    // A host state restore starts the history again (at the next spin, pick or step,
    // with the restored sound as its first entry). Session only: not saved with the
    // set. False (no move) when there is nothing that way. Message thread.
    static constexpr int kHistoryBack = 20;
    bool historyBack(bool playWhenLoaded = false)    { return stepHistory(-1, playWhenLoaded); }
    bool historyForward(bool playWhenLoaded = false) { return stepHistory(+1, playWhenLoaded); }
    bool canHistoryBack() const    { return historyTarget(-1) >= 0; }
    bool canHistoryForward() const { return historyTarget(+1) >= 0; }
    int  getHistorySize() const    { return historyResetPending.load() ? 0 : (int) history.size(); }
    int  getHistoryPos() const     { return historyResetPending.load() ? -1 : historyPos; }   // -1 = none yet

    // --- v2 stage 0: one restore path --------------------------------------------
    // capture() reads the whole sound (Snapshot.h). restore() applies the chosen
    // parts as ONE host undo step (begin every gesture, set every value, end every
    // gesture). With kFile and a file other than the loaded one, the file decodes
    // off the message thread first and the rest applies atomically with the buffer
    // swap: the audition is muted from the swap until the params are in, so the new
    // file never plays under the old trim (the L2 race). Returns false only when
    // kFile names a file that doesn't exist. An empty kFile file (a capture from
    // before the first spin) clears the sample. Any accepted kFile restore supersedes
    // an older pending load, even for the file already loaded; a restore without kFile
    // made while a load is pending takes its parts back from that load, so the newer
    // edit wins when it lands. Message thread.
    loopnest::Snapshot capture() const;
    bool restore(const loopnest::Snapshot&, int parts = loopnest::Snapshot::kAll);
    // CLEAR: every parameter back to its default, as one undo step.
    void resetAllParams();
    // The primitive under restore() and CLEAR. Values are normalised 0..1; params
    // already at their value are skipped, so an unchanged restore adds no undo entry.
    // So is a param the user is mid-drag on (a UI gesture is open): the hand wins,
    // and the host never sees a nested begin on one param.
    using ParamEdit = std::pair<juce::RangedAudioParameter*, float>;
    void setParamsAsOneStep(const std::vector<ParamEdit>&);

    // A spin/restore/state load is still decoding or waiting to be installed.
    bool isLoadPending() const { return landedGen.load() != loadGen.load(); }
    // Bumped by every load request (spin, pick, history, restore, host state).
    juce::uint32 getLoadGeneration() const { return loadGen.load(); }
    // Blocks until the pending load lands (installing it here, so the caller must
    // be the message thread) or timeoutMs passes. True when nothing is pending.
    // For hosts-less callers (the test target); the plugin installs via AsyncUpdater.
    bool finishPendingLoad(int timeoutMs);
    // A decode has finished and waits for the message-thread install. For the test
    // target, which uses it to land a host restore between the two.
    bool hasDecodedLoadWaiting() const { const juce::ScopedLock ql(loadQueueLock); return finishedLoad != nullptr; }

    // Saved-state schema. 0 = pre-v2 (no version property, absolute paths only).
    // 1 = v2: stateVersion, plus the sample path relative to its bank ("sampleRel").
    // 2 = + the loop's tempo ("loopBpm", "loopBpmSrc"). A state without one (0 or 1)
    // restores with the tempo those builds used: the saved trim read as bars. A
    // restore never re-reads a saved tempo, so the set stretches as it did.
    // 3 = + the transport-owned mute ("muted", see pressPlay). A state without it
    // (0..2, or no property) loads MUTED (Andrew, 10-11): an AUDITION instance in an
    // old set stays silent when the host starts, and its key reads dark.
    // 4 = WARP / FLUTTER retapered (10-17) and REVERB TYPE ("reverbType"). An older state's
    // warp is converted at its own warpRate (CharacterChain::warpFromOld) and its flutter
    // halved, so the set keeps its wobble; one without reverbType gets HALL (default).
    static constexpr int kStateVersion = 4;

    // --- AUDITION / FX mode -------------------------------------------------
    // AUDITION (default): output the spun loop through the chain; host input is
    // discarded so a clip on the track can't double up with the audition. FX: host
    // input -> input gain -> chain -> mix -> output gain; the audition voice is
    // silent. The editor flips to FX when a hatch drag starts and back on SPIN/PLAY.
    // Message thread writes the atomic; the audio thread notices the mismatch with
    // the mode it's rendering, fades the old path out, resets the chain's echo/
    // reverb/flutter state (no audition tail bleeding into the clip) and fades in.
    void setFxMode(bool fx);
    bool isFxMode() const { return fxMode.load(); }

    // --- Tempo sync (AUDITION + hatch) ----------------------------------------
    // On: the audition runs through a pitch-preserving time-stretcher at
    // tempo ratio hostBPM / nativeBPM (clamped to loopnest::kMin/kMaxSyncRatio
    // in TimeStretch.h),
    // with PITCH applied as an independent transpose. Off: today's varispeed pitch
    // (speed and pitch together), no stretcher.
    void setSyncEnabled(bool on) { syncEnabled.store(on); freeBpm.store(0.0); }
    bool isSyncEnabled() const   { return syncEnabled.load(); }
    // A tempo of LoopNest's own (the tempo cell, dragged): SYNC goes off and the same
    // stretcher runs at freeBpm / nativeBPM, PITCH a transpose, in the audition and the
    // print. It belongs to the loop it was dragged on: a spin or a picked loop always
    // turns SYNC on, which drops it (Andrew, 10-01). It is saved with the set.
    // The SYNC button, either way, clears it: off = the file's own tempo (varispeed).
    void setFreeBpm(double bpm);                 // <= 0 clears it (SYNC stays as it is)
    double getFreeBpm() const { return freeBpm.load(); }
    bool   isStretching() const { return syncEnabled.load() || freeBpm.load() > 0.0; }
    double getTargetBpm() const { return syncEnabled.load() ? lastHostBpm.load() : freeBpm.load(); }

    // The loop's native tempo is a stored value, never re-read per block. Where it
    // came from: a "(NN)bpm" filename tag (name); a length read as 1/2/4/8 bars of
    // 4/4 (length: the whole file when it lands, or the trimmed loop when asked); or
    // the user (typed, halved/doubled). All three HOLD under a trim (Andrew, 10-01:
    // the bar lines are the file's, like a clip's timeline).
    // none = nothing has fit yet (no sync); only then does a trim at rest look for one.
    enum class BpmSource { none, name, length, user };
    // Native BPM of the loaded loop (0 = unknown).
    double getNativeBpm(BpmSource& source) const;
    // The loaded file's "(NN)bpm" tag, 0 = none.
    double getTagBpm() const { const juce::ScopedLock sl(stateLock); return sampleBuffer.getNumSamples() > 0 ? nameBpm : 0.0; }
    // Set the tempo by hand (clamped to kMinLoopBpm..kMaxLoopBpm); <= 0 goes back
    // to the automatic one (tag, else the whole file as bars). Message thread.
    void setLoopBpm(double bpm);
    // Read the tempo off the trim handles, once (even on a tagged file): the region as
    // 1/2/4/8 bars, the reading closest to the host tempo. False (and no change) when
    // nothing fits. "This region is the beat": bar 1 moves to the start handle too.
    bool setLoopBpmFromRegion();
    // Where bar 1 starts, 0..1 of the file (0 = its first sample): the bar grid's
    // anchor, for a file whose downbeat is not its first sample. Display only (the
    // page's grid), saved with the set, back to 0 for each new file.
    double getBarOrigin() const { return barOrigin.load(); }
    void setBarOrigin(double norm) { barOrigin.store(juce::jlimit(0.0, 1.0, norm)); }
    // What the settle timer does, without its wait: if the trim moved outside a
    // gesture and no trim gesture is open, look for a tempo if there is none. True when
    // it looked (a tempo there already was holds). Message thread; public for the tests.
    bool settleTrimNow();
    static constexpr double kMinLoopBpm = 30.0, kMaxLoopBpm = 300.0;
    // Last tempo the host reported (0 = never), published by the audio thread.
    double getHostBpm() const { return lastHostBpm.load(); }

    // --- Bar-grid lock (docs/v2-scope.md) ------------------------------------------
    // AUDITION with SYNC following the host tempo, LOOP on, while the host transport
    // plays: the loop runs on the host's bar grid. Its left trim handle sits on the
    // host's downbeat, the phase re-derived from the host's ppq every block (a
    // loop-brace wrap, a relocate or a tempo change follows; no drift), and the
    // stretcher's latency is taken off the start by pre-rolling the voice, so the ear
    // lands on the beat with no latency reported to the host. Andrew 10-13 (late): the
    // loop is the trim exactly, no rounding; the params never move.
    // A loop that lands while the locked voice plays waits for the next bar line.
    // The host's transport is the master (Andrew, 10-11): under those conditions its
    // start plays the audition whether or not PLAY was pressed, and its stop stops it;
    // while it plays the PLAY key is a mute (pressPlay). Host stopped: PLAY is today's
    // free-running audition. Leaving the lock with the host rolling: the user's own
    // PLAY plays on, free (a muted one stays silent: free PLAY off, the mute kept); an
    // audition the transport started stops at once if the lock just can't hold (a loop
    // with no tempo, a ratio out of range), or plays on, free (silent if muted), to the
    // host's next stop if the user left it (SYNC off, a free tempo, LOOP off). A fresh instance with no loop (or a loop with no tempo) does
    // nothing on a start. A free tempo, SYNC off or LOOP off: never locked.
    // Song-grid phase (Andrew, 10-11; 10-13 late): the left handle sits on song beats
    // 0, L, 2L ... (L the trim's length in beats, any length: an odd one slides against
    // the song's bars), so any bar and beat the host is at plays the matching loop
    // position. A swap enters mid-loop at that position. barOrigin plays no part.
    // A meter change (Andrew 10-18, "the drummer version"): song beat 0 moves to the first
    // bar line in the new meter, so the loop restarts there (a re-sync, as a relocate
    // costs) and its bar 1 sits on the bar lines after it. One meter all song: never moved.
    bool isBarLocked() const { return barLocked.load(); }
    // For the tests: locked primes so far (a start, a swap, a jump back onto the grid),
    // and whether a swapped-out loop still waits for the timer to free it.
    int  getLockPrimeCount() const { return lockPrimeCount.load(); }
    // Blocks the audio thread output as silence because sampleLock was busy (an install).
    int  getSampleLockMisses() const { return sampleLockMisses.load(); }
    bool holdsRetiredLoop();

    // --- Dry hatch -------------------------------------------------------------
    // On: HATCH prints trim + sync rate only, chain bypassed (the clip then plays
    // through the live chain in FX mode). Off: today's wet render.
    void setDryHatch(bool on) { dryHatch.store(on); }
    bool isDryHatch() const   { return dryHatch.load(); }

    // --- Drum-loop curation ("(b)") ---------------------------------------
    // Scan the chosen (messy, multi-pack) folder and copy just the full drum
    // grooves into <folder>/LoopNestDrums/, then adopt that as the sample
    // folder. Label-driven precision filter, ported 1:1 from the locked spec
    // tools/extract_drum_loops.sh. Runs on a background thread (a ~5k-file scan
    // must never block the message/audio thread); the editor polls progress via
    // its timer. ADDITIVE: existing curated loops are kept, only novel ones are
    // added (dedup by output filename) — never wipe the dest.
    // Extract from `src`, writing the curated drum loops into `dest`. The editor
    // drives folder selection; this only guards the destructive case (dest == src
    // or an ancestor of src would corrupt the library).
    void extractDrumLoops(juce::File src, juce::File dest);     // launch (no-op if busy / unsafe)
    bool  isExtracting()       const { return extractRun->extracting.load(); }
    float getExtractProgress() const { return extractRun->progress.load(); }  // 0..1
    int   getExtractKept()     const { return extractRun->kept.load(); }
    juce::String getExtractStatus() const;
    // Cancel a running extraction (the ✕ in the scope header). The worker winds down
    // at its next file; a cancelled run never adopts the destination.
    void cancelExtraction();
    // Polled by the editor's timer (message thread): when the worker has finished,
    // this adopts the curated folder and clears the busy flag. Lifetime-safe — the
    // worker never touches sampleFiles itself, so there is no captured-`this` async.
    void finishExtractionIfReady();

    bool canSpin()       const { const juce::ScopedLock sl(stateLock); return ! sampleFiles.isEmpty(); }
    bool hasFolder()     const { const juce::ScopedLock sl(stateLock); return sampleFolder.isDirectory(); }
    // A folder path is remembered but no longer exists on disk (moved/renamed/
    // different machine) — the editor surfaces this so it isn't a silent empty scan.
    bool folderMissing() const { const juce::ScopedLock sl(stateLock);
                                 return sampleFolder.getFullPathName().isNotEmpty()
                                        && ! sampleFolder.isDirectory(); }
    int  getSampleCount() const { const juce::ScopedLock sl(stateLock); return sampleFiles.size(); }
    bool hasSample()     const { const juce::ScopedLock sl(stateLock); return currentSample.existsAsFile(); }
    juce::String getCurrentSampleName() const { const juce::ScopedLock sl(stateLock); return currentSampleName; }
    juce::File   getCurrentSampleFile() const { const juce::ScopedLock sl(stateLock); return currentSample; }
    // The restored set's sample that can't be found (its file name), "" = none (B4).
    juce::String getMissingSampleName() const { const juce::ScopedLock sl(stateLock);
                                                return missingSample["sample"].toString().fromLastOccurrenceOf("/", false, false); }

    // The folder's loops, in scan order (the web UI's rack lists them).
    juce::Array<juce::File> getSampleFiles() const { const juce::ScopedLock sl(stateLock); return sampleFiles; }
    // Bumped each time a scan installs the list (a folder pick, a rescan, a set's restore),
    // so the editor's poll re-sends and re-searches it only then (U-M2). Any thread.
    juce::uint32 getFilesGeneration() const { return filesGen.load(); }

    // The preset the editor's label names: -1 = INIT, else an index into kFactoryPresets
    // (UiBridge sets it and re-resolves it after a host undo). Saved with the set as
    // "preset", left out for INIT, so a set without one reads INIT; out of range = INIT.
    int  getPresetIndex() const { return presetIndex.load(); }
    void setPresetIndex(int);

    // Stage 2 meters: the peak |sample| of the host input and of the plugin output,
    // per channel, since the last call (then reset). Message thread; lock-free.
    void takeLevels(float in[2], float out[2]);

    // --- Audition (Play/Pause + Loop of the trimmed region) ---
    // Free PLAY: what plays with the host stopped (and what an internal stop, a print
    // or an editor close, asks for). Under the lock the host's transport owns the
    // audition and this changes nothing audible; the key goes through pressPlay.
    void setPlaying(bool shouldPlay) { transportPlay.store(false); playRequested.store(shouldPlay); }
    bool isPlaying() const { return playRequested.load(); }
    // The PLAY key. Under the lock (the host's transport owns the audition) it is a
    // mute: the loop keeps its place on the grid, silent, and comes back in time. The
    // mute is a state: it holds across the host's stops and starts, across SYNC off
    // or a free tempo (the free audition they hand over to stays silent, the key dark),
    // and is saved with the set. Only a press clears it: under the lock the key
    // toggles it; otherwise (free PLAY) a press clears it and plays / pauses.
    void pressPlay();
    void setMuted(bool m) { lockMuted.store(m); }
    bool isMuted() const  { return lockMuted.load(); }
    // What the key shows: audible playback.
    bool isPlayKeyLit() const { return barLocked.load() ? ! lockMuted.load() : playRequested.load(); }

    void setLooping(bool shouldLoop) { loopEnabled.store(shouldLoop); }
    bool isLooping() const { return loopEnabled.load(); }

    // Seek the audition read head to a 0..1 fraction of the whole sample (from a
    // waveform click/scrub). Consumed by the audio thread on the next block; if a
    // seek lands inside the trim region, pressing Play resumes from there.
    void seekToNorm(float norm) { seekRequestNorm.store(juce::jlimit(0.0f, 1.0f, norm)); }

    // Current playback position as a 0..1 fraction of the whole sample, for the
    // waveform scrubber. Published by the audio thread each block.
    float getPlayheadNorm() const { return playheadNorm.load(); }

    // --- Cash Out: render the shaped trimmed region to a .wav for drag-out.
    // Returns the rendered file (empty File on failure). Runs on the message thread.
    juce::File renderLoop();
    // How many discarded passes a wet print of a region this long runs before the one it
    // keeps: enough to cover the live tail (CharacterChain::tailSec, at least 1 s, D-M4),
    // so the kept pass carries all the tail a loop that short builds up.
    int printPrimePasses(double regionSec) const;

    // --- Print bins (docs/print-bins-spec.md): a bin is a SUBFOLDER of the Prints root,
    // never a stored record, so a Finder rename/mkdir/delete is the truth. Only the
    // selected bin's NAME is remembered (app settings, not apvts). A remembered bin
    // that is gone prints into the root and is NEVER recreated (a recreated ghost
    // beside a renamed bin would split prints silently). Message thread.
    juce::File getPrintsRoot() const;
    juce::StringArray getPrintBins() const;          // a shallow scan, now
    juce::String getPrintBin() const { const juce::ScopedLock sl(binLock); return printBin; }   // "" = the root
    bool isPrintBinMissing() const;                  // a bin is selected and its folder is gone
    // "" = the root; else any folder directly in the Prints root, named as Finder made it
    // ("Beats, 2026", "#1 Kicks": E-L4).
    bool setPrintBin(const juce::String& name);
    bool createPrintBin(const juce::String& name);   // makes the folder (legalBinName) and selects it
    // A new bin's folder name, and a print's bin prefix: "" = not a usable name.
    static juce::String legalBinName(const juce::String&);
    // The Prints root's own subfolder `name`; {} for a name that can't be one (a '/',
    // "." or ".."). Never getChildFile: it reads "~Old" as a home-relative path.
    juce::File getBinFolder(const juce::String& name) const;
    // The picked bin is an app setting every instance shares: re-read it from the file,
    // where another instance may have picked another (E-M2). HATCH and the editor's
    // once-a-second bin check call it.
    void reloadPrintBin();

    // Last successful render — survives editor close/reopen so PRINT can re-arm.
    // Cleared when a new sample loads (the render no longer matches what's loaded).
    juce::File getLastRender() const { const juce::ScopedLock sl(stateLock); return lastRender; }

    // True while the armed render still matches what a HATCH would print now: same
    // sample, trim and every baked param. False → the editor shows RE-HATCH and the
    // key stops being a drag source (dragging would export stale audio).
    bool isLastRenderCurrent() const;

    juce::AudioProcessorValueTreeState apvts;

    // VST3: keep the host's IComponentHandler2 (if it has one) so a bulk apply can
    // bracket its edits in startGroupEdit/finishGroupEdit, the SDK's "one undo step".
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return this; }
    void setIComponentHandler(Steinberg::FUnknown*) override;

private:
    struct HostGroupEdit;   // minimal IComponentHandler2 view (see .cpp)
    HostGroupEdit* hostGroupEdit = nullptr;   // addRef'd; message thread only
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void spinLoad(const juce::File& pick, bool playWhenLoaded);   // spin() / spinTo()

    // --- Async sample loader (v2 stage 0) ------------------------------------------
    // One loader thread decodes; the message thread installs (handleAsyncUpdate).
    // Every request bumps loadGen; a decode or install whose gen is no longer the
    // latest is dropped, so overlapping loads (a host restore racing a UI spin)
    // resolve to the newest request, never an interleave of two.
    struct LoadJob
    {
        juce::File file;
        loopnest::Snapshot apply;   // applied with the swap, per `parts` (never kFile)
        int  parts = 0;
        bool playOnInstall = false;
        bool clearOnFail = false;   // a host restore: an undecodable sample clears, not keeps
        bool mute = false;          // a host restore: silent until it lands (muteUntilGen)
        // A host restore's tempo: the saved lock, or (a state from before the lock)
        // the saved trim read as bars. Neither set: tag, else the whole file's length.
        double bpm = 0.0;
        BpmSource bpmSource = BpmSource::none;
        bool bpmFromRegion = false;
        double barOrigin = 0.0;     // a host restore's saved bar-1 anchor
        // Spin history (message thread): the entry this load is (historyPick: a spin's),
        // and for a step the one it left, so a decode that fails takes the history back.
        int  historyAt = -1, historyFrom = -1;
        bool historyPick = false;
        // A spin, pick or step: LIVE (when it came from there) and a free tempo are left
        // as it lands, never before, so the previous loop doesn't play first (E-M1).
        bool leaveFx = false, syncOn = false;
        juce::uint32 fxGen = 0;     // fxGen at the request: a later LIVE choice keeps LIVE
        juce::uint32 gen = 0;
    };
    // A host restore's load of the file: its tempo and bar 1 from the saved state's
    // properties (setStateInformation, and a missing sample's relink).
    static LoadJob restoreJob(const juce::File&, const juce::ValueTree& saved);
    struct DecodedLoad
    {
        LoadJob job;
        juce::AudioBuffer<float> buffer;
        double sampleRate = 0.0, tagBpm = 0.0;
        bool ok = false;
    };
    void requestLoad(LoadJob);
    void runLoader();                        // loader-thread body
    bool installFinishedLoad();              // message thread; true if something landed
    void handleAsyncUpdate() override { installFinishedLoad(); }
    // False if the file can't be read or fails validation (missing, corrupt header,
    // absurd length/channel count, short read). Any thread; `fm` is the caller's.
    static bool decodeFile(juce::AudioFormatManager& fm, const juce::File&, DecodedLoad&);
    // Drop the loaded sample (a restore whose sample is missing). Cancels any load.
    void clearSample();
    // Drop any queued/decoding/decoded load and mark it landed; the buffer stays.
    // Caller holds stateLock.
    void cancelLoadsLocked();
    // Snapshot -> (param id, natural value) for the chosen parts (kFile ignored).
    static void forEachPart(const loopnest::Snapshot&, int parts,
                            const std::function<void(const char*, float)>&);
    // Snapshot -> normalised edits for the chosen parts (kFile ignored).
    std::vector<ParamEdit> editsFor(const loopnest::Snapshot&, int parts) const;

    // Spin history (see historyBack). Message thread only.
    struct HistoryEntry
    {
        loopnest::Snapshot sound;
        double    bpm = 0.0;                     // its tempo (0 = automatic, as a spin lands)
        BpmSource bpmSource = BpmSource::none;
        double    barOrigin = 0.0;
    };
    std::vector<HistoryEntry> history;
    int historyPos = -1;                         // the entry that is the current sound
    std::atomic<bool> historyResetPending { false };   // a host restore landed (any thread)
    HistoryEntry captureEntry() const;
    void applyHistoryReset();
    int  noteLeaving(const juce::File& next);    // a spin / pick is about to load `next`: its entry
    bool stepHistory(int delta, bool playWhenLoaded);
    int  historyTarget(int delta) const;         // the nearest entry that way whose file exists, or -1
    void historyLoadFailed(const LoadJob&);

public:
    // Ends every gesture still open (C-M11): the editor's close, so a drag it cut off
    // doesn't leave presets, CLEAR and history skipping that param. Message thread.
    void endOpenGestures();
private:
    // UI gesture tracking (F7): open begin/end depth per parameter index, fed by
    // this processor listening to itself. Message thread.
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int index) override;
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int index) override;
    std::vector<int> gestureDepth;

    // A file with no tempo yet (BpmSource::none) takes one from its trim at rest. A trim
    // gesture's end looks at once. A start/end change with no gesture (host undo,
    // automation: any thread) only stamps trimMovedAt; the timer looks once it has been
    // quiet for kTrimSettleMs, so a sweep never guesses on the way. Nothing here runs during
    // an install or a host restore (`restoring`): those bring their own tempo.
    void parameterChanged(const juce::String&, float) override;
    void timerCallback() override;
    bool trimGestureOpen() const;
    bool refitTempoToTrim();                 // message thread
    bool fitRegionLocked();                  // caller holds stateLock
    static constexpr juce::uint32 kTrimSettleMs = 250;
    std::atomic<juce::uint32> trimMovedAt { 0 };   // 0 = at rest
    int startIndex = -1, endIndex = -1;
    loopnest::CharacterParams readCharacter() const;

    // Hash of everything a render bakes in: the sample path, the trim, and every
    // param except the monitor-only ones (glide, bypass). Caller holds stateLock (it
    // reads currentSample).
    juce::uint64 renderSignature() const;

    // Drum-loop curation: one run's state, shared with its worker, which holds its own
    // reference and never touches the processor (C-H3). The destructor cancels and waits
    // up to kExtractJoinMs; a worker stuck past that (a stalled network copy) is left to
    // finish on its own, into this. Each run gets a fresh one. Message thread, but the
    // atomics and `status` (its lock).
    struct ExtractRun
    {
        ExtractRun() { exited.signal(); }    // no worker yet
        std::atomic<bool>  extracting   { false };
        std::atomic<bool>  adoptPending { false };  // worker done; folder waits for the message thread
        std::atomic<bool>  cancel       { false };
        std::atomic<float> progress     { 0.0f };
        std::atomic<int>   kept         { 0 };
        juce::CriticalSection statusLock;
        juce::String status;
        juce::File   curated;                      // set by the worker, read by finishExtractionIfReady
        juce::WaitableEvent exited { true };       // the worker has returned
        void setStatus(const juce::String& s) { const juce::ScopedLock sl(statusLock); status = s; }
    };
    std::shared_ptr<ExtractRun> extractRun = std::make_shared<ExtractRun>();
    static constexpr int kExtractJoinMs = 30000;
    // The worker's body. `adopt` posts the adoption to the message thread.
    static void runExtraction(const std::shared_ptr<ExtractRun>&, juce::File src, juce::File dest,
                              const std::function<void()>& adopt);
    // Lifetime guard for the worker's end-of-scan callAsync: flipped false in the
    // destructor (message thread), checked on the message thread — no race.
    std::shared_ptr<bool> aliveFlag = std::make_shared<bool>(true);

    // App-level (not per-project) memory of the last folder picked, so a fresh
    // LoopNest instance auto-loads your loop stash instead of starting blank.
    // Lives in ~/Library/Application Support/LoopNest/LoopNest.settings.
    void       rememberDefaultFolder(const juce::File&);
    juce::File recallDefaultFolder();
    juce::ApplicationProperties appProperties;
    juce::PropertiesFile* settings();
    juce::CriticalSection binLock;   // a leaf lock: printBin only
    juce::String printBin;           // the selected bin's name, "" = the Prints root

public:
    // App-level settings the web editor keeps (same settings file).
    // The folders picked most recently, newest first, ones that still exist only.
    static constexpr int kMaxRecentFolders = 8;
    juce::Array<juce::File> getRecentFolders();
    // The editor window's scale (window width / 1672), 0 when never set.
    float recallEditorScale();
    void  rememberEditorScale(float);
private:

    // Shared playback-region helper: clamps the start/end params to frame indices.
    void regionFrames(int numFrames, int& startFrame, int& endFrame) const;

    // The two processBlock paths (see setFxMode) and the chain-param push
    // they share with renderLoop (one call site per chain, so they can't drift).
    void renderAudition(juce::AudioBuffer<float>&);
    void renderFx(juce::AudioBuffer<float>&);
    void pushChainParams(CharacterChain&) const;

    // Ruling 3 (fixed latency, Andrew 10-18): the audition's voice goes through the chain's
    // latency() like FX's input, and so must everything it sets on the output at the
    // voice's time (the declick, the lock's mute, the swap's 2 ms fade before the bar
    // line, MIX, OUTPUT, a locked cut-in's tail): each frame's gains go into gainLine and
    // come out latency() frames later with the audio they belong to. The output is the
    // pre-latency audition's, latency() frames late, which the host's delay compensation
    // takes back off. A voice that stops leaves latency() frames of itself in the chain:
    // the chain runs on, on silence, until they are out (latencyDrain), and a play edge
    // waits for that. Audio thread, under sampleLock.
    // processBlock's body (host bypass runs it too, see processBlockBypassed).
    void renderBlock(juce::AudioBuffer<float>&);
    // Host bypass (H2): the input through latency() frames of delay, every block either way;
    // `out` (bypDry, or the buffer itself) gets the delayed copy, nullptr just advances it.
    // Lines and scratch sized in prepareToPlay (bypDry / bypWet hold kBypassScratch frames
    // at least; a bigger block in bypass gets the delay alone, the chain paused).
    void delayHostInput(const juce::AudioBuffer<float>& in, juce::AudioBuffer<float>* out) noexcept;
    std::array<std::vector<float>, 2> bypLine;
    int  bypPos = 0;
    juce::AudioBuffer<float> bypDry, bypWet;
    bool hostBypassed = false;    // the last block was the host's bypass
    static constexpr int kBypassScratch = 8192;

    struct OutGains { float declick = 0.0f, mute = 0.0f, swap = 1.0f, mix = 0.0f, out = 0.0f; };
    std::vector<OutGains> gainLine;
    int  gainPos      = 0;
    int  latencyDrain = 0;   // frames of a stopped voice still to come out of the chain
    int  lockTailIn   = 0;   // frames until a locked cut-in reaches the output (its tail starts)
    float spliceIn    = 1.0f;   // a lock swap's new loop, faded in on the voice (renderAudition)
    void resetLatencyLine() noexcept;
    // One audition frame: `v` (the voice, after INPUT) through EQ, chain and MIX against
    // the delayed taps, the monitor, and the gains of latency() frames ago; written to
    // buffer frame `frame`. `now` are this frame's gains, for latency() frames on.
    void auditionFrame(juce::AudioBuffer<float>& buffer, int frame, float* v, const OutGains& now,
                       bool gmatch, bool bypass) noexcept;
    // The stopped voice's last latency() frames, out of the chain from buffer frame `from`.
    void drainLatency(juce::AudioBuffer<float>& buffer, int from) noexcept;

    // The SYNC audition voice. Reads the trimmed region at `step` source
    // frames per host frame (plain SR correction: no pitch, no tempo) into both
    // channels of `dest`, looping at the trim seam like the varispeed voice, so the
    // stretcher sees one continuous stream across the wrap. Non-looping, it feeds
    // silence once the region ends (voiceEnded). Audio thread, caller holds sampleLock.
    // `src` is sampleBuffer, or the loop retiring at the bar line (bar-grid lock), whose
    // locked end `endPos` falls between frames.
    void feedSyncVoice(const juce::AudioBuffer<float>& src, float* const* dest, int numFrames,
                       int startFrame, double endPos, int regionLen, int seamLen, bool looping,
                       double step) noexcept;

    // Bar-grid lock (see isBarLocked). The host's clock as processBlock last read it;
    // a block renderAudition splits moves it along (advanceHost). Audio thread only.
    struct HostClock
    {
        bool   valid = false, playing = false;   // valid: a ppq and a tempo
        double ppq = 0.0, barStart = 0.0, barBeats = 4.0, bpm = 0.0;   // beats = quarter notes
    };
    HostClock host;
    void   readHostClock(const juce::AudioPlayHead::PositionInfo&) noexcept;
    void   advanceHost(int frames) noexcept;
    // Quarter notes since the phase anchor: the song's bar 1, or the bar line a meter
    // change re-synced to (phaseAnchor, kept by readHostClock).
    double hostBeats() const noexcept;
    double phaseAnchor  = 0.0;   // audio thread: song beat 0 of the loop's phase, in host ppq
    double lastBarBeats = 0.0;   // ...and the meter the last block reported (0 = none yet)
    // Latch the bar line a queued swap waits for (swapAtBeats), once per swap.
    void   latchSwapBar() noexcept;
    // The locked loop: the trim exactly, in source frames, and frames per beat at the
    // native tempo.
    struct LockGeom
    {
        int    startFrame = 0, regionLen = 0, seamLen = 0;
        double loopLen = 0.0, endPos = 0.0, fpb = 0.0, srRatio = 1.0, bpm = 0.0;
    };
    // Off the grid by more than this (host time): prime on it at once, not steer.
    static constexpr double kLockRephaseSec = 0.05;
    // False when the loaded loop can't lock (no tempo, a ratio past the stretcher's).
    bool lockGeometry(int numSampleFrames, LockGeom&) const noexcept;
    // The source frame the ear hears at song beat `beats` (song-grid phase).
    static double lockPos(const LockGeom&, double beats) noexcept;

    // Sync tempo ratio (hostBPM / nativeBPM), clamped to
    // loopnest::kMinSyncRatio..kMaxSyncRatio, or 1 when either tempo is unknown.
    // Pure arithmetic — safe on the audio thread.
    static double syncRatio(double nativeBpm, double hostBpm) noexcept;



    // Non-parameter sample state. Guarded by stateLock: the host may call
    // get/setStateInformation off the message thread (Live's autosave does), and
    // an unguarded juce::String/Array reassignment racing the editor's timer or
    // spin() is a torn read / use-after-free. Re-entrant, so locked helpers may
    // call each other. Never taken on the audio thread (that's sampleLock's job).
    mutable juce::CriticalSection stateLock;
    juce::File           sampleFolder;
    juce::Array<juce::File> sampleFiles;
    juce::File           currentSample;
    juce::String         currentSampleName;
    juce::File           lastRender;       // see getLastRender()
    juce::uint64         lastRenderSig = 0; // renderSignature() at the time lastRender was printed
    // Bumped whenever sampleFolder changes. A rescan captures it with the folder
    // and drops its result if another selection landed while it walked the disk.
    int                  folderGen = 0;
    int                  scanTicket = 0, publishedTicket = 0;   // rescanFolder's order (stateLock)
    std::atomic<juce::uint32> filesGen { 0 };   // see getFilesGeneration(); bumped under stateLock
    std::atomic<int>     presetIndex { -1 };    // see getPresetIndex()
    std::atomic<bool>    rescanOwed { false };  // see markRescanOwed()

    // Loader state. loadQueueLock guards queuedJob / finishedLoad / pendingSample /
    // pendingApply / pendingParts. loadGen, landedGen and muteUntilGen are only
    // written under stateLock too, so a gen check and what it publishes are one
    // step against any other request, cancel or completion.
    std::unique_ptr<juce::Thread> loaderThread;
    juce::WaitableEvent loaderWake, loadDone;
    mutable juce::CriticalSection loadQueueLock;
    std::optional<LoadJob> queuedJob;
    std::unique_ptr<DecodedLoad> finishedLoad;
    // The latest request until it lands: its file, and the values it will apply per
    // pendingParts. getState saves these, so a set saved mid-load names the new
    // file with the params it's about to get. A restore without kFile clears its
    // parts out of pendingParts (the newer edit wins).
    juce::File pendingSample;
    loopnest::Snapshot pendingApply;
    double    pendingBarOrigin = 0.0;              // …and its bar-1 anchor (LoadJob::barOrigin)
    double    pendingBpm = 0.0;                    // the pending load's tempo (LoadJob::bpm)
    BpmSource pendingBpmSource = BpmSource::none;
    int pendingParts = 0;
    // A set restored with its sample missing (B4, a drive not mounted): its sample,
    // sampleRel, loopBpm, loopBpmSrc and barOrigin as saved. getState writes them back
    // while nothing is loaded or loading, and setSampleFolder looks for the file again.
    // Invalid = none. Only a load that lands, or another host restore, replaces it.
    // stateLock.
    juce::ValueTree missingSample;
    std::atomic<juce::uint32> loadGen { 0 }, landedGen { 0 };

    // Serialises whole commits: an install (gen check, buffer swap, param apply),
    // a host setState (claim gen, replaceState) and restore()/CLEAR's param apply.
    // So an older install can never interleave with a newer host restore's params.
    // getState and capture() hold it too, so params and sample are read as one
    // snapshot, never sample B with sample A's params. Taken before stateLock, never
    // while holding it. Held across host gesture callbacks on purpose
    // (stateLock/sampleLock never are). Re-entrant: a host that saves from inside a
    // gesture callback on the committing thread gets in (and the pending overlay
    // keeps that save consistent). Only another thread waits on it: the host's
    // setState or getState thread, behind a commit that never waits on that thread
    // or on a disk walk (setState rescans after releasing it).
    mutable juce::CriticalSection commitLock;

    // >0 while a restore or an install is landing params and audio: the audio
    // thread outputs silence so the new trim/FX never play over the previous
    // sample (or the new sample under the old trim).
    std::atomic<int> restoring { 0 };
    // Nonzero while a host restore's sample is still loading: the audition stays
    // muted until the gen it names lands. A newer load that supersedes it inherits
    // the mute (the restored params never play over the old sample); a cancel or
    // that load's own landing releases it.
    std::atomic<juce::uint32> muteUntilGen { 0 };

    juce::Random random;

    // Loaded audio, guarded by sampleLock. The audio thread try-locks; if it
    // can't (a load is swapping the buffer), it outputs silence for that block.
    // Every reassignment also holds stateLock, so a stateLock holder on the message
    // thread may read it without sampleLock (renderLoop's copy does).
    juce::SpinLock        sampleLock;
    juce::AudioBuffer<float> sampleBuffer;
    double                sampleFileSampleRate = 44100.0;
    double                nameBpm = 0.0;   // "(NN)bpm" filename tag, 0 = none
    double                loopBpm = 0.0;   // the stored native tempo, 0 = unknown
    BpmSource             loopBpmSource = BpmSource::none;
    std::atomic<double>   barOrigin { 0.0 };   // bar 1's place in the file, 0..1 (see setBarOrigin)

    // Audition voice. readPos is fractional for resampling; loops within the region.
    std::atomic<bool> playRequested { false };
    std::atomic<bool> loopEnabled   { true };
    std::atomic<float> playheadNorm { 0.0f }; // published for the scrubber
    std::atomic<float> seekRequestNorm { -1.0f }; // pending UI seek; -1 = none
    bool   playbackActive = false;  // audio-thread only; tracks play edge
    double readPos        = 0.0;
    double hostSampleRate = 44100.0;
    double glideRate      = 0.0;    // smoothed playback rate for PITCH glide

    // Mode / sync / dry-hatch flags (message thread writes, audio thread
    // reads) and the host tempo the audio thread publishes each block.
    std::atomic<bool>   fxMode      { false };
    std::atomic<juce::uint32> fxGen { 0 };     // bumped by every setFxMode(true) (LoadJob::fxGen)
    std::atomic<bool>   syncEnabled { true };
    std::atomic<double> freeBpm     { 0.0 };   // 0 = none
    std::atomic<bool>   dryHatch    { true };
    std::atomic<double> lastHostBpm { 0.0 };
    std::atomic<float>  inPeak[2]  { { 0.0f }, { 0.0f } };   // see takeLevels()
    std::atomic<float>  outPeak[2] { { 0.0f }, { 0.0f } };
    static void holdMax(std::atomic<float>& a, float v) noexcept
    {
        float cur = a.load(std::memory_order_relaxed);
        while (v > cur && ! a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }
    bool fxActive      = false;   // audio-thread only: the path actually rendering
    bool modeFadeIn    = false;   // audio-thread only: ramp the next block up from 0

    // SYNC audition stretcher (audio-thread only, configured for the host
    // SR in prepareToPlay; never in the FX path). The voice feeds stretchIn at the
    // plain SR-correction rate; stretchInAcc carries the fractional input frame so
    // the fed count tracks outFrames * tempo with no drift. Both scratch buffers are
    // sized in prepareToPlay (max block x kMaxSyncRatio, and the priming seek), so
    // processBlock never allocates.
    std::unique_ptr<loopnest::SyncStretcher> stretch;   // made in the constructor
    juce::AudioBuffer<float> stretchIn, stretchOut;
    int    stretchMaxBlock   = 0;      // 0 = not prepared: SYNC falls back to varispeed
    bool   stretchActive     = false;  // the sync path rendered the last block
    bool   stretchNeedsPrime = true;   // reset + outputSeek before the next block
    double stretchInAcc      = 0.0;
    // A prime is the stretcher reset onto the voice plus its pre-roll: outputSeek runs
    // about three of its spectra in one callback (1.1 ms against 0.33 at 96 kHz / 32
    // frames, B5). A callback shorter than kPrimeOneBlockMin frames at 48 kHz (scaled by
    // the rate squared: a spectrum's cost grows with the rate, the callback's time shrinks)
    // spreads it instead, one stretcher interval (one spectrum) a callback, and the voice
    // starts the callback after: two callbacks late, the downbeat whole (measured 10-18:
    // 0.67 ms at 96 kHz / 32, 1.45 ms at 44.1 kHz / 32, 2.67 ms at 96 kHz / 128, the most);
    // under the bar-grid lock the steer then pulls it onto the grid. A callback that is
    // long enough while one spreads (a host's tiny or empty first block) finishes it and
    // plays. <= 0 = never spread: the whole prime in one callback, never late (before B5).
    static constexpr int kPrimeOneBlockMin = 64;
    int    primeLeft         = 0;      // pre-roll output frames a spread prime still owes
    double primeAcc          = 0.0;    // its input count's fraction, carried
    double glideSemis        = 0.0;    // PITCH glide under SYNC: slewed transpose
    bool   voiceEnded        = false;  // one-shot (LOOP off) region ran out
    int    drainLeft         = 0;      // output frames until a one-shot's tail is out

    // Bar-grid lock state. Audio thread, except the three under sampleLock, which an
    // install reads and writes too (installFinishedLoad).
    LockGeom lockGeom;                 // the locked voice's loop; frozen while a swap waits
    bool   lockActive     = false;     // the locked path is rendering
    // A prime that cuts in on sound: the last output frame, decaying (~0.3 ms) under
    // the new start's fade-in, so the cut never steps.
    float  lockTail[2]    = { 0.0f, 0.0f };
    float  lockTailGain   = 0.0f, lockTailCoeff = 0.93f;
    float  lastOut[2]     = { 0.0f, 0.0f };   // the audition's last output frame
    bool   hostWasPlaying = false;
    bool   lockPlaying    = false;     // sampleLock: the locked voice is sounding
    bool   swapQueued     = false;     // sampleLock: a new loop waits for the bar line
    bool   swapHoldsParams = false;    // sampleLock: ...bringing its own knobs (the old loop's held)
    double heldSemis      = 0.0;       // the pitch the audition last played at
    juce::AudioBuffer<float> retiringBuffer;   // sampleLock: the loop playing out to it
    std::atomic<bool> lockMuted      { false };   // the PLAY key's mute under the lock
    std::atomic<bool> transportPlay  { false };   // the host's start (not PLAY) started this audition
    float  muteGain       = 1.0f;      // its declick ramp (1 whenever not locked)
    std::atomic<bool> barLocked      { false };   // published: isBarLocked()
    std::atomic<bool> retiredWaiting { false };   // a swap landed: the timer frees retiringBuffer
    bool   swapLatched    = false;     // the queued swap's bar line is latched in swapAtBeats
    double swapAtBeats    = 0.0;
    double lockInAcc      = 0.0;       // the locked feed's fractional input frame, carried
    double lockTempoLast  = 1.0;       // the last locked tempo ratio (a swap's fade-out runs at it)
    std::atomic<int>    lockPrimeCount   { 0 };
    std::atomic<int>    sampleLockMisses { 0 };

    // Cached atomic parameter pointers (read on the audio thread).
    std::atomic<float>* pitchParam       = nullptr;
    std::atomic<float>* pitchGlideParam  = nullptr; // PITCH 2nd axis: rate glide
    // The chain params, in CharacterParams::fields() order (readCharacter()).
    std::array<std::atomic<float>*, loopnest::CharacterParams::kNum> characterRefs {};
    // Panel power, CharacterParams::powerIds() order (warpOn .. widthOn).
    std::array<std::atomic<float>*, loopnest::CharacterParams::kNumStages> powerRefs {};
    std::atomic<float>* startParam       = nullptr; // playback region, 0..1 of length
    std::atomic<float>* endParam         = nullptr;
    std::atomic<float>* inputParam       = nullptr; // input level in dB (pre-chain)
    std::atomic<float>* outputParam      = nullptr; // output level in dB
    std::atomic<float>* mixParam         = nullptr; // master dry/wet (character chain), 0..100 %
    std::atomic<float>* bypassParam      = nullptr; // A/B monitor: hear the dry source (never printed)
    std::atomic<float>* gainMatchParam   = nullptr; // loudness-match processed → dry (bakes into a wet print)

    // GAIN MATCH + BYPASS for the live path, AUDITION and FX alike (MonitorStage.h);
    // renderLoop uses a local one for a wet print.
    loopnest::MonitorStage monitor;

    // Audition declick state: entry fade-in, deferred-seek + stop fade-out (~2 ms)
    // — hard per-sample swaps/jumps click.
    float declickGain = 1.0f, declickCoeff = 0.05f;
    // A locked start fades in faster (~0.5 ms), so the downbeat it lands on keeps its
    // attack. Only while the lock renders; the free audition always uses declickCoeff.
    float lockInCoeff = 0.2f;
    bool  lockFastIn  = false;
    float pendingSeekNorm = -1.0f;   // mid-play seek waits for the fade-out to land
    bool  stopping = false;          // pause fades out, THEN playbackActive drops

    // Lo-fi character chain for the live audition; renderLoop uses a local one.
    CharacterChain character;

    // Audition declick: INPUT/OUTPUT/MIX are applied per-sample, so a knob drag or
    // preset slam stepping them per block zipper-clicks. ~20 ms linear ramps.
    // (The chain's own amounts slew inside CharacterChain.)
    juce::SmoothedValue<float> smInGain, smOutGain, smMix;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopNestProcessor)
};
