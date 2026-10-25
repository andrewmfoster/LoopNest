// Before JUCE: see the SyncStretcher note in PluginProcessor.h.
#include "TimeStretch.h"
#include "PluginProcessor.h"
#include "WebEditor.h"
#include "BankDrop.h"
#include "FactoryPresets.h"
#include <regex>

namespace
{
    // A tiny juce::Thread that just runs a lambda — lets the processor own its
    // workers (drum extract, sample loader) without bespoke subclasses.
    class FnThread : public juce::Thread
    {
    public:
        FnThread(const juce::String& name, std::function<void()> f)
            : juce::Thread(name), fn(std::move(f)) {}
        void run() override { if (fn) fn(); }
    private:
        std::function<void()> fn;
    };

    // Settings folder and Prints folder follow the product name: "LoopNest", shared
    // with v1 on purpose (its only setting, defaultFolder, means the same here), and the
    // test target ("LoopNest Tests") never touches them.
    juce::String appDirName() { return JucePlugin_Name; }
}

juce::AudioProcessorValueTreeState::ParameterLayout
LoopNestProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "pitch", 1 }, "Pitch",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.01f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("st")));

    // PITCH secondary axis — GLIDE: the playback rate slews toward the pitch knob's
    // target instead of jumping, for a tape-spin-up feel when you turn PITCH while
    // it loops. 0 % = instant (true bypass); higher = slower glide. Audition-only —
    // the render bakes the final pitch with no glide.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "pitchGlide", 1 }, "Glide",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // WARP — slow, deep pitch drift (a warped record), in slot 0. Two axes: the
    // knob `warp` is DEPTH (0..100 %, 0 = steady) and the `warpRate` lane strip is
    // RATE (0..100 %, mapped to ~0.15–1.6 Hz internally). Distinct from FLUTTER,
    // which is the fine/fast tape shimmer; WARP is the deep/slow record sway.
    // The DSP lives in CharacterChain (signal order WARP → FLUTTER → …). WARP and
    // FLUTTER step in 0.5 %: the knob's lower half now spans what the whole old knob
    // did (10-17, stateVersion 4), so this keeps the old resolution there, and a
    // converted old value (setStateInformation) lands within a quarter step.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "warp", 1 }, "Warp",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.5f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "warpRate", 1 }, "Warp Rate",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 40.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // Playback region as a fraction of the sample length.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "start", 1 }, "Start",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "end", 1 }, "End",
        juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));

    // Input level — gain-stages the source INTO the effects rack (applied pre-chain,
    // before the dry/wet split). Same range as Output. Sits above OUTPUT in the SPIN
    // column. Baked into PRINT.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "input", 1 }, "Input",
        juce::NormalisableRange<float>(-60.0f, 6.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    // Output level — a playback-level knob tucked in the cassette column corner
    // (not one of the six character knobs). Applied in audition + render. The big
    // dial is RE-SPIN (a trigger, not a parameter).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float>(-60.0f, 6.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    // Stage 2 character knobs — params exist (so the UI attaches + state persists)
    // but their DSP is intentionally NOT implemented yet (tuned by ear later).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "drive", 1 }, "Drive",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    // DRIVE secondary axis — TONE: ±tilt on the grit (− dark, + bright). 0 = flat.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "driveTone", 1 }, "Drive Tone",
        juce::NormalisableRange<float>(-100.0f, 100.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "flutter", 1 }, "Flutter",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.5f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    // FLUTTER secondary axis — RATE: wobble speed, 50 % = 1× (0 % = ½×, 100 % = 2×).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "flutterRate", 1 }, "Flutter Rate",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "echo", 1 }, "Echo",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "reverb", 1 }, "Reverb",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // Two-handle band-pass on the echo / reverb WET path only (dry stays clean),
    // RC20 "FOCUS" style. Each effect has a low-cut (lo) and high-cut (hi) handle,
    // 0..1 across a ~20 Hz..20 kHz log axis. lo=0 / hi=1 = fully open (true bypass).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "echoLo", 1 }, "Echo Low Cut",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "echoHi", 1 }, "Echo High Cut",
        juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "reverbLo", 1 }, "Reverb Low Cut",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "reverbHi", 1 }, "Reverb High Cut",
        juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));

    // Master EQ — a two-handle band-pass on the WHOLE signal, pre-rack (between
    // INPUT and the FLUTTER→…→REVERB rack). Same 0..1 log-axis convention as the
    // wet bands; eqLo=0 / eqHi=1 = fully open (true bypass). Bakes into PRINT.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "eqLo", 1 }, "EQ Low Cut",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "eqHi", 1 }, "EQ High Cut",
        juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));

    // WIDTH (character knob idx1, replaces the relocated PITCH) — band-limited stereo
    // width. The two band handles (widthLo/widthHi, 0..1 log axis, same convention as
    // the wet bands) select the frequencies that stay stereo / get widened; outside the
    // band the signal folds toward mono as WIDTH rises. width=0 % = true bypass (the
    // stage is skipped, original stereo passes untouched). Bakes into PRINT (stereo only).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "width", 1 }, "Width",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "widthLo", 1 }, "Width Low Cut",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "widthHi", 1 }, "Width High Cut",
        juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));

    // Tertiary axes for the three band cells (ECHO / REVERB / WIDTH):
    //   reverbDecay — tail length (T30, per REVERB TYPE), independent of the wet-level knob.
    //   echoTime    — free slapback→short-dub time (% maps 30..350 ms; no tempo sync,
    //                 baked as character — Ableton handles rhythmic delay after the drop).
    //   widthMid    — CENTER: bipolar ±dB gain on the M/S MID (0 % = unity), works even
    //                 with WIDTH at 0. All additive (old projects load at these neutrals).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "reverbDecay", 1 }, "Reverb Decay",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 31.0f,   // 31 = the 1.4 s tail 50 gave before DECAY went geometric (10-08)
        juce::AudioParameterFloatAttributes().withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "echoTime", 1 }, "Echo Time",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 44.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "widthMid", 1 }, "Width Center",
        juce::NormalisableRange<float>(-100.0f, 100.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    // REVERB TYPE — which tank (CharacterChain::ReverbType order): PLATE (Dattorro),
    // HALL (FDN, default) or SPRING. A change while a tail rings crossfades (~30 ms).
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "reverbType", 1 }, "Reverb Type",
        juce::StringArray { "Plate", "Hall", "Spring" }, 1));

    // Master DRY/WET — blends the unprocessed (pitched/trimmed) signal against the
    // full character chain, applied in both audition and the PRINT render. 100 % =
    // fully wet (the chain as heard before this knob existed).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // The A/B pair, live in AUDITION and FX alike (MonitorStage.h).
    // bypass: hear the dry source (pitched/trimmed, uncoloured) to A/B against the FX.
    //   A monitor: NEVER baked into PRINT/renderLoop.
    // gainMatch: scale the processed output to the dry loudness. Part of the sound:
    //   bakes into a wet print.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "bypass", 1 }, "Monitor Bypass", false));   // not the host's own bypass (Andrew, 10-18)
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "gainMatch", 1 }, "Gain Match", false));

    // Panel power: one switch per rack stage, default ON. Off = that whole stage is
    // neutral (CharacterChain::setParams), its knobs keep their values; tails cut.
    // Part of the sound: bakes into PRINT, history, presets (all ON) and CLEAR.
    {
        static const char* const names[] = { "Warp On", "Flutter On", "Drive On",
                                             "Echo On", "Reverb On", "Width On" };
        for (int i = 0; i < loopnest::CharacterParams::kNumStages; ++i)
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                juce::ParameterID { loopnest::CharacterParams::powerIds()[(size_t) i], 1 }, names[i], true));
    }

    return { params.begin(), params.end() };
}

LoopNestProcessor::LoopNestProcessor()
    : AudioProcessor(BusesProperties()
        .withInput ("Input",  juce::AudioChannelSet::stereo(), true)    // FX mode's track input
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "LoopNestState", createParameterLayout()),
      stretch(std::make_unique<loopnest::SyncStretcher>())
{
    pitchParam      = apvts.getRawParameterValue("pitch");
    pitchGlideParam = apvts.getRawParameterValue("pitchGlide");
    startParam      = apvts.getRawParameterValue("start");
    endParam        = apvts.getRawParameterValue("end");
    inputParam      = apvts.getRawParameterValue("input");
    outputParam     = apvts.getRawParameterValue("output");
    for (int i = 0; i < loopnest::CharacterParams::kNum; ++i)
        characterRefs[(size_t) i] = apvts.getRawParameterValue(loopnest::CharacterParams::fields()[(size_t) i].id);
    for (int i = 0; i < loopnest::CharacterParams::kNumStages; ++i)
        powerRefs[(size_t) i] = apvts.getRawParameterValue(loopnest::CharacterParams::powerIds()[(size_t) i]);
    mixParam        = apvts.getRawParameterValue("mix");
    bypassParam     = apvts.getRawParameterValue("bypass");
    gainMatchParam  = apvts.getRawParameterValue("gainMatch");

    gestureDepth.assign((size_t) getParameters().size(), 0);
    addListener(this);
    startIndex = apvts.getParameter("start")->getParameterIndex();
    endIndex   = apvts.getParameter("end")->getParameterIndex();
    apvts.addParameterListener("start", this);
    apvts.addParameterListener("end", this);
    startTimer(100);

    loaderThread = std::make_unique<FnThread>("LoopNest sample loader", [this] { runLoader(); });
    loaderThread->startThread();

    // Fresh-instance default: auto-load the last folder we used anywhere. A
    // restored project calls setStateInformation afterwards and overrides this.
    reloadPrintBin();
    if (auto def = recallDefaultFolder(); def.isDirectory())
    {
        {
            const juce::ScopedLock sl(stateLock);
            sampleFolder = def;
            ++folderGen;
        }
        rescanFolder();
    }
}

// The VST3 SDK's IComponentHandler2, declared locally so the shared code needn't
// pull in the SDK headers (their IIDs are defined in the VST3 wrapper's unit).
// Same vtable order as pluginterfaces/vst/ivsteditcontroller.h: FUnknown's three,
// then setDirty, requestOpenEditor, startGroupEdit, finishGroupEdit. PLUGIN_API is
// empty on macOS.
struct LoopNestProcessor::HostGroupEdit
{
    virtual int32_t  queryInterface(const char iid[16], void** obj) = 0;
    virtual uint32_t addRef() = 0;
    virtual uint32_t release() = 0;
    virtual int32_t  setDirty(uint8_t state) = 0;
    virtual int32_t  requestOpenEditor(const char* name) = 0;
    virtual int32_t  startGroupEdit() = 0;
    virtual int32_t  finishGroupEdit() = 0;
};

LoopNestProcessor::~LoopNestProcessor()
{
    *aliveFlag = false;   // strand any in-flight callAsync (we're going away)
    if (hostGroupEdit != nullptr) hostGroupEdit->release();

    stopTimer();
    apvts.removeParameterListener("start", this);
    apvts.removeParameterListener("end", this);
    removeListener(this);

    // Join, never kill: a decode checks nothing mid-read, and stopThread's timeout
    // would pthread_cancel without a join, leaving a thread that can still touch
    // our members. A loop file (capped at 5 min of audio) decodes in well under a
    // second from any sane disk; a stalled network volume holds the unload rather
    // than risking a use-after-free.
    loaderThread->signalThreadShouldExit();
    loaderWake.signal();
    loaderThread->waitForThreadToExit(-1);
    cancelPendingUpdate();          // no install after this (JUCE destroys plugins on the message thread)

    // The drum-extract worker: cancelled, it winds down at its next file. Waited for,
    // up to kExtractJoinMs, never killed (stopThread's timeout would pthread_cancel it,
    // no join, C-H3). One still stuck on a stalled copy is left to finish on its own: it
    // holds its run's state and never touches this processor (aliveFlag, above).
    extractRun->cancel.store(true);
    extractRun->exited.wait(kExtractJoinMs);
}

bool LoopNestProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // An effect: stereo out, fed by a stereo (or mono) input. The audition
    // ignores the input; FX mode duplicates a mono input into both channels.
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo()
        && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

void LoopNestProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // An install (message thread) resets `character` and the playback state under
    // sampleLock, and a host may prepare during a load (a rate change, an offline
    // switch), so all of this does too (B8). A leaf lock here, as everywhere: nothing
    // below takes another, and the audio thread only try-locks it.
    {
        const juce::SpinLock::ScopedLockType lock(sampleLock);
        hostSampleRate = sampleRate;
        playbackActive = false;
        readPos = 0.0;
        fxActive   = fxMode.load();   // a fresh prepare starts on the current mode, no fade
        modeFadeIn = false;

        character.prepare(sampleRate);
        gainLine.assign((size_t) character.latency(), OutGains {});
        resetLatencyLine();
        for (auto& l : bypLine) l.assign((size_t) character.latency(), 0.0f);
        bypPos = 0;
        hostBypassed = false;
        bypDry.setSize(2, juce::jmax(samplesPerBlock, kBypassScratch));
        bypWet.setSize(2, juce::jmax(samplesPerBlock, kBypassScratch));

        // SYNC stretcher — default preset (120 ms window), stereo, host SR.
        // All of its allocation happens here (configure), plus the voice/output
        // scratch: the voice feeds up to maxBlock x kMaxSyncRatio frames a block, or
        // outputSeekLength(kMaxSyncRatio) frames when priming, whichever is larger.
        stretch->st.presetDefault(2, (float) sampleRate);
        stretchMaxBlock = juce::jmax(1, samplesPerBlock);
        const int inCap = juce::jmax((int) std::ceil((double) stretchMaxBlock * loopnest::kMaxSyncRatio),
                                     stretch->st.outputSeekLength((float) loopnest::kMaxSyncRatio)) + 2;
        stretchIn.setSize(2, inCap);
        stretchOut.setSize(2, stretchMaxBlock);
        stretchIn.clear();
        stretchOut.clear();
        // outputSeek sizes two scratch vectors of the stretcher's on first use. One seek
        // on silence here, at the longest seek the audio thread can ask for, so that first
        // use is not on the audio thread (C9). The real prime resets it.
        {
            float* in[2] = { stretchIn.getWritePointer(0), stretchIn.getWritePointer(1) };
            stretch->st.outputSeek(in, juce::jmin(inCap, stretch->st.outputSeekLength((float) loopnest::kMaxSyncRatio)));
            stretch->st.reset();
        }
        stretchActive     = false;
        stretchNeedsPrime = true;
        stretchInAcc      = 0.0;
        primeLeft         = 0;
        phaseAnchor       = 0.0;
        lastBarBeats      = 0.0;
        voiceEnded        = false;
        drainLeft         = 0;
        lockActive        = false;
        lockTailGain      = 0.0f;
        hostWasPlaying    = false;

        declickCoeff = (float) (1.0 - std::exp(-1.0 / (0.002 * sampleRate)));
        lockInCoeff  = (float) (1.0 - std::exp(-1.0 / (0.0005 * sampleRate)));
        lockTailCoeff = (float) std::exp(-1.0 / (0.0003 * sampleRate));
        lockFastIn   = false;
        declickGain  = 1.0f;
        pendingSeekNorm = -1.0f;
        stopping = false;

        monitor.prepare(sampleRate);

        smInGain.reset(sampleRate, 0.02);
        smOutGain.reset(sampleRate, 0.02);
        smMix.reset(sampleRate, 0.02);
        smInGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(inputParam->load(),  -60.0f));
        smOutGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(outputParam->load(), -60.0f));
        smMix.setCurrentAndTargetValue(juce::jlimit(0.0f, 1.0f, mixParam->load() * 0.01f));
    }

    // Ruling 3: the chain's fixed delay, in frames at this rate, every mode and setting
    // (CharacterChain.h). Told to the host outside sampleLock: it may call straight back.
    setLatencySamples(character.latency());
}

void LoopNestProcessor::resetLatencyLine() noexcept
{
    std::fill(gainLine.begin(), gainLine.end(), OutGains {});
    gainPos      = 0;
    latencyDrain = 0;
    lockTailIn   = 0;
    spliceIn     = 1.0f;
}

void LoopNestProcessor::releaseResources() {}

void LoopNestProcessor::regionFrames(int numFrames, int& startFrame, int& endFrame) const
{
    // Handles sit on the (N-1) scale (hits, the playhead and the page share it); the end
    // is exclusive, so a fully open end is frame N and an untrimmed loop is the whole file.
    const int lastFrame = numFrames - 1;
    const float endNorm = endParam->load();
    startFrame = juce::jlimit(0, juce::jmax(0, lastFrame),
                              (int) (startParam->load() * lastFrame));
    endFrame   = endNorm >= 1.0f ? juce::jmax(startFrame + 1, numFrames)
                                 : juce::jlimit(startFrame + 1, juce::jmax(startFrame + 1, lastFrame),
                                                (int) (endNorm * lastFrame));
}

// PITCH as the DSP plays it: snapped to the parameter's own 0.01 st grid, so "0 st" is
// exactly rate 1. A set through the normalised value lands 0 at about -2.7e-7 st, which
// stretched every varispeed cycle and print by a frame past the right handle (LCV-1).
static double effectiveSemis(float rawPitch) noexcept
{
    return std::round(static_cast<double>(rawPitch) * 100.0) / 100.0;
}

// Seam declick (audition + render share it, so the print loops exactly like the
// audition). Linear-interpolated read at a fractional source position, plus a
// crossfade (loopnest::seamGains) over the first `seamLen` frames of the region: the head
// fades in while the audio just PAST the end point fades out. At the wrap the
// output continues from x[end] (the next sample after the tail) and lands on
// x[start + seamLen], so there is no step at the seam, and the region length and
// trim points are unchanged. Past the file end the continuation holds the last
// sample (still continuous). Pure reads, no allocation: audio-thread safe.
static float readInterp(const juce::AudioBuffer<float>& buf, int ch, double pos) noexcept
{
    const int n  = buf.getNumSamples();
    const int i0 = juce::jlimit(0, n - 1, (int) pos);
    const int i1 = (i0 + 1 < n) ? i0 + 1 : i0;
    const float frac = (float) juce::jlimit(0.0, 1.0, pos - (double) i0);
    const float a = buf.getSample(ch, i0), b = buf.getSample(ch, i1);
    return a + frac * (b - a);
}

static float readSeamed(const juce::AudioBuffer<float>& buf, int ch, double pos,
                        int startFrame, int regionLen, int seamLen) noexcept
{
    const float head = readInterp(buf, ch, pos);
    const double into = pos - (double) startFrame;
    if (seamLen <= 0 || into < 0.0 || into >= (double) seamLen)
        return head;
    // The head over the audio past the region's end, for their own correlation over ~5
    // seams (the region and the file allowing): loopnest::seamGains (L2, 10-18 review).
    const int endFrame = startFrame + regionLen;
    const int win = juce::jmin(5 * seamLen, regionLen / 4, buf.getNumSamples() - endFrame);
    const double rho = win > 0 ? loopnest::seamCorrelation([&] (int k) { return (double) buf.getSample(ch, startFrame + k); },
                                                           [&] (int k) { return (double) buf.getSample(ch, endFrame + k); }, win)
                               : 0.0;
    float a, b;
    loopnest::seamGains(into / (double) seamLen, rho, a, b);
    const float tail = readInterp(buf, ch, pos + (double) regionLen);
    return head * a + tail * b;
}

