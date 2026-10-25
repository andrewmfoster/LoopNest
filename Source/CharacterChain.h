#pragma once
#include <JuceHeader.h>
#include "Snapshot.h"
#include "ReverbTanks.h"
#include <array>
#include <cmath>
#include <vector>

// Lo-fi character chain (Stage 2 DSP) shared by the live audition (processBlock)
// and the PRINT render (renderLoop) so the exported loop matches what was heard.
// One frame (up to 2 channels) is pushed through at a time; per-channel state
// lives here. Signal order: WARP → FLUTTER → DRIVE → ECHO → REVERB (output level
// is applied by the caller, after this chain). Every control is bypassed cleanly
// at its neutral value, so an all-neutral chain is a true pass-through delayed by
// latency() frames, no colour.
//
// Fixed latency (ruling 3, Andrew 10-18): WARP's and FLUTTER's base delays always
// run, at neutral too (the stage then passes its input delayed by its base, no
// modulation), so the chain's delay is one constant, latency() = the two bases in
// whole frames, at every setting, the panels off included. The processor reports it
// to the host; the caller's dry taps (the MIX dry tap, BYPASS / GAIN MATCH's
// reference) go through delayTaps() to match, and a print takes it back off.
//
// Knob mapping (matches createParameterLayout) — each effect has a secondary axis:
//   warp    0..100 %   slow, deep pitch drift (a warped record) — 0 = steady. 100 % is the
//                      deepest sweep the 16 ms base allows (the 10-17 spike's: ~+90/−100 cents
//                      at rate 40 %, ~+195/−235 at 100 %); 50 % is v2's old 100 % where that
//                      leaves the top half room (±60 cents from rate ~45 % up), else 60 % of
//                      the deepest, so the top half always adds depth; see warpSweepFor
//           rate       how fast the warp sways (lane strip) — slow drift → lurch
//   drive   0..100 %   asymmetric tanh overdrive (grit) — 0 = clean; anti-aliased (2x + ADAA,
//                      ~0.9 to 1.7 samples of delay rising with frequency, nothing reported;
//                      the MIX dry tap follows it, see alignDry)
//           tone       ±tilt on the grit (lane strip) — dark ↔ bright around 1.8 kHz,
//                      level-neutral, 0 = flat
//   flutter 0..100 %   tape wow & flutter pitch wobble — 0 = steady. 0..50 % is v2's old
//                      0..100 % (±35 cents at 50 %, rate 50 %), 100 % ≈ ±100 (flutterDepthFor)
//           rate       wobble speed (lane strip) — half-speed ↔ double-speed
//   echo    0..100 %   tape slapback delay (HF-damped repeats) — 0 = dry
//   reverb  0..100 %   algorithmic space — 0 = dry; type PLATE / HALL (default) / SPRING
//                      (ReverbTanks.h), a type change crossfades the tanks (~30 ms)
//   echo/reverb band   a two-handle band-pass on the wet path (RC20 "FOCUS"
//           style): lo handle = low-cut (high-pass) freq, hi handle = high-cut
//           (low-pass) freq, both 0..1 on a log axis (~20 Hz..20 kHz); the handle's
//           frequency is the cut's -3 dB corner at any sample rate. lo≈0 and
//           hi≈1 each bypass cleanly, so a full-open band is a true pass-through.
//
// Identity intent: WARP is the seasick record-sway (deep + slow), FLUTTER the fine
// tape shimmer (subtle + fast), DRIVE adds grit, ECHO is the tape slapback, REVERB
// places it in a space. WARP and FLUTTER are organic (Andrew, 10-17): 35 % of each
// modulation is a smooth random drift (a seeded spline restarted by reset()), read
// through a cubic Hermite interpolator. Being time-variant, WARP, FLUTTER and every
// REVERB tank make the wet print match the audition's pass (priming passes + 1) counted
// from play start, not every later pass: the audition's wobble and tank modulation run
// on clocks of their own across the loop's wraps. DRIVE is loudness-compensated so turning it up does not
// just get louder: the gain it takes off follows the source's level (see
// driveCompTarget), because a saturator adds far more level to a quiet loop than to a
// hot one.
struct CharacterChain
{
    static constexpr int kMaxCh = 2;
    static constexpr int kToneGrid = 5;     // DRIVE TONE's level-picture corners (see toneCompFor)

    // One cut of a band: a low cut (high-pass) or a high cut (low-pass), two equal
    // first-order sections in series (12 dB/oct, the knee of two one-poles). Each
    // section is a bilinear one-pole, prewarped and placed so the PAIR is -3 dB at the
    // handle's frequency at any sample rate (see cutA1). A first-order bilinear
    // section is set by its pole alone (low-pass b0 = b1 = (1 + a1) / 2, high-pass
    // b0 = -b1 = (1 - a1) / 2), so the coefficients slew as one value per frame
    // (tick) and every point on the way is a valid filter of the same kind.
    // The cut ALWAYS runs, also with its handle in the dead zone (there at the dead
    // zone's edge frequency), and `eng` crossfades its output in and out (~5 ms): at
    // eng 0 the input passes untouched (a true bypass), and engaging it ramps from
    // the input to a filter that has been running all along instead of starting one
    // cold. (A cut started from rest drops out: its settled output is the input's
    // in-band content, not ~0.) Shared by every channel it runs on; state is CutState.
    struct Cut
    {
        bool  highPass = false;                    // true: a low cut
        float a1 = 0.0f, a1Tgt = 0.0f;             // live / target pole term
        float b0 = 1.0f, b1 = 0.0f;                // from a1
        float eng = 0.0f, engTgt = 0.0f;           // 0 = bypassed … 1 = engaged (linear)
        float mixG = 0.0f;                         // eng through a smoothstep: the crossfade gain

        void setB() noexcept
        {
            b0 = highPass ? 0.5f * (1.0f - a1) : 0.5f * (1.0f + a1);
            b1 = highPass ? -b0 : b0;
        }
        void snap() noexcept { a1 = a1Tgt; eng = mixG = engTgt; setB(); }
        // Once per frame: the pole eases toward its target at the chain's ~15 ms slew
        // (landing exactly: a step too small to move it snaps), eng steps linearly.
        inline void tick(float slewA, float engStep) noexcept
        {
            if (a1 != a1Tgt)
            {
                const float n = a1 + slewA * (a1Tgt - a1);
                a1 = (n == a1 || std::abs(a1Tgt - n) < 1.0e-7f) ? a1Tgt : n;
                setB();
            }
            if (eng != engTgt)
            {
                eng  = std::abs(engTgt - eng) <= engStep ? engTgt : eng + (engTgt > eng ? engStep : -engStep);
                mixG = eng * eng * (3.0f - 2.0f * eng);   // no slope corner at either end of the fade
            }
        }
    };

    // One channel's state of a Cut.
    struct CutState
    {
        float x1a = 0.0f, y1a = 0.0f, x1b = 0.0f, y1b = 0.0f;
        inline float process(float in, const Cut& c) noexcept
        {
            const float ya = c.b0 * in + c.b1 * x1a - c.a1 * y1a; x1a = in; y1a = ya;
            const float yb = c.b0 * ya + c.b1 * x1b - c.a1 * y1b; x1b = ya; y1b = yb;
            if (c.mixG <= 0.0f) return in;
            if (c.mixG >= 1.0f) return yb;
            return in + c.mixG * (yb - in);
        }
        void reset() noexcept { x1a = y1a = x1b = y1b = 0.0f; }
    };

