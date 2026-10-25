#include "WebEditor.h"
#include "BinaryData.h"

namespace
{
    // Float parameters the page drives through slider relays (relay name = id).
    const char* const kSliderIds[] = {
        "pitch", "pitchGlide", "input", "output", "start", "end", "mix",
        "warp", "warpRate", "drive", "driveTone", "flutter", "flutterRate",
        "echo", "reverb", "echoLo", "echoHi", "reverbLo", "reverbHi",
        "eqLo", "eqHi", "width", "widthLo", "widthHi",
        "reverbDecay", "echoTime", "widthMid",
    };
    // Bool parameters (WebToggleButtonRelay, relay name = id): the A/B pair and
    // the six panel power switches (CharacterParams::powerIds()).
    const char* const kToggleIds[] = { "bypass", "gainMatch",
                                       "warpOn", "flutterOn", "driveOn", "echoOn", "reverbOn", "widthOn" };
    // Choice parameters (WebComboBoxRelay, relay name = id): REVERB TYPE.
    const char* const kComboIds[] = { "reverbType" };

    constexpr int kPeakColumns = 1600;    // the whole-file view (scope, HATCH thumbnail)
    constexpr int kHiColumns   = 32768;   // the zoomed scope: 32x of 924 px, sent as base64 bytes
    constexpr int kReadChunk   = 1 << 16; // frames per decode read: a closing editor stops between them

    juce::String mimeFor(const juce::String& path)
    {
        const auto ext = path.fromLastOccurrenceOf(".", false, false).toLowerCase();
        if (ext == "html") return "text/html";
        if (ext == "js")   return "text/javascript";
        if (ext == "css")  return "text/css";
        if (ext == "ttf")  return "font/ttf";
        if (ext == "json") return "application/json";
        if (ext == "svg")  return "image/svg+xml";
        if (ext == "png")  return "image/png";
        return "application/octet-stream";
    }

    // Promise-style native function: run on the message thread, answer at once.
    template <typename Fn>
    juce::WebBrowserComponent::NativeFunction fn(Fn&& f)
    {
        return [f = std::forward<Fn>(f)](const juce::Array<juce::var>& args,
                                         juce::WebBrowserComponent::NativeFunctionCompletion done)
        {
            done(f(args));
        };
    }

    struct FnThread : juce::Thread
    {
        FnThread(const juce::String& n, std::function<void()> f) : juce::Thread(n), fn(std::move(f)) {}
        void run() override { fn(); }
        std::function<void()> fn;
    };

    // The page and nothing else (B6): a navigation anywhere but the bundled resources
    // (a file dropped outside the rack, which WebKit would open in its media player) is
    // refused, so nothing can replace the editor. Refusals go to the HATCH log. JUCE's
    // own about:blank (its unload-when-hidden, off here: withKeepPageLoadedWhenBrowserIsHidden)
    // passes, so a refused unload can never loop.
    struct PageView : juce::WebBrowserComponent
    {
        using juce::WebBrowserComponent::WebBrowserComponent;
        std::function<void(const juce::String&)> refused;
        bool pageAboutToLoad(const juce::String& url) override
        {
            if (url == "about:blank" || url.startsWith(juce::WebBrowserComponent::getResourceProviderRoot())) return true;
            if (refused) refused(url);
            return false;
        }
    };

    bool argBool(const juce::Array<juce::var>& a, int i) { return i < a.size() && (bool) a[i]; }
    double argNum(const juce::Array<juce::var>& a, int i) { return i < a.size() ? (double) a[i] : 0.0; }
}

