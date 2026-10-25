#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

// REVERB's three tanks (CharacterChain stage 4): PLATE (Dattorro), HALL (an 8-line FDN)
// and SPRING (a dispersive allpass tank). Ported from the 10-17 listening spike Andrew
// A/B'd and picked by ear; written for LoopNest, no third-party code. Each is a stereo
// WET-ONLY tank: CharacterChain feeds it, band-passes what comes out and mixes it with
// the dry. Everything is allocated in prepare(); process(), setT30() and reset() never
// allocate. Each tank is deterministic from reset() (its modulation restarts too), so a
// print primed from a fresh chain hears what the audition's pass (priming passes + 1)
// from play start did. Not every later pass: the modulation is not tied to the loop.
//
// setT30(s) puts the tank's measured T30 (impulse in, both channels' Schroeder
// integral, -5..-35 dB doubled, as LoopNestMeasure) on `s`: each tank's tuning value is
// read off its own table, measured at 48 kHz on log-spaced T30 nodes and interpolated
// in a log domain where it is close to linear (see each table). Plain C++: no JUCE, so
// the tables' calibration tool compiles against this header alone.
namespace loopnest::verb
{
constexpr double kPi = 3.14159265358979323846;

// A power-of-two ring. get(d) = the sample pushed d frames ago, read BEFORE this frame's
// push (d >= 1); getHermite(d) a 4-point cubic Hermite read between them (d >= 2).
struct Ring
{
    std::vector<float> buf;
    int mask = 0, w = 0;
    void init(int maxDelay)
    {
        int n = 1;
        while (n < maxDelay + 8) n <<= 1;
        buf.assign((size_t) n, 0.0f);
        mask = n - 1;
        w = 0;
    }
    void clear() noexcept { std::fill(buf.begin(), buf.end(), 0.0f); w = 0; }
    inline void push(float v) noexcept { buf[(size_t) w] = v; w = (w + 1) & mask; }
    inline float get(int d) const noexcept { return buf[(size_t) ((w - d) & mask)]; }
    inline float getHermite(double d) const noexcept
    {
        const int i = (int) d;
        const float f = (float) (d - i);
        const float xm1 = get(i - 1), x0 = get(i), x1 = get(i + 1), x2 = get(i + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }
};

// Schroeder allpass (z^-L - g) / (1 - g z^-L); its length can be modulated (Hermite read).
struct Allpass
{
    Ring d;
    int len = 1;
    float g = 0.5f;
    void init(int L, float gain, int extra = 0) { len = L; g = gain; d.init(L + extra + 4); }
    inline float process(float x) noexcept
    {
        const float z = d.get(len), v = x + g * z;
        d.push(v);
        return z - g * v;
    }
    inline float processMod(float x, double delay) noexcept
    {
        const float z = d.getHermite(delay), v = x + g * z;
        d.push(v);
        return z - g * v;
    }
    float tap(int k) const noexcept { return d.get(k + 1); }   // the node k frames old (after this frame's push)
};

struct OnePoleLP
{
    float a = 1.0f, y = 0.0f;
    void set(double fc, double sr) { a = (float) (1.0 - std::exp(-2.0 * kPi * fc / sr)); }
    inline float process(float x) noexcept { y += a * (x - y); return y; }
};

struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void rbj(double fc, double q, double sr, bool hp)   // RBJ low-pass / high-pass
    {
        // Held under Nyquist (C-M4): SPRING's 4057 Hz low-passes on an 8 kHz file's wet
        // print put the pole at 1.032, unstable. A design past Nyquist has no meaning.
        fc = std::min(fc, 0.45 * sr);
        const double w0 = 2.0 * kPi * fc / sr, cw = std::cos(w0), al = std::sin(w0) / (2.0 * q), a0 = 1.0 + al;
        const double bb0 = hp ? (1.0 + cw) * 0.5 : (1.0 - cw) * 0.5, bb1 = hp ? -(1.0 + cw) : (1.0 - cw);
        b0 = (float) (bb0 / a0); b1 = (float) (bb1 / a0); b2 = b0;
        a1 = (float) (-2.0 * cw / a0); a2 = (float) ((1.0 - al) / a0);
    }
    void clear() noexcept { z1 = z2 = 0.0f; }
    inline float process(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Smooth random drift: a Catmull-Rom spline through uniform random points in [-1, 1],
// `rateHz` points a second. Deterministic for a seed (std::mt19937 is specified to the
// bit), so restart() replays the same drift. The rate can change on the fly.
struct RandomSpline
{
    std::mt19937 rng;
    uint32_t seed = 1;
    float p[4] {};
    double t = 0.0, step = 0.0;
    float rnd() noexcept { return (float) ((double) rng() / 4294967295.0 * 2.0 - 1.0); }
    void restart(uint32_t s) noexcept
    {
        seed = s;
        rng.seed(seed);
        for (auto& v : p) v = rnd();
        t = 0.0;
    }
    void setRate(double rateHz, double sr) noexcept { step = rateHz / sr; }
    inline float next() noexcept
    {
        const float f = (float) t;
        const float y = 0.5f * (2.0f * p[1] + (-p[0] + p[2]) * f
                                + (2.0f * p[0] - 5.0f * p[1] + 4.0f * p[2] - p[3]) * f * f
                                + (-p[0] + 3.0f * p[1] - 3.0f * p[2] + p[3]) * f * f * f);
        t += step;
        if (t >= 1.0)
        {
            t -= 1.0;
            p[0] = p[1]; p[1] = p[2]; p[2] = p[3]; p[3] = rnd();
        }
        return std::clamp(y, -1.2f, 1.2f);
    }
};

// A tuning value off a calibration table: `nodes` values at T30 = t0 · ratio^(k / (n - 1)),
// interpolated linearly in log T30 (clamped at the ends).
template <size_t N>
inline double fromTable(const std::array<double, N>& nodes, double t0, double t1, double t30)
{
    const double u = std::clamp(std::log(t30 / t0) / std::log(t1 / t0), 0.0, 1.0) * (double) (N - 1);
    const size_t i = std::min(N - 2, (size_t) u);
    const double f = u - (double) i;
    return nodes[i] + f * (nodes[i + 1] - nodes[i]);
}

// ── PLATE: Dattorro's plate (JAES 1997, "Effect Design Part 1"), delays scaled from his
// 29761 Hz. A mono input through a bandwidth one-pole and four input diffusers feeds a
// figure-eight tank of two halves (modulated allpass, delay, damping, allpass, delay);
// each output sums seven taps across both halves. Bright, dense, quick to build.
struct Plate
{
    // Tank gain against T30: ln(-ln g) at T30 nodes 0.78 .. 5.5 s (9, geometric), 48 kHz.
    static constexpr double kT30Lo = 0.78, kT30Hi = 5.5;
    static constexpr std::array<double, 9> kTable { { 0.47955, 0.22757, -0.01548, -0.28765, -0.55365,
                                                      -0.80514, -1.06602, -1.32104, -1.57257 } };

    double sr = 48000.0;
    float decay = 0.5f;                     // tank gain
    double preMs = 15.0, bwHz = 11000.0, dampHz = 7500.0, modHz = 0.9, excursion = 16.0;
    Ring pre; int preLen = 1;
    Allpass in1, in2, in3, in4, apL1, apL2, apR1, apR2;
    Ring dL1, dL2, dR1, dR2; int nL1 = 1, nL2 = 1, nR1 = 1, nR2 = 1;
    OnePoleLP bw, dampL, dampR;
    double phase = 0.0, phaseStep = 0.0, ex = 0.0;
    int tl[7] {}, tr[7] {};

    void prepare(double rate)
    {
        sr = rate;
        const double s = sr / 29761.0;
        auto S = [s] (int n) { return std::max(1, (int) std::lround(n * s)); };
        preLen = std::max(1, (int) std::lround(preMs * 0.001 * sr));
        pre.init(preLen + 2);
        bw.set(bwHz, sr); dampL.set(dampHz, sr); dampR.set(dampHz, sr);
        in1.init(S(142), 0.75f); in2.init(S(107), 0.75f); in3.init(S(379), 0.625f); in4.init(S(277), 0.625f);
        const int exN = (int) std::ceil(excursion * s) + 4;
        apL1.init(S(672), -0.70f, exN); apR1.init(S(908), -0.70f, exN);
        apL2.init(S(1800), 0.5f); apR2.init(S(2656), 0.5f);
        nL1 = S(4453); nL2 = S(3720); nR1 = S(4217); nR2 = S(3163);
        dL1.init(nL1); dL2.init(nL2); dR1.init(nR1); dR2.init(nR2);
        // Output taps (Dattorro table 2). Left reads mostly the right half and vice versa.
        const int L[7] = { 266, 2974, 1913, 1996, 1990, 187, 1066 };
        const int R[7] = { 353, 3627, 1228, 2673, 2111, 335, 121 };
        for (int i = 0; i < 7; ++i) { tl[i] = S(L[i]); tr[i] = S(R[i]); }
        ex = excursion * s;
        phaseStep = 2.0 * kPi * modHz / sr;
        setDecay(decay);
        reset();
    }
    void setDecay(float d) noexcept
    {
        decay = d;
        apL2.g = apR2.g = std::clamp(d + 0.15f, 0.25f, 0.50f);   // decay diffusion 2, as the paper
    }
    void setT30(double t30) noexcept { setDecay((float) std::exp(-std::exp(fromTable(kTable, kT30Lo, kT30Hi, t30)))); }
    void reset() noexcept
    {
        for (auto* r : { &pre, &dL1, &dL2, &dR1, &dR2 }) r->clear();
        for (auto* a : { &in1, &in2, &in3, &in4, &apL1, &apL2, &apR1, &apR2 }) a->d.clear();
        bw.y = dampL.y = dampR.y = 0.0f;
        phase = 0.0;
    }
    inline void process(float inL, float inR, float& oL, float& oR) noexcept
    {
        const float x = 0.5f * (inL + inR);
        const float p = pre.get(preLen); pre.push(x);
        const float d = in4.process(in3.process(in2.process(in1.process(bw.process(p)))));
        const float lEnd = dL2.get(nL2), rEnd = dR2.get(nR2);
        const double m1 = ex * std::sin(phase), m2 = ex * std::cos(phase);
        phase += phaseStep;
        if (phase > 2.0 * kPi) phase -= 2.0 * kPi;

        const float a = apL1.processMod(d + decay * rEnd, apL1.len + m1);
        const float b = dL1.get(nL1); dL1.push(a);
        dL2.push(apL2.process(dampL.process(b) * decay));

        const float a2 = apR1.processMod(d + decay * lEnd, apR1.len + m2);
        const float b2 = dR1.get(nR1); dR1.push(a2);
        dR2.push(apR2.process(dampR.process(b2) * decay));

        oL = 0.6f * (dR1.get(tl[0] + 1) + dR1.get(tl[1] + 1) - apR2.tap(tl[2]) + dR2.get(tl[3] + 1)
                     - dL1.get(tl[4] + 1) - apL2.tap(tl[5]) - dL2.get(tl[6] + 1));
        oR = 0.6f * (dL1.get(tr[0] + 1) + dL1.get(tr[1] + 1) - apL2.tap(tr[2]) + dL2.get(tr[3] + 1)
                     - dR1.get(tr[4] + 1) - apR2.tap(tr[5]) - dR2.get(tr[6] + 1));
    }
};

// ── HALL: an 8-line feedback delay network after Geraint Luff, "Let's Write a Reverb"
// (ADC 2021). Four Hadamard diffuser stages (shuffled, sign-flipped) feed eight slowly
// modulated lines mixed by a Householder matrix, each line damped and given the gain
// that decays it by 60 dB in the design RT60. Wide, smooth, the default.
struct Hall
{
    // Design RT60 against T30: ln(rt60) at T30 nodes 0.78 .. 5.5 s (9, geometric), at 48 and
    // at 96 kHz. The damping one-pole and the wider band above 20 kHz shorten the measured
    // tail at a higher rate (one 48 kHz table read 7..11 % short at 96 kHz), so the value is
    // interpolated between the two tables in octaves of sample rate (extrapolated a little
    // outside: 44.1 kHz, and up to 192 kHz).
    static constexpr double kT30Lo = 0.78, kT30Hi = 5.5;
    static constexpr std::array<double, 9> kTable { { -0.02388, 0.22011, 0.45316, 0.68458, 0.92871,
                                                      1.16719, 1.41106, 1.65245, 1.88991 } };
    static constexpr std::array<double, 9> kTable96 { { 0.07913, 0.31393, 0.54076, 0.77102, 1.00518,
                                                        1.23563, 1.46552, 1.70310, 1.93352 } };
    static constexpr int N = 8, kStages = 4;

    double sr = 48000.0, srOct = 0.0;   // octaves above 48 kHz (the tables' blend)
    double rt60 = 2.0;
    double preMs = 20.0, dampHz = 6500.0, modMs = 0.45, baseMs = 62.0, earlyGain = 0.35;
    static constexpr double kStageMs[kStages] = { 44.0, 22.0, 11.0, 5.5 };
    std::array<Ring, 2> pre; int preLen = 1;
    struct Stage { std::array<Ring, N> d; int len[N] {}; float sign[N] {}; int shuf[N] {}; };
    std::array<Stage, kStages> st;
    std::array<Ring, N> line;
    double len[N] {}, depth = 0.0;
    // Each line's sine LFO as a rotating phasor (c, s): one complex multiply a frame, not a sin().
    double lc[N] {}, ls[N] {}, rc[N] {}, rs[N] {}, ph0[N] {};
    float g[N] {};
    OnePoleLP lp[N];

    static inline void hadamard(float* v) noexcept
    {
        for (int h = 1; h < N; h *= 2)
            for (int i = 0; i < N; i += 2 * h)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = v[j], b = v[j + h];
                    v[j] = a + b; v[j + h] = a - b;
                }
        const float s = 0.35355339f;   // 1/sqrt(8)
        for (int i = 0; i < N; ++i) v[i] *= s;
    }

    void prepare(double rate)
    {
        sr = rate;
        srOct = std::clamp(std::log2(sr / 48000.0), -0.25, 2.0);
        std::mt19937 rng(20211117u);
        auto U = [&rng] { return (double) rng() / 4294967295.0; };
        preLen = std::max(1, (int) std::lround(preMs * 0.001 * sr));
        for (auto& p : pre) p.init(preLen + 2);
        for (int s = 0; s < kStages; ++s)
        {
            const double range = kStageMs[s] * 0.001 * sr;
            for (int i = 0; i < N; ++i)
            {
                // each channel's delay drawn from its own slice of the range (even spread)
                st[(size_t) s].len[i] = std::max(2, (int) (range * (i + U()) / N));
                st[(size_t) s].d[(size_t) i].init(st[(size_t) s].len[i] + 2);
                st[(size_t) s].sign[i] = U() < 0.5 ? -1.0f : 1.0f;
                st[(size_t) s].shuf[i] = i;
            }
            for (int i = N - 1; i > 0; --i) std::swap(st[(size_t) s].shuf[i], st[(size_t) s].shuf[(int) (U() * (i + 1)) % (i + 1)]);
        }
        depth = modMs * 0.001 * sr;
        for (int i = 0; i < N; ++i)
        {
            len[i] = baseMs * 0.001 * sr * std::pow(2.0, (double) i / N) * (0.97 + 0.06 * U());
            line[(size_t) i].init((int) (len[i] + depth) + 8);
            ph0[i] = 2.0 * kPi * U();
            const double w = 2.0 * kPi * (0.11 + 0.37 * U()) / sr;
            rc[i] = std::cos(w); rs[i] = std::sin(w);
            lp[i].set(dampHz, sr);
        }
        setRT60(rt60);
        reset();
    }
    void setRT60(double t) noexcept
    {
        rt60 = t;
        for (int i = 0; i < N; ++i)
            g[i] = (float) std::pow(10.0, -3.0 * (len[i] / sr) / t);
    }
    void setT30(double t30) noexcept
    {
        const double a = fromTable(kTable, kT30Lo, kT30Hi, t30), b = fromTable(kTable96, kT30Lo, kT30Hi, t30);
        setRT60(std::exp(a + srOct * (b - a)));
    }
    void reset() noexcept
    {
        for (auto& p : pre) p.clear();
        for (auto& s : st) for (auto& d : s.d) d.clear();
        for (int i = 0; i < N; ++i)
        {
            line[(size_t) i].clear();
            lp[i].y = 0.0f;
            lc[i] = std::sin(ph0[i]);      // the phasor's sine leg is the LFO
            ls[i] = std::cos(ph0[i]);
        }
    }
    inline void process(float inL, float inR, float& oL, float& oR) noexcept
    {
        const float pl = pre[0].get(preLen), pr = pre[1].get(preLen);
        pre[0].push(inL); pre[1].push(inR);
        float v[N];
        for (int i = 0; i < N; ++i) v[i] = (i & 1) ? pr : pl;
        for (auto& s : st)
        {
            float t[N];
            for (int i = 0; i < N; ++i) { t[i] = s.d[(size_t) i].get(s.len[i]); s.d[(size_t) i].push(v[i]); }
            for (int i = 0; i < N; ++i) v[i] = t[s.shuf[i]] * s.sign[i];
            hadamard(v);
        }
        float y[N], f[N], sum = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            y[i] = line[(size_t) i].getHermite(len[i] + depth * lc[i]);
            // rotate (sin, cos) by the line's rate; the norm is pulled back to 1 each frame
            const double c = lc[i] * rc[i] + ls[i] * rs[i], s = ls[i] * rc[i] - lc[i] * rs[i];
            const double k = 1.5 - 0.5 * (c * c + s * s);
            lc[i] = c * k; ls[i] = s * k;
            f[i] = lp[i].process(y[i]) * g[i];
            sum += f[i];
        }
        sum *= 2.0f / N;                                   // Householder: I - (2/N) 11^T
        for (int i = 0; i < N; ++i) line[(size_t) i].push(f[i] - sum + v[i]);
        float l = 0.0f, r = 0.0f, el = 0.0f, er = 0.0f;
        for (int i = 0; i < N; i += 2) { l += y[i]; r += y[i + 1]; el += v[i]; er += v[i + 1]; }
        oL = 0.5f * (l + (float) earlyGain * el);
        oR = 0.5f * (r + (float) earlyGain * er);
    }
};

// ── SPRING: a spring tank after Välimäki / Parker / Abel (JAES 2010). A cascade of
// stretched first-order allpasses H(z^K) gives the low-frequency dispersion chirp,
// inside a delay feedback loop low-passed at the chirp's transition frequency, its delay
// drifting a little (a random spline). Two tanks (37 / 44 ms) make left and right. The
// input is high-passed at 110 Hz, as a spring's transducer is. Boingy, lo-fi.
// Rate-independent: the design is the one at 44.1 kHz (stretch K = 5, so the chirp's
// transition is at 4410 Hz). At another rate K is the whole number that keeps the transition
// at or above 4410 Hz, each allpass's coefficient is set so its low-frequency delay (in
// seconds) is the 44.1 kHz one, and the loop's low-passes sit at the same Hz. So a print at
// the file's rate and the audition at the host's have the same spring.
struct Spring
{
    // Loop gain against T30: ln(-ln g) at T30 nodes 0.6 .. 3.0 s (9, geometric), measured at
    // 44.1 kHz (transition 4410 Hz) and at 48 kHz (K = 5 there, transition 4800 Hz). A whole
    // K moves the transition, which spreads the first chirp differently (one table read 11 %
    // short at 48 / 96 kHz on the shortest tails), so the value is blended by where the
    // realised transition sits between the two.
    static constexpr double kT30Lo = 0.6, kT30Hi = 3.0;
    static constexpr std::array<double, 9> kTable { { -0.60245, -0.86043, -1.08785, -1.34434, -1.55869,
                                                      -1.73647, -1.96189, -2.14809, -2.35740 } };
    static constexpr std::array<double, 9> kTable4800 { { -0.74632, -0.91592, -1.12218, -1.35134, -1.55859,
                                                          -1.73907, -1.95579, -2.14925, -2.35893 } };
    static constexpr int M = 120;                       // stretched allpasses per tank

