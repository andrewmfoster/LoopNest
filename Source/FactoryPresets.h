#pragma once
#include "Snapshot.h"

// The factory preset table (moved out of the native editor's PluginEditor.cpp in v2
// stage 2; the web UI's bridge reads it).
namespace loopnest
{
// ---- Factory presets ----------------------------------------------------
// A preset overwrites ONLY the CHARACTER panel (5 knobs + their 2nd axes), the
// echo/reverb wet bands, and the master EQ — never pitch, levels, mix or trim.
// Values are in NATURAL units (UiBridge::selectPreset restores them); STARTER set, tune
// by ear. Column order is loopnest::CharacterParams::fields() (the chain order).
inline constexpr int kNumPresetParams = loopnest::CharacterParams::kNum;

struct FactoryPreset { const char* name; float v[kNumPresetParams]; };
// LOW-END RULE: any wet space (echo/reverb) hi-passes its tail so the kick stays
// dry+punchy — reverbLo/echoLo sit ~0.30-0.38 (≈160-270 Hz on the log axis
// f=20·1000^x), NOT ~0.05-0.12 (≈28-46 Hz, below the kick = boom). Tuned for drum
// loops. 11 destinations across 2 axes (degradation amount × flavor), not feature
// demos: a lo-fi ladder (Vinyl→Dust), an EQ trick (Radio), pure sat (Crush), two
// kick-safe spaces (Slap/Plate/Hall), a stereo move (Width), an everything-bus (Lo-Fi).
// rvbDecay was converted 10-08 when DECAY went geometric, so each keeps its old tail
// (old 30/35/40/50/85 -> 17/20/23/31/69). warp and flutter were converted 10-17 when their
// knobs were retapered, so each keeps its wobble: flutter halved, warp through
// CharacterChain::warpFromOld at its own warpRate (old 15/25/35/45/10/30 -> 8.5/15/23.5/23/5/16,
// rounded to the 0.5 % grid).
// rvbType: 0 PLATE, 1 HALL, 2 SPRING; Plate is PLATE, every other preset HALL (Andrew, 10-18).
//  warp warpRate drive driveTone flutter flutterRate echo reverb  echoLo echoHi  rvbLo rvbHi  eqLo  eqHi   width wLo  wHi    rvbDecay echoTime wMid rvbType
inline const FactoryPreset kFactoryPresets[] = {
    { "Vinyl",    {  8.5, 40, 18, -10, 17.5, 55,   0,  8,  0.00f, 1.00f, 0.32f, 0.85f, 0.04f, 0.92f,  15, 0.10f, 1.00f,  17, 44,   0,   1 } },
    { "Tape",     {   15, 35, 35, -20,   15, 50,   0, 12,  0.00f, 1.00f, 0.34f, 0.80f, 0.04f, 0.80f,  15, 0.10f, 1.00f,  20, 44,   0,   1 } },
    { "Cassette", { 23.5, 30, 22, -15,   30, 60,   0,  8,  0.00f, 1.00f, 0.33f, 0.78f, 0.06f, 0.70f,  25, 0.12f, 1.00f,  17, 44,   0,   1 } },
    { "Dust",     {   23, 55, 55, -25, 12.5, 50,  10, 10,  0.30f, 0.85f, 0.34f, 0.72f, 0.06f, 0.68f,  18, 0.10f, 1.00f,  23, 50,   0,   1 } },
    { "Radio",    {    0, 50, 40,  10,    5, 50,   0,  0,  0.00f, 1.00f, 0.00f, 1.00f, 0.32f, 0.62f,   0, 0.00f, 1.00f,  31, 44,  15,   1 } },
    { "Crush",    {    5, 50, 75,  20,    5, 50,   0,  0,  0.00f, 1.00f, 0.00f, 1.00f, 0.06f, 0.95f,  10, 0.10f, 1.00f,  31, 44,   0,   1 } },
    { "Slap",     {    0, 50, 15,   0,  7.5, 50,  55,  0,  0.30f, 0.85f, 0.00f, 1.00f, 0.04f, 0.85f,  30, 0.15f, 1.00f,  31, 25,   0,   1 } },
    { "Plate",    {    0, 50, 10,   0,    0, 50,   0, 40,  0.00f, 1.00f, 0.35f, 0.95f, 0.03f, 1.00f,  25, 0.10f, 1.00f,  20, 44,   0,   0 } },
    { "Hall",     {    0, 50, 10,   0,    0, 50,   0, 65,  0.00f, 1.00f, 0.36f, 0.92f, 0.03f, 1.00f,  40, 0.10f, 1.00f,  69, 44,   0,   1 } },
    { "Width",    {    0, 50,  0,   0,    0, 50,   0,  0,  0.00f, 1.00f, 0.00f, 1.00f, 0.00f, 1.00f,  70, 0.15f, 1.00f,  31, 44, -20,   1 } },
    { "Lo-Fi",    {   16, 45, 40, -15,   20, 55,  15, 18,  0.30f, 0.80f, 0.34f, 0.78f, 0.06f, 0.72f,  20, 0.10f, 1.00f,  23, 40,   0,   1 } },
};
inline constexpr int kNumFactoryPresets = (int) (sizeof(kFactoryPresets) / sizeof(kFactoryPresets[0]));
}