LoopNestWebEditor::LoopNestWebEditor(LoopNestProcessor& p)
    : AudioProcessorEditor(&p), processor(p), bridge(p)
{
    auto opts = juce::WebBrowserComponent::Options{}
        .withBackend(juce::WebBrowserComponent::Options::Backend::defaultBackend)
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withResourceProvider([this](const juce::String& url) { return getResource(url); });

    for (auto* id : kSliderIds)
    {
        auto* param = dynamic_cast<juce::RangedAudioParameter*>(processor.apvts.getParameter(id));
        jassert(param != nullptr);
        if (param == nullptr) continue;
        SliderLink link;
        link.relay = std::make_unique<juce::WebSliderRelay>(id);
        opts = opts.withOptionsFrom(*link.relay);
        sliders.push_back(std::move(link));
    }
    for (auto* id : kToggleIds)
    {
        auto* param = dynamic_cast<juce::RangedAudioParameter*>(processor.apvts.getParameter(id));
        jassert(param != nullptr);
        if (param == nullptr) continue;
        ToggleLink link;
        link.relay = std::make_unique<juce::WebToggleButtonRelay>(id);
        opts = opts.withOptionsFrom(*link.relay);
        toggles.push_back(std::move(link));
    }
    for (auto* id : kComboIds)
    {
        auto* param = dynamic_cast<juce::RangedAudioParameter*>(processor.apvts.getParameter(id));
        jassert(param != nullptr);
        if (param == nullptr) continue;
        ComboLink link;
        link.relay = std::make_unique<juce::WebComboBoxRelay>(id);
        opts = opts.withOptionsFrom(*link.relay);
        combos.push_back(std::move(link));
    }

    auto* br = &bridge;
    opts = opts
        .withNativeFunction("ln_ready",    fn([this](auto&) { pageReady = true; sendAll(); return juce::var(true); }))
        .withNativeFunction("ln_spin",     fn([br](auto&) { br->spin(); return juce::var(); }))
        .withNativeFunction("ln_play",     fn([br](auto&) { br->togglePlay(); return juce::var(); }))
        .withNativeFunction("ln_spinPick", fn([br](auto& a) { return juce::var(br->spinPick((int) argNum(a, 0))); }))
        .withNativeFunction("ln_back",     fn([br](auto&) { return juce::var(br->historyStep(-1)); }))
        .withNativeFunction("ln_forward",  fn([br](auto&) { return juce::var(br->historyStep(+1)); }))
        .withNativeFunction("ln_loop",     fn([br](auto& a) { br->setLooping(argBool(a, 0)); return juce::var(); }))
        .withNativeFunction("ln_live",     fn([br](auto& a) { br->setLive(argBool(a, 0)); return juce::var(); }))
        .withNativeFunction("ln_sync",     fn([br](auto& a) { br->setSync(argBool(a, 0)); return juce::var(); }))
        .withNativeFunction("ln_tempo",    fn([br](auto& a) { br->setTempo(argNum(a, 0)); return juce::var(); }))
        .withNativeFunction("ln_printFx",  fn([br](auto& a) { br->setPrintFx(argBool(a, 0)); return juce::var(); }))
        .withNativeFunction("ln_preset",   fn([br](auto& a) { br->selectPreset((int) argNum(a, 0)); return juce::var(); }))
        .withNativeFunction("ln_presetStep", fn([br](auto& a) { br->stepPreset((int) argNum(a, 0)); return juce::var(); }))
        .withNativeFunction("ln_seek",     fn([br](auto& a) { br->seek((float) argNum(a, 0)); return juce::var(); }))
        .withNativeFunction("ln_bank",     fn([this](auto&) { chooseFolder(); return juce::var(); }))
        .withNativeFunction("ln_extract",  fn([this](auto&) { startExtraction(); return juce::var(); }))
        .withNativeFunction("ln_extractCancel", fn([this](auto&) { processor.cancelExtraction(); return juce::var(); }))
        // The page turns the overlay off while a popup covers the HATCH tile, so the
        // popup's rows there stay clickable.
        .withNativeFunction("ln_overlay",  fn([this](auto& a) { overlayEnabled = argBool(a, 0); updateOverlay(); return juce::var(); }))
        .withNativeFunction("ln_pick",     fn([br](auto& a) { return juce::var(br->pick((int) argNum(a, 0))); }))
        .withNativeFunction("ln_bpm",      fn([br](auto& a) { br->setBpm(argNum(a, 0)); return juce::var(); }))
        .withNativeFunction("ln_bpmFit",   fn([br](auto&) { return juce::var(br->fitBpm()); }))
        .withNativeFunction("ln_bins",     fn([br](auto&) { return br->bins(); }))
        .withNativeFunction("ln_bin",      fn([br](auto& a) { return juce::var(br->setBin(a.size() > 0 ? a[0].toString() : juce::String())); }))
        .withNativeFunction("ln_binNew",   fn([br](auto& a) { return juce::var(a.size() > 0 && br->newBin(a[0].toString())); }))
        .withNativeFunction("ln_binShow",  fn([br](auto&) { return juce::var(br->showBin()); }))
        .withNativeFunction("ln_barOrigin", fn([br](auto& a) { br->setBarOrigin(argNum(a, 0)); return juce::var(); }))
        .withNativeFunction("ln_openRecent", fn([br](auto& a) { return juce::var(a.size() > 0 && br->openRecent(a[0].toString())); }))
        // The page's corner grip: a scale (window width / 1672), clamped to the limits.
        .withNativeFunction("ln_resize",   fn([this](auto& a) { setScale((float) argNum(a, 0)); return juce::var(); }));

    auto page = std::make_unique<PageView>(opts);
    page->refused = [this](const juce::String& url) { hatchLog("navigation refused: " + url.substring(0, 200)); };
    browser = std::move(page);

    // Attach after the browser exists (the relays emit into it).
    {
        size_t i = 0;
        for (auto* id : kSliderIds)
            if (auto* param = dynamic_cast<juce::RangedAudioParameter*>(processor.apvts.getParameter(id)))
                sliders[i].attachment = std::make_unique<juce::WebSliderParameterAttachment>(*param, *sliders[i].relay, nullptr), ++i;
        i = 0;
        for (auto* id : kToggleIds)
            if (auto* param = dynamic_cast<juce::RangedAudioParameter*>(processor.apvts.getParameter(id)))
                toggles[i].attachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(*param, *toggles[i].relay, nullptr), ++i;
        i = 0;
        for (auto* id : kComboIds)
            if (auto* param = dynamic_cast<juce::RangedAudioParameter*>(processor.apvts.getParameter(id)))
                combos[i].attachment = std::make_unique<juce::WebComboBoxParameterAttachment>(*param, *combos[i].relay, nullptr), ++i;
    }

    addAndMakeVisible(*browser);
    // After the browser, so their NSViews stack above the WKWebView; the drop target
    // first, so HATCH's overlay (which keeps itself last) starts out on top.
    addAndMakeVisible(rackDrop);
    addAndMakeVisible(overlay);
    // Drag-in: files dragged from Finder, Splice or Live over the rack. The view takes
    // no clicks (hitTest: nil), only drags; hover lights the rack (ln_drop).
    rackDrop.accepts = [this](const juce::StringArray& paths) { return bridge.canDrop(paths); };
    rackDrop.onHover = [this](bool h)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("hover", h);
        emit("ln_drop", juce::var(o));
    };
    rackDrop.onDrop = [this](const juce::StringArray& paths) { return bridge.drop(paths); };
    rackDrop.log = [this](const juce::String& m) { hatchLog(m); };
    bridge.log = [this](const juce::String& m) { hatchLog(m); };
    overlay.onHover = [this](bool h)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("hover", h);
        emit("ln_hatch", juce::var(o));
    };
    overlay.onPress = [this](bool d)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("press", d);
        emit("ln_hatch", juce::var(o));
    };
    overlay.log = [this](const juce::String& m) { hatchLog(m); };
    overlay.onClick = [this]
    {
        const auto st = bridge.hatchState();
        hatchLog("click, state " + juce::String((int) st) + " (0 none, 1 idle, 2 armed, 3 stale)");
        if (st != UiBridge::Hatch::idle && st != UiBridge::Hatch::stale) return;   // armed: a click is a no-op, drag it
        const bool ok = bridge.hatch();
        hatchLog(juce::String("hatch() -> ") + (ok ? "ok " : "FAILED ") + bridge.lastPrint().getFullPathName());
        auto* o = new juce::DynamicObject();
        o->setProperty("ok", ok);
        o->setProperty("name", bridge.lastPrint().getFileName());
        emit("ln_hatched", juce::var(o));
    };
    overlay.fileToDrag  = [this] { return bridge.dragFile(); };
    overlay.onDragStart = [this] { bridge.dragStarted(); };
    overlay.onDragEnd   = [this](bool dropped)
    {
        hatchLog(juce::String("drag ended, ") + (dropped ? "dropped" : "cancelled"));
        bridge.dragEnded(dropped);
    };

    // The worker's handle on `alive` is made here, on the message thread: the
    // destructor resets the member, so the worker must never read it. It also stops
    // between chunks of a decode and between the scan's stages once asked to exit
    // (computePeaks/computeHits check too), so the destructor's join is short.
    peakThread = std::make_unique<FnThread>("LoopNest peaks", [this, aliveRef = std::weak_ptr<bool>(alive)]
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        auto stop = [] { return juce::Thread::currentThreadShouldExit(); };
        while (! stop())
        {
            peakWake.wait(200);
            juce::File f;
            int gen;
            {
                const juce::ScopedLock sl(peakLock);
                f = std::exchange(peakRequest, juce::File());
                gen = peakGen.load();
            }
            if (f == juce::File()) continue;

            juce::Array<juce::var> peaks, hits;
            juce::String hi;
            double seconds = 0.0;
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor(f) })
            {
                // Loops are short; cap the read at ten minutes all the same.
                const auto len = (int) juce::jmin<juce::int64>(r->lengthInSamples, (juce::int64) (r->sampleRate * 600.0));
                seconds = r->sampleRate > 0 ? (double) r->lengthInSamples / r->sampleRate : 0.0;
                juce::AudioBuffer<float> buf((int) juce::jlimit(1u, 2u, r->numChannels), juce::jmax(1, len));
                for (int at = 0; at < len && ! stop(); at += kReadChunk)
                    r->read(&buf, at, juce::jmin(kReadChunk, len - at), at, true, buf.getNumChannels() > 1);
                if (stop()) break;
                for (float v : UiBridge::computePeaks(buf, kPeakColumns))
                    peaks.add(std::round(v * 1000.0f) / 1000.0f);
                // Hi-res columns for the zoom, one byte each (sqrt-companded so quiet tails
                // keep some shape), capped at one column per sample.
                const auto hp = UiBridge::computePeaks(buf, juce::jmin(kHiColumns, len));
                if (stop()) break;
                juce::MemoryBlock mb(hp.size());
                auto* bytes = static_cast<juce::uint8*>(mb.getData());
                for (size_t i = 0; i < hp.size(); ++i)
                    bytes[i] = (juce::uint8) juce::jlimit(0, 255, (int) std::lround(std::sqrt(juce::jlimit(0.0f, 1.0f, hp[i])) * 255.0f));
                // Standard base64 for the page's atob (MemoryBlock::toBase64Encoding is JUCE's own format).
                hi = juce::Base64::toBase64(mb.getData(), mb.getSize());
                // The drum hits SNAP lands on, 0..1 on the trim's own scale (frame / last frame).
                if (len > 1)
                    for (int frame : UiBridge::computeHits(buf, r->sampleRate))
                        hits.add((double) frame / (double) (juce::jmax<juce::int64>(2, r->lengthInSamples) - 1));
                if (stop()) break;
            }
            auto* o = new juce::DynamicObject();
            o->setProperty("path", f.getFullPathName());
            o->setProperty("name", f.getFileName());
            o->setProperty("seconds", seconds);
            o->setProperty("peaks", peaks);
            o->setProperty("hi", hi);
            o->setProperty("hits", hits);
            juce::var v(o);
            juce::MessageManager::callAsync([this, v, gen, alive = aliveRef]
            {
                if (alive.expired() || gen != peakGen.load()) return;
                lastPeaks = v;
                emit("ln_peaks", v);
            });
        }
    });
    peakThread->startThread();

   #if LOOPNEST_DIAG_LOG   // off in the public build (CMake LOOPNEST_DIAG_LOG): logFile stays unset
    logFile = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                  .getChildFile("Library/Logs").getChildFile(JucePlugin_Name).getChildFile("hatch.log");
   #endif
    hatchLog(juce::String("---- editor open, ") + juce::PluginHostType().getHostDescription()
             + ", wrapper " + juce::String(juce::AudioProcessor::getWrapperTypeDescription(processor.wrapperType)));

    // Resizable at the reference aspect: the host's frame where it offers one, and the
    // page's corner grip (ln_resize) everywhere. The scale is remembered app-wide.
    setResizable(true, false);
    setResizeLimits(juce::roundToInt(kRefW * kMinScale), juce::roundToInt(kRefH * kMinScale),
                    juce::roundToInt(kRefW * kMaxScale), juce::roundToInt(kRefH * kMaxScale));
    if (auto* c = getConstrainer()) c->setFixedAspectRatio(kRefW / kRefH);
    const float saved = processor.recallEditorScale();
    const float s0 = saved > 0.0f ? juce::jlimit(kMinScale, kMaxScale, saved) : kScale;
    setSize(juce::roundToInt(kRefW * s0), juce::roundToInt(kRefH * s0));
    browser->goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
    startTimerHz(30);
}

