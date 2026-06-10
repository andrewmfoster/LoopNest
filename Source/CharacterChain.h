#pragma once
#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <vector>

// Lo-fi character chain (Stage 2 DSP) shared by the live audition (processBlock)
// and the PRINT render (renderLoop) so the exported loop matches what was heard.
// One frame (up to 2 channels) is pushed through at a time; per-channel state
// lives here. Signal order: WARP → FLUTTER → DRIVE → ECHO → REVERB (output level
// is applied by the caller, after this chain). Every control is bypassed cleanly
// at its neutral value, so an all-neutral chain is a true pass-through (a clean
// trim prints with no added delay/colour).
//
// Knob mapping (matches createParameterLayout) — each effect has a secondary axis:
//   warp    0..100 %   slow, deep pitch drift (a warped record) — 0 = steady
//           rate       how fast the warp sways (lane strip) — slow drift → lurch
//   drive   0..100 %   asymmetric tanh overdrive (grit) — 0 = clean
//           tone       ±tilt on the grit (lane strip) — dark ↔ bright, 0 = flat
//   flutter 0..100 %   tape wow & flutter pitch wobble — 0 = steady
//           rate       wobble speed (lane strip) — half-speed ↔ double-speed
//   echo    0..100 %   tape slapback delay (HF-damped repeats) — 0 = dry
//   reverb  0..100 %   algorithmic room/space — 0 = dry
//   echo/reverb band   a two-handle band-pass on the wet path (RC20 "FOCUS"
//           style): lo handle = low-cut (high-pass) freq, hi handle = high-cut
//           (low-pass) freq, both 0..1 on a log axis (~20 Hz..20 kHz). lo≈0 and
//           hi≈1 each bypass cleanly, so a full-open band is a true pass-through.
//
// Identity intent: WARP is the seasick record-sway (deep + slow), FLUTTER the fine
// tape shimmer (subtle + fast), DRIVE adds grit, ECHO is the tape slapback, REVERB
// places it in a space. DRIVE is loudness-compensated so turning it up does not
// just get louder (see kDriveMakeupDb).
struct CharacterChain
{
    static constexpr int kMaxCh = 2;

    // One-pole high-pass pair (12 dB/oct) — low-cuts an effect's wet path. alpha==0
    // is a clean bypass (so the default ~20 Hz low-cut is a true pass-through).
    struct HpPair
    {
        float x1a = 0.0f, y1a = 0.0f, x1b = 0.0f, y1b = 0.0f;
        inline float process(float in, float alpha) noexcept
        {
            if (alpha <= 0.0f)
            {
                // Bypassed: keep tracking the input so engaging across the dead
                // zone starts from settled state (a low-cut's settled output is
                // ~0) instead of thumping from whatever was left behind.
                x1a = in; y1a = 0.0f; x1b = 0.0f; y1b = 0.0f;
                return in;
            }
            const float ya = alpha * (y1a + in - x1a); x1a = in; y1a = ya;
            const float yb = alpha * (y1b + ya - x1b); x1b = ya; y1b = yb;
            return yb;
        }
        void reset() noexcept { x1a = y1a = x1b = y1b = 0.0f; }
    };

    // One-pole low-pass pair (12 dB/oct) — high-cuts an effect's wet path. alpha>=1
    // is a clean bypass (cutoff above Nyquist → passband everywhere).
    struct LpPair
    {
        float y1 = 0.0f, y2 = 0.0f;
        inline float process(float in, float alpha) noexcept
        {
            if (alpha >= 1.0f)
            {
                // Bypassed: track the input (a wide-open low-pass follows it
                // exactly) so engaging across the dead zone starts settled.
                y1 = in; y2 = in;
                return in;
            }
            y1 += alpha * (in - y1);
            y2 += alpha * (y1 - y2);
            return y2;
        }
        void reset() noexcept { y1 = y2 = 0.0f; }
    };

