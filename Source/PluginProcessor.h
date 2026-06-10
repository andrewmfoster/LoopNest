#pragma once
#include <JuceHeader.h>
#include "CharacterChain.h"

// LoopNest: spin for a random loop from your curated folder, shape it, cash it
// out as a .wav you drag onto an Ableton track. Not a MIDI instrument — it
// ignores MIDI and auditions the trimmed region on a loop via a UI Play button.
class LoopNestProcessor : public juce::AudioProcessor
{
public:
    LoopNestProcessor();
    ~LoopNestProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    // Declares a MIDI input bus so Ableton will open it as an instrument, but
    // playback is never triggered by MIDI — processBlock ignores the buffer.
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

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

    void spin();    // pick a new random loop from the folder (unlimited)

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
    bool  isExtracting()       const { return extracting.load(); }
    float getExtractProgress() const { return extractProgress.load(); }  // 0..1
    int   getExtractKept()     const { return extractKept.load(); }
    juce::String getExtractStatus() const;
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

    // --- Audition (Play/Pause + Loop of the trimmed region) ---
    void setPlaying(bool shouldPlay) { playRequested.store(shouldPlay); }
    bool isPlaying() const { return playRequested.load(); }

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

    // Last successful render — survives editor close/reopen so PRINT can re-arm.
    // Cleared when a new sample loads (the render no longer matches what's loaded).
    juce::File getLastRender() const { const juce::ScopedLock sl(stateLock); return lastRender; }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void rescanFolder();
    // False if the file can't be read or fails validation (missing, corrupt header,
    // absurd length/channel count) — the previous sample stays loaded.
    bool loadSample(const juce::File& file);

    // Drum-loop curation worker + its label filters (see extractDrumLoops()).
    void runExtraction(juce::File src, juce::File dest);  // worker-thread body
    void setExtractStatus(const juce::String&);
    std::unique_ptr<juce::Thread> extractThread;
    // Lifetime guard for the worker's end-of-scan callAsync: flipped false in the
    // destructor (message thread), checked on the message thread — no race.
    std::shared_ptr<bool> aliveFlag = std::make_shared<bool>(true);
    std::atomic<bool>  extracting      { false };
    std::atomic<bool>  adoptPending    { false };  // worker done; folder waits for the message thread
    std::atomic<float> extractProgress { 0.0f };
    std::atomic<int>   extractKept     { 0 };
    juce::CriticalSection extractStatusLock;
    juce::String          extractStatusText;
    juce::File            curatedFolder;           // set by worker, read by finishExtractionIfReady

    // App-level (not per-project) memory of the last folder picked, so a fresh
    // LoopNest instance auto-loads your loop stash instead of starting blank.
    // Lives in ~/Library/Application Support/LoopNest/LoopNest.settings.
    void       rememberDefaultFolder(const juce::File&);
    juce::File recallDefaultFolder();
    juce::ApplicationProperties appProperties;

    // Shared playback-region helper: clamps the start/end params to frame indices.
    void regionFrames(int numFrames, int& startFrame, int& endFrame) const;

    juce::AudioFormatManager formatManager;

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

    juce::Random random;

    // Loaded audio, guarded by sampleLock. The audio thread try-locks; if it
    // can't (a load is swapping the buffer), it outputs silence for that block.
    juce::SpinLock        sampleLock;
    juce::AudioBuffer<float> sampleBuffer;
    double                sampleFileSampleRate = 44100.0;

    // Audition voice. readPos is fractional for resampling; loops within the region.
    std::atomic<bool> playRequested { false };
    std::atomic<bool> loopEnabled   { true };
    std::atomic<float> playheadNorm { 0.0f }; // published for the scrubber
    std::atomic<float> seekRequestNorm { -1.0f }; // pending UI seek; -1 = none
    bool   playbackActive = false;  // audio-thread only; tracks play edge
    double readPos        = 0.0;
    double hostSampleRate = 44100.0;
    double glideRate      = 0.0;    // smoothed playback rate for PITCH glide