LoopNestWebEditor::~LoopNestWebEditor()
{
    stopTimer();
    if (scaleToSave > 0.0f) processor.rememberEditorScale(scaleToSave);
    hatchLog("editor closed");
    alive.reset();
    // Join, never kill (as the processor's loader): stopThread's timeout would
    // pthread_cancel without a join and leave the worker able to touch this editor.
    // It checks threadShouldExit between decode chunks and scan stages.
    peakThread->signalThreadShouldExit();
    peakWake.signal();
    peakThread->waitForThreadToExit(-1);
    // Decision 3 (09-28): closing the window stops the audition; LIVE keeps processing.
    processor.setPlaying(false);
}

void LoopNestWebEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff030506));   // the page's --deep, shown until the page paints
}

void LoopNestWebEditor::resized()
{
    browser->setBounds(getLocalBounds());
    updateOverlay();
    scaleToSave = (float) getWidth() / kRefW;
    scaleSaveTicks = 30;   // saved a second after the last resize
}

void LoopNestWebEditor::setScale(float s)
{
    s = juce::jlimit(kMinScale, kMaxScale, s);
    const int w = juce::roundToInt(kRefW * s), h = juce::roundToInt(kRefH * s);
    if (w != getWidth() || h != getHeight()) setSize(w, h);
}