    void prepare(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;

        // WARP drift delay line (base + the sway's clamp + margin, ms → samples).
        const int nWarp = (int) (sampleRate * 0.001 * (kWarpBaseMs + kWarpSwayMaxMs + 8.0)) + 4;
        for (auto& w : warpLine)
            w.assign((size_t) nWarp, 0.0f);
        warpSize = nWarp;
        warpLeakA = 1.0 - std::exp(-1.0 / (kWarpLeakSec * sampleRate));
        warpDrift.setRate(1.5 * warpRateHz, sampleRate);   // (setParams moves the rates with RATE)
        wowDrift.setRate(1.5 * kWowRateHz * flutterRateMul, sampleRate);
        flutDrift.setRate(1.5 * kFlutRateHz * flutterRateMul, sampleRate);

        // FLUTTER wobble delay line (40 ms, ≫ max wobble).
        const int nWob = (int) (sampleRate * 0.04) + 4;
        for (auto& d : delay)
            d.assign((size_t) nWob, 0.0f);
        delaySize = nWob;
        flutLeakA = 1.0 - std::exp(-1.0 / (kFlutterLeakSec * sampleRate));
        warpGlideStep = (float) (1.0 / (kWarpLeakSec * sampleRate));
        flutGlideStep = (float) (1.0 / (kFlutterLeakSec * sampleRate));

        // The fixed latency: each base in whole frames (so the neutral read is exact),
        // and the dry taps' delay lines to match.
        warpBaseN = juce::jmax(1, (int) std::lround(kWarpBaseMs * 0.001 * sampleRate));
        flutBaseN = juce::jmax(1, (int) std::lround(kBaseDelayMs * 0.001 * sampleRate));
        for (auto& tap : tapLine)
            for (auto& t : tap)
                t.assign((size_t) latency(), 0.0f);
        tankDcR = (float) std::exp(-juce::MathConstants<double>::twoPi * kTankDcHz / sampleRate);

        // ECHO slapback delay line, sized to the MAX time; the live time is a read
        // offset (echoDelay) behind the write head, set in setParams.
        echoLen = juce::jmax(2, (int) (kEchoTimeMaxMs * 0.001 * sampleRate) + 4);
        for (auto& e : echoBuf)
            e.assign((size_t) echoLen, 0.0f);
        echoDelayTgt = juce::jlimit(1.0, (double) echoLen - 1.0, kEchoTimeMs * 0.001 * sampleRate);
        echoDelayF   = echoDelayTgt;

        // DRIVE TONE: the tilt's pivot, prewarped (bilinear, so the pivot and both
        // shelves land exactly at any rate), its level picture's high-passes on the
        // grid of the tilt's own corners (see toneCompFor), and the picture's K-weighting.
        tonePivotK = std::tan(juce::MathConstants<double>::pi * kDriveToneHz / sampleRate);
        for (int k = 0; k < kToneGrid; ++k)
        {
            const double w = tonePivotK * std::pow(10.0, kDriveToneDb * (k - kToneGrid / 2) / (0.5 * (kToneGrid - 1)) / 20.0);
            toneGridB0[(size_t) k] = (float) (1.0 / (1.0 + w));
            toneGridA1[(size_t) k] = (float) ((w - 1.0) / (1.0 + w));
        }
        kWeighting(sampleRate);
        toneLevelA = 1.0 - std::exp(-1.0 / (kToneLevelSec * sampleRate));
        toneIn = 0.0;
        toneHpLevel.fill(0.0);
        toneCached = 2.0f;                               // force the tilt's coeff compute
        // DRIVE DC blocker (wet path) and the level statistics behind its loudness
        // compensation: weights grow by this factor per frame, which is an
        // exponential forgetting of old audio without touching every bin per frame.
        driveDcR       = (float) std::exp(-juce::MathConstants<double>::twoPi * kDriveDcHz / sampleRate);
        driveLevelGrow = std::exp(1.0 / (kDriveLevelSec * sampleRate));
        driveCompA     = (float) (1.0 - std::exp(-1.0 / (0.002 * sampleRate)));
        {
            // DRIVE's lift (kDriveLift*): an RBJ high shelf, its corner a fraction of the rate.
            const double A  = std::pow(10.0, kDriveLiftDb / 40.0), w0 = juce::MathConstants<double>::twoPi * kDriveLiftAt;
            const double cw = std::cos(w0), al = std::sin(w0) / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / kDriveLiftS - 1.0) + 2.0);
            const double sq = 2.0 * std::sqrt(A) * al, a0 = (A + 1.0) - (A - 1.0) * cw + sq;
            drvLiftB = { (float) (A * ((A + 1.0) + (A - 1.0) * cw + sq) / a0), (float) (-2.0 * A * ((A - 1.0) + (A + 1.0) * cw) / a0),
                         (float) (A * ((A + 1.0) + (A - 1.0) * cw - sq) / a0) };
            drvLiftA = { (float) (2.0 * ((A - 1.0) - (A + 1.0) * cw) / a0), (float) (((A + 1.0) - (A - 1.0) * cw - sq) / a0) };
        }
        for (auto& side : driveLevel) side.fill({});
        driveLevelW = 1.0;

        // ECHO HF-damp one-pole — fixed Hz, so computed once here, not per sample.
        echoDampCoeff = onePoleCoeff(kEchoDampHz);
        driveCached = echoCached = reverbCached = -1.0f;   // force coeff recompute

        // Per-frame parameter slew (declick): setParams() writes targets once per
        // block; process() eases the live values toward them so amounts/gains never
        // step at block boundaries. The echo tap slews slower (a tape-style bend
        // beats a click when TIME moves).
        smoothA    = (float)  (1.0 - std::exp(-1.0 / (0.015 * sampleRate)));
        offStepA   = (float)  (1.0 / (0.015 * sampleRate));   // a panel switched off: ≤ 15 ms to neutral
        delaySlewA = 1.0 - std::exp(-1.0 / (0.040 * sampleRate));
        cutEngStep = (float)  (1.0 / (kCutFadeSec * sampleRate));
        reverbQuietLen = juce::jmax(1, (int) (kReverbQuietSec * sampleRate));
        revFadeStep    = (float) (1.0 / (kReverbTypeFadeSec * sampleRate));
        revInRamp      = 0.0f;
        snapNext   = true;   // first setParams() after prepare snaps live = target

        // REVERB's three tanks, all allocated here (only the live one runs, both during a
        // type crossfade), and the decay their tails are tuned to (forced on the next frame).
        plate.prepare(sampleRate);
        hall.prepare(sampleRate);
        spring.prepare(sampleRate);
        revDecayCached = -1.0f;
        reset();
    }

    // A held history step's knobs, landing on the bar line as it is heard (H1), each part
    // snapped (every slewed amount and filter on its target at once, a restored sound
    // exact on its first frame, as the first setParams() after prepare() does). The voice's
    // line enters the chain now, but the old loop's last latency() frames are still inside
    // WARP's and FLUTTER's bases: they must play out on the old knobs. The EQ runs before the
    // bases and takes the new ones at once; WARP's output reaches the line warpBaseN frames
    // on, FLUTTER's and every later stage's latency() frames on, and each part lands then,
    // snapped (process()). Until the last part lands the caller holds its setParams() off
    // (lineHeld()).
    void landAtLine(const loopnest::CharacterParams& next)
    {
        lineParams = next;
        lineWarpIn = warpBaseN;
        lineRestIn = latency();
        auto now = applied;
        now.eqLo = next.eqLo;
        now.eqHi = next.eqHi;
        snapNext = true;
        setParams(now);
    }
    bool lineHeld() const noexcept { return lineRestIn >= 0; }

    void reset()
    {
        echoDampState.fill(0.0f);
        toneResetFilters();
        driveDcX.fill(0.0f);
        driveDcY.fill(0.0f);
        driveClearOs();
        driveLevelToPrior();
        driveCompFor = -1.0f;                           // next engaged frame computes + snaps
        dryMixSnap   = true;                            // alignDry's MIX weight lands, as on a fresh chain
        for (auto& h : echoHp)   h.reset();
        for (auto& h : reverbHp) h.reset();
        for (auto& l : echoLp)   l.reset();
        for (auto& l : reverbLp) l.reset();
        for (auto& h : eqHp)     h.reset();
        for (auto& l : eqLp)     l.reset();
        widthSideHp.reset();
        widthSideLp.reset();
        for (auto& w : warpLine) std::fill(w.begin(), w.end(), 0.0f);
        for (auto& d : delay)    std::fill(d.begin(), d.end(), 0.0f);
        for (auto& e : echoBuf)  std::fill(e.begin(), e.end(), 0.0f);
        for (auto& tap : tapLine)
            for (auto& t : tap) std::fill(t.begin(), t.end(), 0.0f);
        tapPos    = 0;
        lineWarpIn = lineRestIn = -1;
        warpGlide = flutGlide = -1.0f;
        warpWrite = 0;
        warpPhase = 0.0;
        warpSway  = 0.0;
        warpShapePrev = 0.0;
        flutSway  = 0.0;
        flutShapePrev = 0.0;
        warpDrift.restart(kWarpSeed);         // the drifts restart too: a print primed from a
        wowDrift.restart(kWowSeed);           // fresh chain replays the audition's first passes
        flutDrift.restart(kFlutSeed);
        writePos  = 0;
        echoWrite = 0;
        wowPhase  = 0.0;
        flutPhase = 0.0;
        plate.reset();
        hall.reset();
        spring.reset();
        revType = revTypeTgt;
        revFade = -1.0f;
        revInRamp = 0.0f;       // a cleared tank's input ramps in again (D-M1), from the frame
        revRampHold = latency();// the input reaches it: until then it hears the bases' silence
        tankDcX.fill(0.0f);
        tankDcY.fill(0.0f);
        reverbRinging = false;
        reverbQuiet   = 0;
    }

    // The chain's fixed delay in frames at the prepared rate (see the header): the input
    // of a neutral chain comes out this many frames later, exactly.
    int latency() const noexcept { return warpBaseN + flutBaseN; }

    // The caller's two dry taps through latency() frames of delay, one call a frame
    // (after process()): `src` (BYPASS / GAIN MATCH's reference) and `dry` (the MIX dry
    // tap, before alignDry). So dry and wet line up at every MIX, and BYPASS plays on
    // the same reported delay.
    void delayTaps(float* src, float* dry, int nch) noexcept
    {
        if (tapLine[0][0].empty())    // not prepared: no latency, nothing to line up
            return;
        nch = juce::jlimit(1, kMaxCh, nch);
        for (int ch = 0; ch < nch; ++ch)
        {
            float* lines[2] = { src, dry };
            for (int t = 0; t < 2; ++t)
            {
                float& slot = tapLine[(size_t) t][(size_t) ch][(size_t) tapPos];
                const float out = slot;
                slot = lines[t][ch];
                lines[t][ch] = out;
            }
        }
        if (++tapPos >= latency()) tapPos = 0;
    }

    // How long the live tail rings (s): REVERB's T30 and ECHO's repeats to 60 dB down,
    // the longer of the two engaged; 0 = no tail. A wet print primes this long (D-M4).
    static double tailSec(const loopnest::CharacterParams& p) noexcept
    {
        using P = loopnest::CharacterParams;
        double t = 0.0;
        if (p.on[P::kReverb] && p.reverb > 0.0f)
            t = reverbT30For(juce::jlimit(0, kNumReverbTypes - 1, (int) std::lround(p.reverbType)),
                             juce::jlimit(0.0f, 1.0f, p.reverbDecay * 0.01f));
        if (p.on[P::kEcho] && p.echo > 0.0f)
        {
            const double fb = (double) juce::jlimit(0.0f, 1.0f, p.echo * 0.01f) * kEchoFbMax;
            const double ms = kEchoTimeMinMs + (kEchoTimeMaxMs - kEchoTimeMinMs)
                                               * juce::jlimit(0.0, 1.0, (double) p.echoTime * 0.01);
            if (fb > 0.0)
                t = juce::jmax(t, 0.001 * ms * (1.0 + 3.0 / -std::log10(fb)));   // the tap, then 60 dB of repeats
        }
        return t;
    }

    // Live-coefficient setters from the raw param values (called once per block).
    // Each effect's secondary axis comes in alongside its main knob:
    //   warpRatePct / flutterRatePct  0..100 % (rate)
    //   driveTonePct                  −100..+100 % (tilt: − dark, + bright)
    //   echo/reverb band: two 0..1 handle positions on a log frequency axis —
    //       lo = low-cut (high-pass), hi = high-cut (low-pass). lo≈0 / hi≈1 bypass.
    // The one call site shape: every caller reads a CharacterParams (capture order).
    //
    // Panel power (p.on): an OFF stage gets its neutral values here, so the whole
    // stage is a true bypass while the knobs keep their settings. Neutral per stage:
    // WARP / FLUTTER / DRIVE / ECHO / REVERB = primary 0 (every secondary of theirs
    // acts only through the primary: rate, tone, band, time, decay); WIDTH = width 0
    // AND widthMid 0 %, because CENTER acts on its own at width 0. An off stage also
    // ramps to neutral linearly (≤ 15 ms, process()) instead of the exponential
    // slew, so its tail is CUT, and once it lands its echo ring / reverb tank /
    // filter state is cleared (no stale tail when it comes back on).
    void setParams(const loopnest::CharacterParams& p)
    {
        using P = loopnest::CharacterParams;
        applied = p;
        auto e = p;
        if (! p.on[P::kWarp])    e.warp    = 0.0f;
        if (! p.on[P::kFlutter]) e.flutter = 0.0f;
        if (! p.on[P::kDrive])   e.drive   = 0.0f;
        if (! p.on[P::kEcho])    e.echo    = 0.0f;
        if (! p.on[P::kReverb])  e.reverb  = 0.0f;
        if (! p.on[P::kWidth])   { e.width = 0.0f; e.widthMid = 0.0f; }
        for (int i = 0; i < P::kNumStages; ++i)
        {
            stageOff[(size_t) i] = ! p.on[(size_t) i];
            if (p.on[(size_t) i]) stageCleared[(size_t) i] = false;   // the next off clears again
        }
        setParams(e.warp, e.warpRate, e.drive, e.driveTone, e.flutter, e.flutterRate,
                  e.echo, e.reverb, e.echoLo, e.echoHi, e.reverbLo, e.reverbHi,
                  e.eqLo, e.eqHi, e.width, e.widthLo, e.widthHi,
                  e.reverbDecay, e.echoTime, e.widthMid, e.reverbType);
    }

    void setParams(float warpPct, float warpRatePct,
                   float drivePct, float driveTonePct,
                   float flutterPct, float flutterRatePct,
                   float echoPct, float reverbPct,
                   float echoLo01 = 0.0f, float echoHi01 = 1.0f,
                   float reverbLo01 = 0.0f, float reverbHi01 = 1.0f,
                   float eqLo01 = 0.0f, float eqHi01 = 1.0f,
                   float widthPct = 0.0f, float widthLo01 = 0.0f, float widthHi01 = 1.0f,
                   float reverbDecayPct = 50.0f, float echoTimePct = 44.0f,
                   float widthMidPct = 0.0f, float reverbTypeIdx = (float) kHall)
    {
        // WIDTH (band-limited stereo): the two handles band-pass the SIDE (M/S) signal;
        // in-band side is widened, out-of-band side folds toward mono as WIDTH rises.
        // width01==0 → the stage passes the signal untouched (its band filters keep running). True-bypass band edges.
        // CENTER (tertiary): a ±dB gain on the MID (M) of the M/S split — independent of
        // width amount; widthMid==1 (0 %) is unity (no level change).
        widthTgt = juce::jlimit(0.0f, 1.0f, widthPct * 0.01f);
        bandTargets(widthLo01, widthHi01, widthHpCut, widthLpCut);
        widthMidTgt = std::pow(10.0f, (juce::jlimit(-100.0f, 100.0f, widthMidPct) * 0.01f
                                       * kWidthMidDb) / 20.0f);

        // Master EQ band-pass (pre-rack): same two-handle low-cut + high-cut math
        // as the wet bands, but the caller runs processEq() on the WHOLE signal
        // before the dry/wet tap (so it shapes dry + rack alike). True-bypass open.
        bandTargets(eqLo01, eqHi01, eqHpCut, eqLpCut);

        // Two-handle wet band-pass (echo + reverb): independent low-cut (HP) and
        // high-cut (LP); each bypasses cleanly at its open edge.
        bandTargets(echoLo01,   echoHi01,   echoHpCut,   echoLpCut);
        bandTargets(reverbLo01, reverbHi01, reverbHpCut, reverbLpCut);

        warpTgt    = juce::jlimit(0.0f, 1.0f, warpPct * 0.01f);
        // RATE: the sway's speed (the depth in cents is the knob's, see warpSweepFor).
        // The drift's spline runs at 1.5× the sway's rate.
        const double warpHz = warpRateHzFor(warpRatePct);
        if (warpHz != warpRateHz)
        {
            warpRateHz = warpHz;
            warpDrift.setRate(1.5 * warpRateHz, sampleRate);
        }

        driveTgt     = juce::jlimit(0.0f, 1.0f, drivePct   * 0.01f);
        driveToneTgt = juce::jlimit(-1.0f, 1.0f, driveTonePct * 0.01f);
        flutterTgt   = juce::jlimit(0.0f, 1.0f, flutterPct * 0.01f);
        // RATE: exponential around 50 % = 1× (0 % = ½×, 100 % = 2×), the drifts with it.
        const float flutMul = std::pow(2.0f, (juce::jlimit(0.0f, 100.0f, flutterRatePct) - 50.0f) / 50.0f);
        if (flutMul != flutterRateMul)
        {
            flutterRateMul = flutMul;
            wowDrift.setRate(1.5 * kWowRateHz * flutterRateMul, sampleRate);
            flutDrift.setRate(1.5 * kFlutRateHz * flutterRateMul, sampleRate);
        }
        echoTgt    = juce::jlimit(0.0f, 1.0f, echoPct    * 0.01f);
        reverbTgt  = juce::jlimit(0.0f, 1.0f, reverbPct  * 0.01f);
        reverbDecay01 = juce::jlimit(0.0f, 1.0f, reverbDecayPct * 0.01f);
        revTypeTgt = juce::jlimit(0, kNumReverbTypes - 1, (int) std::lround(reverbTypeIdx));

        // ECHO TIME (tertiary): free slapback→short-dub time in ms (no tempo sync — the
        // echo is baked as character, not a grid delay; Ableton's own delay handles rhythm
        // after the drop). Maps to a read offset behind the (max-length) ring's write head;
        // the live tap (echoDelayF) slews toward this in process() — a tape-style bend
        // instead of a hard tap jump while TIME moves.
        const double echoMs = kEchoTimeMinMs + (kEchoTimeMaxMs - kEchoTimeMinMs)
                                               * juce::jlimit(0.0, 1.0, (double) echoTimePct * 0.01);
        echoDelayTgt = juce::jlimit(1.0, (double) juce::jmax(2, echoLen) - 1.0,
                                    echoMs * 0.001 * sampleRate);

        // First call after prepare(): start AT the targets — a render (or a fresh
        // editor session) must not fade its parameters in from zero.
        if (snapNext)
        {
            warp01 = warpTgt; drive01 = driveTgt; driveTone = driveToneTgt;
            flutter01 = flutterTgt; echo01 = echoTgt; reverb01 = reverbTgt;
            width01 = widthTgt; widthMid = widthMidTgt;
            warpEng = warpTgt > 0.0f ? 1.0f : 0.0f;
            driveEng = driveTgt > 0.0f ? 1.0f : 0.0f;
            flutterEng = flutterTgt > 0.0f ? 1.0f : 0.0f;
            echoDelayF = echoDelayTgt;
            revDecay = reverbDecay01;
            // (revInRamp is left as it is: a cleared tank ramps its input in, D-M1. A print's
            // first pass is a priming one, so its head never reaches the file.)
            for (auto* c : { &eqHpCut, &eqLpCut, &echoHpCut, &echoLpCut,
                             &reverbHpCut, &reverbLpCut, &widthHpCut, &widthLpCut })
                c->snap();
            // REVERB TYPE lands at once too. A tail of another type still ringing is
            // dropped (a snap lands under the caller's own declick), so no tank is left
            // holding a stale tail for later.
            if (revType != revTypeTgt || revFade >= 0.0f)
            {
                if (revType != revTypeTgt) tank(revType).reset();
                if (revFade >= 0.0f) tank(revFrom).reset();
                revType = revTypeTgt;
                revFade = -1.0f;
            }
            dryMixSnap = true;      // alignDry's MIX weight lands on the next call too
            snapNext = false;
        }
        // REVERB: the amount knob (reverb01) sets wet level; DECAY (tertiary) sets the
        // tail length independently, TYPE picks the tank (see process() 4. REVERB).
    }

    // Master EQ — a two-handle band-pass run PRE-RACK by the caller (before the
    // dry/wet tap), so it sits between INPUT and FLUTTER in the chain and shapes
    // both the dry blend and the rack feed. True-bypass at open handles (lo≈0/hi≈1).
    void processEq(float* x, int nch)
    {
        nch = juce::jlimit(1, kMaxCh, nch);
        eqHpCut.tick(smoothA, cutEngStep);   // the EQ's handles slew here (once per frame,
        eqLpCut.tick(smoothA, cutEngStep);   // the caller runs this before process())
        for (int ch = 0; ch < nch; ++ch)
        {
            float v = eqHp[(size_t) ch].process(x[ch], eqHpCut);
            v = eqLp[(size_t) ch].process(v, eqLpCut);
            x[ch] = v;
        }
    }

    // The master MIX's dry tap, given DRIVE's linear phase. DRIVE's 2x path delays
    // its output by ~0.9 to 1.7 samples, more toward Nyquist, so the raw tap blended
    // against it would comb (a MIX of 50 % notched ~-39 dB at 16.6 kHz). The caller
    // runs this on its dry tap after process(), with the frame's MIX, and the tap goes
    // through the same linear path the stage's dry share takes (driveLinear), faded by
    // the same engage value: dry and wet share one phase at every moment. At MIX 0
    // nothing is blended against it, so it fades back to the raw tap (driveDryMix, at
    // the chain's slew) and MIX 0 stays the clean tap exactly; the fade itself briefly
    // blends the two, as DRIVE's engage fade does. While DRIVE is idle it returns at
    // once and the tap is untouched. Not for BYPASS (its tap stays raw).
    void alignDry(float* d, int nch, float mix01) noexcept
    {
        const float mixTgt = mix01 > 0.0f ? 1.0f : 0.0f;
        const float eng = driveEng;
        if (eng <= 0.0f || dryMixSnap)
        {
            driveDryMix = mixTgt;   // nothing of the shadow is heard: land at once
            dryMixSnap  = false;
        }
        else if (driveDryMix != mixTgt)
        {
            driveDryMix += smoothA * (mixTgt - driveDryMix);
            if (std::abs(mixTgt - driveDryMix) < 1.0e-5f) driveDryMix = mixTgt;
        }
        const float g = eng * driveDryMix;
        if (g <= 0.0f)
        {
            driveDryIdle = true;
            return;
        }
        nch = juce::jlimit(1, kMaxCh, nch);
        if (driveDryIdle)           // from idle: settled on the current tap, as the stage starts
        {
            for (int ch = 0; ch < kMaxCh; ++ch)
                driveWarmLin(driveDryLin[(size_t) ch], ch < nch ? d[ch] : 0.0f);
            driveDryIdle = false;
        }
        for (int ch = 0; ch < nch; ++ch)
        {
            float s2[2];
            const float a = driveLinear(driveDryLin[(size_t) ch], d[ch], s2);
            d[ch] = g >= 1.0f ? a : d[ch] + g * (a - d[ch]);
        }
    }

    // Process one frame in place. `x` holds `nch` (1 or 2) interpolated samples.
    void process(float* x, int nch)
    {
        if (warpSize <= 0)        // not prepared (no lines, no tanks): the input as it is (L1)
            return;
        nch = juce::jlimit(1, kMaxCh, nch);
        const bool rampHeld = revRampHold > 0;    // (REVERB's input rise, see 4. REVERB)
        if (rampHeld) --revRampHold;
        if (lineRestIn >= 0)                      // a held step landing part by part (landAtLine)
        {
            using P = loopnest::CharacterParams;
            if (lineWarpIn >= 0 && lineWarpIn-- == 0)
            {
                auto now = applied;
                now.warp = lineParams.warp;
                now.warpRate = lineParams.warpRate;
                now.on[P::kWarp] = lineParams.on[P::kWarp];
                snapNext = true;
                setParams(now);
            }
            if (lineRestIn-- == 0)
            {
                snapNext = true;
                setParams(lineParams);
            }
        }

        // Per-frame slew of every audible amount toward its setParams() target —
        // block-rate steps become short ramps (declick). warpEng/flutterEng/driveEng
        // are engage crossfades: those stages replace the signal with a delayed one
        // (DRIVE's by ~0.9 to 1.7 samples), so on/off must fade wet/dry, not snap.
        // (Snapped once within 1e-5 of target — the exponential approach never lands
        // exactly, and the stage-coefficient caches below key on equality.)
        auto slew = [this](float& v, float t)
        { v += smoothA * (t - v); if (std::abs(t - v) < 1.0e-5f) v = t; };
        // A panel switched OFF: at least a linear step toward neutral each frame, so
        // the stage lands exactly on neutral within ~15 ms (the tail is cut, not a
        // ~170 ms exponential creep), still a ramp rather than a click.
        auto slewOff = [this](float& v, float t, bool off)
        {
            if (! off) { v += smoothA * (t - v); if (std::abs(t - v) < 1.0e-5f) v = t; return; }
            const float d = t - v, step = juce::jmax(std::abs(d) * smoothA, offStepA);
            v = std::abs(d) <= step ? t : v + (d > 0.0f ? step : -step);
        };
        using P = loopnest::CharacterParams;
        slewOff(warp01, warpTgt, stageOff[P::kWarp]);
        slewOff(flutter01, flutterTgt, stageOff[P::kFlutter]);
        slewOff(drive01, driveTgt, stageOff[P::kDrive]);
        slewOff(driveEng, driveTgt > 0.0f ? 1.0f : 0.0f, stageOff[P::kDrive]);
        slew(driveTone, driveToneTgt);
        slewOff(echo01, echoTgt, stageOff[P::kEcho]);
        slewOff(reverb01, reverbTgt, stageOff[P::kReverb]);
        slew(revDecay, reverbDecay01);
        slewOff(width01, widthTgt, stageOff[P::kWidth]);
        slewOff(widthMid, widthMidTgt, stageOff[P::kWidth]);
        // The knob turned to 0 with the panel on: the wet stays as it is while the sway
        // glides out (warpGlide / flutGlide, below), then drops in one frame; only an
        // engage (or a panel OFF) crossfades.
        if (! (warpEng > 0.0f && warpTgt <= 0.0f && ! stageOff[P::kWarp]))
            slewOff(warpEng, warpTgt > 0.0f ? 1.0f : 0.0f, stageOff[P::kWarp]);
        if (! (flutterEng > 0.0f && flutterTgt <= 0.0f && ! stageOff[P::kFlutter]))
            slewOff(flutterEng, flutterTgt > 0.0f ? 1.0f : 0.0f, stageOff[P::kFlutter]);
        // Band handles: each cut's pole slews and its dead-zone crossfade steps (Cut).
        for (auto* c : { &echoHpCut, &echoLpCut, &reverbHpCut, &reverbLpCut, &widthHpCut, &widthLpCut })
            c->tick(smoothA, cutEngStep);

        // An OFF stage that has landed on neutral drops its state once (no
        // allocation: fills and resets in place), so switching it back on starts
        // clean instead of replaying the tail it had when it went off. The echo ring
        // keeps running after this (it writes the dry input at echo 0), exactly as a
        // knob turned up from 0 would find it.
        if (stageOff[P::kEcho] && ! stageCleared[P::kEcho] && echo01 == 0.0f)
        {
            for (auto& e : echoBuf) std::fill(e.begin(), e.end(), 0.0f);
            echoDampState.fill(0.0f);
            for (auto& h : echoHp) h.reset();
            for (auto& l : echoLp) l.reset();
            stageCleared[P::kEcho] = true;
        }
        if (stageOff[P::kReverb] && ! stageCleared[P::kReverb] && reverb01 == 0.0f)
        {
            reverbClearTanks();
            for (auto& h : reverbHp) h.reset();
            for (auto& l : reverbLp) l.reset();
            reverbRinging = false;
            reverbQuiet   = 0;
            stageCleared[P::kReverb] = true;
        }
        if (stageOff[P::kDrive] && ! stageCleared[P::kDrive] && drive01 == 0.0f && driveEng == 0.0f)
        {
            toneResetFilters();
            driveDcX.fill(0.0f);
            driveDcY.fill(0.0f);
            driveClearOs();
            stageCleared[P::kDrive] = true;
        }
        if (stageOff[P::kWidth] && ! stageCleared[P::kWidth] && width01 == 0.0f && widthMid == 1.0f)
        {
            widthSideHp.reset();
            widthSideLp.reset();
            stageCleared[P::kWidth] = true;
        }
        echoDelayF += delaySlewA * (echoDelayTgt - echoDelayF);
        if (std::abs(echoDelayTgt - echoDelayF) < 0.05) echoDelayF = echoDelayTgt;

        // 0. WARP — slow, deep pitch drift (a warped record). A slow sine with 35 % of
        //    it a smooth random drift (warpDrift, a spline at 1.5× the rate) modulates a
        //    cubic Hermite read of its own delay line: deeper and slower than FLUTTER, so
        //    the loop sways/melts rather than shimmers. Depth and rate are the two axes
        //    (knob + lane strip). The ring is ALWAYS written and advanced — engaging
        //    mid-play must read recent audio, not a frozen or starved line — and the wet
        //    (base-delayed) path crossfades in via warpEng. Fully faded out = the base
        //    read alone (see below). The wet is read for as long as any of it
        //    is mixed (warpEng > 0: its slew lands on 0 from 1e-5), so disengaging never
        //    moves the read while wet remains; FLUTTER does the same with flutterEng.
        //    The sway (warpSway, ms) is A·shape with A the knob's and rate's sweep
        //    (warpSweepFor, × kWarpRangeMatch), but a knob or RATE move must not jump it
        //    (a jump in a delay is a pitch spike): each frame it moves by what A·shape
        //    moves at the current A, which puts the new depth in the pitch at once, and
        //    eases onto the new sweep over ~kWarpLeakSec. What is read is held inside
        //    the base by a soft knee (warpKnee), so WARP never adds delay past kWarpBaseMs.
        //    The base itself always runs (latency()): fully faded out, the stage is its
        //    input exactly warpBaseN frames late, and the wet crossfades against that read.
        if (warpSize > 0)
        {
            for (int ch = 0; ch < nch; ++ch)
                warpLine[(size_t) ch][(size_t) warpWrite] = x[ch];
            const int baseAt = warpWrite >= warpBaseN ? warpWrite - warpBaseN : warpWrite - warpBaseN + warpSize;

            if (warp01 != warpCachedAmt || warpRateHz != warpCachedHz)
            {
                warpCachedAmt = warp01;
                warpCachedHz  = warpRateHz;
                warpAmpMs     = kWarpRangeMatch * warpSweepFor(warp01, warpRateHz);
            }
            const double shape = (1.0 - kWarpRandMix) * std::sin(warpPhase) + kWarpRandMix * (double) warpDrift.next();
            if (warpEng > 0.0f)
            {
                warpSway += warpAmpMs * (shape - warpShapePrev);
                warpSway += warpLeakA * (warpAmpMs * shape - warpSway);
                warpSway  = juce::jlimit(-2.0 * kWarpBaseMs, 2.0 * kWarpBaseMs, warpSway);
            }
            else            // idle (no wet left at all): on the sway the knob is headed for, so engaging starts there
                warpSway = kWarpRangeMatch * warpSweepFor(warpTgt, warpRateHz) * shape;
            warpShapePrev = shape;

            // Knob to 0, panel on (item 6 of the 10-18 review): the sway is taken to exactly 0
            // on a raised cosine over kWarpLeakSec, the wet whole; at 0 the read is the base
            // read, so the wet then drops out in one frame without a step. A knob raised
            // again mid-glide carries on from where the glide had the sway.
            const double warpEnv = glideStep(warpGlide, warpSway, warpEng > 0.0f && warpTgt <= 0.0f && ! stageOff[P::kWarp],
                                             warpGlideStep);
            if (warpGlide >= 1.0f)
            {
                warpGlide = -1.0f;
                warpSway  = 0.0;
                warpEng   = 0.0f;
            }
            for (int ch = 0; ch < nch; ++ch)
                x[ch] = warpLine[(size_t) ch][(size_t) baseAt];
            if (warpEng > 0.0f)
            {
                const double offset = (double) warpBaseN + warpKnee(warpSway * warpEnv) * 0.001 * sampleRate;
                for (int ch = 0; ch < nch; ++ch)
                {
                    const float wet = hermiteAt(warpLine[(size_t) ch], warpSize, warpWrite, offset);
                    x[ch] = x[ch] + warpEng * (wet - x[ch]);
                }
            }
            warpWrite = (warpWrite + 1) % warpSize;

            warpPhase += juce::MathConstants<double>::twoPi * warpRateHz / sampleRate;
            if (warpPhase >= juce::MathConstants<double>::twoPi)
                warpPhase -= juce::MathConstants<double>::twoPi;
        }

        // 1. FLUTTER — modulated fractional delay (tape pitch wobble): wow and flutter
        //    sines, 35 % of each a random drift of its own, through a cubic Hermite read.
        //    The depth is the knob's through flutterDepthFor (rate only speeds it up).
        //    Same always-write + engage-crossfade scheme as WARP, and the same base: fully
        //    faded out the stage is its input exactly flutBaseN frames late (latency()).
        //    A depth move never jumps the read either (D-M3: a step chirped up to +534 /
        //    -360 cents): the sway (flutSway, ms) moves each frame by what A·shape moves
        //    at the current A and eases onto the new depth over ~kFlutterLeakSec, as
        //    WARP's does. Held inside the base by a soft knee (flutKnee), as WARP's read is.
        if (delaySize > 0)
        {
            for (int ch = 0; ch < nch; ++ch)
                delay[(size_t) ch][(size_t) writePos] = x[ch];
            const int baseAt = writePos >= flutBaseN ? writePos - flutBaseN : writePos - flutBaseN + delaySize;

            // The drifts advance every frame, engaged or not (like the phases).
            const double wowR  = (double) wowDrift.next();
            const double flutR = (double) flutDrift.next();
            if (flutter01 != flutterCached)
            {
                flutterCached = flutter01;
                flutterAmpMs  = kFlutterRangeMatch * flutterDepthFor(flutter01);
            }
            const double wow   = (1.0 - kFlutterRandMix) * std::sin(wowPhase)  + kFlutterRandMix * wowR;
            const double flut  = (1.0 - kFlutterRandMix) * std::sin(flutPhase) + kFlutterRandMix * flutR;
            const double shape = wow + kFlutToWow * flut;
            if (flutterEng > 0.0f)
            {
                flutSway += flutterAmpMs * (shape - flutShapePrev);
                flutSway += flutLeakA * (flutterAmpMs * shape - flutSway);
                flutSway  = juce::jlimit(-2.0 * kBaseDelayMs, 2.0 * kBaseDelayMs, flutSway);
            }
            else            // idle: on the sway the knob is headed for, so engaging starts there
                flutSway = kFlutterRangeMatch * flutterDepthFor(flutterTgt) * shape;
            flutShapePrev = shape;

            const double flutEnv = glideStep(flutGlide, flutSway, flutterEng > 0.0f && flutterTgt <= 0.0f && ! stageOff[P::kFlutter],
                                             flutGlideStep);   // (as WARP's, over kFlutterLeakSec)
            if (flutGlide >= 1.0f)
            {
                flutGlide  = -1.0f;
                flutSway   = 0.0;
                flutterEng = 0.0f;
            }
            for (int ch = 0; ch < nch; ++ch)
                x[ch] = delay[(size_t) ch][(size_t) baseAt];
            if (flutterEng > 0.0f)
            {
                // Held inside the base by a soft knee, as WARP's read is (a depth step up can
                // swing the sway past it for a moment; a hard clamp kinked the pitch there).
                const double offset = (double) flutBaseN + flutKnee(flutSway * flutEnv) * 0.001 * sampleRate;
                for (int ch = 0; ch < nch; ++ch)
                {
                    const float wet = hermiteAt(delay[(size_t) ch], delaySize, writePos, offset);
                    x[ch] = x[ch] + flutterEng * (wet - x[ch]);
                }
            }
            writePos = (writePos + 1) % delaySize;

            wowPhase  += juce::MathConstants<double>::twoPi * kWowRateHz  * flutterRateMul / sampleRate;
            flutPhase += juce::MathConstants<double>::twoPi * kFlutRateHz * flutterRateMul / sampleRate;
            if (wowPhase  >= juce::MathConstants<double>::twoPi) wowPhase  -= juce::MathConstants<double>::twoPi;
            if (flutPhase >= juce::MathConstants<double>::twoPi) flutPhase -= juce::MathConstants<double>::twoPi;
        }

        // 2. DRIVE — asymmetric tanh overdrive (a DC bias before the curve adds
        //    even harmonics → more "analog" than a symmetric shaper). Peak is
        //    normalised to ~unity and dry/wet blended by the knob. Three things keep
        //    it honest: the wet is DC-blocked (asymmetric clipping of a driven signal
        //    leaves an offset the bias subtraction alone cannot remove); the stage's
        //    output is loudness-compensated against the source's own level
        //    (driveCompTarget), so the knob adds grit, not volume, on a quiet loop
        //    and a hot one alike; and the compensation is one gain for both
        //    channels, so the image never shifts. 0 % = clean bypass.
        //    The level statistics are fed at every amount, 0 included (state only,
        //    the signal is untouched), so the knob finds the current source's level
        //    the moment it opens.
        //    Anti-aliasing, with no latency reported or looked ahead: the curve runs at
        //    twice the rate, and each 2x sample is first-order ADAA, the curve's mean
        //    between two inputs rather than a point read (tanh's antiderivative is log
        //    cosh). Either alone falls short on a driven 5 kHz sine: ADAA leaves its
        //    25 kHz harmonic (folding to 19.1 kHz) nearly whole, 2x alone stops just
        //    above −40 dB. The 2x filters are two-coefficient polyphase IIR halfbands
        //    (nothing ahead). How much folds back depends on where the harmonics land
        //    against the decimator's edge: 5 kHz at 44.1 kHz is a good case (~−46 dB in
        //    all), 4.5 kHz or 48 kHz reads ~−25 dB, but what lands there folds to
        //    ~21-23 kHz; folded energy below 15 kHz stays ≲ −44 dB for tones 2.5..9 kHz
        //    at 44.1 and 48 kHz (driveFoldReport). ADAA's mean is taken of the whole blend:
        //    the dry share is the identity's mean (the two samples' average) through the
        //    same lift and halfbands (driveLinear), so dry and wet stay aligned and
        //    never comb. That path delays by ~0.9 sample through the mids, ~1.0 at
        //    10 kHz, rising to ~1.2 at 15 and ~1.7 at 20 kHz (44.1 kHz; delayReport);
        //    the master MIX's dry tap is put through the same path (alignDry), or it
        //    would comb against it. The lift before the curve (kDriveLift*) undoes the
        //    mean's droop and the halfbands' edge (flat within 0.2 dB to 20 kHz). A
        //    band-limited clip rings, so sample peaks sit higher than the point-read
        //    curve's at the same loudness: +1.0 to +1.7 dB on the synthetic loop from
        //    25 to 100 % (chain report, peak column), up to ~+2.5 on other drums and
        //    ~+3 dB on bright noise with TONE +100 (review, 10-17). At
        //    amount 0 the halfbands would still delay, so the stage fades in and out on
        //    driveEng (as WARP/FLUTTER do), and the fade itself briefly blends the
        //    delayed and the raw signal; once idle the stage is skipped: an exact bypass.
        for (int ch = 0; ch < nch; ++ch)
            driveLevelPush(x[ch]);
        driveLevelW *= driveLevelGrow;
        if (driveLevelW > 1.0e12)
            driveLevelRescale(1.0 / driveLevelW);

        if (drive01 > 0.0f || driveEng > 0.0f)
        {
            // tanh per sample only while the (slewed) amount is actually moving;
            // settled values hit the cache.
            if (drive01 != driveCached)
            {
                driveCached = drive01;
                drvPre    = 1.0f + drive01 * kDrivePre;
                drvBias   = drive01 * kDriveBias;
                drvDcOff  = std::tanh(drvBias);
                drvNorm   = std::tanh(drvPre + drvBias) - drvDcOff;   // peak at full-scale in
                drvHistStale = true;
            }
            // Engaging from idle: the 2x path starts settled on the current input (as if
            // it had held it), so its filters' start-up never reaches the fade.
            if (driveOsIdle)
            {
                for (int ch = 0; ch < kMaxCh; ++ch)
                    driveWarmOs(driveOs[(size_t) ch], ch < nch ? x[ch] : 0.0f);
                driveOsIdle = false;
            }
            // ADAA differences two log-coshes of ONE curve, so a moved curve re-reads the
            // last 2x sample's (only while the amount moves; settled, it is carried over).
            if (drvHistStale)
            {
                for (auto& h : driveOs)
                {
                    h.u1  = (double) h.lin.s1 * (double) drvPre + (double) drvBias;
                    h.lc1 = logCosh(h.u1);
                }
                drvHistStale = false;
            }
            // Compensation target: recomputed every few frames while the amount moves
            // (the knob must not outrun it: a stepped amount landing on a kick would
            // let the hit through uncompensated), a few times a second once it
            // settles (the statistics drift slowly). Its own slew is short, just
            // enough to round the steps between recomputes. The first engaged frame
            // snaps, so a render or a fresh play starts at the right level.
            const bool fresh = driveCompFor < 0.0f;
            if (fresh || ++driveCompTick >= (drive01 != driveCompFor ? kDriveCompMoving : kDriveCompSettled))
            {
                driveCompTick = 0;
                driveCompFor  = drive01;
                driveCompTgt  = driveCompTarget();
                if (fresh) driveComp = driveCompTgt;
            }
            driveComp += driveCompA * (driveCompTgt - driveComp);

            const float dcOff  = drvDcOff;
            const float norm   = drvNorm;
            const float comp   = driveComp;
            // TONE: a symmetric first-order tilt of the grit (the wet only), pivoting at
            // kDriveToneHz: lows × 1/g, highs × g, g = ±kDriveToneDb at ±100 %, built as
            // wet/g + (g − 1/g)·HP(wet) with the high-pass's corner at pivot·g. Its own
            // gain (toneComp) holds the wet's K-weighted loudness where it was, from the
            // wet's level picture (toneCompFor), so TONE moves colour, not level, on a
            // dark loop and a bright one alike. 0 = the branch is skipped (true neutral);
            // the high-pass and the picture run at every tone so a move off 0 starts settled.
            if (driveTone != toneCached)
            {
                toneCached = driveTone;
                const double g = std::pow(10.0, kDriveToneDb * (double) driveTone / 20.0);
                const double w = tonePivotK * g;
                toneG    = (float) g;
                toneGInv = (float) (1.0 / g);
                toneB0   = (float) (1.0 / (1.0 + w));
                toneA1   = (float) ((w - 1.0) / (1.0 + w));
            }
            const float tilt = driveTone;
            const float tcomp = tilt != 0.0f ? toneCompFor(tilt) : 1.0f;
            double frameIn = 0.0;
            std::array<double, kToneGrid> frameHp {};
            const float eng = driveEng;
            for (int ch = 0; ch < nch; ++ch)
            {
                const float in = x[ch];
                auto& h = driveOs[(size_t) ch];
                float s2[2];                // the 2x pair, earlier first
                const float dry = driveLinear(h.lin, in, s2);
                float wet2[2];
                for (int k = 0; k < 2; ++k)
                    wet2[k] = (float) ((driveAdaa(h, (double) s2[k]) - (double) dcOff) / (double) norm);
                const float shaped = driveDown(wet2, h.dnWetA, h.dnWetB);
                float wet = shaped - driveDcX[(size_t) ch] + driveDcR * driveDcY[(size_t) ch];
                driveDcX[(size_t) ch] = shaped;
                driveDcY[(size_t) ch] = wet;
                tonePicture(wet, ch, frameIn, frameHp);
                const float hp = toneB0 * (wet - toneX1[(size_t) ch]) - toneA1 * toneY1[(size_t) ch];
                toneX1[(size_t) ch] = wet;
                toneY1[(size_t) ch] = hp;
                if (tilt != 0.0f)
                    wet = (wet * toneGInv + (toneG - toneGInv) * hp) * tcomp;
                const float out = (dry * (1.0f - drive01) + wet * drive01) * comp;
                x[ch] = eng >= 1.0f ? out : in + eng * (out - in);
            }
            if (frameIn > kToneSilent)          // silence says nothing about the spectrum
            {
                toneIn += toneLevelA * (frameIn - toneIn);
                for (int k = 0; k < kToneGrid; ++k)
                    toneHpLevel[(size_t) k] += toneLevelA * (frameHp[(size_t) k] - toneHpLevel[(size_t) k]);
            }
        }
        else
        {
            driveCompFor = -1.0f;   // bypassed: the next engage computes + snaps (≈ unity at ~0 %)
            driveOsIdle  = true;
        }

        // 3. ECHO — tape slapback: a feedback delay whose repeats lose high end
        //    each pass (one-pole damping in the feedback path) for a warm, analog
        //    tail. The knob raises both the feedback and the wet mix; the dry
        //    signal stays full and the echoes are added on top. The ring ALWAYS
        //    runs (write + filters + advance) so engaging mid-play repeats recent
        //    audio, not stale leftovers; at echo01==0 fb/mix are 0 and the output
        //    is exactly dry. The tap (echoDelayF) is fractional + slewed: a TIME
        //    move bends like tape instead of clicking the read head around.
        if (echoLen > 0)
        {
            if (echo01 != echoCached)
            {
                echoCached  = echo01;
                echoFbC     = echo01 * kEchoFbMax;
                echoMixC    = echo01 * kEchoMixMax;
                // Loudness comp: dry+echoes adds level, so trim the output back toward
                // the dry level as the knob opens (feedback path stays un-trimmed).
                echoMakeupC = std::pow(10.0f, echo01 * kEchoMakeupDb / 20.0f);
            }
            const float fb     = echoFbC;
            const float mix    = echoMixC;
            const float damp   = echoDampCoeff;   // fixed-Hz — computed in prepare()
            const float makeup = echoMakeupC;

            double readPos = (double) echoWrite - echoDelayF;
            while (readPos < 0.0) readPos += echoLen;
            const int   r0 = (int) readPos % echoLen;
            const int   r1 = (r0 + 1) % echoLen;
            const float fr = (float) (readPos - std::floor(readPos));

            for (int ch = 0; ch < nch; ++ch)
            {
                const float in = x[ch];
                const float a  = echoBuf[(size_t) ch][(size_t) r0];
                const float b  = echoBuf[(size_t) ch][(size_t) r1];
                const float delayed = a + fr * (b - a);
                echoDampState[(size_t) ch] += damp * (delayed - echoDampState[(size_t) ch]);
                // Band-pass on the repeats (and feedback): low-cut (HP) thins the
                // tail, high-cut (LP) darkens it. dry `in` is untouched, body clean.
                float wet = echoDampState[(size_t) ch];
                wet = echoHp[(size_t) ch].process(wet, echoHpCut);
                wet = echoLp[(size_t) ch].process(wet, echoLpCut);
                echoBuf[(size_t) ch][(size_t) echoWrite] = in + wet * fb;         // feed back
                x[ch] = (in + wet * mix) * makeup;                                // dry + echoes
            }
            echoWrite = (echoWrite + 1) % echoLen;
        }

        // 4. REVERB — an algorithmic space in one of three tanks (ReverbTanks.h):
        //    PLATE, HALL (default) or SPRING. Idle at 0 so a neutral chain stays a true
        //    pass-through. The tank runs WET-ONLY: the dry is re-mixed full-range and
        //    only the wet goes through the band, so lows (kick/bass) stay dry while the
        //    space sits inside the band. Each type's wet gain (kReverbTypeGain) puts its
        //    K-weighted loudness on the others' at the same AMOUNT and DECAY. DECAY (slewed,
        //    revDecay) sets each tank's T30 (reverbT30For). Like ECHO's ring, the tank is
        //    not frozen when the amount reaches 0: it keeps running on silence with its wet
        //    muted (the output is exactly dry) and rings out, and once it has stayed under
        //    kReverbSilent for kReverbQuietSec it is cleared and the stage idles. Raised
        //    again while it rings, the tail is the one it would have decayed to by then.
        //    TYPE: idle, the new tank simply takes over. While a tail rings, the new tank
        //    starts clean on the live input and the old one is crossfaded out over
        //    kReverbTypeFadeSec (equal power: the two tails are uncorrelated), both running
        //    only during the fade; then the old one is cleared. A change asked for during a
        //    fade waits for it to land.
        if (revType != revTypeTgt && revFade < 0.0f && reverb01 == 0.0f && ! reverbRinging)
            revType = revTypeTgt;                       // idle: every tank is clean
        if (reverb01 > 0.0f || reverbRinging)
        {
            const bool on = reverb01 > 0.0f;
            if (revType != revTypeTgt && revFade < 0.0f)
            {
                revFrom = revType;
                revType = revTypeTgt;
                revFade = 0.0f;
            }
            if (revDecay != revDecayCached)
            {
                revDecayCached = revDecay;
                plate.setT30(reverbT30For(kPlate, revDecay));
                hall.setT30(reverbT30For(kHall, revDecay));
                spring.setT30(reverbT30For(kSpring, revDecay));
                for (int t = 0; t < kNumReverbTypes; ++t)
                    revTypeGainC[(size_t) t] = reverbTypeGain(t, revDecay);
            }
            // The tank's input rises over kReverbTypeFadeSec (smoothstep) each time the amount
            // leaves 0, so a tank never hears the input start mid-wave (a step its tail rings with).
            // After a reset() the rise waits out revRampHold: the stage hears the WARP / FLUTTER
            // bases' silence first, and a rise spent on that left the input to step in (D-M1).
            revInRamp = on && ! rampHeld ? juce::jmin(1.0f, revInRamp + revFadeStep) : 0.0f;
            const float gRamp = revInRamp * revInRamp * (3.0f - 2.0f * revInRamp);
            // The tanks' input is DC-blocked (D-L1): HALL and PLATE carry DC around their
            // loops (up to ~11x the input's at DECAY 100), a soft thump when a TYPE change
            // or a ring-out drops it. A one-pole high-pass at kTankDcHz, ahead of every tank.
            // At amount 0 the input is silence, exactly, and the blocker rests (the ramp
            // brings the input back from 0 when the amount leaves it).
            float tankIn[2] = { 0.0f, 0.0f };
            if (on)
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float v = x[ch < nch ? ch : 0] * gRamp;
                    const float y = v - tankDcX[(size_t) ch] + tankDcR * tankDcY[(size_t) ch];
                    tankDcX[(size_t) ch] = v;
                    tankDcY[(size_t) ch] = y;
                    tankIn[ch] = y;
                }
            else
            {
                tankDcX.fill(0.0f);
                tankDcY.fill(0.0f);
            }
            const float inL = tankIn[0], inR = tankIn[1];
            float wet[kMaxCh] = { 0.0f, 0.0f };
            float tankPeak = 0.0f;
            if (revFade < 0.0f)
                tankPeak = runTank(revType, inL, inR, revTypeGainC[(size_t) revType], wet);
            else
            {
                // Equal-power crossfade: the outgoing tank's output at cos, the incoming's
                // at sin, the angle on a smoothstep so neither gain has a slope corner at
                // either end (a corner in a gain is a click of its own). The incoming tank's
                // INPUT rises on the same curve, so it never sees the input start mid-wave
                // (a step its tail would ring with). The fade lands exactly on 1 (old tank
                // at 0) before the old tank is dropped.
                revFade = juce::jmin(1.0f, revFade + revFadeStep);
                const bool  done = revFade >= 1.0f;
                const float th   = juce::MathConstants<float>::halfPi * revFade * revFade * (3.0f - 2.0f * revFade);
                const float gIn  = done ? 1.0f : std::sin(th), gOut = done ? 0.0f : std::cos(th);
                tankPeak = runTank(revType, inL * gIn, inR * gIn, revTypeGainC[(size_t) revType], wet);
                if (done)
                {
                    tank(revFrom).reset();             // faded out: cleared (no allocation)
                    revFade = -1.0f;
                }
                else
                {
                    float old[kMaxCh] = { 0.0f, 0.0f };
                    tankPeak = juce::jmax(tankPeak, runTank(revFrom, inL, inR, revTypeGainC[(size_t) revFrom], old));
                    for (int ch = 0; ch < kMaxCh; ++ch) wet[ch] = gIn * wet[ch] + gOut * old[ch];
                }
            }

            if (reverb01 != reverbCached)
            {
                reverbCached = reverb01;
                revWetC    = reverb01 * kReverbWetMax;
                revDryC    = 1.0f - reverb01 * 0.30f;             // gentle dry duck (as before)
                revMakeupC = std::pow(10.0f, reverb01 * kReverbMakeupDb / 20.0f);
            }
            const float wetGain = revWetC;
            const float dryGain = revDryC;
            const float makeup  = revMakeupC;
            for (int ch = 0; ch < nch; ++ch)
            {
                float w = wet[ch];
                w = reverbHp[(size_t) ch].process(w, reverbHpCut);
                w = reverbLp[(size_t) ch].process(w, reverbLpCut);
                if (on) x[ch] = (x[ch] * dryGain + w * wetGain) * makeup;
            }

            if (on)
            {
                reverbRinging = true;
                reverbQuiet   = 0;
            }
            else if (tankPeak >= kReverbSilent)
                reverbQuiet = 0;
            else if (++reverbQuiet >= reverbQuietLen)
            {
                reverbClearTanks();             // rung out: clear (no allocation) and idle
                for (auto& h : reverbHp) h.reset();
                for (auto& l : reverbLp) l.reset();
                reverbRinging = false;
                reverbQuiet   = 0;
            }
        }

        // 5. WIDTH — band-limited stereo width (M/S). Band-pass the SIDE: the in-band
        //    side is widened (×up to 1+kWidthBoost), the out-of-band side folds toward
        //    mono as WIDTH rises — so bass/whatever sits below the low handle stays
        //    centred while the band gets wider. Stereo-only; bypassed at width01==0
        //    (the ramp makes width=0 an exact pass-through, no discontinuity).
        // Runs when WIDTH is engaged OR CENTER (mid gain) is off unity — so the mid
        // trim works even with width at 0. At width01==0 the side passes untouched.
        // The side's band filters run every stereo frame, also with the stage at rest
        // (like a Cut in its dead zone), so raising WIDTH from 0 starts them from the
        // live input, not the state they held when it reached 0.
        if (nch >= 2)
        {
            const float side = 0.5f * (x[0] - x[1]);
            float inband = widthSideHp.process(side, widthHpCut);
            inband       = widthSideLp.process(inband, widthLpCut);
            if (width01 > 0.0f || std::abs(widthMid - 1.0f) > 1.0e-4f)
            {
                const float mid = 0.5f * (x[0] + x[1]) * widthMid;   // CENTER (mid) ±dB trim
                float sideOut = side;
                if (width01 > 0.0f)
                {
                    const float outband = side - inband;
                    sideOut = inband  * (1.0f + width01 * kWidthBoost)
                            + outband * (1.0f - width01);
                }
                x[0] = mid + sideOut;
                x[1] = mid - sideOut;
            }
        }
    }

    // REVERB TYPE, the choice param's index order.
    enum ReverbType : int { kPlate, kHall, kSpring, kNumReverbTypes };