    void prepare(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;

        // WARP drift delay line (base + max depth + margin, ms → samples).
        const int nWarp = (int) (sampleRate * 0.001 * (kWarpBaseMs + kWarpDepthMs + 8.0)) + 4;
        for (auto& w : warpLine)
            w.assign((size_t) nWarp, 0.0f);
        warpSize = nWarp;

        // FLUTTER wobble delay line (40 ms, ≫ max wobble).
        const int nWob = (int) (sampleRate * 0.04) + 4;
        for (auto& d : delay)
            d.assign((size_t) nWob, 0.0f);
        delaySize = nWob;

        // ECHO slapback delay line, sized to the MAX time; the live time is a read
        // offset (echoDelay) behind the write head, set in setParams.
        echoLen = juce::jmax(2, (int) (kEchoTimeMaxMs * 0.001 * sampleRate) + 4);
        for (auto& e : echoBuf)
            e.assign((size_t) echoLen, 0.0f);
        echoDelayTgt = juce::jlimit(1.0, (double) echoLen - 1.0, kEchoTimeMs * 0.001 * sampleRate);
        echoDelayF   = echoDelayTgt;

        // DRIVE tone tilt: one-pole split corner (fixed, ~1.8 kHz) — the band above
        // it is the "high" content the tilt boosts/cuts.
        driveLpCoeff = onePoleCoeff(kDriveToneHz);

        // ECHO HF-damp one-pole — fixed Hz, so computed once here, not per sample.
        echoDampCoeff = onePoleCoeff(kEchoDampHz);
        driveCached = echoCached = reverbCached = -1.0f;   // force coeff recompute

        // Per-frame parameter slew (declick): setParams() writes targets once per
        // block; process() eases the live values toward them so amounts/gains never
        // step at block boundaries. The echo tap slews slower (a tape-style bend
        // beats a click when TIME moves).
        smoothA    = (float)  (1.0 - std::exp(-1.0 / (0.015 * sampleRate)));
        delaySlewA = 1.0 - std::exp(-1.0 / (0.040 * sampleRate));
        snapNext   = true;   // first setParams() after prepare snaps live = target

        reverb.setSampleRate(sampleRate);
        reset();
    }

    void reset()
    {
        echoDampState.fill(0.0f);
        driveLp.fill(0.0f);
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
        warpWrite = 0;
        warpPhase = 0.0;
        writePos  = 0;
        echoWrite = 0;
        wowPhase  = 0.0;
        flutPhase = 0.0;
        reverb.reset();
    }

    // Live-coefficient setters from the raw param values (called once per block).
    // Each effect's secondary axis comes in alongside its main knob:
    //   warpRatePct / flutterRatePct  0..100 % (rate)
    //   driveTonePct                  −100..+100 % (tilt: − dark, + bright)
    //   echo/reverb band: two 0..1 handle positions on a log frequency axis —
    //       lo = low-cut (high-pass), hi = high-cut (low-pass). lo≈0 / hi≈1 bypass.
    void setParams(float warpPct, float warpRatePct,
                   float drivePct, float driveTonePct,
                   float flutterPct, float flutterRatePct,
                   float echoPct, float reverbPct,
                   float echoLo01 = 0.0f, float echoHi01 = 1.0f,
                   float reverbLo01 = 0.0f, float reverbHi01 = 1.0f,
                   float eqLo01 = 0.0f, float eqHi01 = 1.0f,
                   float widthPct = 0.0f, float widthLo01 = 0.0f, float widthHi01 = 1.0f,
                   float reverbDecayPct = 50.0f, float echoTimePct = 44.0f,
                   float widthMidPct = 0.0f)
    {
        // WIDTH (band-limited stereo): the two handles band-pass the SIDE (M/S) signal;
        // in-band side is widened, out-of-band side folds toward mono as WIDTH rises.
        // width01==0 → stage skipped in process() (true bypass). True-bypass band edges.
        // CENTER (tertiary): a ±dB gain on the MID (M) of the M/S split — independent of
        // width amount; widthMid==1 (0 %) is unity (no level change).
        widthTgt = juce::jlimit(0.0f, 1.0f, widthPct * 0.01f);
        bandCoeffs(widthLo01, widthHi01, widthHpAlpha, widthLpAlpha);
        widthMidTgt = std::pow(10.0f, (juce::jlimit(-100.0f, 100.0f, widthMidPct) * 0.01f
                                       * kWidthMidDb) / 20.0f);

        // Master EQ band-pass (pre-rack): same two-handle low-cut + high-cut math
        // as the wet bands, but the caller runs processEq() on the WHOLE signal
        // before the dry/wet tap (so it shapes dry + rack alike). True-bypass open.
        bandCoeffs(eqLo01, eqHi01, eqHpAlpha, eqLpAlpha);

        warpTgt    = juce::jlimit(0.0f, 1.0f, warpPct * 0.01f);
        warpRateHz = kWarpRateMinHz + (kWarpRateMaxHz - kWarpRateMinHz)
                                      * juce::jlimit(0.0, 1.0, (double) warpRatePct * 0.01);

        driveTgt     = juce::jlimit(0.0f, 1.0f, drivePct   * 0.01f);
        driveToneTgt = juce::jlimit(-1.0f, 1.0f, driveTonePct * 0.01f);
        flutterTgt   = juce::jlimit(0.0f, 1.0f, flutterPct * 0.01f);
        // RATE: exponential around 50 % = 1× (0 % = ½×, 100 % = 2×).
        flutterRateMul = std::pow(2.0f, (juce::jlimit(0.0f, 100.0f, flutterRatePct) - 50.0f) / 50.0f);
        echoTgt    = juce::jlimit(0.0f, 1.0f, echoPct    * 0.01f);
        reverbTgt  = juce::jlimit(0.0f, 1.0f, reverbPct  * 0.01f);
        reverbDecay01 = juce::jlimit(0.0f, 1.0f, reverbDecayPct * 0.01f);

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
            flutterEng = flutterTgt > 0.0f ? 1.0f : 0.0f;
            echoDelayF = echoDelayTgt;
            snapNext = false;
        }

