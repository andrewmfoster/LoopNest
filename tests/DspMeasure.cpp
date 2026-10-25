// v2 audit: a measurement tool for the character chain and the A/B monitor pair.
// It prints numbers; it asserts nothing (LoopNestTests is the pass/fail suite).
//
//   chain  — CharacterChain driven directly: each rack knob's loudness change, peak,
//            DC and delay, DRIVE against source level, DRIVE aliasing and small-signal
//            response, the neutral null. `LoopNestMeasure drivecpu` times DRIVE alone;
//            `LoopNestMeasure drivefiles <files>` runs DRIVE's level and match on real loops.
//   match  — the real processor in AUDITION: what GAIN MATCH leaves at each DRIVE
//            amount, and how long it takes to settle after a knob move. Then the shared
//            stage alone (MonitorStage.h): its gain swing and settle time against its
//            window. `LoopNestMeasure stage` runs that alone.
//   fx     — the real processor in FX: what GAIN MATCH and BYPASS leave against the input.
//   parity — the wet print against the live AUDITION, and against FX fed the dry
//            print (the dry-hatch workflow); with it, DRIVE's level per pass from a
//            fresh chain, GAIN MATCH on the other knobs, and the wet print's loop seam.
//            `LoopNestMeasure parity` runs these alone.
//   effects — every effect against the claim its own comments make: WARP/FLUTTER pitch
//            and rate, DRIVE THD and TONE tilt, ECHO time/repeats/band, REVERB T30 and
//            band, WIDTH correlation and mono sum, the master EQ corners, zipper on fast
//            knob moves, sample-rate drift, the neutral nulls, PITCH and GLIDE through
//            the real processor, and a short row per effect on the files given.
//            `LoopNestMeasure effects [files]` runs this section alone.
//   typegains — `LoopNestMeasure typegains <files>`: REVERB TYPE's wet levels on real
//            loops against HALL's, the gain table that evens them, and the spread (D-M2).
//
// Loudness is K-weighted (ITU-R BS.1770 pre-filter) mean square, in dB against the
// dry signal. The source is a synthetic two-bar drum loop, or the files named on the
// command line. Runs headless under its own product name ("LoopNest Measure"), so the
// plugin's settings and Prints are never touched; its temp audio is deleted at exit.
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/CharacterChain.h"
#include "../Source/MonitorStage.h"
#include <iomanip>
#include <complex>
#include <functional>

