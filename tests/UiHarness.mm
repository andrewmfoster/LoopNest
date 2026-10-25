// LoopNest UI harness (v2 stage 2): the real web editor in a plain window, no host.
// A render thread calls processBlock on silence at 44.1 kHz / 512 so the audition,
// playhead and meters run; a fake playhead reports --bpm (default 90) as the host
// tempo. Never installed; its settings/Prints dirs follow its own JucePlugin_Name.
//
//   --folder <dir>       sample folder to adopt (the harness's own settings remember it)
//   --spin               spin once the folder is scanned
//   --play               start the audition after the spin lands
//   --hatch              print once a sample is loaded (arms the drag)
//   --live               switch to LIVE after startup
//   --pos <x>,<y>        window top-left, screen points (for scripted drags)
//   --on-top             float the window above other apps (scripted drags hit it, not them)
//   --bpm <n>            host tempo the fake playhead reports
//   --snapshot <png>     WKWebView snapshot after --after ms (default 2500). UNPROVEN: on
//                        09-30 the completion never ran (tried activation, LaunchServices
//                        launch, afterScreenUpdates=NO); <png>.txt says how far it got.
//                        Check real-WebKit layout with --js instead.
//   --js <expr>          evaluate in the page after --after ms; result to --js-out
//   --js2 <expr>         a second evaluation at --after2 ms (default --after + 1500), result
//                        to --js2-out: for checks that need the native side to answer first
//   --status <file>      rewrite a JSON status file every 250 ms (drags, hatch, drag-in, state)
//   --quit-after <ms>    exit after this long
// Cocoa/WebKit first: JuceHeader's `using namespace juce` makes MacTypes' Point ambiguous.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/WebEditor.h"

namespace
{
    struct FnThread : juce::Thread
    {
        FnThread(const juce::String& n, std::function<void()> f) : juce::Thread(n), fn(std::move(f)) {}
        void run() override { fn(); }
        std::function<void()> fn;
    };

    struct FakePlayHead : juce::AudioPlayHead
    {
        double bpm = 90.0;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo p;
            p.setBpm(bpm);
            p.setIsPlaying(false);
            return p;
        }
    };

    WKWebView* findWebView(NSView* v)
    {
        if ([v isKindOfClass: [WKWebView class]]) return (WKWebView*) v;
        for (NSView* s in [v subviews])
            if (auto* w = findWebView(s)) return w;
        return nil;
    }

    juce::String argAfter(const juce::StringArray& a, const char* key, const juce::String& def = {})
    {
        const int i = a.indexOf(key);
        return i >= 0 && i + 1 < a.size() ? a[i + 1] : def;
    }
}

