#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <regex>

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
    // The DSP lives in CharacterChain (signal order WARP → FLUTTER → …).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "warp", 1 }, "Warp",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f,
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
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f,
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
    //   reverbDecay — room size / tail length, independent of the wet-level knob.
    //   echoTime    — free slapback→short-dub time (% maps 30..350 ms; no tempo sync,
    //                 baked as character — Ableton handles rhythmic delay after the drop).
    //   widthMid    — CENTER: bipolar ±dB gain on the M/S MID (0 % = unity), works even
    //                 with WIDTH at 0. All additive (old projects load at these neutrals).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "reverbDecay", 1 }, "Reverb Decay",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "echoTime", 1 }, "Echo Time",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 44.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "widthMid", 1 }, "Width Center",
        juce::NormalisableRange<float>(-100.0f, 100.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // Master DRY/WET — blends the unprocessed (pitched/trimmed) signal against the
    // full character chain, applied in both audition and the PRINT render. 100 % =
    // fully wet (the chain as heard before this knob existed).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // AB-monitor toggles (audition-only — NEVER baked into PRINT/renderLoop).
    // bypass: hear the dry source (pitched/trimmed, uncoloured) to A/B against the FX.
    // gainMatch: scale the processed output to the dry loudness so the A/B is fair.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "bypass", 1 }, "Bypass", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "gainMatch", 1 }, "Gain Match", false));

    return { params.begin(), params.end() };
}

LoopNestProcessor::LoopNestProcessor()
    : AudioProcessor(BusesProperties()
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "LoopNestState", createParameterLayout())
{
    formatManager.registerBasicFormats(); // WAV + AIFF
    pitchParam      = apvts.getRawParameterValue("pitch");
    pitchGlideParam = apvts.getRawParameterValue("pitchGlide");
    warpParam       = apvts.getRawParameterValue("warp");
    warpRateParam   = apvts.getRawParameterValue("warpRate");
    startParam      = apvts.getRawParameterValue("start");
    endParam        = apvts.getRawParameterValue("end");
    inputParam      = apvts.getRawParameterValue("input");
    outputParam     = apvts.getRawParameterValue("output");
    driveParam      = apvts.getRawParameterValue("drive");
    driveToneParam  = apvts.getRawParameterValue("driveTone");
    flutterParam    = apvts.getRawParameterValue("flutter");
    flutterRateParam= apvts.getRawParameterValue("flutterRate");
    echoParam       = apvts.getRawParameterValue("echo");
    reverbParam     = apvts.getRawParameterValue("reverb");
    echoLoParam     = apvts.getRawParameterValue("echoLo");
    echoHiParam     = apvts.getRawParameterValue("echoHi");
    reverbLoParam   = apvts.getRawParameterValue("reverbLo");
    reverbHiParam   = apvts.getRawParameterValue("reverbHi");
    eqLoParam       = apvts.getRawParameterValue("eqLo");
    eqHiParam       = apvts.getRawParameterValue("eqHi");
    widthParam      = apvts.getRawParameterValue("width");
    widthLoParam    = apvts.getRawParameterValue("widthLo");
    widthHiParam    = apvts.getRawParameterValue("widthHi");
    reverbDecayParam= apvts.getRawParameterValue("reverbDecay");
    echoTimeParam   = apvts.getRawParameterValue("echoTime");
    widthMidParam   = apvts.getRawParameterValue("widthMid");
    mixParam        = apvts.getRawParameterValue("mix");
    bypassParam     = apvts.getRawParameterValue("bypass");
    gainMatchParam  = apvts.getRawParameterValue("gainMatch");

    // Fresh-instance default: auto-load the last folder we used anywhere. A
    // restored project calls setStateInformation afterwards and overrides this.
    if (auto def = recallDefaultFolder(); def.isDirectory())
    {
        {
            const juce::ScopedLock sl(stateLock);
            sampleFolder = def;
        }
        rescanFolder();
    }
}

LoopNestProcessor::~LoopNestProcessor()
{
    *aliveFlag = false;   // strand any in-flight callAsync (we're going away)

    // Join the drum-extract worker so it can't run past our destruction. 10 s
    // gives a slow/network-volume copy a fair chance; a hard kill past that is
    // safe — copies land on .part temp names and only rename on success.
    if (extractThread != nullptr)
        extractThread->stopThread(10000);
}

bool LoopNestProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Instrument: no input bus, mono or stereo output.
    if (! layouts.getMainInputChannelSet().isDisabled())
        return false;

    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono()
        || out == juce::AudioChannelSet::stereo();
}

