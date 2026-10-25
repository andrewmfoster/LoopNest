# LoopNest

A loop-curation + loop-creation **audio effect** (VST3 + AU) built with JUCE, for macOS / Apple
Silicon. `main` is v2 (2.0.0, tag `v2.0.0`; scope in the untracked `docs/v2-scope.md`); v1, an
instrument, is the history up to tag `v1.0.2`.

## Concept

**Curate → spin → shape → hatch.** Spin a random loop out of a curated folder, trim and shape it
(pitch + lo-fi character) while it loops, and **hatch**: render a `.wav` that drags onto a DAW
track. AUDITION plays the spun loop (host input discarded); FX runs the track's own audio
through the chain, so a dry hatch dropped on the same track keeps its effects live. SYNC follows
the host tempo with pitch held (Signalsmith Stretch); dragging the tempo cell sets a tempo of
LoopNest's own instead (SYNC off, same stretcher; a plugin cannot set the host's). The loop's native tempo is the filename
tag, else the whole file read as 1/2/4/8 bars, or a typed one. A trim never moves the tempo. Under
the bar-grid lock the left handle is bar 1 on the song's downbeat and the loop is the trim
exactly, any length, no rounding; HATCH prints that same trim (Andrew 10-13). The ruler's bar
grid (barOrigin) is display only. The handles snap to detected drum hits.

## Build + test

JUCE 8 is a pinned submodule at `./JUCE` (fresh clone: `--recurse-submodules`). C++17, CMake,
macOS arm64. The commands below use a Release build dir `build-spike/` (untracked; configure a
fresh one with `cmake -B build-spike -DCMAKE_BUILD_TYPE=Release`). It exists in the v2 worktree,
not the main checkout, whose `build/` is a stale Debug tree: don't build into it.

```bash
cmake --build build-spike -j8
build-spike/LoopNestTests_artefacts/Release/LoopNestTests   # exit 0 = pass
```

`cp` over a source keeps its old mtime: `touch` it or make won't rebuild it.

## Install

v2 installs as `LoopNest.vst3` / `LoopNest.component` and replaces v1's bundles (ruling 10-18);
sets that used v1 then show that device missing (v1 is `LpNs`, v2 `LpSx`).

```bash
V=~/Library/Audio/Plug-Ins/VST3; C=~/Library/Audio/Plug-Ins/Components
rm -rf "$V/LoopNest Spike.vst3" "$C/LoopNest Spike.component"   # once: the pre-rename build, same LpSx/Wndr (a duplicate)
rm -rf "$V/LoopNest.vst3" "$C/LoopNest.component"
cp -R "build-spike/LoopNest_artefacts/Release/VST3/LoopNest.vst3" "$V/"
cp -R "build-spike/LoopNest_artefacts/Release/AU/LoopNest.component" "$C/"
codesign -s - --deep --force "$V/LoopNest.vst3" "$C/LoopNest.component"
codesign --verify --deep --strict "$V/LoopNest.vst3" "$C/LoopNest.component"
auval -v aufx LpSx Wndr    # expect AU VALIDATION SUCCEEDED.
```

Pushes need Andrew's yes: the repo is public and a push is a deploy.

## Key notes (hard-won — read before touching build/host behavior)

- **Re-sign after every install** (above). JUCE adds `moduleinfo.json` *after* signing the VST3;
  Ableton silently rejects the stale seal. Re-run `auval` if the param set changed.
- An **effect** (`IS_SYNTH FALSE`, AU `aufx`, `VST3_CATEGORIES "Fx"`, no MIDI bus). Infinite tail
  so the host never suspends it on silent input. (An *instrument* VST3 with no MIDI input bus
  won't open in Ableton; that's why v1 declares one.)
- **A VST3 cannot place clips in a host timeline.** Export is drag-and-drop of the rendered `.wav`
  (`performExternalDragDropOfFiles` off the HATCH key); a drag start flips the processor to FX.
- **Loads are async** (`Snapshot.h`, stage 0): a loader thread decodes, the message thread
  installs. `commitLock` serialises install / host `setStateInformation` / restore param applies;
  lock order `commitLock → stateLock → loadQueueLock | sampleLock`. The audio thread only
  try-locks `sampleLock`. Every restore is one host undo step (`setParamsAsOneStep`).
- The editor is a WKWebView (`WebEditor.cpp`) serving `ui/index.html`; layout work happens in that
  page, not in C++.
- LSP `'JuceHeader.h' file not found` is **noise**. Trust `cmake --build`.

## Project structure