    // Cached atomic parameter pointers (read on the audio thread).
    std::atomic<float>* pitchParam       = nullptr;
    std::atomic<float>* pitchGlideParam  = nullptr; // PITCH 2nd axis: rate glide
    std::atomic<float>* warpParam        = nullptr; // WARP depth (slot 0)
    std::atomic<float>* warpRateParam    = nullptr; // WARP rate (lane strip)
    std::atomic<float>* startParam       = nullptr; // playback region, 0..1 of length
    std::atomic<float>* endParam         = nullptr;
    std::atomic<float>* inputParam       = nullptr; // input level in dB (pre-chain)
    std::atomic<float>* outputParam      = nullptr; // output level in dB
    std::atomic<float>* driveParam       = nullptr; // Stage 2 character knobs
    std::atomic<float>* driveToneParam   = nullptr; // DRIVE 2nd axis: ±tilt
    std::atomic<float>* flutterParam     = nullptr;
    std::atomic<float>* flutterRateParam = nullptr; // FLUTTER 2nd axis: wobble speed
    std::atomic<float>* echoParam        = nullptr;
    std::atomic<float>* reverbParam      = nullptr;
    std::atomic<float>* echoLoParam      = nullptr; // echo wet band: low-cut handle (0..1)
    std::atomic<float>* echoHiParam      = nullptr; // echo wet band: high-cut handle (0..1)
    std::atomic<float>* reverbLoParam    = nullptr; // reverb wet band: low-cut handle (0..1)
    std::atomic<float>* reverbHiParam    = nullptr; // reverb wet band: high-cut handle (0..1)
    std::atomic<float>* eqLoParam        = nullptr; // master EQ (pre-rack): low-cut handle (0..1)
    std::atomic<float>* eqHiParam        = nullptr; // master EQ (pre-rack): high-cut handle (0..1)
    std::atomic<float>* widthParam       = nullptr; // WIDTH (idx1): band-limited stereo width, 0..100 %
    std::atomic<float>* widthLoParam     = nullptr; // WIDTH band: low-cut handle (0..1)
    std::atomic<float>* widthHiParam     = nullptr; // WIDTH band: high-cut handle (0..1)
    std::atomic<float>* reverbDecayParam = nullptr; // REVERB DECAY (tertiary): room/tail, 0..100 %
    std::atomic<float>* echoTimeParam    = nullptr; // ECHO TIME (tertiary): 30..350 ms, 0..100 %
    std::atomic<float>* widthMidParam    = nullptr; // WIDTH CENTER (tertiary): mid ±dB, −100..+100 %
    std::atomic<float>* mixParam         = nullptr; // master dry/wet (character chain), 0..100 %
    std::atomic<float>* bypassParam      = nullptr; // A/B monitor: hear dry source (audition only)
    std::atomic<float>* gainMatchParam   = nullptr; // A/B monitor: loudness-match processed → dry

    // Gain-match running state (audition only). Mean-square trackers for the dry
    // reference + processed output, and the smoothed makeup gain applied when on.
    // The taps are K-weighted (ITU-R BS.1770: high shelf +4 dB @ ~1.68 kHz, then
    // high-pass @ ~38 Hz) before tracking — flat RMS under-counts the 1–5 kHz
    // harmonics drive adds (where the ear is most sensitive), so an RMS match
    // leaves saturated sound audibly louder than the dry reference.
    struct Biquad { float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f; };
    struct BiquadState
    {
        float z1 = 0.0f, z2 = 0.0f;
        float process(const Biquad& c, float x) noexcept
        {
            const float y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            return y;
        }
        void reset() noexcept { z1 = z2 = 0.0f; }
    };
    Biquad kwShelf, kwHipass;                  // coeffs, set per host SR in prepareToPlay
    BiquadState kwRef[2], kwProc[2];           // [shelf, high-pass] per tap
    float  matchGain = 1.0f;
    double refMS = 0.0, procMS = 0.0;

    // Audition declick state: entry fade-in, deferred-seek + stop fade-out (~2 ms),
    // and a ~5 ms A/B bypass crossfade — hard per-sample swaps/jumps click.
    float declickGain = 1.0f, declickCoeff = 0.05f;
    float bypBlend = 0.0f,    bypCoeff = 0.01f;
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