// ~2 ms crossfade, capped at a quarter of the region so short loops keep their body.
static int seamFrames(double srcSR, int regionLen) noexcept
{
    const int f = juce::jmin(juce::roundToInt(0.002 * srcSR), regionLen / 4);
    return f >= 2 ? f : 0;
}

// Native tempo of a stretch of audio. The filename tag wins; otherwise read the
// length as 1, 2, 4 or 8 bars of 4/4 and keep the reading inside 60..200 BPM closest
// to the host tempo (120 when the host gives none). "Closest" is by ratio — the
// smallest speed change — so a 4 s loop at host 90 reads as 120 (x0.75), not as
// 60 (x1.5), where plain BPM distance would tie. 0 = nothing fits. Run when a file
// lands, when the trim comes to rest and on request, never per block: the result
// is stored in loopBpm, which the audio thread, the render and the UI readout all read.
static double inferNativeBpm(double nameBpm, double regionSec, double hostBpm,
                             LoopNestProcessor::BpmSource& source) noexcept
{
    source = LoopNestProcessor::BpmSource::none;
    if (nameBpm > 0.0)
    {
        source = LoopNestProcessor::BpmSource::name;
        return nameBpm;
    }
    if (regionSec <= 0.0)
        return 0.0;

    const double target = hostBpm > 0.0 ? hostBpm : 120.0;
    double best = 0.0;
    for (int bars : { 1, 2, 4, 8 })
    {
        const double bpm = 240.0 * (double) bars / regionSec;   // 4 beats a bar
        if (bpm < 60.0 || bpm > 200.0)
            continue;
        if (best <= 0.0 || std::abs(std::log(bpm / target)) < std::abs(std::log(best / target)))
            best = bpm;
    }
    if (best > 0.0)
        source = LoopNestProcessor::BpmSource::length;
    return best;
}

void LoopNestProcessor::setFreeBpm(double bpm)
{
    if (bpm > 0.0)
    {
        freeBpm.store(juce::jlimit(kMinLoopBpm, kMaxLoopBpm, bpm));
        syncEnabled.store(false);
    }
    else
        freeBpm.store(0.0);
}

double LoopNestProcessor::syncRatio(double nativeBpm, double hostBpm) noexcept
{
    // No host tempo, or no native one: fall back to the loop's own tempo — no sync.
    return nativeBpm > 0.0 && hostBpm > 0.0
             ? juce::jlimit(loopnest::kMinSyncRatio, loopnest::kMaxSyncRatio, hostBpm / nativeBpm)
             : 1.0;
}

double LoopNestProcessor::getNativeBpm(BpmSource& source) const
{
    // stateLock covers loopBpm: installFinishedLoad, the setters and the
    // restore-clear only change it while holding it.
    const juce::ScopedLock sl(stateLock);
    source = sampleBuffer.getNumSamples() > 0 && loopBpm > 0.0 ? loopBpmSource : BpmSource::none;
    return source == BpmSource::none ? 0.0 : loopBpm;
}

void LoopNestProcessor::setLoopBpm(double bpm)
{
    const juce::ScopedLock sl(stateLock);
    const int numFrames = sampleBuffer.getNumSamples();
    if (numFrames <= 0 || sampleFileSampleRate <= 0.0)
        return;
    if (bpm <= 0.0)
    {
        // Automatic, as when the file landed: the tag, else the whole file as bars.
        // A file nothing fits keeps the tempo there was.
        BpmSource src = BpmSource::none;
        const double autoBpm = inferNativeBpm(nameBpm, (double) numFrames / sampleFileSampleRate,
                                              lastHostBpm.load(), src);
        const juce::SpinLock::ScopedLockType lock(sampleLock);
        if (autoBpm > 0.0)
        {
            loopBpm = autoBpm;
            loopBpmSource = src;
        }
        else
            loopBpmSource = loopBpm > 0.0 ? BpmSource::length : BpmSource::none;
        return;
    }
    const juce::SpinLock::ScopedLockType lock(sampleLock);
    loopBpm = juce::jlimit(kMinLoopBpm, kMaxLoopBpm, bpm);
    loopBpmSource = BpmSource::user;
}

bool LoopNestProcessor::fitRegionLocked()
{
    const int numFrames = sampleBuffer.getNumSamples();
    if (numFrames <= 0 || sampleFileSampleRate <= 0.0)
        return false;
    int startFrame, endFrame;
    regionFrames(numFrames, startFrame, endFrame);
    BpmSource src = BpmSource::none;
    const double bpm = inferNativeBpm(0.0, (double) (endFrame - startFrame) / sampleFileSampleRate,
                                      lastHostBpm.load(), src);
    if (bpm <= 0.0)
        return false;
    // Unchanged (a spin's whole-file reading, read again with the 0..1 trim it lands
    // with): no sampleLock take. loopBpm's writers all hold stateLock, as the caller does.
    if (loopBpmSource == BpmSource::length && std::abs(loopBpm - bpm) < 1.0e-9)
        return true;
    const juce::SpinLock::ScopedLockType lock(sampleLock);
    loopBpm = bpm;
    loopBpmSource = BpmSource::length;
    return true;
}

bool LoopNestProcessor::setLoopBpmFromRegion()
{
    const juce::ScopedLock sl(stateLock);
    if (! fitRegionLocked())
        return false;
    barOrigin.store(juce::jlimit(0.0, 1.0, (double) apvts.getRawParameterValue("start")->load()));
    return true;
}

bool LoopNestProcessor::refitTempoToTrim()
{
    trimMovedAt.store(0);
    if (restoring.load() > 0)
        return false;
    // The tempo is the file's and a trim never moves it (the bar lines stay put, like a
    // clip's timeline). Only a file nothing has fit yet takes a tempo from its trim.
    const juce::ScopedLock sl(stateLock);
    if (loopBpmSource != BpmSource::none)
        return false;
    return fitRegionLocked();
}

bool LoopNestProcessor::trimGestureOpen() const
{
    for (int i : { startIndex, endIndex })
        if (juce::isPositiveAndBelow(i, (int) gestureDepth.size()) && gestureDepth[(size_t) i] > 0)
            return true;
    return false;
}

void LoopNestProcessor::parameterChanged(const juce::String&, float)
{
    // start / end only (see the constructor). Any thread, the audio thread included.
    if (restoring.load() == 0)
        trimMovedAt.store(juce::jmax((juce::uint32) 1, juce::Time::getMillisecondCounter()));
}

bool LoopNestProcessor::settleTrimNow()
{
    if (trimMovedAt.load() == 0 || trimGestureOpen())
        return false;
    refitTempoToTrim();
    return true;
}

void LoopNestProcessor::timerCallback()
{
    if (retiredWaiting.exchange(false))
    {
        // A loop the bar-grid lock swapped out: free it here, off the audio thread.
        juce::AudioBuffer<float> retired;
        {
            const juce::SpinLock::ScopedLockType lock(sampleLock);
            if (! swapQueued)
                std::swap(retired, retiringBuffer);
        }
    }
    const auto at = trimMovedAt.load();
    if (at != 0 && juce::Time::getMillisecondCounter() - at >= kTrimSettleMs)
        settleTrimNow();
}

bool LoopNestProcessor::holdsRetiredLoop()
{
    const juce::SpinLock::ScopedLockType lock(sampleLock);
    return retiringBuffer.getNumSamples() > 0;
}

void LoopNestProcessor::pressPlay()
{
    if (barLocked.load())
    {
        lockMuted.store(! lockMuted.load());
        return;
    }
    // Free PLAY (the host stopped, or not locked): the user's own play. It clears the
    // mute too, so a mute set earlier (or saved with the set) can't silently cut the
    // loop just heard when the host then starts.
    lockMuted.store(false);
    transportPlay.store(false);
    playRequested.store(! playRequested.load());
}

void LoopNestProcessor::setFxMode(bool fx)
{
    // FX has no audition voice: stop it so the PLAY key / reels read true. The
    // audio thread does the chain reset when it sees the new mode (processBlock).
    if (fx)
    {
        playRequested.store(false);
        ++fxGen;
    }
    fxMode.store(fx);
}

loopnest::CharacterParams LoopNestProcessor::readCharacter() const
{
    loopnest::CharacterParams p;
    for (int i = 0; i < loopnest::CharacterParams::kNum; ++i)
        p[i] = characterRefs[(size_t) i]->load();
    for (int i = 0; i < loopnest::CharacterParams::kNumStages; ++i)
        p.on[(size_t) i] = powerRefs[(size_t) i]->load() > 0.5f;
    return p;
}

void LoopNestProcessor::pushChainParams(CharacterChain& chain) const
{
    chain.setParams(readCharacter());
}

int LoopNestProcessor::printPrimePasses(double regionSec) const
{
    // At least ~1 s of build-up (as before), and the live tail to 60 dB down: ~1 s left a
    // short loop under a long tail (HALL, DECAY 100: 5.5 s) short of what it builds (D-M4).
    const double sec = juce::jmax(1.0, CharacterChain::tailSec(readCharacter()));
    return juce::jmax(1, (int) std::ceil(sec / juce::jmax(0.001, regionSec)));
}

void LoopNestProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi); // No MIDI bus (an effect); LoopNest ignores MIDI entirely.
    const int n = buffer.getNumSamples();
    const bool fits = n <= bypDry.getNumSamples();
    delayHostInput(buffer, fits ? &bypDry : nullptr);
    renderBlock(buffer);
    if (hostBypassed && fits)
    {
        // Out of the host's bypass: from the delayed input over to the chain, ~5 ms.
        const int len = juce::jmin(n, juce::roundToInt(0.005 * hostSampleRate));
        for (int ch = 0; ch < juce::jmin(2, buffer.getNumChannels()); ++ch)
            for (int i = 0; i < len; ++i)
            {
                const float g = ((float) i + 0.5f) / (float) len;
                buffer.setSample(ch, i, bypDry.getSample(ch, i) + g * (buffer.getSample(ch, i) - bypDry.getSample(ch, i)));
            }
    }
    hostBypassed = false;
}

void LoopNestProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (bypLine[0].empty())       // not prepared: the input as it is (no latency yet)
        return;
    const int n = buffer.getNumSamples();
    if (n > bypWet.getNumSamples())
    {
        delayHostInput(buffer, &buffer);       // too big to run the chain alongside: the delay alone
        hostBypassed = true;
        return;
    }
    juce::AudioBuffer<float> wet(bypWet.getArrayOfWritePointers(), 2, n);
    for (int ch = 0; ch < 2; ++ch)
        wet.copyFrom(ch, 0, buffer, juce::jmin(ch, buffer.getNumChannels() - 1), 0, n);
    delayHostInput(wet, &bypDry);
    renderBlock(wet);                          // the chain runs on, unheard
    juce::ignoreUnused(midi);
    // Into the host's bypass: from the chain over to the delayed input, ~5 ms.
    const int len = hostBypassed ? 0 : juce::jmin(n, juce::roundToInt(0.005 * hostSampleRate));
    for (int ch = 0; ch < juce::jmin(2, buffer.getNumChannels()); ++ch)
        for (int i = 0; i < n; ++i)
        {
            const float d = bypDry.getSample(ch, i);
            const float g = i < len ? ((float) i + 0.5f) / (float) len : 1.0f;
            buffer.setSample(ch, i, wet.getSample(ch, i) + g * (d - wet.getSample(ch, i)));
        }
    hostBypassed = true;
}

void LoopNestProcessor::delayHostInput(const juce::AudioBuffer<float>& in, juce::AudioBuffer<float>* out) noexcept
{
    const int lat = (int) bypLine[0].size();
    if (lat <= 0)
        return;
    const int numIn = juce::jmin(getMainBusNumInputChannels(), in.getNumChannels());
    for (int i = 0; i < in.getNumSamples(); ++i)
    {
        float x[2] = { 0.0f, 0.0f };                 // (both read first: `out` may be `in`)
        for (int ch = 0; ch < 2 && numIn > 0; ++ch)
        {
            const float v = in.getSample(juce::jmin(ch, numIn - 1), i);
            x[ch] = std::isfinite(v) ? v : 0.0f;
        }
        for (int ch = 0; ch < 2; ++ch)
        {
            float& slot = bypLine[(size_t) ch][(size_t) bypPos];
            if (out != nullptr && ch < out->getNumChannels())
                out->setSample(ch, i, slot);
            slot = x[ch];
        }
        if (++bypPos >= lat) bypPos = 0;
    }
}

void LoopNestProcessor::renderBlock(juce::AudioBuffer<float>& buffer)
{
    juce::ScopedNoDenormals noDenormals;

    // Host tempo for SYNC, polled every block. A block that reports
    // none (no playhead, or the host has no tempo) keeps the last tempo seen.
    // The bar-grid lock reads the rest of the clock; a block without one is unlocked.
    host = {};
    if (auto* ph = getPlayHead())
        if (const auto pos = ph->getPosition())
        {
            if (const auto bpm = pos->getBpm(); bpm.hasValue() && *bpm > 0.0)
                lastHostBpm.store(*bpm);
            readHostClock(*pos);
        }

    // Stage 2 meters: the host input's peak before anything overwrites the buffer.
    // Taken max-and-held here, drained by the editor (takeLevels), so no block is missed.
    {
        const int n = buffer.getNumSamples(), nIn = juce::jmin(2, getTotalNumInputChannels());
        for (int ch = 0; ch < 2; ++ch)
        {
            const float pk = ch < nIn ? buffer.getMagnitude(ch, 0, n)
                                      : (nIn == 1 ? buffer.getMagnitude(0, 0, n) : 0.0f);
            holdMax(inPeak[ch], pk);
        }
    }

    // Try to grab the sample; if a load is swapping it in, skip this block. FX mode
    // takes it too: an install resets `character` under this lock, so holding it is
    // what keeps the chain single-threaded in either mode.
    const juce::SpinLock::ScopedTryLockType lock(sampleLock);
    if (! lock.isLocked())
    {
        sampleLockMisses.fetch_add(1, std::memory_order_relaxed);
        buffer.clear();
        return;
    }

    // AUDITION <-> FX switch: render this block on the path we were on, ramped to
    // silence, then flip, reset the chain's time-based state (echo/reverb/flutter —
    // so an audition tail can't bleed into the clip) and ramp the next block in.
    // The fxMode-vs-fxActive mismatch IS the reset flag, consumed here, lock-free.
    const bool wantFx    = fxMode.load();
    const bool switching = (wantFx != fxActive);

    // Into FX, the track's audio fades in on the way IN: the reset chain holds its fixed
    // latency of silence, so a fade on the output would be spent on that, and the audio
    // would step in after it (ruling 3).
    if (fxActive && modeFadeIn)
        buffer.applyGainRamp(0, buffer.getNumSamples(), 0.0f, 1.0f);

    if (fxActive) renderFx(buffer);
    else          renderAudition(buffer);

    const int numFrames = buffer.getNumSamples();
    if (modeFadeIn)
    {
        buffer.applyGainRamp(0, numFrames, 0.0f, 1.0f);
        modeFadeIn = false;
    }
    if (switching)
    {
        buffer.applyGainRamp(0, numFrames, 1.0f, 0.0f);
        fxActive = wantFx;
        character.reset();
        resetLatencyLine();
        monitor.reset(bypassParam->load() > 0.5f);   // fresh gain-match trackers
        playbackActive  = false;                  // audition re-enters via its play edge
        lockActive = lockPlaying = false;         // FX: nothing for a loop to wait on
        barLocked.store(false);
        if (swapQueued)
        {
            // A loop waiting on the bar line lands now, silently (this block faded out),
            // and the timer frees the old one: nothing parks in FX.
            swapQueued = false;
            retiredWaiting.store(true);
            readPos    = 0.0;
        }
        stopping        = false;
        pendingSeekNorm = -1.0f;
        modeFadeIn      = true;
    }

    for (int ch = 0; ch < 2; ++ch)
        holdMax(outPeak[ch], ch < buffer.getNumChannels() ? buffer.getMagnitude(ch, 0, numFrames) : 0.0f);
}

void LoopNestProcessor::takeLevels(float in[2], float out[2])
{
    for (int ch = 0; ch < 2; ++ch)
    {
        in[ch]  = inPeak[ch].exchange(0.0f);
        out[ch] = outPeak[ch].exchange(0.0f);
    }
}

// FX mode: host input -> input gain -> EQ -> chain -> mix -> monitor -> output gain.
// No audition voice. Same per-sample ramps as the audition so knob moves and
// automation don't zipper. Caller holds sampleLock (see processBlock).
void LoopNestProcessor::renderFx(juce::AudioBuffer<float>& buffer)
{
    const int numIn  = getMainBusNumInputChannels();
    const int numOut = juce::jmin(2, buffer.getNumChannels());
    if (numIn <= 0 || numOut <= 0)
    {
        buffer.clear();
        return;
    }

    smInGain.setTargetValue(juce::Decibels::decibelsToGain(inputParam->load(),  -60.0f));
    smOutGain.setTargetValue(juce::Decibels::decibelsToGain(outputParam->load(), -60.0f));
    smMix.setTargetValue(juce::jlimit(0.0f, 1.0f, mixParam->load() * 0.01f));
    pushChainParams(character);
    const bool bypass = bypassParam->load()    > 0.5f;
    const bool gmatch = gainMatchParam->load() > 0.5f;

    for (int frame = 0; frame < buffer.getNumSamples(); ++frame)
    {
        const float inGain  = smInGain.getNextValue();
        const float mix01   = smMix.getNextValue();
        const float outGain = smOutGain.getNextValue();

        // A mono input feeds both channels (the output bus is always stereo). A NaN or
        // inf from upstream plays as silence: the chain's recursive state never takes it
        // in (C-M10).
        float s[2] = { 0.0f, 0.0f };
        for (int ch = 0; ch < numOut; ++ch)
        {
            const float x = buffer.getSample(juce::jmin(ch, numIn - 1), frame);
            s[ch] = std::isfinite(x) ? x * inGain : 0.0f;
        }

        // The track's own audio, pre-EQ: what BYPASS plays and GAIN MATCH targets.
        float src[2] = { s[0], s[1] };

        character.processEq(s, numOut);          // master EQ pre-rack
        float dry[2] = { s[0], s[1] };
        character.process(s, numOut);
        character.delayTaps(src, dry, numOut);          // both taps on the chain's fixed delay
        character.alignDry(dry, numOut, mix01);         // the dry tap in DRIVE's phase (no comb under MIX)
        float blended[2] = { 0.0f, 0.0f };
        for (int ch = 0; ch < numOut; ++ch)
            blended[ch] = dry[ch] + mix01 * (s[ch] - dry[ch]);
        monitor.process(src, blended, numOut, gmatch, bypass);
        for (int ch = 0; ch < numOut; ++ch)
            buffer.setSample(ch, frame, blended[ch] * outGain);
    }
}