class HarnessApp : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override    { return "LoopNest UI Harness"; }
    const juce::String getApplicationVersion() override { return "0"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise(const juce::String& cmd) override
    {
        args = juce::StringArray::fromTokens(cmd, true);
        for (auto& s : args) s = s.unquoted();

        processor = std::make_unique<LoopNestProcessor>();
        playHead.bpm = argAfter(args, "--bpm", "90").getDoubleValue();
        processor->setPlayHead(&playHead);
        processor->setRateAndBufferSizeDetails(44100.0, 512);
        processor->prepareToPlay(44100.0, 512);
        audio = std::make_unique<FnThread>("harness audio", [this]
        {
            juce::AudioBuffer<float> buf(2, 512);
            juce::MidiBuffer midi;
            auto next = juce::Time::getMillisecondCounterHiRes();
            while (! juce::Thread::currentThreadShouldExit())
            {
                buf.clear();
                processor->processBlock(buf, midi);
                next += 512.0 * 1000.0 / 44100.0;
                const auto wait = next - juce::Time::getMillisecondCounterHiRes();
                if (wait > 0) juce::Thread::sleep((int) wait);
                else next = juce::Time::getMillisecondCounterHiRes();
            }
        });
        audio->startRealtimeThread(juce::Thread::RealtimeOptions{});

        if (auto folder = argAfter(args, "--folder"); folder.isNotEmpty())
            processor->setSampleFolder(juce::File(folder));

        window = std::make_unique<juce::DocumentWindow>("LoopNest UI Harness", juce::Colours::black,
                                                        juce::DocumentWindow::closeButton);
        window->setUsingNativeTitleBar(true);
        editor = dynamic_cast<LoopNestWebEditor*>(processor->createEditor());
        // Note an accepted drag's hover for --status, then pass it on to the page.
        editor->getRackDrop().onHover = [this, pass = editor->getRackDrop().onHover](bool h)
        {
            rackHoverSeen = rackHoverSeen || h;
            if (pass) pass(h);
        };
        window->setContentNonOwned(editor, true);
        if (auto pos = argAfter(args, "--pos"); pos.isNotEmpty())
            window->setTopLeftPosition(pos.upToFirstOccurrenceOf(",", false, false).getIntValue(),
                                       pos.fromFirstOccurrenceOf(",", false, false).getIntValue());
        else
            window->centreWithSize(window->getWidth(), window->getHeight());
        window->setAlwaysOnTop(args.contains("--on-top"));
        window->setVisible(true);
        window->toFront(true);

        t0 = juce::Time::getMillisecondCounter();
        startTimer(50);
    }

    void shutdown() override
    {
        stopTimer();
        if (window) window->clearContentComponent();
        delete editor;
        editor = nullptr;
        window.reset();
        audio->stopThread(2000);
        processor->releaseResources();
        processor.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    void timerCallback() override
    {
        const auto ms = juce::Time::getMillisecondCounter() - t0;
        auto& bridge = editor->getBridge();

        if (! spun && args.contains("--spin") && processor->canSpin())
        {
            spun = true;
            bridge.spin();
        }
        if (spun && ! played && args.contains("--play") && processor->hasSample() && ! processor->isLoadPending())
        {
            played = true;
            if (! processor->isPlaying()) bridge.togglePlay();
        }
        if (! hatched && args.contains("--hatch") && processor->hasSample() && ! processor->isLoadPending() && ms > 800)
        {
            hatched = true;
            bridge.hatch();
        }
        if (! wentLive && args.contains("--live") && ms > 1000)
        {
            wentLive = true;
            bridge.setLive(true);
        }

        const auto after = (juce::uint32) argAfter(args, "--after", "2500").getIntValue();
        if (! snapped && ms > after)
        {
            snapped = true;
            if (auto png = argAfter(args, "--snapshot"); png.isNotEmpty()) snapshot(juce::File(png));
            if (auto js = argAfter(args, "--js"); js.isNotEmpty())
            {
                const auto out = juce::File(argAfter(args, "--js-out", "/dev/stdout"));
                editor->getBrowser().evaluateJavascript(js, [out](juce::WebBrowserComponent::EvaluationResult r)
                {
                    if (auto* v = r.getResult()) out.replaceWithText(juce::JSON::toString(*v, true));
                    else out.replaceWithText("ERROR: " + r.getError()->message);
                });
            }
        }

        const auto after2 = (juce::uint32) argAfter(args, "--after2", juce::String((int) after + 1500)).getIntValue();
        if (! snapped2 && ms > after2)
        {
            snapped2 = true;
            if (auto js = argAfter(args, "--js2"); js.isNotEmpty())
            {
                const auto out = juce::File(argAfter(args, "--js2-out", "/dev/stdout"));
                editor->getBrowser().evaluateJavascript(js, [out](juce::WebBrowserComponent::EvaluationResult r)
                {
                    if (auto* v = r.getResult()) out.replaceWithText(juce::JSON::toString(*v, true));
                    else out.replaceWithText("ERROR: " + r.getError()->message);
                });
            }
        }

        if (auto status = argAfter(args, "--status"); status.isNotEmpty() && ms - lastStatus > 250)
        {
            lastStatus = ms;
            auto* o = new juce::DynamicObject();
            o->setProperty("ms", (int) ms);
            o->setProperty("dragsBegun", editor->getOverlay().dragsBegun);
            o->setProperty("dragsEnded", editor->getOverlay().dragsEnded);
            o->setProperty("dragFile", bridge.dragFile().getFullPathName());
            o->setProperty("lastPrint", bridge.lastPrint().getFullPathName());
            o->setProperty("live", processor->isFxMode());
            o->setProperty("playing", processor->isPlaying());
            o->setProperty("sample", processor->getCurrentSampleName());
            const auto sb = editor->getOverlay().getScreenBounds();
            o->setProperty("overlay", juce::String(sb.getX()) + "," + juce::String(sb.getY()) + ","
                                    + juce::String(sb.getWidth()) + "," + juce::String(sb.getHeight()));
            o->setProperty("overlayVisible", editor->getOverlay().isVisible());
            // Drag-in: the rack's drop target (scripted drags aim at it; clicks must reach the page).
            auto& rd = editor->getRackDrop();
            const auto rb = rd.getScreenBounds();
            o->setProperty("rackDrop", juce::String(rb.getX()) + "," + juce::String(rb.getY()) + ","
                                     + juce::String(rb.getWidth()) + "," + juce::String(rb.getHeight()));
            o->setProperty("rackDragsEntered", rd.dragsEntered);
            o->setProperty("rackDrops", rd.drops);
            o->setProperty("rackHoverSeen", rackHoverSeen);
            o->setProperty("rackHit", rd.hitAtCentre());
            o->setProperty("sampleCount", processor->getSampleCount());
            o->setProperty("index", processor->getSampleFiles().indexOf(processor->getCurrentSampleFile()));
            juce::File(status).replaceWithText(juce::JSON::toString(juce::var(o), true));
        }

        if (auto q = argAfter(args, "--quit-after"); q.isNotEmpty() && ms > (juce::uint32) q.getIntValue())
            quit();
    }

    void snapshot(const juce::File& png)
    {
        auto* peer = editor->getPeer();
        WKWebView* wv = peer != nullptr ? findWebView((NSView*) peer->getNativeHandle()) : nil;
        if (wv == nil) { png.withFileExtension("txt").replaceWithText("no WKWebView"); return; }
        png.withFileExtension("txt").replaceWithText("snapshot requested " + juce::String((int) wv.frame.size.width) + "x" + juce::String((int) wv.frame.size.height));
        WKSnapshotConfiguration* cfg = [[[WKSnapshotConfiguration alloc] init] autorelease];
        cfg.afterScreenUpdates = NO; // YES waits for a screen update that never comes headless
        [wv takeSnapshotWithConfiguration: cfg completionHandler: ^(NSImage* img, NSError* err)
        {
            const auto txt = png.withFileExtension("txt");
            if (img == nil) { txt.replaceWithText(err ? juce::String([[err description] UTF8String]) : "nil"); return; }
            CGImageRef cg = [img CGImageForProposedRect: nil context: nil hints: nil];
            if (cg == nullptr) { txt.replaceWithText("no CGImage"); return; }
            NSBitmapImageRep* rep = [[[NSBitmapImageRep alloc] initWithCGImage: cg] autorelease];
            NSData* data = [rep representationUsingType: NSBitmapImageFileTypePNG properties: @{}];
            if (data == nil || ! [data writeToFile: [NSString stringWithUTF8String: png.getFullPathName().toRawUTF8()] atomically: YES])
                txt.replaceWithText("png encode/write failed");
        }];
    }

    juce::StringArray args;
    FakePlayHead playHead;
    std::unique_ptr<LoopNestProcessor> processor;
    std::unique_ptr<juce::Thread> audio;
    std::unique_ptr<juce::DocumentWindow> window;
    LoopNestWebEditor* editor = nullptr;
    juce::uint32 t0 = 0, lastStatus = 0;
    bool spun = false, played = false, hatched = false, wentLive = false, snapped = false, snapped2 = false;
    bool rackHoverSeen = false;
};

START_JUCE_APPLICATION(HarnessApp)
