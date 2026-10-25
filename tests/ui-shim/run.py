#!/Library/Frameworks/Python.framework/Versions/3.10/bin/python3
"""LoopNest v2 stage 2: shim check for ui/index.html in headless Chromium.

Serves the page the way Source/WebEditor.cpp's resource provider does (by original
filename: "/" -> ui/index.html, "/juce/index.js", "fonts/X.ttf"), injects shim.js as
the JUCE backend, then screenshots rest (AUDITION) and LIVE at 1672x941 and 1003x565,
drives the controls, and writes results.json plus side-by-side comparisons against the
mock's reference shots.

  run.py [--out DIR] [--serve]      (--serve: just serve on :8765 for a manual look,
                                     open http://127.0.0.1:8765/ with shim.js injected)
Needs python.org 3.10 (playwright + PIL live there).
"""
import argparse, http.server, json, pathlib, socketserver, sys, threading

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
JS = ROOT / "JUCE/modules/juce_gui_extra/native/javascript"
FILES = {
    "index.html": ROOT / "ui/index.html",
    "index.js": JS / "index.js",
    "check_native_interop.js": JS / "check_native_interop.js",
    "JetBrainsMono-Regular.ttf": ROOT / "fonts/JetBrainsMono-Regular.ttf",
    "JetBrainsMono-Bold.ttf": ROOT / "fonts/JetBrainsMono-Bold.ttf",
    "JetBrainsMono-ExtraBold.ttf": ROOT / "fonts/JetBrainsMono-ExtraBold.ttf",
}
MIME = {"html": "text/html", "js": "text/javascript", "ttf": "font/ttf"}
REF = ROOT.parent / "LoopNest/mockups/stage2-styled-2026-09-29"
REQUESTS = []


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split("?")[0]
        name = "index.html" if path in ("", "/") else path.rsplit("/", 1)[-1]
        REQUESTS.append(self.path)
        f = FILES.get(name)
        if f is None or not f.exists():
            self.send_response(404); self.end_headers(); return
        data = f.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", MIME.get(name.rsplit(".", 1)[-1], "application/octet-stream"))
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):
        pass