// Bar-grid lock helpers (see isBarLocked). Audio thread.
void LoopNestProcessor::readHostClock(const juce::AudioPlayHead::PositionInfo& pos) noexcept
{
    const auto ppq = pos.getPpqPosition();
    const auto bpm = pos.getBpm();
    host.playing = pos.getIsPlaying();
    // A clock the lock can use: finite, a sane tempo, and a position a double still
    // counts frames in. Anything else leaves the audition unlocked (never NaN in it).
    host.valid = ppq.hasValue() && bpm.hasValue() && std::isfinite(*ppq) && std::isfinite(*bpm)
              && *bpm >= 1.0 && *bpm <= 1000.0 && std::abs(*ppq) < 1.0e9;
    if (! host.valid)
        return;
    host.ppq = *ppq;
    host.bpm = *bpm;
    // Bars of whole quarter notes from the host's meter (4/4 = 4, 3/4 and 6/8 = 3).
    bool meter = false;
    if (const auto ts = pos.getTimeSignature(); ts.hasValue() && ts->numerator >= 1 && ts->numerator <= 64
                                                 && ts->denominator >= 1 && ts->denominator <= 64)
    {
        host.barBeats = (double) ts->numerator * 4.0 / (double) ts->denominator;
        meter = true;
    }
    // The host's bar start if it is this bar's; else count it.
    const auto bar = pos.getPpqPositionOfLastBarStart();
    host.barStart = bar.hasValue() && std::isfinite(*bar) && host.ppq >= *bar - 1.0e-6
                         && host.ppq - *bar < host.barBeats + 1.0e-6
                      ? *bar : std::floor(host.ppq / host.barBeats) * host.barBeats;
    // The drummer's re-sync (Andrew 10-18): the loop's beat 0 moves to the first bar line
    // in a new meter, and to any bar line the anchor's bars don't land on (a meter change
    // jumped past by a relocate). A relocate back before the anchor counts from the song's
    // start again. In one meter all song the anchor stays at 0. A host that gives bar
    // starts but no meter is read as 4/4, so its bar lines aren't a 4/4 grid's in another
    // meter: without a meter only a change re-syncs, never the off-grid test (it fired on
    // every bar of a 3/4 song).
    if (host.ppq < phaseAnchor - 1.0e-6)
        phaseAnchor = 0.0;
    const double off = std::abs(std::fmod(host.barStart - phaseAnchor, host.barBeats));
    if ((lastBarBeats > 0.0 && std::abs(host.barBeats - lastBarBeats) > 1.0e-9)
        || (meter && juce::jmin(off, host.barBeats - off) > 1.0e-6))
        phaseAnchor = host.barStart;
    lastBarBeats = host.barBeats;
}

void LoopNestProcessor::advanceHost(int frames) noexcept
{
    if (! host.valid || ! host.playing)
        return;
    host.ppq += (double) frames * host.bpm / (60.0 * hostSampleRate);
    const double bars = std::floor((host.ppq - host.barStart + 1.0e-9) / host.barBeats);
    if (bars > 0.0)
        host.barStart += bars * host.barBeats;
}

double LoopNestProcessor::hostBeats() const noexcept
{
    // From the anchor (readHostClock). Counting whole bars of the current meter instead
    // (round(barStart / barBeats)) re-counted every earlier bar on a change, and a 4/4 ->
    // 3/4 bar landed the loop a beat off (C-M8). In one meter all song the anchor is 0 and
    // this is the host's ppq, which that sum was too: a 4/4 lock renders bit for bit as
    // before (10-18).
    return host.ppq - phaseAnchor;
}

void LoopNestProcessor::latchSwapBar() noexcept
{
    // The next bar line, in song beats; one within half a frame behind is this one. A
    // relocate back past it (more than a bar ahead now) latches the new next one; one
    // forward past it lands the swap at once (renderAudition).
    const double b = hostBeats();
    if (swapLatched && swapAtBeats - b <= host.barBeats + 1.0e-6)
        return;
    const double into = host.ppq - host.barStart;
    swapAtBeats = b - into + (into * 60.0 * hostSampleRate / host.bpm < 0.5 ? 0.0 : host.barBeats);
    swapLatched = true;
}

bool LoopNestProcessor::lockGeometry(int numSampleFrames, LockGeom& g) const noexcept
{
    if (numSampleFrames <= 1 || loopBpm <= 0.0 || sampleFileSampleRate <= 0.0 || ! host.valid)
        return false;
    // A ratio the stretcher would clamp isn't the host's tempo, so it can't be its grid.
    const double ratio = host.bpm / loopBpm;
    if (ratio < loopnest::kMinSyncRatio || ratio > loopnest::kMaxSyncRatio)
        return false;
    int startFrame, endFrame;
    regionFrames(numSampleFrames, startFrame, endFrame);
    // Andrew 10-13 (late): the loop is the trim exactly, any length; no rounding.
    g.startFrame = startFrame;
    g.fpb        = sampleFileSampleRate * 60.0 / loopBpm;
    g.regionLen  = endFrame - startFrame;
    g.loopLen    = (double) g.regionLen;
    g.endPos     = (double) endFrame;
    g.seamLen    = seamFrames(sampleFileSampleRate, g.regionLen);
    g.srRatio    = sampleFileSampleRate / hostSampleRate;
    g.bpm        = loopBpm;
    return true;
}

double LoopNestProcessor::lockPos(const LockGeom& g, double beats) noexcept
{
    // Andrew 10-13 (late): the left handle is bar 1, on song beat 0, and the loop cycles
    // at its own length from there (an odd length slides against the song's bars).
    const double into = std::fmod(g.fpb * beats, g.loopLen);
    return (double) g.startFrame + (into < 0.0 ? into + g.loopLen : into);
}

// The SYNC voice (see the header). Linear-interpolated, seam-faded reads
// at `step`, wrapped at the trim seam exactly like the varispeed voice below.
void LoopNestProcessor::feedSyncVoice(const juce::AudioBuffer<float>& src, float* const* dest, int numFrames,
                                      int startFrame, double endPos, int regionLen, int seamLen, bool looping,
                                      double step) noexcept
{
    const int srcChannels = src.getNumChannels();
    for (int i = 0; i < numFrames; ++i)
    {
        if (! voiceEnded)
        {
            if (readPos >= endPos)
            {
                // Looping: seamless wrap that keeps the fractional overshoot, so a step
                // that doesn't divide the region (file SR != host SR) never drifts off
                // the host grid, a fraction of a frame per wrap (C7). One-shot: rewind.
                const double over = readPos - endPos;
                readPos = (double) startFrame + (looping && over < (double) regionLen ? over : 0.0);
                voiceEnded = ! looping;
            }
            if (readPos < (double) startFrame)   // trim moved under us mid-play
                readPos = (double) startFrame;
        }
        if (voiceEnded)
        {
            dest[0][i] = dest[1][i] = 0.0f;       // one-shot: the stretcher drains on silence
            continue;
        }
        for (int ch = 0; ch < 2; ++ch)            // a mono source feeds both channels
            dest[ch][i] = readSeamed(src, juce::jmin(ch, srcChannels - 1), readPos,
                                     startFrame, regionLen, seamLen);
        readPos += step;
    }
}

// A spread prime's pre-roll output, kept nowhere (B5): the stretcher writes through
// outputs[c][i], so a sink needs no buffer.
namespace
{
    struct DiscardOutput
    {
        struct Channel { float sink = 0.0f; float& operator[](int) noexcept { return sink; } } channel;
        Channel& operator[](int) noexcept { return channel; }
    };
}

