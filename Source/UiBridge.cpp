#include "UiBridge.h"
#include "FactoryPresets.h"
#include "BankDrop.h"

namespace
{
    struct FnThread : juce::Thread
    {
        FnThread(const juce::String& n, std::function<void()> f) : juce::Thread(n), fn(std::move(f)) {}
        void run() override { fn(); }
        std::function<void()> fn;
    };
}

UiBridge::UiBridge(LoopNestProcessor& p) : proc(p)
{
    // Re-arm HATCH on reopen: the print survives an editor close (it lives on the
    // processor). hatchState() marks it stale if the settings moved since.
    printed = proc.getLastRender();
    if (! printed.existsAsFile())
        printed = juce::File();
    samplePath = proc.getCurrentSampleFile().getFullPathName();
    if (proc.takeRescanOwed())
        proc.rescanFolder();   // the last editor closed mid-drop, before its rescan
}

UiBridge::~UiBridge()
{
    // A drag the editor's close cut off never sends its end: end it here, or presets,
    // CLEAR and history keep skipping that param as if a hand were on it (C-M11). The
    // page is the only thing that leaves a gesture open, so every open one is its.
    proc.endOpenGestures();

    alive.reset();
    if (dropThread != nullptr)
    {
        // Join, never kill: it stops before its next file (copyAll) and skips its rescan.
        dropThread->signalThreadShouldExit();
        dropWake.signal();
        dropThread->waitForThreadToExit(-1);
    }
}


void UiBridge::spin()
{
    // LIVE is left when the new loop lands (the processor's load), never before it:
    // the audition would play the previous loop until then (E-M1).
    proc.spin(/*playWhenLoaded*/ proc.isFxMode());
}

void UiBridge::togglePlay()
{
    // From LIVE: back to AUDITION, audible. Else the key: play/pause, or the mute
    // while the host's transport owns the audition (LoopNestProcessor::pressPlay).
    if (proc.isFxMode()) { proc.setFxMode(false); proc.setMuted(false); proc.setPlaying(true); }
    else                   proc.pressPlay();
}

void UiBridge::setLooping(bool on) { proc.setLooping(on); }
void UiBridge::setLive(bool live)  { proc.setFxMode(live); }
void UiBridge::setSync(bool on)    { proc.setSyncEnabled(on); }
void UiBridge::setTempo(double b)  { proc.setFreeBpm(b); }
void UiBridge::setPrintFx(bool fx) { proc.setDryHatch(! fx); }
void UiBridge::seek(float norm)    { proc.seekToNorm(norm); }
void UiBridge::setBpm(double bpm)  { proc.setLoopBpm(bpm); }
bool UiBridge::fitBpm()            { return proc.setLoopBpmFromRegion(); }
void UiBridge::setBarOrigin(double n) { proc.setBarOrigin(n); }

juce::var UiBridge::bins() const
{
    juce::Array<juce::var> out;
    for (const auto& b : proc.getPrintBins())
        out.add(b);
    return out;
}
bool UiBridge::setBin(const juce::String& name) { return proc.setPrintBin(name); }
bool UiBridge::newBin(const juce::String& name) { return proc.createPrintBin(name); }
bool UiBridge::showBin() const
{
    // The picked bin, selected inside the Prints root; the root itself with no bin (or a
    // missing one). Never creates a folder: with no Prints root yet there is nothing to show.
    const auto root = proc.getPrintsRoot();
    const auto bin = proc.getPrintBin();
    const auto dir = bin.isNotEmpty() && ! proc.isPrintBinMissing() ? proc.getBinFolder(bin) : root;
    if (! dir.isDirectory())
        return false;
    dir.revealToUser();
    return true;
}

bool UiBridge::pick(int index)
{
    const auto files = proc.getSampleFiles();
    if (! juce::isPositiveAndBelow(index, files.size())) return false;
    return proc.loadSampleFile(files.getReference(index), /*playWhenLoaded*/ proc.isFxMode());   // leaves LIVE as it lands
}

bool UiBridge::historyStep(int delta)
{
    // One with nowhere to go (an end, a missing file) changes nothing, LIVE included;
    // one that loads leaves LIVE as it lands.
    const bool fromLive = proc.isFxMode();
    return delta < 0 ? proc.historyBack(fromLive) : proc.historyForward(fromLive);
}

