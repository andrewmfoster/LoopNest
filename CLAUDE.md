# LoopNest

A loop-curation + loop-creation **audio effect** (VST3 + AU) built with JUCE, for macOS / Apple
Silicon. `main` is v2 (2.0.0, tag `v2.0.0`; scope in the untracked `docs/v2-scope.md`); v1, an
instrument, is the history up to tag `v1.0.2`.

## Rulings

A trim never moves the tempo. Under the bar-grid lock the loop is the trim exactly, any length,
no rounding, and HATCH prints that same trim (Andrew 10-13). The ruler's bar grid is display only.

## Build + test

JUCE 8 is a pinned submodule at `./JUCE` (fresh clone: `--recurse-submodules`). C++17, CMake,
macOS arm64. The commands below use a Release build dir `build-spike/` (untracked; configure a
fresh one with `cmake -B build-spike -DCMAKE_BUILD_TYPE=Release`).

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

Each rack control is **true-bypass at its neutral value**; the EQ is true-bypass at open handles.
The code is the source of truth for parameters (`Source/CharacterChain.h`, `PluginProcessor`).

PRINT bins are folders, never a stored list; a picked bin that is gone prints to the root and is
never recreated.