// AUDITION mode: the spun loop through the chain (v1's processBlock body).
// Host input is discarded. Caller holds sampleLock (see processBlock).
void LoopNestProcessor::renderAudition(juce::AudioBuffer<float>& buffer)
{
    // The SYNC scratch is sized for prepareToPlay's max block. A host that
    // hands over more runs as consecutive sub-blocks (an AudioBuffer over external
    // data keeps its channel pointers inline — no allocation).
    // A split moves the host clock along for each part and puts it back after, so
    // every frame advances it exactly once (at the caller's level).
    if (stretchMaxBlock > 0 && buffer.getNumSamples() > stretchMaxBlock)
    {
        const auto entry = host;
        for (int start = 0; start < buffer.getNumSamples(); start += stretchMaxBlock)
        {
            juce::AudioBuffer<float> sub(buffer.getArrayOfWritePointers(), buffer.getNumChannels(),
                                         start, juce::jmin(stretchMaxBlock, buffer.getNumSamples() - start));
            renderAudition(sub);
            advanceHost(sub.getNumSamples());
        }
        host = entry;
        return;
    }

    // Bar-grid lock: a loop that landed under the locked voice swaps in on the bar
    // line it latched. A block the line falls inside runs as two: the old loop up to
    // the line (faded out over its last 2 ms, below), the new one from it.
    if (! swapQueued)
        swapLatched = swapHoldsParams = false;
    else if (lockActive && playbackActive && host.valid && host.playing)
    {
        latchSwapBar();
        const int at = (int) std::lround((swapAtBeats - hostBeats()) * 60.0 * hostSampleRate / host.bpm);
        if (at > 0 && at < buffer.getNumSamples())
        {
            const auto entry = host;
            juce::AudioBuffer<float> head(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), 0, at);
            juce::AudioBuffer<float> tail(buffer.getArrayOfWritePointers(), buffer.getNumChannels(),
                                          at, buffer.getNumSamples() - at);
            renderAudition(head);
            advanceHost(at);
            renderAudition(tail);
            host = entry;
            return;
        }
    }

    buffer.clear();   // discard host input — a clip on the track can't double the audition

    // The transport gates an armed PLAY under the lock (see isBarLocked): the host
    // stopping a locked loop drops PLAY and holds it; the host starting plays it again.
    const bool hostRolling = host.valid && host.playing;
    const bool followHost  = syncEnabled.load() && freeBpm.load() <= 0.0 && stretchMaxBlock > 0
                          && loopEnabled.load();
    // The host's transport is the master under the lock's conditions (see isBarLocked):
    // its start plays the audition (lockWanted, below), its stop stops it.
    if (hostRolling != hostWasPlaying)
    {
        if (! hostRolling)
        {
            if (lockActive || transportPlay.load())
                playRequested.store(false);
            transportPlay.store(false);
        }
        hostWasPlaying = hostRolling;
    }

    const int numSampleFrames = sampleBuffer.getNumSamples();
    // A locked voice plays on through a restore or an install, its loop frozen (an
    // install queues the new loop for the bar line, see installFinishedLoad).
    // So does the old loop of a queued swap, which never reads the params.
    const bool lockCarries = (lockActive && playbackActive && hostRolling && followHost)
                          || (swapQueued && playbackActive);
    if (numSampleFrames <= 0 || ((restoring.load() > 0 || muteUntilGen.load() != 0) && ! lockCarries))
    {
        // Empty, or a restore/install is mid-flight (params and audio not both in
        // yet): silence. Play resumes with the usual fade-in once it's installed.
        // (What the voice already put in the chain comes out first, as it was.)
        if (playbackActive)
            latencyDrain = character.latency();
        playbackActive = false;
        lockActive = lockPlaying = false;
        barLocked.store(false);
        if (swapQueued)
        {
            swapQueued = false;   // nothing sounding: as the install would have landed it
            retiredWaiting.store(true);
            readPos    = 0.0;
            character.reset();
        }
        drainLatency(buffer, 0);
        return;
    }

    const int numOutChannels = buffer.getNumChannels();
    const int numFrames      = buffer.getNumSamples();

    // Consume a pending UI seek (waveform click/scrub). Stopped, the playhead just
    // relocates instantly; playing, the jump is deferred behind the declick fade
    // (below) so rewriting readPos mid-waveform doesn't click.
    if (const float seekN = seekRequestNorm.exchange(-1.0f); seekN >= 0.0f)
    {
        if (playbackActive)
            pendingSeekNorm = seekN;
        else
            readPos = juce::jlimit(0.0, (double) (numSampleFrames - 1),
                                   (double) seekN * (double) numSampleFrames);
    }

    // Playback rate: correct for file/host SR mismatch, then apply pitch shift
    // (varispeed). SYNC on: the voice reads at the plain SR-correction
    // rate and the stretcher applies tempo (hostBPM/nativeBPM, recomputed every
    // block so a live host tempo change is followed) and PITCH as a transpose.
    const double srRatio    = sampleFileSampleRate / hostSampleRate;
    // `sync` = the stretcher runs: SYNC (the host's tempo) or a free tempo (LoopNest's own).
    const bool   sync       = isStretching() && stretchMaxBlock > 0;
    const double tempo      = sync ? syncRatio(loopBpm, getTargetBpm()) : 1.0;
    // A queued swap that brings its own knobs (a history step): the old loop keeps its
    // pitch, chain and levels to the bar line; they change with the loop.
    const bool holdParams   = swapQueued && swapHoldsParams;
    double       pitchSemis = holdParams ? heldSemis : effectiveSemis(pitchParam->load());   // (a held swap's landing resets it)
    const double rate       = sync ? srRatio : srRatio * std::pow(2.0, pitchSemis / 12.0);
    heldSemis = pitchSemis;

    if (! holdParams)
    {
        // Input level — gain-staged into the rack (pre-chain, before the dry/wet split).
        // Output level (the big knob). Both floor to silence at -60 dB, and both ramp
        // per-sample (~20 ms) so drags/preset slams don't zipper.
        smInGain.setTargetValue(juce::Decibels::decibelsToGain(inputParam->load(),  -60.0f));
        smOutGain.setTargetValue(juce::Decibels::decibelsToGain(outputParam->load(), -60.0f));

        // Stage 2 character chain (live audition). Same chain runs in renderLoop + FX.
        // (Not while a held step's knobs are still landing: landAtLine.)
        if (! character.lineHeld())
            pushChainParams(character);

        // Master dry/wet: 1 = full character chain, 0 = the clean (pitched/trimmed) tap.
        smMix.setTargetValue(juce::jlimit(0.0f, 1.0f, mixParam->load() * 0.01f));
    }

    // GAIN MATCH + BYPASS: the shared monitor stage (MonitorStage.h), same as FX.
    const bool bypass = bypassParam->load()    > 0.5f;
    const bool gmatch = gainMatchParam->load() > 0.5f;

    // PITCH glide: per-sample slew of the playback rate toward its target. 0 % =
    // instant (coeff 1). The time constant grows ~quadratically up to kGlideMaxSec.
    // Under SYNC the same time constant slews the transpose in
    // semitones, once per block; the tempo ratio is never slewed.
    constexpr double kGlideMaxSec = 1.5;
    const float  glide01    = juce::jlimit(0.0f, 1.0f, pitchGlideParam->load() * 0.01f);
    const double glideTau   = (double) glide01 * glide01 * kGlideMaxSec;
    const double glideCoeff = glideTau <= 0.0 ? 1.0
                            : 1.0 - std::exp(-1.0 / (glideTau * hostSampleRate));
    const double glideBlockCoeff = glideTau <= 0.0 ? 1.0
                                 : 1.0 - std::exp(-(double) numFrames / (glideTau * hostSampleRate));

    int startFrame, endFrame;
    regionFrames(numSampleFrames, startFrame, endFrame);
    const int  regionLen = endFrame - startFrame;
    const int  seamLen   = seamFrames(sampleFileSampleRate, regionLen);
    const bool looping   = loopEnabled.load();

    // Bar-grid lock (see isBarLocked). The old loop's geometry is frozen while a swap
    // waits, and while a restore lands under the locked voice (its params in flux).
    auto ratioOk = [this] (double bpm)
    {
        const double r = host.bpm / bpm;
        return bpm > 0.0 && r >= loopnest::kMinSyncRatio && r <= loopnest::kMaxSyncRatio;
    };
    const bool lockHolds = sync && followHost && hostRolling && lockActive && playbackActive;
    // A queued loop: nothing sounding, it lands now as any install does; under the lock,
    // on its latched bar line, at its song-grid place; with the lock gone, the old loop fades
    // out first (swapCoast, the per-sample loop lands it). Past the line while a restore
    // still lands, the old loop stays faded out (below) until it can.
    bool swapCoast = false;
    if (swapQueued)
    {
        if (! playbackActive)
        {
            // (The chain is reset on the next play edge, after any stop still draining.)
            swapQueued = false;
            retiredWaiting.store(true);
            readPos    = 0.0;
        }
        else if (lockHolds && ratioOk(lockGeom.bpm))
        {
            latchSwapBar();
            const double halfFrame = 0.5 * host.bpm / (60.0 * hostSampleRate);
            if (hostBeats() >= swapAtBeats - halfFrame && restoring.load() == 0 && muteUntilGen.load() == 0)
            {
                const bool knobs = swapHoldsParams;
                swapQueued = false;
                retiredWaiting.store(true);       // the timer frees the old loop
                readPos = (double) startFrame;
                stretchNeedsPrime = true;         // on the grid (below)
                declickGain = 0.0f;
                spliceIn    = 0.0f;               // the new loop comes into the chain faded in too
                if (knobs)
                {
                    // The step's own knobs, exact on the line: no glide up from the old
                    // pitch, no ramp from the old levels, no slew of the chain.
                    pitchSemis = heldSemis = glideSemis = effectiveSemis(pitchParam->load());
                    glideRate = srRatio * (sync ? 1.0 : std::pow(2.0, pitchSemis / 12.0));
                    smInGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(inputParam->load(),  -60.0f));
                    smOutGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(outputParam->load(), -60.0f));
                    smMix.setCurrentAndTargetValue(juce::jlimit(0.0f, 1.0f, mixParam->load() * 0.01f));
                    // The chain's on the line as heard: the old loop's last latency() frames
                    // inside the bases keep the old knobs (H1, CharacterChain::landAtLine).
                    character.landAtLine(readCharacter());
                }
            }
        }
        else
            swapCoast = true;
    }
    const bool lockFrozen = swapQueued
                         || ((restoring.load() > 0 || muteUntilGen.load() != 0) && lockActive && playbackActive);
    bool lockWanted = ! swapCoast && sync && followHost && hostRolling;
    if (lockWanted)
        lockWanted = lockFrozen ? ratioOk(lockGeom.bpm) : lockGeometry(numSampleFrames, lockGeom);
    // The stretcher path: SYNC or a free tempo, or a queued swap's old loop fading out.
    const bool stretched = sync || swapCoast;
    // Out of the lock with the host still rolling. The user's PLAY carries the audition
    // on as free PLAY (a muted one stays silent: free PLAY off, the mute kept for the
    // next press to clear). One the transport started (transportPlay) stops
    // at once when the lock merely can't hold (a loop with no tempo, a ratio out of
    // range: SYNC and LOOP still ask for it); when the user left it (SYNC off, a free
    // tempo, LOOP off) it plays on, free, to the host's next stop. A host stop: above.
    if (lockActive && playbackActive && ! lockWanted && hostRolling)
    {
        if (transportPlay.load() && sync && followHost)
        {
            playRequested.store(false);
            transportPlay.store(false);
        }
        else
            playRequested.store(! lockMuted.load());
    }

    // Where the ear is under SYNC. The voice runs ahead of the output by
    // the stretcher's input latency plus its output latency (in input frames at
    // this tempo), converted to source frames; wrapped back across the seam when
    // looping. The scrubber shows this, and a stop/sync-off rewinds to it.
    auto heardPos = [&]() -> double
    {
        const double lag = ((double) stretch->st.inputLatency()
                            + (double) stretch->st.outputLatency() * tempo) * srRatio;
        const double p = readPos - lag;
        if (p >= (double) startFrame || regionLen <= 0)
            return p;
        return looping ? (double) endFrame + std::fmod(p - (double) startFrame, (double) regionLen)
                       : (double) startFrame;
    };

    // Publishes the current readPos to the UI scrubber as a 0..1 fraction.
    // Divide by (N-1), NOT N, so this matches how regionFrames() maps the trim
    // params (start/end * lastFrame). Otherwise the playhead's reachable range is
    // systematically narrower than the brackets and it parks just short of them.
    // A fully open end is frame N (the exclusive end past the last sample); the
    // clamp holds the playhead at 1 through that last sample.
    // Under the lock the ear is on the host's grid, and so is the playhead; the lock's
    // flags are published with it.
    auto publishPlayhead = [&]
    {
        lockActive  = lockActive && playbackActive;
        lockPlaying = lockActive && ! stopping;
        barLocked.store(lockActive);
        const double p = lockActive ? lockPos(lockGeom, hostBeats() + (double) numFrames * host.bpm
                                                                     / (60.0 * hostSampleRate))
                       : (playbackActive && stretchActive) ? heardPos() : readPos;
        playheadNorm.store((float) juce::jlimit(0.0, 1.0,
                                  p / (double) juce::jmax(1, numSampleFrames - 1)));
    };

    // Play edge: on the transition into play, RESUME from the current position
    // (so Pause/Play continues where it left off). Only snap to the region start
    // if the position is stale — outside the region or never set.
    const bool wantPlay = playRequested.load() || lockWanted;   // the host's start plays it
    if (wantPlay && ! playbackActive && latencyDrain == 0)   // (a stop still draining goes out first)
    {
        if (! playRequested.load())
            transportPlay.store(true);   // the host's start, not the key, started it
        if (readPos < (double) startFrame || readPos >= (double) endFrame)
            readPos = (double) startFrame;
        glideRate  = rate;         // start at the current pitch so Play doesn't glide from 0
        glideSemis = pitchSemis;   // (SYNC: same for the transpose)
        character.reset();
        resetLatencyLine();
        monitor.reset(bypass);                    // fresh gain-match trackers, no A/B crossfade at start
        declickGain = 0.0f;                       // fade in from silence
        playbackActive = true;
        // Every entry into play — Play, a new spin/sample, the FX ->
        // AUDITION switch (all of which drop playbackActive) — resets + primes the
        // stretcher, so no stale spectrum from before smears the onset.
        stretchActive     = sync;
        stretchNeedsPrime = true;
        voiceEnded        = false;
        drainLeft         = 0;
    }
    if (wantPlay)
        stopping = false;            // re-pressed Play during the fade-out: resume
    else if (playbackActive)
        stopping = true;             // pause: fade out first, stop in the loop below

    if (! playbackActive)
    {
        drainLatency(buffer, 0);
        publishPlayhead();
        return;
    }

    // SYNC flipped mid-play. Leaving sync, rewind the voice to the heard
    // position so varispeed carries on from what was playing; entering, prime. A
    // short fade-in covers the path change either way.
    if (sync != stretchActive && ! swapCoast)
    {
        if (stretchActive)
            readPos = heardPos();
        stretchActive     = sync;
        stretchNeedsPrime = sync;
        voiceEnded        = false;
        drainLeft         = 0;
        glideRate         = rate;
        glideSemis        = pitchSemis;
        declickGain       = 0.0f;
    }

    // Into the lock (the host started under a free-running PLAY, SYNC turned on):
    // prime on the grid. Out of it: the free voice carries on from here.
    if (lockWanted != lockActive)
    {
        if (lockWanted)
        {
            stretchNeedsPrime = true;
            muteGain = lockMuted.load() ? 0.0f : 1.0f;   // a muted start stays silent
        }
        else
        {
            stretchInAcc = 0.0;
            declickGain  = juce::jmin(declickGain, muteGain);   // a muted loop leaves silent
        }
        lockActive = lockWanted;
    }
    if (! lockActive)
    {
        lockFastIn = false;
        muteGain   = 1.0f;
    }
    // The PLAY key under the lock: a mute, declicked, the voice running on the grid.
    const bool muted = lockMuted.load();

    // This block's stretched voice. A mid-play seek under SYNC lands here,
    // at a block start, once the declick fade-out is silent (the per-sample loop
    // holds the output at zero until then), and re-primes from the new position.
    bool primeHold = false;   // a spread prime's pre-roll ran instead: this block is silent
    if (stretched)
    {
        if (lockActive || swapCoast)
            pendingSeekNorm = -1.0f;   // the host's grid owns the position
        if (pendingSeekNorm >= 0.0f && declickGain < 1.0e-3f)
        {
            readPos = juce::jlimit(0.0, (double) (numSampleFrames - 1),
                                   (double) pendingSeekNorm * (double) numSampleFrames);
            pendingSeekNorm   = -1.0f;
            stretchNeedsPrime = true;
            voiceEnded        = false;
            drainLeft         = 0;
        }

        float* in[2]  = { stretchIn.getWritePointer(0),  stretchIn.getWritePointer(1) };
        float* out[2] = { stretchOut.getWritePointer(0), stretchOut.getWritePointer(1) };
        const int inCap = stretchIn.getNumSamples();

        // Transpose first: outputSeek synthesises its pre-roll, and it must do so at
        // this block's transpose, not the last one set (C6).
        glideSemis += glideBlockCoeff * (pitchSemis - glideSemis);
        stretch->st.setTransposeSemitones((float) glideSemis);

        // The prime (see kPrimeOneBlockMin). `feed(n)` writes the voice's next n frames
        // into `in`. True when this block plays; false while a spread prime's pre-roll
        // runs (the block silent, the voice late).
        auto primed = [&] (auto&& feed, double primeTempo)
        {
            const double rateSq = (48000.0 / hostSampleRate) * (48000.0 / hostSampleRate);
            const bool oneBlock = kPrimeOneBlockMin <= 0 || (double) numFrames * rateSq >= (double) kPrimeOneBlockMin;
            if (stretchNeedsPrime)
            {
                stretchNeedsPrime = false;
                primeLeft = 0;
                if (oneBlock)
                {
                    // outputSeek = reset + seek + pre-roll: hand it the first
                    // inputLatency + tempo x outputLatency frames from the play position and
                    // the next process() output starts exactly there — prompt, no smear.
                    const int seekLen = juce::jmin(stretch->st.outputSeekLength((float) primeTempo), inCap);
                    feed(seekLen);
                    stretch->st.outputSeek(in, seekLen);
                    return true;
                }
                // Spread: outputSeek's own steps (reset, a seek on the first inputLatency
                // frames, the pre-roll), the pre-roll processed into nothing a slice a block.
                // All but its last step, which adds the pre-roll output back reversed and
                // negated into the stretcher's private output. So the first output matches
                // outputSeek's only at x1 (-137 dB against it); off x1 it differs (-22 dB
                // at x0.75, -13 dB at x1.3, measured 10-18).
                const int seekIn = juce::jmin(stretch->st.inputLatency(), inCap);
                feed(seekIn);
                stretch->st.reset();
                stretch->st.seek(in, seekIn, primeTempo);
                primeLeft = stretch->st.outputLatency();
                primeAcc  = 0.0;
            }
            if (primeLeft <= 0)
                return true;
            // A callback long enough for the whole prime (a host's tiny or empty first
            // block, then its real ones) finishes the pre-roll at once and plays.
            const int n = oneBlock ? primeLeft : juce::jmin(primeLeft, stretch->st.intervalSamples());
            primeAcc += (double) n * primeTempo;
            const int nIn = juce::jlimit(0, inCap, (int) primeAcc);
            primeAcc -= (double) nIn;
            feed(nIn);
            stretch->st.process(in, nIn, DiscardOutput {}, n);
            primeLeft -= n;
            if (oneBlock)
                return true;
            stretchOut.clear(0, numFrames);
            primeHold = true;
            return false;
        };

        if (lockActive)
        {
            // Bar-grid lock. The voice runs `lag` ahead of the ear (the stretcher's
            // input latency plus its output latency at this tempo), so the frame heard
            // at host beat b is lockPos(b): the latency comes off the start (outputSeek
            // pre-rolls it) and nothing is reported to the host. Every block re-derives
            // where the voice should be from the host's ppq, never from its own count.
            // A small error (a tempo change settling, rounding) is fed off at up to 1/16
            // of the block's input; a jump (a relocate, a brace wrap that isn't whole
            // loops, a trim) primes on the grid at once, so the beat after a wrap is there.
            // The input count carries its fraction (lockInAcc), and the error is
            // measured with that fraction counted as fed, so a tiny block's rounding
            // never builds up against the steer's limit.
            const LockGeom& g = lockGeom;
            const auto& src = swapQueued ? retiringBuffer : sampleBuffer;
            const double lockTempo = host.bpm / g.bpm;
            const double b0 = hostBeats();   // song beats: the trim start on song beats 0, L, 2L, ...
            const double b1 = b0 + (double) numFrames * host.bpm / (60.0 * hostSampleRate);
            const double lag = ((double) stretch->st.inputLatency()
                                + (double) stretch->st.outputLatency() * lockTempo) * g.srRatio;
            auto offGrid = [&g] (double want, double pos)   // source frames, the short way round
            {
                const double e = std::fmod(want - pos, g.loopLen);
                return e >= 0.5 * g.loopLen ? e - g.loopLen : e < -0.5 * g.loopLen ? e + g.loopLen : e;
            };
            auto feed = [&] (int n)
            {
                feedSyncVoice(src, in, n, g.startFrame, g.endPos, g.regionLen, g.seamLen, true, g.srRatio);
            };
            lockTempoLast = lockTempo;
            // An ECHO or REVERB that rings keeps being fed while muted, so the unmute comes
            // back into its wash, not a tail building from silence.
            const auto chain = readCharacter();
            const bool rings = (chain.on[(size_t) loopnest::CharacterParams::kEcho] && chain.echo > 0.0f)
                            || (chain.on[(size_t) loopnest::CharacterParams::kReverb] && chain.reverb > 0.0f);
            if (muted && muteGain == 0.0f && ! rings)
            {
                // Muted to silence, a dry chain: nothing is heard, so the stretcher does no
                // work (B5: four muted instances overran at 96 kHz). The grid runs on in the
                // host's ppq; the unmute primes on it.
                stretchNeedsPrime = true;
                primeLeft = 0;
                stretchOut.clear(0, numFrames);
            }
            else
            {
                // (Not while a spread prime runs: the voice is behind the grid on purpose.)
                if (! stretchNeedsPrime && primeLeft <= 0
                    && std::abs(offGrid(lockPos(g, b0) + lag, readPos + lockInAcc * g.srRatio)) / (g.srRatio * lockTempo)
                         > kLockRephaseSec * hostSampleRate)
                    stretchNeedsPrime = true;
                if (stretchNeedsPrime)
                {
                    if (declickGain > 1.0e-3f)
                    {
                        // Cutting in on sound: carry the last frame out under the fade-in,
                        // from the frame the cut reaches the output (auditionFrame).
                        lockTailIn  = character.latency() + 1;
                        declickGain = 0.0f;
                    }
                    readPos = lockPos(g, b0);
                    lockFastIn = true;
                    lockInAcc = 0.0;
                    ++lockPrimeCount;
                }
                if (primed(feed, lockTempo))
                {
                    const double nominal = (double) numFrames * lockTempo;
                    const double steer = juce::jlimit(-nominal / 16.0, nominal / 16.0,
                                                      offGrid(lockPos(g, b1) + lag, readPos + (nominal + lockInAcc) * g.srRatio)
                                                          / g.srRatio);
                    const double want = nominal + steer + lockInAcc;
                    const int nIn = juce::jlimit(0, inCap, (int) std::floor(want));
                    lockInAcc = juce::jlimit(0.0, 1.0, want - (double) nIn);
                    feed(nIn);
                    stretch->st.process(in, nIn, out, numFrames);
                }
            }
        }
        else if (swapCoast)
        {
            // A queued loop whose lock went away (the transport stopped, SYNC, LOOP or
            // the tempo changed): the old loop runs on at its last locked tempo while it
            // fades out, then the new one lands (the per-sample loop below).
            const LockGeom& g = lockGeom;
            stretchInAcc += (double) numFrames * lockTempoLast;
            const int nIn = juce::jlimit(0, inCap, (int) stretchInAcc);
            stretchInAcc -= (double) nIn;
            feedSyncVoice(retiringBuffer, in, nIn, g.startFrame, g.endPos, g.regionLen, g.seamLen, true, g.srRatio);
            stretch->st.process(in, nIn, out, numFrames);
        }
        else
        {
            auto feed = [&] (int n)
            {
                feedSyncVoice(sampleBuffer, in, n, startFrame, (double) endFrame, regionLen, seamLen, looping, srRatio);
            };
            if (stretchNeedsPrime)
                stretchInAcc = 0.0;
            if (primed(feed, tempo))
            {
                // Fractional accumulator: over any run of blocks the voice feeds
                // round-down(sum(outFrames) x tempo) frames, never drifting from the tempo.
                stretchInAcc += (double) numFrames * tempo;
                const int nIn = juce::jlimit(0, inCap, (int) stretchInAcc);
                stretchInAcc -= (double) nIn;
                feed(nIn);
                stretch->st.process(in, nIn, out, numFrames);

                // One-shot: once the region runs out, let the stretcher's tail play out
                // (its latency, in output frames) before stopping.
                if (voiceEnded && drainLeft <= 0)
                    drainLeft = (int) std::ceil((double) stretch->st.outputLatency()
                                                + (double) stretch->st.inputLatency() / tempo) + numFrames;
            }
        }
    }

    // A loop waiting on its latched bar line: the old one fades out over the 2 ms
    // before it, and stays out past it while a restore holds the swap back. Taken per
    // frame with the other output gains (gainLine), so it lands on the audio it fades.
    const bool   swapFades = swapQueued && swapLatched && lockActive && hostRolling;
    const double toBar     = swapFades ? (swapAtBeats - hostBeats()) * 60.0 * hostSampleRate / host.bpm : 0.0;
    const double fadeLen   = 0.002 * hostSampleRate;
    const int    fadeFrom  = swapFades && toBar - fadeLen < (double) numFrames ? juce::jmax(0, (int) (toBar - fadeLen))
                                                                               : numFrames;

    int frame = 0;
    for (; frame < numFrames; ++frame)
    {
        // Advance the gain/mix ramps once per frame (not per channel).
        const float inGain  = smInGain.getNextValue();
        const float mix01   = smMix.getNextValue();
        const float outGain = smOutGain.getNextValue();

        // Declick ramp: ~2 ms fade out into a pending seek or a stop, fade back in
        // after. The jump/stop lands only once the output is (near) silent. Under
        // SYNC the seek jump waits for the next block start (above); until then the
        // fade target stays 0, so the stale stretched audio stays silent.
        // A spread prime holds it at 0 until the voice starts, which then fades in as a
        // prompt start does.
        const bool fadingOut = stopping || pendingSeekNorm >= 0.0f || swapCoast;
        declickGain += (lockFastIn && ! fadingOut ? lockInCoeff : declickCoeff)
                       * ((fadingOut || primeHold ? 0.0f : 1.0f) - declickGain);
        if (fadingOut && declickGain < 1.0e-3f)
        {
            if (swapCoast)
            {
                // The old loop is out: the queued one lands as any install does, and
                // plays from its play edge if PLAY is still on.
                swapQueued = false;
                retiredWaiting.store(true);
                readPos    = 0.0;
                stopping = false;
                playbackActive = false;
                latencyDrain = character.latency();   // (the play edge resets the chain after)
                break;
            }
            if (pendingSeekNorm >= 0.0f && ! stretched)
            {
                readPos = juce::jlimit(0.0, (double) (numSampleFrames - 1),
                                       (double) pendingSeekNorm * (double) numSampleFrames);
                pendingSeekNorm = -1.0f;
            }
            if (stopping)
            {
                if (sync)
                    readPos = heardPos();   // pause resumes where the ear was
                stopping = false;
                playbackActive = false;
                latencyDrain = character.latency();
                break;
            }
        }

        float outSamples[2] = { 0.0f, 0.0f };
        if (stretched)
        {
            if (drainLeft > 0 && --drainLeft == 0)
            {
                // One-shot tail is out: stop and rewind so the next Play restarts cleanly.
                readPos = (double) startFrame;
                voiceEnded = false;
                playRequested.store(false);
                playbackActive = false;
                latencyDrain = character.latency();
                break;
            }
            for (int ch = 0; ch < numOutChannels; ++ch)
                outSamples[ch] = stretchOut.getSample(juce::jmin(ch, 1), frame) * inGain;
            if (numOutChannels == 1)   // mono bus: downmix the stretcher's L+R
                outSamples[0] = 0.5f * (stretchOut.getSample(0, frame) + stretchOut.getSample(1, frame)) * inGain;
        }
        else
        {
            if (readPos >= (double) endFrame)
            {
                if (looping)
                {
                    readPos = (double) startFrame; // seamless wrap
                }
                else
                {
                    // One-shot: stop and rewind so the next Play restarts cleanly.
                    readPos = (double) startFrame;
                    playRequested.store(false);
                    playbackActive = false;
                    latencyDrain = character.latency();
                    break;
                }
            }
            if (readPos < (double) startFrame) // trim moved under us mid-play
                readPos = (double) startFrame;

            // Linear interpolation between the two nearest source samples, with the
            // seam crossfade over the region head (see readSeamed).
            for (int ch = 0; ch < numOutChannels; ++ch)
            {
                const int srcCh = juce::jmin(ch, sampleBuffer.getNumChannels() - 1);
                outSamples[ch] = readSeamed(sampleBuffer, srcCh, readPos, startFrame, regionLen, seamLen)
                               * inGain;   // input gain pre-rack
            }
            if (numOutChannels == 1 && sampleBuffer.getNumChannels() > 1)
            {
                // Mono bus: downmix L+R rather than dropping the right channel.
                outSamples[0] = 0.5f * (outSamples[0]
                    + readSeamed(sampleBuffer, 1, readPos, startFrame, regionLen, seamLen) * inGain);
            }
        }

        // The voice (post-input-gain, PRE-EQ) through EQ, chain, MIX and the monitor
        // (auditionFrame). GAIN MATCH scales the blend to the dry tap's loudness;
        // BYPASS swaps to it.
        if (lockActive)
        {
            muteGain += declickCoeff * ((muted ? 0.0f : 1.0f) - muteGain);
            if (muted && muteGain < 1.0e-4f)
                muteGain = 0.0f;   // silent, not a decaying trace
        }
        const float swapGain = frame >= fadeFrom ? (float) juce::jlimit(0.0, 1.0, (toBar - (double) frame) / fadeLen) : 1.0f;
        // A swap's splice is faded on the voice as well, before the chain: WARP and FLUTTER
        // read up to ~15 ms either side of their base, so the splice can reach the output
        // away from the output's own fade (gainLine) and step there at full level (H1 review).
        if (const float splice = swapGain * spliceIn; splice < 1.0f)
            for (int ch = 0; ch < numOutChannels; ++ch)
                outSamples[ch] *= splice;
        if (spliceIn < 1.0f)
        {
            spliceIn += (lockFastIn ? lockInCoeff : declickCoeff) * (1.0f - spliceIn);
            if (spliceIn > 0.9999f) spliceIn = 1.0f;
        }
        auditionFrame(buffer, frame, outSamples, { declickGain, muteGain, swapGain, mix01, outGain }, gmatch, bypass);

        if (! stretched)   // SYNC: the voice already advanced in feedSyncVoice
        {
            glideRate += glideCoeff * (rate - glideRate);   // PITCH glide slew
            readPos += glideRate;
        }
    }

    if (! playbackActive)
        drainLatency(buffer, frame);   // a stop in this block: the chain plays it out

    publishPlayhead();
}

