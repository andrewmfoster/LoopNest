# LoopNest

A loop-curation + loop-creation instrument (VST3 + AU) built with JUCE, for macOS / Apple Silicon.

## Concept

**Curate → spin → shape → hatch.** Build folders of loops (including via the in-plugin
drum-loop extractor), then spin a random file out of your curated stash, trim and shape it
(pitch + lo-fi character) while it loops, and **hatch**: render a `.wav` whose drag source
drops straight onto a DAW audio track. Spins are unlimited; bar-chopping is left to the DAW
after the drop.

## Tech stack

- **Framework:** JUCE (sibling folder `../JUCE`, referenced relatively in CMakeLists.txt)
- **Build:** CMake · **Formats:** VST3 + AU · **Language:** C++17 · **Platform:** macOS arm64

## Build

```bash
# Clean reconfigure only needed when the plugin type / CMake plugin flags change.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

## Install (after build)

The build emits **two** bundles (VST3 + AU); install + re-sign **both**.

```bash
# VST3 → Ableton / Reaper / Bitwig / Cubase …
rm -rf ~/Library/Audio/Plug-Ins/VST3/LoopNest.vst3
cp -r build/LoopNest_artefacts/Debug/VST3/LoopNest.vst3 ~/Library/Audio/Plug-Ins/VST3/
codesign -s - --deep --force ~/Library/Audio/Plug-Ins/VST3/LoopNest.vst3

