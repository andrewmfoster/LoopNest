# HATCH drag probe (harness only, never Ableton)

Proves the overlay's NSDraggingSession hands a receiver the print's file URL. Both windows float,
and `drag` refuses unless the topmost window at start/end belongs to the named owners, so no
synthetic event lands in another app. Check `pgrep -fl "Ableton Live"` first; skip if it runs.

```bash
swiftc -O drag.swift -o $T/drag; swiftc -O droptarget.swift -o $T/droptarget
$T/droptarget $T/dropzone 300 740 500 200 25 > $T/drop.log &
LOOPNEST_PRINTS_DIR=$T/prints "<harness>" --folder $T/loops --spin --hatch --on-top \
  --pos 100,100 --status $T/status.json --quit-after 25000 &
sleep 5; $T/drag 1030 392 550 840 "LoopNest UI Harness" droptarget
```

Pass (09-30 00:46): drop.log shows `dropped <print>`, the file lands in dropzone, status.json
reads dragsBegun = dragsEnded = 1, live = true. Overlay centre at pos 100,100 is 1030,392.

## Drag-in probe (`dragin.swift`)

The reverse: a floating source window drags real files onto the harness's rack drop target
(`url` = one public.file-url per item, `legacy` = only NSFilenamesPboardType), dwelling over it
before the release. Refuses unless the target point's topmost window is the harness. Aim at the
centre of `rackDrop` from the harness `--status` file; the result shows in `rackDragsEntered`,
`rackDrops`, `rackHoverSeen`, `sampleCount` and `sample`, and in the harness's hatch.log.

```bash
swiftc -O dragin.swift -o $T/dragin
$T/dragin 100 858 <rack centre x> <rack centre y> 1.5 url "LoopNest UI Harness" $T/inbox/a.wav
```

Pass (10-18 00:15): url and legacy drops copy and pick in AUDITION, copy only in LIVE; a .txt
and a folder are refused with no hover; a click and a scroll at the rack still reach the page.