void LoopNestProcessor::prepareToPlay(double sampleRate, int)
{
    hostSampleRate = sampleRate;
    playbackActive = false;
    readPos = 0.0;

    character.prepare(sampleRate);

    declickCoeff = (float) (1.0 - std::exp(-1.0 / (0.002 * sampleRate)));
    bypCoeff     = (float) (1.0 - std::exp(-1.0 / (0.005 * sampleRate)));
    declickGain  = 1.0f;
    pendingSeekNorm = -1.0f;
    stopping = false;

    // Gain-match K-weighting (ITU-R BS.1770), RBJ biquads rebuilt for the host SR.
    // Published filter targets (De Man 2013 derivation, valid at any fs):
    // stage 1 high shelf f0≈1681.97 Hz, +4 dB, Q≈0.7072 · stage 2 HP f0≈38.14 Hz, Q≈0.5003.
    {
        const auto normalize = [] (Biquad& c, double a0)
        {
            c.b0 = (float) (c.b0 / a0); c.b1 = (float) (c.b1 / a0); c.b2 = (float) (c.b2 / a0);
            c.a1 = (float) (c.a1 / a0); c.a2 = (float) (c.a2 / a0);
        };
        {
            const double f0 = 1681.9744509555319, gDb = 3.9998438539733446, q = 0.7071752369554196;
            const double A  = std::pow(10.0, gDb / 40.0);
            const double w0 = juce::MathConstants<double>::twoPi * f0 / sampleRate;
            const double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * q);
            const double sa = 2.0 * std::sqrt(A) * alpha;
            kwShelf.b0 = (float) (A * ((A + 1.0) + (A - 1.0) * cw + sa));
            kwShelf.b1 = (float) (-2.0 * A * ((A - 1.0) + (A + 1.0) * cw));
            kwShelf.b2 = (float) (A * ((A + 1.0) + (A - 1.0) * cw - sa));
            kwShelf.a1 = (float) (2.0 * ((A - 1.0) - (A + 1.0) * cw));
            kwShelf.a2 = (float) ((A + 1.0) - (A - 1.0) * cw - sa);
            normalize(kwShelf, (A + 1.0) - (A - 1.0) * cw + sa);
        }
        {
            const double f0 = 38.13547087602444, q = 0.5003270373238773;
            const double w0 = juce::MathConstants<double>::twoPi * f0 / sampleRate;
            const double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * q);
            kwHipass.b0 = (float) ((1.0 + cw) * 0.5);
            kwHipass.b1 = (float) (-(1.0 + cw));
            kwHipass.b2 = (float) ((1.0 + cw) * 0.5);
            kwHipass.a1 = (float) (-2.0 * cw);
            kwHipass.a2 = (float) (1.0 - alpha);
            normalize(kwHipass, 1.0 + alpha);
        }
        for (auto* s : { &kwRef[0], &kwRef[1], &kwProc[0], &kwProc[1] })
            s->reset();
    }

    smInGain.reset(sampleRate, 0.02);
    smOutGain.reset(sampleRate, 0.02);
    smMix.reset(sampleRate, 0.02);
    smInGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(inputParam->load(),  -60.0f));
    smOutGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(outputParam->load(), -60.0f));
    smMix.setCurrentAndTargetValue(juce::jlimit(0.0f, 1.0f, mixParam->load() * 0.01f));
}

void LoopNestProcessor::releaseResources() {}

void LoopNestProcessor::regionFrames(int numFrames, int& startFrame, int& endFrame) const
{
    const int lastFrame = numFrames - 1;
    startFrame = juce::jlimit(0, juce::jmax(0, lastFrame),
                              (int) (startParam->load() * lastFrame));
    endFrame   = juce::jlimit(startFrame + 1, juce::jmax(startFrame + 1, lastFrame),
                              (int) (endParam->load() * lastFrame));
}

void LoopNestProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi); // LoopNest ignores MIDI entirely.
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    // Try to grab the sample; if a load is swapping it in, skip this block.
    const juce::SpinLock::ScopedTryLockType lock(sampleLock);
    if (! lock.isLocked())
        return;

    const int numSampleFrames = sampleBuffer.getNumSamples();
    if (numSampleFrames <= 0)
    {
        playbackActive = false;
        return;
    }

    const int numOutChannels = buffer.getNumChannels();

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

    // Playback rate: correct for file/host SR mismatch, then apply pitch shift.
    const double pitchRatio = std::pow(2.0, (double) pitchParam->load() / 12.0);
    const double rate = (sampleFileSampleRate / hostSampleRate) * pitchRatio;

    // Input level — gain-staged into the rack (pre-chain, before the dry/wet split).
    // Output level (the big knob). Both floor to silence at -60 dB, and both ramp
    // per-sample (~20 ms) so drags/preset slams don't zipper.
    smInGain.setTargetValue(juce::Decibels::decibelsToGain(inputParam->load(),  -60.0f));
    smOutGain.setTargetValue(juce::Decibels::decibelsToGain(outputParam->load(), -60.0f));

    // Stage 2 character chain (live audition). Same chain runs in renderLoop.
    character.setParams(warpParam->load(), warpRateParam->load(),
                        driveParam->load(), driveToneParam->load(),
                        flutterParam->load(), flutterRateParam->load(),
                        echoParam->load(), reverbParam->load(),
                        echoLoParam->load(), echoHiParam->load(),
                        reverbLoParam->load(), reverbHiParam->load(),
                        eqLoParam->load(), eqHiParam->load(),
                        widthParam->load(), widthLoParam->load(), widthHiParam->load(),
                        reverbDecayParam->load(), echoTimeParam->load(), widthMidParam->load());

    // Master dry/wet: 1 = full character chain, 0 = the clean (pitched/trimmed) tap.
    smMix.setTargetValue(juce::jlimit(0.0f, 1.0f, mixParam->load() * 0.01f));

    // A/B monitor (audition only): bypass swaps the output to the dry source tap;
    // gainMatch scales the processed output to the dry loudness so the level isn't
    // a confound. K-weighted one-pole MS trackers — ~1 s integration so the match
    // gain sits still over a rhythmic loop (a 50 ms window made it a slow AGC:
    // ducking into echo/reverb tails, rising into transients) — plus a ~200 ms
    // makeup smooth.
    const bool  bypass = bypassParam->load()    > 0.5f;
    const bool  gmatch = gainMatchParam->load() > 0.5f;
    const float msCoeff    = (float) (1.0 - std::exp(-1.0 / (1.0  * hostSampleRate)));
    const float matchCoeff = (float) (1.0 - std::exp(-1.0 / (0.20 * hostSampleRate)));

    // PITCH glide: per-sample slew of the playback rate toward its target. 0 % =
    // instant (coeff 1). The time constant grows ~quadratically up to kGlideMaxSec.
    constexpr double kGlideMaxSec = 1.5;
    const float  glide01    = juce::jlimit(0.0f, 1.0f, pitchGlideParam->load() * 0.01f);
    const double glideTau   = (double) glide01 * glide01 * kGlideMaxSec;
    const double glideCoeff = glideTau <= 0.0 ? 1.0
                            : 1.0 - std::exp(-1.0 / (glideTau * hostSampleRate));

    int startFrame, endFrame;
    regionFrames(numSampleFrames, startFrame, endFrame);

    // Publishes the current readPos to the UI scrubber as a 0..1 fraction.
    // Divide by (N-1), NOT N, so this matches how regionFrames() maps the trim
    // params (start/end * lastFrame). Otherwise the playhead's reachable range is
    // systematically narrower than the brackets and it parks just short of them.
    auto publishPlayhead = [this, numSampleFrames]
    {
        playheadNorm.store((float) juce::jlimit(0.0, 1.0,
                                  readPos / (double) juce::jmax(1, numSampleFrames - 1)));
    };

    // Play edge: on the transition into play, RESUME from the current position
    // (so Pause/Play continues where it left off). Only snap to the region start
    // if the position is stale — outside the region or never set.
    const bool wantPlay = playRequested.load();
    if (wantPlay && ! playbackActive)
    {
        if (readPos < (double) startFrame || readPos >= (double) endFrame)
            readPos = (double) startFrame;
        glideRate = rate;   // start at the current pitch so Play doesn't glide from 0
        character.reset();
        matchGain = 1.0f; refMS = procMS = 0.0;   // fresh gain-match trackers
        for (auto* s : { &kwRef[0], &kwRef[1], &kwProc[0], &kwProc[1] })
            s->reset();                           // …and their K-weight filters
        declickGain = 0.0f;                       // fade in from silence
        bypBlend = bypass ? 1.0f : 0.0f;          // no surprise A/B crossfade at start
        playbackActive = true;
    }
    if (wantPlay)
        stopping = false;            // re-pressed Play during the fade-out: resume
    else if (playbackActive)
        stopping = true;             // pause: fade out first, stop in the loop below

    if (! playbackActive)
    {
        publishPlayhead();
        return;
    }

    const bool looping = loopEnabled.load();

    for (int frame = 0; frame < buffer.getNumSamples(); ++frame)
    {
        // Advance the gain/mix ramps once per frame (not per channel).
        const float inGain  = smInGain.getNextValue();
        const float mix01   = smMix.getNextValue();
        const float outGain = smOutGain.getNextValue();

        // Declick ramp: ~2 ms fade out into a pending seek or a stop, fade back in
        // after. The jump/stop lands only once the output is (near) silent.
        const bool fadingOut = stopping || pendingSeekNorm >= 0.0f;
        declickGain += declickCoeff * ((fadingOut ? 0.0f : 1.0f) - declickGain);
        if (fadingOut && declickGain < 1.0e-3f)
        {
            if (pendingSeekNorm >= 0.0f)
            {
                readPos = juce::jlimit(0.0, (double) (numSampleFrames - 1),
                                       (double) pendingSeekNorm * (double) numSampleFrames);
                pendingSeekNorm = -1.0f;
            }
            if (stopping)
            {
                stopping = false;
                playbackActive = false;
                break;
            }
        }

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
                break;
            }
        }
        if (readPos < (double) startFrame) // trim moved under us mid-play
            readPos = (double) startFrame;

        // Linear interpolation between the two nearest source samples.
        const int   i0 = (int) readPos;
        const int   i1 = (i0 + 1 < numSampleFrames) ? i0 + 1 : i0;
        const float frac = (float) (readPos - (double) i0);

        float outSamples[2] = { 0.0f, 0.0f };
        for (int ch = 0; ch < numOutChannels; ++ch)
        {
            const int srcCh = juce::jmin(ch, sampleBuffer.getNumChannels() - 1);
            const float s0 = sampleBuffer.getSample(srcCh, i0);
            const float s1 = sampleBuffer.getSample(srcCh, i1);
            outSamples[ch] = (s0 + frac * (s1 - s0)) * inGain;   // input gain pre-rack
        }
        if (numOutChannels == 1 && sampleBuffer.getNumChannels() > 1)
        {
            // Mono bus: downmix L+R rather than dropping the right channel.
            const float r0 = sampleBuffer.getSample(1, i0);
            const float r1 = sampleBuffer.getSample(1, i1);
            outSamples[0] = 0.5f * (outSamples[0] + (r0 + frac * (r1 - r0)) * inGain);
        }

        // Pure source tap (post-input-gain, PRE-EQ) — the A/B reference: this is what
        // BYPASS plays and what GAIN-MATCH targets (EQ counts as part of "the FX").
        const float src[2] = { outSamples[0], outSamples[1] };

        // Master EQ — pre-rack, on the whole signal before the dry/wet tap (so the
        // clean blend is EQ'd too). True bypass at open handles.
        character.processEq(outSamples, numOutChannels);

        // Character chain (warp → … → reverb), dry/wet blended against the clean
        // (EQ'd, pre-rack) tap.
        float dry[2] = { outSamples[0], outSamples[1] };
        character.process(outSamples, numOutChannels);
        float blended[2] = { 0.0f, 0.0f };
        for (int ch = 0; ch < numOutChannels; ++ch)
            blended[ch] = dry[ch] + mix01 * (outSamples[ch] - dry[ch]);

        // Gain-match: track dry-reference vs processed loudness (mono-summed,
        // K-weighted) and slew a makeup gain toward sqrt(refMS/procMS), clamped
        // to ±12 dB. While the dry ref is ~silent (echo/reverb tails only) the
        // gain HOLDS rather than chasing the floor clamp. Decays back to unity
        // when off, so toggling never jumps.
        const float rMono = (src[0]     + (numOutChannels > 1 ? src[1]     : src[0]))     * 0.5f;
        const float pMono = (blended[0] + (numOutChannels > 1 ? blended[1] : blended[0])) * 0.5f;
        const float rK = kwRef [1].process(kwHipass, kwRef [0].process(kwShelf, rMono));
        const float pK = kwProc[1].process(kwHipass, kwProc[0].process(kwShelf, pMono));
        refMS  += (double) msCoeff * ((double) rK * rK - refMS);
        procMS += (double) msCoeff * ((double) pK * pK - procMS);
        if (gmatch)
        {
            if (refMS > 1.0e-9 && procMS > 1.0e-9)
            {
                const float matchTarget = juce::jlimit(0.25f, 4.0f,
                                                       (float) std::sqrt(refMS / procMS));
                matchGain += matchCoeff * (matchTarget - matchGain);
            }
        }
        else
            matchGain += matchCoeff * (1.0f - matchGain);

        // Output: BYPASS → dry source; else the (gain-matched) processed blend.
        // ~5 ms crossfade between the two — a per-sample hard swap of phase/level-
        // different signals clicks on every A/B.
        bypBlend += bypCoeff * ((bypass ? 1.0f : 0.0f) - bypBlend);
        for (int ch = 0; ch < numOutChannels; ++ch)
        {
            const float proc = blended[ch] * matchGain;
            buffer.setSample(ch, frame,
                             (proc + bypBlend * (src[ch] - proc)) * outGain * declickGain);
        }

        glideRate += glideCoeff * (rate - glideRate);   // PITCH glide slew
        readPos += glideRate;
    }

    publishPlayhead();
}