```
Source/
├── PluginProcessor.{h,cpp}   — audio, plugin state, async load/restore, spin/render
├── Snapshot.h                — CharacterParams + Snapshot ("the whole sound")
├── TimeStretch.h             — SYNC stretcher + cyclic print render (Signalsmith, third_party/)
├── CharacterChain.h          — Stage 2 character DSP, shared by audition + render
├── ReverbTanks.h             — REVERB's PLATE / HALL / SPRING tanks (plain C++)
├── MonitorStage.h            — BYPASS / GAIN MATCH, shared by AUDITION, FX and the wet print
├── WebEditor.{h,cpp}         — the editor: a WKWebView serving ui/index.html from BinaryData
├── UiBridge.{h,cpp}          — the page's native side (actions + state), headless-testable
├── HatchOverlay.{h,mm}       — native NSView over the HATCH tile, the drag-out source
├── RackDrop.{h,mm}           — native NSView over the tape rack, the drag-in target (hitTest: nil)
├── BankDrop.{h,cpp}          — drag-in copy/dedup rules into the bank folder, UI-free (tested)
└── FactoryPresets.h          — kFactoryPresets, applied via restore(kCharacter)
ui/index.html                 — the web UI; its native calls and events: ui/BRIDGE.md
tests/LoopNestTests.cpp       — console tests: snapshot/restore, loader races, get/setState
fonts/                        — bundled JetBrains Mono (OFL) → BinaryData (the page's type)
```

**Drum-loop curation** — the funnel runs `extractDrumLoops(src, dest)` on a background thread,
copying full drum grooves into dest (additive, dedup by filename), then adopts dest. Dest must be
outside the source tree. Don't run it casually from this build: it writes to disk.

## Identity (CMake)

Target `LoopNest`, `PRODUCT_NAME "LoopNest"`, `PLUGIN_CODE LpSx`,
`PLUGIN_MANUFACTURER_CODE Wndr`, `BUNDLE_ID com.wanderfoster.loopnest`, version 2.0.0, apvts state id
`LoopNestState`, `stateVersion` 4 (history: `kStateVersion` in PluginProcessor.h; a pre-4 set's `warp`/`flutter` are converted on restore). Settings, Prints and log dirs follow
`JucePlugin_Name` (`~/Library/Application Support/LoopNest/`, `~/Music/LoopNest/Prints/`,
`~/Library/Logs/LoopNest/`), shared with v1 on purpose: v1's only setting, `defaultFolder`, means
the same here. v1: `LoopNest` / `LpNs` (an instrument) / `com.wanderfoster.loopnest`.

## Parameters

Full signal path: `input(gain) → EQ → [WARP → FLUTTER → DRIVE → ECHO → REVERB → WIDTH] → output(gain)`.
Each rack control is **true-bypass at its
neutral value**; the EQ is true-bypass at open handles. **Character-chain detail lives in
`Source/CharacterChain.h` — the code is the source of truth.**

- `pitch` — −12..+12 st. SYNC off: varispeed (also corrects file/host SR). SYNC on: a transpose.
- `pitchGlide` — 0..100 %, slews the playback rate toward the pitch target (tape spin-up). Audition-only.
- `input` — −60..+6 dB pre-rack gain stage. `output` — −60..+6 dB playback level (post-chain).
- `start` / `end` — trim region, 0..1; reset to 0/1 on each spin.
- `warp` / `flutter` / `drive` / `echo` / `reverb` / `width` — 0..100 %, the six primaries. WARP/FLUTTER:
  100 % = the 10-17 spike's depth; 50 % ≈ the old 100 % (WARP: capped at 60 % of the top); 0.5 % steps.
- `reverbType` — choice PLATE / HALL (default) / SPRING; a change crossfades ~30 ms.
- `warpRate` / `flutterRate` (0..100 %) · `driveTone` (−100..+100 %) — the rotary-cell 2nd axes.
- `echoLo`/`echoHi` · `reverbLo`/`reverbHi` · `widthLo`/`widthHi` — 0..1 each, the band-pass handles
  (log axis ~20 Hz..20 kHz; `lo=0`/`hi=1` = open).
- `reverbDecay` / `echoTime` / `widthMid` — band-cell tertiaries (T30 0.78..5.5 s, SPRING 0.6..3 s · 30..350 ms · mid ±9 dB).
- `eqLo` / `eqHi` — master EQ band-pass handles (pre-rack, before the dry/wet tap).
- `mix` — 0..100 % master DRY/WET; bakes into the print. Default 100 %.
- `bypass` / `gainMatch` — one stage for AUDITION, FX and the wet print (`MonitorStage.h`). BYPASS
  swaps to the dry tap, never bakes. GAIN MATCH loudness-matches (BS.1770) to it, bakes into a wet print.
- `warpOn` … `widthOn` — panel power, default ON. Off = the whole stage neutral (WIDTH also CENTER), knobs
  kept; tails cut in ≤ 15 ms, then cleared. Bakes into PRINT; presets, INIT, CLEAR and pre-power sets = all ON.

PRINT renders the trimmed region (pitch baked in; under SYNC at session tempo), always stereo, to
`~/Music/LoopNest/Prints/` or the picked bin, a subfolder of it (bins are folders, never a
stored list; a picked bin that is gone prints to the root and is never recreated). DRY (default) bakes trim + rate only and bypasses the chain, which
then plays live in FX; wet primes the chain so tails ring across the seam like the audition.

## Aesthetic

**Monochrome engineering-blueprint**: thin white/grey line-art on near-black, one **teal** accent.
Type = bundled JetBrains Mono (OFL); the wordmark is `#wordmark` in `ui/index.html` (bold type with a steel gradient).