namespace
{
    using CP = loopnest::CharacterParams;
    constexpr double kSR = 44100.0;

    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process(double x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    // BS.1770 K-weighting at any sample rate: high shelf (+4 dB, 1682 Hz) then high-pass (38 Hz).
    Biquad kShelf(double sr)
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(juce::MathConstants<double>::pi * f0 / sr);
        const double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        Biquad b;
        b.b0 = (Vh + Vb * K / Q + K * K) / a0;
        b.b1 = 2.0 * (K * K - Vh) / a0;
        b.b2 = (Vh - Vb * K / Q + K * K) / a0;
        b.a1 = 2.0 * (K * K - 1.0) / a0;
        b.a2 = (1.0 - K / Q + K * K) / a0;
        return b;
    }
    Biquad kHipass(double sr)
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(juce::MathConstants<double>::pi * f0 / sr);
        const double a0 = 1.0 + K / Q + K * K;
        Biquad b;
        b.b0 = 1.0; b.b1 = -2.0; b.b2 = 1.0;
        b.a1 = 2.0 * (K * K - 1.0) / a0;
        b.a2 = (1.0 - K / Q + K * K) / a0;
        return b;
    }

    // K-weighted loudness (dB) of buf[from, from+n), channels' mean squares averaged.
    double loudness(const juce::AudioBuffer<float>& buf, int from, int n, double sr = kSR)
    {
        double sum = 0.0;
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        {
            Biquad s = kShelf(sr), h = kHipass(sr);
            const float* d = buf.getReadPointer(ch);
            for (int i = 0; i < from; ++i) h.process(s.process(d[i]));   // settle the filters
            double ms = 0.0;
            for (int i = from; i < from + n; ++i)
            {
                const double v = h.process(s.process(d[i]));
                ms += v * v;
            }
            sum += ms / juce::jmax(1, n);
        }
        return 10.0 * std::log10(sum / juce::jmax(1, buf.getNumChannels()) + 1.0e-20);
    }
    double peakDb(const juce::AudioBuffer<float>& buf, int from, int n)
    {
        return juce::Decibels::gainToDecibels((double) buf.getMagnitude(from, n), -200.0);
    }
    double dcDb(const juce::AudioBuffer<float>& buf, int from, int n)
    {
        double worst = 0.0;
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        {
            double sum = 0.0;
            const float* d = buf.getReadPointer(ch);
            for (int i = from; i < from + n; ++i) sum += d[i];
            worst = juce::jmax(worst, std::abs(sum / juce::jmax(1, n)));
        }
        return juce::Decibels::gainToDecibels(worst, -200.0);
    }

    // Two bars at 120 bpm: kick on the beats, noise snare on 2 and 4, off-beat hats
    // panned a little apart (so WIDTH has a side signal). Peak-normalised to `peakDb`.
    juce::AudioBuffer<float> drumLoop(double peakDbfs, double sr = kSR)
    {
        const int n = (int) (sr * 4.0);
        juce::AudioBuffer<float> b(2, n);
        b.clear();
        juce::Random rng(7);
        auto add = [&] (int at, int len, float gl, float gr, auto&& fn)
        {
            for (int i = 0; i < len && at + i < n; ++i)
            {
                const float v = fn(i);
                b.addSample(0, at + i, v * gl);
                b.addSample(1, at + i, v * gr);
            }
        };
        const int beat = (int) (sr * 0.5);
        for (int k = 0; k < 8; ++k)
        {
            add(k * beat, (int) (sr * 0.25), 1.0f, 1.0f, [&] (int i)
            {
                const double t = (double) i / sr;
                return (float) (std::sin(juce::MathConstants<double>::twoPi * (50.0 * t + 6.0 * (1.0 - std::exp(-t * 30.0))))
                                * std::exp(-t * 14.0));
            });
            if (k % 2 == 1)
                add(k * beat, (int) (sr * 0.2), 0.7f, 0.7f, [&] (int i)
                { return (rng.nextFloat() * 2.0f - 1.0f) * (float) std::exp(-(double) i / (sr * 0.05)); });
            const bool left = (k % 2 == 0);
            add(k * beat + beat / 2, (int) (sr * 0.05), left ? 0.35f : 0.15f, left ? 0.15f : 0.35f, [&] (int i)
            { return (rng.nextFloat() * 2.0f - 1.0f) * (float) std::exp(-(double) i / (sr * 0.01)); });
        }
        b.applyGain(juce::Decibels::decibelsToGain((float) peakDbfs) / juce::jmax(1.0e-9f, b.getMagnitude(0, n)));
        return b;
    }

    CP neutral()
    {
        CP p;                         // every amount 0
        p.warpRate = 40; p.flutterRate = 50; p.echoHi = 1; p.reverbHi = 1; p.eqHi = 1;
        p.widthHi = 1; p.reverbDecay = 50; p.echoTime = 44; p.reverbType = 1;   // HALL, the default
        return p;
    }

    // `loops` passes of src through a fresh chain; returns the last pass (the steady
    // state a looping audition sits in), read the chain's fixed delay (latency()) later,
    // so it lines up with `src` frame for frame.
    juce::AudioBuffer<float> runChain(const juce::AudioBuffer<float>& src, const CP& p, int loops = 3,
                                      double sr = kSR)
    {
        CharacterChain c;
        c.prepare(sr);
        c.setParams(p);
        const int n = src.getNumSamples(), lat = c.latency();
        juce::AudioBuffer<float> out(2, n);
        for (long long k = 0; k < (long long) loops * n + lat; ++k)
        {
            const int i = (int) (k % n);
            float x[2] = { src.getSample(0, i), src.getSample(1, i) };
            c.setParams(p);       // as the processor does, once per block or more
            c.process(x, 2);
            const long long at = k - (long long) (loops - 1) * n - lat;
            if (at >= 0)
            {
                out.setSample(0, (int) at, x[0]);
                out.setSample(1, (int) at, x[1]);
            }
        }
        return out;
    }

    std::string fx(double v, int w = 7, int prec = 1)
    {
        std::ostringstream s;
        s << std::showpos << std::fixed << std::setprecision(prec) << std::setw(w) << v;
        return s.str();
    }

    void chainReport(const juce::AudioBuffer<float>& src, const juce::String& name)
    {
        const int n = src.getNumSamples();
        const double dry = loudness(src, 0, n);
        std::cout << "\n== chain: " << name << "  (peak " << fx(peakDb(src, 0, n)) << " dBFS, K-loudness "
                  << fx(dry) << " dB)\n";

        {
            const auto o = runChain(src, neutral(), 1);
            float diff = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i)
                    diff = juce::jmax(diff, std::abs(o.getSample(ch, i) - src.getSample(ch, i)));
            std::cout << "neutral chain, max |out - in| = " << diff << (diff == 0.0f ? "  (exact pass-through)" : "") << "\n";
        }

        std::cout << "knob        amount   loudness    peak      DC\n";
        struct Knob { const char* name; float CP::* m; };
        const Knob knobs[] = { { "WARP", &CP::warp }, { "FLUTTER", &CP::flutter }, { "DRIVE", &CP::drive },
                               { "ECHO", &CP::echo }, { "REVERB", &CP::reverb }, { "WIDTH", &CP::width } };
        for (const auto& k : knobs)
            for (float amt : { 25.0f, 50.0f, 75.0f, 100.0f })
            {
                CP p = neutral();
                p.*(k.m) = amt;
                const auto o = runChain(src, p);
                std::cout << std::left << std::setw(10) << k.name << std::right << std::setw(6) << (int) amt << " %  "
                          << fx(loudness(o, 0, n) - dry) << " dB  " << fx(peakDb(o, 0, n)) << "  " << fx(dcDb(o, 0, n)) << "\n";
            }
        for (float tone : { -100.0f, 100.0f })
        {
            CP p = neutral();
            p.drive = 50; p.driveTone = tone;
            const auto o = runChain(src, p);
            std::cout << "DRIVE 50 %, TONE " << fx(tone, 5, 0) << "  " << fx(loudness(o, 0, n) - dry) << " dB  "
                      << fx(peakDb(o, 0, n)) << "  " << fx(dcDb(o, 0, n)) << "\n";
        }
        for (float mid : { -100.0f, 100.0f })
        {
            CP p = neutral();
            p.widthMid = mid;
            const auto o = runChain(src, p);
            std::cout << "CENTER " << fx(mid, 5, 0) << "            " << fx(loudness(o, 0, n) - dry) << " dB  "
                      << fx(peakDb(o, 0, n)) << "\n";
        }
    }

    void driveVsLevel()
    {
        std::cout << "\n== chain: DRIVE loudness change against source level (synthetic loop)\n"
                  << "source peak    25 %     50 %     75 %    100 %\n";
        for (double pk : { -1.0, -6.0, -12.0, -18.0 })
        {
            const auto src = drumLoop(pk);
            const double dry = loudness(src, 0, src.getNumSamples());
            std::cout << fx(pk, 6, 0) << " dBFS ";
            for (float amt : { 25.0f, 50.0f, 75.0f, 100.0f })
            {
                CP p = neutral();
                p.drive = amt;
                const auto o = runChain(src, p);
                std::cout << " " << fx(loudness(o, 0, o.getNumSamples()) - dry) << " ";
            }
            std::cout << " dB\n";
        }
    }

    // DRIVE on a quiet sine (-40 dBFS, where the curve is all but linear): the stage's
    // gain at f in dB against its input and its phase delay in samples, from the
    // second pass of a 1 s sine (f on an exact bin).
    struct SineAt { double db = 0.0, delay = 0.0; };
    SineAt driveSineAt(float amt, double f, double sr = kSR)
    {
        const int n = (int) sr;
        const double amp = 0.01;
        juce::AudioBuffer<float> src(2, n);
        for (int i = 0; i < n; ++i)
        {
            const float v = (float) (amp * std::sin(juce::MathConstants<double>::twoPi * f * i / sr));
            src.setSample(0, i, v);
            src.setSample(1, i, v);
        }
        CP p = neutral();
        p.drive = amt;
        const auto o = runChain(src, p, 2, sr);
        const float* d = o.getReadPointer(0);
        double re = 0.0, im = 0.0;
        const double w = juce::MathConstants<double>::twoPi * f / sr;
        for (int i = 0; i < n; ++i) { re += d[i] * std::cos(w * i); im += d[i] * std::sin(w * i); }
        const double a = 2.0 * std::sqrt(re * re + im * im) / n;   // out = a·sin(w i − phi)
        double phi = std::atan2(-re, im);
        if (phi < -0.25 * juce::MathConstants<double>::pi) phi += juce::MathConstants<double>::twoPi;   // (-45°, 315°]: up to 20 kHz
        return { 20.0 * std::log10(juce::jmax(1.0e-20, a / amp)), phi / w };
    }

    // Wet-path delay of a stage that replaces the signal with a delayed read: the
    // index of the output's peak for a unit impulse, at a near-zero amount.
    void delayReport()
    {
        std::cout << "\n== chain: delay added to the signal (impulse in, ms to the output peak)\n";
        struct Case { const char* name; float warp, flutter, drive; };
        for (const Case c : { Case { "WARP on", 1.0f, 0.0f, 0.0f }, Case { "FLUTTER on", 0.0f, 1.0f, 0.0f },
                              Case { "WARP + FLUTTER on", 1.0f, 1.0f, 0.0f },
                              Case { "DRIVE 1 %", 0.0f, 0.0f, 1.0f }, Case { "DRIVE 100 %", 0.0f, 0.0f, 100.0f } })
        {
            juce::AudioBuffer<float> src(2, (int) kSR);
            src.clear();
            src.setSample(0, 100, 1.0f);
            src.setSample(1, 100, 1.0f);
            CP p = neutral();
            p.warp = c.warp; p.flutter = c.flutter; p.drive = c.drive;
            const auto o = runChain(src, p, 1);
            int at = 0; float best = 0.0f;
            for (int i = 0; i < o.getNumSamples(); ++i)
                if (std::abs(o.getSample(0, i)) > best) { best = std::abs(o.getSample(0, i)); at = i; }
            std::cout << std::left << std::setw(20) << c.name << std::right << fx((at - 100) * 1000.0 / kSR, 6, 2) << " ms\n";
        }
        // DRIVE's delay is under a sample, so the peak index above rounds it away: its
        // phase delay on a quiet sine (the stage near-linear) reads it in samples.
        std::cout << "DRIVE phase delay, -40 dBFS sine (samples):   100 Hz    1 kHz    5 kHz   10 kHz   15 kHz   20 kHz\n";
        for (float amt : { 1.0f, 100.0f })
        {
            std::cout << "  DRIVE " << std::setw(3) << (int) amt << " %                            ";
            for (double f : { 100.0, 1000.0, 5000.0, 10000.0, 15000.0, 20000.0 })
                std::cout << fx(driveSineAt(amt, f).delay, 7, 2) << "  ";
            std::cout << "\n";
        }
    }

    void driveResponseReport()
    {
        std::cout << "\n== chain: DRIVE small-signal response (-40 dBFS sines; dB against the input, and against 1 kHz)\n"
                  << "amount             1 kHz    5 kHz   10 kHz   15 kHz   20 kHz   | re 1 kHz: 5k     10k     15k     20k\n";
        for (double sr : { 44100.0, 48000.0 })
        for (float amt : { 1.0f, 25.0f, 100.0f })
        {
            double db[5];
            int k = 0;
            for (double f : { 1000.0, 5000.0, 10000.0, 15000.0, 20000.0 })
                db[k++] = driveSineAt(amt, f, sr).db;
            std::cout << std::fixed << std::setprecision(1) << sr / 1000.0 << "  DRIVE " << std::setw(3) << (int) amt << " % ";
            for (double v : db) std::cout << fx(v, 8, 2) << " ";
            std::cout << "  |        ";
            for (int i = 1; i < 5; ++i) std::cout << fx(db[i] - db[0], 6, 2) << "  ";
            std::cout << "\n";
        }
    }

    // DRIVE's cost: frames per second of the bare chain at a DRIVE amount, stereo,
    // over 20 s of the synthetic loop (median of 5), as ns per frame.
    void driveCpuReport()
    {
        const auto src = drumLoop(-6.0);
        const int n = src.getNumSamples();
        std::cout << "\n== chain: DRIVE cost, stereo frames through the bare chain (median of 5 runs of 20 s)\n";
        for (float amt : { 0.0f, 50.0f, 100.0f })
        {
            CP p = neutral();
            p.drive = amt;
            std::vector<double> t;
            float sink = 0.0f;
            for (int run = 0; run < 5; ++run)
            {
                CharacterChain c;
                c.prepare(kSR);
                c.setParams(p);
                const auto t0 = juce::Time::getHighResolutionTicks();
                for (int pass = 0; pass < 5; ++pass)
                    for (int i = 0; i < n; ++i)
                    {
                        float x[2] = { src.getSample(0, i), src.getSample(1, i) };
                        c.process(x, 2);
                        sink += x[0];
                    }
                t.push_back(juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0)
                            * 1.0e9 / (5.0 * n));
            }
            std::sort(t.begin(), t.end());
            std::cout << "DRIVE " << std::setw(3) << (int) amt << " %   " << std::fixed << std::setprecision(1) << t[2]
                      << " ns/frame  (min " << t[0] << ", max " << t[4] << ")" << (sink == 1.234f ? " " : "") << "\n";
        }
    }

    // DRIVE aliasing: a 5 kHz sine has harmonics at 0/5/10/15/20 kHz below Nyquist;
    // everything else in the output is folded-back distortion. One second at 44.1 kHz
    // puts every one of those on an exact DFT bin, so no window is needed.
    void aliasReport()
    {
        std::cout << "\n== chain: DRIVE aliasing (5 kHz sine, -6 dBFS, 44.1 kHz; energy off the harmonics, dB re the output)\n";
        const int n = (int) kSR;
        juce::AudioBuffer<float> src(2, n);
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.5f * (float) std::sin(juce::MathConstants<double>::twoPi * 5000.0 * i / kSR);
            src.setSample(0, i, v);
            src.setSample(1, i, v);
        }
        for (float amt : { 25.0f, 50.0f, 100.0f })
        {
            CP p = neutral();
            p.drive = amt;
            const auto o = runChain(src, p, 2);
            const float* d = o.getReadPointer(0);
            double total = 0.0, harm = 0.0;
            for (int i = 0; i < n; ++i) total += (double) d[i] * d[i];
            for (int h = 0; h <= 4; ++h)
            {
                double re = 0.0, im = 0.0;
                const double w = juce::MathConstants<double>::twoPi * 5000.0 * h / kSR;
                for (int i = 0; i < n; ++i) { re += d[i] * std::cos(w * i); im += d[i] * std::sin(w * i); }
                harm += (h == 0 ? 1.0 : 2.0) * (re * re + im * im) / n;
            }
            std::cout << "DRIVE " << std::setw(3) << (int) amt << " %  "
                      << fx(10.0 * std::log10(juce::jmax(1.0e-20, total - harm) / total)) << " dB\n";
        }
    }

    // DRIVE fold-back in the audible band: a sine of f Hz (whole Hz, so over 1 s at
    // the rate every harmonic and every fold of one sits on an exact bin) at -6 dBFS
    // through DRIVE; the power at each harmonic's fold that lands below 15 kHz (and on
    // no harmonic) is summed, dB re the whole output. 5 kHz at 44.1 kHz is a kind case
    // for the total above: elsewhere the strongest folds land at ~21-23 kHz instead.
    double driveFoldDb(double f, double sr, float amt)
    {
        const int n = (int) sr;
        juce::AudioBuffer<float> src(2, n);
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.5f * (float) std::sin(juce::MathConstants<double>::twoPi * f * i / sr);
            src.setSample(0, i, v);
            src.setSample(1, i, v);
        }
        CP p = neutral();
        p.drive = amt;
        const auto o = runChain(src, p, 2, sr);
        const float* d = o.getReadPointer(0);
        double total = 0.0;
        for (int i = 0; i < n; ++i) total += (double) d[i] * d[i];
        const long fi = (long) f, fs = (long) sr;
        std::vector<long> bins;
        for (long k = 2; k <= 400; ++k)
        {
            const long h = k * fi;
            if (2 * h < fs) continue;                                   // below Nyquist: a harmonic
            const long r = h % fs, fold = r > fs / 2 ? fs - r : r;
            if (fold >= 15000 || fold % fi == 0) continue;
            if (std::find(bins.begin(), bins.end(), fold) == bins.end()) bins.push_back(fold);
        }
        double e = 0.0;
        for (long b : bins)
        {
            double re = 0.0, im = 0.0;
            const double w = juce::MathConstants<double>::twoPi * (double) b / sr;
            for (int i = 0; i < n; ++i) { re += d[i] * std::cos(w * i); im += d[i] * std::sin(w * i); }
            e += 2.0 * (re * re + im * im) / n;
        }
        return 10.0 * std::log10(juce::jmax(1.0e-20, e) / total);
    }

    void driveFoldReport()
    {
        std::cout << "\n== chain: DRIVE fold-back below 15 kHz (sine at -6 dBFS; folded power under 15 kHz, dB re the output)\n"
                  << "rate      amount   2500 Hz  3500 Hz  4500 Hz  5500 Hz  7000 Hz  9000 Hz\n";
        for (double sr : { 44100.0, 48000.0 })
            for (float amt : { 50.0f, 100.0f })
            {
                std::cout << std::fixed << std::setprecision(1) << sr / 1000.0 << " kHz  " << std::setw(4) << (int) amt << " %  ";
                for (double f : { 2500.0, 3500.0, 4500.0, 5500.0, 7000.0, 9000.0 })
                    std::cout << fx(driveFoldDb(f, sr, amt), 8) << " ";
                std::cout << "\n";
            }
    }

    juce::File writeWav(const juce::File& f, const juce::AudioBuffer<float>& buf)
    {
        f.getParentDirectory().createDirectory();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream(f.createOutputStream());
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                      .withSampleRate(kSR)
                                                      .withNumChannels(2)
                                                      .withBitsPerSample(32)
                                                      .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
        return f;
    }

    void setNatural(LoopNestProcessor& p, const char* id, float v)
    {
        if (auto* q = p.apvts.getParameter(id))
            q->setValueNotifyingHost(q->convertTo0to1(v));
    }
    void setToggle(LoopNestProcessor& p, const char* id, bool on)
    {
        if (auto* q = p.apvts.getParameter(id))
            q->setValueNotifyingHost(on ? 1.0f : 0.0f);
    }

    // `seconds` of the processor's output, appended block by block. `input`, when
    // given, is looped into the buffer first (FX mode's track audio).
    juce::AudioBuffer<float> render(LoopNestProcessor& p, double seconds,
                                    const juce::AudioBuffer<float>* input = nullptr, int* inPos = nullptr)
    {
        const int blocks = (int) std::ceil(seconds * kSR / 512.0);
        juce::AudioBuffer<float> out(2, blocks * 512), buf(2, 512);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            if (input != nullptr)
                for (int i = 0; i < 512; ++i, ++*inPos)
                    for (int ch = 0; ch < 2; ++ch)
                        buf.setSample(ch, i, input->getSample(ch, *inPos % input->getNumSamples()));
            p.processBlock(buf, midi);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom(ch, b * 512, buf, ch, 0, 512);
        }
        return out;
    }

    // DRIVE on real loops (44.1 kHz files, read only): the chain's K-weighted loudness
    // change at each amount (driveVsLevel's measure), then the real processor in
    // AUDITION, GAIN MATCH off and on against BYPASS (matchReport's), over the last of
    // three passes of the whole file.
    void driveFilesReport(int argc, char** argv, int first)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::cout << "\n== DRIVE on real loops: chain loudness vs dry | AUDITION vs BYPASS, match off / on (dB)\n"
                  << "file                                      peak    25 %   50 %   75 %  100 %  |  25 %          50 %          75 %          100 %\n";
        for (int a = first; a < argc; ++a)
        {
            const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[a]));
            std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(f) };
            if (r == nullptr || r->sampleRate != kSR || r->lengthInSamples < 1000)
            {
                std::cout << f.getFileName() << ": skipped (unreadable or not 44.1 kHz)\n";
                continue;
            }
            juce::AudioBuffer<float> buf(2, (int) r->lengthInSamples);
            r->read(&buf, 0, (int) r->lengthInSamples, 0, true, true);
            if (r->numChannels == 1) buf.copyFrom(1, 0, buf, 0, 0, buf.getNumSamples());
            const int n = buf.getNumSamples();
            const double dry = loudness(buf, 0, n);
            std::cout << std::left << std::setw(40) << f.getFileNameWithoutExtension().substring(0, 39) << std::right
                      << fx(peakDb(buf, 0, n), 6) << " ";
            for (float amt : { 25.0f, 50.0f, 75.0f, 100.0f })
            {
                CP p = neutral();
                p.drive = amt;
                const auto o = runChain(buf, p, 2);
                std::cout << fx(loudness(o, 0, n) - dry, 6) << " ";
            }
            std::cout << " | ";
            for (float amt : { 25.0f, 50.0f, 75.0f, 100.0f })
            {
                double level[3] = {};
                for (int mode = 0; mode < 3; ++mode)        // 0 bypass, 1 match off, 2 match on
                {
                    LoopNestProcessor p;
                    p.setSyncEnabled(false);
                    p.prepareToPlay(kSR, 512);
                    loopnest::Snapshot s = p.capture();
                    s.file = f;
                    p.restore(s, loopnest::Snapshot::kFile);
                    p.finishPendingLoad(5000);
                    p.setSyncEnabled(false);
                    setNatural(p, "drive", amt);
                    setToggle(p, "bypass", mode == 0);
                    setToggle(p, "gainMatch", mode == 2);
                    p.setLooping(true);
                    p.setPlaying(true);
                    const auto o = render(p, 3.0 * n / kSR);
                    level[mode] = loudness(o, o.getNumSamples() - n, n);
                    p.releaseResources();
                }
                std::cout << fx(level[1] - level[0], 5) << " / " << fx(level[2] - level[0], 5) << "  ";
            }
            std::cout << "\n";
        }
    }

    void matchReport(const juce::File& root)
    {
        std::cout << "\n== match: AUDITION, real processor. Level against BYPASS (the dry loop), dB\n"
                  << "source peak  DRIVE   match off   match on (settled)\n";
        for (double pk : { -1.0, -12.0 })
        {
            const auto src = drumLoop(pk);
            const auto file = writeWav(root.getChildFile("loop" + juce::String((int) pk) + ".wav"), src);
            const int loopN = src.getNumSamples();
            for (float amt : { 25.0f, 50.0f, 75.0f, 100.0f })
            {
                double level[3] = {};
                for (int mode = 0; mode < 3; ++mode)        // 0 bypass, 1 match off, 2 match on
                {
                    LoopNestProcessor p;
                    p.setSyncEnabled(false);
                    p.prepareToPlay(kSR, 512);
                    loopnest::Snapshot s = p.capture();
                    s.file = file;
                    p.restore(s, loopnest::Snapshot::kFile);
                    p.finishPendingLoad(5000);
                    setNatural(p, "drive", amt);
                    setToggle(p, "bypass", mode == 0);
                    setToggle(p, "gainMatch", mode == 2);
                    p.setLooping(true);
                    p.setPlaying(true);
                    const auto o = render(p, 16.0);         // 4 passes of the 4 s loop
                    level[mode] = loudness(o, o.getNumSamples() - loopN, loopN);
                    p.releaseResources();
                }
                std::cout << fx(pk, 6, 0) << " dBFS  " << std::setw(4) << (int) amt << " %   "
                          << fx(level[1] - level[0]) << "     " << fx(level[2] - level[0]) << "\n";
            }
        }

        std::cout << "\n== match: settle time. DRIVE stepped 0 -> 100 % at t = 0 (source peak -6 dBFS)\n"
                  << "level against the dry loop, per half second:\n";
        const auto src = drumLoop(-6.0);
        const auto file = writeWav(root.getChildFile("loop-step.wav"), src);
        for (const bool match : { false, true })
        {
            LoopNestProcessor p;
            p.setSyncEnabled(false);
            p.prepareToPlay(kSR, 512);
            loopnest::Snapshot s = p.capture();
            s.file = file;
            p.restore(s, loopnest::Snapshot::kFile);
            p.finishPendingLoad(5000);
            setToggle(p, "gainMatch", match);
            p.setLooping(true);
            p.setPlaying(true);
            render(p, 8.0);                                 // two dry passes: trackers settled
            setNatural(p, "drive", 100.0f);
            const auto o = render(p, 8.0);
            // The dry loop's own level in each half-second window differs (kick vs
            // snare), so compare window for window against the dry source.
            const int win = (int) (kSR * 0.5);
            const int phase = (int) ((std::ceil(8.0 * kSR / 512.0) * 512)) % src.getNumSamples();
            juce::AudioBuffer<float> dry(2, o.getNumSamples());
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < o.getNumSamples(); ++i)
                    dry.setSample(ch, i, src.getSample(ch, (phase + i) % src.getNumSamples()));
            std::cout << (match ? "match on  " : "match off ");
            for (int w = 0; (w + 1) * win <= o.getNumSamples(); ++w)
                std::cout << fx(loudness(o, w * win, win) - loudness(dry, w * win, win), 6) << " ";
            std::cout << "\n";
            p.releaseResources();
        }
    }

    // The shared stage on its own (Source/MonitorStage.h), fed the dry loop and the
    // settled chain output: how far its gain wanders over a loop that is not changing
    // (a wandering gain is an audible pump), and how long it takes to come back within
    // 0.5 dB after the processed signal jumps 6 dB. The first trades against the second.
    void stageReport()
    {
        std::cout << "\n== match: the stage alone. Gain swing over a settled loop pass (dB, max minus min),\n"
                  << "   and the time back within 0.5 dB after the processed signal jumps +6 dB\n"
                  << "window  smooth    DRIVE 100   ECHO 100   REVERB 100   full rack 60    settle\n";
        const auto src = drumLoop(-6.0);
        const int n = src.getNumSamples();
        struct Sound { const char* id; float v; };
        std::vector<juce::AudioBuffer<float>> procs;
        for (int k = 0; k < 4; ++k)
        {
            CP p = neutral();
            if (k == 0) p.drive = 100;
            if (k == 1) p.echo = 100;
            if (k == 2) p.reverb = 100;
            if (k == 3) { p.warp = p.flutter = p.drive = p.echo = p.reverb = p.width = 60; }
            procs.push_back(runChain(src, p, 4));
        }
        struct Times { double window, smooth; };
        // The first row is what the plugin runs (MonitorStage's defaults).
        for (const Times t : { Times { 1.0, 0.2 }, Times { 0.6, 0.1 }, Times { 0.4, 0.1 }, Times { 0.25, 0.05 } })
        {
            std::cout << std::fixed << std::setprecision(2) << std::setw(5) << t.window << " s" << std::setw(6) << t.smooth << " s ";
            double settleSec = 0.0;
            for (size_t k = 0; k < procs.size(); ++k)
            {
                loopnest::MonitorStage st;
                st.setTimes(t.window, t.smooth);
                st.prepare(kSR);
                double lo = 1.0e9, hi = -1.0e9;
                auto pass = [&] (float scale, bool track, std::vector<float>* trace)
                {
                    for (int i = 0; i < n; ++i)
                    {
                        const float r[2] = { src.getSample(0, i), src.getSample(1, i) };
                        float x[2] = { procs[k].getSample(0, i) * scale, procs[k].getSample(1, i) * scale };
                        st.process(r, x, 2, true, false);
                        const double g = juce::Decibels::gainToDecibels((double) st.gain(), -200.0);
                        if (track) { lo = juce::jmin(lo, g); hi = juce::jmax(hi, g); }
                        if (trace != nullptr) trace->push_back((float) g);
                    }
                };
                for (int i = 0; i < 4; ++i) pass(1.0f, false, nullptr);
                pass(1.0f, true, nullptr);
                std::cout << std::setw(11) << hi - lo << (k == 3 ? "     " : "");
                if (k == 0)
                {
                    std::vector<float> trace;
                    for (int i = 0; i < 4; ++i) pass(2.0f, false, &trace);
                    // Settled = the last pass's own range; the last sample outside it, widened 0.5 dB.
                    float tlo = 1.0e9f, thi = -1.0e9f;
                    for (size_t i = trace.size() - (size_t) n; i < trace.size(); ++i)
                    { tlo = juce::jmin(tlo, trace[i]); thi = juce::jmax(thi, trace[i]); }
                    size_t lastOut = 0;
                    for (size_t i = 0; i < trace.size(); ++i)
                        if (trace[i] > thi + 0.5f || trace[i] < tlo - 0.5f) lastOut = i;
                    settleSec = (double) lastOut / kSR;
                }
            }
            std::cout << std::setw(8) << settleSec << " s\n";
        }
    }

    void fxReport()
    {
        std::cout << "\n== fx: FX mode, real processor, DRIVE 100 % on the synthetic loop (peak -6 dBFS)\n";
        const auto src = drumLoop(-6.0);
        juce::AudioBuffer<float> outs[3];
        const char* const names[] = { "pills off", "GAIN MATCH on", "BYPASS on" };
        for (int mode = 0; mode < 3; ++mode)
        {
            LoopNestProcessor p;
            p.setFxMode(true);
            p.prepareToPlay(kSR, 512);
            setNatural(p, "drive", 100.0f);
            setToggle(p, "gainMatch", mode == 1);
            setToggle(p, "bypass", mode == 2);
            int pos = 0;
            outs[mode] = render(p, 12.0, &src, &pos);
            p.releaseResources();
        }
        const int n = src.getNumSamples(), from = outs[0].getNumSamples() - n;
        const double dry = loudness(src, 0, n);
        for (int mode = 0; mode < 3; ++mode)
        {
            float diff = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < outs[0].getNumSamples(); ++i)
                    diff = juce::jmax(diff, std::abs(outs[mode].getSample(ch, i) - outs[0].getSample(ch, i)));
            std::cout << std::left << std::setw(15) << names[mode] << std::right << " level against the input "
                      << fx(loudness(outs[mode], from, n) - dry) << " dB";
            if (mode > 0)
                std::cout << ",  max |out - pills off| = " << diff << (diff == 0.0f ? "  (no effect)" : "");
            std::cout << "\n";
        }
    }

    // ── parity: what PRINT writes against what AUDITION and FX play ──────────────
    struct Wav { juce::AudioBuffer<float> buf; double sr = 0.0; int bits = 0; bool isFloat = false; };
    Wav readWav(const juce::File& f)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        Wav w;
        std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(f) };
        if (r == nullptr) return w;
        w.sr = r->sampleRate; w.bits = (int) r->bitsPerSample; w.isFloat = r->usesFloatingPointData;
        w.buf.setSize(2, (int) r->lengthInSamples);
        r->read(&w.buf, 0, (int) r->lengthInSamples, 0, true, true);
        return w;
    }

    // `numSamples` (rounded up to whole 512 blocks) of the processor's output.
    juce::AudioBuffer<float> renderN(LoopNestProcessor& p, int numSamples,
                                     const juce::AudioBuffer<float>* input = nullptr)
    {
        const int blocks = (numSamples + 511) / 512;
        juce::AudioBuffer<float> out(2, blocks * 512), buf(2, 512);
        juce::MidiBuffer midi;
        int inPos = 0;
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            if (input != nullptr)
                for (int i = 0; i < 512; ++i, ++inPos)
                    for (int ch = 0; ch < 2; ++ch)
                        buf.setSample(ch, i, input->getSample(ch, inPos % input->getNumSamples()));
            p.processBlock(buf, midi);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom(ch, b * 512, buf, ch, 0, 512);
        }
        return out;
    }

    // Two periods of `ref` starting `shift` frames in, so a loudness reading of the
    // second one has settled filters, like a reading inside a long live render.
    juce::AudioBuffer<float> tiled(const juce::AudioBuffer<float>& ref, int shift)
    {
        const int n = ref.getNumSamples();
        juce::AudioBuffer<float> t(2, 2 * n);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 2 * n; ++i)
                t.setSample(ch, i, ref.getSample(ch, (i + shift) % n));
        return t;
    }

    // Where in the print a live window starts, when the two are not sample-locked
    // (the SYNC stretcher's latency): coarse on 64-frame energy envelopes, then exact.
    int findShift(const juce::AudioBuffer<float>& a, int aFrom, const juce::AudioBuffer<float>& ref)
    {
        const int n = ref.getNumSamples(), hop = 64, bins = n / hop;
        auto mono = [] (const juce::AudioBuffer<float>& b, int i) { return 0.5 * (b.getSample(0, i) + b.getSample(1, i)); };
        std::vector<double> ea((size_t) bins, 0.0), er((size_t) bins, 0.0);
        for (int j = 0; j < bins; ++j)
            for (int i = 0; i < hop; ++i)
            {
                const double x = mono(a, aFrom + j * hop + i), y = mono(ref, j * hop + i);
                ea[(size_t) j] += x * x; er[(size_t) j] += y * y;
            }
        int coarse = 0; double best = -1.0;
        for (int s = 0; s < bins; ++s)
        {
            double c = 0.0;
            for (int j = 0; j < bins; ++j) c += ea[(size_t) j] * er[(size_t) ((j + s) % bins)];
            if (c > best) { best = c; coarse = s; }
        }
        const int seg = juce::jmin(n, 65536);
        int fine = coarse * hop; best = -1.0e300;
        for (int s = coarse * hop - 2 * hop; s <= coarse * hop + 2 * hop; ++s)
        {
            double c = 0.0;
            for (int i = 0; i < seg; ++i) c += mono(a, aFrom + i) * mono(ref, ((i + s) % n + n) % n);
            if (c > best) { best = c; fine = s; }
        }
        return (fine % n + n) % n;
    }

    // a[aFrom, aFrom + n) against ref (one period, read from `shift`): the null
    // residual in dB re ref, and the loudness and peak differences.
    std::string versus(const juce::AudioBuffer<float>& a, int aFrom, const juce::AudioBuffer<float>& ref,
                       double sr, int shift = 0, bool nullable = true)
    {
        const int n = ref.getNumSamples();
        const auto t = tiled(ref, shift);
        std::ostringstream s;
        if (nullable)
        {
            double e = 0.0, r = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i)
                {
                    const double y = t.getSample(ch, i), d = (double) a.getSample(ch, aFrom + i) - y;
                    e += d * d; r += y * y;
                }
            if (e == 0.0) s << "null   exact  ";
            else          s << "null " << fx(10.0 * std::log10(e / juce::jmax(1.0e-30, r)), 7) << " dB";
        }
        else
            s << "null     n/a   ";
        s << "   loudness " << fx(loudness(a, aFrom, n, sr) - loudness(t, n, n, sr), 6, 2) << " dB"
          << "   peak " << fx(peakDb(a, aFrom, n) - peakDb(ref, 0, n), 6, 2) << " dB";
        return s.str();
    }

    struct ParityCase
    {
        const char* name;
        std::vector<std::pair<const char*, float>> params;   // natural values
        std::vector<const char*> off = {};                   // panel power switched off
        double srcPeak = -6.0, hostSR = kSR, freeBpm = 0.0;
        bool   gainMatch = false;
    };

    void parityCase(const juce::File& root, const ParityCase& c, int index)
    {
        const auto src  = drumLoop(c.srcPeak);
        const auto file = writeWav(root.getChildFile("parity" + juce::String(index) + ".wav"), src);
        auto setUp = [&] (LoopNestProcessor& p, bool fxMode)
        {
            for (const auto& [id, v] : c.params) setNatural(p, id, v);
            for (const char* id : c.off) setToggle(p, id, false);
            setToggle(p, "gainMatch", c.gainMatch);
            p.setFxMode(fxMode);
            p.setSyncEnabled(false);
            p.prepareToPlay(c.hostSR, 512);      // after the params: gains and chain start settled
        };

        LoopNestProcessor a;
        setUp(a, false);
        loopnest::Snapshot s = a.capture();
        s.file = file;
        a.restore(s, loopnest::Snapshot::kFile);
        a.finishPendingLoad(5000);
        a.setSyncEnabled(false);
        if (c.freeBpm > 0.0) a.setFreeBpm(c.freeBpm);

        a.setDryHatch(false);
        const Wav wet = readWav(a.renderLoop());
        a.setDryHatch(true);
        const Wav dry = readWav(a.renderLoop());
        const int P = wet.buf.getNumSamples();
        std::cout << "\n[" << index << "] " << c.name << "\n";
        if (P <= 0 || dry.buf.getNumSamples() != P)
        {
            std::cout << "    print failed (wet " << P << " frames, dry " << dry.buf.getNumSamples() << ")\n";
            return;
        }
        const int prime = a.printPrimePasses((double) P / wet.sr);
        const int lat   = a.getLatencySamples();   // the audition and FX are heard this much later
        std::cout << "    wet print: " << P << " frames at " << wet.sr << " Hz, " << wet.bits << "-bit "
                  << (wet.isFloat ? "float" : "integer") << ", peak " << fx(peakDb(wet.buf, 0, P), 6, 2) << " dBFS, "
                  << prime << " priming pass" << (prime > 1 ? "es" : "") << (a.isStretching() ? ", stretched" : "") << "\n";

        const bool   sameRate = (c.hostSR == wet.sr);
        const bool   locked   = sameRate && ! a.isStretching();   // live output is sample-locked to the print
        const int    Ph       = sameRate ? P : (int) std::lround((double) P * c.hostSR / wet.sr);
        const int    total    = juce::jmax(6 * Ph, (int) (16.0 * c.hostSR), (prime + 2) * Ph);
        const int    last     = total / Ph - 1;

        a.setLooping(true);
        a.setPlaying(true);
        const auto heard = renderN(a, total + lat);
        a.releaseResources();
        juce::AudioBuffer<float> aud(2, total);                // as the host's delay compensation plays it
        for (int ch = 0; ch < 2; ++ch) aud.copyFrom(ch, 0, heard, ch, lat, total);

        if (! sameRate)
        {
            // Different rates: no null. Loudness of a settled live pass against the print's.
            const auto t = tiled(wet.buf, 0);
            std::cout << "    audition at " << c.hostSR << " Hz, settled      vs print:  null     n/a      loudness "
                      << fx(loudness(aud, last * Ph, Ph, c.hostSR) - loudness(t, P, P, wet.sr), 6, 2) << " dB   peak "
                      << fx(peakDb(aud, last * Ph, Ph) - peakDb(wet.buf, 0, P), 6, 2) << " dB\n";
            return;
        }

        if (locked)
        {
            std::cout << "    audition, pass " << prime + 1 << " (the print's age)   vs print:  " << versus(aud, prime * P, wet.buf, wet.sr) << "\n";
            // The print's first 5 ms are faded over from one more wrap (the F11 seam, PluginProcessor
            // renderLoop): a modulated tail (WARP, FLUTTER, every REVERB tank) differs there by design.
            const int head = juce::roundToInt(0.005 * wet.sr);
            double e = 0.0, r = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = head; i < P; ++i)
                {
                    const double y = wet.buf.getSample(ch, i), d = (double) aud.getSample(ch, prime * P + i) - y;
                    e += d * d; r += y * y;
                }
            std::cout << "      the same past the print's 5 ms head:  null "
                      << (e == 0.0 ? std::string("  exact") : fx(10.0 * std::log10(e / juce::jmax(1.0e-30, r)), 7)) << " dB\n";
        }
        const int shift = locked ? 0 : findShift(aud, last * P, wet.buf);
        std::cout << "    audition, pass " << last + 1 << " (settled)" << (last + 1 < 10 ? " " : "") << "          vs print:  "
                  << versus(aud, last * P, wet.buf, wet.sr, shift) << (locked ? "" : "   (best lag)") << "\n";

        LoopNestProcessor f;
        setUp(f, true);
        const auto fxHeard = renderN(f, total + lat, &dry.buf);
        f.releaseResources();
        juce::AudioBuffer<float> fxOut(2, total);
        for (int ch = 0; ch < 2; ++ch) fxOut.copyFrom(ch, 0, fxHeard, ch, lat, total);
        std::cout << "    FX on the dry print, settled         vs print:  " << versus(fxOut, last * P, wet.buf, wet.sr) << "\n";

        juce::AudioBuffer<float> audLast(2, P);
        for (int ch = 0; ch < 2; ++ch) audLast.copyFrom(ch, 0, aud, ch, last * P, P);
        std::cout << "    FX on the dry print, settled      vs audition:  "
                  << versus(fxOut, last * P, audLast, wet.sr, (P - shift) % P) << "\n";
    }

    // DRIVE's loudness compensation works from a picture of the source that a fresh
    // chain (every print) has to learn, and a replayed one (reset()) keeps.
    void driveSettleReport()
    {
        std::cout << "\n== chain: DRIVE 100 % from a fresh chain, loudness against dry on each pass of the 4 s loop\n"
                  << "source peak   pass 1   pass 2   pass 3   pass 4   pass 6   pass 8   replay (reset, pass 1)\n";
        for (double pk : { -1.0, -6.0, -12.0, -18.0 })
        {
            const auto src = drumLoop(pk);
            const int n = src.getNumSamples();
            const double dry = loudness(src, 0, n);
            CharacterChain c;
            c.prepare(kSR);
            CP p = neutral();
            p.drive = 100.0f;
            c.setParams(p);
            juce::AudioBuffer<float> out(2, n);
            auto pass = [&]
            {
                for (int i = 0; i < n; ++i)
                {
                    float x[2] = { src.getSample(0, i), src.getSample(1, i) };
                    c.setParams(p);
                    c.process(x, 2);
                    out.setSample(0, i, x[0]);
                    out.setSample(1, i, x[1]);
                }
                return loudness(out, 0, n) - dry;
            };
            std::cout << fx(pk, 6, 0) << " dBFS ";
            for (int k = 1; k <= 8; ++k)
            {
                const double v = pass();
                if (k <= 4 || k == 6 || k == 8) std::cout << " " << fx(v, 7, 2) << " ";
            }
            c.reset();
            std::cout << "   " << fx(pass(), 7, 2) << " dB\n";
        }
    }

    // Does a wet print loop without a step? A sine that closes on itself is printed
    // through each stage; the step across the file's seam (last frame to first) is set
    // against the largest step between neighbours inside the file.
    void seamReport(const juce::File& root)
    {
        std::cout << "\n== print seam: 441 Hz sine loop (closes on itself), step from the print's last frame to its first\n"
                  << "stage             seam step   largest step inside   ratio\n";
        const int n = (int) (kSR * 4.0);
        juce::AudioBuffer<float> src(2, n);
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.5f * (float) std::sin(juce::MathConstants<double>::twoPi * 441.0 * i / kSR);
            src.setSample(0, i, v);
            src.setSample(1, i, v);
        }
        const auto file = writeWav(root.getChildFile("seam-sine.wav"), src);
        struct Case { const char* name; const char* id; float v; };
        for (const Case c : { Case { "dry print", nullptr, 0.0f }, Case { "WARP 50", "warp", 50.0f }, Case { "WARP 100", "warp", 100.0f },
                              Case { "FLUTTER 50", "flutter", 50.0f }, Case { "FLUTTER 100", "flutter", 100.0f },
                              Case { "DRIVE 100", "drive", 100.0f }, Case { "ECHO 60", "echo", 60.0f },
                              Case { "REVERB 60", "reverb", 60.0f }, Case { "WIDTH 100", "width", 100.0f } })
        {
            LoopNestProcessor p;
            if (c.id != nullptr) setNatural(p, c.id, c.v);
            p.setSyncEnabled(false);
            p.prepareToPlay(kSR, 512);
            loopnest::Snapshot s = p.capture();
            s.file = file;
            p.restore(s, loopnest::Snapshot::kFile);
            p.finishPendingLoad(5000);
            p.setSyncEnabled(false);
            p.setDryHatch(c.id == nullptr);
            const Wav w = readWav(p.renderLoop());
            p.releaseResources();
            const int P = w.buf.getNumSamples();
            if (P < 2) { std::cout << c.name << ": print failed\n"; continue; }
            float seam = 0.0f, inside = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                seam = juce::jmax(seam, std::abs(w.buf.getSample(ch, 0) - w.buf.getSample(ch, P - 1)));
                for (int i = 1; i < P; ++i)
                    inside = juce::jmax(inside, std::abs(w.buf.getSample(ch, i) - w.buf.getSample(ch, i - 1)));
            }
            std::cout << std::left << std::setw(16) << c.name << std::right << std::fixed << std::setprecision(4)
                      << std::setw(10) << seam << std::setw(18) << inside << std::setw(13) << std::setprecision(1)
                      << seam / juce::jmax(1.0e-9f, inside) << "x\n";
        }
    }

    // The same seam on real loops (F11). A drum loop's transients dwarf any seam, so the
    // step is set against its own neighbourhood instead: the median step in the 10 ms on
    // each side of the seam, the print played as a loop (end, then start). The dry print
    // of the same file is the baseline, since a loop's own last frame need not meet its first.
    void seamFilesReport(const juce::File& root, int argc, char** argv, int firstFile)
    {
        std::cout << "\n== print seam on real loops: step across the seam against the median step within 10 ms of it\n";
        for (int a = firstFile; a < argc; ++a)
        {
            const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[a]));
            if (! f.existsAsFile()) { std::cout << f.getFileName() << ": missing\n"; continue; }
            std::cout << "\n" << f.getFileName() << "\nstage             seam step   median near   ratio   seam vs dry   largest near\n";
            float drySeam = 0.0f;
            struct Case { const char* name; const char* id; float v; };
            for (const Case c : { Case { "dry print", nullptr, 0.0f }, Case { "WARP 50", "warp", 50.0f }, Case { "WARP 100", "warp", 100.0f },
                                  Case { "FLUTTER 50", "flutter", 50.0f }, Case { "FLUTTER 100", "flutter", 100.0f } })
            {
                LoopNestProcessor p;
                if (c.id != nullptr) setNatural(p, c.id, c.v);
                p.setSyncEnabled(false);
                p.prepareToPlay(kSR, 512);
                loopnest::Snapshot s = p.capture();
                s.file = f;
                p.restore(s, loopnest::Snapshot::kFile);
                p.finishPendingLoad(5000);
                p.setSyncEnabled(false);
                p.setDryHatch(c.id == nullptr);
                const Wav w = readWav(p.renderLoop());
                p.releaseResources();
                const int P = w.buf.getNumSamples(), H = (int) (0.010 * w.sr);
                if (P < 4 * H) { std::cout << c.name << ": print failed\n"; continue; }
                float seam = 0.0f;
                std::vector<float> near;
                for (int ch = 0; ch < 2; ++ch)
                {
                    seam = juce::jmax(seam, std::abs(w.buf.getSample(ch, 0) - w.buf.getSample(ch, P - 1)));
                    for (int i = P - H; i < P - 1; ++i) near.push_back(std::abs(w.buf.getSample(ch, i + 1) - w.buf.getSample(ch, i)));
                    for (int i = 0; i < H; ++i)         near.push_back(std::abs(w.buf.getSample(ch, i + 1) - w.buf.getSample(ch, i)));
                }
                std::nth_element(near.begin(), near.begin() + (long) near.size() / 2, near.end());
                const float med = near[near.size() / 2];
                const float big = *std::max_element(near.begin(), near.end());
                if (c.id == nullptr) drySeam = seam;
                std::cout << std::left << std::setw(16) << c.name << std::right << std::fixed << std::setprecision(4)
                          << std::setw(10) << seam << std::setw(14) << med << std::setw(8) << std::setprecision(1)
                          << seam / juce::jmax(1.0e-9f, med) << "x" << std::setw(12)
                          << (c.id == nullptr ? std::string("-") : juce::String(seam / juce::jmax(1.0e-9f, drySeam), 1).toStdString() + "x")
                          << std::setw(15) << std::setprecision(4) << big << "\n";
            }
        }
    }

    // SYNC start (C9): the first SYNC block runs the stretcher's seek pre-roll on the
    // audio thread. Its wall time against the block's real-time budget, and against
    // the median of the blocks after it. Timing on this machine, Release build.
    void syncStartReport(const juce::File& root)
    {
        std::cout << "\n== SYNC start: first block (with the pre-roll) vs the block budget, 44.1 kHz\n"
                  << "block  tempo   first block   steady median   worst later   budget   first/budget\n";
        const auto file = writeWav(root.getChildFile("syncstart.wav"), drumLoop(-6.0));
        for (const int bs : { 32, 64, 128, 256, 512 })
            for (const double bpm : { 60.0, 120.0, 240.0 })
            {
                LoopNestProcessor p;
                p.setSyncEnabled(true);
                p.prepareToPlay(kSR, bs);
                loopnest::Snapshot s = p.capture();
                s.file = file;
                p.restore(s, loopnest::Snapshot::kFile);
                p.finishPendingLoad(5000);
                p.setFreeBpm(bpm);
                juce::AudioBuffer<float> buf(2, bs);
                juce::MidiBuffer midi;
                for (int w = 0; w < 50; ++w) { buf.clear(); p.processBlock(buf, midi); }   // warm, stopped
                p.setPlaying(true);
                std::vector<double> t;
                for (int b = 0; b < 400; ++b)
                {
                    buf.clear();
                    const auto t0 = juce::Time::getHighResolutionTicks();
                    p.processBlock(buf, midi);
                    t.push_back(juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0) * 1000.0);
                }
                p.releaseResources();
                const double first = t[0];
                std::vector<double> rest(t.begin() + 1, t.end());
                const double worst = *std::max_element(rest.begin(), rest.end());
                std::nth_element(rest.begin(), rest.begin() + (long) rest.size() / 2, rest.end());
                const double budget = 1000.0 * bs / kSR;
                std::cout << std::setw(5) << bs << std::setw(7) << (int) bpm << std::fixed << std::setprecision(3)
                          << std::setw(11) << first << " ms" << std::setw(13) << rest[rest.size() / 2] << " ms"
                          << std::setw(11) << worst << " ms" << std::setw(7) << std::setprecision(2) << budget << " ms"
                          << std::setw(10) << std::setprecision(0) << 100.0 * first / budget << " %\n";
            }
    }

    // A wet print of one hit and 7.9 s of silence through a long REVERB, ECHO and
    // WIDTH: the tails decay toward zero, where the print's filters and reverb would
    // run on denormals without the render's own ScopedNoDenormals. Median of 5.
    void printTimeReport(const juce::File& root)
    {
        juce::AudioBuffer<float> src(2, (int) (8.0 * kSR));
        src.clear();
        juce::Random r(7);
        for (int i = 0; i < (int) (0.1 * kSR); ++i)
            for (int ch = 0; ch < 2; ++ch)
                src.setSample(ch, i, 0.5f * (r.nextFloat() * 2.0f - 1.0f));
        const auto file = writeWav(root.getChildFile("printtime.wav"), src);
        LoopNestProcessor p;
        for (const auto& [id, v] : std::vector<std::pair<const char*, float>> {
                 { "reverb", 100.0f }, { "reverbDecay", 100.0f }, { "echo", 80.0f },
                 { "width", 70.0f }, { "widthLo", 0.2f }, { "widthHi", 0.8f } })
            setNatural(p, id, v);
        p.setSyncEnabled(false);
        p.prepareToPlay(kSR, 512);
        loopnest::Snapshot s = p.capture();
        s.file = file;
        p.restore(s, loopnest::Snapshot::kFile);
        p.finishPendingLoad(5000);
        p.setSyncEnabled(false);
        p.setDryHatch(false);
        std::vector<double> t;
        for (int k = 0; k < 5; ++k)
        {
            const auto t0 = juce::Time::getHighResolutionTicks();
            const auto f = p.renderLoop();
            t.push_back(juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0) * 1000.0);
            if (f == juce::File()) { std::cout << "\n== print time: the print failed\n"; return; }
        }
        std::sort(t.begin(), t.end());
        std::cout << "\n== print time: 8 s wet print, one hit then silence, REVERB 100 / DECAY 100, ECHO 80, WIDTH 70\n"
                  << "    median " << std::fixed << std::setprecision(1) << t[2] << " ms (min " << t[0]
                  << ", max " << t[4] << ")\n";
    }

    // GAIN MATCH on the knobs other than DRIVE: what it leaves against BYPASS, read
    // as stereo loudness (channel powers summed, as the plugin's own stage does).
    void matchOthersReport(const juce::File& root)
    {
        std::cout << "\n== match: AUDITION, other knobs at 100 % (source peak -6 dBFS). Level against BYPASS, dB\n"
                  << "knob       match off   match on (settled)\n";
        const auto src = drumLoop(-6.0);
        const auto file = writeWav(root.getChildFile("loop-others.wav"), src);
        const int loopN = src.getNumSamples();
        for (const char* id : { "echo", "reverb", "width", "warp", "flutter" })
        {
            double level[3] = {};
            for (int mode = 0; mode < 3; ++mode)            // 0 bypass, 1 match off, 2 match on
            {
                LoopNestProcessor p;
                p.setSyncEnabled(false);
                p.prepareToPlay(kSR, 512);
                loopnest::Snapshot s = p.capture();
                s.file = file;
                p.restore(s, loopnest::Snapshot::kFile);
                p.finishPendingLoad(5000);
                setNatural(p, id, 100.0f);
                setToggle(p, "bypass", mode == 0);
                setToggle(p, "gainMatch", mode == 2);
                p.setLooping(true);
                p.setPlaying(true);
                const auto o = render(p, 16.0);
                level[mode] = loudness(o, o.getNumSamples() - loopN, loopN);
                p.releaseResources();
            }
            std::cout << std::left << std::setw(10) << id << std::right << fx(level[1] - level[0], 8, 2) << "     "
                      << fx(level[2] - level[0], 8, 2) << "\n";
        }
    }

    void parityReport(const juce::File& root)
    {
        std::cout << "\n== parity: the wet print against the live AUDITION and against FX fed the dry print\n"
                  << "   null = residual re the print (lower is closer); loudness and peak = live minus print.\n"
                  << "   Source: the synthetic loop, peak -6 dBFS, host 44.1 kHz, unless a case says otherwise.\n";
        const std::vector<ParityCase> cases = {
            { "neutral chain", {} },
            { "DRIVE 100", { { "drive", 100.0f } } },
            { "ECHO 60 + REVERB 60", { { "echo", 60.0f }, { "reverb", 60.0f } } },
            { "REVERB 60, PLATE", { { "reverb", 60.0f }, { "reverbType", 0.0f } } },
            { "REVERB 60, HALL", { { "reverb", 60.0f }, { "reverbType", 1.0f } } },
            { "REVERB 60, SPRING", { { "reverb", 60.0f }, { "reverbType", 2.0f } } },
            { "WARP 100 + FLUTTER 100", { { "warp", 100.0f }, { "flutter", 100.0f } } },
            { "WARP 50 + FLUTTER 50", { { "warp", 50.0f }, { "flutter", 50.0f } } },
            { "full rack at 50, TONE +50, MIX 70, INPUT +3, OUTPUT -4, EQ low cut",
              { { "warp", 50.0f }, { "flutter", 50.0f }, { "drive", 50.0f }, { "echo", 50.0f }, { "reverb", 50.0f },
                { "width", 50.0f }, { "driveTone", 50.0f }, { "mix", 70.0f }, { "input", 3.0f }, { "output", -4.0f },
                { "eqLo", 0.2f } } },
            { "DRIVE 100 with its panel off, REVERB 50", { { "drive", 100.0f }, { "reverb", 50.0f } }, { "driveOn" } },
            { "PITCH -12, DRIVE 60 + ECHO 40", { { "pitch", -12.0f }, { "drive", 60.0f }, { "echo", 40.0f } } },
            { "trim to the first eighth, DRIVE 100 + REVERB 60", { { "end", 0.125f }, { "drive", 100.0f }, { "reverb", 60.0f } } },
            { "own tempo 100 bpm (stretcher), DRIVE 60 + ECHO 40", { { "drive", 60.0f }, { "echo", 40.0f } }, {}, -6.0, kSR, 100.0 },
            { "own tempo 100 bpm (stretcher), neutral chain", {}, {}, -6.0, kSR, 100.0 },
            { "host 48 kHz, DRIVE 100 + REVERB 50", { { "drive", 100.0f }, { "reverb", 50.0f } }, {}, -6.0, 48000.0 },
            { "host 48 kHz, neutral chain", {}, {}, -6.0, 48000.0 },
            { "hot: source -1 dBFS, INPUT +6, OUTPUT +6, ECHO 100, WIDTH 100",
              { { "input", 6.0f }, { "output", 6.0f }, { "echo", 100.0f }, { "width", 100.0f } }, {}, -1.0 },
            { "trim to the first eighth, DRIVE 100", { { "end", 0.125f }, { "drive", 100.0f } } },
            { "trim to the first eighth, REVERB 60", { { "end", 0.125f }, { "reverb", 60.0f } } },
            { "DRIVE 100, source -18 dBFS", { { "drive", 100.0f } }, {}, -18.0 },
            { "GAIN MATCH on, DRIVE 100, source -18 dBFS", { { "drive", 100.0f } }, {}, -18.0, kSR, 0.0, true },
            { "GAIN MATCH on, REVERB 100", { { "reverb", 100.0f } }, {}, -6.0, kSR, 0.0, true },
            { "GAIN MATCH on, ECHO 60 + REVERB 60, EQ low cut", { { "echo", 60.0f }, { "reverb", 60.0f }, { "eqLo", 0.4f } },
              {}, -6.0, kSR, 0.0, true },
            { "GAIN MATCH on, trim to the first eighth, DRIVE 100 + REVERB 60",
              { { "end", 0.125f }, { "drive", 100.0f }, { "reverb", 60.0f } }, {}, -6.0, kSR, 0.0, true },
        };
        int i = 0;
        for (const auto& c : cases)
            parityCase(root, c, ++i);
    }

    // ── effects: every effect against the claim its own comments make ────────────
    // Each table quotes the claim and where it is made (Source/CharacterChain.h, or
    // PluginProcessor.cpp for PITCH/GLIDE and the gain stages); the claimed constants
    // are restated below because the chain's own are private. The chain is driven
    // directly with its params pushed once per 512-frame block, as the processor
    // pushes them; PITCH, GLIDE and the gain stages run the real processor.
    constexpr double kTwoPi = juce::MathConstants<double>::twoPi;
    constexpr double kClaimWarpMinHz = 0.15, kClaimWarpMaxHz = 1.6, kClaimWarpDev = 0.035;          // CharacterChain kWarpRateMinHz, kWarpRateMaxHz, kWarpPitchDepth
    constexpr double kClaimWarpCapMs = 12.0;                                                        // kWarpSweepCapMs (old cap, new top)
    constexpr double kClaimWowHz = 0.7, kClaimFlutHz = 6.0, kClaimWowOldMs = 1.05, kClaimWowMs = 3.0, kClaimFlutToWow = 0.4; // kWowRateHz, kFlutRateHz, kWowDepthOldMs, kWowDepthMs, kFlutToWow
    constexpr double kClaimToneHz = 1800.0, kClaimToneDb = 4.5;                                     // kDriveToneHz, kDriveToneDb
    constexpr double kClaimT30[] = { 0.8, 1.3, 2.1, 3.4, 5.5 };     // PLATE / HALL, DECAY 0/25/50/75/100 %, ruled 10-08 (:318-326)
    constexpr double kClaimSpringT30Min = 0.6, kClaimSpringT30Max = 3.0;   // SPRING, DECAY 0 / 100 % (geometric), 10-17
    const char* const kTypeName[] = { "PLATE", "HALL", "SPRING" };

    // The knob taper (CharacterChain taper / taperRise), restated: the lower half is the old
    // knob (old top × 2u), the upper half rises to the new top.
    double claimTaper(double u, double oldTop, double newTop)
    {
        u = juce::jlimit(0.0, 1.0, u);
        if (u <= 0.5 || newTop - oldTop <= 1.0e-9) return oldTop * juce::jmin(1.0, 2.0 * u);
        const double t = 2.0 * u - 1.0, rho = oldTop / (newTop - oldTop);
        return oldTop + (newTop - oldTop) * (rho <= 2.0 ? rho * t + (1.0 - rho) * t * t : 1.0 - std::pow(1.0 - t, rho));
    }
    // WARP's pure-sine speed deviation at knob u (0..1), rate f: the sweep's 2 pi f D. The middle
    // is the old top (0.035 / (2 pi f), capped at 12 ms) soft-minimised (p 8) with 0.6 x the top.
    double claimWarpDev(double u, double f)
    {
        const double oldTop = std::min(kClaimWarpCapMs, 1000.0 * kClaimWarpDev / (juce::MathConstants<double>::twoPi * f));
        const double mid = std::pow(std::pow(oldTop, -8.0) + std::pow(0.6 * kClaimWarpCapMs, -8.0), -1.0 / 8.0);
        return juce::MathConstants<double>::twoPi * f * 0.001 * claimTaper(u, mid, kClaimWarpCapMs);
    }
    // FLUTTER's, both components at their peak together (an upper bound), rate multiplier mul.
    double claimFlutterDev(double u, double mul)
    {
        const double wowMs = claimTaper(u, kClaimWowOldMs, kClaimWowMs);
        return juce::MathConstants<double>::twoPi * 0.001 * wowMs * mul * (kClaimWowHz + kClaimFlutToWow * kClaimFlutHz);
    }
    double claimT30(int type, double decayPct)
    {
        const double d = decayPct * 0.01;
        return type == 2 ? kClaimSpringT30Min * std::pow(kClaimSpringT30Max / kClaimSpringT30Min, d)
                         : 0.78 * std::pow(5.5 / 0.78, d);
    }
    constexpr double kClaimEchoMinMs = 30.0, kClaimEchoMaxMs = 350.0, kClaimEchoFb = 0.55;          // kEchoTimeMinMs/MaxMs, kEchoFbMax
    constexpr double kClaimBandLoHz = 20.0, kClaimBandHiHz = 20000.0;                               // kBandFLo, kBandFHi
    constexpr double kClaimGlideMaxSec = 1.5;                                        // PluginProcessor.cpp kGlideMaxSec

    double claimBandHz(double p01) { return kClaimBandLoHz * std::pow(kClaimBandHiHz / kClaimBandLoHz, p01); }

    // fx() without the sign.
    std::string nx(double v, int w = 7, int prec = 1)
    {
        std::ostringstream s;
        s << std::fixed << std::setprecision(prec) << std::setw(w) << v;
        return s.str();
    }
    // A frequency, or "-" for none (an edge finder's -1).
    std::string hz(double v, int w = 8)
    {
        if (v < 0.0) return std::string((size_t) juce::jmax(0, w - 1), ' ') + "-";
        return nx(v, w, v < 100.0 ? 1 : 0);
    }

    using Sweep = std::function<CP(int)>;   // the params at a frame (read at each block start)

    juce::AudioBuffer<float> runBlocks(const juce::AudioBuffer<float>& src, const Sweep& at, double sr,
                                       bool eq = false, int block = 512)
    {
        CharacterChain c;
        c.prepare(sr);
        const int n = src.getNumSamples();
        juce::AudioBuffer<float> out(2, n);
        for (int i = 0; i < n; ++i)
        {
            if (i % block == 0) c.setParams(at(i));
            float x[2] = { src.getSample(0, i), src.getSample(1, i) };
            if (eq) c.processEq(x, 2);      // master EQ pre-rack, as the processor runs it
            c.process(x, 2);
            out.setSample(0, i, x[0]);
            out.setSample(1, i, x[1]);
        }
        return out;
    }
    juce::AudioBuffer<float> runConst(const juce::AudioBuffer<float>& src, const CP& p, double sr, bool eq = false)
    {
        return runBlocks(src, [p] (int) { return p; }, sr, eq);
    }

    // A sine in both channels, the right one `rightPhase` radians ahead.
    juce::AudioBuffer<float> sine(double f, double dbfs, double seconds, double sr, double rightPhase = 0.0)
    {
        const int n = (int) std::lround(seconds * sr);
        const double a = juce::Decibels::decibelsToGain(dbfs);
        juce::AudioBuffer<float> b(2, n);
        for (int i = 0; i < n; ++i)
        {
            const double w = kTwoPi * f * i / sr;
            b.setSample(0, i, (float) (a * std::sin(w)));
            b.setSample(1, i, (float) (a * std::sin(w + rightPhase)));
        }
        return b;
    }
    juce::AudioBuffer<float> impulse(double seconds, double sr, int at)
    {
        juce::AudioBuffer<float> b(2, (int) std::lround(seconds * sr));
        b.clear();
        b.setSample(0, at, 1.0f);
        b.setSample(1, at, 1.0f);
        return b;
    }

    // In-place radix-2 FFT (size a power of two).
    void fftInPlace(std::vector<std::complex<double>>& a)
    {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const std::complex<double> wl = std::polar(1.0, -kTwoPi / (double) len);
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<double> w(1.0, 0.0);
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const auto u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }
    // |X|² of d[0, n), zero-padded to nfft; bins 0..nfft/2.
    std::vector<double> powerSpectrum(const float* d, int n, int nfft)
    {
        std::vector<std::complex<double>> a((size_t) nfft);
        for (int i = 0; i < juce::jmin(n, nfft); ++i) a[(size_t) i] = d[i];
        fftInPlace(a);
        std::vector<double> p((size_t) nfft / 2 + 1);
        for (size_t k = 0; k < p.size(); ++k) p[k] = std::norm(a[k]);
        return p;
    }
    double bandPower(const std::vector<double>& p, int nfft, double sr, double loHz, double hiHz)
    {
        const double bin = sr / nfft;
        double s = 0.0;
        for (int k = juce::jmax(1, (int) std::ceil(loHz / bin)); k < (int) p.size() && k * bin < hiHz; ++k)
            s += p[(size_t) k];
        return s;
    }
    // num over den in dB, each summed over ±1/24 octave around f (one bin at least).
    double ratioDb(const std::vector<double>& num, const std::vector<double>& den, int nfft, double sr, double f)
    {
        const double bin = sr / nfft;
        const int last = (int) num.size() - 1;
        const int a = juce::jlimit(1, last, (int) std::floor(f * std::pow(2.0, -1.0 / 24.0) / bin));
        const int b = juce::jlimit(a, last, (int) std::ceil(f * std::pow(2.0, 1.0 / 24.0) / bin));
        double sn = 0.0, sd = 0.0;
        for (int k = a; k <= b; ++k) { sn += num[(size_t) k]; sd += den[(size_t) k]; }
        return 10.0 * std::log10(juce::jmax(1.0e-30, sn) / juce::jmax(1.0e-30, sd));
    }

    using Resp = std::function<double(double)>;   // a response, dB at a frequency

    // Where a response crosses -x dB on a 1/96-octave grid in [fLo, fHi], log-interpolated:
    // a low cut's edge scanning up, a high cut's scanning down. -1 = no crossing (open).
    double edgeHz(const Resp& r, double x, bool lowCut, double fLo, double fHi)
    {
        const double step = std::pow(2.0, 1.0 / 96.0);
        double f = lowCut ? fLo : fHi, prev = r(f);
        if (prev >= -x) return -1.0;
        for (;;)
        {
            const double g = lowCut ? f * step : f / step;
            if (lowCut ? g > fHi : g < fLo) return -1.0;
            const double v = r(g);
            if (v >= -x)
                return f * std::pow(g / f, (-x - prev) / (v - prev));
            f = g;
            prev = v;
        }
    }

    // One band-handle row: the handle's claimed frequency on the log axis, the measured
    // -3 and -6 dB edges, and the response at the claimed frequency.
    void bandRow(const char* name, double handle, bool lowCut, const Resp& r, double sr)
    {
        const double claim = claimBandHz(handle);
        std::cout << std::left << std::setw(9) << name << std::right << nx(handle, 6, 3) << hz(claim, 10)
                  << hz(edgeHz(r, 3.0, lowCut, 10.0, 0.45 * sr), 10) << hz(edgeHz(r, 6.0, lowCut, 10.0, 0.45 * sr), 10)
                  << "   " << fx(r(claim), 6, 1) << " dB\n";
    }
    const char* const kBandHeader = "handle     pos   claim Hz   -3 dB Hz   -6 dB Hz   at claim\n";

    // A steady tone's frequency, one reading per cycle: the time between positive-going
    // zero crossings (linearly interpolated), placed at the cycle's middle.
    struct Track { std::vector<double> t, f; };
    Track trackFreq(const float* d, int from, int n, double sr)
    {
        Track tr;
        double last = -1.0;
        for (int i = from + 1; i < from + n; ++i)
            if (d[i - 1] < 0.0f && d[i] >= 0.0f)
            {
                const double tc = ((double) (i - 1) - (double) d[i - 1] / ((double) d[i] - (double) d[i - 1])) / sr;
                if (last >= 0.0) { tr.t.push_back(0.5 * (tc + last)); tr.f.push_back(1.0 / (tc - last)); }
                last = tc;
            }
        return tr;
    }
    // Mean frequency over d[from, from + n): whole cycles over their total time.
    double meanFreq(const float* d, int from, int n, double sr)
    {
        const auto tr = trackFreq(d, from, n, sr);
        double total = 0.0;
        for (double f : tr.f) total += 1.0 / f;
        return total > 0.0 ? (double) tr.f.size() / total : 0.0;
    }
    void centsRange(const Track& tr, double f0, double fromSec, double& up, double& down)
    {
        up = -1.0e9; down = 1.0e9;
        for (size_t i = 0; i < tr.t.size(); ++i)
            if (tr.t[i] >= fromSec)
            {
                const double c = 1200.0 * std::log2(tr.f[i] / f0);
                up = juce::jmax(up, c);
                down = juce::jmin(down, c);
            }
    }

    // The strongest modulation in [loHz, hiHz] of a pitch track (cents re f0): the
    // track put on a 250 Hz grid from fromSec, Hann-windowed, scanned in 0.005 Hz steps.
    struct Mod { double hz = 0.0, cents = 0.0; };   // its rate and amplitude (± cents)
    Mod modPeak(const Track& tr, double f0, double fromSec, double loHz, double hiHz)
    {
        const double g = 250.0;
        std::vector<double> c;
        size_t j = 0;
        for (double t = fromSec; ; t += 1.0 / g)
        {
            while (j + 1 < tr.t.size() && tr.t[j + 1] < t) ++j;
            if (j + 1 >= tr.t.size()) break;
            const double u = (t - tr.t[j]) / (tr.t[j + 1] - tr.t[j]);
            c.push_back(1200.0 * std::log2((tr.f[j] + u * (tr.f[j + 1] - tr.f[j])) / f0));
        }
        if (c.size() < 16) return {};
        double mean = 0.0, sumW = 0.0;
        for (double v : c) mean += v;
        mean /= (double) c.size();
        std::vector<double> w(c.size());
        for (size_t k = 0; k < c.size(); ++k)
        {
            w[k] = (c[k] - mean) * (0.5 - 0.5 * std::cos(kTwoPi * (double) k / (double) (c.size() - 1)));
            sumW += 0.5 - 0.5 * std::cos(kTwoPi * (double) k / (double) (c.size() - 1));
        }
        Mod best;
        for (double f = loHz; f <= hiHz; f += 0.005)
        {
            const std::complex<double> step = std::polar(1.0, -kTwoPi * f / g);
            std::complex<double> ph(1.0, 0.0), acc(0.0, 0.0);
            for (double v : w) { acc += v * ph; ph *= step; }
            const double amp = 2.0 * std::abs(acc) / sumW;
            if (amp > best.cents) best = { f, amp };
        }
        return best;
    }

    // A 1 kHz sine (-6 dBFS, 21 s) through the chain: its pitch track, left channel.
    Track wobbleTrack(const CP& p, double sr)
    {
        const auto o = runConst(sine(1000.0, -6.0, 21.0, sr), p, sr);
        return trackFreq(o.getReadPointer(0), 0, o.getNumSamples(), sr);
    }
    double cents(double ratio) { return 1200.0 * std::log2(ratio); }

    // 0 -> 1 -> 0 over `dur` seconds from t0 (a triangle); 0 outside.
    double tri(double t, double t0, double dur)
    {
        const double u = (t - t0) / dur;
        return (u <= 0.0 || u >= 1.0) ? 0.0 : (u < 0.5 ? 2.0 * u : 2.0 - 2.0 * u);
    }

    // The delay WARP's read sits at, ms, over 20 s after a 1 s settle: a 20 Hz sine (period
    // 50 ms, longer than the whole sweep) through the chain; each positive-going zero
    // crossing of the output against the input's crossing ~16 ms before it.
    void warpDelayRange(const CP& p, double sr, double& lo, double& hi)
    {
        const auto o = runConst(sine(20.0, -6.0, 21.0, sr), p, sr);
        lo = 1.0e9; hi = -1.0e9;
        const float* d = o.getReadPointer(0);
        for (int i = (int) sr + 1; i < o.getNumSamples(); ++i)
            if (d[i - 1] < 0.0f && d[i] >= 0.0f)
            {
                const double tc = ((double) (i - 1) - (double) d[i - 1] / ((double) d[i] - (double) d[i - 1])) / sr;
                const double tin = std::round((tc - 0.016) * 20.0) / 20.0;   // the input's crossings are k / 20 s
                lo = juce::jmin(lo, 1000.0 * (tc - tin));
                hi = juce::jmax(hi, 1000.0 * (tc - tin));
            }
    }

    void warpFlutterReport()
    {
        std::cout << "\n== effects: WARP, pitch of a steady 1 kHz sine (20 s after a 1 s settle; rate = strongest\n"
                  << "   component of the pitch track, cents = its peak excursion)\n"
                  << "   claim (CharacterChain.h header, warpSweepFor, taper, process 0. WARP, kWarp*; parameter layout warp/warpRate):\n"
                  << "   slow, deep drift at 0.15..1.6 Hz (rate 0..100 %, linear), 35 % of it a smooth random drift. The\n"
                  << "   knob tapers to its middle at 50 %: the old 100 % (sweep 0.035 / (2 pi x rate), capped at 12 ms;\n"
                  << "   +59.6 / -61.7 cents) soft-minimised with 60 % of the top, so at fast rates it is the old 100 % and at\n"
                  << "   slow ones the top half still adds depth; at 100 % the 10-17 spike's 12 ms sweep at every rate (spike\n"
                  << "   C measured +88 / -100 at rate 40 %, +194 / -232 at rate 100 %, drums).\n"
                  << "   Claimed cents = the sweep as a pure sine (the drift moves the measured peak a few cents either way).\n"
                  << "rate  amount   Hz claim  measured    cents claim (up/down)   measured (up/down)    off claim (up/down)\n";
        for (float rate : { 0.0f, 10.0f, 22.0f, 40.0f, 100.0f })
            for (float amt : { 25.0f, 50.0f, 75.0f, 100.0f })
            {
                CP p = neutral();
                p.warp = amt; p.warpRate = rate;
                const auto tr = wobbleTrack(p, kSR);
                const Mod m = modPeak(tr, 1000.0, 1.0, 0.05, 5.0);
                double up, down;
                centsRange(tr, 1000.0, 1.0, up, down);
                const double f = kClaimWarpMinHz + (kClaimWarpMaxHz - kClaimWarpMinHz) * rate * 0.01;
                const double dev = claimWarpDev(amt * 0.01, f);
                const double cu = cents(1.0 + dev), cd = cents(1.0 - dev);
                std::cout << std::setw(3) << (int) rate << " %" << std::setw(5) << (int) amt << " %" << nx(f, 10, 3) << nx(m.hz, 10, 3)
                          << "    " << fx(cu, 7) << " / " << fx(cd, 7)
                          << "      " << fx(up, 7) << " / " << fx(down, 7)
                          << "      " << fx(100.0 * (up / cu - 1.0), 5, 0) << " % / " << fx(100.0 * (down / cd - 1.0), 4, 0) << " %\n";
            }

        std::cout << "\n   Delay: where WARP's read sits (20 Hz sine, see warpDelayRange), ms. claim (kWarpBaseMs, warpKnee; 10-08 ruling):\n"
                  << "   the base is 16 ms and the sweep stays inside it (never past 31 ms, never under 1 ms)\n"
                  << "rate  amount    min ms    max ms\n";
        for (float rate : { 0.0f, 40.0f, 100.0f })
            for (float amt : { 50.0f, 100.0f })
            {
                CP p = neutral();
                p.warp = amt; p.warpRate = rate;
                double lo, hi;
                warpDelayRange(p, kSR, lo, hi);
                std::cout << std::setw(3) << (int) rate << " %" << std::setw(5) << (int) amt << " %" << nx(lo, 10, 2) << nx(hi, 10, 2) << "\n";
            }

        std::cout << "\n   RATE and knob moves: the rate stepped (one block) after 4 s, or swept 0 -> 100 -> 0 in 50 ms, or the\n"
                  << "   knob stepped 0 -> 100 / 100 -> 50 while engaged; peak pitch over the 6 s after (a move re-centres the\n"
                  << "   sweep over ~1 s, CharacterChain.h warpSway, kWarpLeakSec)\n"
                  << "move                              cents up / down     (steady after: claim)\n";
        struct Move { const char* name; std::function<CP(double)> at; float endAmt, endRate; };
        auto at = [] (float amt, float rate) { CP p = neutral(); p.warp = amt; p.warpRate = rate; return p; };
        for (const Move& mv : { Move { "amount 100, rate 0 -> 100 (step)", [&] (double t) { return at(100, t < 4.0 ? 0.0f : 100.0f); }, 100, 100 },
                                Move { "amount 100, rate 100 -> 0 (step)", [&] (double t) { return at(100, t < 4.0 ? 100.0f : 0.0f); }, 100, 0 },
                                Move { "amount 100, rate 0 -> 100 -> 0, 50 ms", [&] (double t) { return at(100, (float) (100.0 * tri(t, 4.0, 0.05))); }, 100, 0 },
                                Move { "amount 50, rate 0 -> 100 (step)", [&] (double t) { return at(50, t < 4.0 ? 0.0f : 100.0f); }, 50, 100 },
                                Move { "rate 40, amount 50 -> 100 (step)", [&] (double t) { return at(t < 4.0 ? 50.0f : 100.0f, 40); }, 100, 40 },
                                Move { "rate 40, amount 100 -> 50 (step)", [&] (double t) { return at(t < 4.0 ? 100.0f : 50.0f, 40); }, 50, 40 } })
        {
            const auto o = runBlocks(sine(1000.0, -6.0, 10.0, kSR), [&] (int i) { return mv.at(i / kSR); }, kSR);
            double up, down;
            centsRange(trackFreq(o.getReadPointer(0), 0, o.getNumSamples(), kSR), 1000.0, 4.0, up, down);
            const double dev = claimWarpDev(mv.endAmt * 0.01, kClaimWarpMinHz + (kClaimWarpMaxHz - kClaimWarpMinHz) * mv.endRate * 0.01);
            std::cout << std::left << std::setw(34) << mv.name << std::right << fx(up, 8) << " / " << fx(down, 7)
                      << "     (" << fx(cents(1.0 + dev), 6) << " / " << fx(cents(1.0 - dev), 6) << ")\n";
        }

        std::cout << "\n== effects: FLUTTER, same sine. Two components: wow (strongest under 2.5 Hz) and flutter (above)\n"
                  << "   claim (CharacterChain.h header, flutterDepthFor, taper, process 1. FLUTTER, kWow*/kFlut*; parameter layout\n"
                  << "   flutterRate): subtle, fast tape shimmer; wow 0.7 Hz + flutter 6 Hz (0.4 x the wow's depth), 35 % of each a\n"
                  << "   smooth random drift; the wow's delay depth tapers 1.05 ms at 50 % (the old 100 %: +/-35 cents at rate 50 %)\n"
                  << "   to 3.0 ms at 100 % (the spike's: C measured +95 / -102 at rate 50 %); RATE 0/50/100 % = x0.5/x1/x2 speed.\n"
                  << "   Claimed cents = both at their peak together, pure sines (an upper bound).\n"
                  << "rate  amount   wow Hz claim/meas   flutter Hz claim/meas   (+/- cents each)   cents claim (up/down)   measured\n";
        for (float rate : { 0.0f, 50.0f, 100.0f })
            for (float amt : { 25.0f, 50.0f, 75.0f, 100.0f })
            {
                CP p = neutral();
                p.flutter = amt; p.flutterRate = rate;
                const auto tr = wobbleTrack(p, kSR);
                const Mod wow = modPeak(tr, 1000.0, 1.0, 0.05, 2.5), flut = modPeak(tr, 1000.0, 1.0, 2.5, 20.0);
                double up, down;
                centsRange(tr, 1000.0, 1.0, up, down);
                const double mul = std::pow(2.0, (rate - 50.0) / 50.0);
                const double dev = claimFlutterDev(amt * 0.01, mul);
                std::cout << std::setw(3) << (int) rate << " %" << std::setw(5) << (int) amt << " %"
                          << nx(kClaimWowHz * mul, 9, 2) << " /" << nx(wow.hz, 6, 2)
                          << nx(kClaimFlutHz * mul, 12, 2) << " /" << nx(flut.hz, 6, 2)
                          << "        " << nx(wow.cents, 5) << "  " << nx(flut.cents, 5)
                          << "      " << fx(cents(1.0 + dev), 7) << " / " << fx(cents(1.0 - dev), 7)
                          << "   " << fx(up, 7) << " / " << fx(down, 7) << "\n";
            }
    }

    // Power at f in d[from, from + n) (n a whole number of its cycles).
    double tonePower(const float* d, int from, int n, double f, double sr)
    {
        double re = 0.0, im = 0.0;
        const double w = kTwoPi * f / sr;
        for (int i = 0; i < n; ++i) { re += d[from + i] * std::cos(w * i); im += d[from + i] * std::sin(w * i); }
        return re * re + im * im;
    }
    // DRIVE on a -12 dBFS 100 Hz sine: THD (harmonics 2..20 over the fundamental) and
    // H2, H3 alone, dB, over the third second (100 whole cycles).
    struct Thd { double all = 0.0, h2 = 0.0, h3 = 0.0; };
    Thd driveThd(double sr, float amt)
    {
        CP p = neutral();
        p.drive = amt;
        const auto o = runConst(sine(100.0, -12.0, 3.0, sr), p, sr);
        const int from = (int) (2.0 * sr), n = (int) sr;
        const float* d = o.getReadPointer(0);
        const double h1 = tonePower(d, from, n, 100.0, sr);
        double rest = 0.0, h[4] = {};
        for (int k = 2; k <= 20; ++k)
        {
            const double v = tonePower(d, from, n, 100.0 * k, sr);
            rest += v;
            if (k <= 3) h[k] = v;
        }
        auto db = [h1] (double v) { return 10.0 * std::log10(juce::jmax(1.0e-30, v) / h1); };
        return { db(rest), db(h[2]), db(h[3]) };
    }

    void driveReport()
    {
        std::cout << "\n== effects: DRIVE, THD of a -12 dBFS 100 Hz sine (harmonics 2..20)\n"
                  << "   claim (CharacterChain.h process 2. DRIVE, kDrivePre, kDriveBias): asymmetric tanh overdrive (grit), the bias adds\n"
                  << "   even harmonics; dry/wet blended by the knob; 0 % = clean.\n"
                  << "amount     THD dB    THD %     H2 dB     H3 dB\n";
        for (float amt : { 25.0f, 50.0f, 100.0f })
        {
            const Thd t = driveThd(kSR, amt);
            std::cout << std::setw(4) << (int) amt << " %  " << fx(t.all, 9) << nx(100.0 * std::pow(10.0, t.all / 20.0), 9, 2)
                      << fx(t.h2, 10) << fx(t.h3, 10) << "\n";
        }

        std::cout << "\n   TONE: white noise (peak -6 dBFS, mono), level change against the dry source in two bands, and\n"
                  << "   the K-weighted loudness change against TONE 0 at the same DRIVE (the tone alone)\n"
                  << "   claim (CharacterChain.h process 2. DRIVE, toneCompFor, kDriveToneHz/Db): a symmetric first-order tilt of\n"
                  << "   the grit (the wet only) pivoting at 1.8 kHz, shelves +/-4.5 dB at +/-100 %, which reads as a\n"
                  << "   +/-6 dB tilt (high - low) on white noise; level-neutral (its own gain holds the wet's\n"
                  << "   K-weighted loudness); 0 = flat, the branch skipped.\n"
                  << "drive   tone   200-1800 Hz   1.8-16 kHz   tilt (high - low)   loudness vs tone 0\n";
        const int n = (int) (kSR * 4.0), nfft = 65536, from = (int) (kSR * 2.0);
        juce::AudioBuffer<float> noise(2, n);
        juce::Random rng(3);
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.5f * (rng.nextFloat() * 2.0f - 1.0f);
            noise.setSample(0, i, v);
            noise.setSample(1, i, v);
        }
        const auto pin = powerSpectrum(noise.getReadPointer(0) + from, nfft, nfft);
        for (float amt : { 25.0f, 50.0f, 100.0f })
        {
            CP p0 = neutral();
            p0.drive = amt;
            const double flat = loudness(runConst(noise, p0, kSR), from, n - from);
            for (float tone : { -100.0f, -50.0f, 0.0f, 50.0f, 100.0f })
            {
                CP p = neutral();
                p.drive = amt; p.driveTone = tone;
                const auto o = runConst(noise, p, kSR);
                const auto po = powerSpectrum(o.getReadPointer(0) + from, nfft, nfft);
                const double lo = 10.0 * std::log10(bandPower(po, nfft, kSR, 200, 1800) / bandPower(pin, nfft, kSR, 200, 1800));
                const double hi = 10.0 * std::log10(bandPower(po, nfft, kSR, 1800, 16000) / bandPower(pin, nfft, kSR, 1800, 16000));
                std::cout << std::setw(4) << (int) amt << " %" << fx(tone, 7, 0) << " %" << fx(lo, 10) << " dB" << fx(hi, 10)
                          << " dB" << fx(hi - lo, 12) << " dB" << fx(loudness(o, from, n - from) - flat, 16) << " dB\n";
            }
        }
    }

    // ECHO's first repeat after an impulse at frame 1000: its delay in ms.
    double echoDelayMs(double sr, float timePct)
    {
        CP p = neutral();
        p.echo = 50; p.echoTime = timePct;
        const int at = 1000;
        const auto o = runConst(impulse(1.0, sr, at), p, sr);
        int best = at;
        float peak = 0.0f;
        for (int i = at + 8; i < o.getNumSamples(); ++i)         // past the direct impulse
            if (std::abs(o.getSample(0, i)) > peak) { peak = std::abs(o.getSample(0, i)); best = i; }
        return (best - at) * 1000.0 / sr;
    }
    // The first repeat (echo 50 %, time 100 % = 350 ms) after an impulse, as a power spectrum.
    std::vector<double> echoRepeatSpectrum(CP p, int nfft)
    {
        p.echo = 50; p.echoTime = 100;
        const int at = 1000, D = (int) std::lround(kClaimEchoMaxMs * 0.001 * kSR);
        const auto o = runConst(impulse(1.0, kSR, at), p, kSR);
        return powerSpectrum(o.getReadPointer(0) + at + D - 64, 8192, nfft);
    }

    void echoReport()
    {
        std::cout << "\n== effects: ECHO time, impulse in, ms to the first repeat (echo 50 %)\n"
                  << "   claim (CharacterChain.h echoDelayTgt, kEchoTimeMinMs/MaxMs; CLAUDE.md): echoTime 0..100 % = 30..350 ms slapback, linear\n"
                  << "echoTime   claim ms   measured ms\n";
        for (float t : { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
            std::cout << std::setw(5) << (int) t << " %" << nx(kClaimEchoMinMs + (kClaimEchoMaxMs - kClaimEchoMinMs) * t * 0.01, 11)
                      << nx(echoDelayMs(kSR, t), 14, 2) << "\n";

        std::cout << "\n   Repeats: impulse in, echoTime 50 % (190 ms). Energy of each repeat against the direct sound\n"
                  << "   claim (CharacterChain.h process 3. ECHO, kEchoFbMax, kEchoDampHz): feedback 0.55 x amount (repeat count), each repeat\n"
                  << "   HF-damped at 3.2 kHz; the knob raises feedback and wet mix together.\n"
                  << "amount   fb claim (dB/rep)   rep 1 dB   rep 2 dB   rep 3 dB   decay dB/rep (2-5)   repeats above -60 dB\n";
        for (float amt : { 25.0f, 50.0f, 100.0f })
        {
            CP p = neutral();
            p.echo = amt; p.echoTime = 50;
            const int at = 1000, D = (int) std::lround((kClaimEchoMinMs + (kClaimEchoMaxMs - kClaimEchoMinMs) * 0.5) * 0.001 * kSR);
            const auto o = runConst(impulse(12.0, kSR, at), p, kSR);
            const double direct = (double) o.getSample(0, at) * o.getSample(0, at);
            std::vector<double> lv;
            for (int k = 1; at + k * D + D < o.getNumSamples(); ++k)
            {
                double e = 0.0;
                for (int i = at + k * D - D / 4; i < at + k * D + 3 * D / 4; ++i) e += (double) o.getSample(0, i) * o.getSample(0, i);
                lv.push_back(10.0 * std::log10(juce::jmax(1.0e-30, e) / direct));
            }
            int count = 0;
            for (double v : lv) if (v > -60.0) ++count;
            std::cout << std::setw(4) << (int) amt << " %" << fx(20.0 * std::log10(kClaimEchoFb * amt * 0.01), 16) << "   "
                      << fx(lv[0], 10) << fx(lv[1], 11) << fx(lv[2], 11) << fx((lv[4] - lv[1]) / 3.0, 16) << std::setw(20) << count << "\n";
        }

        std::cout << "\n   Band: the first repeat (echo 50 %, 350 ms) against the open band's, -3 dB edges\n"
                  << "   claim (CharacterChain.h header, bandFreq, bandTargets, Cut, cutA1): lo = low cut, hi = high cut, each 0..1 on\n"
                  << "   a log axis 20 Hz..20 kHz; lo <= 0.02 and hi >= 0.98 bypass. Each cut is a 12 dB/oct pair placed\n"
                  << "   so its handle frequency is the -3 dB corner, at any sample rate.\n"
                  << kBandHeader;
        const int nfft = 65536;
        const auto open = echoRepeatSpectrum(neutral(), nfft);
        for (bool lowCut : { true, false })
            for (float h : { 0.25f, 0.5f, 0.75f })
            {
                CP p = neutral();
                (lowCut ? p.echoLo : p.echoHi) = h;
                const auto sp = echoRepeatSpectrum(p, nfft);
                bandRow(lowCut ? "echoLo" : "echoHi", h, lowCut,
                        [&] (double f) { return ratioDb(sp, open, nfft, kSR, f); }, kSR);
            }
    }

    // T30 of a decay: Schroeder's backward integral of both channels' energy from
    // `from`, the time between its -5 and -35 dB crossings, doubled. -1 = never reaches -35.
    double t30(const juce::AudioBuffer<float>& b, int from, double sr)
    {
        const int n = b.getNumSamples();
        std::vector<double> e((size_t) (n - from + 1), 0.0);
        for (int i = n - 1; i >= from; --i)
            e[(size_t) (i - from)] = e[(size_t) (i - from + 1)]
                                   + (double) b.getSample(0, i) * b.getSample(0, i) + (double) b.getSample(1, i) * b.getSample(1, i);
        int i5 = -1, i35 = -1;
        for (size_t i = 0; i < e.size() && i35 < 0; ++i)
        {
            const double db = 10.0 * std::log10(juce::jmax(1.0e-300, e[i]) / e[0]);
            if (i5 < 0 && db <= -5.0) i5 = (int) i;
            if (db <= -35.0) i35 = (int) i;
        }
        return (i5 < 0 || i35 < 0) ? -1.0 : 2.0 * (i35 - i5) / sr;
    }
    double reverbT30(double sr, float amount, float decay, int type = 1)
    {
        CP p = neutral();
        p.reverb = amount; p.reverbDecay = decay; p.reverbType = (float) type;
        const auto o = runConst(impulse(20.0, sr, 1000), p, sr);
        return t30(o, 1001, sr);                   // the dry impulse is frame 1000 alone
    }

    void reverbReport()
    {
        std::cout << "\n== effects: REVERB decay, impulse in, T30 (s) of the wet tail, amount 100 %, per type and sample rate\n"
                  << "   claim (CharacterChain.h process 4. REVERB, reverbT30For, kReverbT30*/kSpringT30*; ReverbTanks.h setT30\n"
                  << "   tables; CLAUDE.md): DECAY = tail length, geometric. PLATE and HALL keep v2's curve (0.78 -> 5.5 s,\n"
                  << "   x1.63 per 25 %), SPRING its own (0.6 -> 3.0 s, x1.50 per 25 %); each within 10 % at 44.1/48/96 kHz;\n"
                  << "   independent of the amount knob (wet level). (off) = worst of the three rates against the claim.\n"
                  << "decay  claim P/H   PLATE 44.1   48     96  (off)    HALL 44.1   48     96  (off)   claim S   SPRING 44.1   48     96  (off)\n";
        const double rates[] = { 44100.0, 48000.0, 96000.0 };
        for (int k = 0; k < 5; ++k)
        {
            const float d = 25.0f * (float) k;
            std::cout << std::setw(4) << (int) d << " %" << nx(claimT30(1, d), 9, 2) << " s";
            for (int type = 0; type < 3; ++type)
            {
                if (type == 2) std::cout << nx(claimT30(2, d), 10, 2) << " s";
                double worst = 0.0;
                std::cout << "  ";
                for (double sr : rates)
                {
                    const double t = reverbT30(sr, 100, d, type);
                    worst = std::abs(t / claimT30(type, d) - 1.0) > std::abs(worst) ? t / claimT30(type, d) - 1.0 : worst;
                    std::cout << nx(t, 7, 2);
                }
                std::cout << " (" << fx(100.0 * worst, 3, 0) << "%)";
            }
            std::cout << "\n";
        }
        std::cout << "   amount 50 %, decay 50 %, 44.1 kHz:";
        for (int type = 0; type < 3; ++type)
            std::cout << "  " << kTypeName[type] << nx(reverbT30(kSR, 50, 50, type), 6, 2) << " s";
        std::cout << "\n";

        std::cout << "\n   Band: the wet tail (reverb 100 %, decay 50 %, first 3 s) against the open band's, per type, 44.1 kHz\n"
                  << "   claim: as ECHO's band (CharacterChain.h process 4. REVERB, bandTargets, Cut), whatever the tank\n"
                  << "type    " << kBandHeader;
        const int nfft = 131072, at = 1000;
        auto wet = [&] (CP p, double sr)
        {
            p.reverb = 100; p.reverbDecay = 50;
            const auto o = runConst(impulse(4.0, sr, at), p, sr);
            return powerSpectrum(o.getReadPointer(0) + at + 1, nfft, nfft);
        };
        for (int type = 0; type < 3; ++type)
        {
            CP base = neutral();
            base.reverbType = (float) type;
            const auto open = wet(base, kSR);
            for (bool lowCut : { true, false })
                for (float h : { 0.25f, 0.5f, 0.75f })
                {
                    CP p = base;
                    (lowCut ? p.reverbLo : p.reverbHi) = h;
                    const auto sp = wet(p, kSR);
                    std::cout << std::left << std::setw(8) << kTypeName[type] << std::right;
                    bandRow(lowCut ? "reverbLo" : "reverbHi", h, lowCut,
                            [&] (double f) { return ratioDb(sp, open, nfft, kSR, f); }, kSR);
                }
        }
        std::cout << "\n   Band at 48 and 96 kHz, handle 0.5 (claim 632 Hz), -3 dB edge Hz per type\n"
                  << "type      reverbLo 48k   96k    reverbHi 48k   96k\n";
        for (int type = 0; type < 3; ++type)
        {
            std::cout << std::left << std::setw(8) << kTypeName[type] << std::right;
            for (bool lowCut : { true, false })
            {
                std::cout << "    ";
                for (double sr : { 48000.0, 96000.0 })
                {
                    CP base = neutral();
                    base.reverbType = (float) type;
                    const auto open = wet(base, sr);
                    CP p = base;
                    (lowCut ? p.reverbLo : p.reverbHi) = 0.5f;
                    const auto sp = wet(p, sr);
                    // scanned to 5 kHz: SPRING's tail has nothing above ~4.4 kHz for the ratio to read
                    std::cout << hz(edgeHz([&] (double f) { return ratioDb(sp, open, nfft, sr, f); }, 3.0, lowCut, 10.0, 5000.0), 7);
                }
            }
            std::cout << "\n";
        }
    }

    // A one-channel buffer of the mono sum, (L + R) / 2.
    juce::AudioBuffer<float> monoSum(const juce::AudioBuffer<float>& b)
    {
        juce::AudioBuffer<float> m(1, b.getNumSamples());
        for (int i = 0; i < b.getNumSamples(); ++i) m.setSample(0, i, 0.5f * (b.getSample(0, i) + b.getSample(1, i)));
        return m;
    }
    struct Image { double corr = 0.0, sideMid = 0.0, mid = 0.0; };   // L/R correlation, S/M dB, mid energy (dB)
    Image imageOf(const juce::AudioBuffer<float>& b, int from, int n)
    {
        double ll = 0, rr = 0, lr = 0, mm = 0, ss = 0;
        for (int i = from; i < from + n; ++i)
        {
            const double l = b.getSample(0, i), r = b.getSample(1, i), m = 0.5 * (l + r), s = 0.5 * (l - r);
            ll += l * l; rr += r * r; lr += l * r; mm += m * m; ss += s * s;
        }
        return { lr / std::sqrt(juce::jmax(1.0e-30, ll * rr)), 10.0 * std::log10(juce::jmax(1.0e-30, ss) / juce::jmax(1.0e-30, mm)),
                 10.0 * std::log10(juce::jmax(1.0e-30, mm)) };
    }

    void widthReport()
    {
        std::cout << "\n== effects: WIDTH on a mono-ish source (common white noise + 0.3x independent noise per side)\n"
                  << "   claim (CharacterChain.h process 5. WIDTH, kWidthBoost, kWidthMidDb; CLAUDE.md): in-band side x(1 + width), so +6 dB\n"
                  << "   side at 100 %; the mid is untouched, so the mono sum (L+R) should not change. CENTER (widthMid)\n"
                  << "   +/-100 % = +/-9 dB on the mid, works at width 0. Mono sum and loudness are K-weighted.\n"
                  << "setting                 L/R corr   S/M dB   S/M change   mid change   mono sum change   loudness change\n";
        const int n = (int) (kSR * 4.0);
        juce::AudioBuffer<float> src(2, n);
        juce::Random rng(11);
        for (int i = 0; i < n; ++i)
        {
            const float c = rng.nextFloat() - 0.5f;
            src.setSample(0, i, c + 0.3f * (rng.nextFloat() - 0.5f));
            src.setSample(1, i, c + 0.3f * (rng.nextFloat() - 0.5f));
        }
        const Image in = imageOf(src, 0, n);
        const double inMono = loudness(monoSum(src), 0, n), inLoud = loudness(src, 0, n);
        struct Row { const char* name; float width, mid; };
        for (const Row r : { Row { "source", -1, 0 }, Row { "WIDTH 25", 25, 0 }, Row { "WIDTH 50", 50, 0 }, Row { "WIDTH 100", 100, 0 },
                             Row { "CENTER -100 (width 0)", 0, -100 }, Row { "CENTER +100 (width 0)", 0, 100 },
                             Row { "WIDTH 100, CENTER +100", 100, 100 } })
        {
            CP p = neutral();
            p.width = juce::jmax(0.0f, r.width); p.widthMid = r.mid;
            const auto o = r.width < 0 ? src : runConst(src, p, kSR);
            const Image im = imageOf(o, 0, n);
            std::cout << std::left << std::setw(22) << r.name << std::right << nx(im.corr, 10, 3) << fx(im.sideMid, 9)
                      << fx(im.sideMid - in.sideMid, 11) << " dB" << fx(im.mid - in.mid, 10) << " dB"
                      << fx(loudness(monoSum(o), 0, n) - inMono, 14) << " dB" << fx(loudness(o, 0, n) - inLoud, 15) << " dB\n";
        }

        std::cout << "\n   Band: WIDTH 100 %, widthLo 0.5 (632 Hz). Side level change below and above the band\n"
                  << "   claim (CharacterChain.h process 5. WIDTH): out-of-band side folds toward mono (x(1 - width)),\n"
                  << "   in-band side is widened\n";
        CP p = neutral();
        p.width = 100; p.widthLo = 0.5f;
        const auto o = runConst(src, p, kSR);
        const int nfft = 65536;
        auto side = [] (const juce::AudioBuffer<float>& b)
        {
            std::vector<float> s((size_t) nfft);
            for (int i = 0; i < nfft; ++i) s[(size_t) i] = 0.5f * (b.getSample(0, i + 8192) - b.getSample(1, i + 8192));
            return powerSpectrum(s.data(), nfft, nfft);
        };
        const auto si = side(src), so = side(o);
        const std::pair<double, double> bands[] = { { 70, 140 }, { 300, 450 }, { 1500, 3000 }, { 3000, 10000 } };
        for (const auto& [lo, hi] : bands)
            std::cout << "side " << nx(lo, 6, 0) << "-" << std::left << std::setw(6) << (int) hi << std::right << " Hz  "
                      << fx(10.0 * std::log10(bandPower(so, nfft, kSR, lo, hi) / bandPower(si, nfft, kSR, lo, hi)), 7) << " dB\n";
    }

    // The master EQ's response (dB) for a pair of handles: its impulse response, from processEq.
    Resp eqResponse(float lo, float hi, double sr)
    {
        CP p = neutral();
        p.eqLo = lo; p.eqHi = hi;
        int nfft = 1;
        while (nfft < (int) (1.4 * sr)) nfft <<= 1;
        const auto o = runConst(impulse((double) nfft / sr + 0.01, sr, 0), p, sr, true);
        const auto sp = std::make_shared<std::vector<double>>(powerSpectrum(o.getReadPointer(0), nfft, nfft));
        const auto flat = std::make_shared<std::vector<double>>(sp->size(), 1.0);
        return [sp, flat, nfft, sr] (double f) { return ratioDb(*sp, *flat, nfft, sr, f); };
    }

    void eqReport()
    {
        std::cout << "\n== effects: master EQ, impulse response of processEq\n"
                  << "   claim (CharacterChain.h processEq, bandTargets, Cut; parameter layout eqLo/eqHi): two-handle band-pass,\n"
                  << "   each handle 0..1 on a log axis 20 Hz..20 kHz; true bypass at lo <= 0.02 / hi >= 0.98.\n"
                  << kBandHeader;
        for (float h : { 0.02f, 0.021f, 0.03f, 0.04f, 0.05f, 0.06f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f })
            bandRow("eqLo", h, true, eqResponse(h, 1.0f, kSR), kSR);
        for (float h : { 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 0.94f, 0.95f, 0.96f, 0.97f, 0.979f, 0.98f })
            bandRow("eqHi", h, false, eqResponse(0.0f, h, kSR), kSR);
    }

    Biquad highPass(double fc, double q, double sr)
    {
        const double w = kTwoPi * fc / sr, cs = std::cos(w), al = std::sin(w) / (2.0 * q), a0 = 1.0 + al;
        Biquad b;
        b.b0 = 0.5 * (1.0 + cs) / a0; b.b1 = -(1.0 + cs) / a0; b.b2 = b.b0;
        b.a1 = -2.0 * cs / a0; b.a2 = (1.0 - al) / a0;
        return b;
    }
    // Peak (dBFS) above 5 kHz (4th-order Butterworth high-pass, run from frame 0) over [from, from + n).
    double hfPeakDb(const juce::AudioBuffer<float>& b, int from, int n, double sr)
    {
        double peak = 0.0;
        for (int ch = 0; ch < 2; ++ch)
        {
            Biquad a = highPass(5000.0, 0.5412, sr), c = highPass(5000.0, 1.3066, sr);
            for (int i = 0; i < from + n && i < b.getNumSamples(); ++i)
            {
                const double v = c.process(a.process(b.getSample(ch, i)));
                if (i >= from) peak = juce::jmax(peak, std::abs(v));
            }
        }
        return juce::Decibels::gainToDecibels(peak, -200.0);
    }

    void zipperReport()
    {
        std::cout << "\n== effects: zipper. Each control swept 0 -> full -> 0 in 50 ms (fast) and in 5 s (slow), pushed once\n"
                  << "   per 512-frame block, on a 220 Hz sine (-12 dBFS, R 90 deg ahead). Peak above 5 kHz, dBFS; fast\n"
                  << "   minus slow = what the motion itself adds (clicks), flagged when over 6 dB and the fast peak is above\n"
                  << "   -60 dBFS. The first row is an unsmoothed gain for scale.\n"
                  << "   claim (CharacterChain.h process slew, smoothA, delaySlewA; PluginProcessor.cpp smInGain/smOutGain): amounts and gains slew\n"
                  << "   (~15 ms chain, ~20 ms gain stages), the echo tap ~40 ms; band handles slew at the chain's ~15 ms\n"
                  << "   (CharacterChain.h Cut::tick). Rates are not slewed.\n"
                  << "control                         fast     slow    fast - slow\n";
        const double sr = kSR;
        const auto fastSrc = sine(220.0, -12.0, 2.0, sr, kTwoPi / 4.0), slowSrc = sine(220.0, -12.0, 7.0, sr, kTwoPi / 4.0);
        const int f0 = (int) sr, fastN = (int) (0.2 * sr), slowN = (int) (5.2 * sr);
        auto row = [&] (const std::string& name, double fast, double slow)
        {
            std::cout << std::left << std::setw(28) << name << std::right << fx(fast, 9) << fx(slow, 9) << fx(fast - slow, 11) << " dB"
                      << (fast - slow > 6.0 && fast > -60.0 ? "   <- motion adds HF" : "") << "\n";
        };
        {
            // An unsmoothed gain stepped once per block, 0 to -12 dB and back: what no slew sounds like here.
            auto stepped = [&] (const juce::AudioBuffer<float>& src, double dur)
            {
                juce::AudioBuffer<float> o(src);
                for (int i = 0; i < o.getNumSamples(); ++i)
                {
                    const float g = 1.0f - 0.75f * (float) tri((double) ((i / 512) * 512) / sr, 1.0, dur);
                    o.setSample(0, i, o.getSample(0, i) * g);
                    o.setSample(1, i, o.getSample(1, i) * g);
                }
                return o;
            };
            row("(unsmoothed gain, ref)", hfPeakDb(stepped(fastSrc, 0.05), f0, fastN, sr), hfPeakDb(stepped(slowSrc, 5.0), f0, slowN, sr));
        }
        struct Knob { const char* name; std::function<void(CP&, double)> set; bool eq; };
        const std::vector<Knob> knobs = {
            { "WARP", [] (CP& p, double v) { p.warp = (float) (100 * v); }, false },
            { "FLUTTER", [] (CP& p, double v) { p.flutter = (float) (100 * v); }, false },
            { "DRIVE", [] (CP& p, double v) { p.drive = (float) (100 * v); }, false },
            { "ECHO", [] (CP& p, double v) { p.echo = (float) (100 * v); }, false },
            { "REVERB", [] (CP& p, double v) { p.reverb = (float) (100 * v); }, false },
            { "WIDTH", [] (CP& p, double v) { p.width = (float) (100 * v); }, false },
            { "warpRate (warp 50)", [] (CP& p, double v) { p.warp = 50; p.warpRate = (float) (100 * v); }, false },
            { "flutterRate (flutter 50)", [] (CP& p, double v) { p.flutter = 50; p.flutterRate = (float) (100 * v); }, false },
            { "driveTone -100..+100 (d 100)", [] (CP& p, double v) { p.drive = 100; p.driveTone = (float) (-100 + 200 * v); }, false },
            { "echoTime (echo 50)", [] (CP& p, double v) { p.echo = 50; p.echoTime = (float) (100 * v); }, false },
            { "echoLo 0..0.8 (echo 50)", [] (CP& p, double v) { p.echo = 50; p.echoLo = (float) (0.8 * v); }, false },
            { "echoHi 1..0.2 (echo 50)", [] (CP& p, double v) { p.echo = 50; p.echoHi = (float) (1.0 - 0.8 * v); }, false },
            { "reverbDecay (reverb 50)", [] (CP& p, double v) { p.reverb = 50; p.reverbDecay = (float) (100 * v); }, false },
            { "reverbLo 0..0.8 (rev 50)", [] (CP& p, double v) { p.reverb = 50; p.reverbLo = (float) (0.8 * v); }, false },
            { "reverbHi 1..0.2 (rev 50)", [] (CP& p, double v) { p.reverb = 50; p.reverbHi = (float) (1.0 - 0.8 * v); }, false },
            { "widthMid 0..+100", [] (CP& p, double v) { p.widthMid = (float) (100 * v); }, false },
            { "widthLo 0..0.8 (width 100)", [] (CP& p, double v) { p.width = 100; p.widthLo = (float) (0.8 * v); }, false },
            { "eqLo 0..0.6", [] (CP& p, double v) { p.eqLo = (float) (0.6 * v); }, true },
            { "eqHi 1..0.4", [] (CP& p, double v) { p.eqHi = (float) (1.0 - 0.6 * v); }, true },
        };
        for (const auto& k : knobs)
        {
            auto run = [&] (const juce::AudioBuffer<float>& src, double dur)
            {
                return runBlocks(src, [&] (int i) { CP p = neutral(); k.set(p, tri(i / sr, 1.0, dur)); return p; }, sr, k.eq);
            };
            row(k.name, hfPeakDb(run(fastSrc, 0.05), f0, fastN, sr), hfPeakDb(run(slowSrc, 5.0), f0, slowN, sr));
        }

        // The processor's own gain stages, in FX (the track's audio in).
        struct Stage { const char* name; const char* id; float lo, hi; };
        for (const Stage s : { Stage { "INPUT -60..0 dB (FX)", "input", -60.0f, 0.0f }, Stage { "OUTPUT -60..0 dB (FX)", "output", -60.0f, 0.0f },
                               Stage { "MIX 0..100 (FX, DRIVE 100)", "mix", 0.0f, 100.0f } })
        {
            auto run = [&] (const juce::AudioBuffer<float>& src, double dur)
            {
                LoopNestProcessor p;
                p.setFxMode(true);
                if (juce::String(s.id) == "mix") setNatural(p, "drive", 100.0f);
                setNatural(p, s.id, s.lo);
                p.prepareToPlay(sr, 512);
                juce::AudioBuffer<float> out(2, src.getNumSamples()), buf(2, 512);
                juce::MidiBuffer midi;
                for (int b = 0; (b + 1) * 512 <= src.getNumSamples(); ++b)
                {
                    setNatural(p, s.id, s.lo + (s.hi - s.lo) * (float) tri(b * 512 / sr, 1.0, dur));
                    for (int ch = 0; ch < 2; ++ch) buf.copyFrom(ch, 0, src, ch, b * 512, 512);
                    p.processBlock(buf, midi);
                    for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, b * 512, buf, ch, 0, 512);
                }
                p.releaseResources();
                return out;
            };
            row(s.name, hfPeakDb(run(fastSrc, 0.05), f0, fastN, sr), hfPeakDb(run(slowSrc, 5.0), f0, slowN, sr));
        }

        std::cout << "\n   Dead-zone crossing: a band handle stepped from open to just inside its dead band, then held.\n"
                  << "   claim (CharacterChain.h Cut, CutState): an open cut keeps running at its dead zone's edge, and engaging\n"
                  << "   it crossfades (~5 ms) from the input to that settled filter, so there is no dropout and no click.\n"
                  << "   Peak of (stepped run - run already at the new value), dBFS, in the 50 ms after the step (the\n"
                  << "   crossfade itself: the open band against the engaged cut) and 200..300 ms after; the sine itself\n"
                  << "   peaks at -12 dBFS. HF = peak above 5 kHz of the stepped run from 50 ms before to 100 ms after the\n"
                  << "   step (a click), and of the held run over the same frames for scale.\n"
                  << "step                              0..50 ms   200..300 ms   HF stepped   HF held\n";
        struct Step { const char* name; std::function<void(CP&)> engage; bool eq; CP base; };
        CP echoOn = neutral(), widthOn = neutral();
        echoOn.echo = 50; widthOn.width = 100;
        const std::vector<Step> steps = {
            { "eqLo 0 -> 0.03", [] (CP& p) { p.eqLo = 0.03f; }, true, neutral() },
            { "eqHi 1 -> 0.97", [] (CP& p) { p.eqHi = 0.97f; }, true, neutral() },
            { "echoLo 0 -> 0.03 (echo 50)", [] (CP& p) { p.echoLo = 0.03f; }, false, echoOn },
            { "echoHi 1 -> 0.97 (echo 50)", [] (CP& p) { p.echoHi = 0.97f; }, false, echoOn },
            { "widthLo 0 -> 0.03 (width 100)", [] (CP& p) { p.widthLo = 0.03f; }, false, widthOn },
        };
        const int sw = (f0 + 511) / 512 * 512;              // the block the step lands on
        for (const auto& st : steps)
        {
            CP to = st.base;
            st.engage(to);
            const auto a = runBlocks(fastSrc, [&] (int i) { return i < sw ? st.base : to; }, sr, st.eq);
            const auto b = runConst(fastSrc, to, sr, st.eq);
            auto peakDiff = [&] (double t0, double t1)
            {
                float d = 0.0f;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = sw + (int) (t0 * sr); i < sw + (int) (t1 * sr); ++i)
                        d = juce::jmax(d, std::abs(a.getSample(ch, i) - b.getSample(ch, i)));
                return juce::Decibels::gainToDecibels((double) d, -200.0);
            };
            const int hfFrom = sw - (int) (0.05 * sr), hfN = (int) (0.15 * sr);
            std::cout << std::left << std::setw(32) << st.name << std::right << fx(peakDiff(0.0, 0.05), 10) << fx(peakDiff(0.2, 0.3), 13)
                      << fx(hfPeakDb(a, hfFrom, hfN, sr), 13) << fx(hfPeakDb(b, hfFrom, hfN, sr), 10) << "\n";
        }
    }

    // REVERB TYPE levels on real loops (D-M2). Each tank's wet alone (the output less the dry
    // share, the third pass, K-weighted) against HALL's at DECAY 0/25/50/75/100, the median
    // over the files; the gain table that puts each type's wet on HALL's there (HALL is kept:
    // the default type, and the table's anchor to v2's Freeverb level); and the whole-mix
    // spread, loudest type minus quietest (the 10-18 audit's measure: amount 50 / 100, decay
    // 0 / 31 / 100), with the table as built and as solved. The wet is linear in its gain, so
    // the solved table's mix is the built one's wet rescaled. 44.1 kHz files only.
    void typeGainsReport(int argc, char** argv, int first)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::vector<juce::AudioBuffer<float>> loops;
        std::vector<juce::String> names;
        for (int a = first; a < argc; ++a)
        {
            const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[a]));
            std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(f) };
            if (r == nullptr || std::abs(r->sampleRate - kSR) > 0.5 || r->lengthInSamples < 1000) continue;
            juce::AudioBuffer<float> b(2, (int) r->lengthInSamples);
            r->read(&b, 0, (int) r->lengthInSamples, 0, true, true);
            if (r->numChannels == 1) b.copyFrom(1, 0, b, 0, 0, b.getNumSamples());
            loops.push_back(std::move(b));
            names.push_back(f.getFileName());
        }
        std::cout << "\n== typegains: REVERB TYPE wet levels on " << loops.size() << " real loops (44.1 kHz), third pass, K-weighted\n";
        if (loops.empty()) return;
        auto share = [] (float amt) { const float r = amt * 0.01f; return (1.0f - 0.3f * r) * std::pow(10.0f, r * -3.5f / 20.0f); };
        auto wetOf = [&] (const juce::AudioBuffer<float>& src, float amt, float decay, int type)
        {
            CP p = neutral(); p.reverb = amt; p.reverbDecay = decay; p.reverbType = (float) type;
            auto o = runChain(src, p, 3, kSR);
            const float sh = share(amt);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < o.getNumSamples(); ++i) o.setSample(ch, i, o.getSample(ch, i) - sh * src.getSample(ch, i));
            return o;
        };
        auto median = [] (std::vector<double> v) { std::sort(v.begin(), v.end()); const size_t m = v.size() / 2;
                                                   return v.size() % 2 ? v[m] : 0.5 * (v[m - 1] + v[m]); };

        // 1. The solve, at amount 100 (the wet's level against HALL's does not depend on it).
        const float nodes[] = { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f };
        double built[3][5], solved[3][5], spreadWet[3][5];
        std::cout << "decay   PLATE wet vs HALL (median, min..max)   SPRING wet vs HALL (median, min..max)\n";
        for (int k = 0; k < 5; ++k)
        {
            std::vector<double> d[3];
            for (const auto& src : loops)
            {
                double L[3];
                for (int type = 0; type < 3; ++type)
                {
                    const auto w = wetOf(src, 100.0f, nodes[k], type);
                    L[type] = loudness(w, 0, w.getNumSamples());
                }
                for (int type = 0; type < 3; ++type) d[type].push_back(L[type] - L[1]);
            }
            std::cout << std::setw(5) << (int) nodes[k];
            for (int type : { 0, 2 })
            {
                const auto mm = std::minmax_element(d[type].begin(), d[type].end());
                std::cout << "      " << fx(median(d[type]), 6, 2) << "  (" << fx(*mm.first, 6, 2) << " .. " << fx(*mm.second, 6, 2) << ")      ";
            }
            std::cout << "\n";
            for (int type = 0; type < 3; ++type)
            {
                built[type][k]  = CharacterChain::reverbTypeGainDb(type, nodes[k] * 0.01f);
                spreadWet[type][k] = median(d[type]);
                solved[type][k] = built[type][k] - (type == 1 ? 0.0 : spreadWet[type][k]);
            }
        }
        std::cout << "\ntable (dB at DECAY 0/25/50/75/100)   built                                    solved\n";
        for (int type = 0; type < 3; ++type)
        {
            std::cout << std::left << std::setw(8) << kTypeName[type] << std::right << "                     ";
            for (int k = 0; k < 5; ++k) std::cout << nx(built[type][k], 6, 2);
            std::cout << "      ";
            for (int k = 0; k < 5; ++k) std::cout << nx(solved[type][k], 6, 2);
            std::cout << "\n";
        }

        // 2. The whole-mix spread, built and solved (the solved one: the built wet rescaled).
        auto tableAt = [&] (const double (&t)[3][5], int type, float decay)
        {
            const double u = juce::jlimit(0.0, 1.0, decay * 0.01) * 4.0;
            const int i = juce::jmin(3, (int) u);
            return t[type][i] + (u - i) * (t[type][i + 1] - t[type][i]);
        };
        std::vector<double> before, after;
        for (const auto& src : loops)
        {
            const int n = src.getNumSamples();
            const double dry = loudness(src, 0, n);
            for (float amt : { 50.0f, 100.0f })
                for (float decay : { 0.0f, 31.0f, 100.0f })
                {
                    double mixB[3], mixA[3];
                    for (int type = 0; type < 3; ++type)
                    {
                        const auto w = wetOf(src, amt, decay, type);
                        const float sh = share(amt);
                        const float g  = (float) juce::Decibels::decibelsToGain(tableAt(solved, type, decay) - tableAt(built, type, decay));
                        juce::AudioBuffer<float> b(2, n), a(2, n);
                        for (int ch = 0; ch < 2; ++ch)
                            for (int i = 0; i < n; ++i)
                            {
                                b.setSample(ch, i, sh * src.getSample(ch, i) + w.getSample(ch, i));
                                a.setSample(ch, i, sh * src.getSample(ch, i) + g * w.getSample(ch, i));
                            }
                        mixB[type] = loudness(b, 0, n) - dry;
                        mixA[type] = loudness(a, 0, n) - dry;
                    }
                    before.push_back(juce::jmax(mixB[0], mixB[1], mixB[2]) - juce::jmin(mixB[0], juce::jmin(mixB[1], mixB[2])));
                    after.push_back(juce::jmax(mixA[0], mixA[1], mixA[2]) - juce::jmin(mixA[0], juce::jmin(mixA[1], mixA[2])));
                }
        }
        std::cout << "\nwhole-mix spread over " << before.size() << " cases (" << loops.size() << " loops x amount 50/100 x decay 0/31/100):\n"
                  << "  table as built:  median " << nx(median(before), 5, 2) << " dB, max " << nx(*std::max_element(before.begin(), before.end()), 5, 2) << " dB\n"
                  << "  table solved:    median " << nx(median(after), 5, 2) << " dB, max " << nx(*std::max_element(after.begin(), after.end()), 5, 2) << " dB\n";
    }

    // REVERB TYPE: loudness across the tanks, a type change while it plays, and what each costs.
    void reverbTypeReport()
    {
        std::cout << "\n== effects: REVERB TYPE, loudness of each tank on the synthetic loop (peak -6 dBFS, third pass), dB\n"
                  << "   against the dry loop. claim (CharacterChain.h kReverbTypeGainDb): the same AMOUNT / DECAY is equally\n"
                  << "   loud in every type, within ~0.5 dB (spread = loudest minus quietest), and as loud as v2's Freeverb\n"
                  << "   was (its column: measured 10-17 on the same loop, before the change).\n"
                  << "amount  decay   Freeverb      PLATE     HALL   SPRING   spread\n";
        const auto loop = drumLoop(-6.0);
        const double dry = loudness(loop, 0, loop.getNumSamples());
        const double freeverb[2][5] = { { -2.46, -2.05, -1.41, -0.50, +0.69 }, { -3.51, -2.21, -0.57, +1.27, +3.25 } };
        for (float amt : { 50.0f, 100.0f })
            for (float dec : { 0.0f, 25.0f, 31.0f, 50.0f, 75.0f, 100.0f })
            {
                double v[3];
                for (int type = 0; type < 3; ++type)
                {
                    CP p = neutral();
                    p.reverb = amt; p.reverbDecay = dec; p.reverbType = (float) type;
                    const auto o = runChain(loop, p);
                    v[type] = loudness(o, 0, o.getNumSamples()) - dry;
                }
                const int node = dec == 31.0f ? -1 : (int) (dec / 25.0f);
                std::cout << std::setw(5) << (int) amt << " %" << std::setw(5) << (int) dec << " %"
                          << (node < 0 ? std::string("         -") : fx(freeverb[amt > 50.0f ? 1 : 0][node], 10, 2))
                          << fx(v[0], 11, 2) << fx(v[1], 9, 2) << fx(v[2], 9, 2)
                          << nx(juce::jmax(v[0], v[1], v[2]) - juce::jmin(v[0], v[1], v[2]), 9, 2) << "\n";
            }

        std::cout << "\n   A TYPE change while it plays: 220 Hz sine (-12 dBFS, R 90 deg ahead), reverb 100 %, decay 50 %, the type\n"
                  << "   stepped (one block) at 2 s. Peak above 5 kHz from 10 ms before to 100 ms after, dBFS, against the louder\n"
                  << "   of the two types run steady over the same frames; flagged when the change adds over 6 dB (a click).\n"
                  << "   claim (CharacterChain.h process 4. REVERB, kReverbTypeFadeSec): a ~30 ms equal-power crossfade, no click.\n"
                  << "change              stepped   steady    stepped - steady\n";
        const auto tone = sine(220.0, -12.0, 3.0, kSR, juce::MathConstants<double>::halfPi);
        const int sw = 2 * 44032;                                   // a block boundary near 2 s
        auto run = [&] (int a, int b)
        {
            return runBlocks(tone, [&] (int i) { CP p = neutral(); p.reverb = 100; p.reverbType = (float) (i < sw ? a : b); return p; }, kSR);
        };
        const int from = sw - (int) (0.01 * kSR), n = (int) (0.11 * kSR);
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
            {
                if (a == b) continue;
                const double stepped = hfPeakDb(run(a, b), from, n, kSR);
                const double steady  = juce::jmax(hfPeakDb(run(a, a), from, n, kSR), hfPeakDb(run(b, b), from, n, kSR));
                std::cout << std::left << std::setw(18) << (juce::String(kTypeName[a]) + " -> " + kTypeName[b]).toStdString() << std::right
                          << fx(stepped, 9) << fx(steady, 9) << fx(stepped - steady, 14) << " dB"
                          << (stepped - steady > 6.0 ? "   <- click" : "") << "\n";
            }
        for (int b = 0; b < 3; ++b)          // the amount itself stepped up from 0 (the tank's input rises ~30 ms too)
        {
            const auto o = runBlocks(tone, [&] (int i) { CP p = neutral(); p.reverb = i < sw ? 0.0f : 100.0f; p.reverbType = (float) b; return p; }, kSR);
            const double stepped = hfPeakDb(o, from, n, kSR), steady = hfPeakDb(run(b, b), from, n, kSR);
            std::cout << std::left << std::setw(18) << (juce::String("amount 0->100 ") + kTypeName[b]).toStdString() << std::right
                      << fx(stepped, 9) << fx(steady, 9) << fx(stepped - steady, 14) << " dB"
                      << (stepped - steady > 6.0 && stepped > -60.0 ? "   <- click" : "") << "   (not a type change; zipper's flag)\n";
        }

        std::cout << "\n   CPU, this build: seconds of processing per second of stereo audio, 44.1 kHz, 60 s of the loop, best of 3\n"
                  << "   (the chain driven directly, params pushed per 512-frame block)\n";
        juce::AudioBuffer<float> long_(2, (int) (60.0 * kSR));
        for (int i = 0; i < long_.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch) long_.setSample(ch, i, loop.getSample(ch, i % loop.getNumSamples()));
        auto cost = [&] (const CP& p, double sr)
        {
            double best = 1.0e9;
            for (int k = 0; k < 3; ++k)
            {
                CharacterChain c;
                c.prepare(sr);
                float sink = 0.0f;
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                for (int i = 0; i < long_.getNumSamples(); ++i)
                {
                    if (i % 512 == 0) c.setParams(p);
                    float x[2] = { long_.getSample(0, i), long_.getSample(1, i) };
                    c.process(x, 2);
                    sink += x[0];
                }
                best = juce::jmin(best, (juce::Time::getMillisecondCounterHiRes() - t0) * 0.001 / (long_.getNumSamples() / sr));
                if (sink == 12345.0f) std::cout << "";             // keep the work
            }
            return best;
        };
        CP neutralP = neutral();
        std::cout << "neutral chain                    " << nx(cost(neutralP, kSR), 9, 5) << " s/s\n";
        for (int type = 0; type < 3; ++type)
        {
            CP p = neutral();
            p.reverb = 50; p.reverbType = (float) type;
            std::cout << "REVERB 50, " << std::left << std::setw(22) << kTypeName[type] << std::right << nx(cost(p, kSR), 9, 5) << " s/s"
                      << "    (at 96 kHz: " << nx(cost(p, 96000.0), 8, 5) << " s/s)\n";
        }
        CP w = neutral(); w.warp = 100;
        CP f = neutral(); f.flutter = 100;
        std::cout << "WARP 100                         " << nx(cost(w, kSR), 9, 5) << " s/s\n"
                  << "FLUTTER 100                      " << nx(cost(f, kSR), 9, 5) << " s/s\n";
    }

    void sampleRateReport()
    {
        std::cout << "\n== effects: sample rate. A compact subset at 44.1, 48 and 96 kHz; SHIFTS = more than 2 % off\n"
                  << "   the 44.1 kHz value (THD: more than 0.5 dB)\n"
                  << "measure                               claim     44.1 kHz     48 kHz     96 kHz\n";
        const double rates[] = { 44100.0, 48000.0, 96000.0 };
        auto row = [&] (const char* name, const std::string& claim, const std::function<double(double)>& m, int prec, bool db = false)
        {
            double v[3];
            for (int i = 0; i < 3; ++i) v[i] = m(rates[i]);
            bool shifts = false;
            for (int i = 1; i < 3; ++i)
                shifts = shifts || (db ? std::abs(v[i] - v[0]) > 0.5 : std::abs(v[i] / v[0] - 1.0) > 0.02);
            std::cout << std::left << std::setw(34) << name << std::right << std::setw(9) << claim
                      << nx(v[0], 12, prec) << nx(v[1], 11, prec) << nx(v[2], 11, prec) << (shifts ? "   SHIFTS" : "") << "\n";
        };
        row("ECHO delay ms (echoTime 50)", "190", [] (double sr) { return echoDelayMs(sr, 50); }, 2);
        for (int type = 0; type < 3; ++type)
            row((juce::String("REVERB T30 s (") + kTypeName[type] + ", decay 50)").toRawUTF8(), nx(claimT30(type, 50), 0, 2),
                [type] (double sr) { return reverbT30(sr, 100, 50, type); }, 2);
        auto warpTrack = [] (double sr) { CP p = neutral(); p.warp = 100; p.warpRate = 50; return wobbleTrack(p, sr); };
        row("WARP rate Hz (100 %, rate 50)", "0.875", [&] (double sr) { return modPeak(warpTrack(sr), 1000.0, 1.0, 0.05, 5.0).hz; }, 3);
        row("WARP peak cents up (100, rate 50)", fx(cents(1.0 + claimWarpDev(1.0, 0.875)), 0, 1),
            [&] (double sr) { double u, d; centsRange(warpTrack(sr), 1000.0, 1.0, u, d); return u; }, 1);
        row("FLUTTER rate Hz (100 %, rate 50)", "6.00", [] (double sr)
            { CP p = neutral(); p.flutter = 100; return modPeak(wobbleTrack(p, sr), 1000.0, 1.0, 2.5, 20.0).hz; }, 2);
        row("DRIVE THD dB (50 %, 100 Hz)", "-", [] (double sr) { return driveThd(sr, 50).all; }, 1, true);
        row("EQ low cut -3 dB Hz (eqLo 0.5)", "632*", [] (double sr) { return edgeHz(eqResponse(0.5f, 1.0f, sr), 3.0, true, 10.0, 0.45 * sr); }, 0);
        row("EQ high cut -3 dB Hz (eqHi 0.5)", "632*", [] (double sr) { return edgeHz(eqResponse(0.0f, 0.5f, sr), 3.0, false, 10.0, 0.45 * sr); }, 0);
        row("EQ high cut -3 dB Hz (eqHi 0.9)", "10024*", [] (double sr) { return edgeHz(eqResponse(0.0f, 0.9f, sr), 3.0, false, 10.0, 0.45 * sr); }, 0);
        std::cout << "   * the handle's frequency on the claimed axis (the cut's -3 dB corner, see the EQ table)\n";
    }

    // max |a - b| over [from, from + n), both channels: "exact", or its level in dBFS.
    std::string nullOf(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int from, int n)
    {
        float d = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = from; i < from + n; ++i) d = juce::jmax(d, std::abs(a.getSample(ch, i) - b.getSample(ch, i)));
        if (d == 0.0f) return "      exact";
        return fx(juce::Decibels::gainToDecibels((double) d, -300.0), 7) + " dBFS";
    }

    void neutralReport()
    {
        std::cout << "\n== effects: neutral nulls on the synthetic loop (peak -6 dBFS), max |out - in|\n"
                  << "   claim (CharacterChain.h header, setParams panel OFF; CLAUDE.md): every control is true-bypass at its neutral\n"
                  << "   value (secondaries act only through their primary); a panel switched OFF is neutral too.\n"
                  << "   'after full' = 3 s at full, then neutral (or OFF), null over the last 3 s of 8.\n"
                  << "stage     neutral, secondaries at extremes        back to neutral after full   panel OFF after full\n";
        const auto loop = drumLoop(-6.0);
        const auto src = tiled(loop, 0);                       // 8 s
        const int n = src.getNumSamples(), tail = (int) (5.0 * kSR), switchAt = (int) (3.0 * kSR);
        struct Case { const char* name; std::function<void(CP&)> secondaries, full; int stage; bool eq; };
        const std::vector<Case> cases = {
            { "WARP", [] (CP& p) { p.warpRate = 100; }, [] (CP& p) { p.warp = 100; }, CP::kWarp, false },
            { "FLUTTER", [] (CP& p) { p.flutterRate = 0; }, [] (CP& p) { p.flutter = 100; }, CP::kFlutter, false },
            { "DRIVE", [] (CP& p) { p.driveTone = 100; }, [] (CP& p) { p.drive = 100; }, CP::kDrive, false },
            { "ECHO", [] (CP& p) { p.echoTime = 0; p.echoLo = 0.8f; p.echoHi = 0.2f; }, [] (CP& p) { p.echo = 100; }, CP::kEcho, false },
            { "REVERB", [] (CP& p) { p.reverbDecay = 100; p.reverbLo = 0.8f; p.reverbHi = 0.2f; }, [] (CP& p) { p.reverb = 100; }, CP::kReverb, false },
            { "  PLATE", [] (CP& p) { p.reverbDecay = 100; p.reverbType = 0; }, [] (CP& p) { p.reverb = 100; }, CP::kReverb, false },
            { "  HALL", [] (CP& p) { p.reverbDecay = 100; p.reverbType = 1; }, [] (CP& p) { p.reverb = 100; }, CP::kReverb, false },
            { "  SPRING", [] (CP& p) { p.reverbDecay = 100; p.reverbType = 2; }, [] (CP& p) { p.reverb = 100; }, CP::kReverb, false },
            { "WIDTH", [] (CP& p) { p.widthLo = 0.3f; p.widthHi = 0.7f; }, [] (CP& p) { p.width = 100; p.widthMid = 100; }, CP::kWidth, false },
            { "EQ", [] (CP& p) { p.eqLo = 0.02f; p.eqHi = 0.98f; }, [] (CP& p) { p.eqLo = 0.5f; p.eqHi = 0.5f; }, -1, true },
        };
        for (const auto& c : cases)
        {
            CP base = neutral();
            c.secondaries(base);
            CP full = base;
            c.full(full);
            const auto o1 = runConst(src, base, kSR, c.eq);
            const auto o2 = runBlocks(src, [&] (int i) { return i < switchAt ? full : base; }, kSR, c.eq);
            std::cout << std::left << std::setw(10) << c.name << std::right << std::setw(18) << nullOf(o1, src, 0, n)
                      << std::setw(40) << nullOf(o2, src, n - tail + (int) (2.0 * kSR), tail - (int) (2.0 * kSR));
            if (c.stage >= 0)
            {
                CP off = full;
                off.on[(size_t) c.stage] = false;
                const auto o3 = runBlocks(src, [&] (int i) { return i < switchAt ? full : off; }, kSR, c.eq);
                std::cout << std::setw(28) << nullOf(o3, src, n - tail + (int) (2.0 * kSR), tail - (int) (2.0 * kSR));
            }
            else
                std::cout << std::setw(28) << "n/a";
            std::cout << "\n";
        }

        std::cout << "\n   The processor's gain stages, FX mode on the same loop (8 s; last 4 s against the input)\n"
                  << "   claim (PluginProcessor.cpp parameter layout input/output/mix, smInGain/smOutGain): INPUT/OUTPUT -60..+6 dB, both floor to\n"
                  << "   silence at -60 dB; MIX 0 % = the clean tap (rack bypassed), 100 % = full chain.\n"
                  << "case                                   gain claim   measured gain   max |out - in x gain|\n";
        struct Fx { const char* name; std::vector<std::pair<const char*, float>> params; double claimDb; };
        const std::vector<Fx> fxCases = {
            { "defaults (neutral rack, MIX 100)", {}, 0.0 },
            { "MIX 0, every rack knob at 100", { { "mix", 0.0f }, { "warp", 100.0f }, { "flutter", 100.0f }, { "drive", 100.0f },
                                                 { "echo", 100.0f }, { "reverb", 100.0f }, { "width", 100.0f } }, 0.0 },
            { "INPUT -6 dB", { { "input", -6.0f } }, -6.0 },
            { "INPUT +6 dB", { { "input", 6.0f } }, 6.0 },
            { "OUTPUT -6 dB", { { "output", -6.0f } }, -6.0 },
            { "OUTPUT +6 dB", { { "output", 6.0f } }, 6.0 },
            { "OUTPUT -60 dB", { { "output", -60.0f } }, -1000.0 },
            { "INPUT -60 dB", { { "input", -60.0f } }, -1000.0 },
        };
        const int ln = loop.getNumSamples();
        for (const auto& c : fxCases)
        {
            LoopNestProcessor p;
            p.setFxMode(true);
            for (const auto& [id, v] : c.params) setNatural(p, id, v);
            p.prepareToPlay(kSR, 512);
            int pos = 0;
            const auto o = render(p, 8.0, &loop, &pos);
            p.releaseResources();
            double eo = 0.0, ei = 0.0;
            float d = 0.0f;
            const float g = c.claimDb < -500.0 ? 0.0f : juce::Decibels::decibelsToGain((float) c.claimDb);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = ln; i < 2 * ln; ++i)
                {
                    const float x = loop.getSample(ch, i - ln), y = o.getSample(ch, i);
                    eo += (double) y * y; ei += (double) x * x;
                    d = juce::jmax(d, std::abs(y - g * x));
                }
            const double gain = 10.0 * std::log10(juce::jmax(1.0e-300, eo) / ei);
            std::cout << std::left << std::setw(38) << c.name << std::right
                      << (c.claimDb < -500.0 ? std::string("    silence") : fx(c.claimDb, 8) + " dB")
                      << (eo == 0.0 ? std::string("        silence") : fx(gain, 12, 3) + " dB")
                      << std::setw(22) << (d == 0.0f ? std::string("exact") : fx(juce::Decibels::gainToDecibels((double) d, -300.0), 7) + " dBFS")
                      << "\n";
        }
    }

    void pitchReport(const juce::File& root)
    {
        std::cout << "\n== effects: PITCH through the real processor (AUDITION, SYNC off). A 441 Hz sine loop (closes\n"
                  << "   on itself, 44.1 kHz file); mean frequency over seconds 1..3 of playback, cents off the claim\n"
                  << "   claim (PluginProcessor.cpp parameter layout pitch, renderAudition; CLAUDE.md): -12..+12 st varispeed, also corrects\n"
                  << "   file/host rate.\n"
                  << "host kHz   pitch st   claim Hz    measured Hz   error cents\n";
        const int n = (int) (kSR * 4.0);
        juce::AudioBuffer<float> sig(2, n);
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.5f * (float) std::sin(kTwoPi * 441.0 * i / kSR);
            sig.setSample(0, i, v);
            sig.setSample(1, i, v);
        }
        const auto file = writeWav(root.getChildFile("pitch-sine.wav"), sig);
        auto start = [&] (LoopNestProcessor& p, double hostSR, float glide)
        {
            p.setSyncEnabled(false);
            p.prepareToPlay(hostSR, 512);
            loopnest::Snapshot s = p.capture();
            s.file = file;
            p.restore(s, loopnest::Snapshot::kFile);
            p.finishPendingLoad(5000);
            p.setSyncEnabled(false);
            setNatural(p, "pitchGlide", glide);
            p.setLooping(true);
            p.setPlaying(true);
        };
        for (double hostSR : { 44100.0, 48000.0 })
            for (float st : { -12.0f, -7.0f, -1.0f, 0.0f, 1.0f, 7.0f, 12.0f })
            {
                if (hostSR != kSR && st != -12.0f && st != 0.0f && st != 12.0f) continue;
                LoopNestProcessor p;
                start(p, hostSR, 0.0f);
                setNatural(p, "pitch", st);
                const auto o = renderN(p, (int) (3.0 * hostSR));
                p.releaseResources();
                const double claim = 441.0 * std::pow(2.0, st / 12.0);
                const double f = meanFreq(o.getReadPointer(0), (int) hostSR, (int) (2.0 * hostSR) - 1, hostSR);
                std::cout << nx(hostSR / 1000.0, 8, 1) << fx(st, 10, 0) << nx(claim, 12, 2) << nx(f, 14, 3)
                          << fx(cents(f / claim), 13, 2) << "\n";
            }

        std::cout << "\n   GLIDE: pitch 0 -> +12 st while playing; time until the rate is 90 % of the way (f >= 1.9 x 441 Hz)\n"
                  << "   claim (PluginProcessor.cpp parameter layout pitchGlide, renderAudition kGlideMaxSec): 0 % = instant; tau = glide^2 x 1.5 s, so 90 % at 2.30 tau\n"
                  << "glide    claim s   measured s\n";
        for (float g : { 0.0f, 25.0f, 50.0f, 100.0f })
        {
            LoopNestProcessor p;
            start(p, kSR, g);
            renderN(p, (int) kSR);                                     // 1 s at pitch 0
            setNatural(p, "pitch", 12.0f);
            const auto o = renderN(p, (int) (7.0 * kSR));
            p.releaseResources();
            const auto tr = trackFreq(o.getReadPointer(0), 0, o.getNumSamples(), kSR);
            double t90 = -1.0;
            for (size_t i = 0; i < tr.t.size() && t90 < 0.0; ++i)
                if (tr.f[i] >= 1.9 * 441.0) t90 = tr.t[i];
            const double tau = (g * 0.01) * (g * 0.01) * kClaimGlideMaxSec;
            std::cout << std::setw(4) << (int) g << " %" << nx(tau * std::log(10.0), 10, 3) << nx(t90, 12, 3) << "\n";
        }
    }

    // One short row per effect on a real loop: each at 100 % against dry.
    void realLoopRows(const juce::AudioBuffer<float>& src, const juce::String& name)
    {
        const int n = src.getNumSamples();
        const double dry = loudness(src, 0, n), dryMono = loudness(monoSum(src), 0, n);
        const Image in = imageOf(src, 0, n);
        std::cout << "\n" << name << "  (peak " << fx(peakDb(src, 0, n)) << " dBFS, L/R corr " << nx(in.corr, 5, 3) << ")\n"
                  << "effect at 100 %      loudness    peak     mono sum    S/M\n";
        struct Row { const char* name; std::function<void(CP&)> set; };
        for (const Row& r : { Row { "WARP", [] (CP& p) { p.warp = 100; } }, Row { "FLUTTER", [] (CP& p) { p.flutter = 100; } },
                              Row { "DRIVE", [] (CP& p) { p.drive = 100; } }, Row { "ECHO", [] (CP& p) { p.echo = 100; } },
                              Row { "REVERB PLATE", [] (CP& p) { p.reverb = 100; p.reverbType = 0; } },
                              Row { "REVERB HALL", [] (CP& p) { p.reverb = 100; p.reverbType = 1; } },
                              Row { "REVERB SPRING", [] (CP& p) { p.reverb = 100; p.reverbType = 2; } },
                              Row { "WIDTH", [] (CP& p) { p.width = 100; } },
                              Row { "CENTER +100", [] (CP& p) { p.widthMid = 100; } } })
        {
            CP p = neutral();
            r.set(p);
            const auto o = runChain(src, p);
            const Image im = imageOf(o, 0, n);
            std::cout << std::left << std::setw(16) << r.name << std::right << fx(loudness(o, 0, n) - dry, 9) << fx(peakDb(o, 0, n) - peakDb(src, 0, n), 8)
                      << fx(loudness(monoSum(o), 0, n) - dryMono, 11) << fx(im.sideMid - in.sideMid, 8) << "  dB\n";
        }
        // TONE alone: DRIVE 100 with TONE at -100 / +100 against DRIVE 100 at TONE 0.
        CP flatP = neutral();
        flatP.drive = 100;
        const auto flat = runChain(src, flatP);
        const double flatL = loudness(flat, 0, n), flatMono = loudness(monoSum(flat), 0, n);
        const Image flatIm = imageOf(flat, 0, n);
        for (float tone : { -100.0f, 100.0f })
        {
            CP p = flatP;
            p.driveTone = tone;
            const auto o = runChain(src, p);
            const Image im = imageOf(o, 0, n);
            std::cout << "TONE " << std::left << std::setw(5) << (tone < 0 ? "-100" : "+100") << " vs 0  " << std::right
                      << fx(loudness(o, 0, n) - flatL, 7) << fx(peakDb(o, 0, n) - peakDb(flat, 0, n), 8)
                      << fx(loudness(monoSum(o), 0, n) - flatMono, 11) << fx(im.sideMid - flatIm.sideMid, 8) << "  dB  (DRIVE 100)\n";
        }
    }

    void effectsReport(const juce::File& root, int argc, char** argv, int firstFile)
    {
        warpFlutterReport();
        driveReport();
        echoReport();
        reverbReport();
        widthReport();
        eqReport();
        zipperReport();
        reverbTypeReport();
        sampleRateReport();
        neutralReport();
        pitchReport(root);

        std::cout << "\n== effects: real loops, each effect at 100 % against dry (dB). Loudness and mono sum K-weighted;\n"
                  << "   S/M = change in side-over-mid energy. Steady state: third pass of the loop.\n";
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        for (int i = firstFile; i < argc; ++i)
        {
            const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[i]));
            std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(f) };
            if (r == nullptr || r->sampleRate != kSR || r->lengthInSamples < 1000)
            {
                std::cout << "\n" << f.getFileName() << ": skipped (unreadable or not 44.1 kHz)\n";
                continue;
            }
            juce::AudioBuffer<float> buf(2, (int) r->lengthInSamples);
            r->read(&buf, 0, (int) r->lengthInSamples, 0, true, true);
            if (r->numChannels == 1) buf.copyFrom(1, 0, buf, 0, 0, buf.getNumSamples());
            realLoopRows(buf, f.getFileName());
        }
    }
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // MessageManager for the loader's AsyncUpdater

    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("LoopNestMeasure-" + juce::String(juce::Time::currentTimeMillis()));
    const juce::ScopeGuard cleanup { [root] { root.deleteRecursively(); } };
    setenv("LOOPNEST_PRINTS_DIR", root.getChildFile("prints").getFullPathName().toRawUTF8(), 1);

    if (argc > 1 && juce::String(argv[1]) == "stage")    // just the shared stage's own numbers
    {
        stageReport();
        std::cout << std::endl;
        return 0;
    }
    if (argc > 1 && juce::String(argv[1]) == "parity")   // just the parity section
    {
        driveSettleReport();
        matchOthersReport(root);
        seamReport(root);
        parityReport(root);
        std::cout << std::endl;
        return 0;
    }
    if (argc > 1 && juce::String(argv[1]) == "seam")     // the print seam: the sine, then the files given
    {
        seamReport(root);
        seamFilesReport(root, argc, argv, 2);
        std::cout << std::endl;
        return 0;
    }
    if (argc > 1 && juce::String(argv[1]) == "syncstart") // SYNC start cost (C9)
    {
        syncStartReport(root);
        std::cout << std::endl;
        return 0;
    }
    if (argc > 2 && juce::String(argv[1]) == "drivefiles") // drivefiles <files>: DRIVE level + match on real loops
    {
        driveFilesReport(argc, argv, 2);
        std::cout << std::endl;
        return 0;
    }
    if (argc > 1 && juce::String(argv[1]) == "drivecpu")  // DRIVE's per-frame cost on the bare chain
    {
        driveCpuReport();
        std::cout << std::endl;
        return 0;
    }
    if (argc > 1 && juce::String(argv[1]) == "printtime") // a long-tail wet print's render time
    {
        printTimeReport(root);
        std::cout << std::endl;
        return 0;
    }
    if (argc > 2 && juce::String(argv[1]) == "dump")     // dump <out dir> <files>: dry and DRIVE 100 % wavs
    {
        const juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[2]));
        outDir.createDirectory();
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        for (int i = 3; i < argc; ++i)
        {
            const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[i]));
            std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(f) };
            if (r == nullptr || r->sampleRate != kSR || r->lengthInSamples < 1000)
                continue;
            juce::AudioBuffer<float> buf(2, (int) r->lengthInSamples);
            r->read(&buf, 0, (int) r->lengthInSamples, 0, true, true);
            if (r->numChannels == 1) buf.copyFrom(1, 0, buf, 0, 0, buf.getNumSamples());
            CP p = neutral();
            p.drive = 100.0f;
            writeWav(outDir.getChildFile(f.getFileNameWithoutExtension() + "-dry.wav"), buf);
            writeWav(outDir.getChildFile(f.getFileNameWithoutExtension() + "-drive100.wav"), runChain(buf, p));
        }
        return 0;
    }
    if (argc > 2 && juce::String(argv[1]) == "typegains") // typegains <files>: REVERB TYPE levels on real loops (D-M2)
    {
        typeGainsReport(argc, argv, 2);
        std::cout << std::endl;
        return 0;
    }
    if (argc > 1 && juce::String(argv[1]) == "effects")  // just the effects audit
    {
        effectsReport(root, argc, argv, 2);
        std::cout << std::endl;
        return 0;
    }

    chainReport(drumLoop(-6.0), "synthetic drum loop");

    // Real loops named on the command line: read, never written. Resampling is not
    // done here, so a file is measured at its own rate through a chain prepared at 44.1 kHz
    // only when it is 44.1 kHz; others are skipped.
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    for (int i = 1; i < argc; ++i)
    {
        const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[i]));
        std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(f) };
        if (r == nullptr || r->sampleRate != kSR || r->lengthInSamples < 1000)
        {
            std::cout << "\n" << f.getFileName() << ": skipped (unreadable or not 44.1 kHz)\n";
            continue;
        }
        juce::AudioBuffer<float> buf(2, (int) r->lengthInSamples);
        r->read(&buf, 0, (int) r->lengthInSamples, 0, true, true);
        if (r->numChannels == 1) buf.copyFrom(1, 0, buf, 0, 0, buf.getNumSamples());
        chainReport(buf, f.getFileName());
    }

    driveVsLevel();
    delayReport();
    aliasReport();
    driveFoldReport();
    driveResponseReport();
    matchReport(root);
    stageReport();
    fxReport();
    driveSettleReport();
    matchOthersReport(root);
    seamReport(root);
    parityReport(root);
    std::cout << std::endl;
    return 0;
}