    struct Tank
    {
        std::vector<float> w;                           // [M][K] allpass states, one shared index
        float a = 0.72f;
        Ring dl; double td = 0.0, depth = 0.0;
        Biquad lpLoop, lpOut;
        RandomSpline drift;
        uint32_t seed = 777u;
        float fb = 0.0f;
        void clear() noexcept
        {
            std::fill(w.begin(), w.end(), 0.0f);
            dl.clear();
            lpLoop.clear(); lpOut.clear();
            drift.restart(seed);
            fb = 0.0f;
        }
        inline float read() noexcept { return dl.getHermite(td + depth * drift.next()); }
        inline float close(float c, float x, float g) noexcept   // the loop's low-pass, feedback, output
        {
            dl.push(x + g * fb);
            c = lpLoop.process(c);
            fb = c;
            return lpOut.process(c);
        }
    };
    double sr = 48000.0;
    float g = 0.8f;                                    // loop gain
    static constexpr double kTransitionHz = 4410.0;    // the chirp's transition at 44.1 kHz (K = 5)
    int K = 1, idx = 0;                                // the allpasses' stretch, and their shared index
    double tableBlend = 0.0;                           // 0 = kTable (transition 4410 Hz) … 1 = kTable4800
    Biquad hp1, hp2;
    std::array<Tank, 2> t;

