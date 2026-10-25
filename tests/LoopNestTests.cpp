// v2 stage 0: console tests for the processor's restore path and saved state.
// Snapshot capture/restore round-trips, parts masks, one-undo-step gestures, CLEAR,
// the async loader (spin trim reset, supersede, failed decode) and get/setState
// (stateVersion, bank-relative paths, legacy states, missing samples). The bar-grid
// lock drives processBlock from a fake host transport and times each beat's tick.
//
// Runs headless: no host, no editor. The processor's settings/Prints dirs follow
// JucePlugin_Name ("LoopNest Tests"), and nothing here calls setSampleFolder or
// renderLoop, so the plugin's own settings and Prints are never touched. Test audio
// lives in a temp folder that is deleted at exit. The UiBridge HATCH tests do print:
// LOOPNEST_PRINTS_DIR (honoured under LOOPNEST_PRINTS_ENV) points renderLoop into it.
#include "../Source/TimeStretch.h"   // first: Accelerate's headers clash with JUCE's names after JuceHeader
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/UiBridge.h"
#include "../Source/FactoryPresets.h"
#include "../Source/BankDrop.h"
#include <complex>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <sys/stat.h>

namespace
{
    int failures = 0, checks = 0;

    void check(bool ok, const juce::String& what, int line)
    {
        ++checks;
        if (! ok)
        {
            ++failures;
            std::cout << "  FAIL (line " << line << "): " << what << std::endl;
        }
    }
    #define CHECK(cond) check((cond), #cond, __LINE__)

    bool near(float a, float b, float tol = 1.0e-3f) { return std::abs(a - b) <= tol; }

    bool sameSound(const loopnest::Snapshot& a, const loopnest::Snapshot& b, bool withFile = true)
    {
        if (withFile && a.file != b.file) return false;
        if (! near(a.start, b.start, 1.0e-4f) || ! near(a.end, b.end, 1.0e-4f)) return false;
        if (! near(a.pitch, b.pitch) || ! near(a.input, b.input) || ! near(a.output, b.output)
            || ! near(a.mix, b.mix))
            return false;
        for (int i = 0; i < loopnest::CharacterParams::kNum; ++i)
            if (! near(a.character[i], b.character[i]))
                return false;
        return a.character.on == b.character.on;
    }

    // --- panel power helpers ---
    using CP = loopnest::CharacterParams;

    // Every chain control at its layout default (all neutral, all panels ON).
    CP neutralChain()
    {
        CP c;
        c.warpRate = 40; c.flutterRate = 50; c.echoHi = 1; c.reverbHi = 1; c.eqHi = 1;
        c.widthHi = 1; c.reverbDecay = 50; c.echoTime = 44; c.reverbType = 1;   // HALL
        return c;
    }

    // Stage `s` at a strong, non-neutral setting: primary AND its secondaries / band / tertiary.
    void setHeavy(CP& c, int s)
    {
        switch (s)
        {
            case CP::kWarp:    c.warp = 60; c.warpRate = 70; break;
            case CP::kFlutter: c.flutter = 70; c.flutterRate = 80; break;
            case CP::kDrive:   c.drive = 75; c.driveTone = 60; break;
            case CP::kEcho:    c.echo = 80; c.echoLo = 0.2f; c.echoHi = 0.8f; c.echoTime = 20; break;
            case CP::kReverb:  c.reverb = 70; c.reverbLo = 0.2f; c.reverbHi = 0.85f; c.reverbDecay = 80; break;
            case CP::kWidth:   c.width = 70; c.widthLo = 0.2f; c.widthHi = 0.8f; c.widthMid = 50; break;
            default: break;
        }
    }

    // Stage `s` dialled to its neutral by hand (what OFF must equal).
    void setNeutral(CP& c, int s)
    {
        switch (s)
        {
            case CP::kWarp:    c.warp = 0; break;
            case CP::kFlutter: c.flutter = 0; break;
            case CP::kDrive:   c.drive = 0; break;
            case CP::kEcho:    c.echo = 0; break;
            case CP::kReverb:  c.reverb = 0; break;
            case CP::kWidth:   c.width = 0; c.widthMid = 0; break;
            default: break;
        }
    }

    // Stereo noise, L and R decorrelated (so WIDTH has a side to work on).
    juce::AudioBuffer<float> noise(int n, int seed, float amp = 0.4f)
    {
        juce::Random r(seed);
        juce::AudioBuffer<float> b(2, n);
        for (int i = 0; i < n; ++i)
        {
            b.setSample(0, i, amp * (r.nextFloat() * 2.0f - 1.0f));
            b.setSample(1, i, amp * (r.nextFloat() * 2.0f - 1.0f));
        }
        return b;
    }

    // Runs `in` through `ch` frame by frame, setParams once per 512-frame block as
    // the processor does. Returns the output.
    juce::AudioBuffer<float> runChain(CharacterChain& ch, const CP& p, const juce::AudioBuffer<float>& in)
    {
        juce::AudioBuffer<float> out(2, in.getNumSamples());
        for (int i = 0; i < in.getNumSamples(); ++i)
        {
            if (i % 512 == 0) ch.setParams(p);
            float x[2] = { in.getSample(0, i), in.getSample(1, i) };
            ch.process(x, 2);
            out.setSample(0, i, x[0]); out.setSample(1, i, x[1]);
        }
        return out;
    }

    // `in` latency frames late (the chain's fixed delay, ruling 3): silence first, its
    // last `latency` frames cut. What a neutral chain must equal exactly.
    juce::AudioBuffer<float> delayed(const juce::AudioBuffer<float>& in, int latency)
    {
        juce::AudioBuffer<float> out(in.getNumChannels(), in.getNumSamples());
        out.clear();
        for (int ch = 0; ch < in.getNumChannels(); ++ch)
            if (latency < in.getNumSamples())
                out.copyFrom(ch, latency, in, ch, 0, in.getNumSamples() - latency);
        return out;
    }

    // The chain's fixed delay at a rate (frames).
    int latencyAt(double sr)
    {
        CharacterChain c;
        c.prepare(sr);
        return c.latency();
    }

    // `next` as a chain fed `prev` then `next` plays it at the fixed delay: frame i is
    // next[i - latency], or prev's tail for the first `latency` frames.
    juce::AudioBuffer<float> delayedAfter(const juce::AudioBuffer<float>& prev, const juce::AudioBuffer<float>& next, int latency)
    {
        juce::AudioBuffer<float> out = delayed(next, latency);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < juce::jmin(latency, out.getNumSamples()); ++i)
            {
                const int k = prev.getNumSamples() + i - latency;
                out.setSample(ch, i, k >= 0 ? prev.getSample(ch, k) : 0.0f);
            }
        return out;
    }

    // Frames [from, n) of a and b identical, bit for bit.
    bool identical(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int from = 0)
    {
        if (a.getNumSamples() != b.getNumSamples()) return false;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = from; i < a.getNumSamples(); ++i)
                if (std::memcmp(a.getReadPointer(ch, i), b.getReadPointer(ch, i), sizeof(float)) != 0
                    && ! (a.getSample(ch, i) == 0.0f && b.getSample(ch, i) == 0.0f))   // +0 vs -0 is still silence
                    return false;
        return true;
    }

    float peakFrom(const juce::AudioBuffer<float>& b, int from)
    {
        float pk = 0.0f;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = from; i < b.getNumSamples(); ++i)
                pk = juce::jmax(pk, std::abs(b.getSample(ch, i)));
        return pk;
    }

    bool powerIs(LoopNestProcessor& p, int s, bool on)
    {
        auto* q = p.apvts.getParameter(CP::powerIds()[(size_t) s]);
        return q != nullptr && (q->getValue() > 0.5f) == on;
    }
    bool allPower(LoopNestProcessor& p, bool on)
    {
        for (int s = 0; s < CP::kNumStages; ++s)
            if (! powerIs(p, s, on)) return false;
        return true;
    }
    void setPower(LoopNestProcessor& p, int s, bool on)
    {
        if (auto* q = p.apvts.getParameter(CP::powerIds()[(size_t) s]))
            q->setValueNotifyingHost(on ? 1.0f : 0.0f);
    }
    void setNatural(LoopNestProcessor& p, const char* id, float v)
    {
        if (auto* q = p.apvts.getParameter(id))
            q->setValueNotifyingHost(q->convertTo0to1(v));
    }

    // A non-default sound on every param's grid (integers for %, 0.01 st, 0.1 dB).
    loopnest::Snapshot sampleSound()
    {
        loopnest::Snapshot s;
        s.start = 0.25f; s.end = 0.75f; s.pitch = -3.5f;
        s.input = -4.5f; s.output = 2.0f; s.mix = 63.0f;
        auto& c = s.character;
        c.warp = 12; c.warpRate = 70; c.drive = 44; c.driveTone = -30; c.flutter = 21;
        c.flutterRate = 80; c.echo = 35; c.reverb = 18; c.echoLo = 0.3f; c.echoHi = 0.8f;
        c.reverbLo = 0.33f; c.reverbHi = 0.9f; c.eqLo = 0.05f; c.eqHi = 0.7f; c.width = 40;
        c.widthLo = 0.12f; c.widthHi = 0.95f; c.reverbDecay = 66; c.echoTime = 25; c.widthMid = -15;
        c.reverbType = 2;                                   // SPRING
        return s;
    }

    juce::File writeWav(const juce::File& f, double seconds, float freq, float amp = 0.5f)
    {
        f.getParentDirectory().createDirectory();
        const int n = (int) (44100.0 * seconds);
        juce::AudioBuffer<float> buf(2, n);
        for (int i = 0; i < n; ++i)
        {
            const float v = amp * std::sin(juce::MathConstants<float>::twoPi * freq * (float) i / 44100.0f);
            buf.setSample(0, i, v);
            buf.setSample(1, i, v);
        }
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream(f.createOutputStream());
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                      .withSampleRate(44100.0)
                                                      .withNumChannels(2)
                                                      .withBitsPerSample(24));
        writer->writeFromAudioSampleBuffer(buf, 0, n);
        return f;
    }

    // A state blob that names a folder and a sample, as a v1 (unversioned) or v2 host would.
    juce::MemoryBlock stateBlob(LoopNestProcessor& p, const juce::File& folder, const juce::File& sample,
                                const juce::String& rel, int version)
    {
        auto state = p.apvts.copyState();
        if (version > 0) state.setProperty("stateVersion", version, nullptr);
        state.setProperty("folder", folder.getFullPathName(), nullptr);
        state.setProperty("sample", sample.getFullPathName(), nullptr);
        if (rel.isNotEmpty()) state.setProperty("sampleRel", rel, nullptr);
        juce::MemoryBlock mb;
        if (auto xml = state.createXml())
            juce::AudioProcessor::copyXmlToBinary(*xml, mb);
        return mb;
    }

    std::unique_ptr<juce::XmlElement> stateXml(LoopNestProcessor& p)
    {
        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        return juce::AudioProcessor::getXmlFromBinary(mb.getData(), (int) mb.getSize());
    }

    // Records the host-facing event order: B(egin) / V(alue) / E(nd), and per event
    // the parameter index, so the one-step shape can be checked per param.
    struct GestureLog : juce::AudioProcessorListener
    {
        juce::String events;
        std::vector<std::pair<char, int>> seq;
        void audioProcessorParameterChanged(juce::AudioProcessor*, int i, float) override { events << "V"; seq.emplace_back('V', i); }
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
        void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int i) override { events << "B"; seq.emplace_back('B', i); }
        void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int i) override { events << "E"; seq.emplace_back('E', i); }
        void clear() { events.clear(); seq.clear(); }

        // Every touched param gets exactly one B, V, E; all B precede any V, all V
        // precede any E (one overlapping gesture), and the B/V/E sets name the same params.
        bool isOneStep() const
        {
            std::map<int, std::array<int, 3>> count;
            int phase = 0;
            for (auto [ev, i] : seq)
            {
                const int k = ev == 'B' ? 0 : ev == 'V' ? 1 : 2;
                if (k < phase) return false;
                phase = k;
                ++count[i][(size_t) k];
            }
            for (auto& [i, c] : count)
                if (c[0] != 1 || c[1] != 1 || c[2] != 1) return false;
            return true;
        }
        int paramsTouched() const
        {
            std::set<int> s;
            for (auto [ev, i] : seq) s.insert(i);
            return (int) s.size();
        }
    };

    // A valid WAV header whose payload was cut short (a half-copied file).
    juce::File writeTruncatedWav(const juce::File& f, const juce::File& from)
    {
        juce::MemoryBlock mb;
        from.loadFileAsData(mb);
        f.getParentDirectory().createDirectory();
        f.replaceWithData(mb.getData(), mb.getSize() / 2);
        return f;
    }

    bool audible(LoopNestProcessor& p, int blocks = 20)
    {
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        for (int i = 0; i < blocks; ++i)
        {
            buf.clear();
            p.processBlock(buf, midi);
            peak = juce::jmax(peak, buf.getMagnitude(0, 512));
        }
        return peak > 0.1f;
    }

    struct FnThread : juce::Thread
    {
        std::function<void()> fn;
        FnThread(const juce::String& n, std::function<void()> f) : juce::Thread(n), fn(std::move(f)) {}
        void run() override { fn(); }
    };

    bool waitForDecode(LoopNestProcessor& p, int ms = 5000)
    {
        for (int t = 0; t < ms && ! p.hasDecodedLoadWaiting(); t += 5)
            juce::Thread::sleep(5);
        return p.hasDecodedLoadWaiting();
    }

    void loadVia(LoopNestProcessor& p, const juce::File& f)
    {
        loopnest::Snapshot s = p.capture();
        s.file = f;
        p.restore(s, loopnest::Snapshot::kFile);
        p.finishPendingLoad(5000);
    }

    // --- bar-grid lock helpers ---

    // A host transport the test moves by hand: tempo, ppq, bar start, playing.
    // Each advance() moves ppq by one block at the current tempo, as a host does.
    struct FakeHost : juce::AudioPlayHead
    {
        double bpm = 120.0, ppq = 0.0, sr = 44100.0;
        int num = 4, den = 4;
        bool playing = false;
        bool rawBar = false; double bar = 0.0;   // report `bar` as the bar start, as given
        bool noMeter = false;                    // bar starts (of num/den) but no time signature
        // The plugin's reported latency (runHost reads it). A host's delay compensation
        // takes it back off: a tick due at host frame t is heard at output frame t + latency.
        int latency = 0;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm(bpm);
            if (! noMeter)
                info.setTimeSignature(TimeSignature { num, den });
            info.setPpqPosition(ppq);
            const double barLen = (double) num * 4.0 / (double) den;
            info.setPpqPositionOfLastBarStart(rawBar ? bar : std::floor(ppq / barLen + 1.0e-9) * barLen);
            info.setIsPlaying(playing);
            return info;
        }
        void advance(int frames) { if (playing) ppq += (double) frames * bpm / (60.0 * sr); }
        // The clock each block saw (runHost), so a tick can be placed on the grid the
        // host actually reported, across jumps and tempo changes.
        struct Seen { double ppq, bpm; bool playing; };
        std::vector<Seen> log;
    };

    // A drum-ish loop: `beats` beats at `bpm`, a 3 kHz tick (2 ms decay) on every beat
    // at amplitude `amp`, over a -80 dB noise bed (a real loop is never digital
    // silence, and the stretcher passes true silence through on a path of its own).
    // `pickup` beats of bed come before the first tick (bar 1's anchor sits there).
    // `ramp` > 0: tick k at amp + k x ramp, so which beat of the loop plays is audible.
    juce::File writeClickLoop(const juce::File& f, double bpm, int beats, float amp, int seed = 1,
                              double pickup = 0.0, float ramp = 0.0f)
    {
        f.getParentDirectory().createDirectory();
        const double fpb = 44100.0 * 60.0 / bpm;
        const int n = (int) std::lround(fpb * (beats + pickup));
        juce::AudioBuffer<float> buf(2, n);
        juce::Random r(seed);
        for (int i = 0; i < n; ++i)
        {
            const float bed = 1.0e-4f * (r.nextFloat() * 2.0f - 1.0f);
            buf.setSample(0, i, bed);
            buf.setSample(1, i, bed);
        }
        for (int k = 0; k < beats; ++k)
        {
            const int at = (int) std::lround(fpb * (k + pickup));
            for (int i = 0; i < 400 && at + i < n; ++i)
            {
                const float v = (amp + ramp * (float) k) * std::exp(-(float) i / 88.0f)
                              * std::sin(juce::MathConstants<float>::twoPi * 3000.0f * (float) i / 44100.0f);
                buf.addSample(0, at + i, v);
                buf.addSample(1, at + i, v);
            }
        }
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream(f.createOutputStream());
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                      .withSampleRate(44100.0)
                                                      .withNumChannels(2)
                                                      .withBitsPerSample(24));
        writer->writeFromAudioSampleBuffer(buf, 0, n);
        return f;
    }

    // Runs `blocks` blocks of `bs` frames, moving the host along; returns the output.
    // `each(b)` runs before block b (a spin, a jump, a tempo change).
    juce::AudioBuffer<float> runHost(LoopNestProcessor& p, FakeHost& h, int blocks, int bs = 512,
                                     const std::function<void(int)>& each = {})
    {
        juce::AudioBuffer<float> out(2, blocks * bs), buf(2, bs);
        juce::MidiBuffer midi;
        h.log.clear();
        h.latency = p.getLatencySamples();
        for (int b = 0; b < blocks; ++b)
        {
            if (each) each(b);
            h.log.push_back({ h.ppq, h.bpm, h.playing });
            buf.clear();
            p.processBlock(buf, midi);
            h.advance(bs);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, b * bs, buf, ch, 0, bs);
        }
        return out;
    }

    // FNV-1a over the output's float bits (both channels): a fingerprint for "unchanged".
    juce::uint64 hashAudio(const juce::AudioBuffer<float>& b)
    {
        juce::uint64 h = 1469598103934665603ull;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                float v = b.getSample(ch, i);
                if (v == 0.0f) v = 0.0f;   // -0 is silence too
                juce::uint32 bits; std::memcpy(&bits, &v, 4);
                for (int k = 0; k < 4; ++k) { h ^= (bits >> (8 * k)) & 0xff; h *= 1099511628211ull; }
            }
        return h;
    }

    // Where the tick nearest `at` (an output frame) lands: the peak of its 1 ms RMS
    // envelope within +-`win` frames. The source tick's own peak sits `tickPeak` frames
    // after its start, so the landing is reported as the tick's start. -1 = no tick.
    double tickAt(const juce::AudioBuffer<float>& out, double at, int win, float* level = nullptr)
    {
        const int n = out.getNumSamples(), w = 44;
        const int lo = juce::jmax(w / 2, (int) at - win), hi = juce::jmin(n - w / 2, (int) at + win);
        double best = 0.0; int bestI = -1;
        for (int i = lo; i < hi; ++i)
        {
            double e = 0.0;
            for (int k = -w / 2; k < w / 2; ++k)
                e += (double) out.getSample(0, i + k) * out.getSample(0, i + k);
            if (e > best) { best = e; bestI = i; }
        }
        if (level != nullptr) *level = (float) std::sqrt(best / w);
        return bestI < 0 || best < 1.0e-6 ? -1.0 : (double) bestI;
    }

    // Where tickAt finds a lone source tick that starts at frame 0 (its envelope peak).
    double tickPeakOffset()
    {
        juce::AudioBuffer<float> b(2, 4000);
        b.clear();
        for (int i = 0; i < 400; ++i)
        {
            const float v = 0.5f * std::exp(-(float) i / 88.0f)
                          * std::sin(juce::MathConstants<float>::twoPi * 3000.0f * (float) i / 44100.0f);
            b.setSample(0, 1000 + i, v);
        }
        return tickAt(b, 1000.0, 400) - 1000.0;
    }

    // Every beat's tick against the grid the host reported, in blocks [fromBlock,
    // toBlock) of a runHost output (`bs` frames a block). The grid loops' ticks sit
    // kGridPickup beats into the loop, so each beat's tick is due that far after it,
    // and is looked for in the block that plays it. Reports the worst and mean
    // offset (ms, + = late) and the beats with no tick; `levels` (if given) gets each
    // tick's level, `onBeat` the beat it belongs to.
    constexpr double kGridPickup = 0.5;
    struct Landing { double worstMs = 0.0, meanMs = 0.0; int ticks = 0, missing = 0; };
    Landing landings(const juce::AudioBuffer<float>& out, const FakeHost& h, int fromBlock, int toBlock,
                     int bs = 512, std::vector<float>* levels = nullptr, std::vector<double>* onBeat = nullptr)
    {
        static const double peak = tickPeakOffset();
        Landing r;
        double sum = 0.0;
        toBlock = juce::jmin(toBlock, (int) h.log.size());
        for (int k = fromBlock; k < toBlock; ++k)
        {
            const auto& seen = h.log[(size_t) k];
            if (! seen.playing)
                continue;
            const double fpb = 60.0 * 44100.0 / seen.bpm;
            const double span = (double) bs / fpb;
            for (double beat = std::ceil(seen.ppq - kGridPickup - 1.0e-9); beat + kGridPickup < seen.ppq + span - 1.0e-9; beat += 1.0)
            {
                const double at = (double) k * bs + (beat + kGridPickup - seen.ppq) * fpb + h.latency;
                if (at + peak + 0.25 * fpb >= (double) out.getNumSamples())
                    continue;
                float lv = 0.0f;
                const double got = tickAt(out, at + peak, (int) (0.25 * fpb), &lv);
                if (got < 0.0 || lv < 0.02f) { ++r.missing; continue; }
                const double ms = (got - peak - at) / 44.1;
                r.worstMs = juce::jmax(r.worstMs, std::abs(ms));
                sum += ms;
                ++r.ticks;
                if (levels != nullptr) levels->push_back(lv);
                if (onBeat != nullptr) onBeat->push_back(beat);
            }
        }
        r.meanMs = r.ticks > 0 ? sum / r.ticks : 0.0;
        return r;
    }
    void printLanding(const char* what, const Landing& l)
    {
        std::cout << "    " << what << ": " << l.ticks << " ticks, worst " << juce::String(l.worstMs, 3)
                  << " ms, mean " << juce::String(l.meanMs, 3) << " ms" << (l.missing > 0 ? ", MISSING " : "")
                  << (l.missing > 0 ? juce::String(l.missing) : juce::String()) << std::endl;
    }
    // The largest step between neighbouring output frames in [from, to): a hard cut of
    // a tone steps by up to its level; the test tones move < 0.02 a frame.
    float maxStepOf(const juce::AudioBuffer<float>& buf, int from, int to)
    {
        float m = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = juce::jmax(1, from); i < juce::jmin(to, buf.getNumSamples()); ++i)
                m = juce::jmax(m, std::abs(buf.getSample(ch, i) - buf.getSample(ch, i - 1)));
        return m;
    }

}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // MessageManager for the loader's AsyncUpdater

    // --hits <file>...: print the hits computeHits finds in real files (seconds), and stop.
    // For tuning the detector by ear and eye; reads the files, writes nothing.
    if (argc > 2 && juce::String(argv[1]) == "--hits")
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        for (int i = 2; i < argc; ++i)
        {
            const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[i]));
            std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(f) };
            if (r == nullptr) { std::cout << f.getFileName() << ": unreadable" << std::endl; continue; }
            juce::AudioBuffer<float> buf((int) juce::jlimit(1u, 2u, r->numChannels), (int) r->lengthInSamples);
            r->read(&buf, 0, (int) r->lengthInSamples, 0, true, buf.getNumChannels() > 1);
            const auto hits = UiBridge::computeHits(buf, r->sampleRate);
            std::cout << f.getFileName() << "  " << juce::String((double) r->lengthInSamples / r->sampleRate, 3)
                      << " s  " << hits.size() << " hits\n ";
            for (int h : hits) std::cout << " " << juce::String((double) h / r->sampleRate, 3);
            std::cout << std::endl;
        }
        return 0;
    }

    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("LoopNestTests-" + juce::String(juce::Time::currentTimeMillis()));
    const juce::ScopeGuard cleanup { [root] { root.deleteRecursively(); } };
    const auto bank = root.getChildFile("bank");
    const auto a = writeWav(bank.getChildFile("packA/loopA_90bpm.wav"), 1.0, 220.0f);
    const auto b = writeWav(bank.getChildFile("packB/loopB.wav"), 0.5, 440.0f);
    const auto c = writeWav(bank.getChildFile("loopC.wav"), 0.75, 330.0f);
    const auto corrupt = bank.getChildFile("zz_corrupt.wav");
    corrupt.replaceWithText("not a wav file");
    const auto outside = writeWav(root.getChildFile("elsewhere/outside.wav"), 0.5, 550.0f);
    setenv("LOOPNEST_PRINTS_DIR", root.getChildFile("prints").getFullPathName().toRawUTF8(), 1);

    using S = loopnest::Snapshot;

    std::cout << "capture: a fresh instance reads the layout defaults" << std::endl;
    {
        LoopNestProcessor p;
        const auto snap = p.capture();
        CHECK(snap.file == juce::File());
        for (int i = 0; i < loopnest::CharacterParams::kNum; ++i)
        {
            auto* param = p.apvts.getParameter(loopnest::CharacterParams::fields()[(size_t) i].id);
            CHECK(param != nullptr);
            if (param != nullptr)
                CHECK(near(snap.character[i], param->convertFrom0to1(param->getDefaultValue())));
        }
        CHECK(near(snap.start, 0.0f) && near(snap.end, 1.0f) && near(snap.mix, 100.0f));
    }

    std::cout << "restore: params round-trip, parts masks" << std::endl;
    {
        LoopNestProcessor p;
        const auto want = sampleSound();
        CHECK(p.restore(want, S::kAll & ~S::kFile));
        CHECK(sameSound(p.capture(), want, false));

        // kCharacter touches only the chain: trim, pitch and levels stay.
        auto other = want;
        for (int i = 0; i < loopnest::CharacterParams::kNum; ++i) other.character[i] = want.character[i];
        other.character.drive = 90; other.character.echo = 5;
        other.start = 0.0f; other.pitch = 7.0f; other.mix = 10.0f;
        p.restore(other, S::kCharacter);
        const auto got = p.capture();
        CHECK(near(got.character.drive, 90) && near(got.character.echo, 5));
        CHECK(near(got.start, want.start, 1.0e-4f) && near(got.pitch, want.pitch) && near(got.mix, want.mix));

        p.restore(other, S::kPitch | S::kLevels);
        CHECK(near(p.capture().pitch, 7.0f) && near(p.capture().mix, 10.0f));
        CHECK(near(p.capture().start, want.start, 1.0e-4f));
    }

    std::cout << "restore: one host undo step (begin-all, set-all, end-all)" << std::endl;
    {
        LoopNestProcessor p;
        GestureLog log;
        p.addListener(&log);
        const auto want = sampleSound();
        p.restore(want, S::kAll & ~S::kFile);
        const int n = log.events.length() / 3;
        CHECK(n == 27);   // 2 trim + pitch + 3 levels + 21 chain, all changed from default
        CHECK(log.events == juce::String::repeatedString("B", n) + juce::String::repeatedString("V", n)
                          + juce::String::repeatedString("E", n));
        CHECK(log.isOneStep() && log.paramsTouched() == 27);
        log.clear();
        p.restore(want, S::kAll & ~S::kFile);   // nothing changes → nothing sent
        CHECK(log.events.isEmpty());

        log.clear();
        p.resetAllParams();                     // CLEAR: also one step
        const int m = log.events.length() / 3;
        CHECK(m == 27);
        CHECK(log.isOneStep() && log.paramsTouched() == 27);
        CHECK(log.events == juce::String::repeatedString("B", m) + juce::String::repeatedString("V", m)
                          + juce::String::repeatedString("E", m));
        p.removeListener(&log);
        CHECK(sameSound(p.capture(), LoopNestProcessor().capture(), false));
    }

    std::cout << "CLEAR resets the audition-only params too" << std::endl;
    {
        LoopNestProcessor p;
        for (auto id : { "bypass", "gainMatch", "pitchGlide" })
            if (auto* q = p.apvts.getParameter(id)) q->setValueNotifyingHost(1.0f);
        p.resetAllParams();
        for (auto id : { "bypass", "gainMatch", "pitchGlide" })
            if (auto* q = p.apvts.getParameter(id)) CHECK(near(q->getValue(), q->getDefaultValue()));
    }

    std::cout << "async load: file + params land together" << std::endl;
    {
        LoopNestProcessor p;
        auto want = sampleSound();
        want.file = a;
        CHECK(p.restore(want));
        CHECK(p.isLoadPending());
        CHECK(p.getCurrentSampleFile() == juce::File());   // nothing lands before the decode
        CHECK(near(p.capture().start, 0.0f));               // …and nor do its params
        CHECK(p.finishPendingLoad(5000));
        CHECK(! p.isLoadPending());
        CHECK(sameSound(p.capture(), want));

        // Same file: params apply at once, no load.
        auto again = want;
        again.character.drive = 3;
        p.restore(again);
        CHECK(! p.isLoadPending() && near(p.capture().character.drive, 3));

        // A missing file is refused and changes nothing.
        auto gone = want;
        gone.file = bank.getChildFile("nope.wav");
        CHECK(! p.restore(gone));
        CHECK(p.getCurrentSampleFile() == a);

        // A file that won't decode: the loaded sample and its trim stay.
        auto bad = want;
        bad.file = corrupt;
        bad.start = 0.5f;
        CHECK(p.restore(bad));
        CHECK(p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == a && near(p.capture().start, want.start, 1.0e-4f));
    }

    std::cout << "async load: a newer request supersedes" << std::endl;
    {
        LoopNestProcessor p;
        auto s1 = sampleSound(); s1.file = a; s1.pitch = 1.0f;
        auto s2 = sampleSound(); s2.file = b; s2.pitch = 2.0f;
        auto s3 = sampleSound(); s3.file = c; s3.pitch = 3.0f;
        p.restore(s1); p.restore(s2); p.restore(s3);
        CHECK(p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == c && near(p.capture().pitch, 3.0f));
    }

    std::cout << "get/setState: round-trip, stateVersion, bank-relative path" << std::endl;
    {
        LoopNestProcessor p;
        p.setStateInformation(stateBlob(p, bank, juce::File(), {}, 1).getData(),
                              (int) stateBlob(p, bank, juce::File(), {}, 1).getSize());
        auto want = sampleSound();
        want.file = b;
        p.restore(want);
        CHECK(p.finishPendingLoad(5000));
        p.setSyncEnabled(false);
        p.setDryHatch(false);
        p.setLooping(false);

        auto xml = stateXml(p);
        CHECK(xml != nullptr);
        if (xml != nullptr)
        {
            CHECK(xml->getIntAttribute("stateVersion") == LoopNestProcessor::kStateVersion);
            CHECK(xml->getStringAttribute("sampleRel") == "packB/loopB.wav");
            CHECK(xml->getStringAttribute("folder") == bank.getFullPathName());
        }

        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        LoopNestProcessor q;
        q.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(q.isLoadPending());
        CHECK(q.finishPendingLoad(5000));
        CHECK(sameSound(q.capture(), want));
        CHECK(q.getSampleFolder() == bank && q.getSampleCount() == 4);   // a, b, c, corrupt
        CHECK(! q.isSyncEnabled() && ! q.isDryHatch() && ! q.isLooping());

        // Saving mid-load saves the file being loaded, not the one it replaces.
        auto next = want;
        next.file = c;
        q.restore(next);
        auto mid = stateXml(q);
        CHECK(mid != nullptr && mid->getStringAttribute("sample") == c.getFullPathName());
        q.finishPendingLoad(5000);

        // A sample outside the bank has no relative path.
        auto out = want;
        out.file = outside;
        q.restore(out);
        q.finishPendingLoad(5000);
        auto ox = stateXml(q);
        CHECK(ox != nullptr && ox->getStringAttribute("sampleRel").isEmpty());
    }

    std::cout << "setState: legacy (pre-v2) state loads by absolute path" << std::endl;
    {
        LoopNestProcessor p;
        const auto mb = stateBlob(p, bank, a, {}, 0);
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == a);
    }

    std::cout << "setState: a moved stash relinks through sampleRel" << std::endl;
    {
        LoopNestProcessor p;
        const auto movedAbs = root.getChildFile("old-location/packA/loopA_90bpm.wav");   // no longer exists
        const auto mb = stateBlob(p, bank, movedAbs, "packA/loopA_90bpm.wav", 1);
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == a);

        // Version 0 ignores a stray sampleRel: the migration is deterministic.
        LoopNestProcessor q;
        const auto mb0 = stateBlob(q, bank, movedAbs, "packA/loopA_90bpm.wav", 0);
        q.setStateInformation(mb0.getData(), (int) mb0.getSize());
        CHECK(q.finishPendingLoad(5000));
        CHECK(q.getCurrentSampleFile() == juce::File());
    }

    std::cout << "setState: a missing sample clears the loaded one and cancels loads" << std::endl;
    {
        LoopNestProcessor p;
        loadVia(p, a);
        CHECK(p.getCurrentSampleFile() == a);
        auto s = p.capture(); s.file = b;
        p.restore(s);                                        // in flight…
        const auto mb = stateBlob(p, bank, bank.getChildFile("gone.wav"), {}, 1);
        p.setStateInformation(mb.getData(), (int) mb.getSize());   // …then a restore with no sample
        CHECK(p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == juce::File() && ! p.hasSample());
    }

    std::cout << "setState: a sample that won't decode clears too" << std::endl;
    {
        LoopNestProcessor p;
        loadVia(p, a);
        const auto mb = stateBlob(p, bank, corrupt, {}, 1);
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == juce::File());
    }

    std::cout << "setState: a missing sample's reference is kept: saved back as it came, sampleMissing, nothing plays; a bank that holds it relinks it; a load replaces it (B4)" << std::endl;
    {
        // A set whose loop sits on a drive that isn't mounted.
        const auto gone = root.getChildFile("unmounted/Loops/kit_120bpm.wav");   // never written
        auto missingBlob = [&] (LoopNestProcessor& p)
        {
            auto st = p.apvts.copyState();
            st.setProperty("stateVersion", LoopNestProcessor::kStateVersion, nullptr);
            st.setProperty("folder", gone.getParentDirectory().getFullPathName(), nullptr);
            st.setProperty("sample", gone.getFullPathName(), nullptr);
            st.setProperty("sampleRel", "kit_120bpm.wav", nullptr);
            st.setProperty("loopBpm", 120.0, nullptr);
            st.setProperty("loopBpmSrc", "name", nullptr);
            st.setProperty("barOrigin", 0.125, nullptr);
            st.getChildWithProperty("id", "start").setProperty("value", 0.25f, nullptr);
            juce::MemoryBlock mb;
            if (auto xml = st.createXml()) juce::AudioProcessor::copyXmlToBinary(*xml, mb);
            return mb;
        };
        auto savedAsCame = [&] (LoopNestProcessor& p)
        {
            const auto x = stateXml(p);
            return x != nullptr && x->getStringAttribute("sample") == gone.getFullPathName()
                && x->getStringAttribute("sampleRel") == "kit_120bpm.wav"
                && near((float) x->getDoubleAttribute("loopBpm"), 120.0f) && x->getStringAttribute("loopBpmSrc") == "name"
                && near((float) x->getDoubleAttribute("barOrigin"), 0.125f);
        };
        LoopNestProcessor p;
        p.prepareToPlay(44100.0, 512);
        loadVia(p, a);                                                // something loaded before the restore
        const auto mb = missingBlob(p);
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(p.finishPendingLoad(5000));
        CHECK(! p.hasSample() && p.getMissingSampleName() == "kit_120bpm.wav");
        UiBridge br(p);
        const auto st = br.state();
        CHECK(st["sampleMissing"].toString() == "kit_120bpm.wav" && ! (bool) st["hasSample"]);
        CHECK(savedAsCame(p));
        {
            // Saved again from a fresh instance: still the same reference.
            juce::MemoryBlock again;
            p.getStateInformation(again);
            LoopNestProcessor q;
            q.setStateInformation(again.getData(), (int) again.getSize());
            CHECK(q.finishPendingLoad(5000) && q.getMissingSampleName() == "kit_120bpm.wav" && savedAsCame(q));
        }
        // Nothing plays while it's missing.
        p.setPlaying(true);
        CHECK(! audible(p, 20));
        juce::AudioBuffer<float> blk(2, 512);
        juce::MidiBuffer midi;
        float pk = 0.0f;
        for (int i = 0; i < 20; ++i) { blk.clear(); p.processBlock(blk, midi); pk = juce::jmax(pk, blk.getMagnitude(0, 512)); }
        CHECK(pk == 0.0f);
        // A bank that doesn't hold it: still missing, still saved as it came.
        p.setSampleFolder(bank);
        p.finishPendingLoad(5000);
        CHECK(! p.hasSample() && p.getMissingSampleName() == "kit_120bpm.wav" && savedAsCame(p));
        // The stash turns up under another folder: picking it as the bank relinks the loop,
        // with its saved tempo and bar 1, the restored trim kept.
        const auto moved = root.getChildFile("remounted");
        const auto back = writeWav(moved.getChildFile("kit_120bpm.wav"), 2.0, 220.0f);
        p.setSampleFolder(moved);
        CHECK(p.finishPendingLoad(5000));
        LoopNestProcessor::BpmSource src = LoopNestProcessor::BpmSource::none;
        CHECK(p.getCurrentSampleFile() == back && p.getMissingSampleName().isEmpty());
        CHECK(near((float) p.getNativeBpm(src), 120.0f) && src == LoopNestProcessor::BpmSource::name);
        CHECK(near((float) p.getBarOrigin(), 0.125f) && near(p.capture().start, 0.25f, 1.0e-4f));
        CHECK(stateXml(p)->getStringAttribute("sample") == back.getFullPathName());
        CHECK(br.state()["sampleMissing"].toString().isEmpty() && (bool) br.state()["hasSample"]);
        // Any loop that lands replaces the reference.
        {
            LoopNestProcessor q;
            q.setStateInformation(mb.getData(), (int) mb.getSize());
            CHECK(q.finishPendingLoad(5000) && q.getMissingSampleName().isNotEmpty());
            loadVia(q, c);
            CHECK(q.getCurrentSampleFile() == c && q.getMissingSampleName().isEmpty());
            CHECK(stateXml(q)->getStringAttribute("sample") == c.getFullPathName());
        }
        // A set saved with no sample at all keeps none.
        {
            LoopNestProcessor q;
            const auto none = stateBlob(q, bank, juce::File(), {}, LoopNestProcessor::kStateVersion);
            q.setStateInformation(none.getData(), (int) none.getSize());
            CHECK(q.finishPendingLoad(5000) && q.getMissingSampleName().isEmpty());
        }
    }

    std::cout << "spin: the trim reset lands with the new file" << std::endl;
    {
        LoopNestProcessor p;
        const auto mb = stateBlob(p, bank, a, {}, 1);
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(p.finishPendingLoad(5000));
        auto trimmed = p.capture();
        trimmed.start = 0.3f; trimmed.end = 0.6f;
        p.restore(trimmed, S::kTrim);

        int moved = 0;
        for (int i = 0; i < 12; ++i)
        {
            const auto before = p.getCurrentSampleFile();
            const float startBefore = p.capture().start;
            p.spin();
            CHECK(near(p.capture().start, startBefore, 1.0e-4f));   // not before the swap
            CHECK(p.finishPendingLoad(5000));
            const auto after = p.getCurrentSampleFile();
            if (after != before)
            {
                ++moved;
                CHECK(near(p.capture().start, 0.0f) && near(p.capture().end, 1.0f));
            }
            else
            {
                CHECK(after != corrupt);   // the corrupt pick fails and leaves the file
            }
        }
        CHECK(moved > 0);
        CHECK(p.getCurrentSampleFile() != corrupt);
    }

    std::cout << "audio: the installed sample plays (AUDITION, no host)" << std::endl;
    {
        LoopNestProcessor p;
        p.setSyncEnabled(false);
        p.prepareToPlay(44100.0, 512);
        loadVia(p, a);
        p.setPlaying(true);
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        for (int i = 0; i < 20; ++i)
        {
            buf.clear();
            p.processBlock(buf, midi);
            peak = juce::jmax(peak, buf.getMagnitude(0, 512));
        }
        CHECK(peak > 0.1f);

        // A restore whose sample is still decoding keeps the audition silent (once what
        // the old loop had in the chain, its fixed delay, has come out).
        auto mb = stateBlob(p, bank, b, {}, 1);
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        p.setPlaying(true);
        if (p.isLoadPending())
        {
            const int lat = p.getLatencySamples();
            juce::AudioBuffer<float> after(2, lat + 512);
            for (int at = 0; at < after.getNumSamples(); at += 512)
            {
                buf.clear();
                p.processBlock(buf, midi);
                for (int ch = 0; ch < 2; ++ch)
                    after.copyFrom(ch, at, buf, ch, 0, juce::jmin(512, after.getNumSamples() - at));
            }
            CHECK(lat > 0 && peakFrom(after, lat) == 0.0f);
        }
        CHECK(p.finishPendingLoad(5000));
        p.releaseResources();
    }


    // --- Codex stage-0 review regressions (docs/stage0-review-codex-2026-09-29.md) ---

    std::cout << "F1: a host restore supersedes a decoded, not-yet-installed load" << std::endl;
    {
        LoopNestProcessor p;
        loadVia(p, a);
        auto sb = sampleSound(); sb.file = b; sb.pitch = 7.0f;
        p.restore(sb);
        CHECK(waitForDecode(p));                 // B decoded, install not yet run
        LoopNestProcessor ref;                   // C: c at pitch -5
        ref.apvts.getParameter("pitch")->setValueNotifyingHost(ref.apvts.getParameter("pitch")->convertTo0to1(-5.0f));
        const auto mb = stateBlob(ref, bank, c, {}, 1);
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == c && near(p.capture().pitch, -5.0f));
    }

    std::cout << "F1/F2: host restores on another thread vs message-thread installs (stress)" << std::endl;
    {
        // The host thread's setState(C) races this thread installing an older B.
        // Whatever the interleave, C was requested last: C's file and params win,
        // nothing stays pending, and the mute releases.
        LoopNestProcessor ref;
        ref.apvts.getParameter("pitch")->setValueNotifyingHost(ref.apvts.getParameter("pitch")->convertTo0to1(-5.0f));
        const auto mb = stateBlob(ref, bank, c, {}, 1);
        int bad = 0;
        for (int iter = 0; iter < 150; ++iter)
        {
            LoopNestProcessor p;
            loadVia(p, a);
            auto sb = sampleSound(); sb.file = b; sb.pitch = 7.0f;
            p.restore(sb);
            if (iter % 2 == 0) waitForDecode(p, 200);
            std::atomic<bool> done { false };
            FnThread host("host setState", [&] { p.setStateInformation(mb.getData(), (int) mb.getSize()); done = true; });
            host.startThread();
            while (! done.load())
                p.finishPendingLoad(0);
            host.stopThread(5000);
            const bool ok = p.finishPendingLoad(5000) && p.getCurrentSampleFile() == c
                            && near(p.capture().pitch, -5.0f) && ! p.isLoadPending();
            bad += ok ? 0 : 1;
        }
        CHECK(bad == 0);
        if (bad != 0) std::cout << "    " << bad << "/150 interleaves ended wrong" << std::endl;
    }

    std::cout << "F3: restoring the loaded file supersedes an older pending restore" << std::endl;
    {
        LoopNestProcessor p;
        loadVia(p, a);
        auto sa = sampleSound(); sa.file = a; sa.pitch = -2.0f;
        auto sb = sampleSound(); sb.file = b; sb.pitch = 6.0f;
        p.restore(sb);
        p.restore(sa);
        CHECK(! p.isLoadPending());
        juce::Thread::sleep(100);                // let B's decode finish, if it will
        CHECK(p.finishPendingLoad(5000));
        p.finishPendingLoad(0);
        CHECK(sameSound(p.capture(), sa));
    }

    std::cout << "F3: a preset during a pending restore keeps the preset" << std::endl;
    {
        LoopNestProcessor p;
        loadVia(p, a);
        auto sb = sampleSound(); sb.file = b;
        p.restore(sb);
        auto preset = sb; preset.character.drive = 91;
        p.restore(preset, S::kCharacter);
        CHECK(p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == b && near(p.capture().character.drive, 91)
              && near(p.capture().pitch, sb.pitch));
    }

    std::cout << "F4: a set saved mid-load reopens with the new file's params" << std::endl;
    {
        LoopNestProcessor p;
        loadVia(p, a);
        auto trimmed = p.capture(); trimmed.start = 0.3f; trimmed.end = 0.6f;
        p.restore(trimmed, S::kTrim);
        auto sb = sampleSound(); sb.file = b; sb.pitch = 7.0f; sb.start = 0.1f; sb.end = 0.9f;
        p.restore(sb);
        juce::MemoryBlock mb;
        p.getStateInformation(mb);               // before B installs
        p.finishPendingLoad(5000);
        LoopNestProcessor q;
        q.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(q.finishPendingLoad(5000));
        CHECK(sameSound(q.capture(), sb));

        // A spin saved before it lands saves its trim reset.
        LoopNestProcessor r;
        const auto rb = stateBlob(r, bank, a, {}, 1);
        r.setStateInformation(rb.getData(), (int) rb.getSize());
        CHECK(r.finishPendingLoad(5000));
        r.restore(trimmed, S::kTrim);
        r.spin();
        auto x = stateXml(r);
        r.finishPendingLoad(5000);
        CHECK(x != nullptr);
        if (x != nullptr)
        {
            auto tree = juce::ValueTree::fromXml(*x);
            CHECK(near((float) tree.getChildWithProperty("id", "start").getProperty("value"), 0.0f)
                  && near((float) tree.getChildWithProperty("id", "end").getProperty("value"), 1.0f));
        }
    }

    std::cout << "C3: a host save on another thread never pairs one sample with the other's params (stress)" << std::endl;
    {
        // Installs land on this thread (the message thread's role) while a host thread
        // saves in a loop. Every save must name a with a's pitch or b with b's.
        LoopNestProcessor p;
        loadVia(p, a);
        auto sa = sampleSound(); sa.file = a; sa.pitch = -5.0f;
        auto sb = sampleSound(); sb.file = b; sb.pitch = 7.0f;
        p.restore(sa);
        std::atomic<bool> done { false };
        std::atomic<int> saves { 0 }, bad { 0 };
        FnThread host("host getState", [&]
        {
            while (! done.load())
            {
                auto xml = stateXml(p);
                bool ok = false;
                if (xml != nullptr)
                {
                    const auto tree = juce::ValueTree::fromXml(*xml);
                    const juce::File f(tree.getProperty("sample").toString());
                    const float pitch = (float) tree.getChildWithProperty("id", "pitch").getProperty("value");
                    ok = (f == a && near(pitch, -5.0f)) || (f == b && near(pitch, 7.0f));
                }
                bad += ok ? 0 : 1;
                ++saves;
            }
        });
        host.startThread();
        for (int iter = 0; iter < 300; ++iter)
        {
            p.restore(iter % 2 == 0 ? sb : sa);
            p.finishPendingLoad(5000);
        }
        done = true;
        host.stopThread(5000);
        if (bad.load() != 0) std::cout << "    " << bad.load() << "/" << saves.load() << " saves mismatched" << std::endl;
        CHECK(saves.load() > 0);
        CHECK(bad.load() == 0);
    }

    std::cout << "F5: a load superseding a host restore keeps its own failure policy" << std::endl;
    {
        LoopNestProcessor p;
        p.setSyncEnabled(false);
        p.prepareToPlay(44100.0, 512);
        loadVia(p, a);
        const auto mb = stateBlob(p, bank, b, {}, 1);
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        auto bad = p.capture(); bad.file = corrupt;
        p.restore(bad, S::kFile);                // supersedes B, won't decode
        CHECK(p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == a);    // kept, not cleared
        p.setPlaying(true);
        CHECK(audible(p));                       // and the inherited mute released
        p.releaseResources();
    }

    std::cout << "F6: an empty-file capture restores (clears the sample)" << std::endl;
    {
        LoopNestProcessor p;
        const auto initial = p.capture();
        auto sa = sampleSound(); sa.file = a;
        p.restore(sa);
        CHECK(p.finishPendingLoad(5000) && p.getCurrentSampleFile() == a);
        auto sb = sa; sb.file = b;
        p.restore(sb);                           // an older load pending too
        CHECK(p.restore(initial));
        CHECK(p.finishPendingLoad(5000));
        CHECK(sameSound(p.capture(), initial) && ! p.hasSample());
    }

    std::cout << "F7: an install never nests a gesture on a param mid-drag" << std::endl;
    {
        LoopNestProcessor p;
        loadVia(p, a);
        auto* start = p.apvts.getParameter("start");
        start->beginChangeGesture();             // the user holds the start handle
        start->setValueNotifyingHost(0.4f);
        GestureLog log;
        p.addListener(&log);
        auto sb = sampleSound(); sb.file = b;
        p.restore(sb);
        CHECK(p.finishPendingLoad(5000));
        CHECK(near(start->getValue(), 0.4f));    // the hand wins
        CHECK(log.isOneStep() && log.paramsTouched() == 26);
        bool touchedStart = false;
        for (auto [ev, i] : log.seq) touchedStart |= i == start->getParameterIndex();
        CHECK(! touchedStart);
        start->endChangeGesture();
        p.removeListener(&log);
    }

    std::cout << "F9: a truncated WAV fails the load" << std::endl;
    {
        LoopNestProcessor p;
        loadVia(p, a);
        const auto cut = writeTruncatedWav(root.getChildFile("elsewhere/cut.wav"), c);
        auto s = p.capture(); s.file = cut;
        p.restore(s);
        CHECK(p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == a);
        const auto mb = stateBlob(p, bank, cut, {}, 1);   // host recall: clears
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(p.finishPendingLoad(5000));
        CHECK(! p.hasSample());
    }

    std::cout << "F10: the install arrives through the message loop (AsyncUpdater)" << std::endl;
    {
        LoopNestProcessor p;
        auto sa = sampleSound(); sa.file = a;
        p.restore(sa);
        for (int t = 0; t < 5000 && p.isLoadPending(); t += 10)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        CHECK(! p.isLoadPending() && sameSound(p.capture(), sa));
    }

    // --- Stage 1: the spike's effect paths alongside the stage-0 loader ---

    std::cout << "FX: host input runs through the chain; the audition voice is silent" << std::endl;
    {
        LoopNestProcessor p;
        p.prepareToPlay(44100.0, 512);
        loadVia(p, a);
        p.setPlaying(true);
        p.setFxMode(true);
        CHECK(! p.isPlaying());                  // FX stops the audition
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        for (int blk = 0; blk < 8; ++blk)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float v = 0.4f * std::sin(0.05f * (float) (blk * 512 + i));
                buf.setSample(0, i, v); buf.setSample(1, i, v);
            }
            p.processBlock(buf, midi);
            if (blk >= 2) peak = juce::jmax(peak, buf.getMagnitude(0, 512));
        }
        CHECK(peak > 0.2f);

        // Back to AUDITION: the host input is discarded, the loop plays.
        p.setFxMode(false);
        p.setSyncEnabled(false);
        p.setPlaying(true);
        buf.clear();
        p.processBlock(buf, midi);               // the switch block
        CHECK(audible(p));
        for (int i = 0; i < 512; ++i) { buf.setSample(0, i, 0.4f); buf.setSample(1, i, 0.4f); }
        p.setPlaying(false);
        for (int blk = 0; blk < 4; ++blk) { buf.clear(); p.processBlock(buf, midi); }
        for (int i = 0; i < 512; ++i) { buf.setSample(0, i, 0.4f); buf.setSample(1, i, 0.4f); }
        p.processBlock(buf, midi);
        CHECK(buf.getMagnitude(0, 512) == 0.0f); // stopped audition: input never leaks through
        p.releaseResources();
    }

    std::cout << "SYNC: the stretched audition plays (no host tempo: the loop's own)" << std::endl;
    {
        LoopNestProcessor p;
        p.setSyncEnabled(true);
        p.prepareToPlay(44100.0, 512);
        loadVia(p, a);
        p.setPlaying(true);
        CHECK(audible(p, 40));
        LoopNestProcessor::BpmSource src;
        CHECK(near((float) p.getNativeBpm(src), 90.0f) && src == LoopNestProcessor::BpmSource::name);
        p.releaseResources();
    }

    std::cout << "SYNC: the start primes at the current transpose, not the last one set (C6)" << std::endl;
    {
        // Fresh: the stretcher's transpose is 0 until a block sets it. Replay: played at
        // -5, then +7 dialled while stopped and played again from the top, so the last
        // transpose set is -5. Both starts are at +7 and must print the same.
        auto run = [&] (LoopNestProcessor& p, int blocks)
        {
            juce::AudioBuffer<float> out(2, blocks * 512), buf(2, 512);
            juce::MidiBuffer midi;
            for (int b = 0; b < blocks; ++b)
            {
                buf.clear();
                p.processBlock(buf, midi);
                for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, b * 512, buf, ch, 0, 512);
            }
            return out;
        };
        auto setUp = [&] (LoopNestProcessor& p, float semis)
        {
            p.setSyncEnabled(true);
            p.prepareToPlay(44100.0, 512);
            loadVia(p, a);
            setNatural(p, "pitch", semis);
            p.setSyncEnabled(true);
            p.setFreeBpm(90.0);                       // the loop's own tempo, no host needed
        };
        LoopNestProcessor fresh;
        setUp(fresh, 7.0f);
        fresh.setPlaying(true);
        const auto f = run(fresh, 8);
        LoopNestProcessor replay;
        setUp(replay, -5.0f);
        replay.setPlaying(true);
        run(replay, 8);
        replay.setPlaying(false);
        run(replay, 4);                               // fades out and stops
        setNatural(replay, "pitch", 7.0f);
        replay.seekToNorm(0.0f);
        replay.setPlaying(true);
        const auto r = run(replay, 8);
        float diff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < f.getNumSamples(); ++i)
                diff = juce::jmax(diff, std::abs(f.getSample(ch, i) - r.getSample(ch, i)));
        if (diff > 1.0e-4f) std::cout << "    fresh vs replay differ by " << diff << std::endl;
        CHECK(f.getMagnitude(0, f.getNumSamples()) > 0.1f && diff <= 1.0e-4f);
        fresh.releaseResources();
        replay.releaseResources();
    }

    std::cout << "SYNC: a loop whose step doesn't divide it (44.1 kHz file, 48 kHz host) never drifts at the wrap (C7)" << std::endl;
    {
        LoopNestProcessor p;
        p.setSyncEnabled(true);
        p.prepareToPlay(48000.0, 512);
        loadVia(p, a);                                // loopA_90bpm, 1 s at 44.1 kHz
        p.setSyncEnabled(true);
        p.setFreeBpm(90.0);                           // tempo ratio 1: only the SR step remains
        setNatural(p, "end", 0.1f);                   // ~4410-frame region: ~10 wraps a second
        const int lastFrame = 44100 - 1;
        const int endFrame  = (int) (0.1f * (float) lastFrame);
        const double step   = 44100.0 / 48000.0;
        p.setPlaying(true);
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        const int blocks = (int) (60.0 * 48000.0 / 512.0);   // a minute of playback
        for (int b = 0; b < blocks; ++b) { buf.clear(); p.processBlock(buf, midi); }
        // Heard position = output frames x step, wrapped in the region (the voice's lead is
        // exactly the pre-roll, which the playhead subtracts back out).
        const double expect = std::fmod((double) blocks * 512.0 * step, (double) endFrame);
        const double heard  = (double) p.getPlayheadNorm() * (double) lastFrame;
        double err = std::abs(heard - expect);
        err = juce::jmin(err, (double) endFrame - err);      // across the wrap
        if (err > 2.0) std::cout << "    heard " << heard << ", expected " << expect << " (" << err << " frames off)" << std::endl;
        CHECK(err <= 2.0);
        p.releaseResources();
    }

    std::cout << "free tempo: a tempo of LoopNest's own turns SYNC off, stretches the audition and the print, and is saved" << std::endl;
    {
        auto frames = [] (const juce::File& f)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
            return r != nullptr ? (double) r->lengthInSamples : 0.0;
        };
        LoopNestProcessor p;
        p.prepareToPlay(44100.0, 512);
        loadVia(p, a);                               // loopA_90bpm, 1 s
        p.setSyncEnabled(false);
        const double plain = frames(p.renderLoop()); // the file's own tempo
        p.setSyncEnabled(true);
        p.setFreeBpm(180.0);
        CHECK(! p.isSyncEnabled() && p.isStretching() && near((float) p.getFreeBpm(), 180.0f));
        CHECK(near((float) p.getTargetBpm(), 180.0f));
        p.setPlaying(true);
        CHECK(audible(p, 40));
        const double fast = frames(p.renderLoop());  // twice the tempo: half the length
        CHECK(plain > 0.0 && std::abs(fast - plain / 2.0) <= 2.0);
        p.setFreeBpm(1000.0);
        CHECK(near((float) p.getFreeBpm(), 300.0f)); // clamped
        p.setFreeBpm(180.0);

        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        LoopNestProcessor q;
        q.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(! q.isSyncEnabled() && near((float) q.getFreeBpm(), 180.0f));

        p.setSyncEnabled(false);                     // the SYNC button, either way, clears it
        CHECK(p.getFreeBpm() == 0.0 && ! p.isStretching());
        p.setFreeBpm(120.0);
        p.setSyncEnabled(true);
        CHECK(p.getFreeBpm() == 0.0 && p.isSyncEnabled());
        p.setFreeBpm(0.0);                           // clearing leaves SYNC as it is
        CHECK(p.isSyncEnabled());

        p.setSampleFolder(a.getParentDirectory());   // a spin or a picked loop drops it, back to SYNC,
        for (int i = 0; i < 200 && ! p.canSpin(); ++i)   // as it lands (E-M1: the old loop keeps its tempo to then)
            juce::Thread::sleep(5);
        CHECK(p.canSpin());
        p.setFreeBpm(120.0);
        p.spin();
        CHECK(near((float) p.getFreeBpm(), 120.0f) && ! p.isSyncEnabled());
        p.finishPendingLoad(5000);
        CHECK(p.getFreeBpm() == 0.0 && p.isSyncEnabled());
        p.setFreeBpm(120.0);
        CHECK(p.loadSampleFile(a));
        p.finishPendingLoad(5000);
        CHECK(p.getFreeBpm() == 0.0 && p.isSyncEnabled());
        p.setSyncEnabled(false);                     // SYNC switched off by hand: a spin turns it on too
        p.spin();
        p.finishPendingLoad(5000);
        CHECK(p.isSyncEnabled());
        p.releaseResources();
    }

    std::cout << "tempo: the file's, held under every trim; read off the handles only when asked; saved and restored" << std::endl;
    {
        using Src = LoopNestProcessor::BpmSource;
        const auto longLoop = writeWav(root.getChildFile("elsewhere/long.wav"), 8.0, 200.0f);
        LoopNestProcessor p;
        loadVia(p, longLoop);
        Src src;
        auto is = [&src] (LoopNestProcessor& x, float bpm, Src want)
        {
            return near((float) x.getNativeBpm(src), bpm, 0.05f) && src == want;
        };
        CHECK(is(p, 120.0f, Src::length));              // 8 s = 4 bars (no host: nearest 120)
        auto* endP = p.apvts.getParameter("end");
        auto drag = [endP] (float v) { endP->beginChangeGesture(); endP->setValueNotifyingHost(v); endP->endChangeGesture(); };

        // A trim never moves the tempo: not mid-drag, not on release.
        endP->beginChangeGesture();
        endP->setValueNotifyingHost(0.375f);            // a 3 s region
        CHECK(is(p, 120.0f, Src::length));
        CHECK(! p.settleTrimNow());                     // the settle waits for the gesture too
        CHECK(is(p, 120.0f, Src::length));
        endP->endChangeGesture();
        CHECK(is(p, 120.0f, Src::length));              // 3 s would read 160: it holds

        // Nor a host edit with no gesture (undo, automation), once it has settled.
        endP->setValueNotifyingHost(0.45f);             // 3.6 s would read 133.3
        CHECK(p.settleTrimNow());
        CHECK(is(p, 120.0f, Src::length));
        CHECK(! p.settleTrimNow());                     // at rest
        drag(0.1f);
        CHECK(is(p, 120.0f, Src::length));

        // By hand: holds under a trim. From the handles: one read, then that holds too.
        p.setLoopBpm(100.0);
        CHECK(is(p, 100.0f, Src::user));
        drag(0.375f);
        CHECK(is(p, 100.0f, Src::user));
        p.setLoopBpm(1.0e6);
        CHECK(near((float) p.getNativeBpm(src), (float) LoopNestProcessor::kMaxLoopBpm));
        endP->beginChangeGesture(); p.apvts.getParameter("start")->setValueNotifyingHost(0.0f); endP->endChangeGesture();
        p.setBarOrigin(0.25);                           // "Bar 1 starts here": display only, any tempo
        CHECK(near((float) p.getBarOrigin(), 0.25f) && src == Src::user);
        CHECK(p.setLoopBpmFromRegion());
        CHECK(is(p, 160.0f, Src::length));              // 3 s as 2 bars
        CHECK(near((float) p.getBarOrigin(), 0.0f));    // "this region is the beat": bar 1 at the start handle
        p.setBarOrigin(0.25);
        drag(0.5f);
        CHECK(is(p, 160.0f, Src::length));
        drag(0.375f);
        p.setLoopBpm(100.0);
        p.setLoopBpm(0.0);                              // automatic, no tag: the whole file again
        CHECK(is(p, 120.0f, Src::length));
        p.setLoopBpm(100.0);

        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        LoopNestProcessor q;
        q.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(q.finishPendingLoad(5000));
        CHECK(is(q, 100.0f, Src::user));                // the saved tempo comes back
        CHECK(near((float) q.getBarOrigin(), 0.25f));   // and where bar 1 starts
        loadVia(q, writeWav(root.getChildFile("elsewhere/other.wav"), 4.0, 200.0f));
        CHECK(near((float) q.getBarOrigin(), 0.0f));    // a new file: its first sample again

        // A set saved with a trim its tempo was not read from: it stretches as saved,
        // and later trims leave it alone.
        {
            auto xml = stateXml(p);
            CHECK(xml != nullptr);
            xml->setAttribute("loopBpm", 120.0);
            xml->setAttribute("loopBpmSrc", "length");
            juce::MemoryBlock held;
            juce::AudioProcessor::copyXmlToBinary(*xml, held);
            LoopNestProcessor h;
            h.setStateInformation(held.getData(), (int) held.getSize());
            CHECK(h.finishPendingLoad(5000));
            CHECK(near(*h.apvts.getRawParameterValue("end"), 0.375f));
            CHECK(is(h, 120.0f, Src::length));
            CHECK(! h.settleTrimNow());                 // the restore left nothing to settle
            CHECK(is(h, 120.0f, Src::length));
            auto* hEnd = h.apvts.getParameter("end");
            hEnd->beginChangeGesture(); hEnd->setValueNotifyingHost(0.45f); hEnd->endChangeGesture();
            CHECK(is(h, 120.0f, Src::length));
        }

        // A state from before the tempo was saved: the saved trim read as bars, as those builds played it.
        const auto old = stateBlob(p, longLoop.getParentDirectory(), longLoop, {}, 1);
        {
            auto xml = juce::AudioProcessor::getXmlFromBinary(old.getData(), (int) old.getSize());
            CHECK(xml != nullptr && ! xml->hasAttribute("loopBpm"));
        }
        LoopNestProcessor r;
        r.setStateInformation(old.getData(), (int) old.getSize());
        CHECK(r.finishPendingLoad(5000));
        CHECK(is(r, 160.0f, Src::length));

        // A snapshot that lands with its own trim: the tempo reads that trim, not the whole file.
        {
            LoopNestProcessor s;
            auto snap = s.capture();
            snap.file = longLoop; snap.start = 0.0f; snap.end = 0.375f;
            CHECK(s.restore(snap));
            CHECK(s.finishPendingLoad(5000));
            CHECK(is(s, 160.0f, Src::length));
        }

        loadVia(p, a);                                  // a tagged file: the tag wins, and holds under a trim
        CHECK(is(p, 90.0f, Src::name));
        drag(0.5f);
        CHECK(is(p, 90.0f, Src::name));
        p.setLoopBpm(45.0);
        CHECK(is(p, 45.0f, Src::user));
        p.setLoopBpm(0.0);
        CHECK(is(p, 90.0f, Src::name));
        CHECK(! p.setLoopBpmFromRegion());              // 0.5 s fits no reading: unchanged
        CHECK(is(p, 90.0f, Src::name));
    }

    std::cout << "spin from FX: lands, returns to playing (playWhenLoaded)" << std::endl;
    {
        LoopNestProcessor p;
        const auto mb = stateBlob(p, bank, a, {}, 1);
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(p.finishPendingLoad(5000));
        p.setFxMode(true);
        p.setFxMode(false);                      // the editor's SPIN does this first
        p.spin(true);
        CHECK(! p.isPlaying());                  // nothing plays before the swap
        CHECK(p.finishPendingLoad(5000));
        CHECK(p.isPlaying() || p.getCurrentSampleFile() == a);   // a corrupt pick keeps a, unplayed
    }

    // --- Bar-grid lock (docs/v2-scope.md "Bar-grid lock") ---

    const auto click90 = writeClickLoop(root.getChildFile("grid/click_90bpm.wav"), 90.0, 8, 0.5f);

    std::cout << "lock: host stopped plays today's free-running PLAY, bit for bit (golden from 55d1e3f), after the host takes the latency off" << std::endl;
    {
        // Free-running scenarios, each with a play, a pause, a seek and a replay, so
        // every free path (stretched to the host tempo, a free tempo, varispeed) is
        // fingerprinted. The goldens were taken from the build before the lock existed.
        // Since the fixed latency (ruling 3) the output is that audio getLatencySamples()
        // frames late, every frame of it, gains and fades included: what is hashed is the
        // output from there on, as long as before (a few blocks more are run for it).
        auto scenario = [&] (int which, bool hostPlaying)
        {
            LoopNestProcessor p;
            FakeHost h;
            h.bpm = 120.0; h.ppq = 3.25; h.playing = hostPlaying;   // mid-bar
            if (which != 1 || hostPlaying) p.setPlayHead(&h);
            p.prepareToPlay(44100.0, 512);
            loadVia(p, click90);
            setNatural(p, "pitch", which == 2 ? -3.0f : 2.0f);
            setNatural(p, "end", 0.8f);
            if (which == 1) p.setFreeBpm(110.0);
            if (which == 2) p.setSyncEnabled(false);
            p.setPlaying(true);
            juce::AudioBuffer<float> all(2, 0);
            auto append = [&all] (const juce::AudioBuffer<float>& blk)
            {
                const int at = all.getNumSamples();
                all.setSize(2, at + blk.getNumSamples(), true);
                for (int ch = 0; ch < 2; ++ch) all.copyFrom(ch, at, blk, ch, 0, blk.getNumSamples());
            };
            append(runHost(p, h, 300));
            p.setPlaying(false);
            append(runHost(p, h, 10));
            p.seekToNorm(0.3f);
            p.setPlaying(true);
            append(runHost(p, h, 200));
            p.seekToNorm(0.6f);                           // a mid-play seek
            append(runHost(p, h, 200));
            const int n = all.getNumSamples(), lat = p.getLatencySamples();
            append(runHost(p, h, (lat + 511) / 512));
            p.releaseResources();
            juce::AudioBuffer<float> heard(2, n);
            for (int ch = 0; ch < 2; ++ch) heard.copyFrom(ch, 0, all, ch, lat, n);
            return lat > 0 ? hashAudio(heard) : 0;
        };
        // Host stopped: SYNC at the host's tempo, a free tempo, varispeed. Host playing:
        // a free tempo and varispeed (no lock either way, since neither follows the host).
        // Varispeed's golden moved once, on purpose (10-14): PITCH now plays on its 0.01 st
        // grid, so -3 st is exactly -3, not the -3.00000024 a normalised set leaves (was
        // 0x24da8ea0c71edc57; the audio moved by at most 3.3e-4 on a 0.47 peak).
        // All three moved once more, on purpose (10-18, L2): the loop seam's crossfade is held
        // at constant power for the head's correlation with what follows the end point
        // (loopnest::seamGains), where it was plain equal power. With the old seam put back
        // the old goldens (0xd1629c2e01d76220, 0x2cf2bafa37b71391, 0xe1d2ed985937f397) still
        // matched bit for bit, so the seam is the whole change.
        const struct { int which; bool playing; juce::uint64 golden; } runs[] = {
            { 0, false, 0x733bb767026e1b93ull }, { 1, false, 0x6877f073784cfcdaull },
            { 2, false, 0xbeffefd236dd28afull }, { 1, true,  0x6877f073784cfcdaull },
            { 2, true,  0xbeffefd236dd28afull } };
        for (const auto& r : runs)
        {
            const auto got = scenario(r.which, r.playing);
            if (got != r.golden)
                std::cout << "    scenario " << r.which << (r.playing ? " (host playing)" : "") << ": "
                          << juce::String::toHexString((juce::int64) got) << std::endl;
            CHECK(got == r.golden);
        }
    }

    // Grid loops: 8 beats at 90 BPM after a half-beat pickup, trimmed from frame 0 to
    // 8 beats (onLoop): the left handle on song beat 0, every tick kGridPickup after its
    // beat. So no tick sits on the loop's seam (whose 2 ms crossfade softens a head
    // transient, free PLAY and the print alike).
    const auto gridDir = root.getChildFile("grid");
    const auto gridA = writeClickLoop(gridDir.getChildFile("gridA_90bpm.wav"), 90.0, 8, 0.5f, 1, 0.5);
    const auto gridB = writeClickLoop(gridDir.getChildFile("gridB_90bpm.wav"), 90.0, 8, 1.0f, 2, 0.5);
    // A third grid loop, and 2- and 4-bar loops whose ticks get louder each beat (the
    // level says which beat of the loop plays), and two sustained tones (a hard cut
    // shows as a step), all in the folder the picks come from.
    const auto gridC  = writeClickLoop(gridDir.getChildFile("gridC_90bpm.wav"), 90.0, 8, 0.75f, 4, 0.5);
    const auto ramp8  = writeClickLoop(gridDir.getChildFile("ramp8_90bpm.wav"), 90.0, 8, 0.15f, 5, 0.5, 0.05f);
    const auto ramp16 = writeClickLoop(gridDir.getChildFile("ramp16_90bpm.wav"), 90.0, 16, 0.1f, 6, 0.5, 0.035f);
    const auto sineA = writeWav(gridDir.getChildFile("sineA_90bpm.wav"), 8.0 * 60.0 / 90.0, 220.0f, 0.5f);
    const auto sineB = writeWav(gridDir.getChildFile("sineB_90bpm.wav"), 8.0 * 60.0 / 90.0, 330.0f, 0.25f);
    // The trim from `from` to `to` beats of a file `fileBeats` long at 90 BPM, each on its
    // exact frame (a quarter frame over, so the float params never truncate below it).
    auto trimTo = [] (LoopNestProcessor& p, double from, double to, double fileBeats)
    {
        const double lastFrame = (double) std::lround(29400.0 * fileBeats) - 1.0;
        setNatural(p, "start", (float) (((double) std::lround(29400.0 * from) + 0.25) / lastFrame));
        setNatural(p, "end",   (float) (((double) std::lround(29400.0 * to) + 0.25) / lastFrame));
    };
    // A grid loop (`beats` beats after the pickup) trimmed to exactly its beats from frame 0.
    auto onLoop = [&] (LoopNestProcessor& p, double beats) { trimTo(p, 0.0, beats, beats + kGridPickup); };
    // `beats` 0: a file that isn't a grid loop (the tones), its trim left whole.
    auto lockUp = [&] (LoopNestProcessor& p, FakeHost& h, const juce::File& f, double beats = 8.0)
    {
        p.setPlayHead(&h);
        p.prepareToPlay(44100.0, 512);
        loadVia(p, f);
        if (beats > 0.0) onLoop(p, beats);
    };
    auto blocksFor = [] (double beats, double bpm) { return (int) std::ceil(beats * 60.0 * 44100.0 / bpm / 512.0); };
    constexpr double kTolMs = 1.0;

    std::cout << "lock: PLAY pressed with the host playing from bar 1: bar 1 on the downbeat, every tick on the grid" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.playing = true;
        lockUp(p, h, gridA);
        const auto out = runHost(p, h, blocksFor(18.5, 90.0), 512, [&] (int blk) { if (blk == 0) p.setPlaying(true); });
        const auto l = landings(out, h, 0, (int) h.log.size());
        printLanding("from ppq 0", l);
        CHECK(p.isBarLocked() && p.isPlaying());
        CHECK(l.missing == 0 && l.ticks >= 18 && l.worstMs <= kTolMs);
    }

    std::cout << "lock: Live's transport is the master: its start plays it (PLAY never pressed; at bar 1 or mid-bar), its stop stops it" << std::endl;
    {
        for (double startAt : { 0.0, 5.37 })
        {
            LoopNestProcessor p;
            FakeHost h;
            h.bpm = 90.0; h.ppq = startAt; h.playing = false;
            lockUp(p, h, gridA);                             // a loop loaded, PLAY never pressed
            const auto idle = runHost(p, h, 40);
            CHECK(peakFrom(idle, 0) == 0.0f && ! p.isPlayKeyLit());
            h.playing = true;                                // Live starts
            const auto out = runHost(p, h, blocksFor(12.0, 90.0));
            CHECK(p.isBarLocked() && p.isPlayKeyLit());      // the key shows it playing
            const auto l = landings(out, h, 0, (int) h.log.size());
            printLanding(startAt == 0.0 ? "start at bar 1" : "start at ppq 5.37", l);
            CHECK(l.missing == 0 && l.ticks >= 11 && l.worstMs <= kTolMs);
            h.playing = false;                               // Live stops: it stops
            const auto stopped = runHost(p, h, 100);
            CHECK(! p.isPlayKeyLit() && ! p.isBarLocked() && peakFrom(stopped, 50 * 512) == 0.0f);
        }
        // Host stopped, PLAY is free; the host's start takes the audition onto the grid
        // whatever the key said.
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.playing = false;
        lockUp(p, h, gridA);
        p.pressPlay();
        CHECK(audible(p, 40) && ! p.isBarLocked() && p.isPlayKeyLit());
        p.pressPlay();
        runHost(p, h, 20);
        CHECK(! p.isPlayKeyLit());
        h.playing = true;
        const auto started = runHost(p, h, 40);
        CHECK(peakFrom(started, 0) > 0.1f && p.isBarLocked() && p.isPlayKeyLit());
        // A fresh instance with no loop: a start does nothing.
        LoopNestProcessor fresh;
        FakeHost f;
        f.playing = true;
        fresh.setPlayHead(&f);
        fresh.prepareToPlay(44100.0, 512);
        const auto none = runHost(fresh, f, 40);
        CHECK(peakFrom(none, 0) == 0.0f && ! fresh.isBarLocked() && ! fresh.isPlayKeyLit());
    }

    std::cout << "lock: the PLAY key mutes under the lock: declicked, the grid kept, back in time; a state across stops and SYNC off, saved; a press clears it" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.playing = true;
        lockUp(p, h, sineA, 0.0);                                 // a tone: a cut shows as a step
        runHost(p, h, 60);
        const int primes = p.getLockPrimeCount();
        juce::AudioBuffer<float> muting;
        int at = -1;
        muting = runHost(p, h, 40, 512, [&] (int blk) { if (blk == 10) { at = blk; p.pressPlay(); } });
        CHECK(p.isMuted() && ! p.isPlayKeyLit() && p.isBarLocked() && p.isPlaying() == false);
        const float stepMute = maxStepOf(muting, at * 512 - 512, at * 512 + 2048);
        CHECK(peakFrom(muting, (at + 2) * 512 + h.latency) == 0.0f);   // silent within a block (of it reaching the output)
        // Muted across a stop and a start: still silent, and the stop doesn't unmute it.
        h.playing = false; runHost(p, h, 20);
        h.playing = true;
        const auto still = runHost(p, h, 40);
        CHECK(peakFrom(still, 0) == 0.0f && p.isMuted() && p.isBarLocked());
        // Saved with the set.
        {
            juce::MemoryBlock mb;
            p.getStateInformation(mb);
            LoopNestProcessor q;
            q.setStateInformation(mb.getData(), (int) mb.getSize());
            CHECK(q.isMuted());
        }
        // Unmuted on the grid: the ticks of a click loop land in time. Muted to silence the
        // stretcher does no work (B5), so the unmute primes, once, on the grid.
        loadVia(p, gridA);
        onLoop(p, 8.0);
        runHost(p, h, blocksFor(5.0, 90.0));                 // the swap lands on its bar line
        const int primesClick = p.getLockPrimeCount();
        const auto back = runHost(p, h, blocksFor(8.0, 90.0), 512, [&] (int blk) { if (blk == 0) p.pressPlay(); });
        const auto l = landings(back, h, 1, (int) h.log.size());
        printLanding("unmuted", l);
        std::cout << "    largest step muting a tone " << stepMute << std::endl;
        CHECK(primes >= 1 && stepMute < 0.05f && p.isPlayKeyLit() && ! p.isMuted());
        CHECK(l.missing == 0 && l.ticks >= 7 && l.worstMs <= kTolMs && p.getLockPrimeCount() == primesClick + 1);
        // SYNC off with Live playing: a muted loop stays silent (the mute is kept, the key
        // dark); a press clears it and plays the loop free.
        p.pressPlay();
        CHECK(p.isMuted());
        p.setSyncEnabled(false);
        const auto freed = runHost(p, h, blocksFor(2.0, 90.0));
        CHECK(p.isMuted() && ! p.isBarLocked() && ! p.isPlayKeyLit() && peakFrom(freed, h.latency) == 0.0f);
        p.pressPlay();
        const auto played = runHost(p, h, blocksFor(2.0, 90.0));
        CHECK(! p.isMuted() && p.isPlaying() && p.isPlayKeyLit() && peakFrom(played, 10 * 512) > 0.1f);
    }

    std::cout << "lock: muted with REVERB up, the stretcher and chain keep running: the unmute comes back into the wash, no re-prime (B5 review)" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.playing = true;
        setNatural(p, "reverb", 60.0f);
        lockUp(p, h, gridA);
        runHost(p, h, 40);
        p.pressPlay();                                           // the mute
        runHost(p, h, 60);
        const int primes = p.getLockPrimeCount();
        CHECK(p.isMuted() && p.isBarLocked());
        p.pressPlay();
        const auto back = runHost(p, h, 4);
        CHECK(! p.isMuted() && p.getLockPrimeCount() == primes && peakFrom(back, 0) > 0.01f);
    }

    std::cout << "lock: PLAY free-running when the host starts mid-bar snaps onto the grid" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.ppq = 2.6; h.playing = false;
        lockUp(p, h, gridA);
        p.setPlaying(true);
        runHost(p, h, 137);                                  // free, off the grid
        CHECK(! p.isBarLocked());
        h.playing = true;
        const auto out = runHost(p, h, blocksFor(12.0, 90.0));
        CHECK(p.isBarLocked());
        const auto l = landings(out, h, 0, (int) h.log.size());
        printLanding("snapped from free PLAY", l);
        CHECK(l.missing == 0 && l.ticks >= 11 && l.worstMs <= kTolMs);
    }

    std::cout << "lock: host tempo 120 over a 90 BPM loop (stretched x1.33) stays on the grid" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 120.0; h.ppq = 0.0; h.playing = true;
        lockUp(p, h, gridA);
        // 25.5 beats: each change below lands a beat before the next tick (a grid tick is
        // kGridPickup past its beat), the room the steer has always had to settle in.
        const auto out = runHost(p, h, blocksFor(25.5, 120.0), 512, [&] (int blk) { if (blk == 0) p.setPlaying(true); });
        const auto l = landings(out, h, 0, (int) h.log.size());
        printLanding("90 -> 120", l);
        CHECK(l.missing == 0 && l.ticks >= 25 && l.worstMs <= kTolMs);
        // Tempo changes mid-play (typed into Live): every tick from the change on.
        for (double to : { 90.0, 140.0 })
        {
            h.bpm = to;
            const auto changed = runHost(p, h, blocksFor(10.0, to));
            const auto l2 = landings(changed, h, 0, (int) h.log.size());
            printLanding(to == 90.0 ? "tempo change 120 -> 90" : "tempo change 90 -> 140", l2);
            CHECK(l2.missing == 0 && l2.ticks >= 9 && l2.worstMs <= kTolMs);
        }
    }

    std::cout << "lock: a loop picked mid-bar swaps in on the next bar line, not mid-bar" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.ppq = 0.0; h.playing = true;
        p.setPlayHead(&h);
        p.prepareToPlay(44100.0, 512);
        p.setSampleFolder(gridDir);
        for (int i = 0; i < 200 && ! p.canSpin(); ++i)
            juce::Thread::sleep(5);
        CHECK(p.loadSampleFile(gridA) && p.finishPendingLoad(5000));
        onLoop(p, 8.0);
        p.setPlaying(true);
        bool picked = false;
        const auto out = runHost(p, h, blocksFor(16.0, 90.0), 512, [&] (int)
        {
            if (! picked && h.ppq >= 5.3)                    // mid bar 2
            {
                picked = true;
                CHECK(p.loadSampleFile(gridB) && p.finishPendingLoad(5000));
                CHECK(p.getCurrentSampleFile() == gridB);
                onLoop(p, 8.0);                              // B's trim; A plays on with its own
            }
        });
        std::vector<float> lv;
        std::vector<double> beat;
        const auto l = landings(out, h, 0, (int) h.log.size(), 512, &lv, &beat);
        printLanding("A then B", l);
        CHECK(l.missing == 0 && l.ticks >= 15 && l.worstMs <= kTolMs);
        int wrongLoop = 0;                                   // A's ticks read 0.28, B's 0.56 (0.36 faded in)
        for (size_t i = 0; i < lv.size(); ++i)
            if ((beat[i] < 8.0) != (lv[i] < 0.31f))
                ++wrongLoop;
        if (wrongLoop > 0) std::cout << "    " << wrongLoop << " ticks from the wrong loop" << std::endl;
        CHECK(wrongLoop == 0 && picked);
        CHECK(p.isBarLocked());
    }

    std::cout << "lock: a loop-brace wrap (whole loops: seamless; not: back on the grid at once) and a relocate" << std::endl;
    {
        for (double brace : { 16.0, 6.0 })
        {
            LoopNestProcessor p;
            FakeHost h;
            h.bpm = 90.0; h.ppq = 0.0; h.playing = true;
            lockUp(p, h, gridA);
            p.setPlaying(true);
            const auto out = runHost(p, h, blocksFor(3.0 * brace + 2.0, 90.0), 512, [&] (int)
            {
                if (h.ppq >= brace) h.ppq -= brace;          // the host wraps between blocks
            });
            std::vector<float> lv;
            const auto l = landings(out, h, 0, (int) h.log.size(), 512, &lv);
            printLanding(brace == 16.0 ? "brace of 4 bars (2 loops)" : "brace of 1.5 bars", l);
            CHECK(l.missing == 0 && l.ticks >= (int) (3.0 * brace) && l.worstMs <= kTolMs);
            if (brace == 16.0)                               // seamless: no tick dipped at the wrap
                CHECK(lv.size() > 1 && *std::min_element(lv.begin() + 1, lv.end())
                                       >= 0.99f * *std::max_element(lv.begin() + 1, lv.end()));
        }
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.ppq = 0.0; h.playing = true;
        lockUp(p, h, gridA);
        p.setPlaying(true);
        const auto out = runHost(p, h, blocksFor(14.0, 90.0), 512, [&] (int blk)
        {
            if (blk == 200) h.ppq = 37.71;                     // the user clicks further along
        });
        const auto l = landings(out, h, 0, (int) h.log.size());
        printLanding("relocate to ppq 37.71", l);
        CHECK(l.missing == 0 && l.ticks >= 13 && l.worstMs <= kTolMs);
    }

    // A processor on the grid folder, `first` picked and locked from ppq 0 at 90 BPM.
    auto pickUp = [&] (LoopNestProcessor& p, FakeHost& h, const juce::File& first, double beats, int bs = 512)
    {
        h.bpm = 90.0; h.ppq = 0.0; h.playing = true;
        p.setPlayHead(&h);
        p.prepareToPlay(44100.0, bs);
        p.setSampleFolder(gridDir);
        for (int i = 0; i < 200 && ! p.canSpin(); ++i)
            juce::Thread::sleep(5);
        CHECK(p.loadSampleFile(first) && p.finishPendingLoad(5000));
        if (beats > 0.0) onLoop(p, beats);
        p.setPlaying(true);
    };
    auto pick = [&] (LoopNestProcessor& p, const juce::File& f, double beats)
    {
        CHECK(p.loadSampleFile(f) && p.finishPendingLoad(5000));
        if (beats > 0.0) onLoop(p, beats);
    };
    std::cout << "lock: song-grid phase: a loop picked mid-phrase swaps at the next bar line, entering at its song-grid bar (2 and 4 bars, picked in song bar 1 and 2)" << std::endl;
    {
        // The ramp loops' level says which beat of the loop plays: song beat b must play
        // loop beat b mod N (its bar 1 on song bars 1, 1 + L, ...), from the swap line on.
        for (int beats : { 8, 16 })
            for (double pickAt : { 1.3, 5.3 })
            {
                const double swapAt = pickAt < 4.0 ? 4.0 : 8.0;
                LoopNestProcessor p;
                FakeHost h;
                pickUp(p, h, gridA, 8.0);
                bool picked = false;
                const auto out = runHost(p, h, blocksFor(swapAt + beats + 3.5, 90.0), 512, [&] (int)
                {
                    if (! picked && h.ppq >= pickAt)
                    {
                        picked = true;
                        pick(p, beats == 8 ? ramp8 : ramp16, (double) beats);
                    }
                });
                std::vector<float> lv;
                std::vector<double> beat;
                const auto l = landings(out, h, 0, (int) h.log.size(), 512, &lv, &beat);
                std::map<int, float> level;                  // loop beat -> its tick's level
                int wrong = 0;
                for (size_t i = 0; i < lv.size(); ++i)
                {
                    if (beat[i] < swapAt) { if (lv[i] >= 0.31f) ++wrong; continue; }   // still A
                    if (std::abs(beat[i] - swapAt) < 0.5) continue;   // faded in (0.5 ms) on the line
                    const int k = (int) std::lround(beat[i]) % beats;
                    if (level.count(k) > 0 && std::abs(level[k] - lv[i]) > 0.02f * lv[i]) ++wrong;
                    level[k] = lv[i];
                }
                for (auto it = level.begin(); it != level.end() && std::next(it) != level.end(); ++it)
                    if (std::next(it)->second <= it->second) ++wrong;   // louder every loop beat
                const int entry = (int) swapAt % beats;      // where the swap enters the loop
                const juce::String what = juce::String(beats / 4) + " bars picked at ppq " + juce::String(pickAt, 1)
                                        + ", enters at its bar " + juce::String(entry / 4 + 1);
                printLanding(what.toRawUTF8(), l);
                if (wrong > 0) std::cout << "    " << wrong << " ticks off the song grid" << std::endl;
                CHECK(picked && l.missing == 0 && l.worstMs <= kTolMs && wrong == 0 && (int) level.size() >= beats - 1);
                if (beats == 16 && pickAt > 4.0)
                {
                    CHECK(entry == 8);                       // a 4-bar loop picked in bar 2 enters at its bar 3
                    // Relocate to bar 7 beat 3 (song beat 26): loop bar 3 beat 3 (loop beat 10).
                    h.ppq = 26.0;
                    std::vector<float> lv2;
                    std::vector<double> beat2;
                    const auto jump = runHost(p, h, blocksFor(4.5, 90.0));
                    const auto l2 = landings(jump, h, 0, (int) h.log.size(), 512, &lv2, &beat2);
                    printLanding("relocate to bar 7 beat 3", l2);
                    int off = 0;
                    for (size_t i = 1; i < lv2.size(); ++i)   // the tick on the jump itself fades in
                    {
                        const int k = (int) std::lround(beat2[i]) % beats;
                        if (level.count(k) == 0 || std::abs(level[k] - lv2[i]) > 0.02f * lv2[i]) ++off;
                    }
                    CHECK(! beat2.empty() && std::abs(beat2[0] - 26.0) < 1.0e-6 && (int) beat2[0] % beats == 10);
                    CHECK(l2.missing == 0 && l2.worstMs <= kTolMs && lv2.size() >= 4 && off == 0);
                }
            }
    }

    std::cout << "lock: HATCH prints the trim exactly, from the left handle (Andrew 10-13 late): locked or not, trim / ratio frames, never turned; bar 1 and LOOP don't stale it" << std::endl;
    {
        auto read = [] (const juce::File& f)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            juce::AudioBuffer<float> b;
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(f) })
            {
                b.setSize((int) r->numChannels, (int) r->lengthInSamples);
                r->read(&b, 0, b.getNumSamples(), 0, true, true);
            }
            return b;
        };
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 120.0; h.playing = false;               // the host's tempo, stopped
        lockUp(p, h, gridA, 0.0);
        trimTo(p, 0.3, 7.9, 8.5);                       // 7.6 beats from a spot that is no beat
        runHost(p, h, 2);
        const int region = (int) std::lround(29400.0 * 7.9) - (int) std::lround(29400.0 * 0.3);
        const int want   = (int) std::lround((double) region / (120.0 / 90.0));
        p.setDryHatch(true);
        const auto dry = read(p.renderLoop());
        p.setDryHatch(false);
        const int wet = read(p.renderLoop()).getNumSamples();
        std::cout << "    dry " << dry.getNumSamples() << ", wet " << wet << " frames (7.6 beats at 120 = " << want << ")" << std::endl;
        CHECK(std::abs(dry.getNumSamples() - want) <= 1 && std::abs(wet - want) <= 1);
        // Its first tick is 0.2 beats in (4410 frames at 120): the print starts at the left handle.
        const double pk = tickPeakOffset();
        const double got  = tickAt(dry, 4410.0 + pk, 2000);
        std::cout << "    first tick at " << (got - pk) << " (left handle + 0.2 beats = 4410)" << std::endl;
        CHECK(got > 0.0 && std::abs(got - pk - 4410.0) / 44.1 <= kTolMs);
        // Locked or not it is one print: bar 1 and LOOP leave it current, and the
        // unlocked one (LOOP off) is the same audio, frame for frame.
        p.setDryHatch(true);
        p.renderLoop();
        CHECK(p.isLastRenderCurrent());
        p.setBarOrigin(0.4);
        CHECK(p.isLastRenderCurrent());
        p.setLooping(false);
        CHECK(p.isLastRenderCurrent());
        const auto unlocked = read(p.renderLoop());
        bool same = unlocked.getNumSamples() == dry.getNumSamples() && unlocked.getNumChannels() == dry.getNumChannels();
        for (int ch = 0; same && ch < dry.getNumChannels(); ++ch)
            for (int i = 0; i < dry.getNumSamples(); ++i)
                if (unlocked.getSample(ch, i) != dry.getSample(ch, i)) { same = false; break; }
        CHECK(same);
        p.setLooping(true);
        p.setFreeBpm(120.0);                            // a free tempo at the same ratio: the same length
        CHECK(std::abs(read(p.renderLoop()).getNumSamples() - want) <= 1);
        p.setSyncEnabled(false);                        // varispeed: the trim, as it was
        CHECK(std::abs(read(p.renderLoop()).getNumSamples() - region) <= 1);   // (its own rounding)
    }

    // An untrimmed loop is the whole file: end = 1 is frame N, exclusive (LC-1 / LX-3).
    // And "0 st" set through the normalised value is exactly rate 1 (LCV-1). Exactly 8
    // beats at 90 (235200 frames, no pickup), a tick half a beat into each beat so none
    // sits on the seam.
    auto writeWhole8 = [] (const juce::File& f)
    {
        f.getParentDirectory().createDirectory();
        const int n = 8 * 29400;
        juce::AudioBuffer<float> buf(2, n);
        juce::Random r(9);
        for (int i = 0; i < n; ++i)
        {
            const float bed = 1.0e-4f * (r.nextFloat() * 2.0f - 1.0f);
            buf.setSample(0, i, bed); buf.setSample(1, i, bed);
        }
        for (int k = 0; k < 8; ++k)
        {
            const int at = (int) std::lround(29400.0 * (k + kGridPickup));
            for (int i = 0; i < 400 && at + i < n; ++i)
            {
                const float v = 0.5f * std::exp(-(float) i / 88.0f)
                              * std::sin(juce::MathConstants<float>::twoPi * 3000.0f * (float) i / 44100.0f);
                buf.addSample(0, at + i, v); buf.addSample(1, at + i, v);
            }
        }
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream(f.createOutputStream());
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                      .withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(24));
        writer->writeFromAudioSampleBuffer(buf, 0, n);
        return f;
    };
    auto readPrint = [] (const juce::File& f)
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        juce::AudioBuffer<float> wav;
        if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(f) })
        {
            wav.setSize((int) r->numChannels, (int) r->lengthInSamples);
            r->read(&wav, 0, wav.getNumSamples(), 0, true, true);
        }
        return wav;
    };
    const auto whole8 = writeWhole8(root.getChildFile("open/whole8_90bpm.wav"));
    const auto whole8Untagged = writeWhole8(root.getChildFile("open/whole8.wav"));

    std::cout << "open trim: an untrimmed tagged file prints all its frames, SYNC at ratio 1 and varispeed at 0 st (OpenTrimUsesAllFrames)" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.playing = false;                // the host's tempo, stopped: ratio 1
        lockUp(p, h, whole8, 0.0);
        setNatural(p, "pitch", 0.0f);
        runHost(p, h, 2);
        p.setDryHatch(true);
        const int sync = readPrint(p.renderLoop()).getNumSamples();
        p.setSyncEnabled(false);
        const int vari = readPrint(p.renderLoop()).getNumSamples();
        std::cout << "    SYNC " << sync << ", varispeed " << vari << " frames (file 235200)" << std::endl;
        CHECK(sync == 235200);
        CHECK(vari == 235200);
    }

    std::cout << "zero pitch: an interior trim prints and loops its own length, no frame past the right handle (ZeroPitchDoesNotExtendInteriorTrim)" << std::endl;
    {
        // 44100 frames; trim 0..0.5 is [0, 22049). An impulse clear of the head seam, and
        // distinct samples either side of the right handle.
        const auto f = root.getChildFile("open/zero.wav");
        {
            juce::AudioBuffer<float> fx(2, 44100);
            juce::Random r(11);
            for (int i = 0; i < 44100; ++i)
            {
                const float bed = 1.0e-4f * (r.nextFloat() * 2.0f - 1.0f);
                fx.setSample(0, i, bed); fx.setSample(1, i, bed);
            }
            for (int ch = 0; ch < 2; ++ch)
            {
                fx.setSample(ch, 1000, 0.9f);
                fx.setSample(ch, 22048, -0.1f);          // the trim's last frame
                fx.setSample(ch, 22049, 0.15f);          // the first one past it
            }
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream(f.createOutputStream());
            auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                          .withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(32));
            writer->writeFromAudioSampleBuffer(fx, 0, 44100);
        }
        LoopNestProcessor p;
        p.prepareToPlay(44100.0, 512);
        loadVia(p, f);
        p.setSyncEnabled(false);
        setNatural(p, "start", 0.0f);
        setNatural(p, "end", 0.5f);
        setNatural(p, "pitch", 0.0f);
        setNatural(p, "pitchGlide", 0.0f);
        p.setDryHatch(true);
        const auto pr = readPrint(p.renderLoop());
        const float last = pr.getNumSamples() > 0 ? pr.getSample(0, pr.getNumSamples() - 1) : 0.0f;
        p.setPlaying(true);
        const auto out = [&]
        {
            juce::AudioBuffer<float> all(2, 200 * 512), buf(2, 512);
            juce::MidiBuffer midi;
            for (int k = 0; k < 200; ++k)
            {
                buf.clear();
                p.processBlock(buf, midi);
                for (int ch = 0; ch < 2; ++ch) all.copyFrom(ch, k * 512, buf, ch, 0, 512);
            }
            return all;
        }();
        std::vector<int> at;
        for (int i = 1; i < out.getNumSamples() - 1; ++i)
        {
            const float v = std::abs(out.getSample(0, i));
            if (v > 0.5f && v >= std::abs(out.getSample(0, i - 1)) && v >= std::abs(out.getSample(0, i + 1)))
                at.push_back(i);
        }
        bool spacing = at.size() >= 3;
        for (size_t i = 1; spacing && i < at.size(); ++i)
            spacing = at[i] - at[i - 1] == 22049;
        std::cout << "    print " << pr.getNumSamples() << " frames, last " << last << "; audition impulse spacing";
        for (size_t i = 1; i < at.size(); ++i) std::cout << " " << at[i] - at[i - 1];
        std::cout << " (trim 22049)" << std::endl;
        CHECK(pr.getNumSamples() == 22049);
        CHECK(std::abs(last - -0.1f) < 1.0e-3f);
        CHECK(spacing);
        p.releaseResources();
    }

    std::cout << "open trim: an untrimmed tagged loop stays on the grid for 100 locked cycles (OpenTaggedLoopDoesNotSlide)" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.playing = true;
        lockUp(p, h, whole8, 0.0);
        p.setPlaying(true);
        const int perLoop = blocksFor(8.0, 90.0);
        for (int k = 0; k < 98; k += 7)
            runHost(p, h, juce::jmin(7, 98 - k) * perLoop);   // cycles 0..97, unheard
        const auto out = runHost(p, h, 2 * perLoop);         // cycles 98 and 99
        const auto l = landings(out, h, 1, (int) h.log.size());
        printLanding("cycles 98-99", l);
        std::cout << "    primes " << p.getLockPrimeCount() << std::endl;
        CHECK(p.isBarLocked() && p.getLockPrimeCount() == 1);
        CHECK(l.missing == 0 && l.ticks >= 14 && std::abs(l.meanMs) <= 0.1);
        p.releaseResources();
    }

    std::cout << "open trim: an untagged file's length reading uses the whole file, on landing and on Auto (OpenLengthInferenceUsesWholeFile)" << std::endl;
    {
        using Src = LoopNestProcessor::BpmSource;
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.playing = false;
        p.setPlayHead(&h);
        p.prepareToPlay(44100.0, 512);
        runHost(p, h, 2);                                // the host's tempo is seen
        loadVia(p, whole8Untagged);
        Src src = Src::none;
        const double landed = p.getNativeBpm(src);
        std::cout << "    landed " << juce::String(landed, 9) << " BPM" << std::endl;
        CHECK(std::abs(landed - 90.0) < 1.0e-9 && src == Src::length);
        p.setLoopBpm(0.0);                               // Auto
        const double autoBpm = p.getNativeBpm(src);
        CHECK(std::abs(autoBpm - 90.0) < 1.0e-9 && src == Src::length);
    }

    std::cout << "lock: song beat 0 plays the left handle, wherever it is and wherever bar 1 is (Andrew 10-13 late)" << std::endl;
    {
        // The left handle 0.3 beats in, on no beat (as a handle snapped to a hit lands):
        // the ticks, 0.2 beats past it, land 0.2 beats past each song beat. Bar 1 moved
        // anywhere changes nothing the lock plays.
        juce::uint64 atZero = 0;
        for (double origin : { 0.0, 0.05, 0.75 })
        {
            LoopNestProcessor p;
            FakeHost h;
            h.bpm = 90.0; h.playing = true;
            lockUp(p, h, gridA, 0.0);
            trimTo(p, 0.3, 8.3, 8.5);
            p.setBarOrigin(origin);
            const auto out = runHost(p, h, blocksFor(10.0, 90.0), 512, [&] (int blk) { if (blk == 0) p.setPlaying(true); });
            const double pk = tickPeakOffset();
            int on = 0;
            double worst = 0.0;
            for (int bt = 0; bt < 9; ++bt)
            {
                const double at = ((double) bt + 0.2) * 29400.0 + h.latency;
                const double got = tickAt(out, at + pk, 7350);
                if (got < 0.0) continue;
                ++on;
                worst = juce::jmax(worst, std::abs(got - pk - at) / 44.1);
            }
            std::cout << "    bar 1 at " << origin << ": " << on << " ticks, worst " << juce::String(worst, 3) << " ms" << std::endl;
            CHECK(p.isBarLocked() && on == 9 && worst <= kTolMs);
            const auto hash = hashAudio(out);
            if (origin == 0.0) atZero = hash;
            else               CHECK(hash == atZero);
        }
    }

    std::cout << "lock: the swap latches its bar line (the host a frame past it still swaps); a restore at the line holds the old loop out" << std::endl;
    {
        // 480-frame blocks put a block start exactly on beat 8; the host reports it a
        // frame late. The swap still lands there, and no A tick plays past it.
        LoopNestProcessor p;
        FakeHost h;
        pickUp(p, h, gridA, 8.0, 480);
        bool picked = false, nudged = false;
        const auto out = runHost(p, h, blocksFor(14.0, 90.0) * 512 / 480, 480, [&] (int)
        {
            if (! picked && h.ppq >= 5.3) { picked = true; pick(p, gridB, 8.0); }
            if (! nudged && std::abs(h.ppq - 8.0) < 1.0e-9) { nudged = true; h.ppq += 90.0 / (60.0 * 44100.0); }
        });
        std::vector<float> lv;
        std::vector<double> beat;
        const auto l = landings(out, h, 0, (int) h.log.size(), 480, &lv, &beat);
        int wrongLoop = 0;
        for (size_t i = 0; i < lv.size(); ++i)
            if ((beat[i] < 8.0) != (lv[i] < 0.31f))
                ++wrongLoop;
        printLanding("ppq nudged a frame past the line", l);
        CHECK(nudged && l.missing == 0 && l.worstMs <= kTolMs && wrongLoop == 0);

        // A host restore lands while the swap waits: muted until its file decodes, which
        // the test holds past the bar line. The old loop stays faded out past the line
        // until the restore's file lands (into the queue), then that one swaps in.
        LoopNestProcessor q;
        FakeHost g;
        pickUp(q, g, gridA, 8.0);
        picked = false;
        bool restored = false, landed = false;
        int lineBlock = -1, landBlock = -1;
        const auto held = runHost(q, g, blocksFor(12.0, 90.0), 512, [&] (int blk)
        {
            if (! picked && g.ppq >= 5.3) { picked = true; pick(q, gridB, 8.0); }
            if (! restored && g.ppq >= 7.0)
            {
                restored = true;
                auto xml = stateXml(q);                 // this build's state (v3, unmuted), naming C
                xml->setAttribute("sample", gridC.getFullPathName());
                xml->removeAttribute("sampleRel");
                juce::MemoryBlock mb;
                juce::AudioProcessor::copyXmlToBinary(*xml, mb);
                q.setStateInformation(mb.getData(), (int) mb.getSize());
            }
            if (lineBlock < 0 && g.ppq >= 8.0) lineBlock = blk;
            if (! landed && g.ppq >= 9.0) { landed = true; landBlock = blk; CHECK(q.finishPendingLoad(5000)); onLoop(q, 8.0); }
        });
        const int line = (int) std::lround((8.0 - g.log[(size_t) lineBlock - 1].ppq) * 29400.0) + (lineBlock - 1) * 512
                       + g.latency;                     // as heard, once the host takes the latency off
        const float past = juce::jmax(held.getMagnitude(0, line, landBlock * 512 + g.latency - line),
                                      held.getMagnitude(1, line, landBlock * 512 + g.latency - line));
        std::cout << "    peak between the line and the restore landing: " << past << std::endl;
        CHECK(restored && landed && past < 1.0e-3f);
        CHECK(q.getCurrentSampleFile() == gridC && q.isBarLocked());
        float lvC = 0.0f;
        const double landPpq = g.log[(size_t) landBlock].ppq, next = std::ceil(landPpq) + 1.0 + kGridPickup;
        CHECK(tickAt(held, (double) landBlock * 512 + (next - landPpq) * 29400.0 + 22.0 + g.latency, 7000, &lvC) > 0.0);
        std::cout << "    after the restore lands, tick level " << lvC << " (C reads 0.42)" << std::endl;
        CHECK(std::abs(lvC - 0.42f) < 0.03f);            // C plays
    }

    std::cout << "lock: a swap the lock gives up on (the host stops, SYNC off, LOOP off) fades the old loop out, then lands" << std::endl;
    {
        for (int how = 0; how < 3; ++how)
        {
            LoopNestProcessor p;
            FakeHost h;
            pickUp(p, h, sineA, 0.0);
            bool picked = false;
            int at = -1;
            const auto out = runHost(p, h, blocksFor(4.0, 90.0), 512, [&] (int blk)
            {
                if (! picked && h.ppq >= 1.3) { picked = true; pick(p, sineB, 0.0); }
                if (at < 0 && h.ppq >= 2.2)
                {
                    at = blk;
                    if (how == 0) h.playing = false;
                    if (how == 1) p.setSyncEnabled(false);
                    if (how == 2) p.setLooping(false);
                }
            });
            const char* what[] = { "host stops", "SYNC off", "LOOP off" };
            const float step = maxStepOf(out, at * 512 - 512, at * 512 + 4096), before = maxStepOf(out, 22050, at * 512 - 512);
            const float after = out.getMagnitude(0, out.getNumSamples() - 8192, 8192);
            std::cout << "    " << what[how] << ": largest step " << step << " (playing " << before
                      << "), level after " << after << std::endl;
            CHECK(at > 0 && step < 0.05f && ! p.isBarLocked());
            CHECK(p.getCurrentSampleFile() == sineB);
            if (how == 0) CHECK(! p.isPlaying() && after == 0.0f);
            else          CHECK(p.isPlaying() && after > 0.1f && after < 0.3f);   // B (0.25), not A (0.5)
        }
    }

    std::cout << "lock: FX with a swap waiting lands it, and the timer frees the old loop (message loop pumped)" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        pickUp(p, h, gridA, 8.0);
        runHost(p, h, 100);
        pick(p, gridB, 8.0);
        CHECK(p.holdsRetiredLoop());                     // A plays on to the bar line
        p.setFxMode(true);
        runHost(p, h, 4);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(350);
        CHECK(! p.holdsRetiredLoop() && ! p.isBarLocked());
        // A swap that lands on its line frees the old loop the same way.
        p.setFxMode(false);
        p.setPlaying(true);
        runHost(p, h, 20);
        pick(p, gridA, 8.0);
        CHECK(p.holdsRetiredLoop());
        runHost(p, h, blocksFor(4.5, 90.0));
        juce::MessageManager::getInstance()->runDispatchLoopUntil(350);
        CHECK(! p.holdsRetiredLoop() && p.isBarLocked());
    }

    std::cout << "lock: a second pick while a swap waits replaces it: the bar line plays the newest" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        pickUp(p, h, gridA, 8.0);
        bool b1 = false, b2 = false;
        const auto out = runHost(p, h, blocksFor(10.0, 90.0), 512, [&] (int)
        {
            if (! b1 && h.ppq >= 1.3) { b1 = true; pick(p, gridB, 8.0); }
            if (! b2 && h.ppq >= 2.5) { b2 = true; pick(p, gridC, 8.0); }
        });
        std::vector<float> lv;
        std::vector<double> beat;
        const auto l = landings(out, h, 0, (int) h.log.size(), 512, &lv, &beat);
        int wrong = 0;                                   // A reads 0.28, B 0.56, C 0.42
        for (size_t i = 1; i < lv.size(); ++i)
            if (beat[i] < 4.0 ? lv[i] >= 0.31f : (beat[i] > 4.0 && std::abs(lv[i] - 0.42f) > 0.03f))
                ++wrong;
        printLanding("A, B queued, C replaces it", l);
        CHECK(l.missing == 0 && l.worstMs <= kTolMs && wrong == 0 && p.getCurrentSampleFile() == gridC);
    }

    std::cout << "lock: host blocks bigger than the prepared size (2048 on 512), through a swap: one clock step per frame" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        pickUp(p, h, gridA, 8.0, 512);
        bool picked = false;
        const auto out = runHost(p, h, blocksFor(14.0, 90.0) / 4, 2048, [&] (int)
        {
            if (! picked && h.ppq >= 5.3) { picked = true; pick(p, gridB, 8.0); }
        });
        std::vector<float> lv;
        std::vector<double> beat;
        const auto l = landings(out, h, 0, (int) h.log.size(), 2048, &lv, &beat);
        int wrongLoop = 0;
        for (size_t i = 0; i < lv.size(); ++i)
            if ((beat[i] < 8.0) != (lv[i] < 0.31f))
                ++wrongLoop;
        printLanding("2048-frame blocks", l);
        CHECK(l.missing == 0 && l.ticks >= 12 && l.worstMs <= kTolMs && wrongLoop == 0);
        CHECK(p.getLockPrimeCount() == 2);               // the start and the swap, nothing else
    }

    std::cout << "lock: a host clock of NaN, inf or 1e20 never locks and never puts NaN out" << std::endl;
    {
        const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
        struct Bad { double ppq, bpm, bar; bool rawBar; };
        const Bad bad[] = { { nan, 90.0, 0.0, false }, { inf, 90.0, 0.0, false }, { 1.0e20, 90.0, 0.0, false },
                            { 1.0, nan, 0.0, false }, { 1.0, inf, 0.0, false }, { 1.0, 1.0e20, 0.0, false },
                            { 1.0, 0.5, 0.0, false }, { 1.0, 90.0, nan, true }, { 1.0, 90.0, inf, true },
                            { 1.0, 90.0, -1.0e20, true } };
        int nonFinite = 0, locked = 0;
        for (const auto& clk : bad)
        {
            LoopNestProcessor p;
            FakeHost h;
            h.playing = true; h.ppq = clk.ppq; h.bpm = clk.bpm; h.rawBar = clk.rawBar; h.bar = clk.bar;
            lockUp(p, h, gridA);
            p.setPlaying(true);
            const auto out = runHost(p, h, 40);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < out.getNumSamples(); ++i)
                    if (! std::isfinite(out.getSample(ch, i))) ++nonFinite;
            // A bad bar start alone is counted from ppq instead: that clock still locks.
            if (p.isBarLocked() != clk.rawBar) ++locked;
        }
        CHECK(nonFinite == 0 && locked == 0);
    }

    std::cout << "lock: 16- and 32-frame host blocks (and empty ones) stay on the grid with one prime" << std::endl;
    {
        for (int bs : { 16, 32 })
        {
            LoopNestProcessor p;
            FakeHost h;
            h.bpm = 120.0; h.playing = true;
            p.setPlayHead(&h);
            p.prepareToPlay(44100.0, bs);
            loadVia(p, gridA);
            onLoop(p, 8.0);
            p.setPlaying(true);
            juce::AudioBuffer<float> none(2, 0);
            juce::MidiBuffer midi;
            const auto out = runHost(p, h, (int) std::ceil(10.0 * 22050.0 / bs), bs, [&] (int blk)
            {
                if (bs == 32 && blk % 7 == 3) p.processBlock(none, midi);   // a host's empty block
            });
            const auto l = landings(out, h, 0, (int) h.log.size(), bs);
            const juce::String what = juce::String(bs) + "-frame blocks";
            printLanding(what.toRawUTF8(), l);
            CHECK(l.missing == 0 && l.ticks >= 9 && l.worstMs <= kTolMs && p.getLockPrimeCount() == 1);
        }
    }

    std::cout << "lock: a host's tiny or empty first block doesn't hold a spread prime through the full ones after it: the downbeat lands as with 512 throughout" << std::endl;
    {
        const auto down = writeClickLoop(gridDir.getChildFile("down_90bpm.wav"), 90.0, 8, 0.5f, 8);   // ticks from frame 0
        auto onsetMs = [&] (int first)
        {
            LoopNestProcessor p;
            FakeHost h;
            h.bpm = 90.0; h.playing = true;
            p.setPlayHead(&h);
            p.prepareToPlay(44100.0, 512);
            loadVia(p, down);
            trimTo(p, 0.0, 8.0, 8.0);
            juce::AudioBuffer<float> buf(2, 512);
            juce::MidiBuffer midi;
            int at = 0;
            for (int blkNo = 0; blkNo < 40; ++blkNo)
            {
                const int n = blkNo == 0 ? first : 512;
                juce::AudioBuffer<float> blk(buf.getArrayOfWritePointers(), 2, n);
                blk.clear();
                p.processBlock(blk, midi);
                h.advance(n);
                for (int i = 0; i < n; ++i)                   // (as heard: the latency taken off)
                    if (std::abs(blk.getSample(0, i)) > 0.05f) return (double) (at + i - p.getLatencySamples()) / 44.1;
                at += n;
            }
            return -1.0;
        };
        const double base = onsetMs(512), tiny = onsetMs(16), empty = onsetMs(0);
        std::cout << "    downbeat at " << juce::String(base, 2) << " ms (512 throughout), " << juce::String(tiny, 2)
                  << " (16 first), " << juce::String(empty, 2) << " (empty first)" << std::endl;
        CHECK(base >= 0.0 && tiny >= 0.0 && empty >= 0.0);
        CHECK(std::abs(tiny - base) <= 0.4 && std::abs(empty - base) <= 0.4);
    }

    // Drummer re-sync after a meter change (Andrew 10-18): song beat b plays loop beat
    // (b - anchor) mod L, the anchor the first bar line in the new meter. The level says
    // which loop beat plays; the loop's own first pass (from song beat 0) sets the levels.
    auto meterRun = [&] (const juce::File& f, int beats, double runBeats, const std::function<void(FakeHost&)>& meter,
                         const std::function<double(double)>& anchorAt, int& wrong, Landing& l)
    {
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.playing = true; h.rawBar = true;
        lockUp(p, h, f, (double) beats);
        const auto out = runHost(p, h, blocksFor(runBeats, 90.0), 512, [&] (int) { meter(h); });
        std::vector<float> lv;
        std::vector<double> beat;
        l = landings(out, h, 0, (int) h.log.size(), 512, &lv, &beat);
        std::map<int, float> level;
        wrong = 0;
        for (size_t i = 0; i < lv.size(); ++i)
        {
            const double sb = std::round(beat[i]);   // the song beat
            if (sb < beats) { level[(int) sb] = lv[i]; continue; }
            const int k = (int) std::lround(sb - anchorAt(sb)) % beats;
            if (level.count(k) == 0 || std::abs(level[k] - lv[i]) > 0.02f * lv[i])
            {
                std::cout << "    song beat " << sb << ": level " << lv[i] << ", loop beat " << k << " is " << level[k] << std::endl;
                ++wrong;
            }
        }
        return p.getLockPrimeCount();
    };
    std::cout << "lock: 4/4, one 3/4 bar, 4/4 again: loop beat 1 on every downbeat after it, nothing skipped or doubled (C-M8, the drummer version)" << std::endl;
    {
        const auto ramp4 = writeClickLoop(gridDir.getChildFile("ramp4_90bpm.wav"), 90.0, 4, 0.2f, 9, 0.5, 0.15f);
        int wrong = 0;
        Landing l;
        const int primes = meterRun(ramp4, 4, 34.0, [] (FakeHost& h)
        {
            const double q = h.ppq;                       // bars at 0, 4, 8, 12; 16 (3/4); 19, 23, 27 ...
            if (q >= 19.0)      { h.num = 4; h.bar = 19.0 + 4.0 * std::floor((q - 19.0) / 4.0 + 1.0e-9); }
            else if (q >= 16.0) { h.num = 3; h.bar = 16.0; }
            else                { h.num = 4; h.bar = 4.0 * std::floor(q / 4.0 + 1.0e-9); }
        }, [] (double sb) { return sb < 16.0 ? 0.0 : sb < 19.0 ? 16.0 : 19.0; }, wrong, l);
        printLanding("4/4, 3/4, 4/4", l);
        // Bar 16 is whole loops from 0 (no jump); bar 19 re-syncs once.
        CHECK(l.missing == 0 && l.ticks >= 30 && l.worstMs <= kTolMs && wrong == 0 && primes == 2);
    }
    std::cout << "lock: a 4/4 intro, then 3/4: a 3-beat loop sits on the 3/4 bars (C-M8, the drummer version)" << std::endl;
    {
        const auto ramp3 = writeClickLoop(gridDir.getChildFile("ramp3_90bpm.wav"), 90.0, 3, 0.2f, 10, 0.5, 0.2f);
        int wrong = 0;
        Landing l;
        const int primes = meterRun(ramp3, 3, 26.0, [] (FakeHost& h)
        {
            const double q = h.ppq;                       // bars at 0, 4; then 8, 11, 14 ... in 3/4
            if (q >= 8.0) { h.num = 3; h.bar = 8.0 + 3.0 * std::floor((q - 8.0) / 3.0 + 1.0e-9); }
            else          { h.num = 4; h.bar = 4.0 * std::floor(q / 4.0 + 1.0e-9); }
        }, [] (double sb) { return sb < 8.0 ? 0.0 : 8.0; }, wrong, l);
        printLanding("4/4 then 3/4", l);
        CHECK(l.missing == 0 && l.ticks >= 22 && l.worstMs <= kTolMs && wrong == 0 && primes == 2);
    }

    std::cout << "history: only SPIN records: a spin keeps the sound it replaces (a rack pick too, with its knobs); back, forward; a spin drops the forward ones; 20 back steps" << std::endl;
    {
        LoopNestProcessor p;
        p.setSampleFolder(gridDir);
        for (int i = 0; i < 200 && ! p.canSpin(); ++i)
            juce::Thread::sleep(5);
        auto at = [&p] () { return p.getCurrentSampleFile(); };
        auto drive = [&p] () { return p.apvts.getRawParameterValue("drive")->load(); };
        CHECK(p.getHistoryPos() == -1 && ! p.canHistoryBack() && ! p.historyBack());
        CHECK(p.loadSampleFile(gridA) && p.finishPendingLoad(5000));   // a rack pick records nothing
        CHECK(p.getHistorySize() == 0 && ! p.canHistoryBack());
        setNatural(p, "drive", 40.0f);
        setNatural(p, "end", 0.5f);
        p.setLoopBpm(100.0);
        p.setBarOrigin(0.125);
        CHECK(p.spinTo(gridB) && p.finishPendingLoad(5000));           // the spin keeps A as it was left
        CHECK(p.getHistorySize() == 2 && p.getHistoryPos() == 1 && p.canHistoryBack() && ! p.canHistoryForward());
        setNatural(p, "drive", 70.0f);                         // a move on B
        CHECK(p.historyBack() && p.finishPendingLoad(5000));   // A, as it was left
        LoopNestProcessor::BpmSource src;
        CHECK(at() == gridA && near(drive(), 40.0f) && near(*p.apvts.getRawParameterValue("end"), 0.5f, 1.0e-4f));
        CHECK(near((float) p.getNativeBpm(src), 100.0f) && src == LoopNestProcessor::BpmSource::user);
        CHECK(near((float) p.getBarOrigin(), 0.125f) && p.canHistoryForward() && ! p.canHistoryBack());
        CHECK(! p.historyBack());                              // the end
        setNatural(p, "drive", 55.0f);                         // a move on A
        CHECK(p.historyForward() && p.finishPendingLoad(5000));
        CHECK(at() == gridB && near(drive(), 70.0f));           // B with its own move
        CHECK(p.historyBack() && p.finishPendingLoad(5000));
        CHECK(at() == gridA && near(drive(), 55.0f));           // A keeps the move made on it
        // A rack pick adds nothing and drops nothing, and a step doesn't record it.
        CHECK(p.loadSampleFile(gridC) && p.finishPendingLoad(5000));
        CHECK(p.getHistorySize() == 2 && p.getHistoryPos() == 0 && p.canHistoryForward() && at() == gridC);
        CHECK(p.historyForward() && p.finishPendingLoad(5000) && at() == gridB);
        CHECK(p.historyBack() && p.finishPendingLoad(5000) && at() == gridA && near(drive(), 55.0f));
        // A spin from a rack-picked loop keeps that loop, with its knobs; the forward ones go.
        CHECK(p.loadSampleFile(gridC) && p.finishPendingLoad(5000));
        setNatural(p, "drive", 33.0f);
        CHECK(p.spinTo(ramp8) && p.finishPendingLoad(5000));
        CHECK(p.getHistorySize() == 3 && p.getHistoryPos() == 2 && ! p.canHistoryForward());
        CHECK(p.historyBack() && p.finishPendingLoad(5000) && at() == gridC && near(drive(), 33.0f));
        CHECK(p.historyBack() && p.finishPendingLoad(5000) && at() == gridA);
        p.spin();                                              // the SPIN key's random spin records too
        CHECK(p.finishPendingLoad(5000) && p.getHistorySize() == 2 && p.getHistoryPos() == 1);
        // Quick spins: a spin while the last one still loads keeps its entry.
        p.spinTo(gridB);
        p.spinTo(gridC);
        CHECK(p.finishPendingLoad(5000) && p.getHistorySize() == 4 && at() == gridC);
        CHECK(p.historyBack() && p.finishPendingLoad(5000) && at() == gridB);
        // 20 back steps at most (21 entries with the current one): the oldest go.
        const auto files = p.getSampleFiles();
        for (int i = 0; i < 25; ++i)
            CHECK(p.spinTo(files[i % files.size()]) && p.finishPendingLoad(5000));
        CHECK(p.getHistorySize() == LoopNestProcessor::kHistoryBack + 1 && p.getHistoryPos() == LoopNestProcessor::kHistoryBack);
        int steps = 0;
        while (p.historyBack()) { p.finishPendingLoad(5000); ++steps; }
        CHECK(steps == LoopNestProcessor::kHistoryBack);
        // The bridge: ◀ ▶ and their dim state.
        UiBridge br(p);
        CHECK(! (bool) br.state()["histBack"] && (bool) br.state()["histFwd"]);
        CHECK(br.historyStep(+1) && p.finishPendingLoad(5000) && (bool) br.state()["histBack"]);
        CHECK(! br.historyStep(-5) || p.finishPendingLoad(5000));
    }

    std::cout << "history: BACK under the lock swaps on the next bar line, song-grid phased, the old loop's knobs held to the line" << std::endl;
    {
        // ramp8 (its level says which loop beat plays) to bar 3, then gridB (0.56) from
        // song beat 8, turned down 12 dB; BACK at ppq 9.3 brings ramp8 back on beat 12,
        // at song-grid loop beat 4, at 0 dB. B's last ticks stay at -12 dB to the line.
        LoopNestProcessor p;
        FakeHost h;
        pickUp(p, h, ramp8, 8.0);
        bool toB = false, back = false;
        const auto out = runHost(p, h, blocksFor(18.0, 90.0), 512, [&] (int)
        {
            if (! toB && h.ppq >= 5.3) { toB = true; CHECK(p.spinTo(gridB) && p.finishPendingLoad(5000)); onLoop(p, 8.0); }
            if (toB && ! back && h.ppq >= 8.5) setNatural(p, "output", -12.0f);
            if (! back && h.ppq >= 9.3) { back = true; CHECK(p.historyBack() && p.finishPendingLoad(5000)); }
        });
        std::vector<float> lv;
        std::vector<double> beat;
        const auto l = landings(out, h, 0, (int) h.log.size(), 512, &lv, &beat);
        std::map<int, float> ref;                              // ramp8's loop beat levels, beats 1..7
        int wrong = 0;
        for (size_t i = 0; i < lv.size(); ++i)
        {
            const int bt = (int) std::lround(beat[i]);
            if (bt >= 1 && bt < 8) ref[bt % 8] = lv[i];
            else if (bt == 8 || bt == 12 || bt == 9) continue;     // fades: in at a line, the -12 dB ramp
            else if (bt > 9 && bt < 12) { if (std::abs(lv[i] - 0.56f * 0.251f) > 0.02f) ++wrong; }   // B, held at -12 dB
            else if (bt > 12 && bt % 8 != 0                    // (loop beat 0 has no reference: it faded in at the start)
                     && (ref.count(bt % 8) == 0 || std::abs(ref[bt % 8] - lv[i]) > 0.02f * lv[i])) ++wrong;
        }
        printLanding("ramp8, B, BACK to ramp8", l);
        if (wrong > 0) std::cout << "    " << wrong << " ticks wrong" << std::endl;
        CHECK(toB && back && l.missing == 0 && l.worstMs <= kTolMs && wrong == 0 && ref.size() == 7);
        CHECK(p.getCurrentSampleFile() == ramp8 && near(*p.apvts.getRawParameterValue("output"), 0.0f, 0.05f));
    }

    std::cout << "history: a missing file is stepped over (and never lights a dead key); in LIVE a step with nowhere to go stays in LIVE" << std::endl;
    {
        const auto hd = root.getChildFile("hist");
        const auto h1 = writeWav(hd.getChildFile("h1_90bpm.wav"), 1.0, 200.0f);
        const auto h2 = writeWav(hd.getChildFile("h2_90bpm.wav"), 1.0, 300.0f);
        const auto h3 = writeWav(hd.getChildFile("h3_90bpm.wav"), 1.0, 400.0f);
        LoopNestProcessor p;
        p.setSampleFolder(hd);
        for (int i = 0; i < 200 && p.getSampleCount() < 3; ++i)
            juce::Thread::sleep(5);
        CHECK(p.loadSampleFile(h1) && p.finishPendingLoad(5000));
        for (const auto& f : { h2, h3 })
            CHECK(p.spinTo(f) && p.finishPendingLoad(5000));
        CHECK(h2.deleteFile());
        CHECK(p.canHistoryBack() && p.historyBack() && p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == h1 && p.getHistoryPos() == 0);    // h2 stepped over
        CHECK(p.canHistoryForward() && p.historyForward() && p.finishPendingLoad(5000) && p.getCurrentSampleFile() == h3);
        CHECK(h1.deleteFile());
        CHECK(! p.canHistoryBack() && ! p.historyBack());                     // only missing files that way
        UiBridge br(p);
        CHECK(! (bool) br.state()["histBack"]);
        br.setLive(true);
        CHECK(! br.historyStep(-1) && p.isFxMode());                         // LIVE untouched
    }

    std::cout << "history: a spin or step that won't decode takes its entry back out; a host restore starts the history again" << std::endl;
    {
        const auto hd = root.getChildFile("hist2");
        const auto g1 = writeWav(hd.getChildFile("g1_90bpm.wav"), 1.0, 200.0f);
        const auto g2 = writeWav(hd.getChildFile("g2_90bpm.wav"), 1.0, 300.0f);
        const auto bad = hd.getChildFile("g3_bad.wav");
        bad.replaceWithText("not a wav file");
        LoopNestProcessor p;
        p.setSampleFolder(hd);
        for (int i = 0; i < 200 && p.getSampleCount() < 3; ++i)
            juce::Thread::sleep(5);
        CHECK(p.loadSampleFile(g1) && p.finishPendingLoad(5000));
        CHECK(p.spinTo(g2) && p.finishPendingLoad(5000));
        CHECK(p.getHistorySize() == 2 && p.getHistoryPos() == 1);
        CHECK(p.spinTo(bad) && p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == g2);                               // the failed spin changed nothing
        CHECK(p.getHistorySize() == 2 && p.getHistoryPos() == 1 && ! p.canHistoryForward());
        // A step whose file no longer decodes: back where it was, the entry gone.
        CHECK(g1.replaceWithText("corrupted since"));
        CHECK(p.historyBack() && p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == g2 && p.getHistorySize() == 1 && p.getHistoryPos() == 0 && ! p.canHistoryBack());
        // A host restore: its sound is where the history starts again.
        const auto g4 = writeWav(hd.getChildFile("g4_90bpm.wav"), 1.0, 500.0f);
        const auto mb = stateBlob(p, hd, g4, {}, 2);
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(p.finishPendingLoad(5000) && p.getCurrentSampleFile() == g4);
        CHECK(p.getHistorySize() == 0 && ! p.canHistoryBack());
        for (int i = 0; i < 200 && p.getSampleCount() < 4; ++i)
            juce::Thread::sleep(5);
        CHECK(p.spinTo(g2) && p.finishPendingLoad(5000));
        CHECK(p.getHistorySize() == 2 && p.historyBack() && p.finishPendingLoad(5000) && p.getCurrentSampleFile() == g4);
    }

    std::cout << "history: a held step lands exactly on the line: no glide from the old pitch (PITCH glide 80 %)" << std::endl;
    {
        // sineA at +5 st, then sineB at 0 st; BACK lands sineA at +5 on beat 8. Right
        // after the line the tone is 220 Hz x 2^(5/12) = 293.7 Hz, not a glide from 0.
        LoopNestProcessor p;
        FakeHost h;
        setNatural(p, "pitchGlide", 80.0f);
        setNatural(p, "pitch", 5.0f);
        pickUp(p, h, sineA, 0.0);
        bool toB = false, back = false;
        const auto out = runHost(p, h, blocksFor(10.0, 90.0), 512, [&] (int)
        {
            if (! toB && h.ppq >= 1.3) { toB = true; CHECK(p.spinTo(sineB) && p.finishPendingLoad(5000)); }
            if (toB && h.ppq >= 1.4 && h.ppq < 1.5) setNatural(p, "pitch", 0.0f);   // glides to 0 well before beat 6
            if (! back && h.ppq >= 5.3) { back = true; CHECK(p.historyBack() && p.finishPendingLoad(5000)); }
        });
        // Zero crossings over 100 ms from 30 ms past the line (beat 8, frame 8 x 29400).
        auto freqAt = [&out] (int from, int len)
        {
            int z = 0;
            for (int i = from + 1; i < from + len; ++i)
                if ((out.getSample(0, i - 1) < 0.0f) != (out.getSample(0, i) < 0.0f)) ++z;
            return 0.5 * z * 44100.0 / len;
        };
        const double before = freqAt(6 * 29400, 4410), after = freqAt(8 * 29400 + 1323, 4410);
        std::cout << "    before the line " << juce::String(before, 1) << " Hz (B at 0 st: 330), 30..130 ms after "
                  << juce::String(after, 1) << " Hz (A at +5 st: 293.7)" << std::endl;
        CHECK(toB && back && std::abs(before - 330.0) < 6.0 && std::abs(after - 293.66) < 6.0);
    }

    std::cout << "lock: the PLAY key's light is what's audible, through Live, the mute, free PLAY and LOOP off / on" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.playing = false;
        lockUp(p, h, sineA, 0.0);
        int mismatches = 0;
        auto step = [&] (const char* what, const std::function<void()>& act)
        {
            act();
            const auto o = runHost(p, h, 60);
            const bool audibleNow = peakFrom(o, 30 * 512) > 0.05f;
            if (audibleNow != p.isPlayKeyLit())
            {
                ++mismatches;
                std::cout << "    " << what << ": key " << (p.isPlayKeyLit() ? "lit" : "dark") << ", audio "
                          << (audibleNow ? "audible" : "silent") << std::endl;
            }
        };
        step("Live stopped, PLAY", [&] { p.pressPlay(); });
        step("PLAY off", [&] { p.pressPlay(); });
        step("Live starts", [&] { h.playing = true; });
        step("mute", [&] { p.pressPlay(); });
        step("Live stops (muted)", [&] { h.playing = false; });
        step("PLAY with Live stopped", [&] { p.pressPlay(); });            // clears the mute
        step("Live starts", [&] { h.playing = true; });                     // so this is heard
        step("LOOP off", [&] { p.setLooping(false); });
        step("PLAY", [&] { p.pressPlay(); });
        step("PLAY", [&] { p.pressPlay(); });
        step("LOOP on", [&] { p.setLooping(true); });
        step("mute", [&] { p.pressPlay(); });
        step("Live stops", [&] { h.playing = false; });
        step("Live starts (still muted)", [&] { h.playing = true; });
        CHECK(mismatches == 0 && p.isMuted());
        // SYNC off or a free tempo while Live plays: a muted loop stays silent, the key
        // dark, until a press clears the mute and plays it free.
        step("SYNC off (muted)", [&] { p.setSyncEnabled(false); });
        CHECK(p.isMuted() && ! p.isPlayKeyLit());
        step("PLAY", [&] { p.pressPlay(); });
        CHECK(! p.isMuted() && p.isPlayKeyLit());
        step("SYNC on", [&] { p.setSyncEnabled(true); });
        CHECK(p.isBarLocked());
        step("mute", [&] { p.pressPlay(); });
        step("free tempo (muted)", [&] { p.setFreeBpm(100.0); });
        CHECK(p.isMuted() && ! p.isPlayKeyLit());
        step("PLAY", [&] { p.pressPlay(); });
        step("Live stops (the user's free PLAY)", [&] { h.playing = false; });
        step("SYNC on, Live stopped", [&] { p.setSyncEnabled(true); });
        step("Live starts", [&] { h.playing = true; });
        CHECK(mismatches == 0 && p.isBarLocked() && p.isPlayKeyLit());
    }

    std::cout << "lock: an audition the transport started never outlives it: a no-tempo spin stops it at once, SYNC off plays to Live's stop" << std::endl;
    {
        const auto notempo = writeWav(gridDir.getChildFile("zz_untimed.wav"), 0.9, 250.0f);   // 0.9 s: no 1/2/4/8-bar reading fits
        for (int userPlay = 0; userPlay < 2; ++userPlay)
        {
            LoopNestProcessor p;
            FakeHost h;
            pickUp(p, h, gridA, 8.0);
            p.setPlaying(false);                        // pickUp pressed PLAY: undo it, so the transport starts it
            h.playing = false;
            runHost(p, h, 10);
            if (userPlay) p.pressPlay();                // the user's own PLAY, before Live starts
            h.playing = true;
            runHost(p, h, 40);
            CHECK(p.isBarLocked());
            bool picked = false;
            const auto out = runHost(p, h, blocksFor(8.0, 90.0), 512, [&] (int)
            {
                if (! picked && h.ppq >= 1.3) { picked = true; pick(p, notempo, 0.0); }
            });
            LoopNestProcessor::BpmSource src;
            CHECK(p.getNativeBpm(src) == 0.0 && ! p.isBarLocked());
            const float after = peakFrom(out, blocksFor(5.0, 90.0) * 512);
            std::cout << "    " << (userPlay ? "PLAY on: plays on free, peak " : "transport-started: stops, peak ") << after << std::endl;
            if (userPlay) CHECK(after > 0.1f && p.isPlaying() && p.isPlayKeyLit());
            else          CHECK(after == 0.0f && ! p.isPlaying() && ! p.isPlayKeyLit());
            // Live stops: the user's free PLAY is theirs (it plays on, the host stopped);
            // nothing the transport started is left to stop.
            h.playing = false;
            const auto stopped = runHost(p, h, 60);
            CHECK(userPlay ? (peakFrom(stopped, 20 * 512) > 0.1f && p.isPlayKeyLit())
                           : (peakFrom(stopped, 0) == 0.0f && ! p.isPlayKeyLit()));
        }
        // Transport-started, then SYNC off while Live plays: free to Live's stop, then stopped.
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.playing = true;
        lockUp(p, h, sineA, 0.0);                            // a tone: any window shows it
        runHost(p, h, 40);
        CHECK(p.isBarLocked() && ! p.isPlaying());
        p.setSyncEnabled(false);
        const auto freed = runHost(p, h, 40);
        CHECK(peakFrom(freed, 20 * 512) > 0.1f && p.isPlayKeyLit());
        h.playing = false;
        const auto ended = runHost(p, h, 40);
        CHECK(peakFrom(ended, 20 * 512) == 0.0f && ! p.isPlayKeyLit());
    }

    std::cout << "lock: HATCH while the host's transport plays costs the audition no block (30 s file, 128-frame blocks)" << std::endl;
    {
        const auto longGrid = writeClickLoop(root.getChildFile("elsewhere/long30_90bpm.wav"), 90.0, 45, 0.5f, 7);
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.playing = true;
        p.setPlayHead(&h);
        p.prepareToPlay(44100.0, 128);
        loadVia(p, longGrid);
        runHost(p, h, 400, 128);
        CHECK(p.isBarLocked());
        const int missesBefore = p.getSampleLockMisses();
        std::atomic<bool> run { true };
        std::atomic<int> blocks { 0 }, cleared { 0 };
        FnThread audio("audio", [&]
        {
            juce::AudioBuffer<float> buf(2, 128);
            juce::MidiBuffer midi;
            while (run.load())
            {
                buf.clear();
                p.processBlock(buf, midi);
                h.advance(128);
                if (buf.getMagnitude(0, 0, 128) == 0.0f) ++cleared;
                ++blocks;
            }
        });
        audio.startThread();
        int prints = 0;
        for (bool dryPrint : { true, false })
        {
            p.setDryHatch(dryPrint);
            if (p.renderLoop().existsAsFile()) ++prints;
        }
        run.store(false);
        audio.stopThread(2000);
        const int misses = p.getSampleLockMisses() - missesBefore;
        std::cout << "    " << prints << " prints over " << blocks.load() << " audio blocks: " << misses
                  << " blocks lost to a busy sample lock" << std::endl;
        CHECK(prints == 2 && blocks.load() > 100 && misses == 0);
    }

    std::cout << "state: a set saved before stateVersion 3 (or without its mute) loads muted; one saved now follows Live" << std::endl;
    {
        auto startLive = [&] (LoopNestProcessor& q, FakeHost& h)
        {
            h.bpm = 90.0; h.ppq = 0.0; h.playing = false;
            q.setPlayHead(&h);
            q.prepareToPlay(44100.0, 512);
            CHECK(q.finishPendingLoad(5000));
            q.setBarOrigin(0.0);
            runHost(q, h, 10);
            h.playing = true;
            return runHost(q, h, blocksFor(2.0, 90.0));
        };
        LoopNestProcessor src;
        loadVia(src, sineA);
        for (int kind = 0; kind < 3; ++kind)
        {
            juce::MemoryBlock mb;
            if (kind == 0) mb = stateBlob(src, gridDir, sineA, {}, 2);         // an old set
            else
            {
                auto xml = stateXml(src);                                     // this build's
                CHECK(xml->getIntAttribute("stateVersion") == LoopNestProcessor::kStateVersion && xml->hasAttribute("muted"));
                if (kind == 1) xml->removeAttribute("muted");                 // v3 without its mute
                juce::AudioProcessor::copyXmlToBinary(*xml, mb);
            }
            LoopNestProcessor q;
            FakeHost h;
            q.setStateInformation(mb.getData(), (int) mb.getSize());
            const auto out = startLive(q, h);
            const bool muted = kind < 2;
            const char* what[] = { "v2 set", "v3 set, no mute saved", "v3 set saved unmuted" };
            std::cout << "    " << what[kind] << ": " << (q.isMuted() ? "muted" : "unmuted") << ", key "
                      << (q.isPlayKeyLit() ? "lit" : "dark") << ", peak " << peakFrom(out, 0) << std::endl;
            CHECK(q.isMuted() == muted && q.isBarLocked() && q.isPlayKeyLit() == ! muted);
            CHECK(muted ? peakFrom(out, 0) == 0.0f : peakFrom(out, 0) > 0.1f);
        }
    }

    std::cout << "lock: HATCH, and the stop an editor close sends, don't stop a loop Live's transport owns" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.playing = true;
        lockUp(p, h, sineA, 0.0);
        runHost(p, h, 40);
        UiBridge br(p);
        CHECK(br.hatch());                              // UiBridge::hatch: setPlaying(false), then print
        const auto afterHatch = runHost(p, h, 40);
        p.setPlaying(false);                            // what LoopNestWebEditor's destructor sends
        const auto afterClose = runHost(p, h, 40);
        CHECK(p.isBarLocked() && p.isPlayKeyLit());
        CHECK(peakFrom(afterHatch, 0) > 0.1f && peakFrom(afterClose, 0) > 0.1f);
        h.playing = false;                              // with Live stopped the same stop is a stop
        runHost(p, h, 10);
        p.pressPlay();
        runHost(p, h, 20);
        p.setPlaying(false);
        const auto freeStopped = runHost(p, h, 40);
        CHECK(peakFrom(freeStopped, 20 * 512) == 0.0f && ! p.isPlayKeyLit());
    }

    std::cout << "lock: 1000 bars at 120 over a 90 BPM loop, no drift" << std::endl;
    {
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 120.0; h.ppq = 0.0; h.playing = true;
        constexpr int bs = 4096;
        p.setPlayHead(&h);
        p.prepareToPlay(44100.0, bs);
        loadVia(p, gridA);
        onLoop(p, 8.0);
        p.setPlaying(true);
        const int total = (int) std::ceil(1002.0 * 4.0 * 60.0 * 44100.0 / 120.0 / bs);
        const int keep = blocksFor(4.0 * 4.0, 120.0) * 512 / bs + 1;   // four bars at each end
        juce::AudioBuffer<float> buf(2, bs), head(2, keep * bs), tail(2, keep * bs);
        juce::MidiBuffer midi;
        FakeHost first = h, last = h;
        for (int blk = 0; blk < total; ++blk)
        {
            (blk < keep ? first : last).log.push_back({ h.ppq, h.bpm, true });
            buf.clear();
            p.processBlock(buf, midi);
            h.advance(bs);
            if (blk < keep)
                for (int ch = 0; ch < 2; ++ch) head.copyFrom(ch, blk * bs, buf, ch, 0, bs);
            if (blk >= total - keep)
                for (int ch = 0; ch < 2; ++ch) tail.copyFrom(ch, (blk - (total - keep)) * bs, buf, ch, 0, bs);
        }
        last.log.erase(last.log.begin(), last.log.end() - keep);
        first.latency = last.latency = p.getLatencySamples();
        const auto l0 = landings(head, first, 0, keep, bs);
        const auto l1 = landings(tail, last, 0, keep, bs);
        printLanding("first bars", l0);
        printLanding("past bar 1000", l1);
        CHECK(h.ppq >= 4000.0);
        CHECK(l0.missing == 0 && l0.ticks >= 15 && l0.worstMs <= kTolMs);
        CHECK(l1.missing == 0 && l1.ticks >= 15 && l1.worstMs <= kTolMs);
        CHECK(std::abs(l1.meanMs - l0.meanMs) <= 0.05);    // nothing crept in between
    }

    std::cout << "lock: a trim that isn't whole beats (3.6) loops at exactly its own length, sliding on the bars; SYNC off, a free tempo or LOOP off never lock" << std::endl;
    {
        // A loop whose ticks get louder each beat, trimmed from frame 0 to 3.6 beats: it
        // holds ticks 0..3, 0.5 beats past the handle. Cycle c plays tick k at song beat
        // 3.6c + k + 0.5, the same level every cycle: no rounding to 4 beats, no drift.
        const auto ramp = writeClickLoop(root.getChildFile("elsewhere/ramp_90bpm.wav"), 90.0, 8, 0.15f, 3, 0.5, 0.05f);
        {
            LoopNestProcessor p;
            FakeHost h;
            h.bpm = 90.0; h.playing = true;
            lockUp(p, h, ramp, 0.0);
            trimTo(p, 0.0, 3.6, 8.5);
            const float endNorm = *p.apvts.getRawParameterValue("end");
            const auto out = runHost(p, h, blocksFor(26.0, 90.0), 512, [&] (int blk) { if (blk == 0) p.setPlaying(true); });
            const double pk = tickPeakOffset();
            int found = 0, off = 0;
            double worst = 0.0;
            float first[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (int c = 0; c < 7; ++c)
                for (int k = 0; k < 4; ++k)
                {
                    const double at = (3.6 * c + k + 0.5) * 29400.0 + h.latency;
                    float lv = 0.0f;
                    const double got = tickAt(out, at + pk, 7350, &lv);
                    if (got < 0.0 || lv < 0.02f) continue;
                    ++found;
                    worst = juce::jmax(worst, std::abs(got - pk - at) / 44.1);
                    if (c == 0) first[k] = lv;
                    else if (std::abs(lv - first[k]) > 0.02f * lv) ++off;
                }
            std::cout << "    " << found << " of 28 ticks on a 3.6-beat cycle, worst " << juce::String(worst, 3) << " ms"
                      << (off > 0 ? ", " + juce::String(off) + " from the wrong loop beat" : juce::String()) << std::endl;
            CHECK(p.isBarLocked() && found == 28 && worst <= kTolMs && off == 0);
            CHECK(first[0] < first[1] && first[1] < first[2] && first[2] < first[3]);
            CHECK(near(*p.apvts.getRawParameterValue("end"), endNorm, 1.0e-7f));   // the trim is untouched
        }
        LoopNestProcessor p;
        FakeHost h;
        h.bpm = 90.0; h.playing = true;
        lockUp(p, h, gridA);
        p.setPlaying(true);
        runHost(p, h, 20);
        CHECK(p.isBarLocked());
        {
            UiBridge br(p);
            CHECK((bool) br.state()["locked"]);           // the page can show it
        }
        p.setFreeBpm(110.0);
        runHost(p, h, 20);
        CHECK(! p.isBarLocked() && p.isPlaying());
        p.setSyncEnabled(true);
        runHost(p, h, 20);
        CHECK(p.isBarLocked());
        p.setSyncEnabled(false);
        runHost(p, h, 20);
        CHECK(! p.isBarLocked() && p.isPlaying());
        p.setSyncEnabled(true);
        p.setLooping(false);
        runHost(p, h, 20);
        CHECK(! p.isBarLocked());
    }

    // ============================================================ UiBridge (v2 stage 2)
    std::cout << "UiBridge: preset cycle INIT, Vinyl .. Lo-Fi, INIT; back from INIT to Lo-Fi" << std::endl;
    {
        LoopNestProcessor p;
        UiBridge br(p);
        CHECK(br.currentPreset() == -1 && UiBridge::presetName(-1) == "INIT");
        juce::StringArray seen;
        for (int i = 0; i <= loopnest::kNumFactoryPresets; ++i)
        {
            br.stepPreset(+1);
            seen.add(UiBridge::presetName(br.currentPreset()));
        }
        CHECK(seen[0] == "Vinyl" && seen[loopnest::kNumFactoryPresets - 1] == "Lo-Fi");
        CHECK(seen[loopnest::kNumFactoryPresets] == "INIT" && br.currentPreset() == -1);
        br.stepPreset(-1);
        CHECK(UiBridge::presetName(br.currentPreset()) == "Lo-Fi");
        br.stepPreset(+1);
        CHECK(br.currentPreset() == -1);
        // A preset lands its column; INIT puts the defaults back.
        br.selectPreset(1);
        auto* warp = p.apvts.getParameter("warp");
        CHECK(near(warp->convertFrom0to1(warp->getValue()), loopnest::kFactoryPresets[1].v[0], 0.01f));
        br.selectPreset(-1);
        CHECK(near(warp->getValue(), warp->getDefaultValue()));
    }

    std::cout << "UiBridge: presetDirty after a knob move; the name re-resolves after a reset" << std::endl;
    {
        LoopNestProcessor p;
        UiBridge br(p);
        br.selectPreset(1);                          // Tape
        CHECK(! br.presetDirty());
        auto* warp = p.apvts.getParameter("warp");
        const float tapeWarp = warp->getValue();
        warp->setValueNotifyingHost(warp->convertTo0to1(60.0f));
        CHECK(br.presetDirty());
        br.state();                                  // no other match, not at defaults: keeps Tape*
        CHECK(br.currentPreset() == 1 && br.presetDirty());
        warp->setValueNotifyingHost(tapeWarp);        // the knob goes back
        CHECK(! br.presetDirty());
        // A host undo to defaults: the drifted name falls back to INIT.
        br.selectPreset(2);                          // Cassette
        for (auto* prm : p.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*>(prm))
                r->setValueNotifyingHost(r->getDefaultValue());
        br.state();
        CHECK(br.currentPreset() == -1);
        CHECK(! br.presetDirty());
    }

    std::cout << "UiBridge: the preset survives an editor reopen and get/setState; a set without one reads INIT (U-M4)" << std::endl;
    {
        LoopNestProcessor p;
        {
            UiBridge br(p);
            br.selectPreset(2);                      // Cassette
            CHECK(p.getPresetIndex() == 2);
        }
        {
            UiBridge br(p);                          // the editor again
            CHECK(br.currentPreset() == 2 && ! br.presetDirty());
            CHECK((int) br.state()["preset"] == 2 && br.state()["presetName"].toString() == UiBridge::presetName(2));
            auto* warp = p.apvts.getParameter("warp");
            warp->setValueNotifyingHost(warp->convertTo0to1(77.0f));   // dirty, and saved dirty
            CHECK(br.presetDirty());
        }
        auto xml = stateXml(p);
        CHECK(xml != nullptr && xml->getIntAttribute("preset", -5) == 2);
        CHECK(xml != nullptr && xml->getIntAttribute("stateVersion") == LoopNestProcessor::kStateVersion);
        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        {
            LoopNestProcessor q;
            q.setStateInformation(mb.getData(), (int) mb.getSize());
            q.finishPendingLoad(5000);
            CHECK(q.getPresetIndex() == 2);
            UiBridge br(q);
            CHECK(br.currentPreset() == 2 && br.presetDirty());          // the name and its "*", as saved
        }
        // INIT is left out of the state, and a set without the attribute (an older one) reads INIT
        {
            UiBridge br(p);
            br.selectPreset(-1);
        }
        xml = stateXml(p);
        CHECK(xml != nullptr && ! xml->hasAttribute("preset"));
        {
            LoopNestProcessor q;
            q.setPresetIndex(4);                     // whatever this instance showed
            auto old = stateXml(p);
            old->removeAttribute("preset");
            juce::MemoryBlock ob;
            juce::AudioProcessor::copyXmlToBinary(*old, ob);
            q.setStateInformation(ob.getData(), (int) ob.getSize());
            q.finishPendingLoad(5000);
            CHECK(q.getPresetIndex() == -1);
            old->setAttribute("preset", 999);        // out of range: INIT
            ob.reset();
            juce::AudioProcessor::copyXmlToBinary(*old, ob);
            q.setPresetIndex(4);
            q.setStateInformation(ob.getData(), (int) ob.getSize());
            q.finishPendingLoad(5000);
            CHECK(q.getPresetIndex() == -1);
        }
    }

    std::cout << "UiBridge: an editor closed mid-drag ends the gesture it left open (C-M11)" << std::endl;
    {
        LoopNestProcessor p;
        GestureLog log;
        p.addListener(&log);
        auto* drive = p.apvts.getParameter("drive");
        auto* echo  = p.apvts.getParameter("echo");
        {
            UiBridge br(p);
            drive->beginChangeGesture();             // the page's drag, never ended
            drive->setValueNotifyingHost(drive->convertTo0to1(40.0f));
            echo->beginChangeGesture();              // one that ends normally
            echo->setValueNotifyingHost(echo->convertTo0to1(30.0f));
            echo->endChangeGesture();
            p.resetAllParams();                      // CLEAR: the hand on DRIVE wins
            CHECK(near(drive->convertFrom0to1(drive->getValue()), 40.0f, 0.6f));
            CHECK(near(echo->getValue(), echo->getDefaultValue()));
            log.clear();
        }                                            // the editor closes
        int ends = 0, begins = 0;
        for (auto [ev, i] : log.seq)
        {
            if (ev == 'E') { ++ends; CHECK(i == drive->getParameterIndex()); }
            if (ev == 'B') ++begins;
        }
        CHECK(ends == 1 && begins == 0);             // exactly the open one, once
        p.resetAllParams();                          // now CLEAR reaches it
        CHECK(near(drive->getValue(), drive->getDefaultValue()));
        {
            UiBridge br(p);                          // nothing open: a close ends nothing
            log.clear();
        }
        CHECK(log.seq.empty());
        p.removeListener(&log);
    }

    std::cout << "UiBridge: hatch none -> idle -> armed -> stale; dragFile only when armed" << std::endl;
    {
        LoopNestProcessor p;
        p.prepareToPlay(44100.0, 512);
        UiBridge br(p);
        CHECK(br.hatchState() == UiBridge::Hatch::none);
        CHECK(! br.hatch() && br.dragFile() == juce::File());
        loadVia(p, a);
        br.takeSampleIfChanged();
        CHECK(br.hatchState() == UiBridge::Hatch::idle && br.dragFile() == juce::File());
        p.setPlaying(true);
        CHECK(br.hatch());
        CHECK(! p.isPlaying());                      // HATCH stops the audition
        CHECK(br.hatchState() == UiBridge::Hatch::armed);
        CHECK(br.dragFile() == br.lastPrint() && br.lastPrint().existsAsFile());
        CHECK(br.lastPrint().isAChildOf(root.getChildFile("prints")));
        // A dry print (the default) skips the chain: a chain knob keeps it armed; pitch always bakes.
        auto* drive = p.apvts.getParameter("drive");
        drive->setValueNotifyingHost(drive->convertTo0to1(70.0f));
        CHECK(br.hatchState() == UiBridge::Hatch::armed);
        auto* pitch = p.apvts.getParameter("pitch");
        pitch->setValueNotifyingHost(pitch->convertTo0to1(3.0f));
        CHECK(br.hatchState() == UiBridge::Hatch::stale && br.dragFile() == juce::File());
        br.dragStarted();
        CHECK(p.isFxMode());                         // a drag hands the track the audio
        p.releaseResources();
    }

    std::cout << "UiBridge: a cancelled HATCH drag goes back to AUDITION; a dropped one stays LIVE; LIVE before stays LIVE (U-M5)" << std::endl;
    {
        LoopNestProcessor p;
        UiBridge br(p);
        CHECK(! p.isFxMode());
        br.dragStarted();
        CHECK(p.isFxMode());
        br.dragEnded(false);                         // Esc, or let go over the plugin
        CHECK(! p.isFxMode());
        br.dragStarted();
        br.dragEnded(true);                          // on a track
        CHECK(p.isFxMode());
        br.dragStarted();                            // from LIVE: the start flipped nothing
        br.dragEnded(false);
        CHECK(p.isFxMode());
        br.setLive(false);
        br.dragEnded(false);                         // an end with no start in flight changes nothing
        CHECK(! p.isFxMode());
        br.setLive(true);
        br.dragEnded(false);
        CHECK(p.isFxMode());
        // back where it was: a playing audition plays again, a stopped one stays stopped
        br.setLive(false);
        p.setPlaying(true);
        br.dragStarted();
        CHECK(p.isFxMode() && ! p.isPlaying());
        br.dragEnded(false);
        CHECK(! p.isFxMode() && p.isPlaying());
        p.setPlaying(false);
        br.dragStarted();
        br.dragEnded(false);
        CHECK(! p.isFxMode() && ! p.isPlaying());
        p.setPlaying(true);
        br.dragStarted();
        br.dragEnded(true);                          // dropped: LIVE, the audition stays stopped
        CHECK(p.isFxMode() && ! p.isPlaying());
    }

    std::cout << "params: A/B bypass is \"Monitor Bypass\" to the host, id bypass (Andrew, 10-18)" << std::endl;
    {
        LoopNestProcessor p;
        auto* byp = dynamic_cast<juce::RangedAudioParameter*>(p.apvts.getParameter("bypass"));
        CHECK(byp != nullptr && byp->getName(100) == "Monitor Bypass" && byp->getParameterID() == "bypass");
        CHECK(byp != nullptr && byp->getVersionHint() == 1);
    }

    std::cout << "UiBridge: a failed reprint reports failure, even with an older print on disk (C10)" << std::endl;
    {
        LoopNestProcessor p;
        p.prepareToPlay(44100.0, 512);
        UiBridge br(p);
        loadVia(p, a);
        br.takeSampleIfChanged();
        CHECK(br.hatch());
        const auto old = br.lastPrint();
        auto* pitch = p.apvts.getParameter("pitch");
        pitch->setValueNotifyingHost(pitch->convertTo0to1(5.0f));
        CHECK(br.hatchState() == UiBridge::Hatch::stale);
        // Prints root on a plain file: the folder can't be made, so the render fails.
        const auto blocker = root.getChildFile("not-a-folder");
        blocker.replaceWithText("x");
        setenv("LOOPNEST_PRINTS_DIR", blocker.getFullPathName().toRawUTF8(), 1);
        CHECK(! br.hatch());
        setenv("LOOPNEST_PRINTS_DIR", root.getChildFile("prints").getFullPathName().toRawUTF8(), 1);
        CHECK(old.existsAsFile() && br.lastPrint() == old);
        CHECK(br.hatchState() == UiBridge::Hatch::stale && br.dragFile() == juce::File());
        CHECK(br.hatch() && br.hatchState() == UiBridge::Hatch::armed);   // the folder back: it prints
        p.releaseResources();
    }

    std::cout << "print bins: subfolders of the Prints root; sticky; a bin that is gone prints to the root, never recreated" << std::endl;
    {
        const auto prints = root.getChildFile("prints");
        LoopNestProcessor p;
        p.setPrintBin({});                           // a clean slate (the name is an app setting)
        p.prepareToPlay(44100.0, 512);
        UiBridge br(p);
        loadVia(p, a);
        br.takeSampleIfChanged();
        prints.getChildFile("made in finder").createDirectory();
        prints.getChildFile(".hidden").createDirectory();
        CHECK(p.getPrintBins().contains("made in finder") && ! p.getPrintBins().contains(".hidden"));
        CHECK(! p.setPrintBin("nope") && p.getPrintBin().isEmpty());      // only a folder that exists
        CHECK(! p.createPrintBin("  ") && ! p.createPrintBin(".."));
        CHECK(LoopNestProcessor::legalBinName("a/b") == "a-b");           // one level only
        CHECK(br.hatch() && br.lastPrint().getParentDirectory() == prints);
        CHECK(br.lastPrint().getFileName().startsWith("loopA_90bpm_+0st_dry_"));    // <source>_<pitch>[_dry]_<stamp>
        CHECK(p.createPrintBin("grimey") && p.getPrintBin() == "grimey" && prints.getChildFile("grimey").isDirectory());
        CHECK(br.hatchState() == UiBridge::Hatch::stale);                 // another bin: the next HATCH puts a copy there
        CHECK(br.hatch() && br.lastPrint().getParentDirectory() == prints.getChildFile("grimey"));
        CHECK(br.lastPrint().getFileName().startsWith("grimey_loopA_90bpm_+0st_dry_"));
        {
            LoopNestProcessor q;                                          // sticky: an app setting
            CHECK(q.getPrintBin() == "grimey");
        }
        // Renamed in Finder: the pick is orphaned. Prints go to the root; no ghost folder.
        CHECK(prints.getChildFile("grimey").moveFileTo(prints.getChildFile("grimy")));
        CHECK(p.isPrintBinMissing() && p.getPrintBin() == "grimey");
        auto* pitch = p.apvts.getParameter("pitch");
        pitch->setValueNotifyingHost(pitch->convertTo0to1(-2.0f));
        CHECK(br.hatch() && br.lastPrint().getParentDirectory() == prints);
        CHECK(br.lastPrint().getFileName().startsWith("loopA_90bpm_-2st_dry_"));
        CHECK(! prints.getChildFile("grimey").exists());
        CHECK(p.getPrintBins().contains("grimy") && p.setPrintBin("grimy") && ! p.isPrintBinMissing());
        const auto st = br.state();
        CHECK(st["bin"].toString() == "grimy" && ! (bool) st["binMissing"]);
        CHECK(p.setPrintBin({}) && p.getPrintBin().isEmpty());            // back to the root for the next run
        p.releaseResources();
    }

    std::cout << "UiBridge: a missing bin shows on the first state, not a second later (U-L7)" << std::endl;
    {
        const auto prints = root.getChildFile("prints");
        LoopNestProcessor p;
        CHECK(p.createPrintBin("soon gone"));
        CHECK(prints.getChildFile("soon gone").deleteRecursively());
        {
            UiBridge br(p);
            CHECK((bool) br.state()["binMissing"]);
        }
        CHECK(p.setPrintBin({}));
        UiBridge br(p);
        CHECK(! (bool) br.state()["binMissing"]);
    }

    std::cout << "UiBridge: a new sample drops the print; spin from LIVE lands in AUDITION, playing" << std::endl;
    {
        LoopNestProcessor p;
        const auto mb = stateBlob(p, bank, a, {}, 1);
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(p.finishPendingLoad(5000));
        p.prepareToPlay(44100.0, 512);
        UiBridge br(p);
        CHECK(br.takeSampleIfChanged().has_value());  // first sight reports the sample
        CHECK(! br.takeSampleIfChanged().has_value());
        CHECK(br.hatch() && br.hatchState() == UiBridge::Hatch::armed);
        loadVia(p, c);
        const auto changed = br.takeSampleIfChanged();
        CHECK(changed.has_value() && *changed == c);
        CHECK(br.lastPrint() == juce::File() && br.hatchState() == UiBridge::Hatch::idle);

        br.setLive(true);
        CHECK(p.isFxMode());
        br.spin();
        CHECK(p.isFxMode() && ! p.isPlaying());                  // LIVE until the new loop lands (E-M1)
        CHECK(p.finishPendingLoad(5000));
        CHECK((! p.isFxMode() && p.isPlaying())
              || (p.isFxMode() && p.getCurrentSampleFile() == c));   // a corrupt pick keeps c and LIVE
        // PLAY from LIVE: back to AUDITION and playing.
        br.setLive(true);
        br.togglePlay();
        CHECK(! p.isFxMode() && p.isPlaying());
        p.releaseResources();
    }

    std::cout << "UiBridge: a HATCH drag while a pick from LIVE decodes keeps LIVE when it lands, the audition off (E-M1 review)" << std::endl;
    {
        LoopNestProcessor p;
        const auto mb = stateBlob(p, bank, a, {}, 1);
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(p.finishPendingLoad(5000));
        p.prepareToPlay(44100.0, 512);
        UiBridge br(p);
        br.setLive(true);
        CHECK(br.pick(p.getSampleFiles().indexOf(c)));
        br.dragStarted();                                         // the clip goes onto the track meanwhile
        CHECK(p.finishPendingLoad(5000) && p.getCurrentSampleFile() == c);
        CHECK(p.isFxMode() && ! p.isPlaying() && p.isSyncEnabled());
        // No drag: the landing leaves LIVE and plays, as before.
        CHECK(br.pick(p.getSampleFiles().indexOf(a)) && p.finishPendingLoad(5000));
        CHECK(p.getCurrentSampleFile() == a && ! p.isFxMode() && p.isPlaying());
        p.releaseResources();
    }

    std::cout << "UiBridge: the current bank picked again from the recents (the drive remounted) relinks a restored set's missing loop (B4 review)" << std::endl;
    {
        const auto drive = root.getChildFile("remount");
        drive.createDirectory();
        {
            LoopNestProcessor q;
            q.setSampleFolder(drive);                              // a recent folder
        }
        const auto kit = drive.getChildFile("kit_90bpm.wav");
        LoopNestProcessor p;
        const auto mb = stateBlob(p, drive, kit, "kit_90bpm.wav", LoopNestProcessor::kStateVersion);
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        CHECK(p.finishPendingLoad(5000) && p.getMissingSampleName() == "kit_90bpm.wav" && p.getSampleFolder() == drive);
        writeWav(kit, 1.0, 220.0f);                                // the drive is back
        UiBridge br(p);
        CHECK(br.openRecent(drive.getFullPathName()));
        CHECK(p.finishPendingLoad(5000) && p.getCurrentSampleFile() == kit);
        CHECK(p.getMissingSampleName().isEmpty() && p.getSampleCount() == 1);
    }

    std::cout << "UiBridge: a rescan that renames an interior file (same count, same ends) re-sends the rack; a row loads what it shows" << std::endl;
    {
        const auto rd = root.getChildFile("rack");
        for (auto* n : { "r1.wav", "r2.wav", "r3.wav", "r4.wav", "r5.wav" })
            writeWav(rd.getChildFile(n), 0.1, 300.0f);
        LoopNestProcessor p;
        p.setSampleFolder(rd);
        CHECK(p.getSampleCount() == 5);
        UiBridge br(p);
        CHECK(br.takeFilesIfChanged().has_value());
        CHECK(! br.takeFilesIfChanged().has_value());
        // Directory order isn't sorted: rename one interior entry until the rescan keeps
        // the count and both ends, so only the middle of the list moved.
        const auto before = p.getSampleFiles();
        auto renamed = before[before.size() / 2];
        bool sameEnds = false, moved = true;
        for (int k = 0; k < 50 && moved && ! sameEnds; ++k)
        {
            const auto to = rd.getChildFile("m" + juce::String(k) + ".wav");
            moved = renamed.moveFileTo(to);
            renamed = to;
            p.setSampleFolder(rd);
            const auto now = p.getSampleFiles();
            sameEnds = now.size() == before.size() && now.getFirst() == before.getFirst()
                       && now.getLast() == before.getLast();
        }
        CHECK(moved && sameEnds);
        const auto sent = br.takeFilesIfChanged();
        CHECK(sent.has_value());
        if (sent.has_value())
        {
            const auto* names = (*sent)["names"].getArray();
            const auto now = p.getSampleFiles();
            bool inOrder = names != nullptr && names->size() == now.size();
            for (int i = 0; inOrder && i < now.size(); ++i)
                inOrder = (*names)[i].toString() == now[i].getFileName();
            CHECK(inOrder);
            const int row = names != nullptr ? names->indexOf(juce::var(renamed.getFileName())) : -1;
            CHECK(row > 0 && row < now.size() - 1);
            CHECK(br.pick(row) && p.finishPendingLoad(5000) && p.getCurrentSampleFile() == renamed);
        }
    }

    std::cout << "UiBridge: the rack list is rebuilt only when the processor's list generation moves (U-M2)" << std::endl;
    {
        const auto gd = root.getChildFile("gen");
        for (auto* n : { "g1.wav", "g2.wav", "g3.wav" })
            writeWav(gd.getChildFile(n), 0.1, 300.0f);
        LoopNestProcessor p;
        p.setSampleFolder(gd);
        UiBridge br(p);
        const auto g0 = p.getFilesGeneration();
        CHECK(br.takeFilesIfChanged().has_value());
        for (int i = 0; i < 5; ++i)
        {
            CHECK(! br.takeFilesIfChanged().has_value());       // unchanged: nothing built
            br.state();
        }
        CHECK(p.getFilesGeneration() == g0);                    // polling never moves it
        CHECK((int) br.state()["index"] == -1);                 // nothing loaded
        const auto files = p.getSampleFiles();
        CHECK(br.pick(1) && p.finishPendingLoad(5000));
        CHECK((int) br.state()["index"] == 1);                  // a new loop: its row found again
        p.rescanFolder();                                       // same files on disk, a new scan
        CHECK(p.getFilesGeneration() == g0 + 1);
        CHECK(br.takeFilesIfChanged().has_value());             // the generation, not the content, decides
        CHECK(! br.takeFilesIfChanged().has_value());
        writeWav(gd.getChildFile("g0.wav"), 0.1, 310.0f);
        p.rescanFolder();
        const auto sent = br.takeFilesIfChanged();
        CHECK(sent.has_value() && sent->getProperty("names", {}).size() == 4);
        CHECK((int) br.state()["index"] == p.getSampleFiles().indexOf(files[1]));   // re-found in the new list
        CHECK(br.openRecent(gd.getFullPathName()));             // a recent picked again: re-sent
        CHECK(br.takeFilesIfChanged().has_value());
    }

    std::cout << "rescan: of two walks of one folder, the later-started one's list stands (a file copied between them stays)" << std::endl;
    {
        const auto rd = root.getChildFile("scanrace");
        writeWav(rd.getChildFile("s1.wav"), 0.1, 300.0f);
        LoopNestProcessor p;
        p.setSampleFolder(rd);
        CHECK(p.getSampleCount() == 1);
        std::atomic<int> walks { 0 };
        juce::WaitableEvent firstWalked, release;
        p.afterScanWalk = [&]
        {
            if (walks++ == 0) { firstWalked.signal(); release.wait(5000); }   // the first walk publishes last
        };
        std::thread early([&p] { p.rescanFolder(); });          // a RECENT re-pick, say: walks before the copy
        CHECK(firstWalked.wait(5000));
        writeWav(rd.getChildFile("s2.wav"), 0.1, 310.0f);       // the drop's copy lands
        p.rescanFolder();                                       // the drop worker's walk, published first
        CHECK(p.getSampleCount() == 2);
        release.signal();
        early.join();
        p.afterScanWalk = nullptr;
        CHECK(p.getSampleCount() == 2 && p.getSampleFiles().contains(rd.getChildFile("s2.wav")));
    }

    std::cout << "UiBridge: computePeaks" << std::endl;
    {
        juce::AudioBuffer<float> b(2, 100);
        b.clear();
        for (int i = 0; i < 50; ++i)  b.setSample(0, i, -0.5f);
        for (int i = 50; i < 100; ++i) b.setSample(1, i, 1.5f);   // clamps to 1
        const auto pk = UiBridge::computePeaks(b, 2);
        CHECK(pk.size() == 2 && near(pk[0], 0.5f) && near(pk[1], 1.0f));
        const auto many = UiBridge::computePeaks(b, 400);          // more columns than samples
        CHECK(many.size() == 400 && near(many[0], 0.5f) && near(many[399], 1.0f));
        CHECK(UiBridge::computePeaks(b, 0).empty());
        juce::AudioBuffer<float> empty(2, 0);
        const auto none = UiBridge::computePeaks(empty, 8);
        CHECK(none.size() == 8 && none[3] == 0.0f);
    }

    std::cout << "UiBridge: computeHits finds each attack's first frame" << std::endl;
    {
        const double sr = 44100.0;
        juce::AudioBuffer<float> b(2, (int) (sr * 2.0));
        b.clear();
        juce::Random rnd(7);
        auto kick = [&] (int at)
        {
            for (int i = 0; at + i < b.getNumSamples(); ++i)   // rings out: a cut-off tail is a click, and a hit
            {
                const float t = (float) i / (float) sr;
                const float v = 0.8f * std::exp(-t / 0.12f) * std::sin(juce::MathConstants<float>::twoPi * 60.0f * t + 1.2f);
                b.addSample(0, at + i, v); b.addSample(1, at + i, v);
            }
        };
        auto hat = [&] (int at, float amp)
        {
            for (int i = 0; i < (int) (sr * 0.06) && at + i < b.getNumSamples(); ++i)
            {
                const float v = amp * std::exp(-(float) i / (float) (sr * 0.012)) * (rnd.nextFloat() * 2.0f - 1.0f);
                b.addSample(0, at + i, v); b.addSample(1, at + i, v);
            }
        };
        // kick, hat alone, kick, a quiet hat over that kick's tail, and two hats 20 ms apart (one hit)
        const int want[] = { 0, 22050, 44100, 49612, 66150 };
        kick(want[0]); hat(want[1], 0.3f); kick(want[2]); hat(want[3], 0.12f); hat(want[4], 0.3f); hat(want[4] + 882, 0.3f);
        const auto hits = UiBridge::computeHits(b, sr);
        CHECK(hits.size() == 5);
        for (size_t i = 0; i < hits.size() && i < 5; ++i)
            CHECK(std::abs(hits[i] - want[i]) <= 44);   // within 1 ms
        CHECK(std::is_sorted(hits.begin(), hits.end()));

        juce::AudioBuffer<float> silent(2, 44100);
        silent.clear();
        CHECK(UiBridge::computeHits(silent, sr).empty());
        juce::AudioBuffer<float> none(2, 0);
        CHECK(UiBridge::computeHits(none, sr).empty());
        // a steady tone has one attack: where it starts
        juce::AudioBuffer<float> tone(1, 44100);
        for (int i = 0; i < 44100; ++i) tone.setSample(0, i, 0.5f * std::sin(0.05f * (float) i));
        CHECK(UiBridge::computeHits(tone, sr).size() == 1);
    }

    // --- Panel power: per-stage on/off (warpOn .. widthOn) ---

    std::cout << "panel power: an OFF stage settles bit-exact on dry within ~15 ms (single stage, every control set)" << std::endl;
    {
        const double sr = 44100.0;
        const int pre = (int) (0.5 * sr), post = (int) (1.0 * sr), settle = (int) (0.016 * sr);
        for (int s = 0; s < CP::kNumStages; ++s)
        {
            CharacterChain ch;
            ch.prepare(sr);
            CP p = neutralChain();
            setHeavy(p, s);
            const auto in1 = noise(pre, 100 + s);
            const auto out1 = runChain(ch, p, in1);
            const bool acted = ! identical(out1, in1, pre / 2);
            p.on[(size_t) s] = false;
            const auto in2 = noise(post, 200 + s);
            const auto out2 = runChain(ch, p, in2);
            // (WARP's last wet frame still has FLUTTER's 8 ms base to pass after it lands.)
            const int after = settle + (s == CP::kWarp ? (int) std::lround(0.008 * sr) : 0);
            const bool dry = identical(out2, delayedAfter(in1, in2, ch.latency()), after);
            if (! acted || ! dry)
                std::cout << "    stage " << s << ": acted " << acted << ", dry after " << after << " frames " << dry << std::endl;
            CHECK(acted);
            CHECK(dry);
        }
    }

    std::cout << "panel power: OFF renders identical to that stage dialled neutral (all six stages heavy)" << std::endl;
    {
        const double sr = 44100.0;
        const auto in = noise((int) sr, 7);
        CP heavy = neutralChain();
        for (int s = 0; s < CP::kNumStages; ++s) setHeavy(heavy, s);
        for (int s = 0; s < CP::kNumStages; ++s)
        {
            CP off = heavy;     off.on[(size_t) s] = false;
            CP neutral = heavy; setNeutral(neutral, s);
            CharacterChain chOff, chNeutral;
            chOff.prepare(sr); chNeutral.prepare(sr);
            const bool same = identical(runChain(chOff, off, in), runChain(chNeutral, neutral, in));
            if (! same) std::cout << "    stage " << s << " differs" << std::endl;
            CHECK(same);
        }
        // Every panel off: the chain is exactly dry from the first frame.
        CP allOff = heavy;
        allOff.on.fill(false);
        CharacterChain chAll;
        chAll.prepare(sr);
        CHECK(identical(runChain(chAll, allOff, in), delayed(in, chAll.latency())));
    }

    std::cout << "panel power: ECHO / REVERB tails are cut, and come back on with no stale tail" << std::endl;
    {
        const double sr = 44100.0;
        for (int s : { (int) CP::kEcho, (int) CP::kReverb })
        {
            CharacterChain ch;
            ch.prepare(sr);
            CP p = neutralChain();
            setHeavy(p, s);
            runChain(ch, p, noise((int) sr, 300 + s));         // build a tail
            // The input goes silent (the stage hears it once the chain's fixed delay has
            // passed), then off: the wet is gone within ~15 ms.
            juce::AudioBuffer<float> flush(2, ch.latency());
            flush.clear();
            runChain(ch, p, flush);
            p.on[(size_t) s] = false;
            juce::AudioBuffer<float> silence(2, (int) (0.025 * sr));
            silence.clear();
            const auto cut = runChain(ch, p, silence);
            CHECK(peakFrom(cut, 0) > 0.0f);                      // it ramps (no hard click) …
            CHECK(peakFrom(cut, (int) (0.016 * sr)) == 0.0f);   // … and is silent by 16 ms
            // Back on 25 ms later (inside the echo time, inside the reverb's tail):
            // silence in, silence out. Nothing of the old tail replays.
            p.on[(size_t) s] = true;
            juce::AudioBuffer<float> quiet(2, (int) sr);
            quiet.clear();
            const float back = peakFrom(runChain(ch, p, quiet), 0);
            if (back != 0.0f) std::cout << "    stage " << s << " replayed " << back << std::endl;
            CHECK(back == 0.0f);

            // Control: the same moves on the knob (primary to 0 for 25 ms, then back)
            // do keep the tail, so the check above is the switch's doing.
            CharacterChain k;
            k.prepare(sr);
            CP q = neutralChain();
            setHeavy(q, s);
            runChain(k, q, noise((int) sr, 300 + s));
            runChain(k, q, flush);
            CP q0 = q; setNeutral(q0, s);
            runChain(k, q0, silence);
            CHECK(peakFrom(runChain(k, q, quiet), 0) > 1.0e-4f);
        }
    }

    std::cout << "panel power: FX path, every panel OFF with every control set, is dry" << std::endl;
    {
        LoopNestProcessor p;
        p.prepareToPlay(44100.0, 512);
        p.setFxMode(true);
        for (auto [id, v] : std::initializer_list<std::pair<const char*, float>> {
                 { "warp", 60 }, { "flutter", 70 }, { "drive", 75 }, { "driveTone", 60 }, { "echo", 80 },
                 { "reverb", 70 }, { "width", 70 }, { "widthMid", 50 }, { "widthLo", 0.2f } })
            setNatural(p, id, v);
        auto run = [&p] (int seed)
        {
            juce::MidiBuffer midi;
            float maxDiff = 0.0f;
            const int lat = p.getLatencySamples();
            const auto in = noise(40 * 512, seed);
            for (int blk = 0; blk < 40; ++blk)
            {
                juce::AudioBuffer<float> buf(2, 512);
                for (int ch = 0; ch < 2; ++ch) buf.copyFrom(ch, 0, in, ch, blk * 512, 512);
                p.processBlock(buf, midi);
                if (blk >= 20)                                   // against the input at the reported latency
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 512; ++i)
                            maxDiff = juce::jmax(maxDiff, std::abs(buf.getSample(ch, i) - in.getSample(ch, blk * 512 + i - lat)));
            }
            return maxDiff;
        };
        CHECK(run(1000) > 0.01f);                // ON: the chain colours it
        for (int s = 0; s < CP::kNumStages; ++s) setPower(p, s, false);
        CHECK((p.capture().character.on == std::array<bool, CP::kNumStages> {}));
        CHECK(run(2000) < 1.0e-6f);              // OFF: dry
        p.releaseResources();
    }

    std::cout << "panel power: a wet print respects the switches; a flip makes the armed print stale" << std::endl;
    {
        LoopNestProcessor p;
        p.prepareToPlay(44100.0, 512);
        loadVia(p, a);
        p.setSyncEnabled(false);
        p.setDryHatch(false);
        auto readWav = [] (const juce::File& f)
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            juce::AudioBuffer<float> wav;
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(f) })
            {
                wav.setSize((int) r->numChannels, (int) r->lengthInSamples);
                r->read(&wav, 0, (int) r->lengthInSamples, 0, true, true);
            }
            return wav;
        };
        // ECHO and REVERB set hard but OFF …
        setNatural(p, "echo", 80); setNatural(p, "reverb", 70); setNatural(p, "reverbDecay", 90);
        setPower(p, CP::kEcho, false); setPower(p, CP::kReverb, false);
        const auto f1 = p.renderLoop();
        CHECK(f1.existsAsFile() && p.isLastRenderCurrent());
        const auto b1 = readWav(f1);
        // … prints the same as both dialled to 0, ON.
        setNatural(p, "echo", 0); setNatural(p, "reverb", 0); setNatural(p, "reverbDecay", 50);
        setPower(p, CP::kEcho, true); setPower(p, CP::kReverb, true);
        const auto f2 = p.renderLoop();
        const auto b2 = readWav(f2);
        CHECK(f2.existsAsFile() && f2 != f1 && b1.getNumSamples() > 0 && identical(b1, b2));
        // renderSignature counts the flags: a flip stales the armed print, flipping back re-arms it.
        CHECK(p.isLastRenderCurrent());
        setPower(p, CP::kDrive, false);
        CHECK(! p.isLastRenderCurrent());
        setPower(p, CP::kDrive, true);
        CHECK(p.isLastRenderCurrent());
        // A DRY print bypasses the chain, so the switches don't stale it (like a knob).
        p.setDryHatch(true);
        CHECK(p.renderLoop().existsAsFile() && p.isLastRenderCurrent());
        setPower(p, CP::kWidth, false);
        CHECK(p.isLastRenderCurrent());
        p.releaseResources();
    }

    std::cout << "print seam (F11): a wet WARP/FLUTTER print loops with no step larger than one inside it" << std::endl;
    {
        // 441 Hz over 4 s closes on itself, so any step at the seam is the print's own.
        const auto sine = writeWav(root.getChildFile("elsewhere/seam441.wav"), 4.0, 441.0f);
        struct Case { const char* id; float v; };
        for (const Case c : { Case { "warp", 50.0f }, Case { "warp", 100.0f },
                              Case { "flutter", 50.0f }, Case { "flutter", 100.0f } })
        {
            LoopNestProcessor p;
            p.prepareToPlay(44100.0, 512);
            loadVia(p, sine);
            p.setSyncEnabled(false);
            p.setDryHatch(false);
            setNatural(p, c.id, c.v);
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            juce::AudioBuffer<float> w;
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(p.renderLoop()) })
            {
                w.setSize((int) r->numChannels, (int) r->lengthInSamples);
                r->read(&w, 0, (int) r->lengthInSamples, 0, true, true);
            }
            const int P = w.getNumSamples();
            CHECK(P > 44100);
            float seam = 0.0f, inside = 0.0f;
            for (int ch = 0; P > 1 && ch < w.getNumChannels(); ++ch)
            {
                seam = juce::jmax(seam, std::abs(w.getSample(ch, 0) - w.getSample(ch, P - 1)));
                for (int i = 1; i < P; ++i)
                    inside = juce::jmax(inside, std::abs(w.getSample(ch, i) - w.getSample(ch, i - 1)));
            }
            if (seam > inside)
                std::cout << "    " << c.id << " " << c.v << ": seam " << seam << " > inside " << inside << std::endl;
            CHECK(seam <= inside);
            p.releaseResources();
        }
    }

    std::cout << "GAIN MATCH + BYPASS: live in FX; GAIN MATCH bakes into a wet print, BYPASS never does" << std::endl;
    {
        auto setToggle = [] (LoopNestProcessor& p, const char* id, bool on)
        {
            if (auto* q = p.apvts.getParameter(id)) q->setValueNotifyingHost(on ? 1.0f : 0.0f);
        };
        auto rms = [] (const juce::AudioBuffer<float>& b, int from)
        {
            return b.getRMSLevel(0, from, b.getNumSamples() - from);
        };
        // FX on a 220 Hz tone with the master EQ's low cut above it: the chain's output
        // is ~18 dB under its input (inside the stage's 24 dB reach). OUTPUT at -6 dB.
        const int blocks = 260, tail = 44100 / 2;             // 3 s, read the last half second
        juce::AudioBuffer<float> in(2, blocks * 512);
        for (int i = 0; i < in.getNumSamples(); ++i)
        {
            const float v = 0.4f * std::sin(juce::MathConstants<float>::twoPi * 220.0f * (float) i / 44100.0f);
            in.setSample(0, i, v); in.setSample(1, i, v);
        }
        auto runFx = [&] (bool match, bool bypass)
        {
            LoopNestProcessor p;
            p.setFxMode(true);
            setNatural(p, "eqLo", 0.5f);
            setNatural(p, "output", -6.0f);
            setToggle(p, "gainMatch", match);
            setToggle(p, "bypass", bypass);
            p.prepareToPlay(44100.0, 512);
            juce::AudioBuffer<float> out(2, blocks * 512), buf(2, 512);
            juce::MidiBuffer midi;
            for (int blk = 0; blk < blocks; ++blk)
            {
                for (int ch = 0; ch < 2; ++ch) buf.copyFrom(ch, 0, in, ch, blk * 512, 512);
                p.processBlock(buf, midi);
                for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, blk * 512, buf, ch, 0, 512);
            }
            p.releaseResources();
            return out;
        };
        const int from = blocks * 512 - tail;
        const float outGain = juce::Decibels::decibelsToGain(-6.0f);
        const float target = outGain * rms(in, from);
        const auto plain = runFx(false, false), matched = runFx(true, false), bypassed = runFx(false, true);
        CHECK(rms(plain, from) < 0.3f * target);              // the EQ took it down
        CHECK(near(rms(matched, from) / target, 1.0f, 0.06f));  // GAIN MATCH brought it back (0.5 dB)
        float worst = 0.0f;
        const int lat = latencyAt(44100.0);
        for (int i = from; i < blocks * 512; ++i)
            worst = juce::jmax(worst, std::abs(bypassed.getSample(0, i) - outGain * in.getSample(0, i - lat)));
        CHECK(worst < 1.0e-4f);                               // BYPASS plays the track's own audio, at the latency

        // The wet print bakes GAIN MATCH, and the armed print goes stale when it flips.
        LoopNestProcessor p;
        p.prepareToPlay(44100.0, 512);
        loadVia(p, a);
        p.setSyncEnabled(false);
        p.setDryHatch(false);
        setNatural(p, "eqLo", 0.5f);
        auto printRms = [&]
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            const auto file = p.renderLoop();
            std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(file) };
            if (r == nullptr) return 0.0f;
            juce::AudioBuffer<float> wav((int) r->numChannels, (int) r->lengthInSamples);
            r->read(&wav, 0, (int) r->lengthInSamples, 0, true, true);
            return wav.getRMSLevel(0, 0, wav.getNumSamples());
        };
        const float off = printRms();
        CHECK(off > 0.0f && p.isLastRenderCurrent());
        setToggle(p, "bypass", true);
        CHECK(p.isLastRenderCurrent());                       // a monitor: nothing to reprint
        CHECK(near(printRms() / off, 1.0f, 1.0e-4f));         // and a print under BYPASS is still the wet one
        setToggle(p, "bypass", false);
        setToggle(p, "gainMatch", true);
        CHECK(! p.isLastRenderCurrent());
        const float on = printRms();
        CHECK(on > 3.0f * off);                               // the print carries the makeup
        // A DRY print bakes neither.
        p.setDryHatch(true);
        const float dryOn = printRms();
        CHECK(p.isLastRenderCurrent());
        setToggle(p, "gainMatch", false);
        CHECK(p.isLastRenderCurrent());
        CHECK(near(printRms() / dryOn, 1.0f, 1.0e-6f));
        p.releaseResources();
    }

    std::cout << "panel power: capture/restore, history and get/setState round-trip the flags" << std::endl;
    {
        LoopNestProcessor p;
        CHECK(allPower(p, true));                // default ON
        setPower(p, CP::kEcho, false);
        setPower(p, CP::kWidth, false);
        auto snap = p.capture();
        CHECK(! snap.character.on[CP::kEcho] && ! snap.character.on[CP::kWidth] && snap.character.on[CP::kWarp]);
        p.resetAllParams();
        CHECK(allPower(p, true));
        CHECK(p.restore(snap, S::kAll & ~S::kFile));   // a history step brings them back
        CHECK(powerIs(p, CP::kEcho, false) && powerIs(p, CP::kWidth, false) && powerIs(p, CP::kDrive, true));

        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        LoopNestProcessor q;
        q.setStateInformation(mb.getData(), (int) mb.getSize());
        q.finishPendingLoad(5000);
        CHECK(powerIs(q, CP::kEcho, false) && powerIs(q, CP::kWidth, false));
        for (int s : { (int) CP::kWarp, (int) CP::kFlutter, (int) CP::kDrive, (int) CP::kReverb })
            CHECK(powerIs(q, s, true));
        CHECK(q.capture().character.on == snap.character.on);
    }

    std::cout << "panel power: a set saved before the switches existed loads with all six ON" << std::endl;
    {
        LoopNestProcessor old;
        auto state = old.apvts.copyState();
        for (auto* id : CP::powerIds())
            state.removeChild(state.getChildWithProperty("id", juce::String(id)), nullptr);
        CHECK(! state.getChildWithProperty("id", "echoOn").isValid());
        state.setProperty("stateVersion", 2, nullptr);
        juce::MemoryBlock mb;
        if (auto xml = state.createXml()) juce::AudioProcessor::copyXmlToBinary(*xml, mb);
        LoopNestProcessor p;
        for (int s = 0; s < CP::kNumStages; ++s) setPower(p, s, false);   // the instance had them off
        p.setStateInformation(mb.getData(), (int) mb.getSize());
        p.finishPendingLoad(5000);
        CHECK(allPower(p, true));
        CHECK(p.capture().character.allOn());
    }

    std::cout << "panel power: a factory preset, INIT and CLEAR turn every panel back ON" << std::endl;
    {
        LoopNestProcessor p;
        UiBridge br(p);
        auto allOff = [&p] { for (int s = 0; s < CP::kNumStages; ++s) setPower(p, s, false); };
        allOff();
        br.selectPreset(3);                      // Dust
        CHECK(allPower(p, true));
        CHECK(! br.presetDirty());
        setPower(p, CP::kEcho, false);           // a panel off is a change from the preset
        CHECK(br.presetDirty());
        allOff();
        br.selectPreset(-1);                     // INIT
        CHECK(allPower(p, true));
        allOff();
        p.resetAllParams();                      // CLEAR
        CHECK(allPower(p, true));
        // The preset table itself: fromArray has every panel ON.
        allOff();
        auto snap = p.capture();
        snap.character = CP::fromArray(loopnest::kFactoryPresets[0].v);
        CHECK(snap.character.allOn());
        p.restore(snap, S::kCharacter);
        CHECK(allPower(p, true));
    }

    // --- Band cuts and REVERB ring-out (audit F17-F20) ---

    // A 220 Hz sine, -12 dBFS, R 90 deg ahead (LoopNestMeasure's zipper/dead-zone source),
    // run through a fresh chain with the params at each 512-frame block; `eq` also runs
    // the master EQ. And the peak above 5 kHz (4th-order Butterworth high-pass) of
    // frames [from, to): what a click or a zipper adds over the sine.
    const double bandSr = 44100.0;
    const int    bandLat = latencyAt(bandSr);   // the chain's fixed delay there
    auto bandSine = [bandSr] (double seconds)
    {
        juce::AudioBuffer<float> buf(2, (int) (seconds * bandSr));
        const double amp = juce::Decibels::decibelsToGain(-12.0), w = juce::MathConstants<double>::twoPi * 220.0 / bandSr;
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            buf.setSample(0, i, (float) (amp * std::sin(w * i)));
            buf.setSample(1, i, (float) (amp * std::sin(w * i + juce::MathConstants<double>::halfPi)));
        }
        return buf;
    };
    auto runBand = [bandSr] (const juce::AudioBuffer<float>& in, const std::function<CP(int)>& at, bool eq)
    {
        CharacterChain ch;
        ch.prepare(bandSr);
        juce::AudioBuffer<float> out(2, in.getNumSamples());
        for (int i = 0; i < in.getNumSamples(); ++i)
        {
            if (i % 512 == 0) ch.setParams(at(i));
            float x[2] = { in.getSample(0, i), in.getSample(1, i) };
            if (eq) ch.processEq(x, 2);
            ch.process(x, 2);
            out.setSample(0, i, x[0]); out.setSample(1, i, x[1]);
        }
        return out;
    };
    auto hfPeakDb = [bandSr] (const juce::AudioBuffer<float>& buf, int from, int to)
    {
        struct Bq { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
                    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
        auto hp = [bandSr] (double q)
        {
            const double w = juce::MathConstants<double>::twoPi * 5000.0 / bandSr, al = std::sin(w) / (2.0 * q),
                         cw = std::cos(w), a0 = 1.0 + al;
            return Bq { 0.5 * (1.0 + cw) / a0, -(1.0 + cw) / a0, 0.5 * (1.0 + cw) / a0, -2.0 * cw / a0, (1.0 - al) / a0 };
        };
        double pk = 0.0;
        for (int ch = 0; ch < 2; ++ch)
        {
            Bq s1 = hp(0.5412), s2 = hp(1.3066);
            for (int i = 0; i < to; ++i)
            {
                const double v = s2.run(s1.run(buf.getSample(ch, i)));
                if (i >= from) pk = juce::jmax(pk, std::abs(v));
            }
        }
        return juce::Decibels::gainToDecibels(pk, -200.0);
    };

    std::cout << "bands: engaging a cut across its dead zone ramps in, no click (F17)" << std::endl;
    {
        struct Step { const char* name; std::function<void(CP&)> base, engage; bool eq; float settleTol; };
        const std::vector<Step> steps = {
            { "eqLo 0 -> 0.03",          [] (CP&) {},                [] (CP& p) { p.eqLo = 0.03f; },   true,  1.0e-6f },
            { "eqHi 1 -> 0.97",          [] (CP&) {},                [] (CP& p) { p.eqHi = 0.97f; },   true,  1.0e-6f },
            { "widthLo 0 -> 0.03 (w 100)", [] (CP& p) { p.width = 100; }, [] (CP& p) { p.widthLo = 0.03f; }, false, 1.0e-6f },
            { "echoLo 0 -> 0.03 (e 50)",  [] (CP& p) { p.echo = 50; },   [] (CP& p) { p.echoLo = 0.03f; },  false, 1.0e-3f },
            { "reverbLo 0 -> 0.03 (r 50)", [] (CP& p) { p.reverb = 50; }, [] (CP& p) { p.reverbLo = 0.03f; }, false, 1.0e-3f },
        };
        const auto in = bandSine(1.5);
        const int sw = 44544;                                   // the block boundary just past 1 s
        for (const auto& st : steps)
        {
            CP from = neutralChain(); st.base(from);
            CP to = from; st.engage(to);
            const auto stepped = runBand(in, [&] (int i) { return i < sw ? from : to; }, st.eq);
            const auto held    = runBand(in, [&] (int) { return to; }, st.eq);
            const double click = hfPeakDb(stepped, sw - 2205, sw + 4410);   // -50 ms .. +100 ms
            float settled = 0.0f;                                // the stepped run lands on the held one
            for (int ch = 0; ch < 2; ++ch)
                for (int i = sw + 8820 + bandLat; i < in.getNumSamples(); ++i)
                    settled = juce::jmax(settled, std::abs(stepped.getSample(ch, i) - held.getSample(ch, i)));
            if (click > -60.0 || settled > st.settleTol)
                std::cout << "    " << st.name << ": HF " << click << " dBFS, off the held run by " << settled << std::endl;
            CHECK(click < -60.0);
            CHECK(settled <= st.settleTol);
        }
    }

    std::cout << "bands: a fast handle sweep does not zipper (F18)" << std::endl;
    {
        // 0 -> full -> 0 in 50 ms from 1 s, pushed per block: the sweep LoopNestMeasure times.
        auto tri = [bandSr] (int i) { const double u = (i / bandSr - 1.0) / 0.05;
                                      return (u <= 0.0 || u >= 1.0) ? 0.0 : (u < 0.5 ? 2.0 * u : 2.0 - 2.0 * u); };
        struct Sweep { const char* name; std::function<void(CP&, double)> set; bool eq; };
        const std::vector<Sweep> sweeps = {
            { "eqLo 0..0.6",          [] (CP& p, double v) { p.eqLo = (float) (0.6 * v); }, true },
            { "eqHi 1..0.4",          [] (CP& p, double v) { p.eqHi = (float) (1.0 - 0.6 * v); }, true },
            { "echoHi 1..0.2 (e 50)", [] (CP& p, double v) { p.echo = 50; p.echoHi = (float) (1.0 - 0.8 * v); }, false },
            { "reverbLo 0..0.8 (r 50)", [] (CP& p, double v) { p.reverb = 50; p.reverbLo = (float) (0.8 * v); }, false },
            { "widthLo 0..0.8 (w 100)", [] (CP& p, double v) { p.width = 100; p.widthLo = (float) (0.8 * v); }, false },
        };
        const auto in = bandSine(1.2);
        for (const auto& s : sweeps)
        {
            const auto out = runBand(in, [&] (int i) { CP p = neutralChain(); s.set(p, tri(i)); return p; }, s.eq);
            const double hf = hfPeakDb(out, (int) bandSr, in.getNumSamples());
            if (hf > -75.0) std::cout << "    " << s.name << ": HF " << hf << " dBFS" << std::endl;
            CHECK(hf < -75.0);
        }
    }

    std::cout << "bands: a handle's frequency is the cut's -3 dB corner at 44.1, 48 and 96 kHz (F19)" << std::endl;
    {
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            for (int k = 0; k < 9; ++k)
            {
                const bool low = k < 4;
                const float pos = low ? std::array<float, 4> { 0.1f, 0.5f, 0.75f, 0.9f }[(size_t) k]
                                      : std::array<float, 5> { 0.1f, 0.5f, 0.75f, 0.9f, 0.96f }[(size_t) (k - 4)];   // 0.96..0.98 blends in
                const double handleHz = 20.0 * std::pow(1000.0, (double) pos);
                // The EQ's impulse response, settled at these handles from the first frame.
                CharacterChain ch;
                ch.prepare(sr);
                CP p = neutralChain();
                (low ? p.eqLo : p.eqHi) = pos;
                ch.setParams(p);
                std::vector<double> h(1 << 16);
                for (size_t i = 0; i < h.size(); ++i)
                {
                    float x[2] = { i == 0 ? 1.0f : 0.0f, 0.0f };
                    ch.processEq(x, 2);
                    h[i] = x[0];
                }
                auto gainDb = [&] (double f)
                {
                    const std::complex<double> step = std::polar(1.0, -juce::MathConstants<double>::twoPi * f / sr);
                    std::complex<double> ph(1.0, 0.0), acc(0.0, 0.0);
                    for (double v : h) { acc += v * ph; ph *= step; }
                    return 20.0 * std::log10(std::abs(acc));
                };
                // The -3 dB crossing, bisected on a log axis within an octave either side.
                double lo = handleHz * 0.5, hi = juce::jmin(handleHz * 2.0, 0.499 * sr);
                for (int it = 0; it < 40; ++it)
                {
                    const double mid = std::sqrt(lo * hi);
                    const bool passing = gainDb(mid) > -3.0103;
                    if (passing == low) hi = mid; else lo = mid;
                }
                const double corner = std::sqrt(lo * hi), err = corner / handleHz - 1.0;
                if (std::abs(err) > 0.03)
                    std::cout << "    " << sr << " Hz, " << (low ? "eqLo " : "eqHi ") << pos << ": -3 dB at "
                              << corner << " Hz, handle " << handleHz << " Hz" << std::endl;
                CHECK(std::abs(err) <= 0.03);
            }
        }
    }

    std::cout << "bands: the high cut slides open over its last 0.02 of travel, no step to open at 0.98" << std::endl;
    {
        // Level at 18 kHz as the eqHi handle runs 0.95 -> 0.98 in 0.001 steps: it only
        // rises, and no step between neighbours exceeds 1 dB.
        auto at18k = [] (float pos)
        {
            CharacterChain ch;
            ch.prepare(44100.0);
            CP p = neutralChain();
            p.eqHi = pos;
            ch.setParams(p);
            const double w = juce::MathConstants<double>::twoPi * 18000.0 / 44100.0;
            double re = 0.0, im = 0.0;
            for (int i = 0; i < (1 << 14); ++i)
            {
                float x[2] = { i == 0 ? 1.0f : 0.0f, 0.0f };
                ch.processEq(x, 2);
                re += x[0] * std::cos(w * i); im -= x[0] * std::sin(w * i);
            }
            return 10.0 * std::log10(re * re + im * im);
        };
        double prev = at18k(0.95f), worst = 0.0;
        bool rising = true;
        for (int k = 951; k <= 980; ++k)
        {
            const double g = at18k((float) k / 1000.0f);
            worst = juce::jmax(worst, std::abs(g - prev));
            rising = rising && g >= prev - 1.0e-3;
            prev = g;
        }
        if (worst > 1.0 || ! rising) std::cout << "    worst step " << worst << " dB, rising " << rising << std::endl;
        CHECK(rising && worst <= 1.0 && std::abs(prev) < 1.0e-3);
    }

    std::cout << "WIDTH raised from 0 starts its band filters from the live input, not a held state" << std::endl;
    {
        // A: WIDTH 70 for 1 s, 0 for 1 s, 70 again. B: 0 for 2 s, then 70. The side's
        // band filters keep running at width 0, so by the re-engage both runs carry the
        // same filter state and their onsets match; a filter frozen at width 0 replays
        // what it held at 1 s.
        CP up = neutralChain();
        up.width = 70; up.widthLo = 0.2f; up.widthHi = 0.8f;
        CP rest = up; rest.width = 0;
        const auto in = noise(132300, 31);
        const int s1 = 44032, s2 = 88064;                       // block boundaries near 1 s and 2 s
        auto run = [&] (bool upFirst)
        {
            CharacterChain ch;
            ch.prepare(44100.0);
            juce::AudioBuffer<float> out(2, in.getNumSamples());
            for (int i = 0; i < in.getNumSamples(); ++i)
            {
                if (i % 512 == 0) ch.setParams(i >= s2 || (upFirst && i < s1) ? up : rest);
                float x[2] = { in.getSample(0, i), in.getSample(1, i) };
                ch.process(x, 2);
                out.setSample(0, i, x[0]); out.setSample(1, i, x[1]);
            }
            return out;
        };
        const auto a = run(true), b = run(false);
        float onset = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = s2; i < s2 + 4410; ++i)
                onset = juce::jmax(onset, std::abs(a.getSample(ch, i) - b.getSample(ch, i)));
        if (onset > 1.0e-5f)
            std::cout << "    onset off by " << juce::Decibels::gainToDecibels(onset, -200.0f) << " dBFS" << std::endl;
        CHECK(onset <= 1.0e-5f);
    }

    std::cout << "REVERB at 0 rings out instead of freezing, then clears (F20)" << std::endl;
    {
        CP on = neutralChain();
        on.reverb = 70; on.reverbDecay = 80;
        CP zero = on; zero.reverb = 0;
        juce::AudioBuffer<float> quiet(2, (int) bandSr);
        quiet.clear();
        auto silence = [] (int n) { juce::AudioBuffer<float> buf(2, n); buf.clear(); return buf; };
        // The tail heard on raising REVERB again after `heldAt0` seconds at 0, silence in;
        // and the output while at 0, once the amount's slew has landed (200 ms on).
        auto backAfter = [&] (double heldAt0, float& whileAt0)
        {
            CharacterChain ch;
            ch.prepare(bandSr);
            runChain(ch, on, noise((int) bandSr, 400));
            const int n = (int) (heldAt0 * bandSr);
            whileAt0 = peakFrom(runChain(ch, zero, silence(n)), juce::jmin(n, (int) (0.2 * bandSr)));
            return peakFrom(runChain(ch, on, quiet), 0);
        };
        float at0a = 0.0f, at0b = 0.0f, at0c = 0.0f;
        const float soon = backAfter(0.025, at0a), later = backAfter(2.0, at0b), long_ = backAfter(20.0, at0c);
        std::cout << "    tail on raising after 25 ms " << juce::Decibels::gainToDecibels(soon)
                  << " dBFS, after 2 s " << juce::Decibels::gainToDecibels(later)
                  << ", after 20 s " << juce::Decibels::gainToDecibels(long_, -200.0f) << std::endl;
        CHECK(soon > 1.0e-3f);                     // a short dip keeps the tail …
        CHECK(later < soon * 0.1f);                // … which kept decaying at 0 (frozen = the same level)
        CHECK(long_ == 0.0f);                      // rung out and cleared: nothing comes back
        CHECK(at0a == 0.0f && at0b == 0.0f && at0c == 0.0f);   // at 0 the output stays exactly dry
    }

    // --- Taste rulings 10-08: WARP / FLUTTER depth, DRIVE TONE, REVERB decay ---

    // The pitch of a 1 kHz sine (-6 dBFS) through the chain, cents re 1 kHz: its
    // highest and lowest one-cycle reading from `fromSec` on (positive-going zero
    // crossings, interpolated), left channel.
    auto centsRange = [bandSr, &runBand] (const std::function<CP(int)>& at, double seconds, double fromSec, double& up, double& down)
    {
        juce::AudioBuffer<float> in(2, (int) (seconds * bandSr));
        for (int i = 0; i < in.getNumSamples(); ++i)
        {
            const float v = 0.5f * (float) std::sin(juce::MathConstants<double>::twoPi * 1000.0 * i / bandSr);
            in.setSample(0, i, v); in.setSample(1, i, v);
        }
        const auto out = runBand(in, at, false);
        const float* d = out.getReadPointer(0);
        up = -1.0e9; down = 1.0e9;
        double last = -1.0;
        for (int i = 1; i < out.getNumSamples(); ++i)
            if (d[i - 1] < 0.0f && d[i] >= 0.0f)
            {
                const double tc = ((double) (i - 1) - (double) d[i - 1] / ((double) d[i] - (double) d[i - 1])) / bandSr;
                if (last >= 0.0 && tc >= fromSec)
                {
                    const double cents = 1200.0 * std::log2(1.0 / ((tc - last) * 1000.0));
                    up = juce::jmax(up, cents); down = juce::jmin(down, cents);
                }
                last = tc;
            }
    };
    auto within = [] (double v, double want, double rel) { return std::abs(v - want) <= std::abs(want) * rel; };

    // WARP / FLUTTER retapered (Andrew 10-17): the knob's lower half is v2's old whole knob,
    // 100 % is the 10-17 spike's sweep (organic: 35 % random drift, so the peaks are read with
    // some room either way of the pure-sine numbers).
    // WARP's v2 sweep (the old full knob, ms) and the pure-sine cents a sweep gives, restated.
    auto warpHz = [] (float ratePct) { return 0.15 + 1.45 * ratePct * 0.01; };
    auto oldTopMs = [&] (float ratePct) { return std::min(12.0, 1000.0 * 0.035 / (juce::MathConstants<double>::twoPi * warpHz(ratePct))); };
    auto sweepCents = [&] (double ms, float ratePct, bool up)
    { const double d = juce::MathConstants<double>::twoPi * warpHz(ratePct) * 0.001 * ms; return 1200.0 * std::log2(up ? 1.0 + d : 1.0 - d); };
    auto grid = [] (float v) { return std::round(v * 2.0f) * 0.5f; };   // the knob's 0.5 % step

    std::cout << "WARP: the knob's depth follows its taper (50 % = the old 100 % from RATE ~45 % up), 100 % the spike's (~+90/-100 at rate 40, ~+195/-235 at 100)" << std::endl;
    {
        struct Want { float rate, amt; double tol; };
        for (const Want& w : { Want { 0, 50, 0.2 }, Want { 10, 50, 0.2 }, Want { 50, 50, 0.15 }, Want { 100, 50, 0.15 },
                               Want { 40, 100, 0.15 }, Want { 100, 100, 0.15 } })
        {
            const double ms = CharacterChain::warpSweepMs(w.amt, w.rate);
            double up, down;
            centsRange([&] (int) { CP p = neutralChain(); p.warp = w.amt; p.warpRate = w.rate; return p; }, 15.0, 1.0, up, down);
            const bool ok = within(up, sweepCents(ms, w.rate, true), w.tol) && within(down, sweepCents(ms, w.rate, false), w.tol);
            if (! ok) std::cout << "    rate " << w.rate << ", amount " << w.amt << ": " << up << " / " << down << " cents, claim "
                                << sweepCents(ms, w.rate, true) << " / " << sweepCents(ms, w.rate, false) << std::endl;
            CHECK(ok);
        }
        // The middle is the old top at fast rates; the top is the spike's 12 ms everywhere.
        CHECK(within(CharacterChain::warpSweepMs(50, 100), oldTopMs(100), 0.02));
        CHECK(within(CharacterChain::warpSweepMs(50, 50), oldTopMs(50), 0.05));
        for (float rate : { 0.0f, 10.0f, 22.0f, 40.0f, 100.0f })
        {
            CHECK(within(CharacterChain::warpSweepMs(100, rate), 12.0, 1.0e-6));
            // No dead travel: the top half adds at least 30 % more sweep, at every rate …
            const double mid = CharacterChain::warpSweepMs(50, rate), top = CharacterChain::warpSweepMs(100, rate);
            if (top < 1.3 * mid) std::cout << "    rate " << rate << ": 50 % " << mid << " ms, 100 % " << top << " ms" << std::endl;
            CHECK(top >= 1.3 * mid);
            // … the sweep rises with the knob at every step, and never grows as the rate rises
            double last = 0.0;
            for (float amt = 2.5f; amt <= 100.0f; amt += 2.5f)
            {
                const double ms = CharacterChain::warpSweepMs(amt, rate);
                CHECK(ms > last && ms <= CharacterChain::warpSweepMs(amt, juce::jmax(0.0f, rate - 5.0f)) + 1.0e-9);
                last = ms;
            }
            // and so does the pitch heard (rate 0 / 10 / 22 / 40 / 100, knob 25 .. 100)
            double lastUp = 0.0, lastDown = 0.0;
            for (float amt : { 25.0f, 50.0f, 75.0f, 100.0f })
            {
                double up, down;
                centsRange([&] (int) { CP p = neutralChain(); p.warp = amt; p.warpRate = rate; return p; }, 12.0, 1.0, up, down);
                if (! (up > lastUp && down < lastDown)) std::cout << "    rate " << rate << ", amount " << amt << ": " << up << " / " << down << " cents" << std::endl;
                CHECK(up > lastUp && down < lastDown);
                lastUp = up; lastDown = down;
            }
        }
        // RATE 0 -> 100 and 100 -> 0 in one step, at amount 100: the sweep re-centres without a
        // pitch bomb (nothing past the faster rate's own swing, +195 / -235, and a margin).
        for (bool slowToFast : { true, false })
        {
            double up, down;
            centsRange([&] (int i) { CP p = neutralChain(); p.warp = 100;
                                     p.warpRate = (i < 4 * (int) bandSr) == slowToFast ? 0.0f : 100.0f; return p; },
                       10.0, 1.0, up, down);
            if (up > 230.0 || down < -275.0) std::cout << "    rate step: " << up << " / " << down << " cents" << std::endl;
            CHECK(up <= 230.0 && down >= -275.0);
        }
        // The knob stepped 100 -> 50 at rate 40: the pitch settles onto the middle's swing, no spike.
        {
            double up, down;
            centsRange([&] (int i) { CP p = neutralChain(); p.warpRate = 40; p.warp = i < 4 * (int) bandSr ? 100.0f : 50.0f; return p; },
                       10.0, 4.0, up, down);
            if (up > 80.0 || down < -80.0) std::cout << "    knob step: " << up << " / " << down << " cents" << std::endl;
            CHECK(up <= 80.0 && down >= -80.0);
        }
    }

    std::cout << "WARP: an old (pre-4) setting converts to the knob that gives its old sweep, at any RATE" << std::endl;
    {
        double worst = 0.0;
        for (float rate : { 0.0f, 10.0f, 22.0f, 40.0f, 70.0f, 100.0f })
            for (float old : { 10.0f, 25.0f, 50.0f, 75.0f, 100.0f })
            {
                const double want = old * 0.01 * oldTopMs(rate);
                const double got  = CharacterChain::warpSweepMs(grid(CharacterChain::warpFromOld(old, rate)), rate);
                worst = juce::jmax(worst, std::abs(got / want - 1.0));
            }
        std::cout << "    worst sweep error after the 0.5 % grid: " << 100.0 * worst << " %" << std::endl;
        CHECK(worst <= 0.03);
    }

    // A one-block move of a control on the 220 Hz sine: peak above 5 kHz from 10 ms before to
    // `after` s (150 ms unless given) after it, against the louder of the two settings run steady.
    // A click is LoopNestMeasure's zipper flag: more than 6 dB over steady AND above -60 dBFS.
    auto clickCheck = [&] (const char* what, const std::function<CP(bool)>& at, double after = 0.15)
    {
        const auto in = bandSine(3.0);
        const int sw = 172 * 512, from = sw - (int) (0.01 * bandSr), to = sw + (int) (after * bandSr);
        const double stepped = hfPeakDb(runBand(in, [&] (int i) { return at(i >= sw); }, false), from, to);
        const double steady  = juce::jmax(hfPeakDb(runBand(in, [&] (int) { return at(false); }, false), from, to),
                                          hfPeakDb(runBand(in, [&] (int) { return at(true); }, false), from, to));
        const bool click = stepped - steady > 6.0 && stepped > -60.0;
        std::cout << "    " << what << ": " << stepped << " dBFS above 5 kHz, steady " << steady << (click ? "  <- click" : "") << std::endl;
        CHECK(! click);
    };

    std::cout << "WARP / FLUTTER: switched off in one block at RATE 100 (the wet fades out, the read never jumps): no click" << std::endl;
    {
        clickCheck("WARP 100 -> 0, rate 100", [] (bool after) { CP p = neutralChain(); p.warp = after ? 0.0f : 100.0f; p.warpRate = 100; return p; });
        clickCheck("WARP 100 -> 0, rate 0", [] (bool after) { CP p = neutralChain(); p.warp = after ? 0.0f : 100.0f; p.warpRate = 0; return p; });
        clickCheck("FLUTTER 100 -> 0, rate 100", [] (bool after) { CP p = neutralChain(); p.flutter = after ? 0.0f : 100.0f; p.flutterRate = 100; return p; });
        clickCheck("WARP 0 -> 100, rate 100", [] (bool after) { CP p = neutralChain(); p.warp = after ? 100.0f : 0.0f; p.warpRate = 100; return p; });
    }

    std::cout << "REVERB: raised from 0 in one block (the tank's input rises, ~30 ms): no click, every type" << std::endl;
    {
        const char* const names[] = { "PLATE", "HALL", "SPRING" };
        for (int type = 0; type < 3; ++type)
            clickCheck((juce::String("REVERB 0 -> 100, ") + names[type]).toRawUTF8(),
                       [type] (bool after) { CP p = neutralChain(); p.reverb = after ? 100.0f : 0.0f; p.reverbType = (float) type; return p; });
    }

    std::cout << "WARP: the read never leaves its base +/- 15 ms (the chain's fixed delay +/- 15 ms; 10-08, ruling 3)" << std::endl;
    {
        // An impulse every 100 ms through WARP 100 alone: each must come out within 15 ms of
        // the chain's fixed delay (WARP's base 16 +/- 15 plus FLUTTER's base, the Hermite
        // read's 2 frames either side), nothing outside.
        for (float rate : { 0.0f, 40.0f, 100.0f })
        {
            juce::AudioBuffer<float> in(2, (int) (20.0 * bandSr));
            in.clear();
            const int period = (int) (0.1 * bandSr);
            for (int i = period; i < in.getNumSamples(); i += period) { in.setSample(0, i, 1.0f); in.setSample(1, i, 1.0f); }
            const auto out = runBand(in, [&] (int) { CP p = neutralChain(); p.warp = 100; p.warpRate = rate; return p; }, false);
            const int lo = bandLat - (int) std::ceil(0.015 * bandSr) - 2, hi = bandLat + (int) std::ceil(0.015 * bandSr) + 2;
            float outside = 0.0f;
            for (int i = period; i < out.getNumSamples(); ++i)
            {
                const int k = (i - period) % period;                // frames since the last impulse
                if (k < lo || k > hi)
                    outside = juce::jmax(outside, std::abs(out.getSample(0, i)), std::abs(out.getSample(1, i)));
            }
            if (outside > 0.0f) std::cout << "    rate " << rate << ": " << outside << " outside the latency +/- 15 ms" << std::endl;
            CHECK(outside == 0.0f);
        }
    }

    std::cout << "FLUTTER: amount 50 is the old 100 (~+/-35 cents at rate 50, ~+/-70 at 100), 100 the spike's (~+95/-102 at rate 50)" << std::endl;
    {
        struct Want { float rate, amt; double up, down; };
        for (const Want& w : { Want { 50, 50, 35.0, -35.8 }, Want { 100, 50, 69.4, -72.3 }, Want { 50, 100, 95.0, -102.0 } })
        {
            double up, down;
            centsRange([&] (int) { CP p = neutralChain(); p.flutter = w.amt; p.flutterRate = w.rate; return p; }, 12.0, 1.0, up, down);
            const bool ok = within(up, w.up, 0.15) && within(down, w.down, 0.15);
            if (! ok) std::cout << "    rate " << w.rate << ", amount " << w.amt << ": " << up << " / " << down << " cents" << std::endl;
            CHECK(ok);
        }
    }

    // BS.1770 K-weighted loudness (dB), both channels' mean squares, of frames [from, end).
    auto kLoudness = [bandSr] (const juce::AudioBuffer<float>& buf, int from)
    {
        struct Bq { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
                    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
        const double pi = juce::MathConstants<double>::pi;
        auto shelf = [&]
        {
            const double K = std::tan(pi * 1681.974450955533 / bandSr), Q = 0.7071752369554196;
            const double Vh = std::pow(10.0, 3.999843853973347 / 20.0), Vb = std::pow(Vh, 0.4996667741545416), a0 = 1.0 + K / Q + K * K;
            return Bq { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                        2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        };
        auto hipass = [&]
        {
            const double K = std::tan(pi * 38.13547087602444 / bandSr), Q = 0.5003270373238773, a0 = 1.0 + K / Q + K * K;
            return Bq { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        };
        double sum = 0.0;
        for (int ch = 0; ch < 2; ++ch)
        {
            Bq s = shelf(), h = hipass();
            for (int i = 0; i < buf.getNumSamples(); ++i)
            {
                const double v = h.run(s.run(buf.getSample(ch, i)));
                if (i >= from) sum += v * v;
            }
        }
        return 10.0 * std::log10(sum / (2.0 * (buf.getNumSamples() - from)) + 1.0e-20);
    };
    // Power of d[0, 2^16) in [loHz, hiHz) (radix-2 FFT, no window: white noise has no lines to leak).
    auto bandPower = [bandSr] (const float* d, double loHz, double hiHz)
    {
        const size_t n = 1 << 16;
        std::vector<std::complex<double>> sp(n);
        for (size_t i = 0; i < n; ++i) sp[i] = d[i];
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(sp[i], sp[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const auto wl = std::polar(1.0, -juce::MathConstants<double>::twoPi / (double) len);
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<double> w(1.0, 0.0);
                for (size_t k = 0; k < len / 2; ++k, w *= wl)
                {
                    const auto u = sp[i + k], v = sp[i + k + len / 2] * w;
                    sp[i + k] = u + v; sp[i + k + len / 2] = u - v;
                }
            }
        }
        double s = 0.0;
        for (size_t k = 1; k < n / 2; ++k)
        {
            const double f = (double) k * bandSr / (double) n;
            if (f >= loHz && f < hiHz) s += std::norm(sp[k]);
        }
        return s;
    };

    std::cout << "DRIVE TONE: +/-6 dB tilt at 1.8 kHz on white noise, level-neutral, 0 = true neutral" << std::endl;
    {
        const int n = (int) (4.0 * bandSr), from = (int) (2.0 * bandSr);
        juce::AudioBuffer<float> wn(2, n);
        juce::Random rng(3);
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.5f * (rng.nextFloat() * 2.0f - 1.0f);
            wn.setSample(0, i, v); wn.setSample(1, i, v);
        }
        // A drum-ish loop: a pitch-dropping kick on the beats, noise snares on 2 and 4,
        // short hats between (two bars at 120 bpm, twice): much darker than white noise.
        juce::AudioBuffer<float> drums(2, n);
        drums.clear();
        juce::Random dr(7);
        const int beat = (int) (0.5 * bandSr);
        for (int k = 0; k < 16; ++k)
        {
            for (int i = 0; i < (int) (0.25 * bandSr); ++i)
            {
                const double t = i / bandSr;
                const float v = 0.6f * (float) (std::sin(juce::MathConstants<double>::twoPi * (50.0 * t + 6.0 * (1.0 - std::exp(-t * 30.0))))
                                                * std::exp(-t * 14.0));
                drums.addSample(0, k * beat + i, v); drums.addSample(1, k * beat + i, v);
            }
            for (int i = 0; i < (int) ((k % 2 == 1 ? 0.2 : 0.05) * bandSr); ++i)
            {
                const float v = (k % 2 == 1 ? 0.4f : 0.0f) * (dr.nextFloat() * 2.0f - 1.0f) * (float) std::exp(-i / (0.05 * bandSr));
                drums.addSample(0, k * beat + i, v); drums.addSample(1, k * beat + i, v);
            }
            for (int i = 0; i < (int) (0.05 * bandSr); ++i)
            {
                const float v = 0.15f * (dr.nextFloat() * 2.0f - 1.0f) * (float) std::exp(-i / (0.01 * bandSr));
                drums.addSample(0, k * beat + beat / 2 + i, v); drums.addSample(1, k * beat + beat / 2 + i, 0.6f * v);
            }
        }
        auto run = [&] (const juce::AudioBuffer<float>& in, float drive, float tone)
        {
            return runBand(in, [&] (int) { CP p = neutralChain(); p.drive = drive; p.driveTone = tone; return p; }, false);
        };
        const double pinLo = bandPower(wn.getReadPointer(0) + from, 200.0, 1800.0), pinHi = bandPower(wn.getReadPointer(0) + from, 1800.0, 16000.0);
        const auto flat = run(wn, 100, 0), flatDrums = run(drums, 100, 0);
        for (float tone : { -100.0f, 100.0f })
        {
            const auto o = run(wn, 100, tone);
            const double lo = 10.0 * std::log10(bandPower(o.getReadPointer(0) + from, 200.0, 1800.0) / pinLo);
            const double hi = 10.0 * std::log10(bandPower(o.getReadPointer(0) + from, 1800.0, 16000.0) / pinHi);
            const double tilt = hi - lo, want = tone > 0 ? 6.0 : -6.0;
            const double dNoise = kLoudness(o, from) - kLoudness(flat, from);
            const double dDrums = kLoudness(run(drums, 100, tone), from) - kLoudness(flatDrums, from);
            if (std::abs(tilt - want) > 1.0 || std::abs(dNoise) > 0.5 || std::abs(dDrums) > 0.5)
                std::cout << "    tone " << tone << ": tilt " << tilt << " dB, loudness change " << dNoise
                          << " dB (noise), " << dDrums << " dB (drums)" << std::endl;
            CHECK(std::abs(tilt - want) <= 1.0);
            CHECK(std::abs(dNoise) <= 0.5);
            CHECK(std::abs(dDrums) <= 0.5);
        }
        // TONE moved and brought back to 0: once its slew lands, bit for bit the chain that
        // never moved it (the tilt leaves nothing behind at 0, DRIVE's own gain untouched).
        const auto back = runBand(drums, [&] (int i) { CP p = neutralChain(); p.drive = 100;
                                                       p.driveTone = i < (int) bandSr ? 80.0f : 0.0f; return p; }, false);
        CHECK(identical(back, flatDrums, (int) (1.5 * bandSr)));
    }

    std::cout << "DRIVE: anti-aliased (5 kHz sine, -6 dBFS: DRIVE 100 % under -40 dB off its harmonics), exact at 0, no click in or out" << std::endl;
    {
        // LoopNestMeasure's aliasReport: a 5 kHz sine's harmonics below Nyquist sit on
        // exact bins of a 1 s window at 44.1 kHz; everything else is folded-back
        // distortion. The second second is read (the chain settled on the first).
        const int n = (int) bandSr;
        juce::AudioBuffer<float> sine5k(2, 2 * n);
        for (int i = 0; i < 2 * n; ++i)
        {
            const float v = 0.5f * (float) std::sin(juce::MathConstants<double>::twoPi * 5000.0 * i / bandSr);
            sine5k.setSample(0, i, v); sine5k.setSample(1, i, v);
        }
        auto aliasDb = [&] (float drive)
        {
            const auto o = runBand(sine5k, [drive] (int) { CP p = neutralChain(); p.drive = drive; return p; }, false);
            const float* d = o.getReadPointer(0, n);
            double total = 0.0, harm = 0.0;
            for (int i = 0; i < n; ++i) total += (double) d[i] * d[i];
            for (int h = 0; h <= 4; ++h)
            {
                double re = 0.0, im = 0.0;
                const double w = juce::MathConstants<double>::twoPi * 5000.0 * h / bandSr;
                for (int i = 0; i < n; ++i) { re += d[i] * std::cos(w * i); im += d[i] * std::sin(w * i); }
                harm += (h == 0 ? 1.0 : 2.0) * (re * re + im * im) / n;
            }
            return 10.0 * std::log10(juce::jmax(1.0e-20, total - harm) / total);
        };
        const double alias100 = aliasDb(100.0f);
        std::cout << "    DRIVE 100 %: " << alias100 << " dB off the harmonics" << std::endl;
        CHECK(alias100 < -40.0);

        // 0 % is the stage skipped: bit for bit the input, from a fresh chain and once a
        // move to 50 % has slewed back (the engage fade lands on 0, nothing left running).
        const auto drums = noise(2 * n, 11);
        CHECK(identical(runBand(drums, [] (int) { CP p = neutralChain(); p.drive = 0; return p; }, false), delayed(drums, bandLat)));
        const auto back = runBand(drums, [n] (int i) { CP p = neutralChain(); p.drive = i < n / 2 ? 50.0f : 0.0f; return p; }, false);
        CHECK(identical(back, delayed(drums, bandLat), n));

        // The 2x path delays the signal, so engaging or releasing must fade, not switch. A
        // knob's fade lands ~170 ms after the move, so the release window runs to 300 ms.
        clickCheck("DRIVE 0 -> 1", [] (bool after) { CP p = neutralChain(); p.drive = after ? 1.0f : 0.0f; return p; });
        clickCheck("DRIVE 0 -> 100", [] (bool after) { CP p = neutralChain(); p.drive = after ? 100.0f : 0.0f; return p; });
        clickCheck("DRIVE 1 -> 0", [] (bool after) { CP p = neutralChain(); p.drive = after ? 0.0f : 1.0f; return p; }, 0.3);
        clickCheck("DRIVE 100, panel off", [] (bool after) { CP p = neutralChain(); p.drive = 100; p.on[CP::kDrive] = ! after; return p; }, 0.3);
        clickCheck("DRIVE 100, panel on", [] (bool after) { CP p = neutralChain(); p.drive = 100; p.on[CP::kDrive] = after; return p; }, 0.3);

        // Fold-back in the audible band (LoopNestMeasure's driveFoldReport): 5 kHz at 44.1 kHz
        // is a kind case for the total above, so tones across 2.5..9 kHz at 44.1 and 48 kHz,
        // the power at every harmonic's fold below 15 kHz, re the output.
        double worstFold = -200.0;
        for (double sr : { 44100.0, 48000.0 })
            for (double f : { 2500.0, 3500.0, 4500.0, 5500.0, 7000.0, 9000.0 })
            {
                const int m = (int) sr;
                CharacterChain ch;
                ch.prepare(sr);
                CP p = neutralChain();
                p.drive = 100;
                juce::AudioBuffer<float> in(2, 2 * m);
                for (int i = 0; i < 2 * m; ++i)
                {
                    const float v = 0.5f * (float) std::sin(juce::MathConstants<double>::twoPi * f * i / sr);
                    in.setSample(0, i, v); in.setSample(1, i, v);
                }
                const auto o = runChain(ch, p, in);
                const float* d = o.getReadPointer(0, m);
                double total = 0.0;
                for (int i = 0; i < m; ++i) total += (double) d[i] * d[i];
                const long fi = (long) f, fs = (long) sr;
                std::vector<long> bins;
                for (long k = 2; k <= 400; ++k)
                {
                    const long h = k * fi;
                    if (2 * h < fs) continue;
                    const long r = h % fs, fold = r > fs / 2 ? fs - r : r;
                    if (fold >= 15000 || fold % fi == 0) continue;
                    if (std::find(bins.begin(), bins.end(), fold) == bins.end()) bins.push_back(fold);
                }
                double e = 0.0;
                for (long b : bins)
                {
                    double re = 0.0, im = 0.0;
                    const double w = juce::MathConstants<double>::twoPi * (double) b / sr;
                    for (int i = 0; i < m; ++i) { re += d[i] * std::cos(w * i); im += d[i] * std::sin(w * i); }
                    e += 2.0 * (re * re + im * im) / m;
                }
                worstFold = juce::jmax(worstFold, 10.0 * std::log10(juce::jmax(1.0e-20, e) / total));
            }
        std::cout << "    worst fold-back below 15 kHz (2.5..9 kHz tones, 44.1 / 48 kHz): " << worstFold << " dB" << std::endl;
        CHECK(worstFold < -40.0);

        // A huge finite sample (the float range's edge) never turns the stage non-finite, so
        // no NaN reaches the DC blocker, the tilt, the echo ring or a reverb tank.
        {
            CharacterChain ch;
            ch.prepare(bandSr);
            CP p = neutralChain();
            p.drive = 100; p.echo = 50; p.reverb = 50;
            auto in = noise(n, 13);
            for (int i : { 1000, 1001, 5000 })
                for (int c = 0; c < 2; ++c) in.setSample(c, i, (i % 2 == 0 ? 3.0e38f : -3.0e38f));
            const auto o = runChain(ch, p, in);
            bool finite = true;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i) finite = finite && std::isfinite(o.getSample(c, i));
            CHECK(finite);
        }

        // reset() leaves the chain as a fresh one: the same input after it prints bit for bit
        // what a freshly prepared chain prints (the 2x paths start settled in both), from the
        // input's first frame out of the fixed delay on. (Before it, DRIVE runs on the delay
        // lines' silence with its gain from the prior reset() keeps (F10): ~1e-9, -180 dB.)
        {
            CP p = neutralChain();
            p.drive = 100;
            const auto in2 = noise(n, 15);
            CharacterChain used, fresh;
            used.prepare(bandSr);
            fresh.prepare(bandSr);
            runChain(used, p, noise(n / 2, 14));
            used.reset();
            CHECK(identical(runChain(used, p, in2), runChain(fresh, p, in2), bandLat));
        }
    }

    std::cout << "DRIVE under MIX: the dry tap follows DRIVE's phase, so DRIVE 1 % at MIX 50 % has no notch (flat within 0.5 dB, 1-20 kHz, 44.1 / 48 kHz)" << std::endl;
    {
        // The real processor in FX, every other control neutral: a -40 dBFS sine in, its gain
        // out (the second of two seconds, f on an exact bin). A raw dry tap against the 2x path
        // notched ~-39 dB at 16.6 kHz (44.1) / 18 kHz (48).
        auto gainAt = [] (double sr, float drive, float mix, double f)
        {
            LoopNestProcessor p;
            setNatural(p, "drive", drive);
            setNatural(p, "mix", mix);
            p.prepareToPlay(sr, 512);
            p.setFxMode(true);
            const int m = (int) sr, blocks = (2 * m + 511) / 512;
            const double amp = 0.01;
            juce::AudioBuffer<float> buf(2, 512);
            juce::MidiBuffer midi;
            double re = 0.0, im = 0.0;
            const double w = juce::MathConstants<double>::twoPi * f / sr;
            for (int b = 0, i0 = 0; b < blocks; ++b, i0 += 512)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = (float) (amp * std::sin(w * (i0 + i)));
                    buf.setSample(0, i, v); buf.setSample(1, i, v);
                }
                p.processBlock(buf, midi);
                for (int i = 0; i < 512; ++i)
                {
                    const int t = i0 + i - m;
                    if (t < 0 || t >= m) continue;
                    re += buf.getSample(0, i) * std::cos(w * t);
                    im += buf.getSample(0, i) * std::sin(w * t);
                }
            }
            p.releaseResources();
            return 20.0 * std::log10(2.0 * std::sqrt(re * re + im * im) / m / amp);
        };
        for (float mix : { 50.0f, 25.0f, 75.0f })
            for (double sr : { 44100.0, 48000.0 })
            {
                double lo = 100.0, hi = -100.0, loF = 0.0;
                for (double f : { 1000.0, 2000.0, 5000.0, 8000.0, 10000.0, 12000.0, 14000.0, 15000.0, 16000.0,
                                  16600.0, 17000.0, 18000.0, 19000.0, 20000.0 })
                {
                    const double g = gainAt(sr, 1.0f, mix, f);
                    if (g < lo) { lo = g; loF = f; }
                    hi = juce::jmax(hi, g);
                }
                std::cout << "    MIX " << mix << " %, " << sr << " Hz: " << lo << " .. " << hi << " dB (lowest at " << loF << " Hz)" << std::endl;
                CHECK(hi - lo <= 0.5);
            }

        // MIX 0 is still the clean tap with DRIVE engaged: the dry tap's alignment fades out
        // when nothing is blended against it (FX, DRIVE 100, noise in, the last second).
        {
            LoopNestProcessor p;
            setNatural(p, "drive", 100.0f);
            setNatural(p, "mix", 0.0f);
            p.prepareToPlay(44100.0, 512);
            p.setFxMode(true);
            const auto in = noise(2 * 44100, 17);
            juce::AudioBuffer<float> buf(2, 512);
            juce::MidiBuffer midi;
            float worst = 0.0f;
            for (int i0 = 0; i0 + 512 <= in.getNumSamples(); i0 += 512)
            {
                for (int c = 0; c < 2; ++c) buf.copyFrom(c, 0, in, c, i0, 512);
                p.processBlock(buf, midi);
                if (i0 >= 44100)                          // against the input at the reported latency
                    for (int c = 0; c < 2; ++c)
                        for (int i = 0; i < 512; ++i)
                            worst = juce::jmax(worst, std::abs(buf.getSample(c, i) - in.getSample(c, i0 + i - p.getLatencySamples())));
            }
            p.releaseResources();
            std::cout << "    MIX 0, DRIVE 100: max |out - in| = " << worst << std::endl;
            CHECK(worst < 1.0e-5f);
        }
    }

    // T30 of a wet tail: Schroeder's backward integral from the frame after the dry impulse
    // (frame 1000 in, out at 1000 + the chain's fixed delay), -5 to -35 dB, doubled.
    auto tailT30 = [bandSr, bandLat] (const juce::AudioBuffer<float>& o)
    {
        const int at = 1000 + bandLat;
        std::vector<double> e((size_t) (o.getNumSamples() - at), 0.0);
        for (int i = o.getNumSamples() - 1; i > at; --i)
            e[(size_t) (i - at - 1)] = e[(size_t) (i - at)] + (double) o.getSample(0, i) * o.getSample(0, i)
                                                            + (double) o.getSample(1, i) * o.getSample(1, i);
        int i5 = -1, i35 = -1;
        for (size_t i = 0; i < e.size() && i35 < 0; ++i)
        {
            const double db = 10.0 * std::log10(juce::jmax(1.0e-300, e[i]) / e[0]);
            if (i5 < 0 && db <= -5.0) i5 = (int) i;
            if (db <= -35.0) i35 = (int) i;
        }
        return (i5 < 0 || i35 < 0) ? -1.0 : 2.0 * (i35 - i5) / bandSr;
    };
    const char* const typeName[] = { "PLATE", "HALL", "SPRING" };

    std::cout << "REVERB: DECAY 0/25/50/75/100 gives T30 ~0.8 / 1.3 / 2.1 / 3.4 / 5.5 s (PLATE, HALL), ~0.6 / 0.9 / 1.3 / 2.0 / 3.0 s (SPRING)" << std::endl;
    {
        const double want[2][5] = { { 0.8, 1.3, 2.1, 3.4, 5.5 }, { 0.6, 0.9, 1.34, 2.0, 3.0 } };
        for (int type = 0; type < 3; ++type)
            for (int k = 0; k < 5; ++k)
            {
                CP p = neutralChain();
                p.reverb = 100; p.reverbDecay = 25.0f * (float) k; p.reverbType = (float) type;
                juce::AudioBuffer<float> imp(2, (int) (20.0 * bandSr));
                imp.clear();
                imp.setSample(0, 1000, 1.0f); imp.setSample(1, 1000, 1.0f);
                const double t30 = tailT30(runBand(imp, [&] (int) { return p; }, false));
                const double w = want[type == 2 ? 1 : 0][k];
                if (! within(t30, w, 0.1)) std::cout << "    " << typeName[type] << " decay " << 25 * k << " %: T30 " << t30 << " s" << std::endl;
                CHECK(within(t30, w, 0.1));
            }
    }

    std::cout << "REVERB: SPRING sounds the same at 44.1 / 48 / 96 kHz (a print at the file's rate = the audition at the host's)" << std::endl;
    {
        // The wet tail of an impulse (reverb 100, decay 50, 3 s), its energy in bands (RBJ
        // band-passes, Q 2) against its total, and the centroid of those bands.
        const double centres[] = { 250, 500, 1000, 2000, 3000, 4000, 5000, 6000, 8000 };
        auto picture = [&] (double sr, std::vector<double>& db, double& centroid)
        {
            CharacterChain ch;
            ch.prepare(sr);
            CP p = neutralChain();
            p.reverb = 100; p.reverbType = 2;
            const int n = (int) (3.0 * sr), at = 100;
            std::vector<float> wet((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                if (i % 512 == 0) ch.setParams(p);
                float x[2] = { i == at ? 1.0f : 0.0f, i == at ? 1.0f : 0.0f };
                ch.process(x, 2);
                wet[(size_t) i] = i > at + ch.latency() ? x[0] : 0.0f;   // the dry impulse is frame at + latency alone
            }
            double total = 0.0;
            for (float v : wet) total += (double) v * v;
            double num = 0.0, den = 0.0;
            db.clear();
            for (double fc : centres)
            {
                const double w0 = juce::MathConstants<double>::twoPi * fc / sr, al = std::sin(w0) / 4.0, a0 = 1.0 + al;
                const double b0 = al / a0, b2 = -al / a0, a1 = -2.0 * std::cos(w0) / a0, a2 = (1.0 - al) / a0;
                double z1 = 0.0, z2 = 0.0, e = 0.0;
                for (float v : wet) { const double y = b0 * v + z1; z1 = -a1 * y + z2; z2 = b2 * v - a2 * y; e += y * y; }
                db.push_back(10.0 * std::log10(e / total));
                num += fc * e; den += e;
            }
            centroid = num / den;
        };
        std::vector<double> ref, got;
        double refC = 0.0, c = 0.0;
        picture(44100.0, ref, refC);
        for (double sr : { 48000.0, 96000.0 })
        {
            picture(sr, got, c);
            double worst = 0.0;
            for (size_t k = 0; k < ref.size(); ++k) worst = juce::jmax(worst, std::abs(got[k] - ref[k]));
            std::cout << "    " << sr << " Hz: bands within " << worst << " dB of 44.1 kHz, centroid " << c << " Hz (44.1 kHz " << refC << ")" << std::endl;
            CHECK(worst <= 1.5 && within(c, refC, 0.03));
        }
    }

    std::cout << "REVERB TYPE: neutral is exact for every type (at 0, back at 0 after full, panel OFF after full)" << std::endl;
    {
        const auto in = noise((int) (6.0 * bandSr), 77);
        const int sw = (int) (2.0 * bandSr), tail = (int) (3.5 * bandSr);
        for (int type = 0; type < 3; ++type)
        {
            CP base = neutralChain();
            base.reverbType = (float) type; base.reverbDecay = 100; base.reverbLo = 0.8f; base.reverbHi = 0.2f;
            CP full = base;
            full.reverb = 100;
            CP off = full;
            off.on[CP::kReverb] = false;
            const auto dry = delayed(in, bandLat);
            const bool atZero = identical(runBand(in, [&] (int) { return base; }, false), dry);
            const bool back   = identical(runBand(in, [&] (int i) { return i < sw ? full : base; }, false), dry, tail);
            const bool offOk  = identical(runBand(in, [&] (int i) { return i < sw ? full : off; }, false), dry, tail);
            if (! (atZero && back && offOk)) std::cout << "    " << typeName[type] << ": at 0 " << atZero << ", back " << back << ", off " << offOk << std::endl;
            CHECK(atZero && back && offOk);
        }
    }

    std::cout << "REVERB TYPE: a change while it plays (or rings out at 0) crossfades, no click" << std::endl;
    {
        // The 220 Hz sine at REVERB 100: the type stepped at a block boundary. Above 5 kHz the
        // change must add nothing over the two types run steady (a ~30 ms smooth crossfade).
        const auto in = bandSine(3.0);
        const int sw = 172 * 512, from = sw - (int) (0.01 * bandSr), to = sw + (int) (0.1 * bandSr);
        auto run = [&] (int a, int b, float amtAfter)
        {
            return runBand(in, [&] (int i) { CP p = neutralChain(); p.reverb = i < sw ? 100.0f : amtAfter;
                                             p.reverbType = (float) (i < sw ? a : b); return p; }, false);
        };
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
            {
                if (a == b) continue;
                const double stepped = hfPeakDb(run(a, b, 100.0f), from, to);
                const double steady  = juce::jmax(hfPeakDb(run(a, a, 100.0f), from, to), hfPeakDb(run(b, b, 100.0f), from, to));
                if (stepped > steady + 3.0) std::cout << "    " << typeName[a] << " -> " << typeName[b] << ": " << stepped << " dBFS above 5 kHz, steady " << steady << std::endl;
                CHECK(stepped <= steady + 3.0);
            }
        // A tail ringing at amount 0 (silence in): changing the type fades it, it never steps.
        for (int a = 0; a < 3; ++a)
        {
            const int b = (a + 1) % 3;
            juce::AudioBuffer<float> burst = noise((int) (3.0 * bandSr), 9);
            for (int i = sw; i < burst.getNumSamples(); ++i) { burst.setSample(0, i, 0.0f); burst.setSample(1, i, 0.0f); }
            const int sw2 = sw + 40 * 512;                                        // ~0.46 s into the ring-out
            const auto o = runBand(burst, [&] (int i) { CP p = neutralChain(); p.reverb = i < sw ? 100.0f : 0.0f;
                                                        p.reverbType = (float) (i < sw2 ? a : b); return p; }, false);
            // amount 0 (its slew landed, ~0.2 s): the output is exactly dry (silence) across the change
            CHECK(peakFrom(o, sw + (int) (0.25 * bandSr)) == 0.0f);
            // and raised again after the change, nothing of the old tank is left: it was faded out
            // and cleared, and the new one heard only silence. (Without the change, the old tail.)
            const int up = sw2 + 4096;
            auto raised = [&] (int typeAfter)
            {
                return runBand(burst, [&] (int i) { CP p = neutralChain(); p.reverb = (i < sw || i >= up) ? 100.0f : 0.0f;
                                                    p.reverbType = (float) (i < sw2 ? a : typeAfter); return p; }, false);
            };
            CHECK(peakFrom(raised(b), up) == 0.0f);
            CHECK(peakFrom(raised(a), up) > 1.0e-3f);
        }
    }

    std::cout << "REVERB TYPE: param default HALL; state round-trip; capture/restore and presets carry it" << std::endl;
    {
        LoopNestProcessor p;
        auto* t = p.apvts.getParameter("reverbType");
        CHECK(t != nullptr && dynamic_cast<juce::AudioParameterChoice*>(t) != nullptr);
        CHECK(near(p.capture().character.reverbType, 1.0f));               // HALL
        setNatural(p, "reverbType", 2.0f);                                 // SPRING
        CHECK(near(p.capture().character.reverbType, 2.0f));
        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        LoopNestProcessor q;
        setNatural(q, "reverbType", 0.0f);
        q.setStateInformation(mb.getData(), (int) mb.getSize());
        q.finishPendingLoad(5000);
        CHECK(near(q.capture().character.reverbType, 2.0f));
        // A snapshot (history, a preset) restores it; INIT and the factory presets put HALL back.
        auto snap = q.capture();
        snap.character.reverbType = 0.0f;
        q.restore(snap, S::kCharacter);
        CHECK(near(q.capture().character.reverbType, 0.0f));
        UiBridge br(q);
        br.selectPreset(-1);
        CHECK(near(q.capture().character.reverbType, 1.0f));
        setNatural(q, "reverbType", 2.0f);
        br.selectPreset(7);                                                // "Plate": the PLATE tank (Andrew, 10-18)
        CHECK(juce::String(loopnest::kFactoryPresets[7].name) == "Plate");
        CHECK(near(q.capture().character.reverbType, 0.0f) && ! br.presetDirty());
        setNatural(q, "reverbType", 1.0f);
        CHECK(br.presetDirty());                                           // the type counts in the preset match
        for (int i = 0; i < loopnest::kNumFactoryPresets; ++i)             // every other preset HALL
            if (i != 7) CHECK(near(loopnest::kFactoryPresets[i].v[loopnest::kNumPresetParams - 1], 1.0f));
    }

    std::cout << "state: a set saved before stateVersion 4 halves WARP / FLUTTER (same wobble) and gets REVERB TYPE HALL" << std::endl;
    {
        LoopNestProcessor src;
        setNatural(src, "warp", 30.0f); setNatural(src, "warpRate", 10.0f); setNatural(src, "flutter", 45.0f); setNatural(src, "reverbType", 2.0f);
        const float warpNew = grid(CharacterChain::warpFromOld(30.0f, 10.0f));   // rate-aware: not 15
        auto state = src.apvts.copyState();
        for (int version : { 3, 2, 0 })
        {
            auto old = state.createCopy();
            old.removeChild(old.getChildWithProperty("id", "reverbType"), nullptr);   // a v3 set has none
            if (version > 0) old.setProperty("stateVersion", version, nullptr);
            juce::MemoryBlock mb;
            if (auto xml = old.createXml()) juce::AudioProcessor::copyXmlToBinary(*xml, mb);
            LoopNestProcessor q;
            setNatural(q, "reverbType", 0.0f);                              // the instance had PLATE
            q.setStateInformation(mb.getData(), (int) mb.getSize());
            q.finishPendingLoad(5000);
            const auto c = q.capture().character;
            if (! (near(c.warp, warpNew) && near(c.flutter, 22.5f) && near(c.reverbType, 1.0f)))
                std::cout << "    v" << version << ": warp " << c.warp << " (want " << warpNew << "), flutter " << c.flutter << ", type " << c.reverbType << std::endl;
            CHECK(near(c.warp, warpNew) && near(c.flutter, 22.5f) && near(c.reverbType, 1.0f));
        }
        // This build's own state keeps them as saved.
        juce::MemoryBlock mb;
        src.getStateInformation(mb);
        LoopNestProcessor q;
        q.setStateInformation(mb.getData(), (int) mb.getSize());
        q.finishPendingLoad(5000);
        const auto c = q.capture().character;
        CHECK(near(c.warp, 30.0f) && near(c.flutter, 45.0f) && near(c.reverbType, 2.0f));
        // The converted knob is the same wobble: v2's WARP 30 at rate 50 was +17.9 / -18.4 cents.
        double up, down;
        const float w50 = grid(CharacterChain::warpFromOld(30.0f, 50.0f));
        centsRange([&] (int) { CP p = neutralChain(); p.warp = w50; p.warpRate = 50; return p; }, 15.0, 1.0, up, down);
        if (! (within(up, 17.9, 0.15) && within(down, -18.4, 0.15))) std::cout << "    WARP " << w50 << ", rate 50: " << up << " / " << down << " cents" << std::endl;
        CHECK(within(up, 17.9, 0.15) && within(down, -18.4, 0.15));
        // Every factory preset lands on its own column (on the knobs' 0.5 % grid): not dirty.
        UiBridge br(q);
        for (int i = 0; i < loopnest::kNumFactoryPresets; ++i)
        {
            br.selectPreset(i);
            if (br.presetDirty()) std::cout << "    preset " << UiBridge::presetName(i) << " reads dirty" << std::endl;
            CHECK(! br.presetDirty());
        }
    }

    std::cout << "REVERB TYPE: the wet print matches the audition for every type (past the print's 5 ms seam head)" << std::endl;
    {
        // A burst loop (noise then silence, 2 s), the tail ringing across the seam. The print
        // is the audition's pass (priming passes + 1) (printPrimePasses: the tail's T30 here),
        // heard at the audition's fixed delay; its first 5 ms are faded over from one more
        // wrap (F11), where a modulated tail differs by design.
        juce::AudioBuffer<float> burst = noise((int) (2.0 * 44100.0), 21, 0.3f);
        for (int i = (int) (0.3 * 44100.0); i < burst.getNumSamples(); ++i) { burst.setSample(0, i, 0.0f); burst.setSample(1, i, 0.0f); }
        const auto loopFile = root.getChildFile("elsewhere/burst.wav");
        {
            loopFile.getParentDirectory().createDirectory();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream(loopFile.createOutputStream());
            auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(32)
                                                                                          .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
            writer->writeFromAudioSampleBuffer(burst, 0, burst.getNumSamples());
        }
        for (int type = 0; type < 3; ++type)
        {
            LoopNestProcessor p;
            setNatural(p, "reverb", 70.0f); setNatural(p, "reverbDecay", 60.0f); setNatural(p, "reverbType", (float) type);
            p.prepareToPlay(44100.0, 512);
            loadVia(p, loopFile);
            p.setSyncEnabled(false);
            p.setDryHatch(false);
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            juce::AudioBuffer<float> w;
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(p.renderLoop()) })
            {
                w.setSize((int) r->numChannels, (int) r->lengthInSamples);
                r->read(&w, 0, (int) r->lengthInSamples, 0, true, true);
            }
            const int P = w.getNumSamples();
            CHECK(P == burst.getNumSamples());
            if (P != burst.getNumSamples()) continue;
            const int prime = p.printPrimePasses((double) P / 44100.0), lat = p.getLatencySamples();
            CHECK(prime == (type == 2 ? 1 : 2));  // T30 ~2.5 s (PLATE, HALL), ~1.6 s (SPRING) over a 2 s loop
            p.setLooping(true);
            p.setPlaying(true);
            juce::AudioBuffer<float> aud(2, (prime + 2) * P + lat + 512), blk(2, 512);
            juce::MidiBuffer midi;
            for (int at = 0; at + 512 <= aud.getNumSamples(); at += 512)
            {
                blk.clear();
                p.processBlock(blk, midi);
                for (int ch = 0; ch < 2; ++ch) aud.copyFrom(ch, at, blk, ch, 0, 512);
            }
            const int head = juce::roundToInt(0.005 * 44100.0);
            double e = 0.0, r = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = head; i < P; ++i)
                {
                    const double y = w.getSample(ch, i), d = (double) aud.getSample(ch, prime * P + lat + i) - y;
                    e += d * d; r += y * y;
                }
            const double nullDb = 10.0 * std::log10(juce::jmax(1.0e-300, e) / juce::jmax(1.0e-300, r));
            if (nullDb > -100.0) std::cout << "    " << typeName[type] << ": print vs audition pass " << prime + 1 << " null " << nullDb << " dB" << std::endl;
            CHECK(r > 0.0 && nullDb <= -100.0);
            p.releaseResources();
        }
    }

    std::cout << "drag-in: copy into the bank, skip identical, suffix a taken name, skip folders / non-audio, refuse no bank" << std::endl;
    {
        namespace bd = loopnest::bankdrop;
        const auto dropBank = root.getChildFile("dropbank");
        dropBank.createDirectory();
        const auto inbox = root.getChildFile("dropsrc");
        const auto kick  = writeWav(inbox.getChildFile("kick_90bpm.wav"), 0.2, 200.0f);
        const auto kick2 = writeWav(inbox.getChildFile("other/kick_90bpm.wav"), 0.2, 300.0f);     // same name, other bytes
        const auto kick3 = writeWav(inbox.getChildFile("third/kick_90bpm.wav"), 0.2, 400.0f);     // and a third
        const auto upper = writeWav(inbox.getChildFile("SNARE.WAV"), 0.2, 250.0f);                // the scan matches any case
        const auto aif   = inbox.getChildFile("pad.aif");   kick.copyFileTo(aif);
        const auto mp3   = inbox.getChildFile("hat.mp3");   kick.copyFileTo(mp3);
        const auto txt   = inbox.getChildFile("notes.txt"); txt.replaceWithText("not audio");
        const auto flac  = inbox.getChildFile("loop.flac"); kick.copyFileTo(flac);               // decodable, but not a scan extension
        const auto dir   = inbox.getChildFile("pack.wav"); dir.createDirectory();                // a folder, even named like audio
        const auto gone  = inbox.getChildFile("missing.wav");

        CHECK(bd::isAudioFile(kick) && bd::isAudioFile(upper) && bd::isAudioFile(aif) && bd::isAudioFile(mp3));
        CHECK(! bd::isAudioFile(txt) && ! bd::isAudioFile(flac) && ! bd::isAudioFile(dir) && ! bd::isAudioFile(gone));

        // refusals: nothing is written
        CHECK(bd::check({ kick }, juce::File()) == bd::Verdict::noBank);
        CHECK(bd::check({ kick }, root.getChildFile("no-such-bank")) == bd::Verdict::noBank);
        CHECK(bd::check({}, dropBank) == bd::Verdict::nothing);
        CHECK(bd::check({ dir }, dropBank) == bd::Verdict::nothing);
        CHECK(bd::check({ inbox }, dropBank) == bd::Verdict::nothing);
        CHECK(bd::check({ gone }, dropBank) == bd::Verdict::nothing);
        CHECK(bd::check({ txt }, dropBank) == bd::Verdict::nothing);
        CHECK(bd::check({ flac }, dropBank) == bd::Verdict::nothing);
        CHECK(bd::check({ txt, dir, flac }, dropBank) == bd::Verdict::nothing);
        CHECK(bd::check({ kick, txt }, dropBank) == bd::Verdict::ok);   // one audio file is enough; the rest is skipped
        CHECK(bd::check({ dir, kick }, dropBank) == bd::Verdict::ok);
        CHECK(bd::copyAll({ txt, dir }, dropBank).empty());
        CHECK(bd::copyAll({ kick }, juce::File()).empty());
        CHECK(dropBank.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0);

        // a new name copies; the same name and bytes again is skipped (nothing new written)
        auto r = bd::copyAll({ kick, upper, aif, mp3 }, dropBank);
        CHECK(r.size() == 4);
        CHECK(r.size() == 4 && r[0].outcome == bd::Outcome::copied && r[0].dest == dropBank.getChildFile("kick_90bpm.wav"));
        CHECK(r.size() == 4 && r[1].outcome == bd::Outcome::copied && r[1].dest.getFileName() == "SNARE.WAV");
        CHECK(dropBank.getChildFile("kick_90bpm.wav").hasIdenticalContentTo(kick));
        const auto stamp = dropBank.getChildFile("kick_90bpm.wav").getLastModificationTime();
        r = bd::copyAll({ kick }, dropBank);
        CHECK(r.size() == 1 && r[0].outcome == bd::Outcome::skipped && r[0].dest == dropBank.getChildFile("kick_90bpm.wav"));
        CHECK(dropBank.getChildFile("kick_90bpm.wav").getLastModificationTime() == stamp);
        CHECK(dropBank.getNumberOfChildFiles(juce::File::findFiles) == 4);

        // the same name with other bytes: ` 2`, then ` 3`, before the extension; never an overwrite
        r = bd::copyAll({ kick2 }, dropBank);
        CHECK(r.size() == 1 && r[0].outcome == bd::Outcome::copied && r[0].dest == dropBank.getChildFile("kick_90bpm 2.wav"));
        CHECK(dropBank.getChildFile("kick_90bpm 2.wav").hasIdenticalContentTo(kick2));
        CHECK(dropBank.getChildFile("kick_90bpm.wav").hasIdenticalContentTo(kick));
        r = bd::copyAll({ kick3 }, dropBank);
        CHECK(r.size() == 1 && r[0].outcome == bd::Outcome::copied && r[0].dest == dropBank.getChildFile("kick_90bpm 3.wav"));
        CHECK(dropBank.getChildFile("kick_90bpm 3.wav").hasIdenticalContentTo(kick3));
        // dropping the second version again finds its own ` 2` copy: skipped, no ` 4`
        r = bd::copyAll({ kick2, kick3, kick }, dropBank);
        CHECK(r.size() == 3 && r[0].outcome == bd::Outcome::skipped && r[0].dest == dropBank.getChildFile("kick_90bpm 2.wav"));
        CHECK(r.size() == 3 && r[1].outcome == bd::Outcome::skipped && r[1].dest == dropBank.getChildFile("kick_90bpm 3.wav"));
        CHECK(r.size() == 3 && r[2].outcome == bd::Outcome::skipped && r[2].dest == dropBank.getChildFile("kick_90bpm.wav"));
        CHECK(! dropBank.getChildFile("kick_90bpm 4.wav").exists());
        CHECK(dropBank.getNumberOfChildFiles(juce::File::findFiles) == 6);

        // a file already in the bank's tree is used where it is, never copied to the top
        const auto nested = writeWav(dropBank.getChildFile("sub/deep.wav"), 0.2, 500.0f);
        r = bd::copyAll({ nested }, dropBank);
        CHECK(r.size() == 1 && r[0].outcome == bd::Outcome::inBank && r[0].dest == nested);
        CHECK(! dropBank.getChildFile("deep.wav").exists());

        // through the bridge: AUDITION picks the first dropped file like a rack pick (trim reset, knobs kept)
        {
            LoopNestProcessor p;
            p.setSampleFolder(dropBank);
            UiBridge br(p);
            const int before = p.getSampleCount();
            CHECK(! br.canDrop({ txt.getFullPathName() }) && ! br.canDrop({ dir.getFullPathName() }));
            CHECK(! br.canDrop({ "relative/kick.wav" }));
            CHECK(! br.drop({ txt.getFullPathName(), dir.getFullPathName() }));
            CHECK(p.getSampleCount() == before);
            CHECK(br.canDrop({ kick.getFullPathName() }));
            CHECK(br.canDrop({ txt.getFullPathName(), "relative/x.wav", kick.getFullPathName() }));

            loadVia(p, dropBank.getChildFile("SNARE.WAV"));
            if (auto* q = p.apvts.getParameter("drive")) q->setValueNotifyingHost(q->convertTo0to1(40.0f));
            if (auto* q = p.apvts.getParameter("start")) q->setValueNotifyingHost(0.25f);
            const auto fresh = writeWav(inbox.getChildFile("fresh_120bpm.wav"), 0.3, 350.0f);
            CHECK(! p.isFxMode());
            CHECK(br.drop({ txt.getFullPathName(), dir.getFullPathName(), fresh.getFullPathName(), kick.getFullPathName() }));
            CHECK(br.finishDrops(5000));                                  // copied and rescanned off the message thread (U-M3)
            CHECK(p.getSampleCount() == before + 1);                      // txt and the folder skipped, fresh copied, kick skipped
            CHECK(! dropBank.getChildFile("notes.txt").exists() && ! dropBank.getChildFile("pack.wav").exists());
            CHECK(p.getSampleFiles().contains(dropBank.getChildFile("fresh_120bpm.wav")));
            CHECK(p.finishPendingLoad(5000));
            CHECK(p.getCurrentSampleFile() == dropBank.getChildFile("fresh_120bpm.wav"));
            CHECK(near(p.capture().start, 0.0f) && near(p.capture().end, 1.0f));
            CHECK(near(p.capture().character.drive, 40.0f, 0.6f));
            CHECK(! p.isFxMode());

            // LIVE: copied only, the loaded loop stays
            br.setLive(true);
            const auto liveDrop = writeWav(inbox.getChildFile("live_drop.wav"), 0.3, 360.0f);
            CHECK(br.drop({ liveDrop.getFullPathName() }));
            CHECK(br.finishDrops(5000));
            CHECK(dropBank.getChildFile("live_drop.wav").hasIdenticalContentTo(liveDrop));
            CHECK(p.getSampleFiles().contains(dropBank.getChildFile("live_drop.wav")));
            p.finishPendingLoad(500);
            CHECK(p.getCurrentSampleFile() == dropBank.getChildFile("fresh_120bpm.wav"));
            CHECK(p.isFxMode());
            const auto files = br.takeFilesIfChanged();
            CHECK(files.has_value());   // the page's rack gets the new list
        }
        // no bank: refused
        {
            LoopNestProcessor p;
            UiBridge br(p);
            if (p.getSampleFolder().isDirectory()) p.setSampleFolder(root.getChildFile("no-such-bank"));
            CHECK(! br.canDrop({ kick.getFullPathName() }));
            CHECK(! br.drop({ kick.getFullPathName() }));
        }

        // queued drops copy in order, and AUDITION ends on the newest one's first file (U-M3)
        {
            LoopNestProcessor p;
            p.setSampleFolder(dropBank);
            UiBridge br(p);
            const auto q1 = writeWav(inbox.getChildFile("queue_one.wav"), 0.2, 610.0f);
            const auto q2 = writeWav(inbox.getChildFile("queue_two.wav"), 0.2, 620.0f);
            CHECK(br.drop({ q1.getFullPathName() }) && br.drop({ q2.getFullPathName() }));
            CHECK(br.finishDrops(5000));
            CHECK(dropBank.getChildFile("queue_one.wav").hasIdenticalContentTo(q1));
            CHECK(dropBank.getChildFile("queue_two.wav").hasIdenticalContentTo(q2));
            CHECK(p.finishPendingLoad(5000));
            CHECK(p.getCurrentSampleFile() == dropBank.getChildFile("queue_two.wav"));
            CHECK(br.finishDrops(0));                                    // nothing left waiting

            // a row picked while the copy runs is newer than the drop: the drop doesn't take over
            const auto q3 = writeWav(inbox.getChildFile("queue_three.wav"), 0.2, 630.0f);
            const auto row = dropBank.getChildFile("queue_one.wav");
            CHECK(br.drop({ q3.getFullPathName() }));
            CHECK(br.pick(p.getSampleFiles().indexOf(row)));
            CHECK(br.finishDrops(5000) && p.finishPendingLoad(5000));
            CHECK(dropBank.getChildFile("queue_three.wav").existsAsFile());
            CHECK(p.getCurrentSampleFile() == row);
        }

        // a drop where nothing lands (the copies fail) is logged
        {
            const auto roBank = root.getChildFile("readonlybank");
            roBank.createDirectory();
            LoopNestProcessor p;
            p.setSampleFolder(roBank);
            UiBridge br(p);
            juce::StringArray logged;
            br.log = [&logged] (const juce::String& m) { logged.add(m); };
            CHECK(chmod(roBank.getFullPathName().toRawUTF8(), 0555) == 0);
            const auto q4 = writeWav(inbox.getChildFile("queue_four.wav"), 0.2, 640.0f);
            CHECK(br.drop({ q4.getFullPathName() }));                    // accepted: the bank exists
            CHECK(br.finishDrops(5000));
            chmod(roBank.getFullPathName().toRawUTF8(), 0755);
            CHECK(! roBank.getChildFile("queue_four.wav").exists());
            CHECK(logged.size() == 1 && logged[0] == "drop: nothing landed (1 failed)");
        }
    }

    std::cout << "drag-in: a symlinked file copies its target's bytes under the link's name; a dangling link is skipped (U-M1)" << std::endl;
    {
        namespace bd = loopnest::bankdrop;
        const auto lbank = root.getChildFile("symdropbank");
        lbank.createDirectory();
        const auto lsrc = root.getChildFile("symdropsrc");
        const auto real = writeWav(lsrc.getChildFile("store/real_take.wav"), 0.2, 450.0f);
        const auto absLink = lsrc.getChildFile("abs_link.wav");
        CHECK(real.createSymbolicLink(absLink, true));
        const auto relLink = lsrc.getChildFile("rel_link.wav");                  // relative to its own folder
        CHECK(juce::File::createSymbolicLink(relLink, "store/real_take.wav", true));
        const auto chain = lsrc.getChildFile("chain_link.wav");                  // a link to a link
        CHECK(relLink.createSymbolicLink(chain, true));
        const auto dangling = lsrc.getChildFile("dangling.wav");
        CHECK(juce::File::createSymbolicLink(dangling, lsrc.getChildFile("nowhere.wav").getFullPathName(), true));
        CHECK(absLink.isSymbolicLink() && relLink.isSymbolicLink() && chain.isSymbolicLink() && dangling.isSymbolicLink());

        CHECK(bd::resolveLinks(chain) == real && bd::resolveLinks(real) == real);
        CHECK(bd::isAudioFile(absLink) && ! bd::isAudioFile(dangling));
        CHECK(bd::check({ dangling }, lbank) == bd::Verdict::nothing);
        CHECK(bd::copyInto(dangling, lbank).outcome == bd::Outcome::failed);

        auto r = bd::copyAll({ absLink, relLink, chain, dangling }, lbank);
        CHECK(r.size() == 3);                                                     // the dangling link isn't audio
        for (const auto& name : { "abs_link.wav", "rel_link.wav", "chain_link.wav" })
        {
            const auto f = lbank.getChildFile(name);
            CHECK(f.existsAsFile() && ! f.isSymbolicLink() && f.hasIdenticalContentTo(real));
        }
        CHECK(r.size() == 3 && r[0].outcome == bd::Outcome::copied && r[2].outcome == bd::Outcome::copied);
        // dropping the link again finds its bytes: skipped
        r = bd::copyAll({ absLink }, lbank);
        CHECK(r.size() == 1 && r[0].outcome == bd::Outcome::skipped && r[0].dest == lbank.getChildFile("abs_link.wav"));
        // a link out of the bank onto a file in it: used where its target is
        const auto inside = writeWav(lbank.getChildFile("sub/inside.wav"), 0.2, 470.0f);
        const auto toInside = lsrc.getChildFile("to_inside.wav");
        CHECK(inside.createSymbolicLink(toInside, true));
        r = bd::copyAll({ toInside }, lbank);
        CHECK(r.size() == 1 && r[0].outcome == bd::Outcome::inBank && r[0].dest == inside);
        // a link already sitting at the name in the bank counts as taken, never the same bytes
        const auto other = writeWav(lsrc.getChildFile("taken.wav"), 0.2, 480.0f);
        CHECK(other.createSymbolicLink(lbank.getChildFile("taken.wav"), true));
        r = bd::copyAll({ other }, lbank);
        CHECK(r.size() == 1 && r[0].outcome == bd::Outcome::copied && r[0].dest == lbank.getChildFile("taken 2.wav"));
        CHECK(lbank.getChildFile("taken.wav").isSymbolicLink());                 // left as it was

        // through the bridge: the copy is a real file the rack lists, and AUDITION loads it
        LoopNestProcessor p;
        p.setSampleFolder(lbank);
        UiBridge br(p);
        const auto viaBridge = lsrc.getChildFile("bridge_link.wav");
        CHECK(real.createSymbolicLink(viaBridge, true));
        CHECK(br.drop({ viaBridge.getFullPathName() }) && br.finishDrops(5000));
        CHECK(p.getSampleFiles().contains(lbank.getChildFile("bridge_link.wav")));
        CHECK(p.finishPendingLoad(5000) && p.getCurrentSampleFile() == lbank.getChildFile("bridge_link.wav"));
    }

    std::cout << "drag-in: an editor closed mid-drop joins the worker and lands nothing afterwards (U-M3)" << std::endl;
    {
        const auto cbank = root.getChildFile("closebank");
        cbank.createDirectory();
        const auto csrc = root.getChildFile("closesrc");
        juce::StringArray many;
        for (int i = 0; i < 40; ++i)
            many.add(writeWav(csrc.getChildFile("c" + juce::String(i) + ".wav"), 0.5, 300.0f + (float) i).getFullPathName());
        LoopNestProcessor p;
        p.setSampleFolder(cbank);
        const auto loadedBefore = p.getCurrentSampleFile();
        {
            UiBridge br(p);
            CHECK(br.drop(many));
            for (int k = 0; k < 2000 && cbank.getNumberOfChildFiles(juce::File::findFiles) == 0; ++k)
                juce::Thread::sleep(1);   // the first file is in: close mid-drop
        }   // the destructor stops the worker between files and joins it
        const int copied = cbank.getNumberOfChildFiles(juce::File::findFiles);
        CHECK(copied < 40);                                                     // the close cut it short
        bool whole = true;                                                       // no half-copied file
        for (const auto& f : cbank.findChildFiles(juce::File::findFiles, false))
            whole = whole && f.hasIdenticalContentTo(csrc.getChildFile(f.getFileName()));
        CHECK(whole && copied > 0);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(100);         // a posted landing finds the bridge gone
        CHECK(p.finishPendingLoad(2000));
        CHECK(p.getCurrentSampleFile() == loadedBefore);                        // nothing picked after the close
        CHECK(cbank.getNumberOfChildFiles(juce::File::findFiles) == copied);    // nothing copied after it either
        // The close skipped the worker's rescan (the bank was empty when scanned): the next
        // editor's bridge does it, and clears the flag.
        CHECK(p.getSampleCount() == 0);
        {
            UiBridge again(p);
            CHECK(p.getSampleCount() == copied);                                // every copied file listed
        }
        CHECK(! p.takeRescanOwed());
        std::cout << "    " << copied << " of 40 copied before the close" << std::endl;
    }

    std::cout << "drum extract: its temp copy is LoopNest's own (.loopnest-part); a user's .part in the destination stays; a linked folder isn't followed (B7, C-M7)" << std::endl;
    {
        const auto src = root.getChildFile("xsrc"), dest = root.getChildFile("xdest"), far = root.getChildFile("xfar");
        writeWav(src.getChildFile("Pack/drum_loop_120bpm.wav"), 3.0, 200.0f);
        writeWav(far.getChildFile("far_drum_loop_120bpm.wav"), 3.0, 300.0f);
        CHECK(far.createSymbolicLink(src.getChildFile("Linked"), true));      // a link out of the source tree
        dest.createDirectory();
        const auto download = dest.getChildFile("song.mp3.part");             // someone's download
        download.replaceWithText("half a download");
        const auto killed = dest.getChildFile("old_drum.wav.loopnest-part");   // a copy an earlier run left
        killed.replaceWithText("a killed copy");
        LoopNestProcessor p;
        p.extractDrumLoops(src, dest);
        for (int i = 0; i < 2000 && p.isExtracting(); ++i)
        {
            p.finishExtractionIfReady();
            juce::Thread::sleep(5);
        }
        CHECK(! p.isExtracting() && dest.getChildFile("Pack__drum_loop_120bpm.wav").existsAsFile());
        CHECK(download.existsAsFile() && ! killed.exists());
        CHECK(dest.findChildFiles(juce::File::findFiles, false, "*.loopnest-part").isEmpty());
        CHECK(! dest.getChildFile("Linked__far_drum_loop_120bpm.wav").exists());
    }

    std::cout << "drum extract: removing the plugin mid-scan cancels the worker and returns promptly (C-H3)" << std::endl;
    {
        const auto src = root.getChildFile("xbig"), dest = root.getChildFile("xbigdest");
        for (int i = 0; i < 400; ++i)
            writeWav(src.getChildFile("drum_" + juce::String(i) + ".wav"), 0.05, 200.0f);   // too short to keep
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        {
            LoopNestProcessor p;
            p.extractDrumLoops(src, dest);
            CHECK(p.isExtracting());
        }
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        if (ms > 5000.0) std::cout << "    took " << ms << " ms" << std::endl;
        CHECK(ms < 5000.0 && dest.findChildFiles(juce::File::findFiles, false).isEmpty());
    }

    std::cout << "bank scan: a linked folder (out of the bank, or back into it) isn't followed (C-M7)" << std::endl;
    {
        const auto bk = root.getChildFile("linkbank"), far = root.getChildFile("linkfar");
        const auto own = writeWav(bk.getChildFile("own_90bpm.wav"), 0.3, 200.0f);
        writeWav(far.getChildFile("far_90bpm.wav"), 0.3, 300.0f);
        bk.getChildFile("sub").createDirectory();
        CHECK(far.createSymbolicLink(bk.getChildFile("out"), true));
        CHECK(bk.createSymbolicLink(bk.getChildFile("sub/loop"), true));      // a cycle
        LoopNestProcessor p;
        p.setSampleFolder(bk);
        const auto files = p.getSampleFiles();
        if (files.size() != 1) std::cout << "    scanned " << files.size() << " files" << std::endl;
        CHECK(files.size() == 1 && files.contains(own));
    }

    std::cout << "drag-in: a file named like a home path (\"~kick.wav\") lands in the bank (B4 review)" << std::endl;
    {
        namespace bd = loopnest::bankdrop;
        const auto tbank = root.getChildFile("tildebank");
        tbank.createDirectory();
        const auto tilde = writeWav(root.getChildFile("tildesrc/~kick_90bpm.wav"), 0.2, 200.0f);
        const auto r = bd::copyAll({ tilde }, tbank);
        CHECK(r.size() == 1 && r[0].outcome == bd::Outcome::copied && r[0].dest.getParentDirectory() == tbank);
        CHECK(juce::File(tbank.getFullPathName() + "/~kick_90bpm.wav").hasIdenticalContentTo(tilde));
        const auto again = bd::copyAll({ tilde }, tbank);           // the same bytes: skipped, found where it landed
        CHECK(again.size() == 1 && again[0].outcome == bd::Outcome::skipped);
    }

    std::cout << "settings: instances share one file: a save never reverts another's bin or recents; HATCH and the bin check read another's pick (E-M2)" << std::endl;
    {
        const auto prints = root.getChildFile("prints");
        const auto f1 = root.getChildFile("recentA"), f2 = root.getChildFile("recentB");
        f1.createDirectory();
        f2.createDirectory();
        CHECK(prints.getChildFile("shared").createDirectory().wasOk());
        LoopNestProcessor A, B;
        CHECK(A.setPrintBin("shared"));
        B.rememberEditorScale(0.9f);                                   // B's own save, after A's pick
        {
            LoopNestProcessor C;
            CHECK(C.getPrintBin() == "shared" && near(C.recallEditorScale(), 0.9f));
        }
        B.prepareToPlay(44100.0, 512);
        loadVia(B, a);
        const auto printed = B.renderLoop();                           // HATCH reads the shared pick
        CHECK(printed.getParentDirectory() == prints.getChildFile("shared") && B.getPrintBin() == "shared");
        CHECK(A.setPrintBin({}));
        UiBridge br(B);
        for (int i = 0; i < 31; ++i) br.state();                       // the once-a-second bin check
        CHECK(B.getPrintBin().isEmpty() && br.state()["bin"].toString().isEmpty());
        A.setSampleFolder(f1);
        B.setSampleFolder(f2);
        {
            LoopNestProcessor C;
            const auto recents = C.getRecentFolders();
            CHECK(recents.size() >= 2 && recents[0] == f2 && recents[1] == f1);
        }
        B.releaseResources();
    }

    std::cout << "print bins: any folder in the Prints root is a bin, named as Finder made it (\"#1 Kicks\", \"Beats, 2026\", \"~Old\"); never one outside it (E-L4)" << std::endl;
    {
        const auto prints = root.getChildFile("prints");
        LoopNestProcessor p;
        CHECK(p.setPrintBin({}));
        p.prepareToPlay(44100.0, 512);
        loadVia(p, a);
        for (auto* name : { "#1 Kicks", "Beats, 2026", "~Old", "a:b" })
        {
            const juce::File dir(prints.getFullPathName() + "/" + name);   // not getChildFile: it reads "~Old" as ~/Old
            CHECK(dir.createDirectory().wasOk());
            CHECK(p.getPrintBins().contains(name) && p.setPrintBin(name) && p.getPrintBin() == name && ! p.isPrintBinMissing());
            const auto out = p.renderLoop();
            if (out.getParentDirectory() != dir) std::cout << "    " << name << ": printed to \"" << out.getFullPathName() << "\"" << std::endl;
            CHECK(out.getParentDirectory() == dir);
            CHECK(out.getFileName().startsWith(LoopNestProcessor::legalBinName(name) + "_loopA_90bpm_+0st_"));
            LoopNestProcessor q;                                          // sticky, as named
            CHECK(q.getPrintBin() == name);
        }
        CHECK(! p.setPrintBin("..") && ! p.setPrintBin(".") && ! p.setPrintBin("a/b") && ! p.setPrintBin("../prints"));
        CHECK(! p.setPrintBin("Nope") && p.getPrintBin() == "a:b");
        // A loop named like a home path prints into the folder too.
        const auto tilde = writeWav(root.getChildFile("elsewhere/~kick_90bpm.wav"), 0.3, 200.0f);
        loadVia(p, tilde);
        const auto tildePrint = p.renderLoop();
        CHECK(tildePrint.getParentDirectory() == juce::File(prints.getFullPathName() + "/a:b")
              && tildePrint.getFileName().startsWith("a-b_~kick_90bpm_"));
        loadVia(p, a);
        // A new bin named like a home path is made in the Prints root.
        const auto homeName = "LoopNestTests-" + juce::String(juce::Time::currentTimeMillis());
        const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(homeName);
        CHECK(p.createPrintBin("~" + homeName) && juce::File(prints.getFullPathName() + "/~" + homeName).isDirectory());
        CHECK(! home.exists());
        home.deleteRecursively();
        CHECK(p.setPrintBin({}));
    }

    std::cout << "HATCH: a long source name still prints, its name well under 255 bytes (ASCII, UTF-8, in a long bin) (E-L3)" << std::endl;
    {
        juce::String ascii;
        while (ascii.length() < 228) ascii << "Pack_Name_Drums_";
        ascii = ascii.substring(0, 228) + "_120bpm";                     // 239 bytes with ".wav"
        juce::String wide;
        for (int i = 0; i < 80; ++i) wide << juce::String::fromUTF8("\xe3\x83\x89");   // 240 bytes
        LoopNestProcessor p;
        p.prepareToPlay(44100.0, 512);
        for (bool longBin : { false, true })
        {
            if (longBin) CHECK(p.createPrintBin(juce::String::repeatedString("b", 80)));   // 64 once legal
            else         CHECK(p.setPrintBin({}));
            for (const auto& name : { ascii, wide })
            {
                const auto f = writeWav(root.getChildFile("longnames").getChildFile(name + ".wav"), 0.5, 220.0f);
                CHECK(f.existsAsFile());
                loadVia(p, f);
                const auto out = p.renderLoop();
                const int bytes = (int) out.getFileName().getNumBytesAsUTF8();
                if (! out.existsAsFile() || bytes > 245) std::cout << "    print \"" << out.getFileName() << "\" " << bytes << " bytes" << std::endl;
                CHECK(out.existsAsFile() && bytes <= 245);
            }
        }
        CHECK(p.setPrintBin({}));
    }

    std::cout << "NaN / inf: a float file's decode as silence and LIVE input's play as silence, so ECHO and REVERB never take one in (C-M10)" << std::endl;
    {
        const auto bad = root.getChildFile("elsewhere/nan_90bpm.wav");
        {
            auto buf = noise(44100, 31, 0.3f);
            buf.setSample(0, 1000, std::numeric_limits<float>::quiet_NaN());
            buf.setSample(1, 2000, std::numeric_limits<float>::infinity());
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream(bad.createOutputStream());
            auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(32)
                                                                                          .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
            writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
        }
        {
            juce::AudioFormatManager fm;                                // the file really carries them
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(bad) };
            juce::AudioBuffer<float> raw(2, 44100);
            CHECK(r != nullptr && r->read(&raw, 0, 44100, 0, true, true)
                  && std::isnan(raw.getSample(0, 1000)) && std::isinf(raw.getSample(1, 2000)));
        }
        auto finite = [] (const juce::AudioBuffer<float>& buf)
        {
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    if (! std::isfinite(buf.getSample(ch, i))) return false;
            return true;
        };
        LoopNestProcessor p;
        setNatural(p, "echo", 60.0f);
        setNatural(p, "reverb", 60.0f);
        p.prepareToPlay(44100.0, 512);
        loadVia(p, bad);
        CHECK(p.getCurrentSampleFile() == bad);
        p.setSyncEnabled(false);
        p.setPlaying(true);
        juce::AudioBuffer<float> blk(2, 512);
        juce::MidiBuffer midi;
        bool ok = true;
        float pk = 0.0f;
        for (int i = 0; i < 200; ++i)
        {
            blk.clear();
            p.processBlock(blk, midi);
            ok = ok && finite(blk);
            pk = juce::jmax(pk, blk.getMagnitude(0, 512));
        }
        CHECK(ok && pk > 0.05f);
        // LIVE: one block with a NaN and an inf in, then clean input; the tail still rings.
        p.setFxMode(true);
        auto in = noise(512, 32, 0.3f);
        ok = true;
        float tail = 0.0f;
        for (int i = 0; i < 200; ++i)
        {
            if (i < 100) blk.makeCopyOf(in); else blk.clear();
            if (i == 50) { blk.setSample(0, 100, std::numeric_limits<float>::quiet_NaN()); blk.setSample(1, 200, -std::numeric_limits<float>::infinity()); }
            p.processBlock(blk, midi);
            ok = ok && finite(blk);
            if (i == 110) tail = blk.getMagnitude(0, 512);
        }
        CHECK(ok && tail > 1.0e-4f);
    }


    // --- Release audit batch 3 (10-18): ruling 3's fixed latency, D-M1..D-M4, C-M4, D-L1, D-L2, a host with no meter ---

    // One processor run in FX: `in` through it in 512-frame blocks, the output.
    auto runFxOn = [] (LoopNestProcessor& p, const juce::AudioBuffer<float>& in)
    {
        const int blocks = (in.getNumSamples() + 511) / 512;
        juce::AudioBuffer<float> out(2, blocks * 512), buf(2, 512);
        juce::MidiBuffer midi;
        for (int blk = 0; blk < blocks; ++blk)
        {
            buf.clear();
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512 && blk * 512 + i < in.getNumSamples(); ++i)
                    buf.setSample(ch, i, in.getSample(ch, blk * 512 + i));
            p.processBlock(buf, midi);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, blk * 512, buf, ch, 0, 512);
        }
        return out;
    };
    auto setToggleOn = [] (LoopNestProcessor& p, const char* id, bool on)
    {
        if (auto* q = p.apvts.getParameter(id)) q->setValueNotifyingHost(on ? 1.0f : 0.0f);
    };

    std::cout << "latency (ruling 3): FX on a fixed, reported delay: an impulse comes out exactly getLatencySamples() late at neutral, MIX 50, with WARP / FLUTTER up at MIX 0 and under BYPASS, and with their panels off (44.1 / 48 / 96 kHz)" << std::endl;
    {
        struct Case { const char* name; float warp, flutter, mix; bool bypass, panelsOff; };
        const Case cases[] = { { "neutral", 0, 0, 100, false, false },
                               { "neutral, MIX 50", 0, 0, 50, false, false },
                               { "WARP 60 / FLUTTER 70, MIX 0", 60, 70, 0, false, false },
                               { "WARP 60 / FLUTTER 70, BYPASS", 60, 70, 100, true, false },
                               { "WARP 60 / FLUTTER 70 panels off, MIX 50", 60, 70, 50, false, true } };
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            const int want = (int) std::lround(0.016 * sr) + (int) std::lround(0.008 * sr);   // WARP's base + FLUTTER's
            bool allOk = true;
            int reported = -1;
            for (const auto& cs : cases)
            {
                LoopNestProcessor p;
                setNatural(p, "warp", cs.warp); setNatural(p, "flutter", cs.flutter); setNatural(p, "mix", cs.mix);
                setToggleOn(p, "bypass", cs.bypass);
                if (cs.panelsOff) { setPower(p, CP::kWarp, false); setPower(p, CP::kFlutter, false); }
                p.setFxMode(true);
                p.prepareToPlay(sr, 512);
                reported = p.getLatencySamples();
                const int at = (int) (0.5 * sr);              // past BYPASS's 5 ms blend, settled
                juce::AudioBuffer<float> in(2, at + 3 * want);
                in.clear();
                in.setSample(0, at, 1.0f); in.setSample(1, at, 1.0f);
                const auto out = runFxOn(p, in);
                int peakAt = -1;
                float peak = 0.0f, rest = 0.0f;
                for (int i = 0; i < out.getNumSamples(); ++i)
                    if (std::abs(out.getSample(0, i)) > peak) { peak = std::abs(out.getSample(0, i)); peakAt = i; }
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < out.getNumSamples(); ++i)
                        if (i != at + reported) rest = juce::jmax(rest, std::abs(out.getSample(ch, i)));
                // (BYPASS's 5 ms blend settles within ~1e-5 of whole in float: the wet's trace is left.)
                const bool ok = reported == want && peakAt == at + reported && near(out.getSample(0, peakAt), 1.0f, 1.0e-4f)
                                && near(out.getSample(1, peakAt), 1.0f, 1.0e-4f) && rest < 1.0e-4f;
                if (! ok) std::cout << "    " << sr << " Hz, " << cs.name << ": out at " << peakAt - at << " frames (" << peak
                                    << "), reported " << reported << ", elsewhere " << rest << std::endl;
                allOk = allOk && ok;
                CHECK(ok);
            }
            std::cout << "    " << sr << " Hz: " << reported << " frames (" << juce::String(1000.0 * reported / sr, 2) << " ms)"
                      << (allOk ? ", every case exact" : "") << std::endl;
        }
    }

    std::cout << "latency (ruling 3): WARP / FLUTTER up, the wet's delay sways about the reported one (impulses every 50 ms for 20 s: mean within 1 ms of it, none outside it +/- 21 ms)" << std::endl;
    {
        for (const auto& [warp, flutter] : { std::pair<float, float> { 60.0f, 0.0f }, { 0.0f, 70.0f }, { 100.0f, 100.0f } })
        {
            LoopNestProcessor p;
            setNatural(p, "warp", warp); setNatural(p, "flutter", flutter);
            p.setFxMode(true);
            p.prepareToPlay(44100.0, 512);
            const int lat = p.getLatencySamples(), period = 2205, count = 400;
            juce::AudioBuffer<float> in(2, (count + 2) * period);
            in.clear();
            for (int k = 1; k <= count; ++k) { in.setSample(0, k * period, 1.0f); in.setSample(1, k * period, 1.0f); }
            const auto out = runFxOn(p, in);
            const int reach = (int) std::ceil(0.021 * 44100.0);   // WARP's 15 ms + FLUTTER's ~5.6 ms of sway
            double sum = 0.0, worst = 0.0;
            int n = 0;
            float stray = 0.0f;
            for (int k = 1; k <= count; ++k)
            {
                double e = 0.0, et = 0.0;
                for (int d = -period / 2; d < period / 2; ++d)
                {
                    const int i = k * period + lat + d;
                    const double v = (double) out.getSample(0, i) * out.getSample(0, i);
                    if (std::abs(d) > reach) stray = juce::jmax(stray, std::abs(out.getSample(0, i)));
                    e += v; et += v * d;
                }
                if (e <= 0.0) continue;
                const double arr = et / e;                        // the impulse's arrival (energy centroid) against the latency
                sum += arr; worst = juce::jmax(worst, std::abs(arr)); ++n;
            }
            const double meanMs = n > 0 ? sum / n / 44.1 : 1.0e9;
            std::cout << "    WARP " << warp << " / FLUTTER " << flutter << ": mean " << juce::String(meanMs, 3) << " ms off the latency, worst "
                      << juce::String(worst / 44.1, 2) << " ms" << (stray > 1.0e-4f ? ", some stray" : "") << std::endl;
            CHECK(n == count && std::abs(meanMs) <= 1.0 && worst <= reach && stray <= 1.0e-4f);   // (20 s of a random drift)
        }
    }

    std::cout << "latency (ruling 3): a print carries none of it: a neutral wet print is the dry print, a WARP / FLUTTER one is the audition's pass as the host plays it (before, they printed 11.5 / 8 ms late)" << std::endl;
    {
        const auto loopFile = writeClickLoop(root.getChildFile("elsewhere/latprint_90bpm.wav"), 90.0, 8, 0.5f, 7, 0.5);
        auto readAll = [] (const juce::File& f)
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            juce::AudioBuffer<float> w;
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(f) })
            {
                w.setSize((int) r->numChannels, (int) r->lengthInSamples);
                r->read(&w, 0, (int) r->lengthInSamples, 0, true, true);
            }
            return w;
        };
        for (const char* id : { "none", "warp", "flutter" })
        {
            LoopNestProcessor p;
            if (juce::String(id) != "none") setNatural(p, id, 50.0f);
            p.prepareToPlay(44100.0, 512);
            loadVia(p, loopFile);
            p.setSyncEnabled(false);
            p.setDryHatch(true);
            const auto dry = readAll(p.renderLoop());
            p.setDryHatch(false);
            const auto wet = readAll(p.renderLoop());
            const int n = dry.getNumSamples();
            CHECK(n > 0 && wet.getNumSamples() == n);
            if (n <= 0 || wet.getNumSamples() != n) continue;
            if (juce::String(id) == "none")
            {
                // Frame for frame (the two paths round differently by an LSB, as before the latency).
                float worst = 0.0f;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i) worst = juce::jmax(worst, std::abs(wet.getSample(ch, i) - dry.getSample(ch, i)));
                CHECK(worst <= 2.0e-7f);
                continue;
            }
            // The wet print is the audition's pass (priming passes + 1) from play start, heard
            // getLatencySamples() late: null past the 5 ms seam head (F11).
            const int prime = p.printPrimePasses((double) n / 44100.0), lat = p.getLatencySamples();
            p.setLooping(true);
            p.setPlaying(true);
            FakeHost idle;
            const auto aud = runHost(p, idle, ((prime + 2) * n + lat) / 512 + 2);
            const int head = juce::roundToInt(0.005 * 44100.0);
            double e = 0.0, r = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = head; i < n; ++i)
                {
                    const double y = wet.getSample(ch, i), d = (double) aud.getSample(ch, prime * n + lat + i) - y;
                    e += d * d; r += y * y;
                }
            const double nullDb = 10.0 * std::log10(juce::jmax(1.0e-300, e) / juce::jmax(1.0e-300, r));
            std::cout << "    " << id << " 50: the print against the audition's pass " << prime + 1 << " at the latency, null "
                      << juce::String(nullDb, 1) << " dB" << std::endl;
            CHECK(r > 0.0 && nullDb <= -100.0);
        }
    }

    std::cout << "REVERB (D-M1): a chain reset (the HATCH drag's flip to FX) or REVERB power-on ramps the tank's input in: no bright tick ~30 ms in" << std::endl;
    {
        // FX switch, the track a 220 Hz sine at -12 dBFS: above 5 kHz in the 300 ms after it, against
        // the switch's own edge with REVERB at 0 and the steady wet. (Audit: PLATE -52 vs -80 steady.)
        const auto tone = bandSine(4.0);
        auto fxSwitch = [&] (float reverb, int type)
        {
            LoopNestProcessor p;
            setNatural(p, "reverb", reverb); setNatural(p, "reverbType", (float) type);
            p.prepareToPlay(44100.0, 512);
            juce::AudioBuffer<float> out(2, 120 * 512), buf(2, 512);
            juce::MidiBuffer midi;
            for (int blk = 0; blk < 120; ++blk)
            {
                if (blk == 40) p.setFxMode(true);
                for (int ch = 0; ch < 2; ++ch) buf.copyFrom(ch, 0, tone, ch, blk * 512, 512);
                p.processBlock(buf, midi);
                for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, blk * 512, buf, ch, 0, 512);
            }
            return out;
        };
        const auto bare = fxSwitch(0.0f, 1);
        const double edge = hfPeakDb(bare, 40 * 512, 40 * 512 + 13230);
        for (int type = 0; type < 3; ++type)
        {
            const auto o = fxSwitch(100.0f, type);
            const double after = hfPeakDb(o, 40 * 512, 40 * 512 + 13230), steady = hfPeakDb(o, 100 * 512, 120 * 512);
            std::cout << "    FX switch, " << typeName[type] << ": " << juce::String(after, 1) << " dBFS above 5 kHz in 300 ms (the bare switch "
                      << juce::String(edge, 1) << ", the steady wet " << juce::String(steady, 1) << ")" << std::endl;
            CHECK(after <= juce::jmax(edge, steady) + 6.0);
        }
        // REVERB's panel off for 1 s then on, against the knob raised from 0 (the input's own ramp).
        // (Audit: PLATE -51 vs -80.)
        for (int type = 0; type < 3; ++type)
        {
            const int on = 3 * 44032;
            auto mk = [type] (float amt, bool power) { CP p = neutralChain(); p.reverb = amt; p.reverbType = (float) type; p.on[CP::kReverb] = power; return p; };
            const auto panel = runBand(tone, [&] (int i) { return mk(100.0f, ! (i >= 2 * 44032 && i < on)); }, false);
            const auto knob  = runBand(tone, [&] (int i) { return mk(i < on ? 0.0f : 100.0f, true); }, false);
            const double pnl = hfPeakDb(panel, on, on + 2646), knb = hfPeakDb(knob, on, on + 2646);
            std::cout << "    REVERB power on, " << typeName[type] << ": " << juce::String(pnl, 1) << " dBFS above 5 kHz in 60 ms (knob 0 -> 100: "
                      << juce::String(knb, 1) << ")" << std::endl;
            CHECK(pnl <= knb + 3.0);
        }
    }

    std::cout << "FLUTTER (D-M3): a depth step (automation, a history restore) bends the pitch no further than FLUTTER's own swing" << std::endl;
    {
        // A 1 kHz sine, the knob stepped in one block at six phases of the wobble; the worst pitch in
        // the 100 ms after it against FLUTTER 100's steady swing (~+95 / -102 at rate 50). Audit:
        // 100 -> 25 read +534 / -360 cents. (To 0 or panel off the wet crossfades out against the
        // base read instead, as WARP's does: not a depth move, not this test.)
        struct Step { const char* name; float from, to; bool panelOff; };
        for (const Step& st : { Step { "100 -> 25", 100, 25, false }, Step { "100 -> 50", 100, 50, false }, Step { "25 -> 100", 25, 100, false },
                                Step { "50 -> 100", 50, 100, false }, Step { "0 -> 100", 0, 100, false } })
        {
            double up = -1.0e9, down = 1.0e9;
            for (int k = 0; k < 6; ++k)
            {
                const int sw = (int) ((4.0 + 0.137 * k) * bandSr) / 512 * 512;
                double u, d;
                centsRange([&] (int i) { CP p = neutralChain(); p.flutter = i < sw ? st.from : st.to;
                                         if (st.panelOff && i >= sw) p.on[CP::kFlutter] = false; return p; },
                           (double) sw / bandSr + 0.1, (double) sw / bandSr, u, d);
                up = juce::jmax(up, u); down = juce::jmin(down, d);
            }
            std::cout << "    FLUTTER " << st.name << ": " << juce::String(up, 1) << " / " << juce::String(down, 1) << " cents in the 100 ms after" << std::endl;
            CHECK(up <= 110.0 && down >= -115.0);
        }
    }

    std::cout << "print (D-M4): a wet print primes for the live tail (REVERB's T30, ECHO's repeats to -60 dB, at least 1 s), and is still the audition's pass that many wraps in" << std::endl;
    {
        // The count: HALL / PLATE at DECAY 100 ring 5.5 s, SPRING 3 s, ECHO 100 at TIME 100 ~4.4 s
        // (0.35 s a repeat at feedback 0.55, -5.2 dB each); a panel off or an amount at 0 has none.
        struct Case { const char* name; std::vector<std::pair<const char*, float>> set; double tail; };
        const Case cases[] = { { "dry chain", {}, 0.0 },
                               { "HALL DECAY 100", { { "reverb", 100 }, { "reverbDecay", 100 }, { "reverbType", 1 } }, 5.5 },
                               { "PLATE DECAY 0", { { "reverb", 50 }, { "reverbDecay", 0 }, { "reverbType", 0 } }, 0.78 },
                               { "SPRING DECAY 100", { { "reverb", 30 }, { "reverbDecay", 100 }, { "reverbType", 2 } }, 3.0 },
                               { "ECHO 100, TIME 100", { { "echo", 100 }, { "echoTime", 100 } }, 0.35 * (1.0 + 3.0 / -std::log10(0.55)) },
                               { "HALL DECAY 100, panel off", { { "reverb", 100 }, { "reverbDecay", 100 }, { "reverbOn", 0 } }, 0.0 } };
        for (const auto& cs : cases)
        {
            LoopNestProcessor p;
            for (const auto& [id, v] : cs.set) setNatural(p, id, v);
            for (double region : { 0.25, 0.5, 2.0, 8.0 })
            {
                const int want = (int) std::ceil(juce::jmax(1.0, cs.tail) / region - 1.0e-9);
                const int got = p.printPrimePasses(region);
                if (std::abs(got - want) > 1) std::cout << "    " << cs.name << ", " << region << " s: " << got << " passes, want " << want << std::endl;
                CHECK(std::abs(got - want) <= 1 && (double) got * region >= juce::jmax(1.0, cs.tail) - 1.0e-6);
            }
        }
        // A 0.33 s trim under HALL DECAY 100: 17 priming passes (it was 4), and the print is the
        // audition's pass 18 at the latency. (On a short loop the tanks' modulation moves the wet
        // pass by pass, +/-2.5 dB around a mean that never settles on the audit's drum loop: what
        // a print of one pass catches is where that wander is, more passes in or not.)
        const auto loopFile = writeClickLoop(root.getChildFile("elsewhere/dm4_90bpm.wav"), 90.0, 8, 0.5f, 9, 0.5);
        LoopNestProcessor p;
        setNatural(p, "reverb", 100.0f); setNatural(p, "reverbDecay", 100.0f); setNatural(p, "reverbType", 1.0f);
        p.prepareToPlay(44100.0, 512);
        loadVia(p, loopFile);
        p.setSyncEnabled(false);
        trimTo(p, 0.5, 1.0, 8.5);                         // half a beat at 90 (0.33 s), a tick at its head
        p.setDryHatch(false);
        juce::AudioBuffer<float> w;
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(p.renderLoop()) })
            {
                w.setSize((int) r->numChannels, (int) r->lengthInSamples);
                r->read(&w, 0, (int) r->lengthInSamples, 0, true, true);
            }
        }
        const int P = w.getNumSamples();
        CHECK(P == 14700);
        if (P > 0)
        {
            const int prime = p.printPrimePasses((double) P / 44100.0), lat = p.getLatencySamples();
            CHECK(prime == 17);
            p.setLooping(true);
            p.setPlaying(true);
            FakeHost idle;                                 // (no playhead: free PLAY)
            const auto aud = runHost(p, idle, ((prime + 2) * P + lat) / 512 + 2);
            const int head = juce::roundToInt(0.005 * 44100.0);
            double e = 0.0, r = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = head; i < P; ++i)
                {
                    const double y = w.getSample(ch, i), d = (double) aud.getSample(ch, prime * P + lat + i) - y;
                    e += d * d; r += y * y;
                }
            const double nullDb = 10.0 * std::log10(juce::jmax(1.0e-300, e) / juce::jmax(1.0e-300, r));
            std::cout << "    0.33 s under HALL DECAY 100: " << prime << " priming passes, the print against the audition's pass "
                      << prime + 1 << " null " << juce::String(nullDb, 1) << " dB" << std::endl;
            CHECK(r > 0.0 && nullDb <= -100.0);
        }
    }

    std::cout << "SPRING (C-M4): an 8 kHz file's wet print stays finite (its 4057 Hz low-passes were past Nyquist, a pole at 1.032)" << std::endl;
    {
        const auto f8 = root.getChildFile("elsewhere/eight_90bpm.wav");
        {
            juce::AudioBuffer<float> buf8 = noise(4 * 8000, 41, 0.4f);
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream(f8.createOutputStream());
            auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(8000.0).withNumChannels(2).withBitsPerSample(24));
            writer->writeFromAudioSampleBuffer(buf8, 0, buf8.getNumSamples());
        }
        LoopNestProcessor p;
        setNatural(p, "reverb", 100.0f); setNatural(p, "reverbDecay", 100.0f); setNatural(p, "reverbType", 2.0f);
        p.prepareToPlay(44100.0, 512);
        loadVia(p, f8);
        p.setSyncEnabled(false);
        p.setDryHatch(false);
        juce::AudioBuffer<float> w;
        double wsr = 0.0;
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(p.renderLoop()) })
            {
                wsr = r->sampleRate;
                w.setSize((int) r->numChannels, (int) r->lengthInSamples);
                r->read(&w, 0, (int) r->lengthInSamples, 0, true, true);
            }
        }
        bool finite = w.getNumSamples() > 0;
        for (int ch = 0; ch < w.getNumChannels(); ++ch)
            for (int i = 0; i < w.getNumSamples(); ++i) finite = finite && std::isfinite(w.getSample(ch, i));
        const float pk = w.getNumSamples() > 0 ? peakFrom(w, 0) : 0.0f;
        std::cout << "    print at " << wsr << " Hz, " << w.getNumSamples() << " frames, peak " << pk << std::endl;
        CHECK(wsr == 8000.0 && finite && pk > 0.01f && pk < 4.0f);
        // And the chain itself at the low rates a file can have, 60 s of noise through SPRING at DECAY 100.
        for (double sr : { 8000.0, 11025.0, 16000.0 })
        {
            CharacterChain ch;
            ch.prepare(sr);
            CP q = neutralChain();
            q.reverb = 100; q.reverbDecay = 100; q.reverbType = 2;
            const auto o = runChain(ch, q, noise((int) (60.0 * sr), 42));
            bool ok = true;
            for (int cc = 0; cc < 2; ++cc) for (int i = 0; i < o.getNumSamples(); ++i) ok = ok && std::isfinite(o.getSample(cc, i));
            CHECK(ok && peakFrom(o, 0) < 4.0f);
        }
    }

    std::cout << "REVERB (D-L1): HALL / PLATE no longer carry DC: a DC input's wet decays to ~0 (the audit read 3.4x / 11x the input on DECAY 100)" << std::endl;
    {
        const float dcIn = 0.25f, share = 0.7f * std::pow(10.0f, -3.5f / 20.0f);   // REVERB 100's dry share
        for (int type = 0; type < 3; ++type)
            for (float decay : { 0.0f, 100.0f })
            {
                CharacterChain ch;
                ch.prepare(bandSr);
                CP q = neutralChain();
                q.reverb = 100; q.reverbDecay = decay; q.reverbType = (float) type;
                juce::AudioBuffer<float> in(2, (int) (10.0 * bandSr));
                for (int cc = 0; cc < 2; ++cc) juce::FloatVectorOperations::fill(in.getWritePointer(cc), dcIn, in.getNumSamples());
                const auto o = runChain(ch, q, in);
                double wet[2] = { 0.0, 0.0 };
                const int from = o.getNumSamples() - (int) bandSr;
                for (int cc = 0; cc < 2; ++cc)
                {
                    for (int i = from; i < o.getNumSamples(); ++i) wet[cc] += o.getSample(cc, i) - share * dcIn;
                    wet[cc] /= bandSr * dcIn;
                }
                if (std::abs(wet[0]) > 1.0e-3 || std::abs(wet[1]) > 1.0e-3)
                    std::cout << "    " << typeName[type] << " DECAY " << decay << ": wet DC " << wet[0] << " / " << wet[1] << " x the input" << std::endl;
                CHECK(std::abs(wet[0]) <= 1.0e-3 && std::abs(wet[1]) <= 1.0e-3);
            }
    }

    std::cout << "SYNC print (D-L2): the seam's crossfade doesn't swell a correlated head (first 2 ms against mid-file; it read +1.3 to +2.2 dB)" << std::endl;
    {
        const double sr = 44100.0;
        const int regionLen = (int) (4.0 * sr);
        double worst = 0.0;
        for (double f : { 2000.0, 441.0 })
            for (double ratio : { 1.0, 1.2, 0.8, 1.5 })
            {
                std::vector<float> src((size_t) regionLen);
                for (int i = 0; i < regionLen; ++i) src[(size_t) i] = (float) (0.5 * std::sin(juce::MathConstants<double>::twoPi * f * i / sr));
                const int numOut = (int) std::lround(regionLen * ratio);
                std::vector<float> l((size_t) numOut), r((size_t) numOut);
                CHECK(loopnest::renderCyclicStretch([&] (int k, float& l0, float& r0) { l0 = r0 = src[(size_t) k]; },
                                                    regionLen, numOut, sr, 0.0f, l.data(), r.data()));
                const int w = (int) std::lround(sr / f);             // one period
                auto level = [&] (int from) { double e = 0.0; for (int i = from; i < from + w; ++i) e += (double) l[(size_t) i] * l[(size_t) i]; return 10.0 * std::log10(e / w); };
                const double mid = level(numOut / 2);
                double head = 0.0;
                for (int k = 0; k * w < (int) (0.002 * sr); ++k)
                    head = std::abs(level(k * w) - mid) > std::abs(head) ? level(k * w) - mid : head;
                worst = juce::jmax(worst, std::abs(head));
                if (ratio == 1.0) CHECK(std::abs(head) <= 0.1);       // a seam that closes: the same audio, no swell at all
                CHECK(std::abs(head) <= 1.0);                         // one that doesn't: within a dB either way
            }
        std::cout << "    worst head level against mid-file " << juce::String(worst, 2) << " dB" << std::endl;
    }

    std::cout << "lock: a host with 3/4 bar starts and no time signature keeps one anchor (it re-anchored, and re-primed, every bar)" << std::endl;
    {
        for (bool meter : { false, true })
        {
            LoopNestProcessor p;
            FakeHost h;
            h.bpm = 90.0; h.playing = true; h.num = 3; h.noMeter = ! meter;
            lockUp(p, h, gridA);
            const auto out = runHost(p, h, blocksFor(48.0, 90.0), 512, [&] (int blk) { if (blk == 0) p.setPlaying(true); });
            const auto l = landings(out, h, 0, (int) h.log.size());
            std::cout << "    " << (meter ? "3/4 given" : "no time signature") << ": " << p.getLockPrimeCount() << " prime(s) over 16 bars" << std::endl;
            printLanding(meter ? "3/4 given" : "no time signature", l);
            CHECK(p.isBarLocked() && p.getLockPrimeCount() == 1 && l.missing == 0 && l.worstMs <= kTolMs);
        }
    }


    // --- Batch 3 review (10-18): H1, H2, L1, L2, L4 and the knob-to-0 glide ---

    std::cout << "history (H1): BACK under the lock lands the step's knobs on the bar line as heard: the old loop's last latency() frames keep the old WARP / FLUTTER / DRIVE (at HEAD a WARP 100 -> 0 stepped 0.42 there, DRIVE fell 3 dB)" << std::endl;
    {
        // The reviewer's case: A (a 220 Hz tone) picked, B (330 Hz) spun in with a knob set,
        // BACK to A: the knob goes back to 0 on the line. In the last latency() frames before
        // the heard line B must still play as it did: no splice step, its level kept.
        const char* names[] = { "no knob", "WARP 100 -> 0", "DRIVE 100 -> 0", "FLUTTER 100 -> 0" };
        for (int variant = 0; variant < 4; ++variant)
        {
            LoopNestProcessor p;
            FakeHost h;
            pickUp(p, h, sineA, 0.0);
            bool toB = false, back = false;
            const auto out = runHost(p, h, blocksFor(10.0, 90.0), 512, [&] (int)
            {
                if (! toB && h.ppq >= 1.3)
                {
                    toB = true;
                    CHECK(p.spinTo(sineB) && p.finishPendingLoad(5000));
                    if (variant == 1) setNatural(p, "warp", 100.0f);
                    if (variant == 2) setNatural(p, "drive", 100.0f);
                    if (variant == 3) setNatural(p, "flutter", 100.0f);
                }
                if (! back && h.ppq >= 5.3) { back = true; CHECK(p.historyBack() && p.finishPendingLoad(5000)); }
            });
            const int lat = p.getLatencySamples(), line = 8 * 29400 + lat, fade = 89;   // the line as heard; the swap's 2 ms fade
            auto rmsOf = [&] (int a0, int a1)
            {
                double e = 0.0;
                for (int i = a0; i < a1; ++i) e += (double) out.getSample(0, i) * out.getSample(0, i);
                return std::sqrt(e / (a1 - a0));
            };
            const float stepBefore = maxStepOf(out, line - 4 * lat, line - lat - 5);
            const float stepWin    = maxStepOf(out, line - lat - 5, line - fade);
            const double levelDb   = 20.0 * std::log10(rmsOf(line - lat + 40, line - fade) / rmsOf(line - 4 * lat, line - 2 * lat));
            std::cout << "    " << names[variant] << ": largest step " << stepWin << " in the last latency() frames (earlier "
                      << stepBefore << "), level there " << juce::String(levelDb, 2) << " dB against earlier" << std::endl;
            // No splice step: at most the new loop's own 0.5 ms fade-in slope (~2x B's steps), which
            // WARP's sway (+/-15 ms against its base) can carry up to 15 ms ahead of the line, so
            // under WARP the window's level holds some of A (the louder tone) and isn't checked.
            CHECK(toB && back && stepWin <= 2.0f * stepBefore && (variant == 1 || std::abs(levelDb) <= 0.5));
        }
    }

    std::cout << "host bypass (H2): the track's audio on the reported latency, never early; a toggle either way crossfades, no step" << std::endl;
    {
        // An impulse through the host's bypass comes out exactly getLatencySamples() late.
        {
            LoopNestProcessor p;
            setNatural(p, "drive", 100.0f); setNatural(p, "reverb", 70.0f);
            p.setFxMode(true);
            p.prepareToPlay(44100.0, 512);
            const int lat = p.getLatencySamples(), at = 3000;
            juce::AudioBuffer<float> in(2, 20 * 512), out(2, 20 * 512), buf(2, 512);
            in.clear();
            in.setSample(0, at, 1.0f); in.setSample(1, at, 1.0f);
            juce::MidiBuffer midi;
            for (int blk = 0; blk < 20; ++blk)
            {
                for (int ch = 0; ch < 2; ++ch) buf.copyFrom(ch, 0, in, ch, blk * 512, 512);
                p.processBlockBypassed(buf, midi);
                for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, blk * 512, buf, ch, 0, 512);
            }
            float rest = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < out.getNumSamples(); ++i)
                    if (i != at + lat) rest = juce::jmax(rest, std::abs(out.getSample(ch, i)));
            // (Rest: the first block's 5 ms fade from the chain, which is DRIVE on silence, ~1e-9.)
            CHECK(lat > 0 && near(out.getSample(0, at + lat), 1.0f, 1.0e-6f) && near(out.getSample(1, at + lat), 1.0f, 1.0e-6f)
                  && rest < 1.0e-6f);
        }
        // A 220 Hz tone through DRIVE 100 and REVERB 70 (the wet far from the dry): the host's
        // bypass on for 40 blocks, then off. The largest step across each toggle against the
        // largest step anywhere else in the run.
        LoopNestProcessor p;
        setNatural(p, "drive", 100.0f); setNatural(p, "reverb", 70.0f);
        p.setFxMode(true);
        p.prepareToPlay(44100.0, 512);
        const auto tone = bandSine(3.0);
        juce::AudioBuffer<float> out(2, 120 * 512), buf(2, 512);
        juce::MidiBuffer midi;
        for (int blk = 0; blk < 120; ++blk)
        {
            for (int ch = 0; ch < 2; ++ch) buf.copyFrom(ch, 0, tone, ch, blk * 512, 512);
            if (blk >= 40 && blk < 80) p.processBlockBypassed(buf, midi);
            else                       p.processBlock(buf, midi);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, blk * 512, buf, ch, 0, 512);
        }
        const float on = maxStepOf(out, 40 * 512 - 4, 40 * 512 + 512), off = maxStepOf(out, 80 * 512 - 4, 80 * 512 + 512);
        const float steady = juce::jmax(maxStepOf(out, 20 * 512, 39 * 512), maxStepOf(out, 50 * 512, 79 * 512),
                                        maxStepOf(out, 90 * 512, 120 * 512));
        std::cout << "    toggle on: largest step " << on << ", off " << off << ", steady " << steady << std::endl;
        CHECK(on <= 1.25f * steady && off <= 1.25f * steady);
    }

    std::cout << "unprepared (L1): blocks before prepareToPlay touch no line (an unprepared chain has no delay): finite out, no latency, the host's bypass passes the input" << std::endl;
    {
        LoopNestProcessor fx;
        fx.setFxMode(true);
        setNatural(fx, "reverb", 70.0f);       // (an unprepared tank read an empty ring: ASan, 10-18)
        setNatural(fx, "warp", 60.0f);
        const auto in = noise(512, 61);
        juce::AudioBuffer<float> buf(in);
        juce::MidiBuffer midi;
        fx.processBlock(buf, midi);
        bool finite = true;
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 512; ++i) finite = finite && std::isfinite(buf.getSample(ch, i));
        CHECK(fx.getLatencySamples() == 0 && finite);
        juce::AudioBuffer<float> byp(in);
        fx.processBlockBypassed(byp, midi);
        CHECK(identical(byp, in));
        LoopNestProcessor aud;
        juce::AudioBuffer<float> b2(in);
        aud.processBlock(b2, midi);
        CHECK(b2.getMagnitude(0, 512) == 0.0f);
    }

    std::cout << "seam (L2): a loop that closes on itself (the audio past its end is its head again) doesn't swell at the wrap: a dry print's first 2 ms on the rest (equal power read +3 dB)" << std::endl;
    {
        // 441 Hz: 100 frames a period. A 2 s trim of a 4 s tone is whole periods, so what
        // follows its end is its head exactly.
        const auto tone441 = writeWav(root.getChildFile("elsewhere/closing441.wav"), 4.0, 441.0f, 0.5f);
        LoopNestProcessor p;
        p.prepareToPlay(44100.0, 512);
        loadVia(p, tone441);
        p.setSyncEnabled(false);
        setNatural(p, "start", 0.0f);
        setNatural(p, "end", (float) ((88200.0 + 0.25) / (176400.0 - 1.0)));
        p.setDryHatch(true);
        juce::AudioBuffer<float> w;
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(p.renderLoop()) })
            {
                w.setSize((int) r->numChannels, (int) r->lengthInSamples);
                r->read(&w, 0, (int) r->lengthInSamples, 0, true, true);
            }
        }
        CHECK(w.getNumSamples() == 88200);
        if (w.getNumSamples() == 88200)
        {
            const double head = w.getRMSLevel(0, 0, 100), mid = w.getRMSLevel(0, 44100, 100);
            const double db = 20.0 * std::log10(head / mid);
            std::cout << "    the seam's first period against mid-file: " << juce::String(db, 2) << " dB" << std::endl;
            CHECK(std::abs(db) <= 0.1);
        }
    }

    std::cout << "FLUTTER (L4): a depth step up never kinks the pitch at the base's edge (soft knee, as WARP's): RATE 100, 25 -> 100 at twelve phases" << std::endl;
    {
        double up = -1.0e9, down = 1.0e9;
        for (int k = 0; k < 12; ++k)
        {
            const int sw = (int) ((4.0 + 0.0613 * k) * bandSr) / 512 * 512;
            double u, d;
            centsRange([&] (int i) { CP q = neutralChain(); q.flutterRate = 100; q.flutter = i < sw ? 25.0f : 100.0f; return q; },
                       (double) sw / bandSr + 0.15, (double) sw / bandSr, u, d);
            up = juce::jmax(up, u); down = juce::jmin(down, d);
        }
        double su, sd;
        centsRange([&] (int) { CP q = neutralChain(); q.flutterRate = 100; q.flutter = 100; return q; }, 12.0, 1.0, su, sd);
        std::cout << "    after the step " << juce::String(up, 1) << " / " << juce::String(down, 1) << " cents (FLUTTER 100's own swing at RATE 100 "
                  << juce::String(su, 1) << " / " << juce::String(sd, 1) << ")" << std::endl;
        CHECK(up <= su + 25.0 && down >= sd - 25.0);
    }

    std::cout << "WARP / FLUTTER: the knob to 0 glides the sway out (raised cosine, 1 s / 0.5 s), then lands exactly on the base: no chirp (it read +107 / -140 and +67 / -71 cents)" << std::endl;
    {
        for (int which = 0; which < 2; ++which)
        {
            const double glide = which == 0 ? 1.0 : 0.5;
            double up = -1.0e9, down = 1.0e9;
            for (int k = 0; k < 6; ++k)
            {
                const int sw = (int) ((4.0 + 0.137 * k) * bandSr) / 512 * 512;
                double u, d;
                centsRange([&] (int i) { CP q = neutralChain();
                                         if (which == 0) q.warp = i < sw ? 100.0f : 0.0f; else q.flutter = i < sw ? 100.0f : 0.0f;
                                         return q; },
                           (double) sw / bandSr + glide + 0.2, (double) sw / bandSr, u, d);
                up = juce::jmax(up, u); down = juce::jmin(down, d);
            }
            // Its own swing at 100 (it fades with the knob's ~15 ms slew), then only the glide's bend.
            double su, sd, gu = -1.0e9, gd = 1.0e9;
            centsRange([&] (int) { CP q = neutralChain(); if (which == 0) q.warp = 100; else q.flutter = 100; return q; }, 12.0, 1.0, su, sd);
            for (int k = 0; k < 6; ++k)
            {
                const int sw = (int) ((4.0 + 0.137 * k) * bandSr) / 512 * 512;
                double u, d;
                centsRange([&] (int i) { CP q = neutralChain();
                                         if (which == 0) q.warp = i < sw ? 100.0f : 0.0f; else q.flutter = i < sw ? 100.0f : 0.0f;
                                         return q; },
                           (double) sw / bandSr + glide + 0.2, (double) sw / bandSr + 0.1, u, d);
                gu = juce::jmax(gu, u); gd = juce::jmin(gd, d);
            }
            std::cout << "    " << (which == 0 ? "WARP" : "FLUTTER") << " 100 -> 0: " << juce::String(up, 1) << " / " << juce::String(down, 1)
                      << " cents from the move (its steady swing " << juce::String(su, 1) << " / " << juce::String(sd, 1) << "), from 100 ms on "
                      << juce::String(gu, 1) << " / " << juce::String(gd, 1) << std::endl;
            CHECK(up <= su && down >= sd && gu <= 45.0 && gd >= -45.0);
            // Past the glide the chain is the input at its fixed delay again, bit for bit.
            const int sw = 2 * 44032, n = sw + (int) ((glide + 0.1) * bandSr);
            const auto in = noise(n + 22050, 70 + which);
            const auto o = runBand(in, [&] (int i) { CP q = neutralChain();
                                                    if (which == 0) q.warp = i < sw ? 100.0f : 0.0f; else q.flutter = i < sw ? 100.0f : 0.0f;
                                                    return q; }, false);
            CHECK(identical(o, delayed(in, bandLat), n));
        }
    }

    std::cout << "\n" << (checks - failures) << "/" << checks << " checks passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