        // Two-handle wet band-pass (echo + reverb): independent low-cut (HP) and
        // high-cut (LP) coeffs; each bypasses cleanly at its open edge.
        bandCoeffs(echoLo01,   echoHi01,   echoHpAlpha,   echoLpAlpha);
        bandCoeffs(reverbLo01, reverbHi01, reverbHpAlpha, reverbLpAlpha);

        // REVERB: the amount knob (reverb01) sets wet level; DECAY (tertiary) sets the
        // room size / tail length independently. We run the reverb WET-ONLY (dry=0) so
        // the dry signal can be re-mixed full-range in process() while only the wet
        // tail gets the low-cut; level is managed there too. Bypassed at reverb01==0.
        juce::Reverb::Parameters rp;
        rp.roomSize   = 0.30f + reverbDecay01 * 0.65f;  // DECAY: small → large hall
        rp.damping    = kReverbDamping;
        rp.wetLevel   = 1.0f;                           // pure wet out; we scale it ourselves
        rp.dryLevel   = 0.0f;
        rp.width      = 1.0f;
        rp.freezeMode = 0.0f;
        reverb.setParameters(rp);
    }

    // Master EQ — a two-handle band-pass run PRE-RACK by the caller (before the
    // dry/wet tap), so it sits between INPUT and FLUTTER in the chain and shapes
    // both the dry blend and the rack feed. True-bypass at open handles (lo≈0/hi≈1).
    void processEq(float* x, int nch)
    {
        nch = juce::jlimit(1, kMaxCh, nch);
        for (int ch = 0; ch < nch; ++ch)
        {
            float v = eqHp[(size_t) ch].process(x[ch], eqHpAlpha);
            v = eqLp[(size_t) ch].process(v, eqLpAlpha);
            x[ch] = v;
        }
    }

    // Process one frame in place. `x` holds `nch` (1 or 2) interpolated samples.
    void process(float* x, int nch)
    {
        nch = juce::jlimit(1, kMaxCh, nch);

        // Per-frame slew of every audible amount toward its setParams() target —
        // block-rate steps become short ramps (declick). warpEng/flutterEng are
        // engage crossfades: those stages replace the signal with a base-delayed
        // read, so on/off must fade wet/dry, not snap.
        // (Snapped once within 1e-5 of target — the exponential approach never lands
        // exactly, and the stage-coefficient caches below key on equality.)
        auto slew = [this](float& v, float t)
        { v += smoothA * (t - v); if (std::abs(t - v) < 1.0e-5f) v = t; };
        slew(warp01, warpTgt);
        slew(flutter01, flutterTgt);
        slew(drive01, driveTgt);
        slew(driveTone, driveToneTgt);
        slew(echo01, echoTgt);
        slew(reverb01, reverbTgt);
        slew(width01, widthTgt);
        slew(widthMid, widthMidTgt);
        slew(warpEng,    warpTgt    > 0.0f ? 1.0f : 0.0f);
        slew(flutterEng, flutterTgt > 0.0f ? 1.0f : 0.0f);
        echoDelayF += delaySlewA * (echoDelayTgt - echoDelayF);
        if (std::abs(echoDelayTgt - echoDelayF) < 0.05) echoDelayF = echoDelayTgt;

        // 0. WARP — slow, deep pitch drift (a warped record). A single slow sine
        //    modulates a fractional read of its own delay line: deeper and slower
        //    than FLUTTER, so the loop sways/melts rather than shimmers. Depth and
        //    rate are the two axes (knob + lane strip). The ring is ALWAYS written
        //    and advanced — engaging mid-play must read recent audio, not a frozen
        //    or starved line — and the wet (base-delayed) path crossfades in via
        //    warpEng. Fully faded out = exact pass-through (true trim preserved).
        if (warpSize > 0)
        {
            for (int ch = 0; ch < nch; ++ch)
                warpLine[(size_t) ch][(size_t) warpWrite] = x[ch];

            if (warpEng > 1.0e-3f)
            {
                const double mod    = (double) warp01 * kWarpDepthMs * std::sin(warpPhase);
                const double offset = (kWarpBaseMs + mod) * 0.001 * sampleRate;

                double readPos = (double) warpWrite - offset;
                while (readPos < 0.0) readPos += warpSize;
                const int   r0  = (int) readPos % warpSize;
                const int   r1  = (r0 + 1) % warpSize;
                const float fr  = (float) (readPos - std::floor(readPos));

                for (int ch = 0; ch < nch; ++ch)
                {
                    const float a = warpLine[(size_t) ch][(size_t) r0];
                    const float b = warpLine[(size_t) ch][(size_t) r1];
                    const float wet = a + fr * (b - a);
                    x[ch] = x[ch] + warpEng * (wet - x[ch]);
                }
            }
            warpWrite = (warpWrite + 1) % warpSize;

            warpPhase += juce::MathConstants<double>::twoPi * warpRateHz / sampleRate;
            if (warpPhase >= juce::MathConstants<double>::twoPi)
                warpPhase -= juce::MathConstants<double>::twoPi;
        }

        // 1. FLUTTER — modulated fractional delay (tape pitch wobble). Same
        //    always-write + engage-crossfade scheme as WARP; fully faded out is an
        //    exact pass-through so a clean print keeps its trim (no constant delay).
        if (delaySize > 0)
        {
            for (int ch = 0; ch < nch; ++ch)
                delay[(size_t) ch][(size_t) writePos] = x[ch];

            if (flutterEng > 1.0e-3f)
            {
                const double wow  = std::sin(wowPhase);
                const double flut = std::sin(flutPhase);
                const double modMs  = flutter01 * (kWowDepthMs * wow + kFlutDepthMs * flut);
                const double offset = (kBaseDelayMs + modMs) * 0.001 * sampleRate;

                double readPos = (double) writePos - offset;
                while (readPos < 0.0) readPos += delaySize;
                const int   r0  = (int) readPos % delaySize;
                const int   r1  = (r0 + 1) % delaySize;
                const float fr  = (float) (readPos - std::floor(readPos));

                for (int ch = 0; ch < nch; ++ch)
                {
                    const float a = delay[(size_t) ch][(size_t) r0];
                    const float b = delay[(size_t) ch][(size_t) r1];
                    const float wet = a + fr * (b - a);
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
        //    normalised to ~unity, dry/wet blended by the knob, and the wet path
        //    is loudness-compensated (kDriveMakeupDb) so turning DRIVE up adds
        //    grit without just getting louder. 0 % = clean bypass.
        if (drive01 > 0.0f)
        {
            // tanh/pow per sample only while the (slewed) amount is actually moving;
            // settled values hit the cache.
            if (drive01 != driveCached)
            {
                driveCached = drive01;
                drvPre    = 1.0f + drive01 * kDrivePre;
                drvBias   = drive01 * kDriveBias;
                drvDcOff  = std::tanh(drvBias);
                drvNorm   = std::tanh(drvPre + drvBias) - drvDcOff;   // peak at full-scale in
                drvMakeup = std::pow(10.0f, drive01 * kDriveMakeupDb / 20.0f);
            }
            const float pre    = drvPre;
            const float bias   = drvBias;
            const float dcOff  = drvDcOff;
            const float norm   = drvNorm;
            const float makeup = drvMakeup;
            // TONE: ±tilt of the grit. Split off the high band (signal − one-pole LP)
            // and add/subtract it: tone>0 brightens (boost highs), tone<0 darkens
            // (remove highs). 0 = flat (no tilt). Cheap, stable, true-bypass at 0.
            const float tilt = driveTone;
            for (int ch = 0; ch < nch; ++ch)
            {
                float wet = ((std::tanh(x[ch] * pre + bias) - dcOff) / norm) * makeup;
                if (tilt != 0.0f)
                {
                    driveLp[(size_t) ch] += driveLpCoeff * (wet - driveLp[(size_t) ch]);
                    const float high = wet - driveLp[(size_t) ch];
                    wet += tilt * high;
                }
                x[ch] = x[ch] * (1.0f - drive01) + wet * drive01;
            }
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
                wet = echoHp[(size_t) ch].process(wet, echoHpAlpha);
                wet = echoLp[(size_t) ch].process(wet, echoLpAlpha);
                echoBuf[(size_t) ch][(size_t) echoWrite] = in + wet * fb;         // feed back
                x[ch] = (in + wet * mix) * makeup;                                // dry + echoes
            }
            echoWrite = (echoWrite + 1) % echoLen;
        }

        // 4. REVERB — algorithmic room/space (JUCE Freeverb). Bypassed at 0 so a
        //    neutral chain stays a true pass-through; params set in setParams().
        if (reverb01 > 0.0f)
        {
            // Keep the dry signal, run the reverb wet-only (dry=0,wet=1 in setParams),
            // low-cut just the wet tail, then re-mix dry full-range + scaled wet. This
            // keeps lows (kick/bass) dry while the space sits above the cut frequency.
            float dry[kMaxCh];
            for (int ch = 0; ch < nch; ++ch) dry[ch] = x[ch];

            if (nch >= 2) reverb.processStereo(&x[0], &x[1], 1);   // x ← pure wet
            else          reverb.processMono(x, 1);

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
                float wet = x[ch];
                wet = reverbHp[(size_t) ch].process(wet, reverbHpAlpha);
                wet = reverbLp[(size_t) ch].process(wet, reverbLpAlpha);
                x[ch] = (dry[ch] * dryGain + wet * wetGain) * makeup;
            }
        }

        // 5. WIDTH — band-limited stereo width (M/S). Band-pass the SIDE: the in-band
        //    side is widened (×up to 1+kWidthBoost), the out-of-band side folds toward
        //    mono as WIDTH rises — so bass/whatever sits below the low handle stays
        //    centred while the band gets wider. Stereo-only; bypassed at width01==0
        //    (the ramp makes width=0 an exact pass-through, no discontinuity).
        // Runs when WIDTH is engaged OR CENTER (mid gain) is off unity — so the mid
        // trim works even with width at 0. At width01==0 the side passes untouched.
        if ((width01 > 0.0f || std::abs(widthMid - 1.0f) > 1.0e-4f) && nch >= 2)
        {
            const float mid  = 0.5f * (x[0] + x[1]) * widthMid;   // CENTER (mid) ±dB trim
            const float side = 0.5f * (x[0] - x[1]);
            float sideOut = side;
            if (width01 > 0.0f)
            {
                float inband = widthSideHp.process(side, widthHpAlpha);
                inband       = widthSideLp.process(inband, widthLpAlpha);
                const float outband = side - inband;
                sideOut = inband  * (1.0f + width01 * kWidthBoost)
                        + outband * (1.0f - width01);
            }
            x[0] = mid + sideOut;
            x[1] = mid - sideOut;
        }
    }

private:
    float onePoleCoeff(float fc) const
    {
        const float c = 1.0f - std::exp(-juce::MathConstants<float>::twoPi
                                        * juce::jlimit(1.0f, (float) sampleRate * 0.49f, fc)
                                        / (float) sampleRate);
        return juce::jlimit(0.0f, 1.0f, c);
    }

    // One-pole high-pass alpha for cutoff `hz`; ≤ kHpOffHz returns 0 (bypass).
    float hpAlpha(float hz) const
    {
        if (hz <= kHpOffHz) return 0.0f;
        const float dt = 1.0f / (float) sampleRate;
        const float rc = 1.0f / (juce::MathConstants<float>::twoPi
                                 * juce::jlimit(1.0f, (float) sampleRate * 0.49f, hz));
        return rc / (rc + dt);
    }

    // Map a band's two handle positions (each 0..1 on a log frequency axis) to a
    // low-cut (high-pass) alpha and a high-cut (low-pass) alpha — both run on the
    // wet path at once (a band-pass). The HP bypasses when lo ≈ 0 (≈ kBandFLo) and
    // the LP bypasses when hi ≈ 1 (≈ kBandFHi), so a full-open band is pass-through.
    float bandFreq(float p01) const
    { return kBandFLo * std::pow(kBandFHi / kBandFLo, juce::jlimit(0.0f, 1.0f, p01)); }

    void bandCoeffs(float lo01, float hi01, float& hpA, float& lpA) const
    {
        hpA = (lo01 <= kBandEdge)         ? 0.0f : hpAlpha(bandFreq(lo01));
        lpA = (hi01 >= 1.0f - kBandEdge)  ? 1.0f : onePoleCoeff(bandFreq(hi01));
    }

    // Tunable character constants (taste, not fidelity — adjust by ear).
    static constexpr double kWarpRateMinHz = 0.15;  // slowest sway (deep drift)
    static constexpr double kWarpRateMaxHz = 1.6;   // fastest sway (lurchy)
    static constexpr double kWarpDepthMs   = 12.0;  // max pitch-drift depth (≫ flutter)
    static constexpr double kWarpBaseMs    = 16.0;  // > kWarpDepthMs so offset stays positive

    static constexpr double kWowRateHz   = 0.7;
    static constexpr double kFlutRateHz  = 6.0;
    static constexpr double kWowDepthMs  = 3.0;
    static constexpr double kFlutDepthMs = 1.2;
    static constexpr double kBaseDelayMs = 8.0;   // > max wobble, so offset stays positive

    static constexpr float kDrivePre      = 10.0f;  // tanh pre-gain at full drive
    static constexpr float kDriveBias     = 0.30f;  // asymmetry → even harmonics
    static constexpr float kDriveMakeupDb = -5.0f;  // wet-path loudness comp at full drive
    static constexpr float kDriveToneHz   = 1800.0f;// TONE tilt split corner (dark/bright)

    // Wet band-pass frequency axis: both handles map 0..1 → kBandFLo..kBandFHi on a
    // log scale. kBandEdge is the dead band at each end where the filter bypasses.
    static constexpr float kBandFLo = 20.0f;
    static constexpr float kBandFHi = 20000.0f;
    static constexpr float kBandEdge = 0.02f;

    static constexpr double kEchoTimeMs    = 170.0; // default slapback time (recall-safe)
    static constexpr double kEchoTimeMinMs = 30.0;  // ECHO TIME knob min (tight slap)
    static constexpr double kEchoTimeMaxMs = 350.0; // ECHO TIME knob max (short dub)
    static constexpr float  kEchoFbMax    = 0.55f;  // feedback at echo=full (repeat count)
    static constexpr float  kEchoMixMax   = 0.50f;  // wet add at echo=full
    static constexpr float  kEchoDampHz   = 3200.0f;// HF rolloff per repeat (tape feel)
    static constexpr float  kEchoMakeupDb = -2.5f;  // output trim at echo=full (loudness comp)

    static constexpr float kReverbWetMax  = 0.45f;  // wet level at reverb=full
    static constexpr float kReverbDamping = 0.40f;  // HF damping inside the tail
    static constexpr float kReverbMakeupDb = -3.5f; // output trim at reverb=full (loudness comp)

    static constexpr float kWidthBoost = 1.0f;      // in-band side gain at width=full (×2)
    static constexpr float kWidthMidDb = 9.0f;      // CENTER: ±dB on MID at widthMid=±100 %

    static constexpr float kHpOffHz       = 22.0f;  // ≤ this cutoff = low-cut bypassed

    double sampleRate = 44100.0;

    // Live (per-frame slewed) values + their setParams() targets. The live values
    // are what the audio actually uses; see the slew block at the top of process().
    float smoothA    = 0.05f;       // per-frame slew coeff (~15 ms)
    double delaySlewA = 0.01;       // echo-tap slew coeff (~40 ms, tape bend)
    bool  snapNext   = true;        // next setParams() snaps live = target (post-prepare)

    float  warp01     = 0.0f;
    float  warpTgt    = 0.0f;
    float  warpEng    = 0.0f;       // engage crossfade (wet/dry) — see process()
    double warpRateHz = kWarpRateMinHz;

    float drive01        = 0.0f;
    float driveTgt       = 0.0f;
    float driveTone      = 0.0f;     // −1..+1 tilt (dark ↔ bright); 0 = flat
    float driveToneTgt   = 0.0f;
    float driveLpCoeff   = 0.0f;     // one-pole split corner for the tilt
    std::array<float, kMaxCh> driveLp { {} };       // tilt split state per channel

    float flutter01      = 0.0f;
    float flutterTgt     = 0.0f;
    float flutterEng     = 0.0f;     // engage crossfade (wet/dry) — see process()
    float flutterRateMul = 1.0f;     // wobble-speed multiplier (½× … 2×)
    float echo01       = 0.0f;
    float echoTgt      = 0.0f;
    float reverb01     = 0.0f;
    float reverbTgt    = 0.0f;
    float reverbDecay01 = 0.5f;     // REVERB DECAY (tertiary): room size / tail length

    // Stage-coefficient caches, keyed on the slewed amounts (which snap to target,
    // so these go quiescent once a knob settles). Saves tanh/pow/exp per sample.
    float driveCached = -1.0f, drvPre = 1.0f, drvBias = 0.0f, drvDcOff = 0.0f,
          drvNorm = 1.0f, drvMakeup = 1.0f;
    float echoCached = -1.0f, echoFbC = 0.0f, echoMixC = 0.0f, echoMakeupC = 1.0f;
    float echoDampCoeff = 0.0f;     // fixed-Hz one-pole, set in prepare()
    float reverbCached = -1.0f, revWetC = 0.0f, revDryC = 1.0f, revMakeupC = 1.0f;

    std::array<float, kMaxCh> echoDampState { {} }; // echo feedback HF-rolloff state

    // Wet band-pass per effect: a low-cut (HP) + high-cut (LP) always run together;
    // each is a true bypass at its open edge (HP alpha 0 / LP alpha 1).
    std::array<HpPair, kMaxCh> echoHp   {};         // low-cut of the echo wet band
    std::array<HpPair, kMaxCh> reverbHp {};         // low-cut of the reverb wet band
    std::array<LpPair, kMaxCh> echoLp   {};         // high-cut of the echo wet band
    std::array<LpPair, kMaxCh> reverbLp {};         // high-cut of the reverb wet band
    float echoHpAlpha    = 0.0f, reverbHpAlpha = 0.0f;
    float echoLpAlpha    = 1.0f, reverbLpAlpha = 1.0f;

    // Master EQ band-pass (pre-rack): low-cut (HP) + high-cut (LP) on the whole
    // signal; true-bypass at open edges (HP alpha 0 / LP alpha 1). See processEq().
    std::array<HpPair, kMaxCh> eqHp {};
    std::array<LpPair, kMaxCh> eqLp {};
    float eqHpAlpha = 0.0f, eqLpAlpha = 1.0f;

    // WIDTH band-pass on the mono SIDE (M/S) signal — one HP + one LP (the side is a
    // single stream). Selects which band stays stereo / gets widened. width01==0 skips.
    float  width01 = 0.0f;
    float  widthTgt = 0.0f;
    float  widthMid = 1.0f;          // CENTER (tertiary): linear MID gain; 1 = unity
    float  widthMidTgt = 1.0f;
    HpPair widthSideHp {};
    LpPair widthSideLp {};
    float  widthHpAlpha = 0.0f, widthLpAlpha = 1.0f;

    std::array<std::vector<float>, kMaxCh> warpLine {}; // WARP drift line
    int    warpSize  = 0;
    int    warpWrite = 0;
    double warpPhase = 0.0;

    std::array<std::vector<float>, kMaxCh> delay {};    // FLUTTER wobble line
    int    delaySize = 0;
    int    writePos  = 0;
    double wowPhase  = 0.0;
    double flutPhase = 0.0;

    std::array<std::vector<float>, kMaxCh> echoBuf {};  // ECHO slapback line (max-length ring)
    int    echoLen   = 0;            // ring size (= max time)
    double echoDelayF   = 1.0;       // live read offset behind write head (slewed, fractional)
    double echoDelayTgt = 1.0;       // TIME knob target for the tap
    int    echoWrite = 0;

    juce::Reverb reverb;                                // REVERB (Freeverb)
};
