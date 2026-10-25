#pragma once

// Pitch-preserving time-stretch for SYNC (proven in docs/spike-fx-sync.md, round 2).
// One include site for the vendored Signalsmith Stretch (third_party/, MIT), plus
// the cyclic loop render the HATCH print uses. No JUCE in here, so an
// offline check can include exactly the code the plugin runs.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// The vendored headers trip ~290 of JUCE's recommended warnings (sign/float
// conversions, shadowing, extra ';', float ==). Silenced for this include only.
#if defined(__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Wconversion"
 #pragma clang diagnostic ignored "-Wsign-conversion"
 #pragma clang diagnostic ignored "-Wshorten-64-to-32"
 #pragma clang diagnostic ignored "-Wimplicit-float-conversion"
 #pragma clang diagnostic ignored "-Wimplicit-int-float-conversion"
 #pragma clang diagnostic ignored "-Wfloat-conversion"
 #pragma clang diagnostic ignored "-Wshadow-all"
 #pragma clang diagnostic ignored "-Wextra-semi"
 #pragma clang diagnostic ignored "-Wfloat-equal"
#endif
#include "signalsmith-stretch/signalsmith-stretch.h"
#if defined(__clang__)
 #pragma clang diagnostic pop
#endif

namespace loopnest
{
using Stretcher = signalsmith::stretch::SignalsmithStretch<float>;

// A loop seam's crossfade (the print's cycle into its continuation here, the region's head
// over the audio past its end in readSeamed): the two gains at t (0..1 across it) for a
// head and continuation whose correlation is rho (0..1). Sine / cosine held at constant
// power for that correlation: rho 0 is plain equal power, rho 1 sums to exactly 1. The
// two are often the same audio a cycle apart, where equal power swelled the head +1.3 to
// +3 dB (D-L2, L2 of the 10-18 review); where they aren't, equal gain would dip.
inline void seamGains(double t01, double rho, float& head, float& cont) noexcept
{
    const double th = t01 * 1.5707963267948966, s = std::sin(th), c = std::cos(th);
    const double g = 1.0 / std::sqrt(1.0 + 2.0 * std::clamp(rho, 0.0, 1.0) * s * c);
    head = (float) (s * g);
    cont = (float) (c * g);
}
// The correlation of `n` frames of a head and its continuation, frame k each (0 when either
// is silent: no telling, so plain equal power).
template <typename Head, typename Cont>
double seamCorrelation(Head&& head, Cont&& cont, int n) noexcept
{
    double hh = 0.0, cc = 0.0, hc = 0.0;
    for (int k = 0; k < n; ++k)
    {
        const double h = head(k), c = cont(k);
        hh += h * h; cc += c * c; hc += h * c;
    }
    return hh > 0.0 && cc > 0.0 ? std::clamp(hc / std::sqrt(hh * cc), 0.0, 1.0) : 0.0;
}

// The audition's stretcher, wrapped so PluginProcessor.h can hold it by pointer
// without including this header (see the note there).
struct SyncStretcher
{
    Stretcher st;
};

// SYNC tempo ratio (hostBPM / nativeBPM) is clamped to this range. Past ~0.5x /
// 2x the stretcher starts randomising phases and smears; beyond 4x it's noise.
// The audition's scratch buffers are sized for kMaxSyncRatio.
constexpr double kMinSyncRatio = 0.25;
constexpr double kMaxSyncRatio = 4.0;

// Render one seamless loop cycle of a region, time-stretched to exactly `numOut`
// frames (tempo ratio regionLen / numOut) and transposed by `semitones`, at the
// source rate. `readFrame(k, l, r)` writes region frame k (0..regionLen-1) of the
// cyclic source stream — the caller's seam-faded read, so the stream is exactly
// what the looped audition voice feeds its stretcher.
//
// Cyclic: the stretcher is seeded (seek) with the region's own tail as input
// history, then runs two full cycles of the wrapped region. The input consumed
// after output frame j is round(j * regionLen / numOut), so every numOut output
// frames eat exactly regionLen input frames and cycles stay phase-aligned. Output
// frame j carries input time (j - outputLatency) * ratio - inputLatency, so the
// kept cycle — the second one, with a whole cycle of warm-up behind it — starts
// at j0 = numOut + outputLatency + inputLatency / ratio (latency compensated: no
// pre-roll, fade-in or gap at the head). Its first few ms are crossfaded against
// the continuation past its end, so the wrap lands on the head without a step even
// where a steady tone's stretched phase doesn't close on a whole period: seamGains, for
// the pair's own correlation over the first ~10 ms.
//
// Message thread only (allocates). Fixed seed: a re-hatch prints identical audio.
// Returns false if there's nothing to render.
template <typename ReadFrame>
bool renderCyclicStretch(ReadFrame&& readFrame, int regionLen, int numOut,
                         double sampleRate, float semitones,
                         float* outL, float* outR)
{
    if (regionLen <= 0 || numOut <= 0 || sampleRate <= 0.0)
        return false;

    Stretcher st(0x4c4e5354L);
    st.presetDefault(2, (float) sampleRate);
    st.setTransposeSemitones(semitones);

    const double ratio = (double) regionLen / (double) numOut;
    auto wrap = [regionLen] (std::int64_t k)
    {
        const auto m = k % regionLen;
        return (int) (m < 0 ? m + regionLen : m);
    };

    // History: the seekLength() frames just before the region start, wrapped — i.e.
    // the region's tail — so the first analysis windows see real loop audio.
    const int histLen = st.seekLength();
    std::vector<float> hl((size_t) histLen), hr((size_t) histLen);
    for (int i = 0; i < histLen; ++i)
        readFrame(wrap((std::int64_t) i - histLen), hl[(size_t) i], hr[(size_t) i]);
    {
        float* h[2] = { hl.data(), hr.data() };
        st.seek(h, histLen, ratio);
    }

    const int seam  = std::min((int) std::lround(0.002 * sampleRate), numOut / 4);
    const int corr  = seam >= 2 ? std::max(seam, std::min((int) std::lround(0.01 * sampleRate), numOut / 4)) : 0;
    const int lag   = (int) std::lround((double) st.outputLatency()
                                        + (double) st.inputLatency() / ratio);
    const int j0    = numOut + lag;
    const int total = j0 + numOut + corr;

    std::vector<float> ol((size_t) total), orr((size_t) total);
    constexpr int kChunk = 1024;
    const int maxIn = (int) std::ceil(kChunk * ratio) + 2;
    std::vector<float> il((size_t) maxIn), ir((size_t) maxIn);

    std::int64_t fedIn = 0;   // input frames handed over so far (stream index)
    for (int j = 0; j < total; j += kChunk)
    {
        const int nOut = std::min(kChunk, total - j);
        const std::int64_t wantIn = ((std::int64_t) (j + nOut) * regionLen + numOut / 2) / numOut;
        const int nIn = (int) (wantIn - fedIn);
        for (int i = 0; i < nIn; ++i)
            readFrame(wrap(fedIn + i), il[(size_t) i], ir[(size_t) i]);
        fedIn = wantIn;

        float* in[2]  = { il.data(), ir.data() };
        float* out[2] = { ol.data() + j, orr.data() + j };
        st.process(in, nIn, out, nOut);
    }

    // The head against its continuation (~10 ms), both channels as one.
    const double rho = seamCorrelation([&] (int k) { return (double) (k % 2 ? orr : ol)[(size_t) (j0 + k / 2)]; },
                                       [&] (int k) { return (double) (k % 2 ? orr : ol)[(size_t) (j0 + numOut + k / 2)]; },
                                       2 * corr);
    for (int i = 0; i < numOut; ++i)
    {
        float l = ol[(size_t) (j0 + i)], r = orr[(size_t) (j0 + i)];
        if (seam >= 2 && i < seam)
        {
            float a, b;
            seamGains((double) i / (double) seam, rho, a, b);
            l = l * a + ol [(size_t) (j0 + numOut + i)] * b;
            r = r * a + orr[(size_t) (j0 + numOut + i)] * b;
        }
        outL[i] = l;
        outR[i] = r;
    }
    return true;
}
} // namespace loopnest