void LoopNestProcessor::auditionFrame(juce::AudioBuffer<float>& buffer, int frame, float* v, const OutGains& now,
                                      bool gmatch, bool bypass) noexcept
{
    const int numOutChannels = buffer.getNumChannels();
    // Pure source tap (post-input-gain, PRE-EQ) — the A/B reference: this is what
    // BYPASS plays and what GAIN-MATCH targets (EQ counts as part of "the FX").
    float src[2] = { v[0], v[1] };

    // Master EQ — pre-rack, on the whole signal before the dry/wet tap (so the
    // clean blend is EQ'd too). True bypass at open handles.
    character.processEq(v, numOutChannels);

    // Character chain (warp → … → reverb), dry/wet blended against the clean
    // (EQ'd, pre-rack) tap, both taps on the chain's fixed delay.
    float dry[2] = { v[0], v[1] };
    character.process(v, numOutChannels);
    character.delayTaps(src, dry, numOutChannels);

    // The gains this audio had at the voice's time, latency() frames ago (now's, unprepared).
    OutGains g = now;
    if (! gainLine.empty())
    {
        auto& slot = gainLine[(size_t) gainPos];
        g = slot;
        slot = now;
        if (++gainPos >= (int) gainLine.size()) gainPos = 0;
    }

    character.alignDry(dry, numOutChannels, g.mix);   // the dry tap in DRIVE's phase (no comb under MIX)
    float blended[2] = { 0.0f, 0.0f };
    for (int ch = 0; ch < numOutChannels; ++ch)
        blended[ch] = dry[ch] + g.mix * (v[ch] - dry[ch]);
    monitor.process(src, blended, numOutChannels, gmatch, bypass);

    if (lockTailIn > 0 && --lockTailIn == 0)   // a locked prime cut in here: the old frame decays out
    {
        lockTail[0] = lastOut[0];
        lockTail[1] = lastOut[1];
        lockTailGain = 1.0f;
    }
    for (int ch = 0; ch < numOutChannels; ++ch)
        buffer.setSample(ch, frame, blended[ch] * g.out * g.declick * g.mute);
    if (lockTailGain > 1.0e-4f)
    {
        for (int ch = 0; ch < numOutChannels; ++ch)
            buffer.addSample(ch, frame, lockTail[juce::jmin(ch, 1)] * lockTailGain);
        lockTailGain *= lockTailCoeff;
    }
    for (int ch = 0; ch < numOutChannels; ++ch)
        buffer.setSample(ch, frame, buffer.getSample(ch, frame) * g.swap);
    for (int ch = 0; ch < 2; ++ch)   // where a locked prime that cuts in starts its tail from
        lastOut[ch] = buffer.getSample(juce::jmin(ch, numOutChannels - 1), frame);
}

void LoopNestProcessor::drainLatency(juce::AudioBuffer<float>& buffer, int from) noexcept
{
    const bool bypass = bypassParam->load()    > 0.5f;
    const bool gmatch = gainMatchParam->load() > 0.5f;
    for (int frame = from; frame < buffer.getNumSamples() && latencyDrain > 0; ++frame, --latencyDrain)
    {
        float silence[2] = { 0.0f, 0.0f };
        auditionFrame(buffer, frame, silence, {}, gmatch, bypass);
    }
}

juce::AudioProcessorEditor* LoopNestProcessor::createEditor()
{
    return new LoopNestWebEditor(*this);   // stage 2: the WebView UI
}

// === Spin ===

void LoopNestProcessor::setSampleFolder(const juce::File& folder)
{
    {
        const juce::ScopedLock sl(stateLock);
        sampleFolder = folder;
        ++folderGen;
    }
    rescanFolder();
    rememberDefaultFolder(folder); // becomes the default for future fresh instances
    relinkMissingSample();         // B4: the bank may hold a restored set's missing sample
}

// One audio-extension list for the spin scan (rescanFolder), the drum extractor's
// gather and the rack's drag-in (BankDrop.h) — if they differ, the extractor copies
// in loops spin can never pick. decodeFile decodes all of these (CoreAudioFormat
// handles .mp3).
using loopnest::bankdrop::kAudioFileWildcard;

// App-level settings file, shared by every LoopNest instance (not per-project):
// ~/Library/Application Support/LoopNest/LoopNest.settings, v1's file too. v1 wrote
// only defaultFolder (the last bank picked); v2 reads it the same way and adds its
// own keys beside it.
static juce::PropertiesFile::Options settingsFileOptions()
{
    juce::PropertiesFile::Options o;
    o.applicationName     = appDirName();
    o.filenameSuffix      = "settings";
    o.folderName          = appDirName();
    o.osxLibrarySubFolder = "Application Support";
    return o;
}

juce::PropertiesFile* LoopNestProcessor::settings()
{
    appProperties.setStorageParameters(settingsFileOptions());
    return appProperties.getUserSettings();
}

void LoopNestProcessor::rememberDefaultFolder(const juce::File& folder)
{
    if (! folder.isDirectory())
        return;

    if (auto* s = settings())
    {
        // Every instance holds its own copy of the shared file: read what the others
        // saved first, or this save writes their changes back out (E-M2).
        s->reload();
        s->setValue("defaultFolder", folder.getFullPathName());
        // Recents: newest first, no duplicates, capped.
        juce::StringArray recent;
        recent.addLines(s->getValue("recentFolders", {}));
        recent.removeString(folder.getFullPathName());
        recent.insert(0, folder.getFullPathName());
        recent.removeEmptyStrings();
        while (recent.size() > kMaxRecentFolders)
            recent.remove(recent.size() - 1);
        s->setValue("recentFolders", recent.joinIntoString("\n"));
        s->saveIfNeeded();
    }
}

juce::File LoopNestProcessor::recallDefaultFolder()
{
    if (auto* s = settings())
        return juce::File(s->getValue("defaultFolder", {}));
    return {};
}

juce::Array<juce::File> LoopNestProcessor::getRecentFolders()
{
    juce::Array<juce::File> out;
    if (auto* s = settings())
    {
        juce::StringArray recent;
        recent.addLines(s->getValue("recentFolders", {}));
        for (const auto& path : recent)
            if (juce::File::isAbsolutePath(path) && juce::File(path).isDirectory())
                out.addIfNotAlreadyThere(juce::File(path));
    }
    return out;
}

juce::File LoopNestProcessor::getPrintsRoot() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                   .getChildFile(appDirName()).getChildFile("Prints");   // not v1's (appDirName)
   #if LOOPNEST_PRINTS_ENV
    // The UI harness and the tests only (never the plugin): prints go where the run says.
    if (const auto env = juce::SystemStats::getEnvironmentVariable("LOOPNEST_PRINTS_DIR", {}); env.isNotEmpty())
        dir = juce::File(env);
   #endif
    return dir;
}

juce::String LoopNestProcessor::legalBinName(const juce::String& name)
{
    // One folder level, visible, and safe as a filename prefix.
    auto n = juce::File::createLegalFileName(name.replaceCharacters("/:\\", "---")).trim();
    while (n.startsWithChar('.'))
        n = n.substring(1).trim();
    return n.substring(0, 64).trim();
}

juce::StringArray LoopNestProcessor::getPrintBins() const
{
    juce::StringArray bins;
    for (const auto& d : getPrintsRoot().findChildFiles(juce::File::findDirectories | juce::File::ignoreHiddenFiles, false))
        bins.add(d.getFileName());
    bins.sortNatural();
    return bins;
}

juce::File LoopNestProcessor::getBinFolder(const juce::String& name) const
{
    if (name.isEmpty() || name.containsChar('/') || name == "." || name == "..")
        return {};
    return juce::File(getPrintsRoot().getFullPathName() + juce::File::getSeparatorString() + name);
}

bool LoopNestProcessor::isPrintBinMissing() const
{
    const auto bin = getPrintBin();
    return bin.isNotEmpty() && ! getBinFolder(bin).isDirectory();
}

void LoopNestProcessor::reloadPrintBin()
{
    if (auto* s = settings())
    {
        s->reload();
        auto bin = s->getValue("printBin", {});
        if (bin.isNotEmpty() && getBinFolder(bin) == juce::File())
            bin.clear();   // not a name a bin can have: the root
        const juce::ScopedLock sl(binLock);
        printBin = bin;
    }
}

bool LoopNestProcessor::setPrintBin(const juce::String& name)
{
    // Any folder there, as the bins list shows it: legalBinName is for the folders
    // createPrintBin makes, and turned "#1 Kicks" into a name that isn't there (E-L4).
    if (name.isNotEmpty() && ! getBinFolder(name).isDirectory())
        return false;
    {
        const juce::ScopedLock sl(binLock);
        printBin = name;
    }
    if (auto* s = settings())
    {
        s->reload();   // another instance's newer settings stay (E-M2)
        s->setValue("printBin", name);
        s->saveIfNeeded();
    }
    return true;
}

bool LoopNestProcessor::createPrintBin(const juce::String& name)
{
    const auto bin = legalBinName(name);
    if (bin.isEmpty())
        return false;
    const auto dir = getBinFolder(bin);
    if (! dir.isDirectory() && ! dir.createDirectory().wasOk())
        return false;
    return setPrintBin(bin);
}

float LoopNestProcessor::recallEditorScale()
{
    if (auto* s = settings())
        return (float) s->getDoubleValue("editorScale", 0.0);
    return 0.0f;
}

void LoopNestProcessor::rememberEditorScale(float scale)
{
    if (auto* s = settings())
    {
        s->reload();   // another instance's newer settings stay (E-M2)
        s->setValue("editorScale", (double) scale);
        s->saveIfNeeded();
    }
}

void LoopNestProcessor::rescanFolder()
{
    // Scan into a local array, then swap under the lock — the directory walk can
    // be slow (big stash, network volume) and must not hold stateLock throughout.
    juce::File folder;
    int gen = 0, ticket = 0;
    {
        const juce::ScopedLock sl(stateLock);
        folder = sampleFolder;
        gen    = folderGen;
        ticket = ++scanTicket;
    }

    juce::Array<juce::File> found;
    if (folder.isDirectory())
    {
        // Recursive: a folder-of-folders (e.g. a loop stash with sub-packs) works.
        // Symlinks are skipped, files and folders (a stray link escapes the chosen tree,
        // or loops back into it: C-M7), and the scan
        // caps out so a runaway pick (root, a huge mount) can't hang the message
        // thread unbounded — the cap is a backstop, not a license to point it at /.
        constexpr int kMaxScan = 20000;
        for (const auto& entry : juce::RangedDirectoryIterator(
                 folder, true, kAudioFileWildcard, juce::File::findFiles, juce::File::FollowSymlinks::no))
        {
            if (entry.getFile().isSymbolicLink())
                continue;
            found.add(entry.getFile());
            if (found.size() >= kMaxScan)
                break;
        }
    }

    if (afterScanWalk) afterScanWalk();
    const juce::ScopedLock sl(stateLock);
    if (gen != folderGen)
        return;   // the selection moved on mid-walk; that selection's own scan publishes
    // Two scans of one folder (a drop's worker, a recent picked again) can finish out of
    // order: a scan that started earlier never replaces a later one's list, which may
    // hold a file copied in between.
    if (ticket < publishedTicket)
        return;
    publishedTicket = ticket;
    sampleFiles.swapWith(found);
    ++filesGen;
}

void LoopNestProcessor::setPresetIndex(int i)
{
    presetIndex.store(juce::isPositiveAndBelow(i, loopnest::kNumFactoryPresets) ? i : -1);
}

void LoopNestProcessor::spin(bool playWhenLoaded)
{
    // Pick under the lock — gate and pick must see the same sampleFiles (a
    // setState/rescan on another thread could empty it between the two).
    juce::File pick, avoid;
    {
        const juce::ScopedLock sl(stateLock);
        if (sampleFiles.isEmpty())
            return;
        {
            const juce::ScopedLock ql(loadQueueLock);
            avoid = pendingSample != juce::File() ? pendingSample : currentSample;
        }

        // Pick a random file, avoiding an immediate repeat when possible.
        do {
            pick = sampleFiles.getReference(random.nextInt(sampleFiles.size()));
        } while (sampleFiles.size() > 1 && pick == avoid);
    }
    spinLoad(pick, playWhenLoaded);
}

bool LoopNestProcessor::spinTo(const juce::File& file, bool playWhenLoaded)
{
    {
        const juce::ScopedLock sl(stateLock);
        if (! sampleFiles.contains(file))
            return false;
    }
    spinLoad(file, playWhenLoaded);
    return true;
}

void LoopNestProcessor::spinLoad(const juce::File& pick, bool playWhenLoaded)
{
    // A fresh spin plays in full until the user trims it. The trim reset rides
    // with the load, so it lands with the buffer swap; a vanished/corrupt pick
    // fails the decode and leaves the loaded sample and its trim untouched.
    LoadJob job;
    job.file  = pick;
    job.apply.start = 0.0f;
    job.apply.end   = 1.0f;
    job.parts = loopnest::Snapshot::kTrim;
    job.playOnInstall = playWhenLoaded;
    job.syncOn  = true;            // every new loop follows the host: SYNC on, a dragged tempo dropped (Andrew, 10-01)
    job.leaveFx = isFxMode();      // ...both as it lands (E-M1)
    job.fxGen   = fxGen.load();
    job.historyAt   = noteLeaving(pick);
    job.historyPick = true;
    requestLoad(std::move(job));
}

bool LoopNestProcessor::loadSampleFile(const juce::File& file, bool playWhenLoaded)
{
    {
        const juce::ScopedLock sl(stateLock);
        if (! sampleFiles.contains(file))
            return false;
    }
    LoadJob job;   // the same shape as a spin's: the file, with its trim reset
    job.file  = file;
    job.apply.start = 0.0f;
    job.apply.end   = 1.0f;
    job.parts = loopnest::Snapshot::kTrim;
    job.playOnInstall = playWhenLoaded;
    job.syncOn  = true;            // as a spin does
    job.leaveFx = isFxMode();
    job.fxGen   = fxGen.load();
    requestLoad(std::move(job));   // no history entry: only SPIN records one (Andrew, 10-11)
    return true;
}

// === Spin history ===

LoopNestProcessor::HistoryEntry LoopNestProcessor::captureEntry() const
{
    HistoryEntry e;
    e.sound = capture();
    BpmSource src = BpmSource::none;
    e.bpm = getNativeBpm(src);
    e.bpmSource = src;
    e.barOrigin = getBarOrigin();
    return e;
}

void LoopNestProcessor::applyHistoryReset()
{
    if (historyResetPending.exchange(false))
    {
        history.clear();
        historyPos = -1;
    }
}

int LoopNestProcessor::noteLeaving(const juce::File& next)
{
    applyHistoryReset();
    // The sound being left, with the knob moves made on it: the current entry's own
    // loop refreshes that entry; any other (a rack pick, which records nothing itself)
    // becomes an entry of its own. While a load is still landing, its entry already
    // says what it will be: keep that.
    if (historyPos >= 0)
        history.resize((size_t) historyPos + 1);   // a new branch: the forward entries go
    if (! isLoadPending() || historyPos < 0)
    {
        auto now = captureEntry();
        if (now.sound.file != juce::File())
        {
            if (historyPos >= 0 && history[(size_t) historyPos].sound.file == now.sound.file)
                history[(size_t) historyPos] = now;
            else
            {
                history.push_back(now);
                historyPos = (int) history.size() - 1;
            }
        }
    }
    // The new loop as it will land: these knobs, its file, the trim open, its own tempo.
    HistoryEntry e;
    e.sound = capture();
    e.sound.file  = next;
    e.sound.start = 0.0f;
    e.sound.end   = 1.0f;
    history.push_back(e);
    while ((int) history.size() > kHistoryBack + 1)
        history.erase(history.begin());
    historyPos = (int) history.size() - 1;
    return historyPos;
}

int LoopNestProcessor::historyTarget(int delta) const
{
    if (historyResetPending.load() || historyPos < 0)
        return -1;
    for (int i = historyPos + delta; i >= 0 && i < (int) history.size(); i += delta)
        if (history[(size_t) i].sound.file.existsAsFile())
            return i;
    return -1;
}

bool LoopNestProcessor::stepHistory(int delta, bool playWhenLoaded)
{
    applyHistoryReset();
    const int to = historyTarget(delta);   // a missing file's entry is stepped over
    if (to < 0)
        return false;
    // Keep the knob moves made on this entry's loop since. A loop picked from the rack
    // over it records nothing (only SPIN does): the entry stays as it was.
    if (! isLoadPending() && history[(size_t) historyPos].sound.file == getCurrentSampleFile())
        history[(size_t) historyPos] = captureEntry();
    const int from = historyPos;
    historyPos = to;
    const auto& e = history[(size_t) to];

    // The whole sound, through the loader even for the loaded file, so it lands like
    // a spin: async, and on the bar line under the lock.
    const juce::ScopedLock cl(commitLock);
    LoadJob job;
    job.file  = e.sound.file;
    job.apply = e.sound;
    job.parts = loopnest::Snapshot::kAll & ~loopnest::Snapshot::kFile;
    job.bpm   = e.bpmSource == BpmSource::none ? 0.0 : e.bpm;
    job.bpmSource = e.bpmSource;
    job.barOrigin = e.barOrigin;
    job.playOnInstall = playWhenLoaded;
    job.historyAt   = to;
    job.historyFrom = from;
    job.syncOn  = true;            // as a spin does
    job.leaveFx = isFxMode();
    job.fxGen   = fxGen.load();
    requestLoad(std::move(job));
    return true;
}