void LoopNestWebEditor::hatchLog(const juce::String& m)
{
    if (logFile == juce::File()) return;   // always, unless built with LOOPNEST_DIAG_LOG
    if (! logFile.getParentDirectory().isDirectory()) logFile.getParentDirectory().createDirectory();
    if (logFile.getSize() > 512 * 1024) logFile.deleteFile();   // diagnostics only: keep it small
    logFile.appendText(juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S ") + m + "\n", false, false, "\n");
}

void LoopNestWebEditor::updateOverlay()
{
    const float s = (float) getWidth() / kRefW;
    const auto r = hatchTileRef();
    overlay.setBounds(juce::Rectangle<float>(r.getX() * s, r.getY() * s, r.getWidth() * s, r.getHeight() * s)
                          .getSmallestIntegerContainer());
    const auto rk = rackRef();
    rackDrop.setBounds(juce::Rectangle<float>(rk.getX() * s, rk.getY() * s, rk.getWidth() * s, rk.getHeight() * s)
                           .getSmallestIntegerContainer());
    if (rackDrop.bringToFront()) hatchLog("drop target was buried: moved back above the page");
    // The tile lives in AUDITION's print row; LIVE shows meters there instead.
    overlay.setVisible(overlayEnabled && ! processor.isFxMode());

    // A host can re-add the web view above the overlay (09-30: clicks never reached
    // HATCH in Live). Keep it on top, and log what a click at its centre would hit.
    if (overlay.isVisible())
    {
        if (overlay.bringToFront()) hatchLog("overlay was buried: moved back on top");
        const auto hit = overlay.hitAtCentre();
        if (hit != lastHit)
        {
            lastHit = hit;
            const auto b = overlay.getBounds();
            hatchLog("hit test at tile centre: " + hit + "  (overlay " + b.toString() + " in "
                     + juce::String(getWidth()) + "x" + juce::String(getHeight()) + ")");
        }
    }
}

void LoopNestWebEditor::emit(const char* id, const juce::var& v)
{
    if (browser != nullptr)
        browser->emitEventIfBrowserIsVisible(id, v);
}

void LoopNestWebEditor::sendAll()
{
    if (auto files = bridge.takeFilesIfChanged()) lastFiles = *files;
    if (! lastFiles.isVoid()) emit("ln_files", lastFiles);
    if (auto f = bridge.takeSampleIfChanged()) scanPeaks(*f);
    else if (! lastPeaks.isVoid()) emit("ln_peaks", lastPeaks);
    emit("ln_state", bridge.state());
}

void LoopNestWebEditor::scanPeaks(const juce::File& f)
{
    // The request and its generation publish together, under the lock the worker takes
    // them under: a scan still queued can't pick up this generation, and an empty sample
    // clears the queue (else a queued older file would land as current after a clear).
    const bool valid = f.existsAsFile();
    {
        const juce::ScopedLock sl(peakLock);
        ++peakGen;
        peakRequest = valid ? f : juce::File();
    }
    if (! valid)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("path", juce::String());
        o->setProperty("name", juce::String());
        o->setProperty("seconds", 0.0);
        o->setProperty("peaks", juce::Array<juce::var>());
        lastPeaks = juce::var(o);
        emit("ln_peaks", lastPeaks);
        return;
    }
    peakWake.signal();
}

