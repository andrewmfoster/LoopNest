#pragma once
#include <algorithm>
#include <cmath>

// The A/B monitor pair, GAIN MATCH and BYPASS, as one stage shared by AUDITION, FX
// and the wet print (one code path, so what a mode plays and what a print bakes
// cannot drift apart).
//
//   GAIN MATCH  scales the processed signal to the loudness of the dry reference.
//               Part of the sound: it runs in both modes and bakes into a wet print.
//   BYPASS      swaps the output to the dry reference. A monitor only: never printed.
//
// The dry reference is the source after INPUT and before the master EQ (the EQ counts
// as part of "the FX"). Loudness is K-weighted (ITU-R BS.1770: high shelf +4 dB at
// ~1.68 kHz, then high-pass at ~38 Hz): flat RMS under-counts the 1-5 kHz harmonics
// DRIVE adds, where the ear is most sensitive. Each channel is weighted and squared
// on its own and the powers summed, as BS.1770 does; a mono sum cancels whatever
// REVERB and WIDTH put in the sides and reads the processed signal too quiet.
namespace loopnest
{
class MonitorStage
{
public:
    // windowSec: the loudness trackers' integration time. smoothSec: the makeup
    // gain's own slew. Call before prepare().
    void setTimes(double windowSec, double smoothSec) noexcept
    {
        window = windowSec;
        smooth = smoothSec;
    }

    void prepare(double sampleRate)
    {
        // RBJ biquads rebuilt for this rate. Published filter targets (De Man 2013
        // derivation, valid at any fs): stage 1 high shelf f0≈1681.97 Hz, +4 dB,
        // Q≈0.7072 · stage 2 HP f0≈38.14 Hz, Q≈0.5003.
        const double twoPi = 6.283185307179586;
        const auto normalize = [] (Biquad& c, double a0)
        {
            c.b0 = (float) (c.b0 / a0); c.b1 = (float) (c.b1 / a0); c.b2 = (float) (c.b2 / a0);
            c.a1 = (float) (c.a1 / a0); c.a2 = (float) (c.a2 / a0);
        };
        {
            const double f0 = 1681.9744509555319, gDb = 3.9998438539733446, q = 0.7071752369554196;
            const double A  = std::pow(10.0, gDb / 40.0);
            const double w0 = twoPi * f0 / sampleRate;
            const double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * q);
            const double sa = 2.0 * std::sqrt(A) * alpha;
            shelf.b0 = (float) (A * ((A + 1.0) + (A - 1.0) * cw + sa));
            shelf.b1 = (float) (-2.0 * A * ((A - 1.0) + (A + 1.0) * cw));
            shelf.b2 = (float) (A * ((A + 1.0) + (A - 1.0) * cw - sa));
            shelf.a1 = (float) (2.0 * ((A - 1.0) - (A + 1.0) * cw));
            shelf.a2 = (float) ((A + 1.0) - (A - 1.0) * cw - sa);
            normalize(shelf, (A + 1.0) - (A - 1.0) * cw + sa);
        }
        {
            const double f0 = 38.13547087602444, q = 0.5003270373238773;
            const double w0 = twoPi * f0 / sampleRate;
            const double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * q);
            hipass.b0 = (float) ((1.0 + cw) * 0.5);
            hipass.b1 = (float) (-(1.0 + cw));
            hipass.b2 = (float) ((1.0 + cw) * 0.5);
            hipass.a1 = (float) (-2.0 * cw);
            hipass.a2 = (float) (1.0 - alpha);
            normalize(hipass, 1.0 + alpha);
        }
        msCoeff    = 1.0 - std::exp(-1.0 / (window * sampleRate));
        matchCoeff = (float) (1.0 - std::exp(-1.0 / (smooth * sampleRate)));
        bypCoeff   = (float) (1.0 - std::exp(-1.0 / (0.005 * sampleRate)));
        reset(false);
    }

    // Fresh trackers and unity gain (a play edge, a mode switch, a print). `bypassed`
    // snaps the A/B blend so the first block has no surprise crossfade.
    void reset(bool bypassed) noexcept
    {
        for (auto& tap : kw)
            for (auto& ch : tap)
                for (auto& s : ch)
                    s.reset();
        refMS = procMS = 0.0;
        matchGain = 1.0f;
        bypBlend  = bypassed ? 1.0f : 0.0f;
    }

    // One frame. `src` is the dry reference, `io` the processed blend on the way in
    // and the monitor output on the way out (before OUTPUT gain).
    void process(const float* src, float* io, int numChannels, bool gainMatch, bool bypass) noexcept
    {
        const int n = std::min(numChannels, 2);
        double r = 0.0, p = 0.0;
        for (int ch = 0; ch < n; ++ch)
        {
            const float rK = kw[0][ch][1].process(hipass, kw[0][ch][0].process(shelf, src[ch]));
            const float pK = kw[1][ch][1].process(hipass, kw[1][ch][0].process(shelf, io[ch]));
            r += (double) rK * rK;
            p += (double) pK * pK;
        }
        refMS  += msCoeff * (r - refMS);
        procMS += msCoeff * (p - procMS);

        // Slew the makeup toward sqrt(ref/proc). While the dry reference is ~silent
        // (echo/reverb tails only) the gain HOLDS rather than chasing the clamp. Off,
        // it decays back to unity, so toggling never jumps.
        if (gainMatch)
        {
            if (refMS > kFloor && procMS > kFloor)
                matchGain += matchCoeff * (std::clamp((float) std::sqrt(refMS / procMS), 1.0f / kMaxGain, kMaxGain)
                                           - matchGain);
        }
        else
            matchGain += matchCoeff * (1.0f - matchGain);

        // BYPASS -> the dry reference, over a ~5 ms crossfade: a hard per-sample swap
        // of phase/level-different signals clicks on every A/B.
        bypBlend += bypCoeff * ((bypass ? 1.0f : 0.0f) - bypBlend);
        for (int ch = 0; ch < n; ++ch)
        {
            const float proc = io[ch] * matchGain;
            io[ch] = proc + bypBlend * (src[ch] - proc);
        }
    }

    float gain() const noexcept { return matchGain; }

    static constexpr float kMaxGain = 15.848932f;   // +/-24 dB

private:
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

    static constexpr double kFloor = 1.0e-9;   // below this a tap counts as silent

    double window = 1.0, smooth = 0.2;
    Biquad shelf, hipass;
    BiquadState kw[2][2][2];                   // [ref, proc][channel][shelf, high-pass]
    double refMS = 0.0, procMS = 0.0, msCoeff = 0.0;
    float  matchGain = 1.0f, matchCoeff = 0.0f;
    float  bypBlend = 0.0f,  bypCoeff = 0.01f;
};
}