void LoopNestProcessor::historyLoadFailed(const LoadJob& job)
{
    // The load never landed, so the sound is still the one it left: point back at that,
    // and drop the entry that won't decode (no dead step, no duplicate).
    if (job.historyAt < 0 || job.historyAt != historyPos || job.historyAt >= (int) history.size()
        || history[(size_t) job.historyAt].sound.file != job.file)
        return;
    history.erase(history.begin() + job.historyAt);
    if (job.historyPick)
        historyPos = job.historyAt - 1;
    else
        historyPos = job.historyFrom > job.historyAt ? job.historyFrom - 1 : job.historyFrom;
}

// === v2 stage 0: snapshot / restore ===

loopnest::Snapshot LoopNestProcessor::capture() const
{
    // One commit's worth: an install can't swap the file between reading it and
    // reading the params (see commitLock).
    const juce::ScopedLock cl(commitLock);
    loopnest::Snapshot snap;
    {
        const juce::ScopedLock sl(stateLock);
        snap.file = currentSample;
    }
    snap.start     = startParam->load();
    snap.end       = endParam->load();
    snap.pitch     = pitchParam->load();
    snap.input     = inputParam->load();
    snap.output    = outputParam->load();
    snap.mix       = mixParam->load();
    snap.character = readCharacter();
    return snap;
}

void LoopNestProcessor::forEachPart(const loopnest::Snapshot& snap, int parts,
                                    const std::function<void(const char*, float)>& add)
{
    using S = loopnest::Snapshot;
    if (parts & S::kTrim)  { add("start", snap.start); add("end", snap.end); }
    if (parts & S::kPitch)   add("pitch", snap.pitch);
    if (parts & S::kLevels) { add("input", snap.input); add("output", snap.output); add("mix", snap.mix); }
    if (parts & S::kCharacter)
    {
        for (int i = 0; i < loopnest::CharacterParams::kNum; ++i)
            add(loopnest::CharacterParams::fields()[(size_t) i].id, snap.character[i]);
        for (int i = 0; i < loopnest::CharacterParams::kNumStages; ++i)
            add(loopnest::CharacterParams::powerIds()[(size_t) i], snap.character.on[(size_t) i] ? 1.0f : 0.0f);
    }
}

std::vector<LoopNestProcessor::ParamEdit>
LoopNestProcessor::editsFor(const loopnest::Snapshot& snap, int parts) const
{
    std::vector<ParamEdit> edits;
    forEachPart(snap, parts, [&] (const char* id, float natural)
    {
        if (auto* p = apvts.getParameter(id))
            edits.emplace_back(p, p->convertTo0to1(natural));
    });
    return edits;
}

void LoopNestProcessor::audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int index)
{
    if (juce::isPositiveAndBelow(index, (int) gestureDepth.size()))
        ++gestureDepth[(size_t) index];
}

void LoopNestProcessor::endOpenGestures()
{
    // Each end comes back through audioProcessorParameterChangeGestureEnd, so read the
    // depth first.
    const auto& params = getParameters();
    for (size_t i = 0; i < gestureDepth.size(); ++i)
        for (int n = gestureDepth[i]; n > 0; --n)
            params[(int) i]->endChangeGesture();
}

void LoopNestProcessor::audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int index)
{
    if (juce::isPositiveAndBelow(index, (int) gestureDepth.size()) && gestureDepth[(size_t) index] > 0)
        --gestureDepth[(size_t) index];
    // A trim handle let go: the tempo follows now, never during the drag.
    if ((index == startIndex || index == endIndex) && ! trimGestureOpen())
        refitTempoToTrim();
}

void LoopNestProcessor::setIComponentHandler(Steinberg::FUnknown* handler)
{
    // IComponentHandler2 IID (0xF040B4B3, 0xA36045EC, 0xABCDC045, 0xB4D5A2CC) in the
    // non-COM byte order INLINE_UID uses on macOS.
    static const char kIID[16] = { (char) 0xF0, (char) 0x40, (char) 0xB4, (char) 0xB3,
                                   (char) 0xA3, (char) 0x60, (char) 0x45, (char) 0xEC,
                                   (char) 0xAB, (char) 0xCD, (char) 0xC0, (char) 0x45,
                                   (char) 0xB4, (char) 0xD5, (char) 0xA2, (char) 0xCC };
    const juce::ScopedLock cl(commitLock);
    if (hostGroupEdit != nullptr) { hostGroupEdit->release(); hostGroupEdit = nullptr; }
    if (handler == nullptr) return;
    void* obj = nullptr;
    // FUnknown's queryInterface is its first virtual; view the handler through the
    // same vtable to call it.
    if (reinterpret_cast<HostGroupEdit*>(handler)->queryInterface(kIID, &obj) == 0 && obj != nullptr)
        hostGroupEdit = static_cast<HostGroupEdit*>(obj);   // queryInterface addRef'd it
}

void LoopNestProcessor::setParamsAsOneStep(const std::vector<ParamEdit>& edits)
{
    // Begin-all, set-all, end-all: the host sees one overlapping gesture across
    // every changed param (a gesture per param in turn is 20+ undo entries).
    const juce::ScopedLock cl(commitLock);
    std::vector<ParamEdit> changed;
    changed.reserve(edits.size());
    for (const auto& e : edits)
    {
        if (e.first == nullptr || juce::approximatelyEqual(e.first->getValue(), e.second))
            continue;
        const auto idx = e.first->getParameterIndex();
        if (juce::isPositiveAndBelow(idx, (int) gestureDepth.size()) && gestureDepth[(size_t) idx] > 0)
            continue;   // mid-drag: the user's gesture owns this param
        changed.push_back(e);
    }

    // Overlapping gestures alone do NOT group in Live (measured 09-29, AU: a preset
    // undid one knob per Cmd-Z). VST3 also brackets them in a host group edit, for
    // hosts that implement it (untested in any); Live 09-29 returns
    // kNotImplemented (3) from both calls, so in Live this changes nothing.
    if (changed.empty()) return;
    if (hostGroupEdit != nullptr) hostGroupEdit->startGroupEdit();
    for (const auto& e : changed) e.first->beginChangeGesture();
    for (const auto& e : changed) e.first->setValueNotifyingHost(e.second);
    for (const auto& e : changed) e.first->endChangeGesture();
    if (hostGroupEdit != nullptr) hostGroupEdit->finishGroupEdit();
}

bool LoopNestProcessor::restore(const loopnest::Snapshot& snap, int parts)
{
    using S = loopnest::Snapshot;
    const juce::ScopedLock cl(commitLock);
    if (parts & S::kFile)
    {
        if (snap.file == juce::File())
        {
            clearSample();   // the valid empty-sample state (a capture before any spin)
        }
        else if (snap.file != getCurrentSampleFile())
        {
            if (! snap.file.existsAsFile())
                return false;
            LoadJob job;
            job.file  = snap.file;
            job.apply = snap;
            job.parts = parts & ~S::kFile;
            requestLoad(std::move(job));
            return true;
        }
        else
        {
            // The loaded file: still the newest request, so an older load in flight
            // must not land over it later.
            const juce::ScopedLock sl(stateLock);
            cancelLoadsLocked();
        }
    }
    else
    {
        // A partial edit while a load decodes: those parts are the user's now.
        const juce::ScopedLock ql(loadQueueLock);
        pendingParts &= ~parts;
    }
    setParamsAsOneStep(editsFor(snap, parts & ~S::kFile));
    return true;
}

void LoopNestProcessor::resetAllParams()
{
    const juce::ScopedLock cl(commitLock);
    {
        const juce::ScopedLock ql(loadQueueLock);
        pendingParts = 0;   // CLEAR wins over a pending load's values
    }
    std::vector<ParamEdit> edits;
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*>(p))
            edits.emplace_back(rp, rp->getDefaultValue());
    setParamsAsOneStep(edits);
}

// ===================== Drum-loop curation ("(b)") =======================
// 1:1 port of tools/extract_drum_loops.sh (the locked spec). KEEP a file iff:
//   filename has a drum-family word  AND  filename has no musical KEY tag
//   AND nothing on its path is a single-element/one-shot marker
//   AND its duration is in [2, 30] s.
// Filename labels are the highest-signal feature in this library, so this is
// label-driven — only survivors are decoded (for duration), so it stays fast.

namespace
{
    // The extractor's temp copy (renamed on success). LoopNest's own suffix, so the
    // sweep of leftovers never takes a user's file (B7).
    constexpr const char* kExtractTempSuffix = ".loopnest-part";

    // grep -iE equivalents (ECMAScript, case-insensitive). Verbatim from the spec
    // so the verified 221/5634 result is reproduced — do NOT add word boundaries.
    const std::regex kPositiveRe { "drum|drums|drumloop|break|breaks|groove|beat|beats",
                                   std::regex::icase };
    const std::regex kKeyRe      { "[A-G](#|b)? ?(maj|min|major|minor)\\b",
                                   std::regex::icase };
    const std::regex kElemRe     { "hat|hi-?hat|snare|kick|clap|perc|percussion|shaker|ride|"
                                   "crash|tom|conga|bongo|rim|cymbal|cowbell|tamb",
                                   std::regex::icase };
    const std::regex kOneShotRe  { "one.?shot", std::regex::icase };
    // Native-tempo tag in a loop's filename ("…_92bpm", "92 BPM").
    const std::regex kBpmRe      { "(\\d{2,3})\\s*bpm", std::regex::icase };

    bool rx(const juce::String& s, const std::regex& re)
    {
        const auto utf8 = s.toStdString();
        return std::regex_search(utf8, re);
    }

}

juce::String LoopNestProcessor::getExtractStatus() const
{
    const juce::ScopedLock sl(extractRun->statusLock);
    return extractRun->status;
}

void LoopNestProcessor::extractDrumLoops(juce::File src, juce::File dest)
{
    if (extractRun->extracting.load() || ! src.isDirectory())
        return;

    // Dest must be a usable directory (the save-mode chooser permits picking an
    // existing FILE, and createDirectory can fail on a read-only volume) — never
    // start a scan whose output, and subsequent folder adoption, can't land.
    if (dest.existsAsFile() || (! dest.exists() && ! dest.createDirectory()))
    {
        extractRun->setStatus("aborted \xe2\x80\x94 destination isn't a usable folder");
        return;
    }

    // Guard: the destination must be outside the source tree. If dest == src or an
    // ancestor of it, the scan prunes everything under dest and finds nothing — and
    // it would also tangle the curated output with the source library.
    if (dest == src || src.isAChildOf(dest))
    {
        extractRun->setStatus("aborted — destination can't be the source folder");
        return;
    }

    // A fresh run: a worker of an earlier one keeps its own.
    auto run = std::make_shared<ExtractRun>();
    run->extracting.store(true);
    run->setStatus("scanning…");
    run->exited.reset();
    extractRun = run;
    // The adoption reaches the message thread with the processor's lifetime guard, so
    // the worker never needs `this` alive.
    auto adopt = [this, alive = aliveFlag]
    {
        juce::MessageManager::callAsync([this, alive]
        {
            if (*alive)
                finishExtractionIfReady();
        });
    };
    if (! juce::Thread::launch([run, src, dest, adopt] { runExtraction(run, src, dest, adopt); }))
    {
        run->setStatus("aborted \xe2\x80\x94 couldn't start");
        run->extracting.store(false);
        run->exited.signal();
    }
}

void LoopNestProcessor::runExtraction(const std::shared_ptr<ExtractRun>& run, juce::File src, juce::File dest,
                                      const std::function<void()>& adopt)
{
    const juce::ScopeGuard exited { [&run] { run->exited.signal(); } };   // last: the destructor waits on it
    const auto outDir = dest;

    // The worker decodes durations off its own format manager (never share one
    // across threads; the sample loader has its own too).
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    auto bail = [&run] { return run->cancel.load(); };

    // --- Gather candidates first (excluding our own output + AppleDouble) -----
    juce::Array<juce::File> candidates;
    for (const auto& entry : juce::RangedDirectoryIterator(
             src, true, kAudioFileWildcard, juce::File::findFiles, juce::File::FollowSymlinks::no))   // linked folders too (C-M7)
    {
        if (bail()) { run->setStatus("cancelled"); run->extracting.store(false); return; }
        const auto f = entry.getFile();
        if (f.isSymbolicLink())              continue;   // a link could escape the tree
        if (f.isAChildOf(outDir))            continue;   // don't re-ingest our output
        if (f.getFileName().startsWith("._")) continue;  // AppleDouble sidecar
        candidates.add(f);
    }

    // --- Additive: keep existing curated loops, only add novel ones ---------
    outDir.createDirectory();   // no-op if it already exists
    if (! outDir.isDirectory())
    {
        run->setStatus("aborted \xe2\x80\x94 can't create the destination folder");
        run->extracting.store(false);
        return;
    }

    // Sweep any half-written temp copies from an interrupted prior run (the copy
    // below goes temp-name -> rename so a kill never leaves a truncated .wav that the
    // dedup would then trust forever). Only LoopNest's own suffix: the destination
    // may be any folder, and a plain .part there is someone's download (B7).
    for (const auto& stale : outDir.findChildFiles(juce::File::findFiles, false, juce::String("*") + kExtractTempSuffix))
        stale.deleteFile();

    const int total = candidates.size();
    int kept = 0, skipped = 0;
    for (int i = 0; i < total; ++i)
    {
        if (bail()) break;
        const auto f    = candidates.getReference(i);
        const auto base = f.getFileName();
        const auto rel  = f.getRelativePathFrom(src);

        // Label gates (cheap) before the duration decode (costly).
        const bool keep =
               rx(base, kPositiveRe)        // must name a drum family
            && ! rx(base, kKeyRe)           // a musical key tag → melodic, reject
            && ! rx(rel,  kOneShotRe)       // one-shot folder on the path, reject
            && ! rx(rel,  kElemRe);         // single-element marker on the path, reject

        if (keep)
        {
            std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(f));
            if (reader != nullptr && reader->sampleRate > 0.0)
            {
                const double secs = (double) reader->lengthInSamples / reader->sampleRate;
                reader.reset();
                if (secs >= 2.0 && secs <= 30.0)
                {
                    // Prefix nested files with their FULL relative dir path so
                    // same-named loops from Pack/Vol1 and Pack/Vol2 don't collide
                    // in the flat output. Files sitting directly in the source get
                    // their bare name (no "(root)__").
                    const juce::String dirPart = rel.containsChar('/')
                        ? rel.upToLastOccurrenceOf("/", false, false) : juce::String();
                    const juce::String prefix = dirPart.isNotEmpty()
                        ? dirPart.replaceCharacter('/', '_') + "__" : juce::String();

                    // Additive: a loop already curated (same output name) is left
                    // alone — only novel loops get copied in. Copy lands on a temp
                    // name first, renamed on success, so a hard thread-kill mid-copy
                    // can't leave a truncated file the dedup would trust forever.
                    auto destFile = outDir.getChildFile("./" + prefix + base);   // "./": a leading '~' isn't home
                    if (destFile.existsAsFile())
                        ++skipped;
                    else
                    {
                        auto tmpFile = destFile.getSiblingFile(destFile.getFileName() + kExtractTempSuffix);
                        if (f.copyFileTo(tmpFile) && tmpFile.moveFileTo(destFile))
                        {
                            ++kept;
                            run->kept.store(kept);
                        }
                        else
                        {
                            tmpFile.deleteFile();
                        }
                    }
                }
            }
        }

        run->progress.store((float) (i + 1) / (float) juce::jmax(1, total));
        if ((i & 31) == 0)
            run->setStatus("scanning " + juce::String(i + 1) + "/" + juce::String(total)
                             + "  ·  " + juce::String(kept) + " kept");
    }

    const bool cancelled = bail();
    run->setStatus(cancelled ? "cancelled"
                               : juce::String(kept) + " added"
                                 + (skipped > 0 ? "  \xc2\xb7  " + juce::String(skipped)
                                                    + " already present"
                                                : juce::String()));

    // Hand the curated folder to the message thread to adopt. The worker never
    // touches sampleFiles itself; finishExtractionIfReady() does the swap. The
    // editor timer polls it AND we callAsync it (lifetime-guarded) so finishing
    // with the window closed still adopts — otherwise a save-and-close mid-scan
    // strands the adoption until the editor reopens.
    // Only adopt when the folder now holds usable loops (newly kept, or already
    // curated from a prior run). A zero-result run must not swap the user's stash
    // for an empty folder — setSampleFolder would also save it as the default.
    if (! cancelled && kept + skipped > 0)
    {
        run->curated = outDir;
        run->adoptPending.store(true);
        adopt();
    }
    else
    {
        run->extracting.store(false);
    }
}

void LoopNestProcessor::cancelExtraction()
{
    const auto run = extractRun;
    if (! run->extracting.load())
        return;
    run->cancel.store(true);
    // The worker may already have finished and queued its adoption; a cancel that
    // lands in that window still wins — drop the adoption and clear the busy flag.
    if (run->adoptPending.exchange(false))
    {
        run->setStatus("cancelled");
        run->extracting.store(false);
    }
}

void LoopNestProcessor::finishExtractionIfReady()
{
    const auto run = extractRun;
    if (! run->adoptPending.exchange(false))
        return;

    setSampleFolder(run->curated);  // rescan + remember + reset spins (message thread)
    run->extracting.store(false);
}

bool LoopNestProcessor::decodeFile(juce::AudioFormatManager& fm, const juce::File& file,
                                   DecodedLoad& out)
{
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr)
        return false;

    // Validate before allocating: a corrupt/huge header would otherwise wrap the
    // (int) cast negative or trigger a multi-GB allocation (a bad_alloc takes
    // Ableton down with us). 5 min @ 192 kHz stereo ≈ 460 MB — far beyond any
    // loop, cheap insurance.
    constexpr juce::int64 kMaxSeconds = 300;
    const auto maxFrames = (juce::int64) (reader->sampleRate * (double) kMaxSeconds);
    if (reader->numChannels < 1 || reader->numChannels > 64
        || reader->sampleRate <= 0.0
        || reader->lengthInSamples <= 0 || reader->lengthInSamples > maxFrames)
        return false;

    // WAV/AIFF readers take the length from the header and zero-fill a short read
    // (returning true), so a truncated file would install as padded audio. Their
    // PCM/float payload can't be smaller than frames x channels x sample bytes, and
    // the file holds that payload plus its header: a smaller file is truncated.
    const auto& fmt = reader->getFormatName();
    if ((fmt == "WAV file" || fmt == "AIFF file") && reader->bitsPerSample > 0
        && file.getSize() < reader->lengthInSamples * (juce::int64) reader->numChannels
                                                    * (juce::int64) (reader->bitsPerSample / 8))
        return false;

    try
    {
        out.buffer.setSize((int) reader->numChannels, (int) reader->lengthInSamples);
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    // A failed read (formats that report one) is a failed load, same as the checks
    // above; WAV/AIFF truncation is caught by the size check before the allocation.
    if (! reader->read(&out.buffer, 0, (int) reader->lengthInSamples, 0, true, true))
        return false;
    out.sampleRate = reader->sampleRate;
    // A float file can carry NaN or inf: as silence, or one would ring in the echo and
    // reverb state for good, BYPASS included (C-M10).
    for (int ch = 0; ch < out.buffer.getNumChannels(); ++ch)
        for (auto* x = out.buffer.getWritePointer(ch), * end = x + out.buffer.getNumSamples(); x != end; ++x)
            if (! std::isfinite(*x))
                *x = 0.0f;

    // Native-tempo tag from the filename (kBpmRe). '_' and '-' read as
    // spaces first so pack-style "Drums_90_BPM" tags match the \s* too.
    out.tagBpm = 0.0;
    {
        const auto name = file.getFileNameWithoutExtension()
                              .replaceCharacters("_-", "  ").toStdString();
        std::smatch m;
        if (std::regex_search(name, m, kBpmRe))
            out.tagBpm = juce::String(m[1].str()).getDoubleValue();
    }
    return true;
}