bool UiBridge::spinPick(int index)
{
    const auto files = proc.getSampleFiles();
    if (! juce::isPositiveAndBelow(index, files.size())) return false;
    return proc.spinTo(files.getReference(index), /*playWhenLoaded*/ proc.isFxMode());   // leaves LIVE as it lands
}

bool UiBridge::openRecent(const juce::String& path)
{
    if (proc.isExtracting()) return false;
    for (const auto& f : proc.getRecentFolders())
        if (f.getFullPathName() == path)
        {
            if (f != proc.getSampleFolder()) proc.setSampleFolder(f);
            else
            {
                // The same bank again: after a remount its files (and a restored set's
                // missing loop, B4) are back, so walk it and look again.
                proc.rescanFolder();
                proc.relinkMissingSample();
            }
            filesSent = false;   // re-send: the recents' order changed
            return true;
        }
    return false;
}

namespace
{
    juce::Array<juce::File> filesOf(const juce::StringArray& paths)
    {
        juce::Array<juce::File> out;
        for (const auto& p : paths)
            if (juce::File::isAbsolutePath(p)) out.add(juce::File(p));   // not a path: skipped
        return out;
    }
}

bool UiBridge::canDrop(const juce::StringArray& paths) const
{
    return loopnest::bankdrop::check(filesOf(paths), proc.getSampleFolder()) == loopnest::bankdrop::Verdict::ok;
}

bool UiBridge::drop(const juce::StringArray& paths)
{
    auto items = filesOf(paths);
    const auto bank = proc.getSampleFolder();
    if (loopnest::bankdrop::check(items, bank) != loopnest::bankdrop::Verdict::ok)
        return false;
    {
        const juce::ScopedLock sl(dropLock);
        dropQueue.push_back({ std::move(items), bank });
        ++dropsBusy;
    }
    dropLoadGen = proc.getLoadGeneration();   // a load after this one is a newer choice than the drop's pick
    if (dropThread == nullptr)
    {
        // The worker's handle on `alive` is made here, on the message thread (the
        // destructor resets the member, so the worker never reads it).
        dropThread = std::make_unique<FnThread>("LoopNest drop", [this, aliveRef = std::weak_ptr<bool>(alive)]
        {
            while (! juce::Thread::currentThreadShouldExit())
            {
                dropWake.wait(-1);
                for (;;)
                {
                    DropJob job;
                    {
                        const juce::ScopedLock sl(dropLock);
                        if (dropQueue.empty() || juce::Thread::currentThreadShouldExit()) break;
                        job = std::move(dropQueue.front());
                        dropQueue.pop_front();
                    }
                    juce::File first;
                    int failed = 0;
                    for (const auto& r : loopnest::bankdrop::copyAll(job.items, job.bank))
                    {
                        if (r.outcome == loopnest::bankdrop::Outcome::failed) ++failed;
                        else if (first == juce::File()) first = r.dest;
                    }
                    // The rack's list (ln_files on the next poll). Not when closing: the walk
                    // would hold up the close, so the next editor's bridge does it.
                    if (first != juce::File())
                    {
                        if (juce::Thread::currentThreadShouldExit()) proc.markRescanOwed();
                        else                                         proc.rescanFolder();
                    }
                    {
                        const juce::ScopedLock sl(dropLock);
                        --dropsBusy;
                        if (first != juce::File()) dropPick = first;
                        else                       dropFailed += juce::jmax(1, failed);
                    }
                    juce::MessageManager::callAsync([this, aliveRef] { if (! aliveRef.expired()) landDrops(); });
                }
            }
        });
        dropThread->startThread();
    }
    dropWake.signal();
    return true;
}

