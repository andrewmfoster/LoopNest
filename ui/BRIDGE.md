# ui/index.html ↔ native: the bridge

The page is one file, `ui/index.html` (the locked stage 2 mock, ported). It is bundled into
BinaryData with JUCE's `juce/index.js` + `check_native_interop.js` and the JetBrains Mono TTFs,
and served by `LoopNestWebEditor::getResource` **by original filename**: `/` → `index.html`,
`/juce/index.js` → `index.js`, `fonts/JetBrainsMono-Regular.ttf` → that TTF. Nothing loads from
disk or the network. A new file must be added to `juce_add_binary_data` in `CMakeLists.txt`, and
its filename must be unique across the bundle.

Layout: the stage is the mock's 1672 × 941 reference space, scaled from the top-left corner by
`fit = min(innerWidth / 1672, innerHeight / 941)` (0.6 in the 1003 × 565 window), the same
mapping `WebEditor::updateOverlay` uses, so the page's `#hatch` tile and the native overlay
cover the same rect: ref (1452, 454, 196, 66) × fit.

The editor is resizable at the reference aspect, 0.6–1.25× (default 0.8): by the host's frame
where it offers one, and by the page's `#grip` corner everywhere (`ln_resize`). The scale is
saved app-wide (`editorScale` in the settings file) a second after a resize settles, and on close.

## Boot

1. The page registers its event listeners, then calls `ln_ready()` once.
2. Native answers with `ln_files`, `ln_peaks` (cached, or a fresh scan) and `ln_state`, then
   emits `ln_state` at 30 Hz. Events only reach a visible browser (`emitEventIfBrowserIsVisible`).

## Parameters: relays

One `WebSliderRelay` per float parameter, relay name = parameter id:
`pitch pitchGlide input output start end mix warp warpRate drive driveTone flutter flutterRate
echo reverb echoLo echoHi reverbLo reverbHi eqLo eqHi width widthLo widthHi reverbDecay
echoTime widthMid`. Toggles (`WebToggleButtonRelay`): `bypass gainMatch warpOn flutterOn driveOn
echoOn reverbOn widthOn` (the last six are panel power, default ON). Choice (`WebComboBoxRelay`):
`reverbType`, choices `Plate Hall Spring`, default Hall.

- Values on the relay are **natural units** (the attachment sends `convertFrom0to1`). They are
  floats: 0 dB can arrive as 8.9e-7, so formatters round before choosing a sign.
- The page writes with `setNormalisedValue(toNorm(v))`, where
  `toNorm(v) = ((v - start) / (end - start)) ^ skew` from the relay's `properties`
  (JUCE `NormalisableRange`); it waits for `propertiesChanged` before writing.
- Every drag is a gesture: `sliderDragStarted()` … writes … `sliderDragEnded()`. A click-sized
  change (double-click reset, a wheel notch, a band reset) is its own begin/set/end.
- Any `valueChangedEvent` (host automation, undo, a preset) marks the controls dirty; the next
  frame re-renders them. `K[id]` in the page is a proxy over the relays.
- Trim handles on the scope drive `start` / `end`.
- Reset: double-click or option-click a knob, a band or the WET track puts it back to its
  `PARAM_DEFAULTS` value (a band opens fully). Every knob's value readout (`typeIn`) takes a
  click to type a value in the units it shows; Enter or a click away sets it, Esc cancels,
  Delete/Backspace on the untouched value (or Enter on an empty one) sets the default.
- REVERB TYPE: a PLATE / HALL / SPRING stack (`.rtype`) in the gutter right of REVERB's big knob.
  The combo relay's value is normalised (index / 2); the page reads `round(value × 2)`, shows no
  choice until the first value lands (`__ln.reverbType()` = -1, `check().relays.reverbType` = null,
  `check().reverbTypeReceived` false), and writes `setChoiceIndex` only once the relay's `choices` are in
  (the attachment makes each click one host undo step). The active choice takes `--acc` while the
  panel is lit (on, REVERB above 0), a grey ring otherwise; panel off dims it like the rest.
  Host automation, undo and presets move it through `valueChangedEvent`. `__ln.reverbType()` /
  `__ln.setReverbType(i)` are the test hooks; `__ln.check()` reports it in `relays`.