juce::AudioProcessorEditor* LoopNestProcessor::createEditor()
{
    return new LoopNestEditor(*this);
}

// === Spin ===

void LoopNestProcessor::setSampleFolder(const juce::File& folder)
{
    {
        const juce::ScopedLock sl(stateLock);
        sampleFolder = folder;
    }
    rescanFolder();
    rememberDefaultFolder(folder); // becomes the default for future fresh instances
}

// App-level settings file, shared by every LoopNest instance (not per-project):
// ~/Library/Application Support/LoopNest/LoopNest.settings.
static juce::PropertiesFile::Options settingsFileOptions()
{
    juce::PropertiesFile::Options o;
    o.applicationName     = "LoopNest";
    o.filenameSuffix      = "settings";
    o.folderName          = "LoopNest";
    o.osxLibrarySubFolder = "Application Support";
    return o;
}

void LoopNestProcessor::rememberDefaultFolder(const juce::File& folder)
{
    if (! folder.isDirectory())
        return;

    appProperties.setStorageParameters(settingsFileOptions());
    if (auto* s = appProperties.getUserSettings())
    {
        s->setValue("defaultFolder", folder.getFullPathName());
        s->saveIfNeeded();
    }
}

juce::File LoopNestProcessor::recallDefaultFolder()
{
    appProperties.setStorageParameters(settingsFileOptions());
    if (auto* s = appProperties.getUserSettings())
        return juce::File(s->getValue("defaultFolder", {}));
    return {};
}