void UiBridge::landDrops()
{
    juce::File pick;
    int failed = 0;
    {
        const juce::ScopedLock sl(dropLock);
        pick = std::exchange(dropPick, juce::File());
        failed = std::exchange(dropFailed, 0);
    }
    // Nothing on screen says so yet (U-L6): the HATCH log does.
    if (failed > 0 && log) log("drop: nothing landed (" + juce::String(failed) + " failed)");
    // In AUDITION a rack pick (trim reset, knobs kept, no history entry), unless a load
    // was asked for since the drop (a row, a spin): that choice is newer.
    if (pick != juce::File() && ! proc.isFxMode() && proc.getLoadGeneration() == dropLoadGen)
        if (proc.loadSampleFile(pick))
            dropLoadGen = proc.getLoadGeneration();
}

bool UiBridge::finishDrops(int timeoutMs)
{
    const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax(0, timeoutMs);
    for (;;)
    {
        {
            const juce::ScopedLock sl(dropLock);
            if (dropsBusy == 0) break;
        }
        if (juce::Time::getMillisecondCounter() >= until) return false;
        juce::Thread::sleep(2);
    }
    landDrops();
    return true;
}

juce::String UiBridge::presetName(int idx)
{
    return idx >= 0 && idx < loopnest::kNumFactoryPresets ? juce::String(loopnest::kFactoryPresets[idx].name)
                                                          : juce::String("INIT");
}

void UiBridge::selectPreset(int idx)
{
    idx = juce::jlimit(-1, loopnest::kNumFactoryPresets - 1, idx);
    // INIT restores the parameter layout's defaults; a preset its own column. One
    // undo step. Either way every panel comes back ON (fromArray's default).
    auto snap = proc.capture();
    if (idx >= 0)
        snap.character = loopnest::CharacterParams::fromArray(loopnest::kFactoryPresets[idx].v);
    else
    {
        for (int j = 0; j < loopnest::kNumPresetParams; ++j)
            if (auto* p = proc.apvts.getParameter(loopnest::CharacterParams::fields()[(size_t) j].id))
                snap.character[j] = p->convertFrom0to1(p->getDefaultValue());
        snap.character.on.fill(true);
    }
    // The label first: a host save between the two never pairs the preset's params with
    // the old label.
    proc.setPresetIndex(idx);
    proc.restore(snap, loopnest::Snapshot::kCharacter);
}

void UiBridge::stepPreset(int delta)
{
    // Cycle INIT, 0 .. n-1, INIT (n + 1 slots).
    const int slots = loopnest::kNumFactoryPresets + 1;
    const int slot  = ((proc.getPresetIndex() + 1 + delta) % slots + slots) % slots;
    selectPreset(slot - 1);
}

bool UiBridge::presetMatches(int idx) const
{
    if (idx < 0 || idx >= loopnest::kNumFactoryPresets) return false;
    const auto& p = loopnest::kFactoryPresets[idx];
    for (int j = 0; j < loopnest::kNumPresetParams; ++j)
        if (auto* param = proc.apvts.getParameter(loopnest::CharacterParams::fields()[(size_t) j].id))
            if (std::abs(param->getValue() - param->convertTo0to1(p.v[j])) > 1.0e-3f)
                return false;
    return allPanelsOn();   // a preset has every panel ON
}

bool UiBridge::allPanelsOn() const
{
    for (auto* id : loopnest::CharacterParams::powerIds())
        if (auto* param = proc.apvts.getParameter(id))
            if (param->getValue() < 0.5f)
                return false;
    return true;
}

bool UiBridge::characterAtDefaults() const
{
    for (int j = 0; j < loopnest::kNumPresetParams; ++j)
        if (auto* param = proc.apvts.getParameter(loopnest::CharacterParams::fields()[(size_t) j].id))
            if (std::abs(param->getValue() - param->getDefaultValue()) > 1.0e-3f)
                return false;
    return allPanelsOn();
}

bool UiBridge::presetDirty() const
{
    const int preset = proc.getPresetIndex();
    return preset >= 0 && ! presetMatches(preset);
}

UiBridge::Hatch UiBridge::hatchState() const
{
    if (! proc.hasSample()) return Hatch::none;
    if (! printed.existsAsFile()) return Hatch::idle;
    return proc.isLastRenderCurrent() ? Hatch::armed : Hatch::stale;
}