private:
    // ── WARP / FLUTTER depth (the knob tapers, Andrew 10-17) ─────────────────────
    // Each knob's lower half scales its middle, the upper half opens to the 10-17 spike's
    // wide sweep (FLUTTER's middle is its old whole knob, WARP's see warpSweepFor). An old
    // set is converted on restore (warpFromOld / flutterFromOld, stateVersion 4).
    //   0..50 %:  the middle × 2u, linear.
    //   50..100 %: from there to the top, smooth and monotonic: with t = 2u − 1 and
    //   rho = middle / (top − middle), the rise is rho·t + (1 − rho)·t² (rho ≤ 2,
    //   the slope at 50 % carries on, C1) or 1 − (1 − t)^rho (rho > 2: as steep as the
    //   lower half at 50 %, flattening to the top). The two agree at rho = 2.
    static double taperRise(double t, double rho) noexcept
    {
        t = juce::jlimit(0.0, 1.0, t);
        return rho <= 2.0 ? rho * t + (1.0 - rho) * t * t : 1.0 - std::pow(1.0 - t, rho);
    }
    static double taper(double u, double mid, double top) noexcept
    {
        u = juce::jlimit(0.0, 1.0, u);
        if (u <= 0.5 || top - mid <= 1.0e-9)
            return mid * juce::jmin(1.0, 2.0 * u);
        return mid + (top - mid) * taperRise(2.0 * u - 1.0, mid / (top - mid));
    }
    // WARP's sweep (ms, before kWarpRangeMatch) at knob u (0..1) and rate f (Andrew 10-17).
    // The top (100 %) is kWarpSweepMaxMs at every rate: the deepest sweep the 16 ms base
    // allows (with kWarpRangeMatch and the knee). The middle (50 %) is v2's old 100 %, T_old =
    // kWarpPitchDepth / (2π f) (±60 cents) capped at kWarpSweepCapMs, but never past
    // kWarpMidFrac of the top: a soft minimum of the two (p = kWarpSoftMinP, smooth in rate),
    // so the top half adds depth at every rate (≥ 1/0.6 × at slow rates) and at fast rates
    // the middle is the old top (within ~1 % at RATE 100, ~4 % at 50). Monotonic in the
    // knob, and in rate (the sweep never grows as the rate rises).
    static double warpOldTop(double f) noexcept
    {
        return std::min(kWarpSweepCapMs, 1000.0 * kWarpPitchDepth / (juce::MathConstants<double>::twoPi * f));
    }
    static double warpMid(double f) noexcept
    {
        const double a = warpOldTop(f), b = kWarpMidFrac * kWarpSweepMaxMs;
        return std::pow(std::pow(a, -kWarpSoftMinP) + std::pow(b, -kWarpSoftMinP), -1.0 / kWarpSoftMinP);
    }
    static double warpSweepFor(double u, double f) noexcept { return taper(u, warpMid(f), kWarpSweepMaxMs); }