void LoopNestWebEditor::timerCallback()
{
    updateOverlay();
    if (scaleSaveTicks > 0 && --scaleSaveTicks == 0) processor.rememberEditorScale(scaleToSave);
    if (! pageReady) return;
    if (auto files = bridge.takeFilesIfChanged()) { lastFiles = *files; emit("ln_files", lastFiles); }
    if (auto f = bridge.takeSampleIfChanged()) scanPeaks(*f);
    emit("ln_state", bridge.state());
}

void LoopNestWebEditor::chooseFolder()
{
    if (processor.isExtracting()) return;
    auto start = processor.hasFolder() ? processor.getSampleFolder()
                                       : juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    chooser = std::make_unique<juce::FileChooser>("Choose a folder of loops", start);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                         [this](const juce::FileChooser& fc)
                         {
                             const auto result = fc.getResult();
                             if (result.isDirectory()) processor.setSampleFolder(result);
                         });
}

void LoopNestWebEditor::startExtraction()
{
    // Stage 1's funnel, unchanged: pick a source, then a destination outside it.
    // Additive, never destructive (dedup by filename); the processor guards dest.
    if (processor.isExtracting()) return;
    auto srcStart = processor.hasFolder() ? processor.getSampleFolder()
                                          : juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    chooser = std::make_unique<juce::FileChooser>("Choose a folder to extract drum loops from", srcStart);
    // The second chooser is its own member: replacing `chooser` from inside its
    // own callback would destroy it mid-call.
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                         [this](const juce::FileChooser& fc)
    {
        const auto src = fc.getResult();
        if (! src.isDirectory()) return;
        destChooser = std::make_unique<juce::FileChooser>("Choose or name the destination folder for the curated loops", src);
        destChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [this, src](const juce::FileChooser& fc2)
        {
            const auto dest = fc2.getResult();
            if (dest == juce::File()) return;
            if (dest == src || src.isAChildOf(dest))
            {
                juce::NativeMessageBox::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Can't extract there",
                    "The destination can't be the source folder (or a parent of it). Pick a different destination.", this);
                return;
            }
            processor.extractDrumLoops(src, dest);
        });
    });
}

std::optional<juce::WebBrowserComponent::Resource> LoopNestWebEditor::getResource(const juce::String& url)
{
    // "/" -> index.html; "/juce/index.js", "/fonts/X.ttf" -> by original filename.
    auto path = url.upToFirstOccurrenceOf("?", false, false);
    if (path.isEmpty() || path == "/") path = "/index.html";
    const auto file = path.fromLastOccurrenceOf("/", false, false);

    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        if (file != BinaryData::getNamedResourceOriginalFilename(BinaryData::namedResourceList[i]))
            continue;
        int size = 0;
        if (const auto* data = BinaryData::getNamedResource(BinaryData::namedResourceList[i], size))
        {
            juce::WebBrowserComponent::Resource r;
            r.data.resize((size_t) size);
            std::memcpy(r.data.data(), data, (size_t) size);
            r.mimeType = mimeFor(file);
            return r;
        }
    }
    return std::nullopt;
}