void LoopNestProcessor::requestLoad(LoadJob job)
{
    {
        // gen bumps under stateLock (see loadGen), the queue under its own lock.
        const juce::ScopedLock sl(stateLock);
        const juce::ScopedLock ql(loadQueueLock);
        job.gen = ++loadGen;
        pendingSample = job.file;
        pendingApply  = job.apply;
        pendingBpm    = job.bpm;
        pendingBarOrigin = job.barOrigin;
        pendingBpmSource = job.bpmSource;
        pendingParts  = job.parts;
        // A host restore's mute moves to whatever supersedes it, so its params never
        // play over the old sample while the newer load decodes.
        if (job.mute || muteUntilGen.load() != 0)
            muteUntilGen.store(job.gen);
        queuedJob = std::move(job);   // latest wins; an older queued job is dropped
    }
    loadDone.reset();
    loaderWake.signal();
}

void LoopNestProcessor::runLoader()
{
    // Own format manager: never share one with another thread.
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();   // WAV + AIFF (+ CoreAudio: mp3 etc. on macOS)

    while (! juce::Thread::currentThreadShouldExit())
    {
        loaderWake.wait(-1);
        if (juce::Thread::currentThreadShouldExit())
            break;

        std::optional<LoadJob> job;
        {
            const juce::ScopedLock ql(loadQueueLock);
            job.swap(queuedJob);
        }
        if (! job.has_value())
            continue;

        auto decoded = std::make_unique<DecodedLoad>();
        decoded->job = std::move(*job);
        decoded->ok  = decodeFile(fm, decoded->job.file, *decoded);
        if (juce::Thread::currentThreadShouldExit())
            break;                    // the processor is going away: publish nothing

        {
            const juce::ScopedLock ql(loadQueueLock);
            if (decoded->job.gen != loadGen.load())
                continue;             // superseded mid-decode; the newer job is queued
            finishedLoad = std::move(decoded);
        }
        loadDone.signal();
        triggerAsyncUpdate();
    }
}

bool LoopNestProcessor::installFinishedLoad()
{
    // The whole install is one commit (see commitLock): a host setState can't claim
    // a newer gen or replace params between this gen check and this param apply.
    const juce::ScopedLock cl(commitLock);
    std::unique_ptr<DecodedLoad> d;
    {
        const juce::ScopedLock ql(loadQueueLock);
        d = std::move(finishedLoad);
    }
    if (d == nullptr)
        return false;

    const auto gen = d->job.gen;
    // Mute across the swap AND the param apply, so the audio thread sees either
    // the old file under the old trim, silence, or the new file under the new trim.
    ++restoring;
    const juce::ScopeGuard unmute { [this] { --restoring; } };
    loopnest::Snapshot apply;
    int parts = 0;
    {
        // Install buffer + metadata as one step: stateLock outside, sampleLock inside
        // (the only nesting order used anywhere), so no reader ever sees file B's audio
        // under file A's name/path/hatch filename.
        const juce::ScopedLock sl(stateLock);
        if (gen != loadGen.load())
            return false;             // a newer request (or a restore's cancel) owns the slot
        if (! d->ok && d->job.clearOnFail)
        {
            // The restored set's sample won't decode: same as a missing one.
            clearSample();
            return true;
        }
        if (d->ok)
        {
            // The tempo for this file. A restore brings its saved one; a state from
            // before it was saved reads its saved trim as bars (what those builds
            // played); anything else is the tag, else the whole file's length, then
            // (below, once the trim is in) the trim it lands with. Worked out before the
            // swap, so the install takes sampleLock once: under the bar-grid lock every
            // extra take is a chance for the audio thread's try-lock to drop a block.
            BpmSource bpmSrc = BpmSource::none;
            double bpm = 0.0;
            if (d->job.bpm > 0.0)
            {
                bpm    = juce::jlimit(kMinLoopBpm, kMaxLoopBpm, d->job.bpm);
                bpmSrc = d->job.bpmSource == BpmSource::none ? BpmSource::user : d->job.bpmSource;
            }
            else
            {
                const int frames = d->buffer.getNumSamples();
                int startFrame = 0, endFrame = frames;
                if (d->job.bpmFromRegion)
                    regionFrames(frames, startFrame, endFrame);
                bpm = inferNativeBpm(d->tagBpm, (double) (endFrame - startFrame) / d->sampleRate,
                                     lastHostBpm.load(), bpmSrc);
            }
            int partsNow = 0;
            {
                const juce::ScopedLock ql(loadQueueLock);
                partsNow = pendingParts;
            }
            juce::AudioBuffer<float> retired;   // frees after the lock, off the audio thread
            {
                const juce::SpinLock::ScopedLockType lock(sampleLock);
                if (lockPlaying || swapQueued)
                {
                    // Knobs beyond the trim (a history step) wait for the line with the loop.
                    swapHoldsParams = (swapQueued && swapHoldsParams)
                                   || (partsNow & ~loopnest::Snapshot::kTrim) != 0;
                    // Bar-grid lock: the playing loop moves aside and plays on, its loop
                    // frozen, to the next bar line, where the audio thread swaps this one
                    // in (renderAudition). A loop queued before this one never played.
                    if (! swapQueued)
                        std::swap(retiringBuffer, sampleBuffer);
                    swapQueued = true;
                }
                else
                {
                    std::swap(retired, retiringBuffer);
                    playbackActive = false;
                    readPos = 0.0;
                    character.reset();
                }
                std::swap(sampleBuffer, d->buffer);   // old buffer frees with `d`, off the audio thread
                sampleFileSampleRate = d->sampleRate;
                nameBpm = d->tagBpm;
                barOrigin.store(juce::jlimit(0.0, 1.0, d->job.barOrigin));
                loopBpm = bpm;
                loopBpmSource = bpmSrc;
            }
            currentSample = d->job.file;
            currentSampleName = d->job.file.getFileName();
            lastRender = juce::File();   // a new sample invalidates the old render
            missingSample = {};          // a real load replaces a missing sample's reference (B4)
        }
        // The values as they stand now: a partial restore since the request may
        // have taken some parts back.
        const juce::ScopedLock ql(loadQueueLock);
        apply = pendingApply;
        parts = pendingParts;
    }

    if (! d->ok)
        historyLoadFailed(d->job);   // message thread, as the history is

    // Outside stateLock: gesture callbacks reach the host. pendingSample stays set
    // until the params are in, so a getState meanwhile still overlays them.
    if (d->ok)
    {
        setParamsAsOneStep(editsFor(apply, parts));
        if (d->job.bpm <= 0.0)
        {
            // A snapshot that lands with its own trim: the following tempo reads that.
            const juce::ScopedLock sl(stateLock);
            if (gen == loadGen.load() && (loopBpmSource == BpmSource::length || loopBpmSource == BpmSource::none))
                fitRegionLocked();
        }
        trimMovedAt.store(0);
        // A spin, pick or step leaves LIVE and a free tempo only now, with the new loop
        // in: flipped at the request, the previous loop played first (E-M1). Not when LIVE
        // was chosen again since (a HATCH drag while it decoded): the clip just dropped
        // keeps the track, and the audition stays off.
        const bool leaveFx = d->job.leaveFx && fxGen.load() == d->job.fxGen;
        if (leaveFx)
            setFxMode(false);
        if (d->job.syncOn)
            setSyncEnabled(true);
        if (d->job.playOnInstall && (leaveFx || ! d->job.leaveFx))
            playRequested.store(true);
    }
    {
        // Publish only if still the newest (a host callback above could have
        // re-entered with a request); never moves landedGen backwards.
        const juce::ScopedLock sl(stateLock);
        const juce::ScopedLock ql(loadQueueLock);
        if (gen == loadGen.load())
        {
            pendingSample = juce::File();
            pendingParts = 0;
            landedGen.store(gen);
            muteUntilGen.store(0);
        }
    }
    return true;
}

bool LoopNestProcessor::finishPendingLoad(int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax(0, timeoutMs);
    while (isLoadPending())
    {
        installFinishedLoad();
        if (! isLoadPending())
            break;
        const auto now = juce::Time::getMillisecondCounter();
        if (now >= deadline)
            return false;
        loadDone.wait((int) (deadline - now));
        loadDone.reset();
    }
    return true;
}

void LoopNestProcessor::cancelLoadsLocked()
{
    const juce::ScopedLock ql(loadQueueLock);
    const auto gen = ++loadGen;   // an in-flight decode or install sees a stale gen and drops
    queuedJob.reset();
    finishedLoad.reset();
    pendingSample = juce::File();
    pendingParts = 0;
    landedGen.store(gen);
    muteUntilGen.store(0);
}

void LoopNestProcessor::clearSample()
{
    const juce::ScopedLock sl(stateLock);
    cancelLoadsLocked();
    juce::AudioBuffer<float> retired;   // frees after the lock, off the audio thread
    {
        const juce::SpinLock::ScopedLockType lock(sampleLock);
        sampleBuffer.setSize(0, 0);
        nameBpm = 0.0;
        loopBpm = 0.0;
        loopBpmSource = BpmSource::none;
        barOrigin.store(0.0);
        playbackActive = false;
        readPos = 0.0;
        swapQueued = false;
        std::swap(retired, retiringBuffer);
    }
    currentSample = juce::File();
    currentSampleName.clear();
    lastRender = juce::File();
}

// === Cash Out: render the shaped trimmed region to a .wav ===

juce::File LoopNestProcessor::renderLoop()
{
    // The print's chain runs here on the message thread, so it needs the audio
    // path's flush-to-zero too: tails decaying toward silence would otherwise run
    // its filters and reverb on denormals.
    juce::ScopedNoDenormals noDenormals;
    reloadPrintBin();   // the bin another instance picked since is where this prints (E-M2)

    // Snapshot what we need under the lock, then render without holding it.
    // stateLock too, so the audio, its filename and the signature all describe
    // the same sample even if a load lands mid-render.
    juce::AudioBuffer<float> src;
    double srcSR = 44100.0;
    int startFrame = 0, endFrame = 0;
    juce::File sampleFile;
    juce::uint64 sig = 0;
    // Dry/sync snapshot. Toggles are message-thread writes, same thread as
    // this render, so the signature below sees the same values.
    const bool dryPrint = dryHatch.load();
    const bool sync     = isStretching();   // SYNC or a free tempo: the stretched print
    double tempoRatio = 1.0;
    {
        // stateLock alone: every reassignment of sampleBuffer (an install, clearSample)
        // and of the tempo/rate beside it holds stateLock, and the audio thread only
        // reads them, so this copy (a malloc and the whole file) never holds sampleLock.
        // Under the bar-grid lock the audition plays on through a print: a held
        // sampleLock would cost it a block of silence (the audio thread's try-lock).
        const juce::ScopedLock sl(stateLock);
        if (sampleBuffer.getNumSamples() <= 0)
            return {};
        src.makeCopyOf(sampleBuffer);
        srcSR = sampleFileSampleRate;
        // Andrew 10-13 (late): locked or not, the print is the trim exactly, from the left handle.
        regionFrames(src.getNumSamples(), startFrame, endFrame);
        sampleFile = currentSample;
        if (sync)
            tempoRatio = syncRatio(loopBpm, getTargetBpm());
        sig = renderSignature();
    }

    const int srcChannels = src.getNumChannels();

    // Always render stereo. The audition runs on the host's stereo bus with mono
    // sources duplicated into both channels, and the chain's WIDTH/CENTER stage and
    // stereo reverb only engage at 2 channels — rendering mono would silently drop
    // them and break print-what-you-hear.
    const int outChannels = 2;

    // Render at the source sample rate with varispeed pitch; Ableton's warp
    // realigns tempo on drop. So the readout rate is just the pitch ratio.
    // SYNC on: pitch-preserving stretch by the audition's tempo ratio
    // (hostBPM/nativeBPM at the last host tempo seen) with PITCH as a transpose,
    // so the print is already at session tempo and a bar-length region lands on
    // the grid as a whole number of bars.
    const double semis   = effectiveSemis(pitchParam->load());
    const double rate    = sync ? tempoRatio
                                : std::pow(2.0, semis / 12.0);
    const float  inGain  = juce::Decibels::decibelsToGain(inputParam->load(),  -60.0f);
    const float  outGain = juce::Decibels::decibelsToGain(outputParam->load(), -60.0f);

    // Character chain at the source rate — identical processing to the audition.
    CharacterChain renderChar;
    renderChar.prepare(srcSR);
    pushChainParams(renderChar);
    const float mix01 = juce::jlimit(0.0f, 1.0f, mixParam->load() * 0.01f);
    // GAIN MATCH bakes into a wet print: the same stage the live path runs, fresh and
    // carried through the priming passes like the chain. BYPASS never prints.
    loopnest::MonitorStage renderMonitor;
    renderMonitor.prepare(srcSR);
    const bool gmatch = gainMatchParam->load() > 0.5f;

    // The params above are read one by one, outside the lock. If any moved since the
    // signature was taken (automation mid-print), the print may mix old and new values:
    // it is still written, but never marked current, so HATCH shows it stale (C8).
    bool torn;
    {
        const juce::ScopedLock sl(stateLock);
        torn = renderSignature() != sig;
    }

    const int regionLen = endFrame - startFrame;
    const int seamLen   = seamFrames(srcSR, regionLen);   // same seam fade as the audition
    // SYNC: exactly round(regionLen / ratio) frames — the whole-bars length.
    const int numOut = sync ? (int) std::lround((double) regionLen / tempoRatio)
                            : (int) std::ceil((double) regionLen / rate);
    if (numOut <= 0)
        return {};

    // SYNC: one seamless stretched + transposed cycle of the region (the
    // same seam-faded stream the audition voice feeds its stretcher), rendered
    // cyclically so the print has no fade-in and no seam click (TimeStretch.h).
    // The chain passes below (priming + capture) then read this cycle in place of
    // the source, so the existing tail priming wraps it exactly like the audition.
    juce::AudioBuffer<float> stretched;
    if (sync)
    {
        try
        {
            stretched.setSize(2, numOut);
            auto readFrame = [&] (int k, float& l, float& r)
            {
                const double p = (double) (startFrame + k);
                l = readSeamed(src, 0, p, startFrame, regionLen, seamLen);
                r = readSeamed(src, juce::jmin(1, srcChannels - 1), p, startFrame, regionLen, seamLen);
            };
            if (! loopnest::renderCyclicStretch(readFrame, regionLen, numOut, srcSR, (float) semis,
                                                stretched.getWritePointer(0), stretched.getWritePointer(1)))
                return {};
        }
        catch (const std::bad_alloc&)
        {
            return {};
        }
    }

    // Same guard as decodeFile: a long file pitched down (-12 st doubles numOut)
    // can ask for a big block, and a bad_alloc out of a click handler takes the
    // host down with us.
    juce::AudioBuffer<float> out;
    try
    {
        out.setSize(outChannels, numOut);
    }
    catch (const std::bad_alloc&)
    {
        return {};
    }
    out.clear();

    // The looped audition never resets the chain at the trim seam, so echo/reverb
    // tails ring across the wrap — that steady state is what the user shaped
    // against. A single cold pass would print a dry head and hard-cut the tail
    // (and a region shorter than the echo delay would print no echo at all). So:
    // run discarded priming passes over the region first — enough to cover the live
    // tail (printPrimePasses) — then capture the final pass with the chain state
    // carried in, exactly like one more wrap of the audition loop.
    // A dry print has no chain state to prime: one straight pass.
    const int primePasses = dryPrint ? 0 : printPrimePasses((double) numOut / srcSR);
    // The chain's fixed delay (ruling 3) is the host's to compensate live; a print is
    // offline and starts on its own first frame: the kept pass is read `lat` frames
    // later in the chain's output, running on into the next wrap for them.
    const int lat = dryPrint ? 0 : renderChar.latency();

    // One frame of the wet chain at output frame n / source position pos, the
    // chain and GAIN MATCH state carried. Wet prints only.
    auto wetFrame = [&] (int n, double pos, float (&blended)[2])
    {
        float s[2] = { 0.0f, 0.0f };
        for (int ch = 0; ch < outChannels; ++ch)
        {
            const int srcCh = juce::jmin(ch, srcChannels - 1);
            s[ch] = (sync ? stretched.getSample(ch, n)
                          : readSeamed(src, srcCh, pos, startFrame, regionLen, seamLen))
                  * inGain;                                // input gain pre-rack (bakes in)
        }
        float ref[2] = { s[0], s[1] };               // GAIN MATCH's dry reference, pre-EQ
        renderChar.processEq(s, outChannels);        // master EQ pre-rack (bakes in)
        float dry[2] = { s[0], s[1] };
        renderChar.process(s, outChannels);
        renderChar.delayTaps(ref, dry, outChannels);         // both taps on the chain's fixed delay
        renderChar.alignDry(dry, outChannels, mix01);       // the dry tap in DRIVE's phase, as live
        for (int ch = 0; ch < outChannels; ++ch)
            blended[ch] = dry[ch] + mix01 * (s[ch] - dry[ch]);
        renderMonitor.process(ref, blended, outChannels, gmatch, false);
    };

    // Wet print seam (F11). WARP/FLUTTER wobble on clocks of their own, so at the
    // print's end the wobble sits somewhere other than where it sat at its head, and
    // played as a loop the file steps there. Render the first few ms of one more wrap
    // (what the audition plays right after the end point, chain carried) and fade the
    // print's head over from it: the file's last frame now runs straight into a head
    // that is the audition's own continuation, and by the fade's end it is the captured
    // head again. Rates stay exact; only those first ms differ from the audition's
    // first pass. Equal-gain (cos^2) fade: the two are the same audio, correlated.
    const int xfLen = dryPrint ? 0 : juce::jmin(juce::roundToInt(0.005 * srcSR), numOut / 4);
    std::vector<float> cont[2];
    for (auto& c : cont) c.assign((size_t) juce::jmax(0, xfLen), 0.0f);

    // Passes over the region, frame n of pass `pass` landing on print frame
    // (pass - primePasses) * numOut + n - lat: the kept frames, then the seam's.
    const int passes = primePasses + 1 + (dryPrint ? 0 : (lat + xfLen + numOut - 1) / numOut);
    for (int pass = 0; pass < passes; ++pass)
    {
        double pos = (double) startFrame;
        for (int n = 0; n < numOut && (sync || pos < (double) endFrame); ++n)
        {
            // Dry hatch: trim + rate only — no gains, EQ, chain or mix. In FX
            // mode the live chain adds all of that to the clip, so baking any of it
            // here would apply it twice.
            if (dryPrint)
            {
                for (int ch = 0; ch < outChannels; ++ch)
                {
                    const int srcCh = juce::jmin(ch, srcChannels - 1);
                    out.setSample(ch, n, sync ? stretched.getSample(ch, n)
                                              : readSeamed(src, srcCh, pos, startFrame, regionLen, seamLen));
                }
                pos += rate;
                continue;
            }

            float blended[2];
            wetFrame(n, pos, blended);
            const long long at = (long long) (pass - primePasses) * numOut + n - lat;
            if (at >= 0 && at < numOut)
                for (int ch = 0; ch < outChannels; ++ch)
                    out.setSample(ch, (int) at, blended[ch] * outGain);
            else if (at >= numOut && at < (long long) numOut + xfLen)
                for (int ch = 0; ch < outChannels; ++ch)
                    cont[ch][(size_t) (at - numOut)] = blended[ch] * outGain;

            pos += rate;
        }
    }

    for (int n = 0; n < xfLen; ++n)
    {
        const float t = ((float) n + 0.5f) / (float) xfLen * juce::MathConstants<float>::halfPi;
        const float in = std::sin(t) * std::sin(t);
        for (int ch = 0; ch < outChannels; ++ch)
            out.setSample(ch, n, out.getSample(ch, n) * in + cont[ch][(size_t) n] * (1.0f - in));
    }

    // Write to a durable per-user folder, NOT the temp dir: Ableton references the
    // dragged file in place until Collect-All-and-Save, and macOS purges temp
    // content after a few days — a temp render means a saved set silently goes
    // offline later. Names are timestamped + collision-proofed (never reuse or
    // delete an existing path — another instance's armed render, or a saved set,
    // may still point at it). Nothing here auto-prunes for the same reason; the
    // folder is user-visible and self-explanatory.
    auto dir = getPrintsRoot();
    dir.createDirectory();
    if (! dir.isDirectory())
        return {};
    // The selected bin, if its folder is still there. A bin that is gone (renamed or
    // deleted in Finder) prints into the root: never recreate it (see the header).
    auto bin = getPrintBin();
    if (bin.isNotEmpty() && getBinFolder(bin).isDirectory())
        dir = getBinFolder(bin);
    else
        bin.clear();

    // <bin>_<source>_<pitch>[_dry]_<stamp>: the source leads the stamp because a DAW
    // browser sorts a folder by name. A filename holds 255 bytes: the source is cut to
    // 140 and the bin prefix to 64 (UTF-8), so the rest and a " (2)" always fit (E-L3).
    auto clipBytes = [] (juce::String t, int maxBytes)
    {
        while ((int) t.getNumBytesAsUTF8() > maxBytes)
            t = t.dropLastCharacters(1);
        return t;
    };
    auto base = clipBytes(sampleFile.getFileNameWithoutExtension(), 140);
    if (base.isEmpty()) base = "loop";
    const auto prefix = clipBytes(legalBinName(bin), 64);   // a Finder-made bin's name, made filename-safe
    // +0st, -3st, +2.5st (the knob's float can sit a hair off a whole number)
    const double st = std::round((double) apvts.getRawParameterValue("pitch")->load() * 10.0) / 10.0;
    const auto pitch = juce::String(st < 0.0 ? "-" : "+")
                         + (std::abs(st - std::round(st)) < 0.05 ? juce::String((int) std::abs(std::round(st)))
                                                                 : juce::String(std::abs(st), 1)) + "st";
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
    // "./" first: getChildFile reads a name that starts with '~' (a bin "~Old", a loop
    // "~kick.wav") as a home path, and the print landed outside the folder.
    auto file = dir.getNonexistentChildFile("./" + (prefix.isNotEmpty() ? prefix + "_" : juce::String()) + base + "_" + pitch
                                                + (dryPrint ? "_dry_" : "_") + stamp, ".wav");

    // 32-bit float, not 24-bit int: the int writer clamps at 0 dBFS, while the
    // audition goes to the host as float and never clips. A hot loop + INPUT/OUTPUT
    // gain + ECHO/WIDTH can peak over 0 dBFS; float keeps the overs so the print
    // matches what was heard (the DAW fader brings it down, same as the audition).
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
    {
        file.deleteFile();   // createOutputStream may have created it before failing
        return {};
    }

    // On success the writer takes the stream and nulls `stream`; on failure it's
    // left with us and freed on scope exit.
    auto writer = wav.createWriterFor(stream,
                                      juce::AudioFormatWriterOptions{}
                                          .withSampleRate(srcSR)
                                          .withNumChannels(outChannels)
                                          .withBitsPerSample(32)
                                          .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer == nullptr)
    {
        stream.reset();      // close before deleting — no 0-byte leftovers in Prints
        file.deleteFile();
        return {};
    }

    const bool ok = writer->writeFromAudioSampleBuffer(out, 0, out.getNumSamples());
    writer.reset();   // flush + close
    if (! ok)
    {
        file.deleteFile();   // disk full etc. — never arm a truncated render
        return {};
    }

    {
        const juce::ScopedLock sl(stateLock);
        // A load that landed mid-render already moved on; don't arm this print over it.
        if (currentSample != sampleFile)
            return {};
        lastRender    = file;   // survives editor close/reopen (PRINT re-arms from it)
        lastRenderSig = torn ? 0 : sig;   // 0 never matches (see isLastRenderCurrent)
    }
    return file;
}