- Panel power: each rack column's name row leads with a round light (`.pwr`); a click on the
  light (only the light) flips that column's `…On` toggle (one host undo step). Off, the column gets
  `.off` (knobs, band, labels, name and value at 0.32 opacity, arcs and band grey; the knobs
  still drag). Under BYPASS (`#stage.bypass`) a lit light shows `--dormant` grey. The page
  treats a power toggle as ON until its relay's first value lands (no flash at open).

## Native functions (`Juce.getNativeFunction`)

| function | args | effect |
|---|---|---|
| `ln_ready` | | page is listening: send files, peaks, state |
| `ln_spin` | | random loop; always lands in AUDITION (from LIVE it plays once loaded) and always turns SYNC on. Both happen as it lands, not at the call: `live` and `sync` read as before until then (so the old loop never plays first); a loop that won't decode changes neither |
| `ln_back` / `ln_forward` | | spin history ◀ ▶: restore the whole sound (file, trim, pitch, levels, character, tempo, bar 1) of the loop before / after. Only SPIN records (Andrew, 10-11): a spin keeps the sound it replaces (whatever is current, a rack-picked loop with its knobs included) and the loop it brings; a rack pick (`ln_pick`: a row, ◀ ▶, a typed number) adds nothing and drops nothing. Up to 20 back steps (21 entries with the current one); a spin after stepping back drops the forward ones; an entry whose file is gone is stepped over, and one that won't decode drops out. Lands like a spin (AUDITION; from LIVE it plays once loaded; SYNC on; under the bar-grid lock on the next bar line, the step's knobs exact on the line). False, and nothing changes (LIVE stays LIVE), when there is nowhere to go. A host state restore starts the history again. Session only |
| `ln_play` | | play/pause; from LIVE: back to AUDITION and play (and unmuted). While the host's transport owns the audition it is a mute instead: the loop keeps its place on the grid, silent. The transport owns it when all hold: AUDITION, SYNC following the host (not a free tempo), LOOP on, the host playing, a loop loaded whose tempo is known and within the stretcher's range of the host's (0.25–4×). The mute holds across the host's stops and starts, and across SYNC off or a free tempo (the free audition they hand over to stays silent, the key dark); it is saved with the set (a set saved before `stateVersion` 3 loads muted). Only a press clears it: the key under the lock, or a free press, which also plays / pauses |
| `ln_loop` | bool | audition loops |
| `ln_live` | bool | the AUDITION / LIVE tabs (LIVE = the processor's FX mode) |
| `ln_sync` | bool | the SYNC button: follow host tempo; either way it clears a free tempo (off = the file's own tempo) |
| `ln_tempo` | bpm | a tempo of LoopNest's own (the tempo cell, dragged; clamped 30..300): SYNC goes off and the loop plays at it, pitch held, audition and print; a spin or a picked loop (`ln_spin`, `ln_pick`) always turns SYNC on, which drops it (Andrew, 10-01); saved with the set; ≤ 0 drops it |
| `ln_printFx` | bool | the print row's FX switch (on = chain bakes into the print) |
| `ln_preset` | idx | −1 = INIT, 0..10 factory (one host undo step). The index lives on the processor and is saved with the set, so `state.preset` survives an editor reopen and a project reload (a set without one reads INIT) |
| `ln_presetStep` | ±1 | cycle INIT + factory presets, wrapping |
| `ln_seek` | 0..1 | read head, fraction of the whole file (scope click) |
| `ln_bank` | | native folder chooser (BANK popup: "Choose folder…") |
| `ln_extract` / `ln_extractCancel` | | drum-loop extraction (BANK popup) |
| `ln_overlay` | bool | show / hide the native HATCH overlay |
| `ln_pick` | index | load row `index` of `ln_files.names`; lands in AUDITION (from LIVE it plays once loaded), SYNC on as a spin; adds no spin-history entry |
| `ln_spinPick` | index | SPIN during a search: the page picks row `index` among the matches; lands as `ln_pick` does, but it is a spin, so the spin history records it |
| `ln_openRecent` | path | adopt a folder from `ln_files.recents` (refused while extracting) |
| `ln_resize` | scale | window width / 1672, clamped to the limits (the `#grip` drag) |
| `ln_bpm` | bpm | set the loop's tempo by hand (clamped 30..300; source `user`, holds under later trims); ≤ 0 = back to automatic (the filename tag, else the whole file as 1/2/4/8 bars) |
| `ln_bpmFit` | | "the trimmed loop is the beat": read the tempo off the trim handles, once (source `length`), even on a tagged file: the region as 1/2/4/8 bars, the reading closest to the host tempo, and bar 1 moves to the start handle; false (and no change) when nothing fits |
| `ln_bins` | | returns the bins now: the Prints root's subfolders, scanned on the call (hidden ones skipped) |
| `ln_bin` | name | pick the bin HATCH prints into; `""` = the root; false for a folder that is not there |
| `ln_binNew` | name | make the folder under the Prints root and pick it; false for an unusable name |
| `ln_binShow` | | reveal the folder HATCH prints into now in Finder (the picked bin, else the root); never creates one; false when it is not there |
| `ln_barOrigin` | 0..1 | where bar 1 starts, as a fraction of the file (0 = reset to its first sample); display only, saved with the set, 0 again for each new file |

The page optimistically shows a toggle it just sent for 300 ms; after that `ln_state` rules.

## Events (native → page)

- `ln_state` (30 Hz): `hasSample sample sampleMissing folder folderMissing count index canSpin histBack histFwd loading playing
  locked loop live sync freeBpm printFx playhead nativeBpm bpmSource(name|length|user|none) tagBpm barOrigin hostBpm
  hatch(none|idle|armed|stale) printName bin binMissing preset presetName presetDirty extracting
  extractProgress extractKept extractStatus inL inR outL outR`. `playhead` is 0..1 of the whole
  file; levels are linear peaks since the previous state. `playing` is what the PLAY key shows:
  audible playback (under the transport's ownership, false while muted). `locked`: the audition
  runs on the host's bar grid. `histBack` / `histFwd`: a spin-history step is possible. `tagBpm` is the loaded file's
  filename tempo (0 = untagged), whatever `nativeBpm` currently is. `freeBpm` is the dragged
  tempo (0 = none; only ever set with `sync` false). `sampleMissing` is the file name of a
  restored set's loop that can't be found ("" = none; `hasSample` is false and nothing plays):
  a save keeps its reference (path, tempo, bar 1), and choosing a bank that holds it (at its
  saved path, or at its saved path relative to the bank) loads it. Any loop that lands clears it.
- `ln_files` `{folder, path, names: [file names], recents: [{name, path}]}`: on boot, whenever
  the folder's list changes, and when the recents' order does. `recents` is the last 8 folders
  adopted, newest first, the current one included.
- `ln_peaks` `{path, name, seconds, peaks: [1600 × 0..1], hi, hits}`: per loaded sample (empty
  when none). `hi` is base64 of up to 32768 bytes, one per column, sqrt-companded (`v = (b/255)²`),
  for the zoomed scope and the loupe. `hits` is the detected drum attacks as fractions of the
  file, 0..1 (`UiBridge::computeHits`: at least 45 ms apart; can be empty).
- `ln_hatch` `{hover: bool}` or `{press: bool}`: the overlay's pointer, for the tile's looks.
- `ln_hatched` `{ok, name}`: a click on the overlay printed; the page plays render → crack, then
  shows `state.hatch` / `printName`.
- `ln_drop` `{hover: bool}`: an accepted drag-in is over the rack (`#n-drop.on`, see DRAG-IN).

## What the page renders from where

- Rack spines: `ln_files.names`, BPM = `(\d+) ?bpm` in the name (case-insensitive) else blank;
  lit row = `state.index`, or a just-clicked row for up to 2 s while its load lands. A click loads
  the row (`ln_pick`); ◀ ▶ step through the rack (through the matches while a search is on); the
  `n / N` counter is the bank position and takes a typed number on click (Enter loads, Esc or
  blur cancels). Search filters spines by every word typed; SPIN during a search picks among
  the matches page-side (`ln_spinPick`), otherwise `ln_spin`. A new folder clears the search.
- Wire: from the loaded spine into the cassette label. Scrolled out of the rack's view it holds
  at the rack's top or bottom edge; filtered out by a search it hides.
- BANK popup: RECENT (`ln_files.recents`, current one lit) above Choose folder… and extraction. BANK label = `state.folder`; scope header = folder · count + sample, or with no sample loaded
  `state.sampleMissing · MISSING` in the same type and grey (gone once a load lands).
- Cassette label: `state.sample`, BPM = `nativeBpm` unless `bpmSource` is `none`. Tempo cell
  = what PLAYS (Andrew, 10-01): SYNC on shows `hostBpm`; off shows `freeBpm`, else `nativeBpm`.
  Its value drags up and down (3 px a BPM, shift = tenths; `ln_tempo`): SYNC goes off and the
  loop plays at that tempo. The SYNC button snaps back to the host, and so does the next
  loop (a spin or a pick); double-click drops a dragged tempo. A plugin cannot set the host's tempo, so the drag never reaches Ableton.
  No drag while the loop has no tempo (`bpmSource` `none`).
- Tempo: `nativeBpm` is native's, saved with the set, and the page never computes it.
  `bpmSource` says where it came from: `name` = the filename tag; `length` = a length read
  as 1/2/4/8 bars (the whole file when it lands, or the trimmed loop on From the handles);
  `user` = typed, halved or doubled. **A trim never moves it** (Andrew, 10-01: the bar lines
  are the file's, like a clip's timeline). Only `none` (nothing has fit yet) takes a tempo
  from its trim: when a trim gesture ends, or 250 ms after a gesture-less start/end change
  goes quiet (host undo, automation). A restored set comes back at its saved tempo. The
  footer's tempo is what the FILE is, a button drawn as a pill (`#bpm-t`, laid over the canvas
  text). A click opens a list 220 ms later (so a double-click can type instead): `Half-time`
  (`ln_bpm(2×)`: the file counted twice as fast plays half as fast once stretched),
  `Double-time` (`ln_bpm(½×)`), From the handles (`ln_bpmFit`, one read), Auto (`ln_bpm(0)`:
  the tag, else the whole file), `Type a BPM…`. Double-click, that last row, or any click
  while there is no tempo types in place (`#bpm-in`: Enter sets 30..300, Esc or blur cancels).
  Footer source word: `from filename`, `set` or `guessed`; with no tempo it reads
  `NO TEMPO · SET BPM`.
- Scope, bar grid and HATCH thumbnail: `ln_peaks`. The grid is the FILE's bars: bar 1 starts
  at `state.barOrigin` (the file's first sample unless moved), one bar = 240 / (nativeBpm ×
  seconds) of the file, lines run both ways from bar 1 and numbers from it on; the trim
  handles never move it; each bar number sits on the line where its bar starts. Right-click
  (or ctrl-click) in the wave opens `Bar 1 starts here` (the spot snaps like a handle;
  `ln_barOrigin`) and, once moved, `Reset bar 1`. The footer
  shows the trimmed loop's length in bars (a fraction is fine). SNAP (`#snap-t`, page-local, ON at every open):
  `SNAP HITS` / `SNAP OFF`. On, a dragged handle lands on the nearest of `ln_peaks.hits` or a
  file end (always nearest, not magnetic); alt or shift (fine) drags free; a file with no hits
  drags free. The hits draw as ticks along the top of the scope only while SNAP is on.
  Zoom is page-local (`VIEW` = [a, b] of the file, up to 32×, reset by a new loop): pinch or
  ⌘-scroll zooms about the pointer, a sideways swipe or shift-scroll pans, double-click shows the
  whole file. Zoomed, the scope shows an overview bar and `ZOOM n×` in the footer; a handle out of
  view shows as an edge arrow. Holding a trim handle shows a 12× loupe; shift turns the drag
  relative at a tenth of the motion.
  Playhead: `state.playhead`, extrapolated between states at the measured rate.
- LIVE: the input scope scrolls `max(inL, inR)` at session tempo; meters from `inL/inR/outL/outR`.

## HATCH

The page has **no click handler** on `#hatch`: the native overlay (`Source/HatchOverlay`) sits
above the WKWebView and owns every mouse event there (click = print, drag = the `.wav` drag-out).
Hover / press come back as `ln_hatch`, a print as `ln_hatched`. Any page popup whose rect
intersects the tile calls `ln_overlay(false)` while open and `ln_overlay(true)` on close. The
overlay is also hidden natively in LIVE (the tile's row shows meters there). A drag start flips
AUDITION to LIVE; a drag that ends without a drop flips back, playing if it was. Esc is one; so is
letting go anywhere over the plugin: the page refuses every drop, and the rack's drop target
refuses a HATCH print from a LoopNest overlay (Andrew, 10-18).

## DRAG-IN (Andrew, 10-17)

Audio files dragged over the tape rack from Finder, Splice or Live drop into the bank. The page
takes no drop itself: document-level `dragover` / `drop` handlers cancel the default and set
`dropEffect = 'none'`, so a file let go anywhere else on the page does nothing (WebKit would
otherwise open it in its media player), and the editor refuses any navigation outside its bundled
resources (`pageAboutToLoad`). A native view (`Source/RackDrop`, `RackDropTarget`) sits above the
WKWebView over `#n-rack` (ref (38, 134, 282, 372) × fit, `WebEditor::rackRef`), registered for
`public.file-url` and `NSFilenamesPboardType`. Its `hitTest:` returns nil, so clicks, scroll and
hover reach the page; only a drag session finds it. A drag is accepted with a bank and at least one
audio file (the bank scan's extensions); folders and other files in it are skipped. Accepted,
the page lights `#n-drop` (`.spine.sel`'s teal, not drawn at rest) on `ln_drop`. The drop copies
each audio file into the bank's top level (`Source/BankDrop`: same name and bytes = skipped, same name
and other bytes = ` 2`, ` 3` before the extension, a file already in the bank's tree is used
where it is, a symlink copies its target's bytes under its own name) and rescans, on a worker
thread off the message thread; then, back on it, AUDITION picks the first like a rack pick (unless
a row, spin or other load was asked for meanwhile: that choice stands); in LIVE it only copies. A
drop where nothing landed is logged to the HATCH log (nothing on screen yet). A HATCH print dragged
from LoopNest's own overlay is refused (Andrew, 10-18).
The new list arrives as `ln_files`. Every drag that enters is logged to the HATCH log with the
pasteboard types it carried.

## BIN (print bins, `docs/print-bins-spec.md` in the main checkout)

A bin is a subfolder of the Prints root, never a stored record: the page asks `ln_bins` each
time the popup opens, so a Finder rename, mkdir or delete is the truth. Only the picked bin's
NAME is remembered (`state.bin`, an app setting shared by every instance, `""` = the root).
`state.binMissing` = that folder is gone (checked once a second): HATCH then prints into the
root and **never recreates it**; the chip reads `NO BIN` and the popup says why. Popup: a
search box, `No bin` (loose in Prints; the root is not itself a bin), the bins, then `Show … in Finder`
(`ln_binShow`). There is no standing create row (Andrew, 10-01): a typed name that is not a
bin yet shows `Create new bin "name"` first, and Enter takes it unless the name matches a bin exactly. Picking another bin makes
an armed print stale, so the next HATCH puts a copy there. Print names:
`<bin>_<source>_<pitch>[_dry]_<stamp>.wav` (no bin prefix in the root).

## Stubs (later stages)

Extraction progress has no slot in the mock: it shows in the
loop count (`extract 42%`, teal) and in the BANK popup.

## HATCH log

`~/Library/Logs/LoopNest/hatch.log`: editor open/close with the host, every overlay mouse
event, how each drag-out ended (dropped / cancelled), every navigation the editor refused, whenever
`bringToFront()` (30 Hz) had to restack the overlay, and whenever the view a click at the tile
centre would reach changes. Read it after a failed HATCH in a host. Written only by a build
configured with `-DLOOPNEST_DIAG_LOG=ON`; the default (public) build writes nothing.

## Test hooks

`window.__ln` (frozen): `state()`, `files()`, `check()` (errors captured from page start, fonts,
rack vs state, peaks, relay values vs the processor's defaults and ranges, the HATCH rect vs
ref × fit), `roundTrip()` / `roundTripResult()` (sets every float param through the page's setter,
re-asks the native relay, compares, restores: writes parameters, tests only), `setParam`,
`getParam`, `view()` / `setView(a, b)`, `snap()` / `setSnap(bool)`, `snapN(v)` (nearest hit),
`bars()` (the loop's bars), `hits()`; `check()` carries `footer` (the loop facts as drawn), `snap`
(the pill's text), `peaks.hits` (the count) and `state.tagBpm`. Native: `LoopNestTests --hits
<wav>...` prints the detector's hits for real files (read-only). Real WebKit: `LoopNestUiHarness --js … --js2 …` (see `tests/UiHarness.mm`). Chromium
with a fake backend: `tests/ui-shim/run.py`.