public:
    static double warpRateHzFor(float warpRatePct) noexcept
    {
        return kWarpRateMinHz + (kWarpRateMaxHz - kWarpRateMinHz) * juce::jlimit(0.0, 1.0, (double) warpRatePct * 0.01);
    }
    // stateVersion 4 migration (PluginProcessor setStateInformation, FactoryPresets): the knob
    // value that gives an old (pre-4) WARP setting's sweep, old warp × T_old at its RATE.
    static float warpFromOld(float oldWarpPct, float warpRatePct) noexcept
    {
        const double f = warpRateHzFor(warpRatePct), m = warpMid(f), top = kWarpSweepMaxMs;
        const double want = juce::jlimit(0.0, 1.0, (double) oldWarpPct * 0.01) * warpOldTop(f);
        if (want <= m) return (float) (100.0 * want / (2.0 * m));
        const double rho = m / (top - m), y = juce::jlimit(0.0, 1.0, (want - m) / (top - m));
        double t;   // invert taperRise
        if (rho > 2.0)                       t = 1.0 - std::pow(1.0 - y, 1.0 / rho);
        else if (std::abs(1.0 - rho) < 1e-9) t = y / rho;
        else                                 t = (-rho + std::sqrt(rho * rho + 4.0 * (1.0 - rho) * y)) / (2.0 * (1.0 - rho));
        return (float) (50.0 + 50.0 * juce::jlimit(0.0, 1.0, t));
    }
    // WARP's sweep in ms (before kWarpRangeMatch) for a knob and RATE, for tests and the audit.
    static double warpSweepMs(float warpPct, float warpRatePct) noexcept
    { return warpSweepFor(juce::jlimit(0.0, 1.0, (double) warpPct * 0.01), warpRateHzFor(warpRatePct)); }
    // FLUTTER's rate does not enter its depth: the old knob is the new one halved.
    static float flutterFromOld(float oldFlutterPct) noexcept { return 0.5f * oldFlutterPct; }
    // A REVERB TYPE's wet gain (dB) at a decay, off kReverbTypeGainDb, for the measurement tool.
    static float reverbTypeGainDb(int type, float decay01) noexcept
    { return juce::Decibels::gainToDecibels(reverbTypeGain(juce::jlimit(0, kNumReverbTypes - 1, type), decay01)); }