void LoopNestProcessor::rescanFolder()
{
    // Scan into a local array, then swap under the lock — the directory walk can
    // be slow (big stash, network volume) and must not hold stateLock throughout.
    juce::File folder;
    {
        const juce::ScopedLock sl(stateLock);
        folder = sampleFolder;
    }

    juce::Array<juce::File> found;
    if (folder.isDirectory())
    {
        // Recursive: a folder-of-folders (e.g. a loop stash with sub-packs) works.
        // Symlinks are skipped (a stray link escapes the chosen tree) and the scan
        // caps out so a runaway pick (root, a huge mount) can't hang the message
        // thread unbounded — the cap is a backstop, not a license to point it at /.
        constexpr int kMaxScan = 20000;
        for (const auto& entry : juce::RangedDirectoryIterator(
                 folder, true, "*.wav;*.aiff;*.aif", juce::File::findFiles))
        {
            if (entry.getFile().isSymbolicLink())
                continue;
            found.add(entry.getFile());
            if (found.size() >= kMaxScan)
                break;
        }
    }

    const juce::ScopedLock sl(stateLock);
    sampleFiles.swapWith(found);
}

void LoopNestProcessor::spin()
{
    // Pick under the lock — gate and pick must see the same sampleFiles (a
    // setState/rescan on another thread could empty it between the two).
    juce::File pick;
    {
        const juce::ScopedLock sl(stateLock);
        if (sampleFiles.isEmpty())
            return;

        // Pick a random file, avoiding an immediate repeat when possible.
        do {
            pick = sampleFiles.getReference(random.nextInt(sampleFiles.size()));
        } while (sampleFiles.size() > 1 && pick == currentSample);
    }

    // Only reset the trim if the load succeeded — a vanished/corrupt pick must not
    // wipe the user's trim on the still-loaded previous sample.
    if (! loadSample(pick))
        return;

    // A fresh spin plays in full until the user trims it.
    // Gesture-bracketed so the host sees a proper edit (undo/automation-safe).
    if (auto* s = apvts.getParameter("start"))
        { s->beginChangeGesture(); s->setValueNotifyingHost(0.0f); s->endChangeGesture(); }
    if (auto* e = apvts.getParameter("end"))
        { e->beginChangeGesture(); e->setValueNotifyingHost(1.0f); e->endChangeGesture(); }
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

    bool rx(const juce::String& s, const std::regex& re)
    {
        const auto utf8 = s.toStdString();
        return std::regex_search(utf8, re);
    }

    // A tiny juce::Thread that just runs a lambda — lets the processor own its
    // worker without a bespoke subclass.
    class FnThread : public juce::Thread
    {
    public:
        explicit FnThread(std::function<void()> f)
            : juce::Thread("LoopNest drum extract"), fn(std::move(f)) {}
        void run() override { if (fn) fn(); }
    private:
        std::function<void()> fn;
    };
}