def serve(port):
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", port), Handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(HERE / "out"))
    ap.add_argument("--serve", action="store_true")
    a = ap.parse_args()
    if a.serve:
        serve(8765); print("serving on http://127.0.0.1:8765/ (inject shim.js yourself)")
        threading.Event().wait()
    out = pathlib.Path(a.out); out.mkdir(parents=True, exist_ok=True)
    srv = serve(0); port = srv.server_address[1]
    base = f"http://127.0.0.1:{port}/"
    from playwright.sync_api import sync_playwright
    res = {"shots": {}, "interactions": {}}
    errors = []

    def open_page(b, w, h, query):
        ctx = b.new_context(viewport={"width": w, "height": h}, device_scale_factor=1)
        pg = ctx.new_page()
        pg.on("console", lambda m: errors.append(f"console.{m.type}: {m.text}") if m.type in ("error", "warning") else None)
        pg.on("pageerror", lambda e: errors.append(f"pageerror: {e}"))
        pg.on("requestfailed", lambda r: errors.append(f"requestfailed: {r.url}"))
        pg.add_init_script(path=str(HERE / "shim.js"))
        pg.goto(base + query)
        pg.wait_for_function("window.__ln && __ln.state() && document.fonts.status === 'loaded' && __ln.check().peaks.n > 0 || (window.__ln && __ln.state() && __ln.state().hasSample === false)")
        pg.wait_for_timeout(1200)
        return ctx, pg

    with sync_playwright() as p:
        # python.org 3.10's playwright pins an older browser build than the one installed
        # (~/Library/Caches/ms-playwright); use whichever headless shell is there.
        shells = sorted(pathlib.Path.home().glob("Library/Caches/ms-playwright/chromium_headless_shell-*/chrome-headless-shell-mac-arm64/chrome-headless-shell"))
        b = p.chromium.launch(executable_path=str(shells[-1])) if shells else p.chromium.launch()
        for (w, h) in [(1672, 941), (1003, 565)]:
            for mode, query in [("rest", "?freeze=1"), ("live", "?freeze=1&live=1")]:
                REQUESTS.clear()
                ctx, pg = open_page(b, w, h, query)
                if mode == "live": pg.wait_for_timeout(8500)   # the input scope fills from the right, ~8 s a screen at 90 BPM
                shot = out / f"shim-{mode}-{w}x{h}.png"
                pg.screenshot(path=str(shot))
                res["shots"][f"{mode}-{w}x{h}"] = {"png": str(shot), "check": pg.evaluate("__ln.check()"),
                                                   "requests": sorted(set(REQUESTS))}
                ctx.close()
        # edge fixtures: a long real-world file name at plugin size; no folder at all
        for mode, (w, h), query in [("long", (1003, 565), "?freeze=1&long=1"), ("empty", (1672, 941), "?empty=1")]:
            ctx, pg = open_page(b, w, h, query)
            shot = out / f"shim-{mode}-{w}x{h}.png"
            pg.screenshot(path=str(shot))
            res["shots"][f"{mode}-{w}x{h}"] = {"png": str(shot), "check": pg.evaluate("__ln.check()")}
            ctx.close()
        # drag-in: the rack's teal light while the native drop target reports an accepted drag (ln_drop);
        # rest and after must match each other (nothing drawn at rest)
        for mode, (w, h), query in [("drop", (1672, 941), "?freeze=1"), ("drop-live", (1672, 941), "?freeze=1&live=1"),
                                    ("drop", (1003, 565), "?freeze=1")]:
            ctx, pg = open_page(b, w, h, query)
            s = w / 1672; clip = {"x": 0, "y": 40 * s, "width": 680 * s, "height": 500 * s}
            on = lambda: pg.evaluate("document.getElementById('n-drop').classList.contains('on')")
            tag = f"{mode}-{w}x{h}"
            pg.screenshot(path=str(out / f"shim-{tag}-rest.png"), clip=clip)
            pg.evaluate("__shim.rackDrop(true)"); pg.wait_for_timeout(150); lit = on()
            pg.screenshot(path=str(out / f"shim-{tag}-hover.png"), clip=clip)
            pg.evaluate("__shim.rackDrop(false)"); pg.wait_for_timeout(150)
            pg.screenshot(path=str(out / f"shim-{tag}-after.png"), clip=clip)
            res["shots"][f"{tag}-hover"] = {"lit": lit, "lit_after": on(), "png": str(out / f"shim-{tag}-hover.png")}
            ctx.close()
        # print row close-up at 2x like shot-final-print-row-2x (FX off over FX on)
        ctx = b.new_context(viewport={"width": 1672, "height": 941}, device_scale_factor=2)
        pg = ctx.new_page(); pg.add_init_script(path=str(HERE / "shim.js"))
        pg.on("pageerror", lambda e: errors.append(f"pageerror: {e}"))
        pg.goto(base + "?freeze=1"); pg.wait_for_function("window.__ln && __ln.check().peaks.n > 0"); pg.wait_for_timeout(900)
        clip = {"x": 1200, "y": 448, "width": 462, "height": 78}
        pg.screenshot(path=str(out / "shim-print-row-off-2x.png"), clip=clip)
        pg.click("#fx"); pg.wait_for_timeout(400)
        pg.screenshot(path=str(out / "shim-print-row-on-2x.png"), clip=clip)
        pg.screenshot(path=str(out / "shim-loop-2x.png"), clip={"x": 1486, "y": 404, "width": 176, "height": 46})
        # spin history ◀ ▶ either side of the SPIN dial at 2x: back lit / forward dim, then back hovered
        hclip = {"x": 330, "y": 396, "width": 320, "height": 120}
        pg.screenshot(path=str(out / "shim-history-2x.png"), clip=hclip)
        pg.hover("#h-back"); pg.wait_for_timeout(250)
        pg.screenshot(path=str(out / "shim-history-hover-2x.png"), clip=hclip)
        ctx.close()

        # ---- interactions (1672x941, playhead running)
        ctx, pg = open_page(b, 1672, 941, "")
        I = res["interactions"]
        I["snap_default"] = {"on": pg.evaluate("__ln.snap()"), "pill": pg.inner_text("#snap-t"), "cls": pg.get_attribute("#snap-t", "class"),
                             "hits": pg.evaluate("__ln.check().peaks.hits")}
        pg.evaluate("__ln.setSnap(false)")   # the drags below measure free motion; SNAP has its own block at the end
        inv = lambda: pg.evaluate("__shim.log.invoke.map(x => x.join(' '))")
        def last_inv(): v = inv(); return v[-1] if v else None
        pg.click("#sync"); pg.wait_for_timeout(150)
        I["sync"] = {"invoke": last_inv(), "tempo": pg.inner_text("#tempo-line"), "cls": pg.get_attribute("#sync", "class")}
        pg.click("#sync"); pg.wait_for_timeout(150)
        I["sync_back"] = {"invoke": last_inv(), "tempo": pg.inner_text("#tempo-line")}
        pg.click("#loop-t"); pg.wait_for_timeout(450)
        I["loop"] = {"invoke": last_inv(), "cls": pg.get_attribute("#loop-t", "class")}
        pg.click("#fx"); pg.wait_for_timeout(450)
        I["fx"] = {"invoke": last_inv(), "cls": pg.get_attribute("#fx", "class")}
        pg.click("#pl-play"); pg.wait_for_timeout(450)
        I["play"] = {"invoke": last_inv(), "playing": pg.evaluate("__ln.state().playing"), "cls": pg.get_attribute("#pl-play", "class")}
        pg.click("#pl-play"); pg.wait_for_timeout(300)
        # spin history ◀ ▶ beside the SPIN dial: the fixture is one spin in (back lit, forward dim)
        hist = lambda: pg.evaluate("__ln.check().hist")
        h0 = hist()
        pg.click("#h-back"); pg.wait_for_timeout(250)
        h1 = hist(); idx1 = pg.evaluate("__ln.state().index")
        pg.click("#h-back", force=True); pg.wait_for_timeout(250)                   # dim at the end: nothing sent
        n_before = len(inv())
        pg.click("#h-fwd"); pg.wait_for_timeout(250)
        I["history"] = {"rest": h0, "after_back": h1, "index_after_back": idx1, "invokes": [x for x in inv() if x in ("ln_back", "ln_forward")],
                        "after_fwd": hist(), "index_after_fwd": pg.evaluate("__ln.state().index"), "sent_after_end_click": inv()[n_before - 1:]}
        # knob drag: WARP's main knob
        kb = pg.evaluate("(() => { const r = document.querySelectorAll('#rack .col')[0].querySelectorAll('.knob')[1].getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()")
        pg.mouse.move(*kb); pg.mouse.down(); pg.mouse.move(kb[0], kb[1] - 30, steps=5); pg.mouse.move(kb[0], kb[1] - 60, steps=5); pg.mouse.up()
        pg.wait_for_timeout(120)
        I["knob_drag_warp"] = {"gestures": pg.evaluate("__shim.log.gestures.filter(g => g[0] === 'warp').map(g => g.join(' '))"),
                               "native": pg.evaluate("__shim.sv.warp"), "page": pg.evaluate("__ln.getParam('warp')"),
                               "text": pg.inner_text("#rack .col:nth-child(1) .namel")}
        pg.evaluate("__shim.automate('drive', 55)"); pg.wait_for_timeout(120)
        I["host_automation_drive_55"] = {"text": pg.inner_text("#rack .col:nth-child(3) .namel"), "page": pg.evaluate("__ln.getParam('drive')")}
        pg.click("#p-next"); pg.click("#p-next"); pg.wait_for_timeout(200)
        I["preset_next_x2"] = {"invokes": inv()[-2:], "name": pg.inner_text("#pname"), "warp_text": pg.inner_text("#rack .col:nth-child(1) .namel")}
        pg.click("#pname"); pg.wait_for_timeout(100)
        items = pg.evaluate("[...document.querySelectorAll('#pop-list li')].map(l => l.textContent)")
        pg.locator("#pop-list li", has_text="Hall").dispatch_event("mousedown"); pg.wait_for_timeout(200)
        I["preset_popup"] = {"items": items, "invoke": last_inv(), "name": pg.inner_text("#pname"), "open": pg.evaluate("document.getElementById('pop').className")}
        pg.click("#bank"); pg.wait_for_timeout(100)
        I["bank_popup"] = {"items": pg.evaluate("[...document.querySelectorAll('#pop-list li')].map(l => l.textContent)"),
                           "rect": pg.evaluate("(() => { const p = document.getElementById('pop'); return [p.offsetLeft, p.offsetTop, p.offsetWidth, p.offsetHeight]; })()")}
        pg.locator("#pop-list li", has_text="Extract").dispatch_event("mousedown"); pg.wait_for_timeout(1200)
        I["extract"] = {"invoke": last_inv(), "count_text": pg.inner_text("#n-count")}
        pg.click("#bank"); pg.wait_for_timeout(200)
        pg.screenshot(path=str(out / "shim-bank-extracting.png"), clip={"x": 24, "y": 60, "width": 420, "height": 200})
        I["extract"]["popup"] = pg.evaluate("[...document.querySelectorAll('#pop-list li')].map(l => l.textContent)")
        pg.keyboard.press("Escape")
        pg.click("#bin"); pg.wait_for_timeout(100)
        I["bin_popup"] = {"items": pg.evaluate("[...document.querySelectorAll('#pop-list li')].map(l => l.textContent)"),
                          "rect": pg.evaluate("(() => { const p = document.getElementById('pop'); return [p.offsetLeft, p.offsetTop, p.offsetWidth, p.offsetHeight]; })()"),
                          "overlay_calls": pg.evaluate("__shim.log.overlay")}
        pg.keyboard.press("Escape")
        # seek + trim
        pg.mouse.click(680 + 22 + 924 * 0.5, 76 + 76 + 60); pg.wait_for_timeout(100)
        I["seek_half"] = {"invoke": last_inv()}
        hx, hy = 680 + 22 + 924 * pg.evaluate("__ln.getParam('start')"), 76 + 76 + 120
        pg.mouse.move(hx, hy); pg.mouse.down(); pg.mouse.move(hx + 50, hy, steps=4); pg.mouse.move(hx + 100, hy, steps=4); pg.mouse.up(); pg.wait_for_timeout(120)
        I["trim_start_drag_100px"] = {"native_start": pg.evaluate("__shim.sv.start"), "expect": 100 / 924,
                                      "gestures": pg.evaluate("__shim.log.gestures.filter(g => g[0] === 'start').map(g => g.join(' '))")}
        pg.screenshot(path=str(out / "shim-trimmed.png"), clip={"x": 680, "y": 76, "width": 968, "height": 366})
        # HATCH via the (fake) native overlay
        pg.evaluate("__shim.hatchHover(true)"); pg.wait_for_timeout(100)
        I["hatch_hover"] = {"cls": pg.get_attribute("#hatch", "class")}
        pg.evaluate("__shim.hatchClick()"); pg.wait_for_timeout(300)
        I["hatch_render_0.3s"] = {"cls": pg.get_attribute("#hatch", "class")}
        pg.wait_for_timeout(1600)
        I["hatch_after_1.9s"] = {"cls": pg.get_attribute("#hatch", "class"), "file": pg.inner_text("#hatch-file")}
        pg.screenshot(path=str(out / "shim-hatch-armed-row.png"), clip={"x": 1200, "y": 448, "width": 462, "height": 78})
        pg.evaluate("__shim.hatchHover(false)")
        pg.click("#fx"); pg.wait_for_timeout(450)
        I["hatch_stale_after_fx"] = {"cls": pg.get_attribute("#hatch", "class")}
        # spin
        before = pg.evaluate("__ln.state().sample")
        pg.click("#spin"); pg.wait_for_timeout(120)
        mid = pg.evaluate("__ln.check().label")
        pg.wait_for_timeout(1200)
        I["spin"] = {"invoke": last_inv(), "before": before, "after": pg.evaluate("__ln.state().sample"), "label_mid": mid,
                     "label_end": pg.evaluate("__ln.check().label"), "rack": pg.evaluate("__ln.check().rack")}
        # bypass pill (toggle relay)
        pg.click("#m-byp"); pg.wait_for_timeout(100)
        I["bypass"] = {"native": pg.evaluate("__shim.tv.bypass"), "stage_bypass": pg.evaluate("document.getElementById('stage').classList.contains('bypass')")}
        pg.click("#m-byp")
        # LIVE tab
        pg.click(".tab[data-mode=fx]"); pg.wait_for_timeout(500)
        I["live_tab"] = {"invoke": last_inv(), "mode": pg.evaluate("__ln.check().mode")}
        pg.click(".tab[data-mode=audition]"); pg.wait_for_timeout(400)
        I["audition_tab"] = {"invoke": last_inv(), "mode": pg.evaluate("__ln.check().mode")}
        # ---- batch 1: rack pick, stepper, typed number, search, wire at the edge, zoom, loupe, recents, grip
        pos = lambda: pg.inner_text("#n-pos")
        pg.locator("#n-rack .spine").nth(5).click(); pg.wait_for_timeout(60)
        lit_pending = pos()
        pg.wait_for_timeout(500)
        I["rack_pick_5"] = {"invoke": last_inv(), "pos_pending": lit_pending, "pos": pos(), "index": pg.evaluate("__ln.state().index")}
        pg.click("#n-dn"); pg.wait_for_timeout(500)
        I["step_next"] = {"invoke": last_inv(), "index": pg.evaluate("__ln.state().index")}
        pg.click("#n-up"); pg.wait_for_timeout(500)
        I["step_prev"] = {"invoke": last_inv(), "index": pg.evaluate("__ln.state().index")}
        pg.click("#n-pos"); pg.wait_for_timeout(60)
        has_input = pg.evaluate("!!document.querySelector('#n-pos input')")
        pg.keyboard.press("Meta+A"); pg.keyboard.type("100"); pg.keyboard.press("Enter"); pg.wait_for_timeout(500)
        I["typed_100"] = {"input_shown": has_input, "invoke": last_inv(), "index": pg.evaluate("__ln.state().index"), "pos": pos()}
        pg.click("#n-pos"); pg.keyboard.type("9999"); pg.keyboard.press("Enter"); pg.wait_for_timeout(300)
        I["typed_out_of_range"] = {"invoke": last_inv(), "pos": pos()}
        pg.click("#n-q"); pg.keyboard.type("dusty break"); pg.wait_for_timeout(150)
        vis = pg.evaluate("[...document.querySelectorAll('#n-rack .spine')].filter(b => !b.classList.contains('hide')).map(b => b.title)")
        I["search"] = {"count_text": pg.inner_text("#n-count"), "visible": len(vis), "all_match": all("dusty" in v.lower() and "break" in v.lower() for v in vis),
                       "focused": pg.evaluate("document.activeElement && document.activeElement.id")}
        pg.screenshot(path=str(out / "shim-search.png"), clip={"x": 0, "y": 40, "width": 680, "height": 460})
        picks = []
        n_inv = len(inv())
        for _ in range(4):
            pg.click("#spin"); pg.wait_for_timeout(450); picks.append(pg.evaluate("__ln.state().sample"))
        # SPIN among the matches is a spin (ln_spinPick: the spin history records it), not a rack pick
        I["spin_in_search"] = {"picks": picks, "all_match": all("dusty" in v.lower() and "break" in v.lower() for v in picks),
                               "sent": sorted(set(x.split(" ")[0] for x in inv()[n_inv:]))}
        n_inv = len(inv())
        pg.click("#n-dn"); pg.wait_for_timeout(450)
        I["step_in_search"] = {"sample": pg.evaluate("__ln.state().sample"), "sent": sorted(set(x.split(" ")[0] for x in inv()[n_inv:]))}
        pg.click("#n-x"); pg.wait_for_timeout(150)
        I["search_cleared"] = {"count_text": pg.inner_text("#n-count"), "hidden": pg.evaluate("document.querySelectorAll('#n-rack .spine.hide').length")}
        # wire: load row 2, scroll the rack to the bottom so the spine is off view
        pg.locator("#n-rack .spine").nth(2).click(); pg.wait_for_timeout(500)
        pg.evaluate("document.getElementById('n-rack').scrollTop = 1e6"); pg.wait_for_timeout(300)
        I["wire_scrolled_off"] = {"d": pg.get_attribute("#wire-p", "d"), "opacity": pg.evaluate("getComputedStyle(document.getElementById('wire-p')).opacity")}
        pg.screenshot(path=str(out / "shim-wire-edge.png"), clip={"x": 0, "y": 40, "width": 680, "height": 460})
        pg.evaluate("document.getElementById('n-rack').scrollTop = 0"); pg.wait_for_timeout(200)
        # zoom about the pointer (cmd-scroll and pinch = ctrl-wheel), pan, double-click reset
        wx = lambda n: 680 + 22 + 924 * n
        wy = 76 + 76 + 60
        pg.mouse.move(wx(0.25), wy); pg.keyboard.down("Meta")
        for _ in range(4): pg.mouse.wheel(0, -120); pg.wait_for_timeout(30)
        pg.keyboard.up("Meta"); pg.wait_for_timeout(100)
        v1 = pg.evaluate("__ln.view()")
        pg.keyboard.down("Control"); pg.mouse.wheel(0, -60); pg.keyboard.up("Control"); pg.wait_for_timeout(100)
        v2 = pg.evaluate("__ln.view()")
        pg.mouse.wheel(80, 0); pg.wait_for_timeout(100)
        v3 = pg.evaluate("__ln.view()")
        pg.screenshot(path=str(out / "shim-zoomed.png"), clip={"x": 680, "y": 76, "width": 968, "height": 366})
        I["zoom"] = {"cmd_scroll": v1, "pinch": v2, "pan": v3,
                     "keeps_pointer": abs((0.25 - v1["a"]) / (v1["b"] - v1["a"]) - 0.25) < 0.02}
        pg.mouse.dblclick(wx(0.5), wy); pg.wait_for_timeout(100)
        I["zoom_reset"] = pg.evaluate("__ln.view()")
        # loupe while dragging the start handle in the whole-file view; then shift-fine
        pg.evaluate("__ln.setParam('start', 0.2)"); pg.wait_for_timeout(100)
        hx, hy = wx(0.2), 76 + 76 + 120
        pg.mouse.move(hx, hy); pg.mouse.down(); pg.mouse.move(hx + 20, hy, steps=3); pg.wait_for_timeout(80)
        pg.screenshot(path=str(out / "shim-loupe.png"), clip={"x": 680, "y": 76, "width": 968, "height": 366})
        s_before = pg.evaluate("__shim.sv.start")
        pg.keyboard.down("Shift"); pg.mouse.move(hx + 120, hy, steps=6); pg.keyboard.up("Shift"); pg.mouse.up(); pg.wait_for_timeout(100)
        s_after = pg.evaluate("__shim.sv.start")
        I["shift_fine_100px"] = {"before": s_before, "after": s_after, "delta": s_after - s_before, "expect": 0.1 * 100 / 924}
        # recents in the BANK popup
        pg.click("#bank"); pg.wait_for_timeout(100)
        items = pg.evaluate("[...document.querySelectorAll('#pop-list li')].map(l => l.textContent)")
        pg.screenshot(path=str(out / "shim-bank-recents.png"), clip={"x": 0, "y": 40, "width": 500, "height": 260})
        pg.locator("#pop-list li", has_text="BREAKS").dispatch_event("mousedown"); pg.wait_for_timeout(100)
        I["recents"] = {"items": items, "invoke": last_inv()}
        # resize grip
        g = pg.evaluate("(() => { const r = document.getElementById('grip').getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()")
        pg.mouse.move(*g); pg.mouse.down(); pg.mouse.move(g[0] - 200, g[1] - 100, steps=6); pg.mouse.up(); pg.wait_for_timeout(100)
        I["grip"] = {"resize_calls": pg.evaluate("__shim.log.resize.slice(-3)")}
        # ---- batch 2: SNAP to hits, bar 1, the file's tempo and grid under trims, the tempo popup, print bins
        pg.locator("#n-rack .spine").nth(2).click(); pg.wait_for_timeout(600)      # rimshot_shuffle_88bpm: 16 bars
        foot = lambda: pg.evaluate("__ln.check().footer")
        st = lambda k: pg.evaluate(f"__ln.state().{k}")
        hx0, hy = wx(0), 76 + 76 + 120
        # a tagged file: the tag holds under a trim; the loop reads 12 of its 16 bars
        bars0, bpm0 = pg.evaluate("__ln.bars()"), st("nativeBpm")
        pg.evaluate("__ln.setParam('start', 0.25)"); pg.wait_for_timeout(150)
        I["tag_holds_under_trim"] = {"bars_before": bars0, "bars_after": pg.evaluate("__ln.bars()"), "bpm_before": bpm0,
                                     "bpm_after": st("nativeBpm"), "source": st("bpmSource"), "footer": foot()}
        pg.evaluate("__ln.setParam('start', 0)"); pg.wait_for_timeout(80)
        # SNAP pill: a toggle. On, a 100 px drag of the start handle lands on a hit; alt drags free.
        pg.evaluate("__ln.setSnap(true)")
        pg.click("#snap-t"); pg.wait_for_timeout(60); pill_off = pg.inner_text("#snap-t")
        off_x = pg.evaluate("__ln.snapN(0.1082)")
        pg.click("#snap-t"); pg.wait_for_timeout(60)
        hits = pg.evaluate("__ln.hits()")
        pg.mouse.move(hx0, hy); pg.mouse.down(); pg.mouse.move(hx0 + 50, hy, steps=4); pg.mouse.move(hx0 + 100, hy, steps=4); pg.wait_for_timeout(60)
        snapped = pg.evaluate("__shim.sv.start")
        pg.keyboard.down("Alt"); pg.mouse.move(hx0 + 101, hy); pg.wait_for_timeout(60)
        free = pg.evaluate("__shim.sv.start")
        pg.keyboard.up("Alt"); pg.mouse.up(); pg.wait_for_timeout(100)
        near = min(hits + [0, 1], key=lambda h: abs(h - 100 / 924))
        I["snap_hits_drag_100px"] = {"pill_off": pill_off, "off_is_free": off_x == 0.1082, "pill": pg.inner_text("#snap-t"), "pressed": pg.get_attribute("#snap-t", "aria-pressed"),
                                     "hits": len(hits), "start": snapped, "on_a_hit": any(abs(snapped - h) < 1e-9 for h in hits),
                                     "nearest": abs(snapped - near) < 1e-9, "alt_free": free, "alt_expect": 101 / 924,
                                     "ends": [pg.evaluate("__ln.snapN(0.0001)"), pg.evaluate("__ln.snapN(0.9999)")]}
        pg.screenshot(path=str(out / "shim-snap.png"), clip={"x": 680, "y": 76, "width": 968, "height": 366})
        # ---- bar 1: right-click in the wave. The spot snaps like a handle; a trim never moves it.
        ctx_items = lambda: pg.evaluate("[...document.querySelectorAll('#pop-list li')].map(l => l.textContent)")
        origin = lambda: st("barOrigin")
        pg.mouse.click(wx(0.3), wy, button="right"); pg.wait_for_timeout(100)
        items = ctx_items()
        search_shown = pg.evaluate("getComputedStyle(document.getElementById('pop-q')).display")
        pg.locator("#pop-list li", has_text="Bar 1 starts here").dispatch_event("mousedown"); pg.wait_for_timeout(150)
        o1 = origin(); near = min(hits + [0, 1], key=lambda h: abs(h - 0.3))
        I["bar1_set_snapped"] = {"items": items, "search": search_shown, "invoke": last_inv(), "origin": o1, "on_a_hit": any(abs(o1 - h) < 1e-9 for h in hits),
                                 "nearest": abs(o1 - near) < 1e-9, "open": pg.evaluate("document.getElementById('pop').className")}
        pg.evaluate("__ln.setParam('start', 0.5)"); pg.wait_for_timeout(150)
        I["bar1_holds_under_trim"] = {"origin": origin(), "held": origin() == o1, "bars": pg.evaluate("__ln.bars()"), "footer": foot()}
        pg.screenshot(path=str(out / "shim-bar1.png"), clip={"x": 680, "y": 76, "width": 968, "height": 366})
        pg.evaluate("__ln.setParam('start', 0)"); pg.wait_for_timeout(80)
        pg.mouse.click(wx(0.6), wy, button="right"); pg.wait_for_timeout(100)
        items = ctx_items()
        pg.keyboard.press("Escape"); pg.mouse.click(wx(0.6), 76 + 20); pg.wait_for_timeout(100)      # dismissed: nothing sent
        dismissed = {"open": pg.evaluate("document.getElementById('pop').className"), "origin": origin()}
        pg.mouse.click(wx(0.6), wy, button="right"); pg.wait_for_timeout(100)
        pg.locator("#pop-list li", has_text="Reset bar 1").dispatch_event("mousedown"); pg.wait_for_timeout(150)
        I["bar1_reset"] = {"items": items, "dismissed": dismissed, "invoke": last_inv(), "origin": origin()}
        pg.evaluate("__ln.setSnap(false)")
        pg.mouse.click(wx(0.3), wy, button="right"); pg.wait_for_timeout(100)
        pg.locator("#pop-list li", has_text="Bar 1 starts here").dispatch_event("mousedown"); pg.wait_for_timeout(150)
        I["bar1_free_snap_off"] = {"invoke": last_inv(), "origin": origin(), "expect": 0.3, "free": abs(origin() - 0.3) < 1e-3}
        pg.evaluate("__ln.setSnap(true)")
        pg.click(".tab[data-mode=fx]"); pg.wait_for_timeout(400)
        pg.mouse.click(wx(0.5), wy, button="right"); pg.wait_for_timeout(100)
        I["bar1_no_menu_in_live"] = {"open": pg.evaluate("document.getElementById('pop').className"), "origin": origin()}
        pg.click(".tab[data-mode=audition]"); pg.wait_for_timeout(400)
        # ---- the tempo and the grid are the file's. From the handles is the one read off a trim:
        # 8 of the 16 bars -> 88 BPM guessed, and bar 1 moves to the start handle.
        pg.evaluate("__ln.setSnap(false)")
        pg.evaluate("__ln.setParam('start', 0.25)"); pg.evaluate("__ln.setParam('end', 0.75)"); pg.wait_for_timeout(120)
        pg.click("#bpm-t"); pg.wait_for_timeout(350)
        items = pg.evaluate("[...document.querySelectorAll('#pop-list li')].map(l => l.textContent)")
        pg.locator("#pop-list li", has_text="From the handles").dispatch_event("mousedown"); pg.wait_for_timeout(200)
        I["bpm_fit"] = {"items": items, "invoke": last_inv(), "bpm": st("nativeBpm"), "source": st("bpmSource"), "bars": pg.evaluate("__ln.bars()"),
                        "origin": origin(), "origin_at_start_handle": origin() == 0.25, "footer": foot()}
        pg.screenshot(path=str(out / "shim-loop-grid.png"), clip={"x": 680, "y": 76, "width": 968, "height": 366})
        # a trim holds the tempo: drag the end handle in with the mouse, to half the loop. The tempo,
        # its source and bar 1 hold through the drag and after release; only the bar count follows.
        ex = wx(0.75)
        pg.mouse.move(ex, hy); pg.mouse.down()
        seen = []
        for dx in (-60, -120, -180, -231):          # to half the file: 4 bars at 88
            pg.mouse.move(ex + dx, hy, steps=3); pg.wait_for_timeout(120)
            seen.append({"bpm": st("nativeBpm"), "bars": round(pg.evaluate("__ln.bars()"), 3), "footer": foot()})
        pg.mouse.up(); pg.wait_for_timeout(400)
        I["trim_holds_tempo"] = {"during": seen, "held_through_drag": all(abs(d["bpm"] - 88) < 1e-6 for d in seen),
                                 "end": pg.evaluate("__shim.sv.end"), "bpm_after": st("nativeBpm"), "bars_after": pg.evaluate("__ln.bars()"),
                                 "source": st("bpmSource"), "origin": origin(), "footer": foot(),
                                 "held": st("nativeBpm") == 88 and st("bpmSource") == "length" and origin() == 0.25}
        # a region no reading would fit (a 64th of the file, 0.68 s): still the file's tempo
        pg.evaluate("__ln.setParam('end', 0.25 + 1 / 64)"); pg.wait_for_timeout(400)
        I["tiny_trim_holds"] = {"bpm": st("nativeBpm"), "source": st("bpmSource"), "bars": pg.evaluate("__ln.bars()"), "footer": foot()}
        # host automation of the trim (no gesture): nothing settles later either
        pg.evaluate("__shim.automate('start', 0)"); pg.evaluate("__shim.automate('end', 0.5)"); pg.wait_for_timeout(80)
        mid = st("nativeBpm")
        pg.wait_for_timeout(400)
        I["host_edit_holds"] = {"bpm_at_80ms": mid, "bpm": st("nativeBpm"), "source": st("bpmSource"), "origin": origin(),
                                "bars": pg.evaluate("__ln.bars()"), "footer": foot()}
        # the one exception: a file with NO tempo yet takes one from its trim, on release (a gesture)
        # or 250 ms after a gesture-less edit goes quiet
        none = "(() => { __shim.S.bpmSource = 'none'; __shim.S.nativeBpm = 0; })()"
        pg.evaluate(none); pg.wait_for_timeout(120)
        foot_none = foot()
        ex = wx(0.5)
        pg.mouse.move(ex, hy); pg.mouse.down(); pg.mouse.move(ex - 120, hy, steps=3); pg.mouse.move(ex - 231, hy, steps=3); pg.wait_for_timeout(120)
        during = {"bpm": st("nativeBpm"), "source": st("bpmSource")}
        pg.mouse.up(); pg.wait_for_timeout(200)
        I["no_tempo_takes_trim"] = {"footer_before": foot_none, "during_drag": during, "end": pg.evaluate("__shim.sv.end"),
                                    "bpm": st("nativeBpm"), "source": st("bpmSource"), "bars": pg.evaluate("__ln.bars()"), "footer": foot()}
        pg.evaluate(none); pg.evaluate("__shim.automate('end', 0.125)"); pg.wait_for_timeout(80)
        mid = st("nativeBpm")
        pg.wait_for_timeout(400)
        I["no_tempo_host_edit_settles"] = {"bpm_at_80ms": mid, "bpm": st("nativeBpm"), "source": st("bpmSource"),
                                           "bars": pg.evaluate("__ln.bars()"), "footer": foot()}
        pg.evaluate("__ln.setSnap(true)")
        # the file-tempo control: Double-time counts the file as half as fast (holds under the next trim);
        # double-click types in place; out of range stays open; auto
        pg.click("#bpm-t"); pg.wait_for_timeout(350)
        items = pg.evaluate("[...document.querySelectorAll('#pop-list li')].map(l => l.textContent)")
        pg.locator("#pop-list li", has_text="Double-time").dispatch_event("mousedown"); pg.wait_for_timeout(200)
        half = st("nativeBpm")
        pg.evaluate("__ln.setParam('end', 0.5)"); pg.wait_for_timeout(150)
        I["bpm_half"] = {"items": items, "invoke": last_inv(), "bpm": half, "source": st("bpmSource"),
                         "bpm_after_trim": st("nativeBpm"), "bars": pg.evaluate("__ln.bars()"), "footer": foot()}
        shown = lambda: pg.evaluate("getComputedStyle(document.getElementById('bpm-in')).display")
        pill = pg.evaluate("(() => { const c = getComputedStyle(document.getElementById('bpm-t')); return c.borderTopStyle + ' ' + c.borderTopWidth; })()")
        pg.dblclick("#bpm-t"); pg.wait_for_timeout(350)
        typed_state = {"input": shown(), "value": pg.input_value("#bpm-in"), "popup": pg.evaluate("document.getElementById('pop').className"),
                       "focused": pg.evaluate("document.activeElement && document.activeElement.id")}
        pg.keyboard.type("92.5"); pg.keyboard.press("Enter"); pg.wait_for_timeout(200)
        I["bpm_typed"] = {"pill": pill, "editing": typed_state, "invoke": last_inv(), "bpm": st("nativeBpm"), "footer": foot(), "input_after": shown()}
        pg.dblclick("#bpm-t"); pg.wait_for_timeout(350); pg.keyboard.type("9"); pg.keyboard.press("Enter"); pg.wait_for_timeout(150)
        refused = shown()
        pg.keyboard.press("Escape"); pg.wait_for_timeout(80)
        I["bpm_typed_out_of_range"] = {"invoke": last_inv(), "bpm": st("nativeBpm"), "input_still_open": refused, "input_after_esc": shown()}
        pg.click("#bpm-t"); pg.wait_for_timeout(350)
        pg.locator("#pop-list li", has_text="Type a BPM").dispatch_event("mousedown"); pg.wait_for_timeout(150)
        I["bpm_type_row"] = {"input": shown(), "popup": pg.evaluate("document.getElementById('pop').className")}
        pg.keyboard.press("Escape"); pg.wait_for_timeout(80)
        pg.click("#bpm-t"); pg.wait_for_timeout(350)
        items = pg.evaluate("[...document.querySelectorAll('#pop-list li')].map(l => l.textContent)")
        pg.locator("#pop-list li", has_text="Auto").dispatch_event("mousedown"); pg.wait_for_timeout(200)
        I["bpm_auto"] = {"items": items, "invoke": last_inv(), "bpm": st("nativeBpm"), "source": st("bpmSource")}
        # ---- the tempo cell is what plays: drag it and SYNC comes off; the SYNC button snaps back to the host
        cell = lambda: {"text": pg.inner_text("#tempo-line"), "sync": st("sync"), "free": st("freeBpm"), "sync_cls": pg.get_attribute("#sync", "class")}
        if not st("sync"): pg.click("#sync"); pg.wait_for_timeout(400)
        before = cell()
        tb = pg.evaluate("(() => { const r = document.getElementById('tempo-line').getBoundingClientRect(); return [r.left + 20, r.top + r.height / 2]; })()")
        n_inv = len(inv())
        pg.mouse.move(*tb); pg.mouse.down(); pg.mouse.up(); pg.wait_for_timeout(150)                 # a click is not a drag
        click_sent = inv()[n_inv:]
        pg.evaluate("__shim.hatchClick()"); pg.wait_for_timeout(1900)
        pg.mouse.move(*tb); pg.mouse.down(); pg.mouse.move(tb[0], tb[1] - 15, steps=5); pg.wait_for_timeout(80)
        mid = cell()
        pg.mouse.move(tb[0], tb[1] - 30, steps=5); pg.mouse.up(); pg.wait_for_timeout(500)
        I["tempo_drag_up_30px"] = {"before": before, "click_sent": click_sent, "mid": mid, "after": cell(), "invoke": last_inv(),
                                   "expect": 100, "native_unmoved": st("nativeBpm"), "hatch": st("hatch")}
        pg.mouse.move(*tb); pg.mouse.down(); pg.keyboard.down("Shift"); pg.mouse.move(tb[0], tb[1] + 15, steps=5); pg.keyboard.up("Shift"); pg.mouse.up(); pg.wait_for_timeout(500)
        I["tempo_drag_shift_fine"] = {"after": cell(), "expect": 99.5}
        pg.mouse.move(*tb); pg.mouse.down(); pg.mouse.move(tb[0], tb[1] + 2000, steps=8); pg.mouse.up(); pg.wait_for_timeout(500)
        I["tempo_drag_floor"] = {"after": cell(), "expect": 30}
        pg.click("#sync"); pg.wait_for_timeout(500)
        I["sync_snaps_back"] = {"invoke": last_inv(), "after": cell()}
        pg.mouse.move(*tb); pg.mouse.down(); pg.mouse.move(tb[0], tb[1] - 30, steps=5); pg.mouse.up(); pg.wait_for_timeout(500)
        dragged = cell()
        pg.mouse.dblclick(*tb); pg.wait_for_timeout(500)
        I["tempo_dblclick_drops_it"] = {"dragged": dragged, "invoke": last_inv(), "after": cell()}
        pg.screenshot(path=str(out / "shim-tempo-row.png"), clip={"x": 680, "y": 380, "width": 968, "height": 150})
        pg.click("#sync"); pg.wait_for_timeout(400)
        # ---- print bins: the BIN popup asks native for the folders each time it opens
        chip = lambda: pg.inner_text("#bin-path")
        def open_bin():
            pg.click("#bin"); pg.wait_for_timeout(150)
            return ctx_items()
        items = open_bin()
        focused = pg.evaluate("document.activeElement && document.activeElement.id")
        pg.keyboard.type("gri"); pg.wait_for_timeout(80)
        filtered = ctx_items()
        pg.keyboard.press("Meta+A"); pg.keyboard.type("grimey"); pg.keyboard.press("Enter"); pg.wait_for_timeout(200)
        I["bin_pick_typed"] = {"asked": inv().count("ln_bins"), "items": items, "focused": focused, "filtered": filtered, "invoke": last_inv(),
                               "bin": st("bin"), "chip": chip(), "open": pg.evaluate("document.getElementById('pop').className")}
        open_bin()
        cur = pg.evaluate("[...document.querySelectorAll('#pop-list li.cur')].map(l => l.textContent)")
        n_inv = len(inv())
        I["bin_no_standing_create_row"] = {"current": cur, "create_rows": pg.locator("#pop-list li.new").count(), "hint": pg.get_attribute("#pop-q", "placeholder")}
        pg.keyboard.type("GRIMEY"); pg.wait_for_timeout(80)
        I["bin_no_create_for_a_bin_that_exists"] = {"items": ctx_items()}
        pg.keyboard.press("Meta+A")
        pg.keyboard.type("tape"); pg.wait_for_timeout(80)
        typed = ctx_items()
        pg.keyboard.press("Enter"); pg.wait_for_timeout(200)
        I["bin_new"] = {"items": typed, "invoke": last_inv(), "bin": st("bin"), "chip": chip(), "listed_next_open": open_bin()}
        pg.keyboard.press("Escape")
        # a print lands in the picked bin; picking another bin makes the armed print stale
        pg.evaluate("__shim.hatchClick()"); pg.wait_for_timeout(1900)
        armed = {"hatch": st("hatch"), "file": pg.inner_text("#hatch-file"), "printName": st("printName")}
        open_bin(); pg.locator("#pop-list li:not(.sep)", has_text="No bin").dispatch_event("mousedown"); pg.wait_for_timeout(450)
        I["bin_change_stales_print"] = {"armed": armed, "invoke": last_inv(), "bin": st("bin"), "chip": chip(),
                                        "hatch": st("hatch"), "cls": pg.get_attribute("#hatch", "class")}
        # the picked bin's folder is gone: the chip says PRINTS, the popup says why, nothing recreates it
        open_bin(); pg.locator("#pop-list li", has_text="tape").dispatch_event("mousedown"); pg.wait_for_timeout(200)
        n_inv = len(inv())
        pg.evaluate("__shim.S.binMissing = true"); pg.wait_for_timeout(150)
        items = open_bin()
        pg.screenshot(path=str(out / "shim-bins.png"), clip={"x": 1180, "y": 440, "width": 480, "height": 300})
        info = pg.evaluate("[...document.querySelectorAll('#pop-list li.info')].map(l => l.textContent)")
        pg.locator("#pop-list li.info").dispatch_event("mousedown"); pg.wait_for_timeout(100)                   # the note is not a choice
        I["bin_missing"] = {"chip": chip(), "title": pg.get_attribute("#bin", "title"), "items": items, "info": info,
                            "sent": [x for x in inv()[n_inv:] if x != "ln_bins"], "open": pg.evaluate("document.getElementById('pop').className")}
        pg.locator("#pop-list li:not(.info):not(.sep)", has_text="No bin").dispatch_event("mousedown"); pg.wait_for_timeout(200)
        I["bin_back_to_root"] = {"invoke": last_inv(), "bin": st("bin"), "missing": st("binMissing"), "chip": chip()}
        open_bin(); pg.locator("#pop-list li", has_text="grimey").dispatch_event("mousedown"); pg.wait_for_timeout(200)
        items = open_bin()
        pg.locator("#pop-list li", has_text="in Finder").dispatch_event("mousedown"); pg.wait_for_timeout(150)
        I["bin_show_in_finder"] = {"row": items[-1], "invoke": last_inv(), "open": pg.evaluate("document.getElementById('pop').className")}
        open_bin(); pg.locator("#pop-list li:not(.info):not(.sep)", has_text="No bin").dispatch_event("mousedown"); pg.wait_for_timeout(200)
        pg.evaluate("__ln.setParam('end', 1)"); pg.evaluate("__ln.setSnap(false)")
        pg.evaluate("__ln.roundTrip()"); pg.wait_for_timeout(1200)
        I["round_trip_shim"] = pg.evaluate("__ln.roundTripResult()")
        I["final_check_errors"] = pg.evaluate("__ln.check().errors")
        # ---- a dragged tempo is the loop's it was dragged on: the next spin goes back to SYNC (last: it changes the loop)
        pg.mouse.move(*tb); pg.mouse.down(); pg.mouse.move(tb[0], tb[1] - 30, steps=5); pg.mouse.up(); pg.wait_for_timeout(500)
        dragged = cell()
        pg.click("#spin"); pg.wait_for_timeout(600)
        I["spin_drops_dragged_tempo"] = {"dragged": dragged, "after": cell(), "host": st("hostBpm")}
        ctx.close()
        b.close()
    res["errors"] = errors
    (out / "results.json").write_text(json.dumps(res, indent=1))

    # ---- side by side with the reference shots
    from PIL import Image, ImageChops
    for mine, ref in [("rest-1672x941", "shot-final-rest.png"), ("live-1672x941", "shot-final-live.png"),
                      ("rest-1003x565", "shot-final-scale-0.6.png")]:
        a = Image.open(out / f"shim-{mine}.png").convert("RGB"); r = Image.open(REF / ref).convert("RGB")
        sbs = Image.new("RGB", (a.width, a.height * 2 + 8), (255, 0, 255))
        sbs.paste(r, (0, 0)); sbs.paste(a, (0, a.height + 8)); sbs.save(out / f"cmp-{mine}.png")
        d = ImageChops.difference(a, r).convert("L").point(lambda v: min(255, v * 4)); d.save(out / f"diff-{mine}.png")
    print(json.dumps({"errors": errors, "interactions": res["interactions"]}, indent=1)[:6000])


if __name__ == "__main__":
    sys.exit(main())