# AU → Logic / GarageBand / MainStage
rm -rf ~/Library/Audio/Plug-Ins/Components/LoopNest.component
cp -r build/LoopNest_artefacts/Debug/AU/LoopNest.component ~/Library/Audio/Plug-Ins/Components/
codesign -s - --deep --force ~/Library/Audio/Plug-Ins/Components/LoopNest.component
```

Validate the AU: `auval -v aumu LpNs Wndr` → expect `AU VALIDATION SUCCEEDED.`
Then re-add the plugin instance in the DAW (rescan only if the param set / plugin type changed).

## Key notes (hard-won — read before touching build/host behavior)

- **After every build+install, re-run `codesign -s - --deep --force`** on the installed
  bundle(s). JUCE adds `moduleinfo.json` *after* signing the VST3, leaving a stale signature
  that Ableton silently rejects. **THE recurring trap.** Re-run `auval` too if the param set
  changed, or Logic keeps the old scan.
- The plugin is an **instrument** (`IS_SYNTH TRUE`) that **ignores MIDI for sound** — audition
  is driven by the UI Play/Stop, not note-on. But it **must declare a MIDI input bus**
  (`NEEDS_MIDI_INPUT TRUE`): ⚠️ Ableton refuses to open an Instrument-category VST3 with no
  event input ("could not be opened"). `processBlock` ignores the MIDI buffer entirely.
- **A VST3 cannot place clips in a host timeline** — no host API exists. Export is drag-and-drop
  of the rendered `.wav` (`performExternalDragDropOfFiles`, off the HATCH key once armed); the
  editor is a `DragAndDropContainer`.
- The editor paints in a **1672×941 reference space** via a global scale transform; the window
  is 1003×565 (0.6×). Children are placed with `R(x,y,w,h)` in `resized()`, so reference/mockup
  coordinates map **1:1 — paste them directly.**
- The clang/LSP `'JuceHeader.h' file not found` diagnostics are **noise** — that header only
  exists after CMake configures. Trust the `cmake --build` result.

## Project structure

```
Source/
├── PluginProcessor.{h,cpp}   — audio, plugin state, spin/render
├── PluginEditor.{h,cpp}      — monochrome-blueprint UI (component map below)
└── CharacterChain.h          — Stage 2 character DSP, shared by audition + render
fonts/                        — bundled JetBrains Mono (OFL) → BinaryData (build dep)
```

**PluginEditor component map** (names in the source):
- `TapeLookAndFeel` — blueprint rotary L&F (rings + bold pointer + teal value arc). Name is historical.
- `TransportButton` — PLAY / LOOP / PRINT keys; PRINT (label **"HATCH"**) is the **drag-out source**.
  Renders like PLAY until armed; on hatch its teal fill fades in (`armGlow`) and the label becomes "DRAG".
- `ReelBay` — line-art cassette; reels revolve only while audio plays; clicking spins.
- `SpinDial` — big RE-SPIN rotor; a trigger, not a parameter (click = spin-burst).
- `WaveformDisplay` — teal scope; header band shows folder · count + filename and hosts the two
  `IconButton`s on its left.
- `ValueChip` — editable value read-outs (`juce::Label` subclass).
- `IconButton` — **folder** (switch sample folder) + **funnel** (drum extract), docked in the scope header.
- `BandFilter` — two-handle band-pass (low-cut + high-cut, teal kept-band fill). Drives two apvts params
  directly (not a `SliderAttachment`). **Four instances**: `echoBand` / `reverbBand` / `widthBand` +
  `eqBand` (master EQ, pre-rack).
- `MixSlider` — master DRY/WET strip; `SliderAttachment` to `mix`. Bottom-right in the bottom band.
- `PresetBar` — factory-preset selector, bottom band between EQ and MIX. Centred name + `*` when tweaked
  off-book; chevrons wrap; name-click → scrolling `PopupMenu`. The editor owns the factory table
  (`kFactoryPresets`, 12 presets) + `applyPreset`/`presetMatches`. A preset overwrites the full CHARACTER
  cell (6 knobs + every 2nd axis + the 3 tertiaries + master EQ) — never pitch/levels/mix/trim.
  `currentPreset=-1` ("INIT") at load; never auto-applied.
- INPUT / OUTPUT knobs — two `KnobControl`s stacked in the SPIN column. Both −60..+6 dB.
- **Bottom band** (`R(569,812,1063,102)`) — master **EQ** band · **PRESET** bar · **MIX** strip.
- **Knob tier** — one full-width 6-knob CHARACTER panel (`R(569,492,1063,302)`), columns mirroring the
  DSP chain order: **WARP · FLUTTER · DRIVE · ECHO · REVERB · WIDTH** (idx0–5).
  - idx0–2: rotary cells — secondary rotary (RATE/RATE/TONE) above the primary knob.
  - idx3–5: band cells — a `BandFilter` band on top, a tertiary mini-rotary (TIME/DECAY/CENTER), primary below.
- Logo mark (`drawLogo`) — nested concentric flat-top hexagons, innermost filled teal.
- Wordmark — hand-drawn chamfered-metal vector (`namespace wm`), spells "LOOPNEST".

**Drum-loop curation** — the funnel icon runs `LoopNestProcessor::extractDrumLoops(src, dest)`: a
background thread scans a source folder, copies just the full drum grooves into a chosen dest, then
auto-adopts dest as the sample folder. Additive (dedup by output filename). Guard: dest must be outside
the source tree.

## Identity (CMake)

Target `LoopNest`, `PRODUCT_NAME "LoopNest"`, `PLUGIN_CODE LpNs`,
`PLUGIN_MANUFACTURER_CODE Wndr`, `COMPANY_NAME "Wander Foster"`,
`BUNDLE_ID com.wanderfoster.loopnest`, apvts state id `LoopNestState`.
Classes `LoopNestProcessor` / `LoopNestEditor`.

## Parameters

Full signal path: `input(gain) → EQ → [WARP → FLUTTER → DRIVE → ECHO → REVERB → WIDTH] → output(gain)`.
The CHARACTER panel's left→right order mirrors this chain. Each rack control is **true-bypass at its
neutral value**; the EQ is true-bypass at open handles. **Character-chain detail lives in
`Source/CharacterChain.h` — the code is the source of truth.**

- `pitch` — −12..+12 st, varispeed (fractional readPos + linear interp; also corrects file/host SR mismatch).
- `pitchGlide` — 0..100 %, slews the playback rate toward the pitch target (tape spin-up). Audition-only.
- `input` — −60..+6 dB pre-rack gain stage. `output` — −60..+6 dB playback level (post-chain).
- `start` / `end` — trim region, 0..1; reset to 0/1 on each spin.
- `warp` / `flutter` / `drive` / `echo` / `reverb` / `width` — 0..100 %, the six Stage-2 character primaries.
- `warpRate` / `flutterRate` (0..100 %) · `driveTone` (−100..+100 %) — the rotary-cell 2nd axes.
- `echoLo`/`echoHi` · `reverbLo`/`reverbHi` · `widthLo`/`widthHi` — 0..1 each, the band-pass handles
  (log axis ~20 Hz..20 kHz; `lo=0`/`hi=1` = open).
- `reverbDecay` / `echoTime` / `widthMid` — band-cell tertiaries (room/tail · 30..350 ms slapback · mid ±9 dB).
- `eqLo` / `eqHi` — master EQ band-pass handles (pre-rack, runs on the whole signal before the dry/wet tap).
- `mix` — 0..100 % master DRY/WET; bakes into the print. Default 100 %.
- `bypass` / `gainMatch` — A/B monitor pair, audition-only (never bakes): BYPASS swaps to the dry tap;
  GAIN MATCH K-weighted (BS.1770) loudness-matches the processed level to the dry tap.

PRINT renders the trimmed region at the source sample rate with pitch baked in, always stereo, to a
timestamped file under `~/Music/LoopNest/Prints/`. The chain is primed with discarded passes so
echo/reverb tails ring across the loop seam exactly like the looped audition — print-what-you-hear.

## Aesthetic

**Monochrome engineering-blueprint** — thin white/grey line-art on near-black, a single **teal** accent
(HATCH button, active waveform, knob value arcs). Type = bundled JetBrains Mono (OFL); the wordmark is a
hand-drawn chamfered-metal vector (`namespace wm`).