    void prepare(double rate)
    {
        sr = rate;
        K = std::max(1, (int) std::floor(sr / (2.0 * kTransitionHz) + 1.0e-9));
        tableBlend = std::clamp((sr / (2.0 * K) - kTransitionHz) / (4800.0 - kTransitionHz), 0.0, 1.0);
        const double tdMs[2] = { 37.0, 44.0 };
        const double aa[2]   = { 0.72, 0.75 };             // the 44.1 kHz design's coefficients
        for (int s = 0; s < 2; ++s)
        {
            auto& k = t[(size_t) s];
            k.w.assign((size_t) (M * K), 0.0f);
            // a section's delay at DC: (K / sr)·(1 − a)/(1 + a) s; held at the design's
            const double q = (1.0 - aa[s]) / (1.0 + aa[s]) * sr / (2.0 * kTransitionHz * K);
            k.a = (float) ((1.0 - q) / (1.0 + q));
            k.td = tdMs[s] * 0.001 * sr;
            k.depth = 0.00025 * sr;
            k.dl.init((int) (k.td + k.depth) + 8);
            k.lpLoop.rbj(kTransitionHz * 0.92, 0.707, sr, false);
            k.lpOut.rbj(kTransitionHz * 0.92, 0.707, sr, false);
            k.seed = 777u + (uint32_t) s;
            k.drift.setRate(3.0, sr);
        }
        hp1.rbj(110.0, 0.707, sr, true);
        hp2.rbj(110.0, 0.707, sr, true);
        reset();
    }
    void setT30(double t30) noexcept
    {
        const double a = fromTable(kTable, kT30Lo, kT30Hi, t30), b = fromTable(kTable4800, kT30Lo, kT30Hi, t30);
        g = (float) std::exp(-std::exp(a + tableBlend * (b - a)));
    }
    void reset() noexcept
    {
        hp1.clear(); hp2.clear();
        for (auto& k : t) k.clear();
        idx = 0;
    }
    inline void process(float inL, float inR, float& oL, float& oR) noexcept
    {
        const float x = hp2.process(hp1.process(0.5f * (inL + inR)));
        // Each tank: its delay's read through M allpasses H(z) = (a + z^-K) / (1 + a z^-K)
        // (direct form II), then the loop closes. The two tanks' cascades run side by side
        // in one loop: two independent chains, so the CPU overlaps them.
        float c0 = t[0].read(), c1 = t[1].read();
        const float a0 = t[0].a, a1 = t[1].a;
        float* s0 = t[0].w.data() + idx;
        float* s1 = t[1].w.data() + idx;
        for (int m = 0; m < M; ++m, s0 += K, s1 += K)
        {
            const float w0 = *s0, u0 = c0 - a0 * w0;
            const float w1 = *s1, u1 = c1 - a1 * w1;
            c0 = a0 * u0 + w0;  *s0 = u0;
            c1 = a1 * u1 + w1;  *s1 = u1;
        }
        if (++idx == K) idx = 0;
        oL = t[0].close(c0, x, g);
        oR = t[1].close(c1, x, g);
    }
};
} // namespace loopnest::verb