bool UiBridge::hatch()
{
    if (! proc.hasSample()) return false;
    proc.setPlaying(false);   // stop the audition before printing
    const auto f = proc.renderLoop();
    if (! f.existsAsFile())
        return false;   // a failed reprint is a failure, even with an older print on disk (C10)
    printed = f;
    return true;
}

juce::File UiBridge::dragFile() const
{
    return hatchState() == Hatch::armed ? printed : juce::File();
}

void UiBridge::dragStarted()
{
    dragFlipped = ! proc.isFxMode();
    dragWasPlaying = proc.isPlaying();   // LIVE stops the audition (setFxMode)
    proc.setFxMode(true);
}

void UiBridge::dragEnded(bool dropped)
{
    if (! dropped && dragFlipped)
    {
        // A cancelled drag puts nothing on the track: back to the audition as it was.
        proc.setFxMode(false);
        if (dragWasPlaying) proc.setPlaying(true);
    }
    dragFlipped = dragWasPlaying = false;
}

std::optional<juce::File> UiBridge::takeSampleIfChanged()
{
    const auto f = proc.getCurrentSampleFile();
    if (sampleSeen && f.getFullPathName() == samplePath)
        return std::nullopt;
    if (sampleSeen)
        printed = juce::File();   // a new loop invalidates the print
    sampleSeen = true;
    samplePath = f.getFullPathName();
    return f;
}

std::optional<juce::var> UiBridge::takeFilesIfChanged()
{
    // Any new scan re-sends (the page resolves a row click by index into this list, so
    // even an interior rename with the same count and ends must). The generation is read
    // before the list: a scan landing between the two re-sends on the next poll. (Recents
    // stay out: openRecent clears filesSent itself, and reading them stats every folder.)
    const auto gen = proc.getFilesGeneration();
    if (filesSent && gen == filesGenSent)
        return std::nullopt;
    filesSent = true;
    filesGenSent = gen;
    const auto files = proc.getSampleFiles();

    juce::Array<juce::var> names;
    for (const auto& f : files)
        names.add(f.getFileName());
    juce::Array<juce::var> recents;
    for (const auto& f : proc.getRecentFolders())
    {
        auto* r = new juce::DynamicObject();
        r->setProperty("name", f.getFileName());
        r->setProperty("path", f.getFullPathName());
        recents.add(juce::var(r));
    }
    auto* o = new juce::DynamicObject();
    o->setProperty("folder", proc.getSampleFolder().getFileName());
    o->setProperty("path", proc.getSampleFolder().getFullPathName());
    o->setProperty("names", names);
    o->setProperty("recents", recents);
    return juce::var(o);
}