void LoopNestProcessor::setExtractStatus(const juce::String& s)
{
    const juce::ScopedLock sl(extractStatusLock);
    extractStatusText = s;
}

juce::String LoopNestProcessor::getExtractStatus() const
{
    const juce::ScopedLock sl(extractStatusLock);
    return extractStatusText;
}

void LoopNestProcessor::extractDrumLoops(juce::File src, juce::File dest)
{
    if (extracting.load() || ! src.isDirectory())
        return;

    // Dest must be a usable directory (the save-mode chooser permits picking an
    // existing FILE, and createDirectory can fail on a read-only volume) — never
    // start a scan whose output, and subsequent folder adoption, can't land.
    if (dest.existsAsFile() || (! dest.exists() && ! dest.createDirectory()))
    {
        setExtractStatus("aborted \xe2\x80\x94 destination isn't a usable folder");
        return;
    }

    // Guard: the destination must be outside the source tree. If dest == src or an
    // ancestor of it, the scan prunes everything under dest and finds nothing — and
    // it would also tangle the curated output with the source library.
    if (dest == src || src.isAChildOf(dest))
    {
        setExtractStatus("aborted — destination can't be the source folder");
        return;
    }

    extracting.store(true);
    extractProgress.store(0.0f);
    extractKept.store(0);
    setExtractStatus("scanning…");

    // Wind down any prior (finished) worker before starting a new one.
    extractThread.reset();
    extractThread = std::make_unique<FnThread>([this, src, dest] { runExtraction(src, dest); });
    extractThread->startThread();
}

void LoopNestProcessor::runExtraction(juce::File src, juce::File dest)
{
    const auto outDir = dest;

    // The worker decodes durations off its own format manager so it never races
    // the audio/message thread's shared formatManager.
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    auto* thread = juce::Thread::getCurrentThread();
    auto bail = [thread] { return thread != nullptr && thread->threadShouldExit(); };

    // --- Gather candidates first (excluding our own output + AppleDouble) -----
    juce::Array<juce::File> candidates;
    for (const auto& entry : juce::RangedDirectoryIterator(
             src, true, "*.wav;*.aiff;*.aif;*.mp3", juce::File::findFiles))
    {
        if (bail()) { extracting.store(false); return; }
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
        setExtractStatus("aborted \xe2\x80\x94 can't create the destination folder");
        extracting.store(false);
        return;
    }

    // Sweep any half-written .part leftovers from an interrupted prior run (the
    // copy below goes temp-name -> rename so a kill never leaves a truncated .wav
    // that the dedup would then trust forever).
    for (const auto& stale : outDir.findChildFiles(juce::File::findFiles, false, "*.part"))
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
                    auto destFile = outDir.getChildFile(prefix + base);
                    if (destFile.existsAsFile())
                        ++skipped;
                    else
                    {
                        auto tmpFile = destFile.getSiblingFile(destFile.getFileName() + ".part");
                        if (f.copyFileTo(tmpFile) && tmpFile.moveFileTo(destFile))
                        {
                            ++kept;
                            extractKept.store(kept);
                        }
                        else
                        {
                            tmpFile.deleteFile();
                        }
                    }
                }
            }
        }

        extractProgress.store((float) (i + 1) / (float) juce::jmax(1, total));
        if ((i & 31) == 0)
            setExtractStatus("scanning " + juce::String(i + 1) + "/" + juce::String(total)
                             + "  ·  " + juce::String(kept) + " kept");
    }

    const bool cancelled = bail();
    setExtractStatus(cancelled ? "cancelled"
                               : juce::String(kept) + " added"
                                 + (skipped > 0 ? "  \xc2\xb7  " + juce::String(skipped)
                                                    + " already present"
                                                : juce::String()));

    // Hand the curated folder to the message thread to adopt. The worker never
    // touches sampleFiles itself; finishExtractionIfReady() does the swap. The
    // editor timer polls it AND we callAsync it (lifetime-guarded) so finishing
    // with the window closed still adopts — otherwise a save-and-close mid-scan
    // strands the adoption until the editor reopens.
    if (! cancelled)
    {
        curatedFolder = outDir;
        adoptPending.store(true);
        juce::MessageManager::callAsync([this, alive = aliveFlag]
        {
            if (*alive)
                finishExtractionIfReady();
        });
    }
    else
    {
        extracting.store(false);
    }
}