juce::uint64 LoopNestProcessor::renderSignature() const
{
    // FNV-1a over the sample path and every baked param's raw value. Audition-only
    // params are skipped so toggling A/B or glide doesn't mark the print stale.
    // Also the DRY/SYNC flags and, with SYNC on, the tempo ratio (a host
    // tempo change re-times the print). PITCH is always in (under SYNC it
    // bakes in as the stretcher's transpose). A dry print bakes only trim + rate +
    // pitch, so it skips the chain/level params — tweaking the live FX after a dry
    // hatch doesn't mark the print stale.
    juce::uint64 h = 14695981039346656037ull;
    auto mix = [&h] (const void* data, size_t n)
    {
        auto* p = static_cast<const juce::uint8*>(data);
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    };
    const auto path = currentSample.getFullPathName().toStdString();
    mix(path.data(), path.size());
    // The bin it would land in: picking another bin makes the armed print stale, so
    // the next HATCH puts a copy there.
    const auto bin = getPrintBin().toStdString();
    mix(bin.data(), bin.size());
    const bool dry  = dryHatch.load();
    const bool sync = isStretching();
    mix(&dry,  sizeof(dry));
    mix(&sync, sizeof(sync));
    if (sync)
    {
        const double r = syncRatio(loopBpm, getTargetBpm());
        mix(&r, sizeof(r));
    }
    for (auto* p : getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*>(p);
        if (rp == nullptr) continue;
        const auto& id = rp->getParameterID();
        if (id == "pitchGlide" || id == "bypass")    // monitor-only: never printed
            continue;
        if (dry && id != "start" && id != "end" && id != "pitch")
            continue;
        const float v = rp->getValue();
        mix(&v, sizeof(v));
    }
    return h;
}

bool LoopNestProcessor::isLastRenderCurrent() const
{
    const juce::ScopedLock sl(stateLock);
    return lastRender != juce::File() && lastRenderSig != 0 && renderSignature() == lastRenderSig;
}

// === State persistence ===
// The parameter tree (apvts) is the master state; the non-parameter data
// (folder, current sample, loop flag) rides along as extra properties on its root.

void LoopNestProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // The params and the sample as one snapshot: without the commit lock an install
    // landing between copyState and the stateLock section below would save the new
    // sample with the old sample's params (lock order: commitLock → stateLock).
    const juce::ScopedLock cl(commitLock);
    auto state = apvts.copyState();
    state.setProperty("stateVersion", kStateVersion, nullptr);
    {
        const juce::ScopedLock sl(stateLock);
        // A load still decoding is what the set means (a restore's sample, or a
        // spin a moment old): save it, not the file it's about to replace.
        juce::File sample = currentSample;
        // The locked tempo rides along, so a reopened set stretches as it did. A
        // pending load saves its own (none for a spin: it locks when it lands, and a
        // restore without one reads the saved trim, which a spin resets to the file).
        double bpm = loopBpm;
        BpmSource bpmSrc = loopBpmSource;
        double origin = barOrigin.load();
        {
            const juce::ScopedLock ql(loadQueueLock);
            if (pendingSample != juce::File())
            {
                bpm = pendingBpm;
                bpmSrc = pendingBpmSource;
                origin = pendingBarOrigin;
                // …with the values it will land with (a spin's trim reset, a
                // snapshot's params), not the old file's still in the tree.
                sample = pendingSample;
                forEachPart(pendingApply, pendingParts, [&state] (const char* id, float natural)
                {
                    auto param = state.getChildWithProperty("id", juce::String(id));
                    if (param.isValid())
                        param.setProperty("value", natural, nullptr);
                });
            }
        }
        state.setProperty("folder", sampleFolder.getFullPathName(), nullptr);
        state.setProperty("sample", sample.getFullPathName(), nullptr);
        if (origin > 0.0) state.setProperty("barOrigin", origin, nullptr);
        else              state.removeProperty("barOrigin", nullptr);
        if (bpm > 0.0 && bpmSrc != BpmSource::none)
        {
            state.setProperty("loopBpm", bpm, nullptr);
            state.setProperty("loopBpmSrc", bpmSrc == BpmSource::name ? "name"
                                          : bpmSrc == BpmSource::user ? "user" : "length", nullptr);
        }
        else
        {
            // copyState carries the last restored set's properties: never save its tempo.
            state.removeProperty("loopBpm", nullptr);
            state.removeProperty("loopBpmSrc", nullptr);
        }
        // v1: relative to the bank, so a moved stash can be relinked by re-pointing
        // the bank. Empty when the sample isn't under it.
        state.setProperty("sampleRel",
                          sample.isAChildOf(sampleFolder) ? sample.getRelativePathFrom(sampleFolder)
                                                          : juce::String(),
                          nullptr);
        // B4: nothing loaded or loading, and the restored set's sample missing: its
        // reference goes back out exactly as it came in.
        if (sample == juce::File() && missingSample.isValid())
            for (auto* id : { "sample", "sampleRel", "loopBpm", "loopBpmSrc", "barOrigin" })
            {
                if (missingSample.hasProperty(id)) state.setProperty(id, missingSample.getProperty(id), nullptr);
                else                               state.removeProperty(id, nullptr);
            }
    }
    state.setProperty("loop", loopEnabled.load(), nullptr);
    // Mode + toggles ride along too, so a reopened set with a dry clip on
    // the track comes back in FX (not auditioning over it).
    state.setProperty("fxMode", fxMode.load(),      nullptr);
    state.setProperty("sync",   syncEnabled.load(), nullptr);
    state.setProperty("freeBpm", freeBpm.load(),    nullptr);
    state.setProperty("dry",    dryHatch.load(),    nullptr);
    state.setProperty("muted",  lockMuted.load(),   nullptr);   // the transport-owned mute (isBarLocked)
    // The preset label (U-M4). Optional, no stateVersion: a set without it, older or
    // INIT, reads INIT, and an older build ignores it. copyState carries the last
    // restored set's, so INIT removes it.
    if (presetIndex.load() >= 0) state.setProperty("preset", presetIndex.load(), nullptr);
    else                         state.removeProperty("preset", nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void LoopNestProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr)
        return;
    historyResetPending.store(true);   // the restored set starts the spin history again

    auto state = juce::ValueTree::fromXml(*xml);
    if (! state.isValid() || state.getType() != apvts.state.getType())
        return;

    // Migration. 0 (pre-v2, no property): absolute paths only. 1: + sampleRel.
    // 2: + loopBpm / loopBpmSrc (absent: the saved trim read as bars, see LoadJob).
    // 3: + muted (absent, or an older state: muted). 4: WARP / FLUTTER retapered and
    // + reverbType (an older state's warp and flutter converted, below).
    // A newer schema than this build knows still loads what it can, read as this
    // build's meanings. Not every value kept its meaning: at 4, warp and flutter did
    // (converted here, for a saved state only). Host automation lanes and controller
    // mappings of warp / flutter recorded before 4 are NOT converted and now drive the
    // new taper (Andrew 10-17: accepted, v2 unreleased, only his test sets have any).
    const int version = (int) state.getProperty("stateVersion", 0);

    const juce::File folder(state.getProperty("folder", juce::String()).toString());
    const juce::File absSample(state.getProperty("sample", juce::String()).toString());
    const juce::String relSample = version >= 1 ? state.getProperty("sampleRel", juce::String()).toString()
                                                : juce::String();

    // The absolute path wins while it still exists; otherwise the bank-relative
    // one (the stash moved with its bank folder re-pointed).
    juce::File sample = absSample;
    if (! sample.existsAsFile() && relSample.isNotEmpty() && folder.isDirectory())
        sample = folder.getChildFile("./" + relSample);   // "./": a leading '~' isn't home

    {
        // One commit (see commitLock). Claim the newest gen BEFORE any param
        // changes: an older load still decoding or awaiting install is dropped now,
        // so it can never land over the params replaceState is about to set.
        const juce::ScopedLock cl(commitLock);

        // Mute the audition for the whole restore: replaceState lands the saved
        // trim/FX immediately, but the saved sample decodes afterwards. The job's
        // mute (muteUntilGen) holds the silence until that sample lands.
        ++restoring;
        const juce::ScopeGuard unmute { [this] { --restoring; } };

        juce::ValueTree missing;
        if (sample.existsAsFile())
        {
            // Params come from replaceState, so the load applies none.
            auto job = restoreJob(sample, state);
            job.clearOnFail = true;
            job.mute = true;
            requestLoad(std::move(job));
        }
        else
        {
            // Recalled state has no sample (or it vanished): don't leave the previous
            // sample playing/armed under state that says otherwise.
            clearSample();
            // B4: one that can't be found (a drive not mounted) is still the set's: its
            // reference rides along until a load replaces it, so a save never forgets it.
            if (absSample.getFullPathName().isNotEmpty())
            {
                missing = juce::ValueTree("MissingSample");
                missing.setProperty("sample", absSample.getFullPathName(), nullptr);
                missing.setProperty("sampleRel", relSample, nullptr);
                for (auto* id : { "loopBpm", "loopBpmSrc", "barOrigin" })
                    if (state.hasProperty(id))
                        missing.setProperty(id, state.getProperty(id), nullptr);
            }
        }

        // A set saved before panel power has no warpOn .. widthOn: replaceState
        // would leave those params wherever this instance had them, so a missing
        // one is written in explicitly as ON (its default).
        for (auto* id : loopnest::CharacterParams::powerIds())
            if (! state.getChildWithProperty("id", juce::String(id)).isValid())
            {
                juce::ValueTree param("PARAM");
                param.setProperty("id", juce::String(id), nullptr);
                param.setProperty("value", 1.0f, nullptr);
                state.appendChild(param, nullptr);
            }

        // Before 4, WARP and FLUTTER had other tapers (CharacterChain taper): convert
        // them to the knob values that give the set's old wobble depth. WARP's depends on
        // its RATE (warpFromOld), FLUTTER's is halved. The params' 0.5 % grid rounds them.
        // And a set without a reverbType (or saved before it) gets HALL, the default, not
        // this instance's.
        if (version < 4)
        {
            auto warp = state.getChildWithProperty("id", "warp"), flutter = state.getChildWithProperty("id", "flutter");
            const auto rate = state.getChildWithProperty("id", "warpRate");
            const float ratePct = rate.isValid() ? (float) rate.getProperty("value", 40.0) : 40.0f;
            if (warp.isValid())
                warp.setProperty("value", CharacterChain::warpFromOld((float) warp.getProperty("value", 0.0), ratePct), nullptr);
            if (flutter.isValid())
                flutter.setProperty("value", CharacterChain::flutterFromOld((float) flutter.getProperty("value", 0.0)), nullptr);
        }
        if (! state.getChildWithProperty("id", "reverbType").isValid())
        {
            juce::ValueTree param("PARAM");
            param.setProperty("id", "reverbType", nullptr);
            param.setProperty("value", 1.0f, nullptr);
            state.appendChild(param, nullptr);
        }

        apvts.replaceState(state);

        loopEnabled.store((bool) state.getProperty("loop", true));
        setFxMode((bool) state.getProperty("fxMode", false));
        syncEnabled.store((bool) state.getProperty("sync", true));
        freeBpm.store(syncEnabled.load() ? 0.0 : juce::jlimit(0.0, kMaxLoopBpm, (double) state.getProperty("freeBpm", 0.0)));
        dryHatch.store((bool) state.getProperty("dry", true));
        // 3: the saved mute. Older, or without one: muted (see kStateVersion).
        lockMuted.store(version >= 3 && state.hasProperty("muted") ? (bool) state.getProperty("muted") : true);
        setPresetIndex((int) state.getProperty("preset", -1));   // absent: INIT

        const juce::ScopedLock sl(stateLock);
        sampleFolder = folder;
        ++folderGen;
        missingSample = missing;
    }
    // The disk walk runs outside the commit, so an install on the message thread
    // never waits on it.
    rescanFolder();
}

LoopNestProcessor::LoadJob LoopNestProcessor::restoreJob(const juce::File& file, const juce::ValueTree& saved)
{
    LoadJob job;
    job.file = file;
    job.bpm = (double) saved.getProperty("loopBpm", 0.0);
    const auto src = saved.getProperty("loopBpmSrc", juce::String()).toString();
    job.bpmSource = src == "name" ? BpmSource::name : src == "user" ? BpmSource::user : BpmSource::length;
    job.bpmFromRegion = job.bpm <= 0.0;
    job.barOrigin = (double) saved.getProperty("barOrigin", 0.0);
    return job;
}

void LoopNestProcessor::relinkMissingSample()
{
    // The saved path first (the drive is back), else the bank-relative one under the
    // folder just picked. It lands as the host restore would have: the saved tempo and
    // bar 1, the params already in. A load already on its way replaces the reference
    // itself when it lands, so it is left alone.
    const juce::ScopedLock cl(commitLock);
    juce::File file;
    juce::ValueTree saved;
    {
        const juce::ScopedLock sl(stateLock);
        if (! missingSample.isValid() || isLoadPending())
            return;
        saved = missingSample.createCopy();
        file = juce::File(saved["sample"].toString());
        const auto rel = saved["sampleRel"].toString();
        if (! file.existsAsFile() && rel.isNotEmpty() && sampleFolder.isDirectory())
            file = sampleFolder.getChildFile("./" + rel);   // "./": a leading '~' isn't home
    }
    if (file.existsAsFile())
        requestLoad(restoreJob(file, saved));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LoopNestProcessor();
}