juce::var UiBridge::state()
{
    // A host undo puts params back without telling us, so a drifted preset name
    // re-resolves: another preset the params now match exactly, or INIT when
    // they're all at defaults; else it keeps its "*".
    int preset = proc.getPresetIndex();
    if (preset >= 0 && ! presetMatches(preset))
    {
        for (int i = 0; i < loopnest::kNumFactoryPresets; ++i)
            if (presetMatches(i)) { preset = i; break; }
        if (! presetMatches(preset) && characterAtDefaults())
            preset = -1;
        proc.setPresetIndex(preset);
    }

    proc.finishExtractionIfReady();

    auto* o = new juce::DynamicObject();
    const auto sample = proc.getCurrentSampleFile();
    o->setProperty("hasSample", proc.hasSample());
    o->setProperty("sample", proc.getCurrentSampleName());
    o->setProperty("sampleMissing", proc.getMissingSampleName());   // a restored set's loop not found ("" = none)
    o->setProperty("folder", proc.getSampleFolder().getFileName());
    o->setProperty("folderMissing", proc.folderMissing());
    o->setProperty("count", proc.getSampleCount());
    // The loaded loop's row: searched again only when the list or the loop changed (U-M2).
    const auto gen = proc.getFilesGeneration();
    if (! indexValid || gen != indexGen || sample.getFullPathName() != indexPath)
    {
        indexValid = true;
        indexGen = gen;
        indexPath = sample.getFullPathName();
        sampleIndex = proc.getSampleFiles().indexOf(sample);
    }
    o->setProperty("index", sampleIndex);
    o->setProperty("canSpin", proc.canSpin());
    o->setProperty("histBack", proc.canHistoryBack());   // spin history ◀ ▶ (dim at the ends)
    o->setProperty("histFwd", proc.canHistoryForward());
    o->setProperty("loading", proc.isLoadPending());
    o->setProperty("playing", proc.isPlayKeyLit());   // audible playback (a mute under the lock reads off)
    o->setProperty("loop", proc.isLooping());
    o->setProperty("live", proc.isFxMode());
    o->setProperty("sync", proc.isSyncEnabled());
    o->setProperty("freeBpm", proc.getFreeBpm());
    o->setProperty("locked", proc.isBarLocked());   // on the host's bar grid (no UI reads it yet)
    o->setProperty("printFx", ! proc.isDryHatch());
    o->setProperty("playhead", proc.getPlayheadNorm());

    using Src = LoopNestProcessor::BpmSource;
    Src src = Src::none;
    const double bpm = proc.getNativeBpm(src);
    o->setProperty("nativeBpm", bpm);
    o->setProperty("bpmSource", src == Src::name ? "name" : src == Src::length ? "length"
                                 : src == Src::user ? "user" : "none");
    o->setProperty("tagBpm", proc.getTagBpm());
    o->setProperty("barOrigin", proc.getBarOrigin());
    o->setProperty("hostBpm", proc.getHostBpm());

    const auto h = hatchState();
    o->setProperty("hatch", h == Hatch::none ? "none" : h == Hatch::idle ? "idle" : h == Hatch::armed ? "armed" : "stale");
    o->setProperty("printName", printed.getFileName());
    // The bin HATCH prints into ("" = the root). Once a second: re-read the shared pick
    // (another instance may have changed it, E-M2) and stat its folder.
    // The first call checks at once (U-L7: it used to wait a second).
    if (binTick < 0 || ++binTick >= 30) { binTick = 0; proc.reloadPrintBin(); binMissing = proc.isPrintBinMissing(); }
    o->setProperty("bin", proc.getPrintBin());
    o->setProperty("binMissing", binMissing);

    o->setProperty("preset", preset);
    o->setProperty("presetName", presetName(preset));
    o->setProperty("presetDirty", presetDirty());

    o->setProperty("extracting", proc.isExtracting());
    o->setProperty("extractProgress", proc.getExtractProgress());
    o->setProperty("extractKept", proc.getExtractKept());
    o->setProperty("extractStatus", proc.getExtractStatus());

    float in[2], out[2];
    proc.takeLevels(in, out);
    o->setProperty("inL", in[0]);  o->setProperty("inR", in[1]);
    o->setProperty("outL", out[0]); o->setProperty("outR", out[1]);
    return juce::var(o);
}

std::vector<float> UiBridge::computePeaks(const juce::AudioBuffer<float>& b, int n)
{
    std::vector<float> peaks((size_t) juce::jmax(0, n), 0.0f);
    const int len = b.getNumSamples();
    if (n <= 0 || len <= 0) return peaks;
    for (int i = 0; i < n; ++i)
    {
        if ((i & 255) == 0 && juce::Thread::currentThreadShouldExit()) return peaks;   // see the header
        const int a = (int) ((juce::int64) i * len / n);
        const int e = juce::jmax(a + 1, (int) ((juce::int64) (i + 1) * len / n));
        float m = 0.0f;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            m = juce::jmax(m, b.getMagnitude(ch, a, juce::jmin(e, len) - a));
        peaks[(size_t) i] = juce::jmin(1.0f, m);
    }
    return peaks;
}