void LoopNestProcessor::finishExtractionIfReady()
{
    if (! adoptPending.exchange(false))
        return;

    setSampleFolder(curatedFolder);  // rescan + remember + reset spins (message thread)
    extracting.store(false);
}

bool LoopNestProcessor::loadSample(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr)
        return false;

    // Validate before allocating: a corrupt/huge header would otherwise wrap the
    // (int) cast negative or trigger a multi-GB synchronous allocation (seconds of
    // UI freeze; a bad_alloc out of a click handler takes Ableton down with us).
    // 5 min @ 192 kHz stereo ≈ 460 MB — far beyond any loop, cheap insurance.
    constexpr juce::int64 kMaxSeconds = 300;
    const auto maxFrames = (juce::int64) (reader->sampleRate * (double) kMaxSeconds);
    if (reader->numChannels < 1 || reader->numChannels > 64
        || reader->sampleRate <= 0.0
        || reader->lengthInSamples <= 0 || reader->lengthInSamples > maxFrames)
        return false;

    // Read fully into a temp buffer off the audio thread, then swap under lock.
    juce::AudioBuffer<float> temp;
    try
    {
        temp.setSize((int) reader->numChannels, (int) reader->lengthInSamples);
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    reader->read(&temp, 0, (int) reader->lengthInSamples, 0, true, true);

    {
        const juce::SpinLock::ScopedLockType lock(sampleLock);
        sampleBuffer = std::move(temp);
        sampleFileSampleRate = reader->sampleRate;
        playbackActive = false;
        readPos = 0.0;
        character.reset();
    }

    {
        const juce::ScopedLock sl(stateLock);
        currentSample = file;
        currentSampleName = file.getFileName();
        lastRender = juce::File();   // a new sample invalidates the old render
    }
    return true;
}

// === Cash Out: render the shaped trimmed region to a .wav ===