private:
    // FLUTTER's wow depth (ms, before kFlutterRangeMatch; flutter is kFlutToWow of it).
    static double flutterDepthFor(double u) noexcept { return taper(u, kWowDepthOldMs, kWowDepthMs); }
    // The sway as read: itself up to kWarpKneeMs, then a tanh knee that never reaches
    // kWarpSwayMaxMs, so the read stays inside the base delay (C1 at the knee).
    // A glide out of the sway (knob to 0): `glide` < 0 = none. Started when `out` turns true,
    // it runs 0 -> 1 by `step` a frame; the envelope it returns, a raised cosine 1 -> 0, scales
    // the sway as read (exactly 0 at the end). Cancelled (knob raised again), the envelope is
    // folded into the sway, so the read carries on from where it was.
    static double glideStep(float& glide, double& sway, bool out, float step) noexcept
    {
        if (! out)
        {
            if (glide >= 0.0f)
                sway *= 0.5 * (1.0 + std::cos(juce::MathConstants<double>::pi * (double) glide));
            glide = -1.0f;
            return 1.0;
        }
        glide = glide < 0.0f ? 0.0f : juce::jmin(1.0f, glide + step);
        return glide >= 1.0f ? 0.0 : 0.5 * (1.0 + std::cos(juce::MathConstants<double>::pi * (double) glide));
    }
    // FLUTTER's sway as read: itself up to kFlutKneeMs, then a tanh knee that never reaches
    // kBaseDelayMs - 1 (as warpKnee).
    static double flutKnee(double v) noexcept
    {
        const double a = std::abs(v), top = kBaseDelayMs - 1.0;
        if (a <= kFlutKneeMs) return v;
        const double room = top - kFlutKneeMs;
        return std::copysign(kFlutKneeMs + room * std::tanh((a - kFlutKneeMs) / room), v);
    }
    static double warpKnee(double v) noexcept
    {
        const double a = std::abs(v);
        if (a <= kWarpKneeMs) return v;
        const double room = kWarpSwayMaxMs - kWarpKneeMs;
        return std::copysign(kWarpKneeMs + room * std::tanh((a - kWarpKneeMs) / room), v);
    }
    // A 4-point cubic Hermite read `d` frames behind the frame just written at `w` (d ≥ 1).
    static float hermiteAt(const std::vector<float>& line, int size, int w, double d) noexcept
    {
        const int   i = (int) d;
        const float f = (float) (d - i);
        auto at = [&] (int k) { int j = w - k; if (j < 0) j += size; return line[(size_t) j]; };
        const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

    // ── REVERB tanks ─────────────────────────────────────────────────────────────
    // The T30 DECAY asks of a type: PLATE and HALL share v2's curve (0.78 → 5.5 s, ×1.63
    // per 25 %, as Freeverb had it, so a set's DECAY keeps its tail), SPRING its own
    // shorter one (0.6 → 3.0 s, ×1.50 per 25 %), a spring tank being a short thing.
    static double reverbT30For(int type, float decay01) noexcept
    {
        const double d = juce::jlimit(0.0, 1.0, (double) decay01);
        return type == kSpring ? kSpringT30Min * std::pow(kSpringT30Max / kSpringT30Min, d)
                               : kReverbT30Min * std::pow(kReverbT30Max / kReverbT30Min, d);
    }
    // A type's wet gain at a decay: its table (dB at DECAY 0/25/50/75/100), interpolated.
    static float reverbTypeGain(int type, float decay01) noexcept
    {
        const auto& g = kReverbTypeGainDb[(size_t) type];
        const float u = juce::jlimit(0.0f, 1.0f, decay01) * (float) (g.size() - 1);
        const int   i = juce::jmin((int) g.size() - 2, (int) u);
        return juce::Decibels::decibelsToGain(g[(size_t) i] + (u - (float) i) * (g[(size_t) i + 1] - g[(size_t) i]));
    }
    struct TankRef
    {
        CharacterChain& c; int type;
        void reset() noexcept
        {
            if (type == kPlate) c.plate.reset(); else if (type == kSpring) c.spring.reset(); else c.hall.reset();
        }
    };
    TankRef tank(int type) noexcept { return { *this, type }; }
    // One frame of a tank: `out` gets its wet × gain, the return is its raw peak (ring-out).
    inline float runTank(int type, float inL, float inR, float gain, float (&out)[kMaxCh]) noexcept
    {
        float l = 0.0f, r = 0.0f;
        if (type == kPlate)       plate.process(inL, inR, l, r);
        else if (type == kSpring) spring.process(inL, inR, l, r);
        else                      hall.process(inL, inR, l, r);
        out[0] = l * gain;
        out[1] = r * gain;
        return juce::jmax(std::abs(l), std::abs(r));
    }
    void reverbClearTanks() noexcept
    {
        tank(revType).reset();
        if (revFade >= 0.0f) tank(revFrom).reset();
        revFade = -1.0f;
        revType = revTypeTgt;
        revInRamp = 0.0f;       // the tanks are clean: their input ramps in again (D-M1)
        tankDcX.fill(0.0f);
        tankDcY.fill(0.0f);
    }

    float onePoleCoeff(float fc) const
    {
        const float c = 1.0f - std::exp(-juce::MathConstants<float>::twoPi
                                        * juce::jlimit(1.0f, (float) sampleRate * 0.49f, fc)
                                        / (float) sampleRate);
        return juce::jlimit(0.0f, 1.0f, c);
    }

    // ── DRIVE TONE level hold ────────────────────────────────────────────────
    // The tilt's power gain on any spectrum is 1/g² + (g² − 1/g²)·P_hp / P, where P is
    // the wet's power and P_hp its power through the tilt's own high-pass (corner
    // pivot·g), both K-weighted (BS.1770, as LoopNestMeasure and GAIN MATCH weigh
    // loudness). The picture keeps P and P_hp for corners on a grid spanning the
    // tilt's range (g = 10^(±kDriveToneDb/20)); a tone in between interpolates. So
    // the gain follows a TONE move at once; only the picture of the source is slow.
    void kWeighting(double sr)
    {
        // RBJ biquads, the BS.1770 targets (as MonitorStage.h): high shelf
        // f0≈1681.97 Hz +4 dB Q≈0.7072, then high-pass f0≈38.14 Hz Q≈0.5003.
        const double twoPi = juce::MathConstants<double>::twoPi;
        {
            const double f0 = 1681.9744509555319, gDb = 3.9998438539733446, q = 0.7071752369554196;
            const double A = std::pow(10.0, gDb / 40.0), w0 = twoPi * f0 / sr;
            const double cw = std::cos(w0), sa = 2.0 * std::sqrt(A) * std::sin(w0) / (2.0 * q);
            const double a0 = (A + 1.0) - (A - 1.0) * cw + sa;
            kwShelf = { (float) (A * ((A + 1.0) + (A - 1.0) * cw + sa) / a0), (float) (-2.0 * A * ((A - 1.0) + (A + 1.0) * cw) / a0),
                        (float) (A * ((A + 1.0) + (A - 1.0) * cw - sa) / a0), (float) (2.0 * ((A - 1.0) - (A + 1.0) * cw) / a0),
                        (float) (((A + 1.0) - (A - 1.0) * cw - sa) / a0) };
        }
        {
            const double f0 = 38.13547087602444, q = 0.5003270373238773, w0 = twoPi * f0 / sr;
            const double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * q), a0 = 1.0 + alpha;
            kwHipass = { (float) (0.5 * (1.0 + cw) / a0), (float) (-(1.0 + cw) / a0), (float) (0.5 * (1.0 + cw) / a0),
                         (float) (-2.0 * cw / a0), (float) ((1.0 - alpha) / a0) };
        }
    }

    // One channel's wet into this frame's picture sums.
    inline void tonePicture(float wet, int ch, double& frameIn, std::array<double, kToneGrid>& frameHp) noexcept
    {
        auto bq = [] (const std::array<float, 5>& c, std::array<float, 2>& z, float v) noexcept
        {
            const float y = c[0] * v + z[0];
            z[0] = c[1] * v - c[3] * y + z[1];
            z[1] = c[2] * v - c[4] * y;
            return y;
        };
        const float k = bq(kwHipass, kwState[(size_t) ch][1], bq(kwShelf, kwState[(size_t) ch][0], wet));
        frameIn += (double) k * k;
        for (int i = 0; i < kToneGrid; ++i)
        {
            auto& st = toneGridState[(size_t) ch][(size_t) i];
            const float h = toneGridB0[(size_t) i] * (k - st[0]) - toneGridA1[(size_t) i] * st[1];
            st[0] = k;
            st[1] = h;
            frameHp[(size_t) i] += (double) h * h;
        }
    }

    // The gain that holds the tilted wet's K-weighted power at the untilted wet's.
    float toneCompFor(float tone) const noexcept
    {
        if (toneIn <= kToneSilent)
            return 1.0f;                                 // no picture yet
        const double u = (juce::jlimit(-1.0, 1.0, (double) tone) + 1.0) * 0.5 * (kToneGrid - 1);
        const int    i = juce::jmin(kToneGrid - 2, (int) u);
        const double f = u - i;
        const double hp = toneHpLevel[(size_t) i] + f * (toneHpLevel[(size_t) i + 1] - toneHpLevel[(size_t) i]);
        const double rho = juce::jlimit(0.0, 1.0, hp / toneIn);
        const double g2 = (double) toneG * toneG, gi2 = (double) toneGInv * toneGInv;
        return (float) (1.0 / std::sqrt(gi2 + (g2 - gi2) * rho));
    }

    // The tilt's and the picture's filter states (not the picture: like DRIVE's level
    // picture, what was learned about the source outlives a reset).
    void toneResetFilters() noexcept
    {
        toneX1.fill(0.0f);
        toneY1.fill(0.0f);
        for (auto& ch : kwState) for (auto& z : ch) z.fill(0.0f);
        for (auto& ch : toneGridState) for (auto& z : ch) z.fill(0.0f);
    }

    // ── DRIVE anti-aliasing ──────────────────────────────────────────────────
    // DRIVE's linear path, one channel (process() 2. DRIVE): the lift (a biquad,
    // transposed direct form), the upsampler's and the decimator's halfbands (each two
    // first-order allpass paths at the base rate, [x1, y1] apiece) and the last 2x
    // sample. It carries the stage's dry share, and on its own it is the MIX dry tap's
    // shadow (alignDry).
    struct DriveLin
    {
        float lift[2] {};
        float upA[2] {}, upB[2] {}, dnA[2] {}, dnB[2] {};
        float s1 = 0.0f;
    };
    // …and the stage's whole 2x path: that, the wet share's decimator and the ADAA
    // history (the curve's argument at the last 2x sample and its log cosh).
    struct DriveOs
    {
        DriveLin lin;
        float dnWetA[2] {}, dnWetB[2] {};
        double u1 = 0.0, lc1 = 0.0;
    };

    static inline float allpass1(float v, float a, float* z) noexcept
    {
        const float y = a * (v - z[1]) + z[0];
        z[0] = v;
        z[1] = y;
        return y;
    }
    // Two-path polyphase halfband down: a 2x pair becomes one output (path A takes the
    // later sample). Up is the mirror (driveLinear): one input feeds both paths, path A
    // first.
    static inline float driveDown(const float* s2, float* zA, float* zB) noexcept
    {
        return 0.5f * (allpass1(s2[1], kDriveDnC0, zA) + allpass1(s2[0], kDriveDnC1, zB));
    }
    // One input through lift → 2x → the identity's ADAA (each 2x sample's mean with the
    // one before) → back down: the dry share. The 2x pair is left in s2 for the curve.
    // The input is held to ±kDriveInMax first: far above any real signal, it keeps a
    // huge finite sample from overflowing the filters into a NaN that would latch.
    inline float driveLinear(DriveLin& h, float v, float* s2) const noexcept
    {
        v = juce::jlimit(-kDriveInMax, kDriveInMax, v);
        const float lifted = drvLiftB[0] * v + h.lift[0];
        h.lift[0] = drvLiftB[1] * v - drvLiftA[0] * lifted + h.lift[1];
        h.lift[1] = drvLiftB[2] * v - drvLiftA[1] * lifted;
        s2[0] = allpass1(lifted, kDriveUpC0, h.upA);
        s2[1] = allpass1(lifted, kDriveUpC1, h.upB);
        const float m2[2] = { 0.5f * (s2[0] + h.s1), 0.5f * (s2[1] + s2[0]) };
        h.s1 = s2[1];
        return driveDown(m2, h.dnA, h.dnB);
    }

    // log cosh in the form that cannot overflow: |u| + log(1 + e^(−2|u|)) − ln 2.
    static inline double logCosh(double u) noexcept
    {
        const double a = std::abs(u);
        return a + std::log1p(std::exp(-2.0 * a)) - 0.69314718055994531;
    }
    // First-order ADAA of tanh(pre·s + bias) from the last 2x sample to s: the
    // difference of log coshes over the difference of arguments (in the argument, so
    // pre cancels). Two arguments too close to tell apart are ill-conditioned there;
    // the curve at their midpoint is then the same mean to well under float precision.
    inline double driveAdaa(DriveOs& h, double s) const noexcept
    {
        const double u  = s * (double) drvPre + (double) drvBias;
        const double lc = logCosh(u);
        const double du = u - h.u1;
        const double t  = std::abs(du) > kDriveAdaaEps ? (lc - h.lc1) / du : std::tanh(0.5 * (u + h.u1));
        h.u1  = u;
        h.lc1 = lc;
        return t;
    }

    // The 2x paths back to rest (reset(), an OFF panel landing); the next frame that
    // runs them starts settled, as a fresh chain's first frame does.
    void driveClearOs() noexcept
    {
        driveOs.fill({});
        driveDryLin.fill({});
        drvHistStale = true;        // u1 / lc1 are re-read for the live curve
        driveOsIdle  = true;
        driveDryIdle = true;
    }
    // A linear path settled on a held input v: every state at its DC value.
    void driveWarmLin(DriveLin& h, float v) const noexcept
    {
        v = juce::jlimit(-kDriveInMax, kDriveInMax, v);
        h.lift[1] = (drvLiftB[2] - drvLiftA[1]) * v;
        h.lift[0] = (drvLiftB[1] - drvLiftA[0]) * v + h.lift[1];
        for (float* z : { h.upA, h.upB, h.dnA, h.dnB })
            z[0] = z[1] = v;
        h.s1 = v;
    }
    // …and the stage's: the wet decimator at the curve's value there, for the live
    // curve (drvPre/drvBias cached).
    void driveWarmOs(DriveOs& h, float v) noexcept
    {
        driveWarmLin(h.lin, v);
        v = juce::jlimit(-kDriveInMax, kDriveInMax, v);
        const float w = (std::tanh(v * drvPre + drvBias) - drvDcOff) / drvNorm;
        for (float* z : { h.dnWetA, h.dnWetB })
            z[0] = z[1] = w;
        drvHistStale = true;
    }

    // ── DRIVE loudness compensation ──────────────────────────────────────────
    // A saturator's gain depends on the level it is fed: quiet audio sits on the
    // steep part of the curve and comes out much louder, hot audio is already near
    // the ceiling. So a fixed makeup is right for one source level only. Instead
    // the stage keeps a running picture of its input's amplitudes (half-octave
    // bins, positive and negative apart because the curve is asymmetric), and the
    // compensation is worked out from that picture and the knob: push each bin's
    // amplitude through the same blend the audio takes, compare the power coming
    // out with the power going in. The knob enters that sum directly, so the gain
    // follows a knob move at once; only the picture of the source is slow.
    struct LevelBin { double n = 0.0, e = 0.0; };   // weighted count, weighted sum of x²

    inline void driveLevelPush(float v) noexcept
    {
        const float a = std::abs(v);
        if (a < kDriveLevelFloor) return;            // silence says nothing about the source
        if (drivePriorHeld)                          // real audio: the prior steps aside (F10)
        {
            for (auto& side : driveLevel) side.fill({});
            driveLevelW    = 1.0;
            drivePriorHeld = false;
            driveCompFor   = -1.0f;   // and the gain snaps from the source's picture on this frame:
        }                             // a fresh chain's first frames are its latency() of silence
        int e = 0;
        const float m = std::frexp(a, &e);           // a = m·2^e, m in [0.5, 1)
        const int bin = juce::jlimit(0, kLevelBins - 1,
                                     2 * e - 2 + (m >= 0.70710678f ? 1 : 0) + kLevelBinOffset);
        auto& b = driveLevel[v < 0.0f ? 1 : 0][(size_t) bin];
        b.n += driveLevelW;
        b.e += driveLevelW * (double) a * (double) a;
    }

    void driveLevelRescale(double k) noexcept
    {
        for (auto& side : driveLevel)
            for (auto& b : side) { b.n *= k; b.e *= k; }
        driveLevelW = 1.0;
    }

    // reset(): what was learned about the source stays, as a prior (the same loop
    // usually plays again), and nothing learned yet means a moderate level. The prior
    // only answers until real audio arrives: the first sample above the floor clears
    // it, so the picture is the source's alone from then on (F10, Andrew 10-08). A
    // prior kept alongside the audio carried more energy than a quiet loop does, so a
    // fresh chain (every print, a new instance) ran up to 5 dB hot for ~30 s.
    void driveLevelToPrior() noexcept
    {
        drivePriorHeld = true;
        double n = 0.0;
        for (const auto& side : driveLevel)
            for (const auto& b : side) n += b.n;
        const double mass = kDrivePriorSec * sampleRate * 2.0;   // two channels push per frame
        if (n > 0.0)
        {
            driveLevelRescale(mass / n);
            return;
        }
        const int bin = juce::jlimit(0, kLevelBins - 1, kLevelBinOffset - 4);   // 2^-2 = −12 dBFS
        for (auto& side : driveLevel)
        {
            side.fill({});
            side[(size_t) bin] = { mass * 0.5, mass * 0.5 * 0.25 * 0.25 };
        }
        driveLevelW = 1.0;
    }

    // The gain that returns the stage's output to its input's power, for the live
    // amount (drvPre/drvBias/drvDcOff/drvNorm are already cached for it). The wet's
    // mean is taken off first, as the DC blocker does. Equal power lands within
    // about a dB of equal K-weighted loudness across a 17 dB range of source levels
    // (LoopNestMeasure), so there is no further trim.
    float driveCompTarget() const noexcept
    {
        const double d = (double) drive01;
        double amp[2][kLevelBins], wet[2][kLevelBins];
        double n = 0.0, ein = 0.0, wsum = 0.0;
        for (int s = 0; s < 2; ++s)
            for (int i = 0; i < kLevelBins; ++i)
            {
                const auto& b = driveLevel[(size_t) s][(size_t) i];
                if (b.n <= 0.0) { amp[s][i] = wet[s][i] = 0.0; continue; }
                const double a = std::sqrt(b.e / b.n) * (s == 0 ? 1.0 : -1.0);
                const double w = (std::tanh(a * (double) drvPre + (double) drvBias) - (double) drvDcOff)
                                 / (double) drvNorm;
                amp[s][i] = a; wet[s][i] = w;
                n += b.n; ein += b.e; wsum += b.n * w;
            }
        if (n <= 0.0 || ein <= 0.0)
            return driveCompTgt;
        const double mean = wsum / n;
        double eout = 0.0;
        for (int s = 0; s < 2; ++s)
            for (int i = 0; i < kLevelBins; ++i)
            {
                const double f = (1.0 - d) * amp[s][i] + d * (wet[s][i] - mean);
                eout += driveLevel[(size_t) s][(size_t) i].n * f * f;
            }
        if (eout <= 0.0)
            return driveCompTgt;
        return (float) juce::jlimit(0.02, 1.5, std::sqrt(ein / eout));
    }

    // Map a band handle position (0..1 on a log frequency axis) to its frequency.
    float bandFreq(float p01) const
    { return kBandFLo * std::pow(kBandFHi / kBandFLo, juce::jlimit(0.0f, 1.0f, p01)); }

    // The pole term of one section of a cut whose PAIR is -3 dB at `hz`. Bilinear
    // design, prewarped: the digital response at f is the analog one at
    // tan(pi f / sr), so placing the analog -3 dB point at tan(pi hz / sr) lands the
    // digital one exactly on hz at every sample rate. Two equal one-poles are -3 dB
    // together where each is -1.5 dB: for a high cut (low-pass) at sqrt(sqrt(2) - 1)
    // of a section's own corner, for a low cut (high-pass) at 1 / that.
    float cutA1(float hz, bool highPass) const
    {
        const double w = std::tan(juce::MathConstants<double>::pi
                                  * juce::jlimit(1.0, sampleRate * 0.49, (double) hz) / sampleRate);
        const double k = highPass ? w * kCutPairCorner : w / kCutPairCorner;   // a section's corner
        return (float) ((k - 1.0) / (k + 1.0));
    }

    // A band's two handle positions → its low cut's and high cut's targets. lo ≤
    // kBandEdge / hi ≥ 1 − kBandEdge = that cut open (eng 0, an exact pass-through);
    // an open cut keeps running, so engaging it crossfades into a filter already
    // settled on the input (see Cut). Over the kBandRamp next to the dead zone the
    // cut's pole slides from the ramp's inner corner to kCutOpenA1, where a section
    // is a pass-through to within a hundredth of a dB in band (its pole all but
    // cancels its zero), so dragging a handle to its end never steps from -3 dB to
    // open. From the ramp inward the handle is the -3 dB corner.
    void bandTargets(float lo01, float hi01, Cut& hp, Cut& lp) const
    {
        auto slide = [this] (float into, float inner, bool highPass)
        {
            const float a = cutA1(bandFreq(inner), highPass), open = highPass ? -kCutOpenA1 : kCutOpenA1;
            const float t = juce::jlimit(0.0f, 1.0f, into / kBandRamp);
            return a + t * (open - a);
        };
        hp.engTgt = lo01 > kBandEdge ? 1.0f : 0.0f;
        hp.a1Tgt  = lo01 >= kBandEdge + kBandRamp ? cutA1(bandFreq(lo01), true)
                                                  : slide(kBandEdge + kBandRamp - lo01, kBandEdge + kBandRamp, true);
        lp.engTgt = hi01 < 1.0f - kBandEdge ? 1.0f : 0.0f;
        lp.a1Tgt  = hi01 <= 1.0f - kBandEdge - kBandRamp ? cutA1(bandFreq(hi01), false)
                                                         : slide(hi01 - (1.0f - kBandEdge - kBandRamp), 1.0f - kBandEdge - kBandRamp, false);
    }

    // Tunable character constants (taste, not fidelity — adjust by ear).
    static constexpr double kWarpRateMinHz = 0.15;  // slowest sway (deep drift)
    static constexpr double kWarpRateMaxHz = 1.6;   // fastest sway (lurchy)
    static constexpr double kWarpPitchDepth = 0.035; // peak speed change at 50 % (v2's old 100 %): +59.6 / -61.7 cents, RATE ~22 % up
    static constexpr double kWarpLeakSec   = 1.0;   // a knob or RATE move settles onto the new sweep over this
    // The sweep is capped (Andrew 10-08) so WARP's added delay stays the 16 ms it
    // always was: an uncapped slowest rate needs 0.035 / (2π · 0.15 Hz) = 37 ms, which
    // put FX 40 ms late (unreported to the host) and a slap under the dry at MIX < 100.
    // The cap is also the spike's full sweep (12 ms × kWarpRangeMatch at the top). The
    // read stays inside the base: the knee (warpKnee) bends it short of kWarpSwayMaxMs.
    static constexpr double kWarpSweepCapMs = 12.0;
    static constexpr double kWarpSweepMaxMs = 12.0;  // the top: 12 ms × kWarpRangeMatch = 15 ms, the knee's room
    static constexpr double kWarpMidFrac    = 0.6;   // the middle never past this much of the top …
    static constexpr double kWarpSoftMinP   = 8.0;   // … through a soft minimum this sharp
    static constexpr double kWarpBaseMs    = 16.0;
    static constexpr double kWarpSwayMaxMs = kWarpBaseMs - 1.0;
    static constexpr double kWarpKneeMs    = 12.0;
    // The organic drift (10-17 spike, version C): 35 % of the modulation is a random
    // spline, and the whole is scaled ×1.25, which gives back the pure sine's peak swing.
    static constexpr double kWarpRandMix    = 0.35;
    static constexpr double kWarpRangeMatch = 1.25;
    static constexpr uint32_t kWarpSeed = 4242u, kWowSeed = 9001u, kFlutSeed = 9002u;   // the spike's

    static constexpr double kWowRateHz   = 0.7;
    static constexpr double kFlutRateHz  = 6.0;
    static constexpr double kWowDepthOldMs = 1.05; // wow depth at 50 % (v2's old 100 %): with flutter ±35 cents, rate 50 %
    static constexpr double kWowDepthMs  = 3.0;   // …and at 100 %, the spike's: ≈ +95 / −102 cents at rate 50 % (×2 at rate 100 %)
    static constexpr double kFlutToWow   = 0.4;   // flutter depth = 0.4 × wow's (ratio 2.5, as ever)
    static constexpr double kFlutterRandMix    = 0.35;
    static constexpr double kFlutterRangeMatch = 1.25;
    static constexpr double kBaseDelayMs = 8.0;   // > max wobble (≈ 5.6 ms at 100 %), so offset stays positive
    static constexpr double kFlutterLeakSec = 0.5; // a depth move settles onto the new depth over this (D-M3)
    static constexpr double kFlutKneeMs  = 6.0;   // FLUTTER's knee (its steady sway peaks ~5.6 ms at 100 %)

    static constexpr float kDrivePre      = 10.0f;  // tanh pre-gain at full drive
    static constexpr float kDriveBias     = 0.30f;  // asymmetry → even harmonics
    static constexpr double kDriveDcHz     = 10.0;  // wet DC blocker corner
    static constexpr double kDriveLevelSec = 6.0;   // how long the source's level picture remembers
    static constexpr double kDrivePriorSec = 0.05;  // what that picture is worth after a reset
    static constexpr float kDriveLevelFloor = 1.0e-3f; // below −60 dBFS = silence, not level
    static constexpr int   kLevelBins      = 24;    // half-octave bins, −60 … +12 dBFS
    static constexpr int   kLevelBinOffset = 20;    // bin 0 starts at 2^-10
    static constexpr int   kDriveCompMoving  = 8;   // frames between recomputes while the knob moves
    static constexpr int   kDriveCompSettled = 512; // …and once it has settled
    static constexpr float kDriveToneHz   = 1800.0f;// TONE tilt pivot (dark/bright), 0 dB there
    static constexpr double kDriveToneDb  = 4.5;    // TONE shelves at ±100 %: lows ∓, highs ± this
    static constexpr double kToneLevelSec = 2.0;    // how long TONE's spectrum picture remembers
    static constexpr double kToneSilent   = 1.0e-12;// a frame's K-weighted power under −120 dB = silence
    // DRIVE anti-aliasing: the 2x halfbands' allpass coefficients (two-path polyphase
    // IIR, elliptic, transition 0.12 up and 0.06 down as a fraction of the 2x rate), the
    // gap in the curve's argument below which ADAA reads the midpoint, the bound on the
    // stage's input, and the lift: an RBJ high shelf at a fixed fraction of the rate
    // (so the same filter at every rate), fitted so the whole linear path stays within
    // ±0.2 dB from 1 to 20 kHz at 44.1 and 48 kHz (it undoes the mean's droop and the
    // halfbands' edge).
    static constexpr float  kDriveUpC0    = 0.21354163889888372f;
    static constexpr float  kDriveUpC1    = 0.68855296017957490f;
    static constexpr float  kDriveDnC0    = 0.30627952515379336f;
    static constexpr float  kDriveDnC1    = 0.78214423687352042f;
    static constexpr double kDriveAdaaEps = 1.0e-5;
    static constexpr float  kDriveInMax   = 1.0e4f;
    static constexpr double kDriveLiftAt  = 0.37;   // shelf corner, × the sample rate
    static constexpr double kDriveLiftDb  = 3.0;    // …its gain at Nyquist
    static constexpr double kDriveLiftS   = 0.4;    // …and slope

    // Wet band-pass frequency axis: both handles map 0..1 → kBandFLo..kBandFHi on a
    // log scale. kBandEdge is the dead band at each end where the filter bypasses.
    static constexpr float kBandFLo = 20.0f;
    static constexpr float kBandFHi = 20000.0f;
    static constexpr float kBandEdge = 0.02f;
    static constexpr float kBandRamp = 0.02f;   // the slide-out zone inside each dead zone's edge
    static constexpr float kCutOpenA1 = 0.9999f;// a section's pole at the dead zone: all but a pass-through
    static constexpr double kCutPairCorner = 0.64359425290558262;  // sqrt(sqrt(2) - 1), see cutA1
    static constexpr double kCutFadeSec    = 0.005;  // a cut's dead-zone crossfade

    static constexpr double kEchoTimeMs    = 170.0; // default slapback time (recall-safe)
    static constexpr double kEchoTimeMinMs = 30.0;  // ECHO TIME knob min (tight slap)
    static constexpr double kEchoTimeMaxMs = 350.0; // ECHO TIME knob max (short dub)
    static constexpr float  kEchoFbMax    = 0.55f;  // feedback at echo=full (repeat count)
    static constexpr float  kEchoMixMax   = 0.50f;  // wet add at echo=full
    static constexpr float  kEchoDampHz   = 3200.0f;// HF rolloff per repeat (tape feel)
    static constexpr float  kEchoMakeupDb = -2.5f;  // output trim at echo=full (loudness comp)

    static constexpr float kReverbWetMax  = 0.45f;  // wet level at reverb=full
    static constexpr float kReverbMakeupDb = -3.5f; // output trim at reverb=full (loudness comp)
    static constexpr double kReverbT30Min = 0.78, kReverbT30Max = 5.5;   // PLATE / HALL T30 at DECAY 0 / 100
    static constexpr double kSpringT30Min = 0.6,  kSpringT30Max = 3.0;   // SPRING's
    // Each tank's wet gain (PLATE, HALL, SPRING), dB at DECAY 0/25/50/75/100 %. HALL's puts it
    // as loud as v2's Freeverb was at that AMOUNT / DECAY on LoopNestMeasure's loop (solved
    // 10-17, K-weighted, third pass). PLATE's and SPRING's put their wet on HALL's on 12 real
    // loops of Andrew's bank, the median of each one's wet against HALL's (D-M2, re-solved
    // 10-18 with `LoopNestMeasure typegains`: the whole-mix type-to-type spread went from a
    // median 1.13 / max 4.30 dB to 0.56 / 4.11; the max is one loop's spectrum, not the table).
    // Andrew's ear decides. The 10-17 table, every type on the synthetic loop, to restore:
    //   PLATE  { 4.16f, 5.94f, 7.30f, 8.32f, 9.10f }
    //   HALL   { 2.09f, 3.28f, 4.29f, 5.00f, 5.49f }
    //   SPRING { 6.66f, 7.19f, 7.88f, 8.59f, 9.19f }
    static constexpr std::array<std::array<float, 5>, 3> kReverbTypeGainDb { {
        { { 3.42f, 5.20f, 6.48f, 7.20f, 7.58f } },
        { { 2.09f, 3.28f, 4.29f, 5.00f, 5.49f } },
        { { 7.02f, 8.07f, 9.00f, 9.85f, 10.51f } } } };
    static constexpr double kReverbTypeFadeSec = 0.03;    // a TYPE change's crossfade
    static constexpr double kTankDcHz = 15.0;  // the tanks' input DC blocker corner (D-L1)
    static constexpr float  kReverbSilent   = 1.0e-5f; // tank output under −100 dBFS …
    static constexpr double kReverbQuietSec = 0.2;     // … this long (> any tank's first arrival, ~80 ms) = rung out

    static constexpr float kWidthBoost = 1.0f;      // in-band side gain at width=full (×2)
    static constexpr float kWidthMidDb = 9.0f;      // CENTER: ±dB on MID at widthMid=±100 %

    double sampleRate = 44100.0;

    // Live (per-frame slewed) values + their setParams() targets. The live values
    // are what the audio actually uses; see the slew block at the top of process().
    float smoothA    = 0.05f;       // per-frame slew coeff (~15 ms)
    float offStepA   = 0.0015f;     // min per-frame step to neutral for an OFF panel (15 ms full scale)
    // Panel power (CharacterParams::on, chain order): OFF, and whether the stage's
    // state has been cleared since it landed on neutral (see process()).
    std::array<bool, loopnest::CharacterParams::kNumStages> stageOff {};
    std::array<bool, loopnest::CharacterParams::kNumStages> stageCleared {};
    double delaySlewA = 0.01;       // echo-tap slew coeff (~40 ms, tape bend)
    float cutEngStep  = 0.0045f;    // per-frame step of a cut's dead-zone crossfade (kCutFadeSec)
    bool  snapNext   = true;        // next setParams() snaps live = target (post-prepare)

    float  warp01     = 0.0f;
    float  warpTgt    = 0.0f;
    float  warpEng    = 0.0f;       // engage crossfade (wet/dry) — see process()
    double warpRateHz = kWarpRateMinHz;
    double warpSway    = 0.0;       // the sway, ms (see process())
    double warpShapePrev = 0.0;     // the modulation's shape last frame
    double warpLeakA   = 0.0;       // its settle coefficient (kWarpLeakSec)
    float  warpCachedAmt = -1.0f;   // warpAmpMs is for this amount …
    double warpCachedHz  = -1.0;    // … and rate
    double warpAmpMs     = 0.0;     // the sway's amplitude, ms (warpSweepFor × kWarpRangeMatch)
    loopnest::verb::RandomSpline warpDrift;   // WARP's drift

    float drive01        = 0.0f;
    float driveTgt       = 0.0f;
    float driveTone      = 0.0f;     // −1..+1 tilt (dark ↔ bright); 0 = flat
    float driveToneTgt   = 0.0f;
    // TONE tilt (see process()): coefficients for the live tone (cached on it), the
    // high-pass state per channel, and the level picture (see toneCompFor).
    float  toneCached = 2.0f, toneG = 1.0f, toneGInv = 1.0f, toneB0 = 1.0f, toneA1 = 0.0f;
    double tonePivotK = 0.13;
    std::array<float, kMaxCh> toneX1 { {} }, toneY1 { {} };
    std::array<float, 5> kwShelf {}, kwHipass {};                         // b0 b1 b2 a1 a2
    std::array<std::array<std::array<float, 2>, 2>, kMaxCh> kwState {};   // [ch][shelf, hp][z1, z2]
    std::array<float, kToneGrid> toneGridB0 {}, toneGridA1 {};
    std::array<std::array<std::array<float, 2>, kToneGrid>, kMaxCh> toneGridState {};   // [ch][corner][x1, y1]
    double toneIn = 0.0, toneLevelA = 0.0;
    std::array<double, kToneGrid> toneHpLevel {};
    float  driveEng = 0.0f;                         // the stage's engage crossfade (see process())
    std::array<DriveOs, kMaxCh> driveOs {};         // its 2x path per channel (see DriveOs)
    std::array<DriveLin, kMaxCh> driveDryLin {};    // the MIX dry tap's shadow of it (alignDry)
    std::array<float, 3> drvLiftB { { 1.0f, 0.0f, 0.0f } };   // the lift's b0..b2 …
    std::array<float, 2> drvLiftA { { 0.0f, 0.0f } };         // … and a1, a2 (kDriveLift*)
    bool   drvHistStale = true;                     // the ADAA history needs the live curve's log cosh
    bool   driveOsIdle  = true;                     // the stage was skipped since it last ran
    bool   driveDryIdle = true;                     // …and so was the shadow
    float  driveDryMix  = 1.0f;                     // the shadow's share while MIX > 0 (1) or at 0 (→ 0)
    bool   dryMixSnap   = true;                     // land driveDryMix on the next alignDry
    std::array<float, kMaxCh> driveDcX { {} };      // wet DC blocker state per channel
    std::array<float, kMaxCh> driveDcY { {} };
    float  driveDcR = 0.999f;                       // …and its pole (kDriveDcHz)
    // Loudness compensation (see driveCompTarget): the source's level picture
    // [positive, negative][bin], the growing weight new audio enters with, and the
    // slewed gain. driveCompFor < 0 = not computed since the stage was last bypassed.
    std::array<std::array<LevelBin, kLevelBins>, 2> driveLevel {};
    double driveLevelW = 1.0, driveLevelGrow = 1.0;
    bool   drivePriorHeld = false;                  // the picture is a prior, not yet the source's
    float  driveComp = 1.0f, driveCompTgt = 1.0f, driveCompFor = -1.0f;
    float  driveCompA = 0.01f;                      // its slew (~2 ms)
    int    driveCompTick = 0;

    float flutter01      = 0.0f;
    float flutterTgt     = 0.0f;
    float flutterEng     = 0.0f;     // engage crossfade (wet/dry) — see process()
    float flutterRateMul = 1.0f;     // wobble-speed multiplier (½× … 2×)
    float flutterCached  = -1.0f;    // flutterAmpMs is for this amount
    double flutterAmpMs  = 0.0;      // the wow's amplitude, ms (flutterDepthFor × kFlutterRangeMatch)
    loopnest::verb::RandomSpline wowDrift, flutDrift;   // FLUTTER's drifts
    float echo01       = 0.0f;
    float echoTgt      = 0.0f;
    float reverb01     = 0.0f;
    float reverbTgt    = 0.0f;
    float reverbDecay01 = 0.5f;     // REVERB DECAY (tertiary): tail length, target …
    float revDecay = 0.5f;          // … and live (slewed)
    float revDecayCached = -1.0f;   // the tanks' T30 is set for this decay
    int   revTypeTgt = kHall;       // REVERB TYPE asked for …
    int   revType = kHall;          // … the tank running …
    int   revFrom = kHall;          // … and, while revFade >= 0, the one fading out
    float revFade = -1.0f, revFadeStep = 0.0008f;   // the type crossfade 0..1 (< 0 = none)
    std::array<float, 3> revTypeGainC { { 1.0f, 1.0f, 1.0f } };   // each type's wet gain at revDecay
    float revInRamp = 0.0f;         // the tank input's rise since the amount left 0 (0..1)
    int   revRampHold = 0;          // frames after a reset() before it may rise (latency())
    std::array<float, kMaxCh> tankDcX { {} }, tankDcY { {} };   // the tanks' input DC blocker (kTankDcHz) …
    float tankDcR = 0.998f;                                      // … and its pole
    bool  reverbRinging = false;    // the tank holds a tail (runs at amount 0 until rung out)
    int   reverbQuiet = 0, reverbQuietLen = 4410;   // frames under kReverbSilent / needed

    // Stage-coefficient caches, keyed on the slewed amounts (which snap to target,
    // so these go quiescent once a knob settles). Saves tanh/pow/exp per sample.
    float driveCached = -1.0f, drvPre = 1.0f, drvBias = 0.0f, drvDcOff = 0.0f,
          drvNorm = 1.0f;
    float echoCached = -1.0f, echoFbC = 0.0f, echoMixC = 0.0f, echoMakeupC = 1.0f;
    float echoDampCoeff = 0.0f;     // fixed-Hz one-pole, set in prepare()
    float reverbCached = -1.0f, revWetC = 0.0f, revDryC = 1.0f, revMakeupC = 1.0f;

    std::array<float, kMaxCh> echoDampState { {} }; // echo feedback HF-rolloff state

    // Wet band-pass per effect: a low-cut (HP) + high-cut (LP) always run together;
    // each is a true bypass at its open edge (eng 0, see Cut).
    std::array<CutState, kMaxCh> echoHp   {};       // low-cut of the echo wet band
    std::array<CutState, kMaxCh> reverbHp {};       // low-cut of the reverb wet band
    std::array<CutState, kMaxCh> echoLp   {};       // high-cut of the echo wet band
    std::array<CutState, kMaxCh> reverbLp {};       // high-cut of the reverb wet band
    Cut echoHpCut { true }, reverbHpCut { true };
    Cut echoLpCut {},       reverbLpCut {};

    // Master EQ band-pass (pre-rack): low-cut (HP) + high-cut (LP) on the whole
    // signal; true-bypass at open edges (eng 0). See processEq().
    std::array<CutState, kMaxCh> eqHp {};
    std::array<CutState, kMaxCh> eqLp {};
    Cut eqHpCut { true }, eqLpCut {};

    // WIDTH band-pass on the mono SIDE (M/S) signal — one HP + one LP (the side is a
    // single stream). Selects which band stays stereo / gets widened. Runs every stereo frame.
    float  width01 = 0.0f;
    float  widthTgt = 0.0f;
    float  widthMid = 1.0f;          // CENTER (tertiary): linear MID gain; 1 = unity
    float  widthMidTgt = 1.0f;
    CutState widthSideHp {};
    CutState widthSideLp {};
    Cut      widthHpCut { true }, widthLpCut {};

    std::array<std::vector<float>, kMaxCh> warpLine {}; // WARP drift line
    int    warpSize  = 0;
    int    warpWrite = 0;
    double warpPhase = 0.0;

    std::array<std::vector<float>, kMaxCh> delay {};    // FLUTTER wobble line
    int    delaySize = 0;
    int    writePos  = 0;
    double wowPhase  = 0.0;
    double flutPhase = 0.0;
    double flutSway  = 0.0;         // FLUTTER's sway, ms (see process())
    double flutShapePrev = 0.0;     // its modulation's shape last frame
    double flutLeakA = 0.0;         // its settle coefficient (kFlutterLeakSec)
    // The knob-to-0 glides (glideStep): progress 0..1 (< 0 = none) and its step a frame.
    float  warpGlide = -1.0f, flutGlide = -1.0f, warpGlideStep = 1.0e-5f, flutGlideStep = 2.0e-5f;

    // landAtLine(): the params last given to setParams(), the step's, and the frames until
    // WARP's part and the rest land (< 0 = none pending).
    loopnest::CharacterParams applied, lineParams;
    int    lineWarpIn = -1, lineRestIn = -1;

    // The fixed latency (see the header): WARP's and FLUTTER's bases in whole frames, and
    // the caller's two dry taps [src, dry][channel] delayed by their sum (delayTaps).
    int    warpBaseN = 0, flutBaseN = 0;   // (0 until prepare(): an unprepared chain has no delay)
    std::array<std::array<std::vector<float>, kMaxCh>, 2> tapLine {};
    int    tapPos = 0;

    std::array<std::vector<float>, kMaxCh> echoBuf {};  // ECHO slapback line (max-length ring)
    int    echoLen   = 0;            // ring size (= max time)
    double echoDelayF   = 1.0;       // live read offset behind write head (slewed, fractional)
    double echoDelayTgt = 1.0;       // TIME knob target for the tap
    int    echoWrite = 0;

    loopnest::verb::Plate  plate;                       // REVERB's tanks (ReverbTanks.h)
    loopnest::verb::Hall   hall;
    loopnest::verb::Spring spring;
};