std::vector<int> UiBridge::computeHits(const juce::AudioBuffer<float>& b, double sampleRate)
{
    std::vector<int> hits;
    const int len = b.getNumSamples(), chans = b.getNumChannels();
    if (len <= 0 || chans <= 0 || sampleRate <= 0.0)
        return hits;

    // Two energy envelopes at a ~0.36 ms grain: the signal, and its first difference
    // (a crude high-pass: hats and snare crack stand clear of a kick's tail).
    const int grain = juce::jmax(4, juce::roundToInt(sampleRate * 0.00036));
    const int n = len / grain;
    if (n < 16)
        return hits;
    std::vector<float> env[2];
    env[0].assign((size_t) n, 0.0f);
    env[1].assign((size_t) n, 0.0f);
    float prev = 0.0f;
    for (int g = 0; g < n; ++g)
    {
        if ((g & 4095) == 0 && juce::Thread::currentThreadShouldExit()) return {};   // see the header
        double full = 0.0, high = 0.0;
        for (int i = g * grain, e = i + grain; i < e; ++i)
        {
            float x = 0.0f;
            for (int ch = 0; ch < chans; ++ch)
                x += b.getSample(ch, i);
            x /= (float) chans;
            full += (double) x * x;
            high += (double) (x - prev) * (x - prev);
            prev = x;
        }
        env[0][(size_t) g] = (float) (full / grain);
        env[1][(size_t) g] = (float) (high / grain);
    }

    // Novelty per ~3 ms hop: how far the next ~9 ms rise over the loudest of the
    // last ~12 ms, in dB, the larger of the two bands. Floors sit 50 dB under each
    // band's loudest hop, so noise in a quiet tail never reads as a rise.
    const int hop = 8, nh = n / hop;                   // 8 grains
    if (nh < 4)
        return hits;
    std::vector<float> nov((size_t) nh, 0.0f);
    std::vector<char>  band((size_t) nh, 0);
    for (int k = 0; k < 2; ++k)
    {
        if (juce::Thread::currentThreadShouldExit()) return {};
        std::vector<float> he((size_t) nh, 0.0f);
        float top = 0.0f;
        for (int h = 0; h < nh; ++h)
        {
            float sum = 0.0f;
            for (int g = h * hop; g < (h + 1) * hop; ++g) sum += env[k][(size_t) g];
            he[(size_t) h] = sum / (float) hop;
            top = juce::jmax(top, he[(size_t) h]);
        }
        if (top <= 0.0f)
            continue;
        const float floor = top * 1.0e-5f;
        for (int h = 0; h < nh; ++h)
        {
            float ahead = 0.0f, before = 0.0f;
            for (int j = h; j < juce::jmin(nh, h + 3); ++j) ahead = juce::jmax(ahead, he[(size_t) j]);
            for (int j = juce::jmax(0, h - 4); j < h; ++j)  before = juce::jmax(before, he[(size_t) j]);
            const float rise = 10.0f * std::log10((ahead + floor) / (before + floor));
            if (rise > nov[(size_t) h]) { nov[(size_t) h] = rise; band[(size_t) h] = (char) k; }
        }
    }

    // A hit: a rise of 6 dB or more that is the largest within the gap either side.
    const double hopSec = (double) (hop * grain) / sampleRate;
    const int gap = juce::jmax(1, juce::roundToInt(kHitGapMs * 0.001 / hopSec));
    for (int h = 0; h < nh; ++h)
    {
        if ((h & 4095) == 0 && juce::Thread::currentThreadShouldExit()) return {};
        if (nov[(size_t) h] < 6.0f)
            continue;
        bool peak = true;
        for (int j = juce::jmax(0, h - gap); j <= juce::jmin(nh - 1, h + gap) && peak; ++j)
            if (j != h && (nov[(size_t) j] > nov[(size_t) h] || (nov[(size_t) j] == nov[(size_t) h] && j < h)))
                peak = false;
        if (! peak)
            continue;

        // Where the attack starts: the first grain of the rise to pass a tenth of the
        // way from the level before it to its peak, in the band that saw it.
        const auto& e = env[(int) band[(size_t) h]];
        const int g0 = juce::jmax(0, (h - 1) * hop), g1 = juce::jmin(n, (h + 3) * hop);
        float before = 0.0f, top = 0.0f;
        for (int g = juce::jmax(0, g0 - 2 * hop); g < g0; ++g) before = juce::jmax(before, e[(size_t) g]);
        for (int g = g0; g < g1; ++g) top = juce::jmax(top, e[(size_t) g]);
        const float thresh = before + 0.1f * (top - before);
        int at = g0;
        while (at < g1 - 1 && e[(size_t) at] <= thresh) ++at;
        const int frame = at * grain;
        if (hits.empty() || frame > hits.back())
            hits.push_back(frame);
    }
    return hits;
}