juce::File LoopNestProcessor::renderLoop()
{
    // Snapshot what we need under the lock, then render without holding it.
    juce::AudioBuffer<float> src;
    double srcSR = 44100.0;
    int startFrame = 0, endFrame = 0;
    {
        const juce::SpinLock::ScopedLockType lock(sampleLock);
        if (sampleBuffer.getNumSamples() <= 0)
            return {};
        src.makeCopyOf(sampleBuffer);
        srcSR = sampleFileSampleRate;
        regionFrames(src.getNumSamples(), startFrame, endFrame);
    }

    const int srcChannels = src.getNumChannels();

    // Always render stereo. The audition runs on the host's stereo bus with mono
    // sources duplicated into both channels, and the chain's WIDTH/CENTER stage and
    // stereo reverb only engage at 2 channels — rendering mono would silently drop
    // them and break print-what-you-hear.
    const int outChannels = 2;

    // Render at the source sample rate with varispeed pitch; Ableton's warp
    // realigns tempo on drop. So the readout rate is just the pitch ratio.
    const double pitchRatio = std::pow(2.0, (double) pitchParam->load() / 12.0);
    const float  inGain     = juce::Decibels::decibelsToGain(inputParam->load(),  -60.0f);
    const float  outGain    = juce::Decibels::decibelsToGain(outputParam->load(), -60.0f);

    // Character chain at the source rate — identical processing to the audition.
    CharacterChain renderChar;
    renderChar.prepare(srcSR);
    renderChar.setParams(warpParam->load(), warpRateParam->load(),
                         driveParam->load(), driveToneParam->load(),
                         flutterParam->load(), flutterRateParam->load(),
                         echoParam->load(), reverbParam->load(),
                         echoLoParam->load(), echoHiParam->load(),
                         reverbLoParam->load(), reverbHiParam->load(),
                         eqLoParam->load(), eqHiParam->load(),
                         widthParam->load(), widthLoParam->load(), widthHiParam->load(),
                        reverbDecayParam->load(), echoTimeParam->load(), widthMidParam->load());
    const float mix01 = juce::jlimit(0.0f, 1.0f, mixParam->load() * 0.01f);

    const int regionLen = endFrame - startFrame;
    const int numOut = (int) std::ceil((double) regionLen / pitchRatio);
    if (numOut <= 0)
        return {};

    juce::AudioBuffer<float> out(outChannels, numOut);
    out.clear();

    // The looped audition never resets the chain at the trim seam, so echo/reverb
    // tails ring across the wrap — that steady state is what the user shaped
    // against. A single cold pass would print a dry head and hard-cut the tail
    // (and a region shorter than the echo delay would print no echo at all). So:
    // run discarded priming passes over the region first — enough to cover ~1 s
    // of tail build-up — then capture the final pass with the chain state carried
    // in, exactly like one more wrap of the audition loop.
    const double regionSec   = (double) numOut / srcSR;
    const int    primePasses = juce::jlimit(1, 8, (int) std::ceil(1.0 / juce::jmax(0.001, regionSec)));

    for (int pass = 0; pass <= primePasses; ++pass)
    {
        const bool capture = (pass == primePasses);

        double pos = (double) startFrame;
        for (int n = 0; n < numOut && pos < (double) endFrame; ++n)
        {
            const int   i0 = (int) pos;
            const int   i1 = (i0 + 1 < src.getNumSamples()) ? i0 + 1 : i0;
            const float frac = (float) (pos - (double) i0);

            float s[2] = { 0.0f, 0.0f };
            for (int ch = 0; ch < outChannels; ++ch)
            {
                const int srcCh = juce::jmin(ch, srcChannels - 1);
                const float a = src.getSample(srcCh, i0);
                const float b = src.getSample(srcCh, i1);
                s[ch] = (a + frac * (b - a)) * inGain;   // input gain pre-rack (bakes in)
            }

            renderChar.processEq(s, outChannels);        // master EQ pre-rack (bakes in)
            float dry[2] = { s[0], s[1] };
            renderChar.process(s, outChannels);
            if (capture)
                for (int ch = 0; ch < outChannels; ++ch)
                    out.setSample(ch, n, (dry[ch] + mix01 * (s[ch] - dry[ch])) * outGain);

            pos += pitchRatio;
        }
    }

    // Write to a durable per-user folder, NOT the temp dir: Ableton references the
    // dragged file in place until Collect-All-and-Save, and macOS purges temp
    // content after a few days — a temp render means a saved set silently goes
    // offline later. Names are timestamped + collision-proofed (never reuse or
    // delete an existing path — another instance's armed render, or a saved set,
    // may still point at it). Nothing here auto-prunes for the same reason; the
    // folder is user-visible and self-explanatory.
    auto dir = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                   .getChildFile("LoopNest").getChildFile("Prints");
    dir.createDirectory();
    if (! dir.isDirectory())
        return {};

    auto base = getCurrentSampleFile().getFileNameWithoutExtension();
    if (base.isEmpty()) base = "loop";
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
    auto file = dir.getNonexistentChildFile(base + "_LoopNest_" + stamp, ".wav");

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return {};

    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.get(), srcSR, (unsigned int) outChannels, 24, {}, 0));
    if (writer == nullptr)
        return {};

    stream.release(); // the writer owns the stream now
    writer->writeFromAudioSampleBuffer(out, 0, out.getNumSamples());
    writer.reset();   // flush + close

    {
        const juce::ScopedLock sl(stateLock);
        lastRender = file;   // survives editor close/reopen (PRINT re-arms from it)
    }
    return file;
}

// === State persistence ===
// The parameter tree (apvts) is the master state; the non-parameter data
// (folder, current sample, loop flag) rides along as extra properties on its root.

void LoopNestProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    {
        const juce::ScopedLock sl(stateLock);
        state.setProperty("folder", sampleFolder.getFullPathName(), nullptr);
        state.setProperty("sample", currentSample.getFullPathName(), nullptr);
    }
    state.setProperty("loop", loopEnabled.load(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void LoopNestProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr)
        return;

    auto state = juce::ValueTree::fromXml(*xml);
    if (! state.isValid() || state.getType() != apvts.state.getType())
        return;

    apvts.replaceState(state);

    {
        const juce::ScopedLock sl(stateLock);
        sampleFolder = juce::File(state.getProperty("folder", juce::String()).toString());
    }
    rescanFolder();

    loopEnabled.store((bool) state.getProperty("loop", true));

    const juce::File savedSample(state.getProperty("sample", juce::String()).toString());
    if (! (savedSample.existsAsFile() && loadSample(savedSample)))
    {
        // Recalled state has no sample (or it vanished): don't leave the previous
        // sample playing/armed under state that says otherwise.
        {
            const juce::SpinLock::ScopedLockType lock(sampleLock);
            sampleBuffer.setSize(0, 0);
            playbackActive = false;
            readPos = 0.0;
        }
        const juce::ScopedLock sl(stateLock);
        currentSample = juce::File();
        currentSampleName.clear();
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LoopNestProcessor();
}
