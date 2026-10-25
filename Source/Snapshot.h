#pragma once
#include <JuceHeader.h>
#include <array>

// v2 stage 0: the one description of "the sound". Before this, the chain values
// were spelled out positionally in three places (the audition's setParams, the
// render's, and the editor's preset table). Now CharacterParams is the only list,
// and history, presets and CLEAR all restore through LoopNestProcessor::restore().
namespace loopnest
{
    // The CharacterChain's inputs, in natural units (%, or 0..1 for band handles),
    // in chain order. No defaults here on purpose: the parameter layout owns them,
    // and a second copy would drift. Read one from the processor (capture()).
    // reverbType is a choice (0 PLATE, 1 HALL, 2 SPRING) carried as its index, last so
    // the older columns keep their places.
    struct CharacterParams
    {
        float warp = 0, warpRate = 0, drive = 0, driveTone = 0, flutter = 0, flutterRate = 0,
              echo = 0, reverb = 0, echoLo = 0, echoHi = 0, reverbLo = 0, reverbHi = 0,
              eqLo = 0, eqHi = 0, width = 0, widthLo = 0, widthHi = 0,
              reverbDecay = 0, echoTime = 0, widthMid = 0, reverbType = 0;

        static constexpr int kNum = 21;
        struct Field { const char* id; float CharacterParams::* member; };

        // Parameter id <-> member, in chain order. The factory preset table's
        // columns follow this order too.
        static const std::array<Field, kNum>& fields()
        {
            using P = CharacterParams;
            static const std::array<Field, kNum> f {{
                { "warp", &P::warp },           { "warpRate", &P::warpRate },
                { "drive", &P::drive },         { "driveTone", &P::driveTone },
                { "flutter", &P::flutter },     { "flutterRate", &P::flutterRate },
                { "echo", &P::echo },           { "reverb", &P::reverb },
                { "echoLo", &P::echoLo },       { "echoHi", &P::echoHi },
                { "reverbLo", &P::reverbLo },   { "reverbHi", &P::reverbHi },
                { "eqLo", &P::eqLo },           { "eqHi", &P::eqHi },
                { "width", &P::width },         { "widthLo", &P::widthLo },
                { "widthHi", &P::widthHi },     { "reverbDecay", &P::reverbDecay },
                { "echoTime", &P::echoTime },   { "widthMid", &P::widthMid },
                { "reverbType", &P::reverbType },
            }};
            return f;
        }

        // Panel power, one per rack stage in chain order (the six bool params
        // warpOn .. widthOn, default ON). Off = that whole stage is neutral (the chain
        // reads the flags, CharacterChain::setParams), its knob values untouched. Kept
        // apart from the floats so the preset table's columns never shift; a
        // CharacterParams built from a preset column (fromArray) has every panel ON.
        enum Stage : int { kWarp, kFlutter, kDrive, kEcho, kReverb, kWidth, kNumStages };
        std::array<bool, kNumStages> on { { true, true, true, true, true, true } };

        static const std::array<const char*, kNumStages>& powerIds()
        {
            static const std::array<const char*, kNumStages> ids {{
                "warpOn", "flutterOn", "driveOn", "echoOn", "reverbOn", "widthOn" }};
            return ids;
        }
        bool allOn() const
        {
            for (bool b : on) if (! b) return false;
            return true;
        }

        float&       operator[](int i)       { return this->*(fields()[(size_t) i].member); }
        const float& operator[](int i) const { return this->*(fields()[(size_t) i].member); }

        static CharacterParams fromArray(const float (&v)[kNum])
        {
            CharacterParams p;
            for (int i = 0; i < kNum; ++i)
                p[i] = v[i];
            return p;
        }
    };

    // Everything a print bakes in bar GAIN MATCH: the file, its trim, pitch, the chain
    // and the master levels. Glide, BYPASS and GAIN MATCH are left out, so a restore
    // never flips the A/B pair (GAIN MATCH does bake into a wet print and counts in
    // renderSignature(); the host's own state saves it with the set).
    struct Snapshot
    {
        juce::File file;
        float start = 0.0f, end = 1.0f;      // trim, 0..1 of the file
        float pitch = 0.0f;                  // semitones
        float input = 0.0f, output = 0.0f;   // dB
        float mix = 100.0f;                  // %
        CharacterParams character;

        // Which parts a restore applies. Presets are kCharacter; history is kAll.
        enum Parts : int
        {
            kFile      = 1 << 0,
            kTrim      = 1 << 1,
            kPitch     = 1 << 2,
            kCharacter = 1 << 3,
            kLevels    = 1 << 4,
            kAll       = kFile | kTrim | kPitch | kCharacter | kLevels,
        };
    };
}
