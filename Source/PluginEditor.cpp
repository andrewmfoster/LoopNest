#include "PluginEditor.h"
#include "BinaryData.h"
#include <cmath>

// ===================================================================
// LoopNest — monochrome engineering-blueprint UI.
// White/grey line-art on near-black, with a single teal accent reserved
// for: the PRINT button, the active waveform, and the spin indicator.
// Painted in the mockup's 1672x941 reference space via a global scale
// transform, so all coordinates below are "reference pixels".
// Reference: mockups/design pivot refined.png
// ===================================================================
namespace
{
    // Blueprint palette.
    const juce::Colour cBg        { 0xff060809 };  // near-black panel
    const juce::Colour cBgDeep    { 0xff030506 };  // deeper recess / scope screen
    const juce::Colour cLine      { 0xffeef3f5 };  // primary white line-art
    const juce::Colour cLineSoft  { 0xff97a3a6 };  // secondary grey line / text
    const juce::Colour cLineFaint { 0xff343f41 };  // faint dividers / inner rings
    const juce::Colour cTeal      { 0xff1fe3da };  // ACCENT: print, waveform, spin
    const juce::Colour cTealDim   { 0xff1f9089 };  // accent, muted
    const juce::Colour cDormant   { 0xff5d676a };  // teal → grey when FX bypassed

    // One-shot parameter edit with a proper host gesture bracket (VST3 beginEdit/
    // performEdit/endEdit). A bare setValueNotifyingHost is performEdit with no
    // bracket — Live's undo and touch/latch automation recording misbehave on it.
    inline void setParamGestured(juce::AudioProcessorParameter* p, float v01)
    {
        if (p == nullptr) return;
        p->beginChangeGesture();
        p->setValueNotifyingHost(v01);
        p->endChangeGesture();
    }

    constexpr float kRefW = 1672.0f, kRefH = 941.0f;

    // Six character knobs: column centres (reference px) + shared row geometry.
    // Stack top→bottom: secondary-axis NAME · secondary control · primary knob ·
    // primary NAME. No value read-outs (the knobs speak for themselves).
    // idx0-3 secondary = a small rotary knob; idx4-5 = the echo/reverb filter band.
    // Rows spread evenly across the panel (dividers 606..888, ~11px gaps) so nothing
    // crowds the top and the bottom space is used.
    // PITCH was pulled OUT to the SPIN column (task 4/5). The six character knobs
    // mirror the DSP chain order (WARP · FLUTTER · DRIVE · ECHO · REVERB · WIDTH) — spread evenly
    // across one full-width CHARACTER panel x569..1632 (6 cols, ~177 sp). Centres are
    // 569 + 177.17·(i+0.5), indexed by knob idx (so kKnobCx[i]-indexed loops just work).
    const float kKnobCx[6] = { 658.0f, 835.0f, 1012.0f, 1189.0f, 1366.0f, 1543.0f };
    // Right region shifted UP 90px (icons + MIX vacated the top band) to free a bottom
    // band under the knobs panel for presets + master EQ. Knob-band Y constants carry
    // the −90 so resized() and paint() stay in lock-step from one place.
    constexpr float kSecNameY  = 505.0f;                       // secondary-axis NAME (h26, font18)
    constexpr float kSecKnobY  = 538.0f, kSecKnobSz = 64.0f;   // small secondary knob (idx0-2 rotary cols)
    constexpr float kBandY     = 498.0f;                       // band cells (idx3-5): band at cell top
    constexpr float kTerKnobY  = 540.0f, kTerKnobSz = 56.0f;   // band cells: tertiary rotary
    constexpr float kTerGap    = 8.0f;                         // constant knob→label gap; the compact
                                                               // [knob|gap|label] unit is centred on the
                                                               // column (see terUnitLayout()).
    constexpr float kTerNameY  = 556.0f;                       // tertiary label, RIGHT of the rotary (h24)
    constexpr float kKnobY     = 609.0f, kKnobSz = 126.0f;     // primary knob; bottom = 735
    constexpr float kKnobNameY = 742.0f;                       // primary NAME, under the knob (h32)

    // ---- Factory presets ----------------------------------------------------
    // A preset overwrites ONLY the CHARACTER panel (5 knobs + their 2nd axes), the
    // echo/reverb wet bands, and the master EQ — never pitch, levels, mix or trim.
    // Values are in NATURAL units (applyPreset converts to 0..1); STARTER set, tune
    // by ear. Field order MUST match kPresetParamIds.
    const char* kPresetParamIds[] = {
        "warp", "warpRate", "drive", "driveTone", "flutter", "flutterRate",
        "echo", "reverb", "echoLo", "echoHi", "reverbLo", "reverbHi", "eqLo", "eqHi",
        "width", "widthLo", "widthHi", "reverbDecay", "echoTime", "widthMid" };
    constexpr int kNumPresetParams = (int) (sizeof(kPresetParamIds) / sizeof(kPresetParamIds[0]));

    struct FactoryPreset { const char* name; float v[kNumPresetParams]; };
    // LOW-END RULE: any wet space (echo/reverb) hi-passes its tail so the kick stays
    // dry+punchy — reverbLo/echoLo sit ~0.30-0.38 (≈160-270 Hz on the log axis
    // f=20·1000^x), NOT ~0.05-0.12 (≈28-46 Hz, below the kick = boom). Tuned for drum
    // loops. 12 destinations across 2 axes (degradation amount × flavor), not feature
    // demos: a lo-fi ladder (Vinyl→Dust), an EQ trick (Radio), pure sat (Crush), two
    // kick-safe spaces (Slap/Plate/Hall), a stereo move (Width), an everything-bus (Lo-Fi).
    //  warp warpRate drive driveTone flutter flutterRate echo reverb  echoLo echoHi  rvbLo rvbHi  eqLo  eqHi   width wLo  wHi    rvbDecay echoTime wMid
    const FactoryPreset kFactoryPresets[] = {
        { "Clean",    {  0, 50,  0,   0,   0, 50,   0,  0,  0.00f, 1.00f, 0.00f, 1.00f, 0.00f, 1.00f,   0, 0.00f, 1.00f,  50, 44,   0 } },
        { "Vinyl",    { 15, 40, 18, -10,  35, 55,   0,  8,  0.00f, 1.00f, 0.32f, 0.85f, 0.04f, 0.92f,  15, 0.10f, 1.00f,  30, 44,   0 } },
        { "Tape",     { 25, 35, 35, -20,  30, 50,   0, 12,  0.00f, 1.00f, 0.34f, 0.80f, 0.04f, 0.80f,  15, 0.10f, 1.00f,  35, 44,   0 } },
        { "Cassette", { 35, 30, 22, -15,  60, 60,   0,  8,  0.00f, 1.00f, 0.33f, 0.78f, 0.06f, 0.70f,  25, 0.12f, 1.00f,  30, 44,   0 } },
        { "Dust",     { 45, 55, 55, -25,  25, 50,  10, 10,  0.30f, 0.85f, 0.34f, 0.72f, 0.06f, 0.68f,  18, 0.10f, 1.00f,  40, 50,   0 } },
        { "Radio",    {  0, 50, 40,  10,  10, 50,   0,  0,  0.00f, 1.00f, 0.00f, 1.00f, 0.32f, 0.62f,   0, 0.00f, 1.00f,  50, 44,  15 } },
        { "Crush",    { 10, 50, 75,  20,  10, 50,   0,  0,  0.00f, 1.00f, 0.00f, 1.00f, 0.06f, 0.95f,  10, 0.10f, 1.00f,  50, 44,   0 } },
        { "Slap",     {  0, 50, 15,   0,  15, 50,  55,  0,  0.30f, 0.85f, 0.00f, 1.00f, 0.04f, 0.85f,  30, 0.15f, 1.00f,  50, 25,   0 } },
        { "Plate",    {  0, 50, 10,   0,   0, 50,   0, 40,  0.00f, 1.00f, 0.35f, 0.95f, 0.03f, 1.00f,  25, 0.10f, 1.00f,  35, 44,   0 } },
        { "Hall",     {  0, 50, 10,   0,   0, 50,   0, 65,  0.00f, 1.00f, 0.36f, 0.92f, 0.03f, 1.00f,  40, 0.10f, 1.00f,  85, 44,   0 } },
        { "Width",    {  0, 50,  0,   0,   0, 50,   0,  0,  0.00f, 1.00f, 0.00f, 1.00f, 0.00f, 1.00f,  70, 0.15f, 1.00f,  50, 44, -20 } },
        { "Lo-Fi",    { 30, 45, 40, -15,  40, 55,  15, 18,  0.30f, 0.80f, 0.34f, 0.78f, 0.06f, 0.72f,  20, 0.10f, 1.00f,  40, 40,   0 } },
    };
    constexpr int kNumFactoryPresets = (int) (sizeof(kFactoryPresets) / sizeof(kFactoryPresets[0]));

    // Bundled JetBrains Mono (SIL OFL) — weight 0=regular, 1=bold, 2=extra-bold.
    juce::Typeface::Ptr jbType (int weight)
    {
        static juce::Typeface::Ptr reg = juce::Typeface::createSystemTypefaceFor(
            BinaryData::JetBrainsMonoRegular_ttf, (size_t) BinaryData::JetBrainsMonoRegular_ttfSize);
        static juce::Typeface::Ptr bold = juce::Typeface::createSystemTypefaceFor(
            BinaryData::JetBrainsMonoBold_ttf, (size_t) BinaryData::JetBrainsMonoBold_ttfSize);
        static juce::Typeface::Ptr xbold = juce::Typeface::createSystemTypefaceFor(
            BinaryData::JetBrainsMonoExtraBold_ttf, (size_t) BinaryData::JetBrainsMonoExtraBold_ttfSize);
        return weight >= 2 ? xbold : (weight == 1 ? bold : reg);
    }

    juce::Font mono (float h, bool bold = false)
    {
        return juce::Font(juce::FontOptions{}.withTypeface(jbType(bold ? 1 : 0)).withHeight(h));
    }

    // Extra-bold cut (kept available; the wordmark is now hand-drawn vectors).
    [[maybe_unused]] juce::Font monoX (float h)
    {
        return juce::Font(juce::FontOptions{}.withTypeface(jbType(2)).withHeight(h));
    }

    // Band-cell tertiary [knob | gap | label] geometry (idx3-5). Laid out as a
    // COMPACT unit centred on the column with a CONSTANT knob→label gap: the label
    // is left-justified (not boxed/centred), so every cell matches WIDTH/CENTER's
    // spacing regardless of label length (TIME/DECAY no longer drift wider). Shared
    // by resized() (knob bounds) and paint() (label rect) so they stay in lock-step.
    void terUnitLayout (int idx, const char* label, juce::Rectangle<float>& knobOut,
                        juce::Rectangle<float>& labelOut)
    {
        const float textW   = mono(18.0f, false).getStringWidthFloat(label);
        const float unitW   = kTerKnobSz + kTerGap + textW;
        const float unitLeft = kKnobCx[idx] - unitW * 0.5f;
        knobOut  = { unitLeft, kTerKnobY, kTerKnobSz, kTerKnobSz };
        labelOut = { unitLeft + kTerKnobSz + kTerGap, kTerNameY, textW + 2.0f, 24.0f };
    }

    juce::String fmtTime(double seconds)
    {
        if (seconds < 0.0) seconds = 0.0;
        const int totalDs = juce::roundToInt(seconds * 10.0);   // deciseconds
        const int mm = totalDs / 600;
        const int ss = (totalDs / 10) % 60;
        const int d  = totalDs % 10;
        return juce::String(mm) + ":" + juce::String(ss).paddedLeft('0', 2)
             + "." + juce::String(d);
    }

    // Machined screw head — a blueprint hardware detail at panel/key corners.
    void drawScrew(juce::Graphics& g, float cx, float cy, float r, juce::Colour col)
    {
        g.setColour(col);
        g.drawEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f, 1.2f);
        const float s = r * 0.55f;
        g.drawLine(cx - s, cy, cx + s, cy, 1.1f);
        g.drawLine(cx, cy - s, cx, cy + s, 1.1f);
    }

    // The LoopNest mark: nested concentric hexagons (a hive cell / "nest-in-nest" —
    // the nesting pun + recursive containment), with the innermost hex filled teal as
    // the single focal accent (the loop nested at the core; absorbs the retired
    // spins-left dots into one point). Flat-top orientation → reads as a honeycomb cell.
    // Drawn in line-art around centre (cx, cy); cy aligns with the wordmark cap band.
    void drawLogo(juce::Graphics& g, float cx, float topY)
    {
        const float pi = juce::MathConstants<float>::pi;
        const float cy = topY + 46.0f;                 // vertical centre (aligns to wordmark)

        // Flat-top regular hexagon (vertices at the left/right points) of centre→vertex
        // radius R around (cx, cy): width = 2R, height = R·√3. Sharp mitered corners
        // keep the crisp blueprint line-work.
        auto hexPath = [&](float R)
        {
            juce::Path h;
            for (int i = 0; i < 6; ++i)
            {
                const float a = pi / 3.0f * (float) i;  // 0,60,…,300° → flat top + bottom edges
                const float px = cx + std::cos(a) * R;
                const float py = cy + std::sin(a) * R;
                if (i == 0) h.startNewSubPath(px, py); else h.lineTo(px, py);
            }
            h.closeSubPath();
            return h;
        };
        const juce::PathStrokeType hexStroke(2.6f, juce::PathStrokeType::mitered,
                                             juce::PathStrokeType::rounded);

        // Three nested rings: bold outer shell → soft middle → teal core.
        g.setColour(cLine);
        g.strokePath(hexPath(33.0f), hexStroke);
        g.setColour(cLineSoft);
        g.strokePath(hexPath(22.0f), juce::PathStrokeType(1.6f, juce::PathStrokeType::mitered,
                                                          juce::PathStrokeType::rounded));

        // Innermost hex = the teal focal core, with a faint glow fill underneath.
        const auto core = hexPath(11.0f);
        g.setColour(cTeal.withAlpha(0.20f));
        g.fillPath(core);
        g.setColour(cTeal);
        g.strokePath(core, juce::PathStrokeType(2.0f, juce::PathStrokeType::mitered,
                                                juce::PathStrokeType::rounded));
    }

    // ---- Hand-drawn "LOOPNEST" wordmark --------------------------------------
    // Chamfered-corner tech letterforms (L O P N E S T) built as vector paths in a
    // 100-wide × 140-tall cell, filled with a vertical brushed-metal gradient so
    // they read as bevelled steel like the reference logo. No font dependency.
    namespace wm
    {
        constexpr float H = 140.0f, S = 22.0f, C = 8.0f, Cin = 5.0f;   // S = stroke weight (thinner → closer to ref)

        struct Glyph { std::vector<juce::Path> parts; float width; };

        // A rectangle whose four corners are cut at 45° (per-corner chamfer size).
        inline void chamRect(juce::Path& p, float x0, float y0, float x1, float y1,
                             float tl, float tr, float br, float bl)
        {
            p.startNewSubPath(x0 + tl, y0);
            p.lineTo(x1 - tr, y0);  p.lineTo(x1, y0 + tr);
            p.lineTo(x1, y1 - br);  p.lineTo(x1 - br, y1);
            p.lineTo(x0 + bl, y1);  p.lineTo(x0, y1 - bl);
            p.lineTo(x0, y0 + tl);  p.closeSubPath();
        }

        // A thick straight bar from A to B (for the diagonal arms of the K).
        inline void thickSeg(juce::Path& p, float ax, float ay, float bx, float by, float hw)
        {
            const float dx = bx - ax, dy = by - ay, L = std::sqrt(dx * dx + dy * dy);
            const float nx = -dy / L * hw, ny = dx / L * hw;
            p.startNewSubPath(ax + nx, ay + ny);
            p.lineTo(bx + nx, by + ny);
            p.lineTo(bx - nx, by - ny);
            p.lineTo(ax - nx, ay - ny);
            p.closeSubPath();
        }

        inline Glyph build(char ch)
        {
            Glyph gl; juce::Path p;
            switch (ch)
            {
                case 'O':
                {
                    chamRect(p, 0, 0, 104, H, C, C, C, C);                 // outer ring
                    chamRect(p, S, S, 104 - S, H - S, Cin, Cin, Cin, Cin); // counter
                    p.setUsingNonZeroWinding(false);                      // → punched hole
                    gl.parts = { p }; gl.width = 104.0f; break;
                }
                case 'L':
                {
                    p.startNewSubPath(C, 0);
                    p.lineTo(S, 0);          p.lineTo(S, H - S);
                    p.lineTo(86 - C, H - S); p.lineTo(86, H - S + C);
                    p.lineTo(86, H - C);     p.lineTo(86 - C, H);
                    p.lineTo(C, H);          p.lineTo(0, H - C);
                    p.lineTo(0, C);          p.closeSubPath();
                    gl.parts = { p }; gl.width = 86.0f; break;
                }
                case 'P':
                {
                    p.startNewSubPath(C, 0);
                    p.lineTo(98 - C, 0);  p.lineTo(98, C);
                    p.lineTo(98, 86 - C); p.lineTo(98 - C, 86);
                    p.lineTo(S, 86);      p.lineTo(S, H);
                    p.lineTo(C, H);       p.lineTo(0, H - C);
                    p.lineTo(0, C);       p.closeSubPath();
                    chamRect(p, S, S, 98 - S, 86 - S, Cin, Cin, Cin, Cin); // bowl counter
                    p.setUsingNonZeroWinding(false);
                    gl.parts = { p }; gl.width = 98.0f; break;
                }
                case 'C':
                {
                    p.startNewSubPath(C, 0);
                    p.lineTo(104 - C, 0); p.lineTo(104, C);
                    p.lineTo(104, S);     p.lineTo(S, S);       // mouth + throat top
                    p.lineTo(S, H - S);   p.lineTo(104, H - S); // throat + mouth bottom
                    p.lineTo(104, H - C); p.lineTo(104 - C, H);
                    p.lineTo(C, H);       p.lineTo(0, H - C);
                    p.lineTo(0, C);       p.closeSubPath();
                    gl.parts = { p }; gl.width = 104.0f; break;
                }
                case 'K':
                {
                    juce::Path stem, up, lo;
                    chamRect(stem, 0, 0, S, H, C, 0, 0, C);            // stem (left corners cut)
                    thickSeg(up, S * 0.45f, H * 0.52f, 100, 0,   11);  // upper arm
                    thickSeg(lo, S * 0.45f, H * 0.48f, 100, H,   11);  // lower leg
                    gl.parts = { stem, up, lo }; gl.width = 100.0f; break;
                }
                case 'N':
                {
                    // Separate paths (like K) — the diagonal quad winds opposite the
                    // chamfered stems, so a single shared path punches holes at the crossings.
                    juce::Path stem, rstem, diag;
                    chamRect(stem,  0, 0, S, H, C, 0, 0, C);             // left stem
                    chamRect(rstem, 100 - S, 0, 100, H, 0, C, C, 0);     // right stem
                    thickSeg(diag,  S * 0.5f, 0, 100 - S * 0.5f, H, 11); // top-left → bottom-right diagonal
                    gl.parts = { stem, rstem, diag }; gl.width = 100.0f; break;
                }
                case 'E':
                {
                    chamRect(p, 0, 0, S, H, C, 0, 0, C);                       // left stem
                    chamRect(p, 0, 0, 90, S, C, C, Cin, Cin);                  // top arm
                    chamRect(p, 0, (H - S) * 0.5f, 78, (H + S) * 0.5f,
                             Cin, Cin, Cin, Cin);                             // middle arm (shorter)
                    chamRect(p, 0, H - S, 90, H, Cin, Cin, C, C);             // bottom arm
                    gl.parts = { p }; gl.width = 90.0f; break;
                }
                case 'S':
                {
                    chamRect(p, 0, 0, 96, S, C, C, Cin, Cin);                  // top bar
                    chamRect(p, 0, S * 0.5f, S, (H + S) * 0.5f, C, Cin, Cin, 0); // upper-left riser
                    chamRect(p, 0, (H - S) * 0.5f, 96, (H + S) * 0.5f,
                             Cin, Cin, Cin, Cin);                            // middle bar
                    chamRect(p, 96 - S, (H - S) * 0.5f, 96, H - S * 0.5f,
                             0, Cin, Cin, C);                                // lower-right riser
                    chamRect(p, 0, H - S, 96, H, Cin, Cin, C, C);             // bottom bar
                    gl.parts = { p }; gl.width = 96.0f; break;
                }
                case 'T':
                {
                    chamRect(p, 0, 0, 96, S, C, C, Cin, Cin);                  // top bar
                    chamRect(p, (96 - S) * 0.5f, S * 0.5f, (96 + S) * 0.5f, H,
                             Cin, Cin, C, C);                                 // centre stem
                    gl.parts = { p }; gl.width = 96.0f; break;
                }
                default: gl.width = 60.0f; break;
            }
            return gl;
        }
    } // namespace wm

    // Visual advance width of the wordmark (excludes the trailing inter-glyph gap),
    // so the logo + wordmark can be packed as a compact unit and centred.
    float wordmarkWidth(const char* text, float capH)
    {
        const float scale = capH / wm::H;
        float x = 0.0f;
        for (const char* c = text; *c != '\0'; ++c)
        {
            if (*c == ' ') { x += 50.0f * scale; continue; }
            x += (wm::build(*c).width + 13.0f) * scale;
        }
        return x - 13.0f * scale;   // drop the trailing gap after the last glyph
    }

    // Draw "LOOPNEST" starting at (startX, topY) with the given cap height.
    void drawWordmark(juce::Graphics& g, float startX, float topY, float capH)
    {
        const float scale = capH / wm::H;
        float x = startX;
        for (const char* c = "LOOPNEST"; *c != '\0'; ++c)
        {
            if (*c == ' ') { x += 50.0f * scale; continue; }
            const auto gl = wm::build(*c);

            // Drop shadow, offset down-right, behind the glyph.
            const auto shadowXf = juce::AffineTransform::scale(scale).translated(x + 2.5f, topY + 3.0f);
            g.setColour(juce::Colour(0x80000000));
            for (auto part : gl.parts) { auto s = part; s.applyTransform(shadowXf); g.fillPath(s); }

            // Brushed-metal vertical gradient → bevelled-steel read.
            juce::ColourGradient grad(juce::Colour(0xfff5f7f8), x, topY,
                                      juce::Colour(0xffe7eaec), x, topY + capH, false);
            grad.addColour(0.18, juce::Colour(0xffd2d7da));
            grad.addColour(0.50, juce::Colour(0xff7d848a));
            grad.addColour(0.82, juce::Colour(0xffb9bfc3));

            const auto xf = juce::AffineTransform::scale(scale).translated(x, topY);
            g.setGradientFill(grad);
            for (auto part : gl.parts) { auto s = part; s.applyTransform(xf); g.fillPath(s); }

            x += (gl.width + 13.0f) * scale;
        }
    }
}

// ============================= TapeLookAndFeel =============================
// Blueprint rotary: a thin white ring, an arc of engraved ticks, and a clean
// pointer line. No fills, no gradients — line-art only.

TapeLookAndFeel::TapeLookAndFeel()
{
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

// Blueprint popup item: centred mono text, teal highlight bar, teal tick for the
// current preset. Used by the preset dropdown (the only PopupMenu in the editor).
void TapeLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
    bool isSeparator, bool isActive, bool isHighlighted, bool isTicked, bool /*hasSubMenu*/,
    const juce::String& text, const juce::String& /*shortcut*/, const juce::Drawable* /*icon*/,
    const juce::Colour* /*textColour*/)
{
    if (isSeparator)
    {
        auto r = area.toFloat().reduced(8.0f, 0.0f);
        g.setColour(cLineSoft.withAlpha(0.3f));
        g.drawHorizontalLine(area.getCentreY(), r.getX(), r.getRight());
        return;
    }

    auto r = area.reduced(2);
    if (isHighlighted && isActive)
    {
        g.setColour(cTeal.withAlpha(0.18f));
        g.fillRoundedRectangle(r.toFloat(), 3.0f);
    }

    g.setColour((isTicked ? cTeal : cLine).withAlpha(isActive ? 0.95f : 0.4f));
    g.setFont(getPopupMenuFont());
    g.drawText(text, r, juce::Justification::centred, true);
}

void TapeLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                                       float pos, float a0, float a1, juce::Slider& slider)
{
    auto area = juce::Rectangle<int>(x, y, w, h).toFloat();
    const float cx = area.getCentreX();
    const float cy = area.getCentreY();
    const float R  = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;

    auto onArc = [&](float ang, float rad)
    {
        return juce::Point<float>(cx + std::sin(ang) * rad, cy - std::cos(ang) * rad);
    };

    // Faint radial ticks following the rotary sweep — engraved dial feel.
    g.setColour(cLineSoft.withAlpha(0.55f));
    const int nTicks = 21;
    for (int i = 0; i <= nTicks; ++i)
    {
        const float a = a0 + (a1 - a0) * (float) i / (float) nTicks;
        auto inner = onArc(a, R * 0.80f);
        auto outer = onArc(a, R * 0.93f);
        g.drawLine(inner.x, inner.y, outer.x, outer.y, 1.0f);
    }

    // Outer thin ring, main ring, faint inner ring.
    g.setColour(cLineSoft.withAlpha(0.7f));
    g.drawEllipse(cx - R * 0.74f, cy - R * 0.74f, R * 1.48f, R * 1.48f, 1.0f);
    g.setColour(cLine);
    g.drawEllipse(cx - R * 0.66f, cy - R * 0.66f, R * 1.32f, R * 1.32f, 1.8f);
    g.setColour(cLineFaint);
    g.drawEllipse(cx - R * 0.55f, cy - R * 0.55f, R * 1.10f, R * 1.10f, 1.0f);

    // Teal value arc: a track from the knob's neutral point (0 dB / 0 st / 0%) to
    // the current value. valueToProportionOfLength(0) gives the neutral position
    // generically — far-left for 0..100% knobs, centre for bipolar ones.
    const float zeroNorm = juce::jlimit(0.0f, 1.0f, (float) slider.valueToProportionOfLength(0.0));
    const float aCur  = a0 + pos      * (a1 - a0);
    const float aZero = a0 + zeroNorm * (a1 - a0);
    if (std::abs(aCur - aZero) > 0.012f)
    {
        const float rArc = R * 0.83f;
        juce::Path arc;
        arc.addCentredArc(cx, cy, rArc, rArc, 0.0f,
                          juce::jmin(aZero, aCur), juce::jmax(aZero, aCur), true);
        const juce::Colour ac = fxDormant ? cDormant : cTeal;
        g.setColour(ac.withAlpha(0.20f));   // soft glow underlay
        g.strokePath(arc, juce::PathStrokeType(R * 0.17f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
        g.setColour(ac);                     // crisp value line (grey when bypassed)
        g.strokePath(arc, juce::PathStrokeType(R * 0.06f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }

    // Bold pointer + centre cross.
    const float a = a0 + pos * (a1 - a0);
    auto base = onArc(a, R * 0.20f);
    auto tip  = onArc(a, R * 0.64f);
    g.setColour(cLine);
    g.drawLine(base.x, base.y, tip.x, tip.y, juce::jmax(2.6f, R * 0.075f));
    const float cs = R * 0.085f;
    g.drawLine(cx - cs, cy, cx + cs, cy, 1.1f);
    g.drawLine(cx, cy - cs, cx, cy + cs, 1.1f);
}

// ================================ ValueChip ================================
// Editable knob read-out. Click selects (teal outline); typing a number or a
// double-click opens an inline editor; Return commits, Delete resets to default,
// Esc / click-away clears focus.

ValueChip::ValueChip()
{
    setEditable(false, false, false);  // editing is driven manually; blur commits the edit
    setWantsKeyboardFocus(true);
    setJustificationType(juce::Justification::centred);
    onTextChange = [this] { if (onCommitText) onCommitText(getText()); };
}

void ValueChip::mouseDown(const juce::MouseEvent&)
{
    dragging = false;
    if (linkedSlider != nullptr)
        dragStartProp = linkedSlider->valueToProportionOfLength(linkedSlider->getValue());
    if (! hasKeyboardFocus(false)) grabKeyboardFocus();   // → focusGained → teal outline
}

void ValueChip::mouseDrag(const juce::MouseEvent& e)
{
    // Ableton-style: click-hold-drag up/down nudges the value (a small threshold
    // keeps a plain click as a select). Shift = fine. The slider's attachment
    // pushes the change to the param, so the knob + arc + read-out all track.
    if (linkedSlider == nullptr || isBeingEdited()) return;
    const int dy = e.getDistanceFromDragStartY();
    if (! dragging)
    {
        if (std::abs(dy) <= 3) return;
        dragging = true;
        // Pin the OS cursor in place (the standard JUCE knob move): movement keeps
        // generating deltas without the pointer travelling off across the screen.
        e.source.enableUnboundedMouseMovement(true);
    }
    const double pxFull = e.mods.isShiftDown() ? 900.0 : 220.0;   // px for a full-range sweep
    const double prop = juce::jlimit(0.0, 1.0, dragStartProp - (double) dy / pxFull);
    linkedSlider->setValue(linkedSlider->proportionOfLengthToValue(prop),
                           juce::sendNotificationSync);
}

void ValueChip::mouseUp(const juce::MouseEvent& e)
{
    // Releasing unbounded movement warps the cursor back to where the drag began.
    if (dragging) { dragging = false; e.source.enableUnboundedMouseMovement(false); }
}

void ValueChip::mouseDoubleClick(const juce::MouseEvent&)
{
    startEditing({});
}

void ValueChip::startEditing(const juce::String& initial)
{
    showEditor();
    if (auto* ed = getCurrentTextEditor())
    {
        ed->setJustification(juce::Justification::centred);   // sit where the at-rest value sits
        if (initial.isNotEmpty()) { ed->setText(initial, false); ed->moveCaretToEnd(); }
        else                        ed->selectAll();
    }
}

bool ValueChip::keyPressed(const juce::KeyPress& key)
{
    if (isBeingEdited()) return juce::Label::keyPressed(key);   // the editor owns the keys

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        if (onResetDefault) onResetDefault();
        return true;
    }
    if (key == juce::KeyPress::escapeKey) { giveAwayKeyboardFocus(); return true; }
    if (key == juce::KeyPress::returnKey) { startEditing({});        return true; }

    // A digit / sign / point begins editing, seeded with the typed character.
    const auto ch = key.getTextCharacter();
    if (ch != 0 && (juce::CharacterFunctions::isDigit((juce::juce_wchar) ch)
                    || ch == '-' || ch == '+' || ch == '.'))
    {
        startEditing(juce::String::charToString(ch));
        return true;
    }
    return juce::Label::keyPressed(key);
}

void ValueChip::focusGained(FocusChangeType) { selected = true;  repaint(); }
void ValueChip::focusLost  (FocusChangeType) { selected = false; repaint(); }

void ValueChip::paint(juce::Graphics& g)
{
    juce::Label::paint(g);   // background + text + the faint resting outline
    if (selected && ! isBeingEdited())
    {
        g.setColour(cTeal);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 4.0f, 1.8f);
    }
}

// ============================= TransportButton =============================
// Blueprint transport keys: thin outline boxes. PLAY/LOOP are white outlines;
// PRINT is the one teal-filled primary control. LOOP brightens when engaged.

TransportButton::TransportButton(Kind k) : juce::Button("transport"), kind(k)
{
    setClickingTogglesState(k == Kind::loop);
}

void IconButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    auto b = getLocalBounds().toFloat().reduced(1.5f);
    const float rad = b.getHeight() * 0.18f;

    // White at rest, teal on hover, faint when disabled (dimmed mid-extraction).
    const juce::Colour col = isEnabled() ? (highlighted ? cTeal : cLine) : cLineFaint;
    const float frameAlpha = isEnabled() ? (highlighted ? 0.9f : 0.55f) : 0.3f;

    if (down && isEnabled())
    {
        g.setColour(cTeal.withAlpha(0.10f));
        g.fillRoundedRectangle(b, rad);
    }
    g.setColour(col.withAlpha(frameAlpha));
    g.drawRoundedRectangle(b, rad, 1.6f);

    // Tight inset so the glyph fills the frame at roughly the size of the folder
    // glyph that used to live in the waveform header band.
    auto gz = b.reduced(b.getWidth() * 0.16f, b.getHeight() * 0.22f);
    g.setColour(col);
    const float sw = 1.8f;

    if (kind == Kind::folder)
    {
        // Matches the folder glyph in the waveform header band.
        juce::Path p;
        p.startNewSubPath(gz.getX(), gz.getBottom());
        p.lineTo(gz.getX(), gz.getY() + gz.getHeight() * 0.25f);
        p.lineTo(gz.getX() + gz.getWidth() * 0.45f, gz.getY() + gz.getHeight() * 0.25f);
        p.lineTo(gz.getX() + gz.getWidth() * 0.6f,  gz.getY());
        p.lineTo(gz.getRight(), gz.getY());
        p.lineTo(gz.getRight(), gz.getBottom());
        p.closeSubPath();
        g.strokePath(p, juce::PathStrokeType(sw));
    }
    else // funnel: wide mouth tapering to a short spout — "filter down to drums".
    {
        const float cx = gz.getCentreX();
        const float spoutHalf = gz.getWidth() * 0.10f;
        const float neckY = gz.getY() + gz.getHeight() * 0.55f;
        juce::Path p;
        p.startNewSubPath(gz.getX(), gz.getY());
        p.lineTo(gz.getRight(),      gz.getY());
        p.lineTo(cx + spoutHalf,     neckY);
        p.lineTo(cx + spoutHalf,     gz.getBottom());
        p.lineTo(cx - spoutHalf,     gz.getBottom());
        p.lineTo(cx - spoutHalf,     neckY);
        p.closeSubPath();
        g.strokePath(p, juce::PathStrokeType(sw, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
    }
}

void MonitorButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    auto b = getLocalBounds().toFloat().reduced(1.0f);
    const float r = b.getHeight() * 0.5f;                 // full pill radius
    const bool  engaged = (kind != Kind::clear) && getToggleState();

    // Corner rounding makes the split pill: gainMatch rounds its LEFT, bypass its
    // RIGHT (their flat edges butt together as the divider); clear is a full pill.
    bool tl = true, tr = true, bl = true, br = true;
    if (kind == Kind::gainMatch) { tr = false; br = false; }
    if (kind == Kind::bypass)    { tl = false; bl = false; }

    juce::Path shape;
    shape.addRoundedRectangle(b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                              r, r, tl, tr, bl, br);

    // Pressed wash — the same faint grey press feedback as the transport keys (no
    // solid fill on engage; the engaged state reads as a teal rim, like LOOP).
    if (down)
    {
        g.setColour(cLine.withAlpha(0.06f));
        g.fillPath(shape);
    }

    // Border: engaged → full teal rim (like LOOP); a CLEAR press pulses the same rim
    // in via clickGlow then fades it out; otherwise the dim strip border (cLineFaint,
    // like EQ/PRESET) brightening on hover.
    const float teal01 = engaged ? 1.0f : clickGlow;
    const juce::Colour baseBorder = (highlighted && ! engaged) ? cLineSoft.withAlpha(0.9f)
                                                               : cLineFaint;
    g.setColour(baseBorder.interpolatedWith(cTeal, teal01));
    g.strokePath(shape, juce::PathStrokeType(1.3f + 1.1f * teal01));

    const juce::Colour fg = engaged ? cLine : cLine.withAlpha(highlighted ? 1.0f : 0.85f);
    g.setColour(fg);

    // Content centres in the FULL bounds — at mid-height the pill border sits at the
    // bounds edges regardless of the corner caps, so centred = equidistant L/R.
    if (kind == Kind::gainMatch)
    {
        // Two faders at different levels — the "match the gains" glyph.
        auto gz = b.reduced(b.getWidth() * 0.30f, b.getHeight() * 0.27f);
        const float sw = 1.6f, x1 = gz.getX(), x2 = gz.getRight();
        const float cw = gz.getWidth() * 0.30f;
        g.drawLine(x1, gz.getY(), x1, gz.getBottom(), sw);
        g.drawLine(x2, gz.getY(), x2, gz.getBottom(), sw);
        auto cap = [&](float cx, float cy)
        { g.fillRect(juce::Rectangle<float>(cx - cw, cy - 2.0f, cw * 2.0f, 4.0f)); };
        cap(x1, gz.getY() + gz.getHeight() * 0.62f);
        cap(x2, gz.getY() + gz.getHeight() * 0.34f);
    }
    else
    {
        g.setFont(mono(b.getHeight() * 0.34f, true));
        g.drawText(kind == Kind::bypass ? "BYPASS" : "CLEAR",
                   b, juce::Justification::centred, false);
    }
}

void MonitorButton::clicked()
{
    // CLEAR has no toggle state, so flash a brief teal rim as press confirmation.
    if (kind == Kind::clear)
    {
        clickGlow = 1.0f;
        startTimerHz(60);
        repaint();
    }
}

void MonitorButton::timerCallback()
{
    clickGlow -= 0.08f;                   // ~200 ms fade
    if (clickGlow <= 0.0f) { clickGlow = 0.0f; stopTimer(); }
    repaint();
}

void TransportButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    auto b = getLocalBounds().toFloat().reduced(1.5f);
    const float rad = b.getHeight() * 0.12f;
    const bool isPrint = (kind == Kind::print);
    const bool lit = (kind == Kind::loop) && getToggleState();
    // "Active" = engaged control: LOOP toggled on, or PLAY currently running.
    const bool active = lit || (kind == Kind::play && showPause);
    const float h = b.getHeight();

    if (isPrint)
    {
        // HATCH reads like the PLAY key (thin outline); once it arms, the teal fill
        // FADES IN (armGlow 0→1) so the drag-source "payoff" state glows on. At rest
        // (armGlow 0) it is visually identical to PLAY.
        if (down)
        {
            g.setColour(cLine.withAlpha(0.06f));
            g.fillRoundedRectangle(b, rad);
        }
        if (armGlow > 0.0f)
        {
            auto fill = cTeal;
            if (down)      fill = fill.darker(0.18f);
            else if (highlighted) fill = fill.brighter(0.10f);
            g.setColour(fill.withAlpha(armGlow));
            g.fillRoundedRectangle(b, rad);
        }
        // Border: white like PLAY, easing to the armed teal-rim treatment as it fills.
        g.setColour(cLine.withAlpha(highlighted ? 0.95f : 0.8f)
                        .interpolatedWith(cLine.withAlpha(0.85f), armGlow));
        g.drawRoundedRectangle(b, rad, 1.6f);
    }
    else
    {
        if (down)
        {
            g.setColour(cLine.withAlpha(0.06f));
            g.fillRoundedRectangle(b, rad);
        }
        // Active control gets the teal border treatment (no grey fill).
        g.setColour(active ? cTeal : cLine.withAlpha(highlighted ? 0.95f : 0.8f));
        g.drawRoundedRectangle(b, rad, active ? 2.4f : 1.6f);
    }

    // Glyph + label drawn as ONE centred group (icon directly left of the text,
    // the pair centred across the key). Every key has a glyph — including PRINT.
    // PRINT glyph/label rides the fill: cLine while it reads as PLAY, easing to dark
    // (cBgDeep) once the teal fill has glowed in.
    const auto fg = isPrint ? cLine.withAlpha(0.92f).interpolatedWith(cBgDeep, armGlow)
                            : cLine.withAlpha(active ? 1.0f : 0.92f);
    const juce::Font labelFont = mono(h * 0.38f, true);
    const juce::String label = isPrint ? (armed ? "DRAG" : "HATCH") : getButtonText();
    const float textW  = juce::GlyphArrangement::getStringWidth(labelFont, label);
    const float glyphW = h * 0.44f;   // box reserved for the icon
    const float gap    = h * 0.16f;   // icon→text spacing
    const float startX = b.getCentreX() - (glyphW + gap + textW) * 0.5f;
    const float gx = startX + glyphW * 0.5f;   // glyph centre
    const float gy = b.getCentreY();

    g.setColour(fg);
    if (kind == Kind::play)
    {
        if (showPause)
        {
            // Two bars — transport is running; this key pauses it.
            const float bw = h * 0.11f, bh = h * 0.30f, sp = h * 0.085f;
            g.fillRect(gx - sp - bw, gy - bh * 0.5f, bw, bh);
            g.fillRect(gx + sp,      gy - bh * 0.5f, bw, bh);
        }
        else
        {
            const float s = h * 0.26f;
            juce::Path tri;
            tri.addTriangle(gx - s * 0.5f, gy - s, gx - s * 0.5f, gy + s, gx + s * 0.85f, gy);
            g.fillPath(tri);
        }
    }
    else if (kind == Kind::loop)
    {
        // Circular "loop" arrow: an almost-closed ring with a gap near the top and a
        // tangential arrowhead at its leading end (reads as continuous rotation).
        const float r  = h * 0.22f;
        const float tp = juce::MathConstants<float>::twoPi;
        auto onC = [&](float a) { return juce::Point<float>(gx + std::sin(a) * r, gy - std::cos(a) * r); };

        const float a0 = 0.55f, a1 = tp - 0.02f;   // leave a gap near the top of the ring
        juce::Path arc;
        arc.addCentredArc(gx, gy, r, r, 0.0f, a0, a1, true);
        g.strokePath(arc, juce::PathStrokeType(h * 0.06f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

        // Arrowhead at the a1 end, oriented along the arc tangent there.
        const float hd  = h * 0.12f;
        auto tip  = onC(a1);
        auto tail = onC(a1 - 0.28f);
        auto dir  = tip - tail;
        const float L = juce::jmax(0.001f, dir.getDistanceFromOrigin());
        dir = dir / L;
        const juce::Point<float> nrm(-dir.y, dir.x);
        juce::Path head;
        head.addTriangle(tip.x + dir.x * hd,         tip.y + dir.y * hd,
                         tip.x + nrm.x * hd * 0.62f, tip.y + nrm.y * hd * 0.62f,
                         tip.x - nrm.x * hd * 0.62f, tip.y - nrm.y * hd * 0.62f);
        g.fillPath(head);
    }
    else if (armed)
    {
        // DRAG: arrow pointing right ("drag this loop out to a track").
        const float s = h * 0.24f;
        g.drawLine(gx - s, gy, gx + s * 0.45f, gy, h * 0.06f);
        juce::Path head;
        head.addTriangle(gx + s * 0.7f, gy, gx - s * 0.05f, gy - s * 0.55f, gx - s * 0.05f, gy + s * 0.55f);
        g.fillPath(head);
    }
    else
    {
        // HATCH: arrow down onto a baseline ("hatch the take out to disk").
        const float s = h * 0.24f;
        g.drawLine(gx, gy - s, gx, gy + s * 0.4f, h * 0.06f);
        juce::Path head;
        head.addTriangle(gx - s * 0.55f, gy + s * 0.1f, gx + s * 0.55f, gy + s * 0.1f, gx, gy + s * 0.7f);
        g.fillPath(head);
        g.drawLine(gx - s * 0.7f, gy + s * 0.9f, gx + s * 0.7f, gy + s * 0.9f, h * 0.06f);
    }

    // Label — monospace, left-aligned right after the glyph, vertically centred.
    g.setColour(fg);
    g.setFont(labelFont);
    g.drawText(label, juce::Rectangle<float>(startX + glyphW + gap, b.getY(),
                                             textW + h * 0.25f, h),
               juce::Justification::centredLeft, false);
}

void TransportButton::setArmed(bool shouldBeArmed)
{
    if (armed == shouldBeArmed) return;
    armed = shouldBeArmed;
    setMouseCursor(armed ? juce::MouseCursor::DraggingHandCursor
                         : juce::MouseCursor::NormalCursor);
    // Tween the teal fill in (arming) or out (a new spin/render invalidated it).
    startTimerHz(30);
    repaint();
}

void TransportButton::timerCallback()
{
    const float target = armed ? 1.0f : 0.0f;
    const float step    = 0.10f;                 // ~0.33s glow @30Hz
    if (armGlow < target)      armGlow = juce::jmin(target, armGlow + step);
    else if (armGlow > target) armGlow = juce::jmax(target, armGlow - step);
    else { stopTimer(); return; }
    repaint();
}

void TransportButton::mouseDown(const juce::MouseEvent& e)
{
    dragLaunched = false;
    juce::Button::mouseDown(e);
}

void TransportButton::mouseDrag(const juce::MouseEvent& e)
{
    // Once armed, dragging off PRINT performs the external file drag into Ableton.
    if (kind == Kind::print && armed && ! dragLaunched && getFile != nullptr)
    {
        const auto file = getFile();
        if (file.existsAsFile())
        {
            dragLaunched = true;
            juce::DragAndDropContainer::performExternalDragDropOfFiles(
                { file.getFullPathName() }, false, this, nullptr);
            return;
        }
    }
    juce::Button::mouseDrag(e);
}

void TransportButton::mouseUp(const juce::MouseEvent& e)
{
    if (dragLaunched) { dragLaunched = false; return; }  // swallow the click that began a drag
    juce::Button::mouseUp(e);
}

// ================================= ReelBay =================================
// Line-art cassette: two reels rendered as concentric rings with rotating
// prong holes (to read as the spin control), a tape path, and a capstan.

ReelBay::ReelBay()  { setOpaque(true); startTimerHz(30); }
ReelBay::~ReelBay() { stopTimer(); }

void ReelBay::timerCallback()
{
    // Reels revolve only while the tape is playing; they hold position when paused.
    if (isPlaying && ! isPlaying()) return;

    const float twoPi = juce::MathConstants<float>::twoPi;
    phase  += 0.045f;
    phase2 += 0.045f * 0.85f;
    if (phase  > twoPi) phase  -= twoPi;
    if (phase2 > twoPi) phase2 -= twoPi;
    repaint();
}

void ReelBay::mouseUp(const juce::MouseEvent&)
{
    if (onSpin && (! canSpin || canSpin()))
        onSpin();
}

void ReelBay::drawReel(juce::Graphics& g, juce::Point<float> c, float r, float angle) const
{
    // r is the outer rim radius (=91 in the 459x308 design space).
    const float twoPi = juce::MathConstants<float>::twoPi;
    auto ring = [&](float rad, juce::Colour col, float wdt)
    {
        g.setColour(col);
        g.drawEllipse(c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f, wdt);
    };
    auto radial = [&](float a, float r0, float r1, float wdt, juce::Colour col)
    {
        g.setColour(col);
        g.drawLine(c.x + std::sin(a) * r0, c.y - std::cos(a) * r0,
                   c.x + std::sin(a) * r1, c.y - std::cos(a) * r1, wdt);
    };

    // Outer rim (bright) + a faint inner rim line.
    ring(r,          cLine,      1.8f);
    ring(r * 0.965f, cLineFaint, 0.8f);

    // Hub rim — the inner boundary the wound pack winds down to.
    ring(r * 0.42f, cLine, 1.3f);

    // Six clean spokes from the splined centre to the hub rim — the open,
    // uncluttered look of the reference reel (replaces the busy fine-tooth cog).
    for (int i = 0; i < 6; ++i)
        radial(angle + twoPi * (float) i / 6.0f, r * 0.14f, r * 0.40f, 1.4f, cLineSoft);

    // Splined drive hub: the classic 6-tooth cassette sprocket around the spindle.
    {
        const float rOut = r * 0.155f, rIn = r * 0.10f;
        const int   teeth = 6;
        juce::Path spline;
        for (int i = 0; i < teeth * 2; ++i)
        {
            const float a  = angle + twoPi * (float) i / (float) (teeth * 2);
            const float rr = (i % 2 == 0) ? rOut : rIn;
            const float px = c.x + std::sin(a) * rr, py = c.y - std::cos(a) * rr;
            if (i == 0) spline.startNewSubPath(px, py); else spline.lineTo(px, py);
        }
        spline.closeSubPath();
        g.setColour(cLine);
        g.strokePath(spline, juce::PathStrokeType(1.3f));
    }

    // Spindle hole.
    ring(r * 0.06f, cLine, 1.2f);
}

void ReelBay::paintShell(juce::Graphics& g)
{
    g.fillAll(cBg);   // opaque component — covers the editor behind us entirely
    auto b = getLocalBounds().toFloat();
    // Draw in the recreation's 459x308 cassette design space, scaled to bounds.
    const float sx = b.getWidth() / 459.0f, sy = b.getHeight() / 308.0f;
    auto X = [&](float v) { return b.getX() + v * sx; };
    auto Y = [&](float v) { return b.getY() + v * sy; };
    auto rrect = [&](float x, float y, float w, float h, float rad, juce::Colour col, float wdt)
    {
        g.setColour(col);
        g.drawRoundedRectangle(X(x), Y(y), w * sx, h * sy, rad * sx, wdt);
    };

    // Cassette shell — outer + inner outline.
    rrect(2,  2,  455, 304, 22, cLine,     1.8f);
    rrect(10, 8,  439, 292, 18, cLineSoft, 1.1f);

    // Corner screws.
    for (auto p : { juce::Point<float>(21, 20), { 438.0f, 20.0f },
                    { 21.0f, 288.0f }, { 438.0f, 288.0f } })
        drawScrew(g, X(p.x), Y(p.y), 8.0f * sx, cLineSoft.withAlpha(0.7f));

    const juce::Point<float> c1 { X(120), Y(139) };
    const juce::Point<float> c2 { X(332), Y(139) };
    const float r = 91.0f * sx;

    // Wound-tape pack: a dense translucent disc of fine concentric layers — bright at
    // the rim, fading toward the hub — reading as spooled tape (ref close-up). The LEFT
    // reel is nearly full (winds deep, bright); the RIGHT holds less (shallow, fainter).
    auto windPack = [&](juce::Point<float> c, float rInnerF, float rOuterF, float bright)
    {
        const float ri = rInnerF * sx, ro = rOuterF * sx;

        // Translucent sheen: transparent at the hub, building toward the rim.
        juce::ColourGradient grad(juce::Colours::transparentBlack, c.x, c.y,
                                  cLineSoft.withAlpha(bright), c.x, c.y - ro, true);
        const float p0 = ri / ro;
        grad.addColour(juce::jlimit(0.0f, 1.0f, p0 - 0.02f), juce::Colours::transparentBlack);
        grad.addColour(juce::jlimit(0.0f, 1.0f, p0 + 0.05f), cLineSoft.withAlpha(bright * 0.30f));
        grad.addColour(0.93f, cLineSoft.withAlpha(bright));
        grad.addColour(1.0f,  juce::Colours::transparentBlack);
        g.setGradientFill(grad);
        g.fillEllipse(c.x - ro, c.y - ro, ro * 2.0f, ro * 2.0f);

        // Crisp wound layers: distinct concentric rings, brighter toward the rim,
        // with alternating intensity so they read as grooved spooled tape.
        const int n = juce::jmax(5, (int) ((rOuterF - rInnerF) / 2.0f));
        for (int i = 0; i < n; ++i)
        {
            const float t  = (float) i / (float) (n - 1);
            const float rr = ri + (ro - ri) * t;
            const float a  = bright * (0.30f + 0.45f * t) * ((i % 2 == 0) ? 1.0f : 0.55f);
            g.setColour(cLineSoft.withAlpha(juce::jlimit(0.0f, 1.0f, a)));
            g.drawEllipse(c.x - rr, c.y - rr, rr * 2.0f, rr * 2.0f, 0.8f);
        }

        // Glassy diagonal glint, clipped to the wound-pack annulus.
        {
            juce::Path ann;
            ann.addEllipse(c.x - ro, c.y - ro, ro * 2.0f, ro * 2.0f);
            ann.addEllipse(c.x - ri, c.y - ri, ri * 2.0f, ri * 2.0f);
            ann.setUsingNonZeroWinding(false);
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(ann);
            const float gx = c.x - ro * 0.40f, gy = c.y - ro * 0.55f;
            juce::ColourGradient glint(cLine.withAlpha(bright * 0.35f), gx, gy,
                                       juce::Colours::transparentBlack, gx, gy + ro * 1.1f, true);
            g.setGradientFill(glint);
            g.fillEllipse(c.x - ro, c.y - ro, ro * 2.0f, ro * 2.0f);
        }

        // Bright inner edge of the pack.
        g.setColour(cLine.withAlpha(0.85f));
        g.drawEllipse(c.x - ri, c.y - ri, ri * 2.0f, ri * 2.0f, 1.2f);
    };
    windPack(c1, 0.42f * 91.0f, 0.97f * 91.0f, 0.95f);   // left: wound deep & bright (nearly full)
    windPack(c2, 0.74f * 91.0f, 0.97f * 91.0f, 0.55f);   // right: thin outer band, hub shows through

    // Exposed tape: one continuous strand threaded off the left reel's outer rim,
    // hugging the OUTSIDE of all four guide rollers (so they sit inside the loop) and
    // across the bottom below the label, then back up onto the right reel's outer rim —
    // the classic cassette path. Built as point→circle / circle→circle external
    // tangents joined by short outward wrap-arcs; drawn as a faint scrolling dash.
    {
        struct Node { float x, y, r; };
        const Node nodes[] = {
            { 120.0f - 91.0f, 139.0f,  0.0f },  //   far-left point of left reel rim
            {  54.0f, 240.0f, 13.0f },          // 1 outer-left roller
            { 117.0f, 280.0f, 12.0f },          // 2 inner-left roller
            { 335.0f, 280.0f, 12.0f },          // 3 inner-right roller (mirror of 2 about x=226)
            { 398.0f, 240.0f, 13.0f },          // 4 outer-right roller (mirror of 1 about x=226)
            { 332.0f + 91.0f, 139.0f, 0.0f },   //   far-right point of right reel rim
        };
        const int N = (int) (sizeof(nodes) / sizeof(nodes[0]));
        const float Gx = 226.0f, Gy = 140.0f;   // assembly centre; "outer" = away from here
        const float pi = juce::MathConstants<float>::pi, twoPi = juce::MathConstants<float>::twoPi;

        // External-tangent unit normal between two nodes, taken on the outer side.
        // A unit normal n perpendicular to the shared tangent satisfies d·n = r1 - r2
        // (d = centre offset); the two solutions sit on either side — pick the outer.
        float segNx[8] = { 0 }, segNy[8] = { 0 };
        for (int i = 0; i < N - 1; ++i)
        {
            const Node& a = nodes[i]; const Node& b = nodes[i + 1];
            const float dx = b.x - a.x, dy = b.y - a.y, D = std::sqrt(dx * dx + dy * dy);
            const float base = std::atan2(dy, dx);
            const float off  = std::acos(juce::jlimit(-1.0f, 1.0f, (a.r - b.r) / D));
            const float mx = (a.x + b.x) * 0.5f - Gx, my = (a.y + b.y) * 0.5f - Gy;
            const float n1x = std::cos(base + off), n1y = std::sin(base + off);
            const float n2x = std::cos(base - off), n2y = std::sin(base - off);
            const bool first = (mx * n1x + my * n1y) >= (mx * n2x + my * n2y);
            segNx[i] = first ? n1x : n2x;
            segNy[i] = first ? n1y : n2y;
        }

        juce::Array<juce::Point<float>> pts;
        for (int i = 0; i < N; ++i)
        {
            const Node& nd = nodes[i];
            if (nd.r <= 0.0f) { pts.add (juce::Point<float> (nd.x, nd.y)); continue; }

            const float aEntry = std::atan2 (segNy[i - 1], segNx[i - 1]);  // incoming tangent pt
            const float aExit  = std::atan2 (segNy[i],     segNx[i]);      // outgoing tangent pt
            float d = aExit - aEntry;
            while (d >  pi) d -= twoPi;
            while (d < -pi) d += twoPi;
            // Pick the sweep whose midpoint bulges farthest from the assembly centre.
            auto out = [&](float a)
            {
                const float px = nd.x + nd.r * std::cos(a) - Gx, py = nd.y + nd.r * std::sin(a) - Gy;
                return px * px + py * py;
            };
            const float dAlt = d + (d > 0 ? -twoPi : twoPi);
            if (out (aEntry + d * 0.5f) < out (aEntry + dAlt * 0.5f)) d = dAlt;

            const int steps = 12;
            for (int s = 0; s <= steps; ++s)
            {
                const float a = aEntry + d * (float) s / (float) steps;
                pts.add (juce::Point<float> (nd.x + nd.r * std::cos(a), nd.y + nd.r * std::sin(a)));
            }
        }

        // Solid, continuous tape line — always fully visible (no gaps, no motion).
        g.setColour(cLineSoft.withAlpha(0.6f));
        juce::Point<float> prev;
        for (int i = 0; i < pts.size(); ++i)
        {
            const juce::Point<float> p { X(pts[i].x), Y(pts[i].y) };
            if (i > 0) g.drawLine(prev.x, prev.y, p.x, p.y, 1.3f);
            prev = p;
        }
    }

    // Tape guide rollers.
    g.setColour(cLineSoft);
    for (auto gp : { juce::Point<float>(54, 240), { 398.0f, 240.0f } })
        g.drawEllipse(X(gp.x) - 13 * sx, Y(gp.y) - 13 * sx, 26 * sx, 26 * sx, 1.2f);
    for (auto gp : { juce::Point<float>(117, 280), { 335.0f, 280.0f } })
        g.drawEllipse(X(gp.x) - 12 * sx, Y(gp.y) - 12 * sx, 24 * sx, 24 * sx, 1.2f);

    // Centre label window.
    rrect(159, 250, 142, 40, 5, cLine,     1.4f);
    rrect(165, 242, 130, 19, 4, cLineSoft, 1.0f);
    g.setColour(cLineSoft);
    g.drawEllipse(X(230) - 8 * sx, Y(242) - 8 * sx, 16 * sx, 16 * sx, 1.0f);
}

void ReelBay::paint(juce::Graphics& g)
{
    // Static shell (cassette body, wound packs, tape-path tangent math, rollers,
    // label) blits from a cached image — rebuilding all of it at 30 Hz just to
    // rotate two reels was wasted raster. Only the reels draw per frame.
    const float ps = juce::jmax(1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const int iw = juce::jmax(1, juce::roundToInt((float) getWidth()  * ps));
    const int ih = juce::jmax(1, juce::roundToInt((float) getHeight() * ps));
    if (shellCache.getWidth() != iw || shellCache.getHeight() != ih)
    {
        shellCache = juce::Image(juce::Image::ARGB, iw, ih, true);
        juce::Graphics cg(shellCache);
        cg.addTransform(juce::AffineTransform::scale(ps));   // physical px = retina-sharp
        paintShell(cg);
    }
    g.drawImage(shellCache, getLocalBounds().toFloat());

    auto b = getLocalBounds().toFloat();
    const float sx = b.getWidth() / 459.0f, sy = b.getHeight() / 308.0f;
    const juce::Point<float> c1 { b.getX() + 120.0f * sx, b.getY() + 139.0f * sy };
    const juce::Point<float> c2 { b.getX() + 332.0f * sx, b.getY() + 139.0f * sy };
    drawReel(g, c1, 91.0f * sx, phase);
    drawReel(g, c2, 91.0f * sx, -phase2);
}

// =============================== BandFilter ===============================
// Two-handle wet band-pass above ECHO / REVERB (RC20 "FOCUS"). The low-cut handle
// (left, arrow points left) and high-cut handle (right, arrow points right) each
// ride a log frequency axis 0..1; the kept band between them fills teal and its
// shoulders slope to the floor at each cut. lo≈0 + hi≈1 = fully open (greyed).

BandFilter::BandFilter()  { startTimerHz(20); }
BandFilter::~BandFilter() { stopTimer(); }

void BandFilter::attach(juce::RangedAudioParameter* lo, juce::RangedAudioParameter* hi)
{
    loP = lo; hiP = hi;
    repaint();
}

float BandFilter::loVal() const { return loP ? loP->getValue() : 0.0f; }  // 0..1 range → value
float BandFilter::hiVal() const { return hiP ? hiP->getValue() : 1.0f; }

void BandFilter::setVal(juce::RangedAudioParameter* p, float v01)
{
    if (p) p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, v01));
}

int BandFilter::pickHandle(float x) const
{
    auto b = getLocalBounds().toFloat().reduced(1.0f);
    const float xLo = b.getX() + loVal() * b.getWidth();
    const float xHi = b.getX() + hiVal() * b.getWidth();
    return std::abs(x - xLo) <= std::abs(x - xHi) ? 1 : 2;
}

void BandFilter::timerCallback()
{
    if (loVal() != cachedLo || hiVal() != cachedHi)
    {
        cachedLo = loVal(); cachedHi = hiVal();
        repaint();
    }
}

void BandFilter::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced(1.0f);
    const float W = b.getWidth(), H = b.getHeight();
    const float x0 = b.getX();
    const float lo = loVal(), hi = hiVal();
    const float xLo = x0 + lo * W, xHi = x0 + hi * W;
    const float top = b.getY() + H * 0.20f, bot = b.getY() + H * 0.84f;
    const bool  loOff = lo <= 0.02f, hiOff = hi >= 0.98f;
    const bool  open  = loOff && hiOff;

    g.setColour(cLineFaint);
    g.drawRoundedRectangle(b, H * 0.16f, 1.0f);

    // Band-pass response: flat (kept) between the handles, shoulders slope to the
    // floor past each cut. The product of a low-cut and high-cut response curve.
    juce::Path curve;
    const int   N    = 48;
    const float knee = juce::jmax(1.0f, W * 0.06f);
    for (int i = 0; i <= N; ++i)
    {
        const float fx = x0 + W * (float) i / (float) N;
        const float hp = loOff ? 1.0f
                       : juce::jlimit(0.0f, 1.0f, 0.5f + 0.5f * std::tanh((fx - xLo) / knee * 2.4f));
        const float lp = hiOff ? 1.0f
                       : juce::jlimit(0.0f, 1.0f, 0.5f - 0.5f * std::tanh((fx - xHi) / knee * 2.4f));
        const float resp = hp * lp;
        const float y = bot - resp * (bot - top);
        if (i == 0) curve.startNewSubPath(fx, y); else curve.lineTo(fx, y);
    }
    juce::Path fill = curve;
    fill.lineTo(x0 + W, bot);
    fill.lineTo(x0, bot);
    fill.closeSubPath();
    const juce::Colour ac = fxDormant ? cDormant : cTeal;
    g.setColour(ac.withAlpha(open ? 0.05f : 0.15f));
    g.fillPath(fill);
    g.setColour(open ? cLineSoft.withAlpha(0.7f) : ac.withAlpha(0.95f));
    g.strokePath(curve, juce::PathStrokeType(1.6f));

    // Handles — a thin full-height cut line marking the exact handle position.
    // Greyed when the handle is at its open edge (that side bypassed → true
    // pass-through). (Teardrop bulb glyph removed — line only.)
    auto drawHandle = [&](float hx, bool live)
    {
        const auto col = live ? ac : cLineSoft.withAlpha(0.8f);
        g.setColour(col);
        g.fillRoundedRectangle(hx - 0.9f, top, 1.8f, bot - top, 0.9f);
    };
    drawHandle(xLo, ! loOff);
    drawHandle(xHi, ! hiOff);
}

void BandFilter::mouseDown(const juce::MouseEvent& e)
{
    active = pickHandle((float) e.x);
    if (active == 1 && loP) loP->beginChangeGesture();
    if (active == 2 && hiP) hiP->beginChangeGesture();
    mouseDrag(e);
}

void BandFilter::mouseDrag(const juce::MouseEvent& e)
{
    if (! active) return;
    auto b = getLocalBounds().toFloat().reduced(1.0f);
    float p = juce::jlimit(0.0f, 1.0f, ((float) e.x - b.getX()) / juce::jmax(1.0f, b.getWidth()));
    constexpr float gap = 0.04f;                 // keep lo < hi (never cross)
    if (active == 1) setVal(loP, juce::jmin(p, hiVal() - gap));
    else             setVal(hiP, juce::jmax(p, loVal() + gap));
    repaint();
}

void BandFilter::mouseUp(const juce::MouseEvent&)
{
    if (active == 1 && loP) loP->endChangeGesture();
    if (active == 2 && hiP) hiP->endChangeGesture();
    active = 0;
}

// =============================== MixSlider ================================
// Master DRY/WET strip (RC20 "MAGNITUDE"): a thin horizontal track, teal fill
// from dry (left) to the handle, a slim metal handle bar at the value.

MixSlider::MixSlider()
{
    setSliderStyle(juce::Slider::LinearHorizontal);
    setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
}

void MixSlider::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    const float cy = b.getCentreY();
    const float x0 = b.getX() + 6.0f, x1 = b.getRight() - 6.0f;
    const float p  = (float) valueToProportionOfLength(getValue());
    const float hx = x0 + p * (x1 - x0);

    g.setColour(cLineSoft.withAlpha(0.55f));
    g.drawLine(x0, cy, x1, cy, 1.6f);
    g.setColour(cTeal.withAlpha(0.9f));
    g.drawLine(x0, cy, hx, cy, 2.4f);

    const float hw = 8.0f, hh = b.getHeight() * 0.74f;
    juce::Rectangle<float> handle(hx - hw * 0.5f, cy - hh * 0.5f, hw, hh);
    g.setColour(cLine);
    g.fillRoundedRectangle(handle, 2.0f);
    g.setColour(cLineSoft.withAlpha(0.85f));
    g.drawRoundedRectangle(handle, 2.0f, 1.0f);
}

// =============================== PresetBar ===============================
// Blueprint-native take on RC-20's preset strip: a thin rounded-rect panel, the
// preset name (teal) centred with a '*' suffix when tweaked off-book, and a pair of
// stacked chevrons at the right that step prev/next. Clicking the name opens a
// scrolling PopupMenu of all presets. No LOAD/SAVE (factory presets only).

void PresetBar::setCurrent(int idx, bool isDirty)
{
    if (idx == current && isDirty == dirty) return;
    current = idx; dirty = isDirty; repaint();
}

juce::Rectangle<float> PresetBar::arrowZone() const
{
    auto b = getLocalBounds().toFloat();
    return b.removeFromRight(b.getHeight() * 0.78f);   // square-ish cap at the right
}

void PresetBar::step(int delta)
{
    if (presets.isEmpty()) return;
    const int n = presets.size();
    if (current < 0)
        current = delta >= 0 ? 0 : n - 1;   // from INIT: next = first preset, prev = last
    else
        current = (current + delta % n + n) % n;
    if (onSelect) onSelect(current);
    repaint();
}

void PresetBar::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced(1.0f);

    // Panel.
    g.setColour(juce::Colour(0xff0d1213));                 // a touch darker than the faceplate
    g.fillRoundedRectangle(b, b.getHeight() * 0.28f);
    g.setColour(cLineFaint);
    g.drawRoundedRectangle(b, b.getHeight() * 0.28f, 1.3f);

    // Chevron cap (right): a hairline divider + two stacked triangles.
    auto az = arrowZone();
    g.setColour(cLineFaint);
    g.drawLine(az.getX(), b.getY() + 4.0f, az.getX(), b.getBottom() - 4.0f, 1.0f);
    const float acx = az.getCentreX(), acy = az.getCentreY();
    const float aw = 6.0f, ah = 4.0f, gap = 5.0f;
    g.setColour(cLineSoft);
    juce::Path up;                                         // prev
    up.startNewSubPath(acx - aw, acy - gap);
    up.lineTo(acx + aw, acy - gap);
    up.lineTo(acx, acy - gap - ah);
    up.closeSubPath();
    g.fillPath(up);
    juce::Path dn;                                         // next
    dn.startNewSubPath(acx - aw, acy + gap);
    dn.lineTo(acx + aw, acy + gap);
    dn.lineTo(acx, acy + gap + ah);
    dn.closeSubPath();
    g.fillPath(dn);

    // Name (centred over the area left of the chevrons).
    auto nameArea = b.withTrimmedRight(az.getWidth());
    juce::String label = (current >= 0 && current < presets.size())
                       ? presets[current] : juce::String("INIT");
    if (dirty && current >= 0) label += " *";
    g.setColour(current >= 0 ? cTeal : cLineSoft);
    g.setFont(mono(b.getHeight() * 0.46f, true));
    g.drawText(label, nameArea, juce::Justification::centred, false);
}

void PresetBar::mouseDown(const juce::MouseEvent& e)
{
    if (presets.isEmpty()) return;
    auto az = arrowZone();
    if (az.contains((float) e.x, (float) e.y))
    {
        step(e.y < az.getCentreY() ? -1 : +1);             // top chevron = prev, bottom = next
        return;
    }

    // Name area → scrolling dropdown of every preset (PopupMenu auto-scrolls if tall).
    juce::PopupMenu m;
    m.setLookAndFeel(&getLookAndFeel());   // use the editor's TapeLookAndFeel (centred items)
    for (int i = 0; i < presets.size(); ++i)
        m.addItem(i + 1, presets[i], true, i == current);
    // Width pinned to the button so the (centred-text) menu unfurls centred under it.
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this)
                        .withMinimumWidth(getWidth()).withMaximumNumColumns(1),
        [self = juce::Component::SafePointer<PresetBar>(this)](int r)
        {
            // SafePointer: the host can destroy the editor while the menu is open;
            // a raw `this` here is a use-after-free on selection.
            if (self == nullptr || r <= 0) return;
            self->current = r - 1;
            if (self->onSelect) self->onSelect(self->current);
            self->repaint();
        });
}

// ================================ SpinDial ================================
// Big rotor: concentric line-art rings ringed by two curved
// spin-arrows, plus a label. Clicking it re-spins. The arrow
// ring idles with a slow rotation so it reads as the active spin control.

SpinDial::SpinDial()  { startTimerHz(30); }
SpinDial::~SpinDial() { stopTimer(); }

void SpinDial::timerCallback()
{
    if (! spinning) return;             // at rest: hold, no repaint

    // One continuous tween: advance progress, ease-out, map onto a whole number of
    // revolutions so the arrows decelerate and land exactly back at the rest angle.
    spinProg += kSpinStep;
    if (spinProg >= 1.0f) { spinProg = 1.0f; spinning = false; }

    const float t    = spinProg;
    const float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);   // cubic ease-out
    spin = ease * juce::MathConstants<float>::twoPi * (float) kSpinTurns;
    while (spin >= juce::MathConstants<float>::twoPi)
        spin -= juce::MathConstants<float>::twoPi;
    if (! spinning) spin = 0.0f;        // snap home exactly (ease(1)*N*2π ≡ 0)

    repaint();
}

void SpinDial::mouseUp(const juce::MouseEvent&)
{
    if (onSpin && (! canSpin || canSpin()))
    {
        spinProg = 0.0f;   // (re)start the tween from rest
        spinning = true;
        onSpin();
    }
}

void SpinDial::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    const float W = b.getWidth(), H = b.getHeight();
    const float labelH    = H * 0.16f;            // RE-SPIN band reserved at the bottom
    const float circleH   = H - labelH;           // the dial+arrows live in what's left
    const float cx        = b.getCentreX();
    const float cy        = b.getY() + circleH * 0.5f;   // dial centred in the circle area

    // Derive the dial radius from the arrow ring's outer extent so the WHOLE graphic
    // (rings + revolving arrows + arrowheads) fits inside the bounds at any rotation —
    // no self-clip at the top. ar = r*1.34 (+headroom for the arrowhead/stroke).
    const float arMax = juce::jmin(W, circleH) * 0.5f * 0.94f;  // 6% pad for head/stroke
    const float r     = arMax / 1.34f;
    const bool  live  = (! canSpin || canSpin());
    const float pi    = juce::MathConstants<float>::pi;

    auto onArc = [&](float a, float rad)
    { return juce::Point<float>(cx + std::sin(a) * rad, cy - std::cos(a) * rad); };

    const float dim = live ? (hovered ? 1.0f : 0.92f) : 0.4f;

    // Concentric rings.
    g.setColour(cLine.withAlpha(dim));
    g.drawEllipse(cx - r, cy - r, r * 2, r * 2, 2.0f);
    g.drawEllipse(cx - r * 0.87f, cy - r * 0.87f, r * 1.74f, r * 1.74f, 1.8f);
    g.setColour(cLineSoft.withAlpha(dim));
    g.drawEllipse(cx - r * 0.76f, cy - r * 0.76f, r * 1.52f, r * 1.52f, 1.0f);

    // Engraved rim ticks (full circle), slowly rotating to read as "live".
    g.setColour(cLineSoft.withAlpha(dim * 0.7f));
    for (int i = 0; i < 36; ++i)
    {
        const float a = spin + pi * 2.0f * (float) i / 36.0f;
        auto inner = onArc(a, r * 0.90f);
        auto outer = onArc(a, r * 0.99f);
        g.drawLine(inner.x, inner.y, outer.x, outer.y, 1.0f);
    }

    // Centre hub + cross.
    g.setColour(cLine.withAlpha(dim));
    g.drawEllipse(cx - r * 0.11f, cy - r * 0.11f, r * 0.22f, r * 0.22f, 1.4f);
    g.drawLine(cx - r * 0.07f, cy, cx + r * 0.07f, cy, 1.1f);
    g.drawLine(cx, cy - r * 0.07f, cx, cy + r * 0.07f, 1.1f);

    // Two curved spin-arrows hugging the rotor. They revolve with the tick ring
    // through the spin tween and glow teal in proportion to angular speed (the
    // ease-out derivative ∝ (1-t)²), fading out as they decelerate home.
    const float ar = r * 1.34f, head = r * 0.16f;
    const float glow = spinning ? (1.0f - spinProg) * (1.0f - spinProg) : 0.0f;
    g.setColour(cLineSoft.withAlpha(dim).interpolatedWith(cTeal, glow * 0.85f));
    auto arrow = [&](float from, float to)
    {
        juce::Path p;
        p.addCentredArc(cx, cy, ar, ar, 0.0f, from, to, true);
        g.strokePath(p, juce::PathStrokeType(2.2f));
        // Arrowhead at the `to` end, pointing along the arc tangent.
        auto tip  = onArc(to, ar);
        auto tail = onArc(to - 0.22f, ar);          // a step back along the arc
        auto dirN = (tip - tail);                    // tangent direction
        const float len = juce::jmax(0.001f, dirN.getDistanceFromOrigin());
        dirN = dirN / len;
        const juce::Point<float> norm(-dirN.y, dirN.x);
        juce::Path h;
        h.addTriangle(tip.x + dirN.x * head, tip.y + dirN.y * head,
                      tip.x + norm.x * head * 0.6f, tip.y + norm.y * head * 0.6f,
                      tip.x - norm.x * head * 0.6f, tip.y - norm.y * head * 0.6f);
        g.fillPath(h);
    };
    arrow(-pi * 0.92f + spin, -pi * 0.38f + spin);   // left side (revolves with ring)
    arrow( pi * 0.38f + spin,  pi * 0.92f + spin);   // right side

    // (No text label — the spin-arrow rotor reads as "re-spin" on its own.)
}

// ============================ WaveformDisplay ============================

WaveformDisplay::WaveformDisplay(LoopNestProcessor& p) : processor(p)
{
    // Opaque (paintChrome fills the full bounds): our 30 Hz playhead repaints must
    // not cascade into the editor's background paint (vector wordmark et al).
    setOpaque(true);
    formatManager.registerBasicFormats();
    thumbnail.addChangeListener(this);
}

WaveformDisplay::~WaveformDisplay() { thumbnail.removeChangeListener(this); }

void WaveformDisplay::setFile(const juce::File& f)
{
    if (f.existsAsFile()) thumbnail.setSource(new juce::FileInputSource(f));
    else                  thumbnail.clear();
    viewStart = 0.0f;   // a new loop resets the zoom to the whole file
    viewEnd   = 1.0f;
    waveCacheDirty = true;
    repaint();
}

float WaveformDisplay::startNorm() const { return *processor.apvts.getRawParameterValue("start"); }
float WaveformDisplay::endNorm()   const { return *processor.apvts.getRawParameterValue("end"); }

void WaveformDisplay::setTrim(const juce::String& id, float v)
{
    if (auto* p = processor.apvts.getParameter(id))
        p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, v));
    repaint();
}

juce::Rectangle<float> WaveformDisplay::headerArea() const
{
    auto b = getLocalBounds().toFloat();
    return { b.getWidth() * 0.022f, b.getHeight() * 0.045f, b.getWidth() * 0.955f, b.getHeight() * 0.115f };
}

juce::Rectangle<float> WaveformDisplay::waveArea() const
{
    auto b = getLocalBounds().toFloat();
    // Width keeps L/R margins equal (0.021 each side) so the bracket frame is symmetric.
    return { b.getWidth() * 0.021f, b.getHeight() * 0.215f, b.getWidth() * 0.958f, b.getHeight() * 0.625f };
}

float WaveformDisplay::xToNorm(float x) const
{
    auto wa = waveArea();
    const float f = (x - wa.getX()) / juce::jmax(1.0f, wa.getWidth());
    return juce::jlimit(0.0f, 1.0f, viewStart + f * (viewEnd - viewStart));
}

float WaveformDisplay::normToX(float n) const
{
    auto wa = waveArea();
    const float span = juce::jmax(1.0e-6f, viewEnd - viewStart);
    return wa.getX() + (n - viewStart) / span * wa.getWidth();
}

void WaveformDisplay::setView(float start, float end)
{
    constexpr float minSpan = 0.004f;   // don't zoom past ~0.4% of the file
    float s = juce::jlimit(0.0f, 1.0f, start);
    float e = juce::jlimit(0.0f, 1.0f, end);
    if (e - s < minSpan)                 // keep a sane minimum window
    {
        e = juce::jmin(1.0f, s + minSpan);
        s = e - minSpan;
    }
    viewStart = s;
    viewEnd   = e;
    repaint();
}

bool WaveformDisplay::overHandle(juce::Point<float> p, Handle which) const
{
    // The bracket only grabs from its top pennant: a small zone at the top edge.
    auto wa = waveArea();
    constexpr float grabX = 12.0f;     // half-width of the catch zone
    const float topZone = wa.getY() + 18.0f;
    const float hx = normToX(which == Handle::start ? startNorm() : endNorm());
    return p.y <= topZone && std::abs(p.x - hx) <= grabX;
}

void WaveformDisplay::mouseDown(const juce::MouseEvent& e)
{
    if (headerArea().contains(e.position))
        return;  // display-only band; folder switching lives in the top toolbar
    if (thumbnail.getTotalLength() <= 0.0) return;

    // Alt-drag pans the zoomed view (when zoomed in) without disturbing trim/seek.
    if (e.mods.isAltDown())
    {
        panning  = true;
        lastPanX = (float) e.x;
        dragging = Handle::none;
        return;
    }
    panning = false;

    // Trim brackets move ONLY when grabbed by their top triangle. A click/drag
    // anywhere else in the scope seeks/scrubs the playhead instead.
    const bool onS = overHandle(e.position, Handle::start);
    const bool onE = overHandle(e.position, Handle::end);
    if (onS || onE)
    {
        const float sx = normToX(startNorm()), ex = normToX(endNorm());
        dragging = (onS && (! onE || std::abs((float) e.x - sx) <= std::abs((float) e.x - ex)))
                     ? Handle::start : Handle::end;
        // Bracket the whole drag in one host gesture (begin here, end in mouseUp)
        // so undo and touch/latch automation see a single edit, like the knobs do.
        if (auto* p = processor.apvts.getParameter(dragging == Handle::start ? "start" : "end"))
            p->beginChangeGesture();
        mouseDrag(e);
        return;
    }

    dragging = Handle::none;
    processor.seekToNorm(xToNorm((float) e.x));
    repaint();
}

void WaveformDisplay::mouseDrag(const juce::MouseEvent& e)
{
    if (thumbnail.getTotalLength() <= 0.0) return;

    if (panning)
    {
        // Shift the view by the drag delta, keeping the span (no zoom change).
        auto wa = waveArea();
        const float span = viewEnd - viewStart;
        const float dNorm = ((float) e.x - lastPanX) / juce::jmax(1.0f, wa.getWidth()) * span;
        lastPanX = (float) e.x;
        float s = viewStart - dNorm;
        s = juce::jlimit(0.0f, 1.0f - span, s);   // clamp so the window stays on-screen
        setView(s, s + span);
        return;
    }

    const float norm = xToNorm((float) e.x);
    constexpr float minGap = 0.01f;
    if      (dragging == Handle::start) setTrim("start", juce::jmin(norm, endNorm() - minGap));
    else if (dragging == Handle::end)   setTrim("end",   juce::jmax(norm, startNorm() + minGap));
    else { processor.seekToNorm(norm); repaint(); }  // scrub the playhead
}

void WaveformDisplay::mouseUp(const juce::MouseEvent&)
{
    if (dragging != Handle::none)
    {
        if (auto* p = processor.apvts.getParameter(dragging == Handle::start ? "start" : "end"))
            p->endChangeGesture();
        dragging = Handle::none;
    }
    panning = false;
}

void WaveformDisplay::mouseMove(const juce::MouseEvent& e)
{
    // Cursor hint so the drag-the-triangle affordance is discoverable.
    const bool onHandle = thumbnail.getTotalLength() > 0.0
                       && (overHandle(e.position, Handle::start)
                        || overHandle(e.position, Handle::end));
    setMouseCursor(onHandle ? juce::MouseCursor::LeftRightResizeCursor
                            : juce::MouseCursor::NormalCursor);
}

void WaveformDisplay::mouseWheelMove(const juce::MouseEvent& e,
                                     const juce::MouseWheelDetails& wheel)
{
    if (thumbnail.getTotalLength() <= 0.0 || ! waveArea().contains(e.position))
        return;

    // Two-finger scroll PANS the zoomed view (zoom lives on pinch / mouseMagnify).
    // Keeping scroll pan-only removes the old zoom/pan axis-flip that made the
    // gesture feel fidgety. Either axis pans — whichever the user pushes harder —
    // so a natural horizontal or vertical swipe scrubs the timeline.
    const float span = viewEnd - viewStart;
    if (span >= 0.999f) return;                       // nothing to pan when fully zoomed out

    const float delta = std::abs(wheel.deltaX) >= std::abs(wheel.deltaY)
                          ? wheel.deltaX : -wheel.deltaY;
    const float s = juce::jlimit(0.0f, 1.0f - span, viewStart - delta * span);
    setView(s, s + span);
}

void WaveformDisplay::mouseMagnify(const juce::MouseEvent& e, float scaleFactor)
{
    if (thumbnail.getTotalLength() <= 0.0 || ! waveArea().contains(e.position))
        return;
    if (scaleFactor <= 0.0f) return;                  // guard against bogus deltas

    // Cursor-anchored pinch zoom: the sample under the fingers stays put.
    // scaleFactor > 1 = fingers spreading = zoom in = smaller view span.
    const float span    = viewEnd - viewStart;
    const float anchor  = xToNorm((float) e.x);
    const float newSpan = juce::jlimit(0.004f, 1.0f, span / scaleFactor);
    const float f = (anchor - viewStart) / juce::jmax(1.0e-6f, span);  // anchor's view fraction
    setView(anchor - f * newSpan, anchor - f * newSpan + newSpan);
}

void WaveformDisplay::mouseDoubleClick(const juce::MouseEvent& e)
{
    // Double-click the scope to zoom back out to the whole file.
    if (! headerArea().contains(e.position))
        setView(0.0f, 1.0f);
}

void WaveformDisplay::changeListenerCallback(juce::ChangeBroadcaster*)
{
    waveCacheDirty = true;   // thumbnail raster progressed — the cached trace is stale
    repaint();
}

void WaveformDisplay::paintChrome(juce::Graphics& g, const juce::String& folder,
                                  const juce::String& name, bool missing)
{
    g.fillAll(cBg);   // opaque component — corners outside the rounded screen too
    auto b = getLocalBounds().toFloat();
    const float W = b.getWidth(), H = b.getHeight();
    const float corner = H * 0.04f;

    // Screen: flat near-black with a thin white frame (no glow).
    g.setColour(cBgDeep);
    g.fillRoundedRectangle(b, corner);
    g.setColour(cLine.withAlpha(0.7f));
    g.drawRoundedRectangle(b.reduced(1.5f), corner, 1.6f);

    // Divider line below the folder/filename header band.
    g.setColour(cLineSoft.withAlpha(0.5f));
    g.drawHorizontalLine((int) (H * 0.175f), W * 0.022f, W * 0.978f);

    // Header: folder glyph + "folder · count" / filename, in white.
    auto hdr = headerArea();
    g.setFont(mono(hdr.getHeight() * 0.62f));
    // The two toolbar icons (folder switch + funnel) now sit at the left of this header
    // strip, so the folder text is trimmed in to clear them (~2 icon widths + gap).
    auto folderTextArea = hdr.withTrimmedLeft(hdr.getHeight() * 2.05f);
    g.setColour(missing ? cLineSoft : cLine);
    g.drawText(folder, folderTextArea, juce::Justification::centredLeft, false);

    if (name.isNotEmpty())
    {
        // Keep the filename clear of the folder label: it starts at a left threshold
        // just right of the folder text and truncates with an ellipsis on the right,
        // rather than bleeding back over the folder name / sample count.
        const float folderW   = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), folder);
        const float threshold = folderTextArea.getX() + folderW + hdr.getHeight() * 0.6f;
        auto nameArea = headerArea().withLeft(juce::jmin(threshold, headerArea().getRight()));
        g.setColour(cLineSoft);
        g.drawText(name, nameArea, juce::Justification::centredRight, true);
    }
}

void WaveformDisplay::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    const float W = b.getWidth(), H = b.getHeight();
    auto wa = waveArea();

    const bool missing = processor.folderMissing();
    juce::String folder = missing
        ? (processor.getSampleFolder().getFileName() + "  \xc2\xb7  not found")
        : processor.hasFolder()
            ? (processor.getSampleFolder().getFileName() + "  \xc2\xb7  "
                  + juce::String(processor.getSampleCount()))
            : "No folder set";
    const auto name = processor.getCurrentSampleName();

    if (thumbnail.getTotalLength() <= 0.0)
    {
        // Empty scope repaints rarely — not worth caching.
        paintChrome(g, folder, name, missing);
        g.setColour(cTealDim);
        g.setFont(mono(H * 0.075f));
        g.drawText(missing      ? "FOLDER NOT FOUND \xe2\x80\x94 RELINK WITH THE FOLDER ICON"
                   : processor.hasFolder() ? "SPIN TO LOAD A LOOP"
                                           : "CHOOSE A FOLDER WITH THE FOLDER ICON",
                   wa, juce::Justification::centred, false);
        return;
    }

    const double len = thumbnail.getTotalLength();
    auto waInt = wa.toNearestInt();

    // Static layer from the cache; rebuild only when the file/zoom/header/display
    // scale changed (see the header comment on waveCache).
    const float ps = juce::jmax(1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const juce::String headerKey = folder + "|" + name + (missing ? "|!" : "");
    const int iw = juce::jmax(1, juce::roundToInt(W * ps));
    const int ih = juce::jmax(1, juce::roundToInt(H * ps));
    if (waveCacheDirty || waveCache.getWidth() != iw || waveCache.getHeight() != ih
        || cacheViewStart != viewStart || cacheViewEnd != viewEnd
        || cacheHeaderKey != headerKey)
    {
        waveCache = juce::Image(juce::Image::ARGB, iw, ih, true);
        juce::Graphics cg(waveCache);
        cg.addTransform(juce::AffineTransform::scale(ps));   // physical px = retina-sharp

        paintChrome(cg, folder, name, missing);

        cg.saveState();
        cg.reduceClipRegion(waInt);

        // Centre axis.
        cg.setColour(cLineFaint);
        cg.drawHorizontalLine((int) wa.getCentreY(), wa.getX(), wa.getRight());

        // Waveform trace — the teal accent, drawn over the current zoom window.
        cg.setColour(cTeal);
        thumbnail.drawChannels(cg, waInt, viewStart * len, viewEnd * len, 0.95f);

        cg.restoreState();

        // Zoom overview rail: when zoomed in, a faint full-file track with a teal
        // segment marking the visible window — shows where you are in the sample.
        if (viewEnd - viewStart < 0.999f)
        {
            const float ry = H * 0.197f, rh = 2.4f;
            const float rL = wa.getX(), rW = wa.getWidth();
            cg.setColour(cLineFaint);
            cg.fillRect(rL, ry, rW, rh);
            cg.setColour(cTeal);
            cg.fillRect(rL + viewStart * rW, ry, (viewEnd - viewStart) * rW, rh);
        }

        waveCacheDirty = false;
        cacheViewStart = viewStart;
        cacheViewEnd   = viewEnd;
        cacheHeaderKey = headerKey;
    }
    g.drawImage(waveCache, b);

    // --- Dynamic overlays (cheap to redraw at 30 Hz) ---
    const float sN = startNorm(), eN = endNorm();
    const float sx = normToX(sN);
    const float ex = normToX(eN);

    // Everything that maps through the zoom window clips to the scope so a
    // handle/playhead scrolled off-view can't bleed onto the frame or footer.
    g.saveState();
    g.reduceClipRegion(waInt);

    // Dim the trimmed-away regions.
    g.setColour(cBgDeep.withAlpha(0.74f));
    g.fillRect(wa.getX(), wa.getY(), sx - wa.getX(), wa.getHeight());
    g.fillRect(ex, wa.getY(), wa.getRight() - ex, wa.getHeight());

    // Trim handles (white line-art pennants).
    auto drawTrim = [&](float hx)
    {
        g.setColour(cLine);
        g.fillRect(hx - 1.2f, wa.getY(), 2.4f, wa.getHeight());
        juce::Path flag;
        flag.addTriangle(hx - 9.0f, wa.getY(), hx + 9.0f, wa.getY(), hx, wa.getY() + 13.0f);
        g.fillPath(flag);
        g.fillRect(hx - 7.0f, wa.getBottom() - 3.0f, 14.0f, 3.0f);
    };
    drawTrim(sx);
    drawTrim(ex);

    // Scrubber / playhead — always shown now that a click can seek it.
    {
        const float ph = processor.getPlayheadNorm();
        const float px = normToX(ph);
        g.setColour(cTeal.brighter(0.3f));
        g.fillRect(px - 1.0f, wa.getY(), 2.0f, wa.getHeight());
    }

    g.restoreState();

    // Footer timecodes (grey).
    g.setColour(cLineSoft);
    g.setFont(mono(H * 0.07f));
    juce::Rectangle<float> foot(W * 0.06f, H * 0.82f, W * 0.88f, H * 0.13f);
    g.drawText(fmtTime(sN * len), foot, juce::Justification::centredLeft, false);
    g.drawText("LOOP  " + juce::String((eN - sN) * len, 1) + "s",
               foot, juce::Justification::centred, false);
    g.drawText(fmtTime(eN * len), foot, juce::Justification::centredRight, false);
}

// ============================ LoopNestEditor ============================

LoopNestEditor::LoopNestEditor(LoopNestProcessor& p)
    : AudioProcessorEditor(&p), processor(p), waveform(p)
{
    setLookAndFeel(&lnf);

    addAndMakeVisible(reels);
    reels.canSpin   = [this] { return processor.canSpin(); };
    reels.onSpin    = [this] { processor.spin(); refresh(); };
    reels.isPlaying = [this] { return processor.isPlaying(); };

    addAndMakeVisible(respin);
    respin.canSpin = [this] { return processor.canSpin(); };
    respin.onSpin  = [this] { processor.spin(); refresh(); };

    addAndMakeVisible(waveform);
    // The header band is display-only now; folder switching lives in the top toolbar.

    addAndMakeVisible(playButton);
    playButton.setButtonText("PLAY");
    playButton.onClick = [this] { processor.setPlaying(! processor.isPlaying()); refresh(); };

    addAndMakeVisible(loopButton);
    loopButton.setButtonText("LOOP");
    loopButton.setToggleState(processor.isLooping(), juce::dontSendNotification);
    loopButton.onClick = [this] { processor.setLooping(loopButton.getToggleState()); };

    addAndMakeVisible(printButton);
    printButton.setButtonText("HATCH");
    printButton.onClick = [this] { cashOut(); };
    printButton.getFile = [this] { return renderedFile; };

    addAndMakeVisible(folderButton);
    folderButton.onClick = [this] { chooseFolder(); };

    addAndMakeVisible(extractButton);
    extractButton.onClick = [this] { startExtraction(); };

    auto setupKnob = [this](juce::Slider& k)
    {
        k.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        k.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        k.setRotaryParameters(juce::MathConstants<float>::pi * 1.2f,
                              juce::MathConstants<float>::pi * 2.8f, true);
        addAndMakeVisible(k);
    };

    auto setupChip = [this](juce::Label& l)
    {
        l.setJustificationType(juce::Justification::centred);
        l.setColour(juce::Label::textColourId, cLine);
        l.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        l.setColour(juce::Label::outlineColourId, cLineFaint);
        // Inline-editor styling: teal box + caret so it reads like the focus outline.
        l.setColour(juce::Label::backgroundWhenEditingColourId, cBgDeep);
        l.setColour(juce::Label::textWhenEditingColourId, cLine);
        l.setColour(juce::Label::outlineWhenEditingColourId, cTeal);
        addAndMakeVisible(l);
    };

    const char* ids[kNumKnobs] = { "warp", "flutter", "drive", "echo", "reverb", "width" };
    for (int i = 0; i < kNumKnobs; ++i)
    {
        setupKnob(knobs[(size_t) i].slider);
        // No value read-out on the six character knobs — knob + name only.
        knobs[(size_t) i].attachment =
            std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                processor.apvts, ids[i], knobs[(size_t) i].slider);
    }

    // OUTPUT level knob — separate from the character band, tucked in the cassette
    // column's bottom-right corner. Keeps its editable read-out chip (not a pill).
    setupKnob(outputKnob.slider);
    setupChip(outputValue);
    outputKnob.attachment =
        std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.apvts, "output", outputKnob.slider);
    outputValue.linkedSlider = &outputKnob.slider;
    outputValue.onCommitText = [this](const juce::String& t)
    {
        outputKnob.slider.setValue(t.getDoubleValue(), juce::sendNotificationSync);
    };
    outputValue.onResetDefault = [this]
    {
        if (auto* param = processor.apvts.getParameter("output"))
            setParamGestured(param, param->getDefaultValue());
    };

    // INPUT level knob — pre-rack gain stage, mirrors OUTPUT directly above it.
    setupKnob(inputKnob.slider);
    setupChip(inputValue);
    inputKnob.attachment =
        std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.apvts, "input", inputKnob.slider);
    inputValue.linkedSlider = &inputKnob.slider;
    inputValue.onCommitText = [this](const juce::String& t)
    {
        inputKnob.slider.setValue(t.getDoubleValue(), juce::sendNotificationSync);
    };
    inputValue.onResetDefault = [this]
    {
        if (auto* param = processor.apvts.getParameter("input"))
            setParamGestured(param, param->getDefaultValue());
    };

    // PITCH knob — clean transpose, relocated out of the character tier into the SPIN
    // column row (INPUT · OUTPUT · PITCH). Editable chip in semitones; kept out of presets.
    setupKnob(pitchKnob.slider);
    setupChip(pitchValue);
    pitchKnob.attachment =
        std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.apvts, "pitch", pitchKnob.slider);
    pitchValue.linkedSlider = &pitchKnob.slider;
    pitchValue.onCommitText = [this](const juce::String& t)
    {
        pitchKnob.slider.setValue(t.getDoubleValue(), juce::sendNotificationSync);
    };
    pitchValue.onResetDefault = [this]
    {
        if (auto* param = processor.apvts.getParameter("pitch"))
            setParamGestured(param, param->getDefaultValue());
    };

    // One secondary control per knob (column index matches `knobs`): idx0-3 are small
    // rotary knobs (rate/glide/tone) styled like the mains; idx4-5 are the two-handle
    // echo/reverb wet band-pass strips (drive their lo/hi params directly).
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    setupKnob(warpRateKnob);
    setupKnob(driveToneKnob);
    setupKnob(flutterRateKnob);
    setupKnob(echoTimeKnob);        // idx3 (ECHO)   tertiary = TIME
    setupKnob(reverbDecayKnob);     // idx4 (REVERB) tertiary = DECAY
    setupKnob(widthMidKnob);        // idx5 (WIDTH)  tertiary = CENTER
    addAndMakeVisible(echoBand);    // idx3 2nd axis = a band, with a tertiary rotary below
    addAndMakeVisible(reverbBand);  // idx4
    addAndMakeVisible(widthBand);   // idx5
    addAndMakeVisible(eqBand);
    warpRateAttach    = std::make_unique<SA>(processor.apvts, "warpRate",     warpRateKnob);
    driveToneAttach   = std::make_unique<SA>(processor.apvts, "driveTone",    driveToneKnob);
    flutterRateAttach = std::make_unique<SA>(processor.apvts, "flutterRate",  flutterRateKnob);
    echoTimeAttach    = std::make_unique<SA>(processor.apvts, "echoTime",     echoTimeKnob);
    reverbDecayAttach = std::make_unique<SA>(processor.apvts, "reverbDecay",  reverbDecayKnob);
    widthMidAttach    = std::make_unique<SA>(processor.apvts, "widthMid",     widthMidKnob);
    // (pitchGlide param survives for the relocated PITCH but has no UI knob now.)
    widthBand.attach (processor.apvts.getParameter("widthLo"),  processor.apvts.getParameter("widthHi"));
    echoBand.attach  (processor.apvts.getParameter("echoLo"),   processor.apvts.getParameter("echoHi"));
    reverbBand.attach(processor.apvts.getParameter("reverbLo"), processor.apvts.getParameter("reverbHi"));
    eqBand.attach    (processor.apvts.getParameter("eqLo"),     processor.apvts.getParameter("eqHi"));

    // Factory preset selector (bottom band, between EQ and MIX). Selecting one pushes
    // its character/EQ values to the params; tweaking afterwards flips the name to "* "
    // (detected in refresh()). Not auto-applied on load — recalled project state stands.
    {
        juce::StringArray names;
        for (int i = 0; i < kNumFactoryPresets; ++i) names.add(kFactoryPresets[i].name);
        presetBar.setPresets(names);
    }
    presetBar.onSelect = [this](int i) { currentPreset = i; applyPreset(i); refresh(); };
    addAndMakeVisible(presetBar);

    // A/B monitor cluster (bottom band, right of the preset bar). gainMatch + bypass
    // are toggles bound to the bool params; clear is momentary → resets all params.
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    gainMatchButton.setClickingTogglesState(true);
    bypassButton.setClickingTogglesState(true);
    addAndMakeVisible(gainMatchButton);
    addAndMakeVisible(bypassButton);
    addAndMakeVisible(clearButton);
    gainMatchAttach = std::make_unique<BA>(processor.apvts, "gainMatch", gainMatchButton);
    bypassAttach    = std::make_unique<BA>(processor.apvts, "bypass",    bypassButton);
    clearButton.onClick = [this] { clearAllParams(); };

    // Master DRY/WET strip (top-right header).
    mixSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    addAndMakeVisible(mixSlider);
    mixAttach = std::make_unique<SA>(processor.apvts, "mix", mixSlider);

    // Watch every click in the editor so one outside a value chip drops its focus.
    setWantsKeyboardFocus(true);
    addMouseListener(this, true);

    setSize(juce::roundToInt(kRefW * 0.6f), juce::roundToInt(kRefH * 0.6f));  // 1003 x 565
    waveform.setFile(processor.getCurrentSampleFile());
    lastSamplePath = processor.getCurrentSampleFile().getFullPathName();

    // Re-arm PRINT on reopen: the render survives an editor close (it lives on the
    // processor), so the DRAG key shouldn't forget it.
    renderedFile = processor.getLastRender();
    if (renderedFile.existsAsFile())
        printButton.setArmed(true);
    refresh();
    startTimerHz(30);
}

LoopNestEditor::~LoopNestEditor()
{
    processor.setPlaying(false);
    setLookAndFeel(nullptr);
}

void LoopNestEditor::mouseDown(const juce::MouseEvent& e)
{
    // A click anywhere that isn't a value chip (or its open inline editor) drops
    // keyboard focus off any focused chip — clearing the teal outline and committing
    // an in-progress edit. JUCE won't blur it on its own when the clicked component
    // (waveform, reels, empty panel) doesn't itself take keyboard focus.
    auto* hit = e.eventComponent;
    if (hit == &outputValue || outputValue.isParentOf(hit)
        || hit == &inputValue || inputValue.isParentOf(hit)
        || hit == &pitchValue || pitchValue.isParentOf(hit))
        return;                           // click landed on a chip → leave focus alone

    if (outputValue.hasKeyboardFocus(true) || inputValue.hasKeyboardFocus(true)
        || pitchValue.hasKeyboardFocus(true))
        grabKeyboardFocus();
}

juce::Rectangle<int> LoopNestEditor::R(float x, float y, float w, float h) const
{
    const float s = (float) getWidth() / kRefW;
    return juce::Rectangle<int>(juce::roundToInt(x * s), juce::roundToInt(y * s),
                                juce::roundToInt(w * s), juce::roundToInt(h * s));
}

void LoopNestEditor::chooseFolder()
{
    auto start = processor.hasFolder() ? processor.getSampleFolder()
        : juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    chooser = std::make_unique<juce::FileChooser>("Choose a folder of loops", start);
    chooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result.isDirectory()) { processor.setSampleFolder(result); refresh(); }
        });
}

void LoopNestEditor::startExtraction()
{
    if (processor.isExtracting())
        return;

    auto srcStart = processor.hasFolder() ? processor.getSampleFolder()
        : juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    extractChooser = std::make_unique<juce::FileChooser>(
        "Choose a folder to extract drum loops from", srcStart);
    extractChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& fc)
        {
            auto src = fc.getResult();
            if (src.isDirectory())
                chooseExtractDest(src);
        });
}

void LoopNestEditor::chooseExtractDest(juce::File src)
{
    // Open inside the source with an empty name field — the user types a new
    // folder name (new) or navigates to an existing one (redo). No preset name.
    extractChooser = std::make_unique<juce::FileChooser>(
        "Choose or name the destination folder for the curated loops", src);
    extractChooser->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectDirectories
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, src](const juce::FileChooser& fc)
        {
            auto dest = fc.getResult();
            if (dest == juce::File())
                return;
            confirmAndExtract(src, dest);
        });
}

void LoopNestEditor::confirmAndExtract(juce::File src, juce::File dest)
{
    // Guard (mirrors the processor's): the destination must be outside the source
    // tree, otherwise the scan prunes everything and finds nothing.
    if (dest == src || src.isAChildOf(dest))
    {
        juce::NativeMessageBox::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Can't extract there",
            "The destination can't be the source folder (or a parent of it). "
            "Pick a different destination.", this);
        return;
    }

    // Additive + non-destructive: existing curated loops are kept, only novel ones
    // are added — so no overwrite confirmation is needed.
    processor.extractDrumLoops(src, dest);
    refresh();
}

void LoopNestEditor::cashOut()
{
    processor.setPlaying(false);  // stop audition before printing
    auto file = processor.renderLoop();
    if (file.existsAsFile())
    {
        renderedFile = file;
        printButton.setArmed(true);  // PRINT now reads "DRAG" and is the drag source
    }
    refresh();
}

void LoopNestEditor::timerCallback() { refresh(); }

void LoopNestEditor::applyPreset(int idx)
{
    if (idx < 0 || idx >= kNumFactoryPresets) return;
    const auto& p = kFactoryPresets[idx];
    for (int j = 0; j < kNumPresetParams; ++j)
        if (auto* param = processor.apvts.getParameter(kPresetParamIds[j]))
            setParamGestured(param, param->convertTo0to1(p.v[j]));
}

void LoopNestEditor::clearAllParams()
{
    // Full blank slate: every parameter back to its INIT default (pitch/glide/levels/
    // trim/character/EQ/bands/mix + bypass/gainMatch off). The loaded sample stays.
    for (auto* p : processor.getParameters())
        setParamGestured(p, p->getDefaultValue());
    currentPreset = -1;   // → "INIT"
    refresh();
}

bool LoopNestEditor::presetMatches(int idx) const
{
    if (idx < 0 || idx >= kNumFactoryPresets) return false;
    const auto& p = kFactoryPresets[idx];
    for (int j = 0; j < kNumPresetParams; ++j)
        if (auto* param = processor.apvts.getParameter(kPresetParamIds[j]))
            if (std::abs(param->getValue() - param->convertTo0to1(p.v[j])) > 1.0e-3f)
                return false;
    return true;
}

void LoopNestEditor::refresh()
{
    // Drum-loop curation: adopt the curated folder once the worker finishes. The
    // funnel dims while the scan runs (static glyph, no changing label); the folder
    // switch is locked out during a run to avoid a folder/adopt collision.
    processor.finishExtractionIfReady();
    const bool busy = processor.isExtracting();
    extractButton.setEnabled(! busy);
    folderButton.setEnabled(! busy);

    const auto path = processor.getCurrentSampleFile().getFullPathName();
    if (path != lastSamplePath)
    {
        lastSamplePath = path;
        waveform.setFile(processor.getCurrentSampleFile());
        renderedFile = juce::File();
        printButton.setArmed(false);  // a new loop invalidates the printed render
    }

    playButton.setButtonText(processor.isPlaying() ? "PAUSE" : "PLAY");
    playButton.setShowPauseGlyph(processor.isPlaying());
    playButton.setEnabled(processor.hasSample());
    if (loopButton.getToggleState() != processor.isLooping())
        loopButton.setToggleState(processor.isLooping(), juce::dontSendNotification);
    loopButton.setEnabled(processor.hasSample());
    printButton.setEnabled(processor.hasSample());

    // INPUT / OUTPUT read-out chips (the six character knobs show a turn/hover pill).
    if (! inputValue.isBeingEdited())
        inputValue.setText(juce::String(inputKnob.slider.getValue(), 1) + " dB",
                           juce::dontSendNotification);
    if (! outputValue.isBeingEdited())
        outputValue.setText(juce::String(outputKnob.slider.getValue(), 1) + " dB",
                            juce::dontSendNotification);
    if (! pitchValue.isBeingEdited())
    {
        const double st = pitchKnob.slider.getValue();
        pitchValue.setText((st > 0 ? "+" : "") + juce::String(st, 1) + " st",
                           juce::dontSendNotification);
    }

    if (processor.isPlaying())
        waveform.repaint();

    // Grey-out sync: when BYPASS is engaged, knob value arcs + filter bands drop to
    // grey (dormant). Read the param here so user clicks, host automation and project
    // recall all stay in lock-step; repaint the knob/band area only on a change.
    const bool byp = processor.apvts.getRawParameterValue("bypass")->load() > 0.5f;
    if (byp != lnf.fxDormant)
    {
        lnf.fxDormant = byp;
        for (auto* b : { &echoBand, &reverbBand, &widthBand, &eqBand })
            b->fxDormant = byp;
        repaint();
    }

    // Preset selector: the name shows a trailing "*" once the live params drift off
    // the loaded preset (a knob turn, a band drag, an EQ move — any tracked param).
    presetBar.setCurrent(currentPreset, currentPreset >= 0 && ! presetMatches(currentPreset));

    // Only repaint the (static-ish) faceplate when the folder status actually
    // changes — path and missing-flag included, so a renamed/relinked folder with
    // the same file count still refreshes the scope header.
    const juce::String sig = processor.getSampleFolder().getFullPathName()
                           + "|" + juce::String(processor.getSampleCount())
                           + (processor.folderMissing() ? "|!" : "");
    if (sig != lastStatusSig)
    {
        lastStatusSig = sig;
        repaint();
    }
}

void LoopNestEditor::paint(juce::Graphics& g)
{
    g.fillAll(cBg);
    const float s = (float) getWidth() / kRefW;
    g.addTransform(juce::AffineTransform::scale(s));

    // ---- Panel frame (single blueprint border) ----
    g.setColour(cLine.withAlpha(0.85f));
    g.drawRoundedRectangle(juce::Rectangle<float>(10, 10, kRefW - 20, kRefH - 20), 15.0f, 2.0f);

    // ---- Header: logo mark + wordmark ----
    // Logo + wordmark are packed as a COMPACT unit and centred over the cassette
    // panel's centre (x≈293, the SPIN column / reel-bay midpoint), sitting above its
    // top border. Unit width = hex body (2·R=66) + gap + wordmark advance.
    {
        constexpr float logoR = 33.0f, logoGap = 38.0f, capH = 54.0f;
        constexpr float cassetteCx = 293.0f;            // SPIN column / reel-bay centre
        const float wmW   = wordmarkWidth("LOOPNEST", capH);
        const float unitW = logoR * 2.0f + logoGap + wmW;
        const float unitL = cassetteCx - unitW * 0.5f;
        drawLogo(g, unitL + logoR, 9.5f);               // topY tuned: hex top flush at y≈27
        drawWordmark(g, unitL + logoR * 2.0f + logoGap, 27.0f, capH);
    }

    // ---- Bottom-band caption: MIX (EQ + PRESET self-label / need no caption) ----
    g.setColour(cLineSoft.withAlpha(0.8f));
    g.setFont(mono(23.4f, true));
    g.drawText("MIX", juce::Rectangle<float>(1435, 824, 166, 24),
               juce::Justification::centred, false);

    // ---- Full-height SPIN column outline ----
    // (Cassette, RE-SPIN dial + its label are drawn by child components.)
    g.setColour(cLine.withAlpha(0.7f));
    g.drawRoundedRectangle(juce::Rectangle<float>(40, 95, 506, 817), 16.0f, 1.8f);

    // ---- Knob tier: one full-width CHARACTER panel (6 even knobs) ----
    // PITCH was relocated to the SPIN column, so the old mini-panel is gone; the
    // CHARACTER panel now spans the full tier x569..1632 (matches the bottom band).
    g.setColour(cLine.withAlpha(0.6f));
    g.drawRoundedRectangle(juce::Rectangle<float>(569, 492, 1063, 302), 14.0f, 1.6f);  // CHARACTER

    // ---- Bottom band: reserved for presets + master EQ (Pass D/E), holds MIX now ----
    // Spans the right region under the knobs panel; the lower SPIN column sits to its
    // left (max x546) so there is no overlap. Right edge 1632 (30px margin).
    g.setColour(cLine.withAlpha(0.6f));
    g.drawRoundedRectangle(juce::Rectangle<float>(569, 812, 1063, 102), 14.0f, 1.6f);

    // ---- Shape-knob name labels + dividers ----
    // Five dividers at the midpoints between the six even CHARACTER columns.
    g.setColour(cLineSoft.withAlpha(0.5f));
    const float divX[5] = { 746.5f, 923.5f, 1100.5f, 1277.5f, 1454.5f };
    for (float dx : divX) g.drawLine(dx, 508, dx, 790, 1.0f);

    // Secondary-axis names (small, soft). Rotary cells (idx0-2) name the rotary at the
    // cell top (RATE/TONE). Band cells (idx3-5) drop the old "FILTER" word — the band
    // glyph reads as a filter on its own — and instead label the tertiary rotary that
    // sits below the band (TIME/DECAY/CENTER) at kTerNameY.
    g.setColour(cLineSoft.withAlpha(0.7f));
    g.setFont(mono(18.0f, false));
    const char* secNames[kNumKnobs] = { "RATE", "RATE", "TONE", "", "", "" };
    for (int i = 0; i < kNumKnobs; ++i)
        g.drawText(secNames[i], juce::Rectangle<float>(kKnobCx[i] - 95.0f, kSecNameY, 190.0f, 26.0f),
                   juce::Justification::centred, false);
    // Tertiary labels sit to the RIGHT of their rotary (knob left / name right, paired)
    // so it's unambiguous the name belongs to the knob, not the band above it.
    const char* terNames[3] = { "TIME", "DECAY", "CENTER" };   // idx3/4/5 tertiary rotaries
    for (int i = 0; i < 3; ++i)
    {
        juce::Rectangle<float> kb, lb;
        terUnitLayout(i + 3, terNames[i], kb, lb);
        g.drawText(terNames[i], lb, juce::Justification::left, false);
    }

    // Knob names (bold) sit UNDER each knob; no value read-out.
    g.setColour(cLineSoft);
    g.setFont(mono(26.0f, true));
    const char* names[kNumKnobs] = { "WARP", "FLUTTER", "DRIVE", "ECHO", "REVERB", "WIDTH" };
    for (int i = 0; i < kNumKnobs; ++i)
        g.drawText(names[i], juce::Rectangle<float>(kKnobCx[i] - 95.0f, kKnobNameY, 190.0f, 32.0f),
                   juce::Justification::centred, false);

    // INPUT / OUTPUT captions over their knobs (pre-rack gain stage + playback level).
    // INPUT · OUTPUT · PITCH captions over the SPIN-column row (centres x135/293/451).
    g.drawText("INPUT",  juce::Rectangle<float>( 89, 453, 92, 24), juce::Justification::centred, false);
    g.drawText("OUTPUT", juce::Rectangle<float>(247, 453, 92, 24), juce::Justification::centred, false);
    g.drawText("PITCH",  juce::Rectangle<float>(405, 453, 92, 24), juce::Justification::centred, false);
}

void LoopNestEditor::resized()
{
    // Cassette hugs the column top border (now y95, lifted into the gap below the
    // logo/wordmark) + grown, opening the lower column (now y406 down) for the
    // INPUT/OUTPUT/PITCH row + RE-SPIN (task 4). Centre held on column axis x293.
    reels.setBounds(R(56, 124, 474, 300));
    // INPUT · OUTPUT · PITCH — one horizontal row under the cassette, evenly spaced
    // across the column (centres x135 / 293 / 451; knob 84, chip 92×28 below; captions
    // drawn at y427 in paint()). PITCH (idx) relocated here from the character tier.
    const int kRowKnobY = 481, kRowKnobSz = 84, kRowChipY = 573;
    const int rowCx[3]  = { 135, 293, 451 };  // INPUT, OUTPUT, PITCH
    auto placeRow = [&](KnobControl& k, ValueChip& v, int cx)
    {
        k.slider.setBounds(R(cx - kRowKnobSz / 2, kRowKnobY, kRowKnobSz, kRowKnobSz));
        v.setBounds        (R(cx - 46, kRowChipY, 92, 28));
    };
    placeRow(inputKnob,  inputValue,  rowCx[0]);
    placeRow(outputKnob, outputValue, rowCx[1]);
    placeRow(pitchKnob,  pitchValue,  rowCx[2]);

    // RE-SPIN dial — centred on the column axis (x293) below the knob row. paint() fits
    // the dial graphic inside these bounds so the revolving arrows never clip.
    respin.setBounds(R(143, 622, 300, 320));

    // Scope top pulled flush with the logo's topmost edge (shackle apex ≈ y27); bottom
    // held at y392 so the scope just grows taller. Right edge = 1632 (30px margin,
    // mirroring the SPIN column's 30px left margin). Transport + knob panel below.
    waveform.setBounds(R(572, 27, 1060, 355));

    playButton.setBounds(R(573, 400, 298, 74));
    loopButton.setBounds(R(907, 400, 318, 74));
    printButton.setBounds(R(1257, 400, 375, 74));   // right edge 1632

    // Toolbar icons now live INSIDE the scope's header strip, left of the folder name:
    // folder (switch) + funnel (extract). 32px, vertically centred in the ~42px band;
    // WaveformDisplay::paint trims its folder text left to clear them.
    folderButton.setBounds (R(600, 48, 32, 32));
    extractButton.setBounds(R(638, 48, 32, 32));

    // Per-column stack: secondary NAME (paint) · secondary control · primary knob ·
    // primary NAME (paint). idx0/2/3 secondary = a small rotary knob centred above the
    // main knob; idx3/4/5 = a two-handle band in the same slot (ECHO / REVERB / WIDTH).
    juce::Slider* secKnobs[3] = { &warpRateKnob, &flutterRateKnob, &driveToneKnob };
    const int     secIdx[3]   = { 0, 1, 2 };
    for (int k = 0; k < 3; ++k)
        secKnobs[k]->setBounds(R(kKnobCx[secIdx[k]] - kSecKnobSz * 0.5f, kSecKnobY, kSecKnobSz, kSecKnobSz));
    // Bands centre on their own column (the 6-knob CHARACTER panel is evenly spaced, so
    // band centre == knob centre — no panel-relative offset needed).
    echoBand.setBounds  (R(kKnobCx[3] - 78.0f, kBandY, 156.0f, 30.0f));
    reverbBand.setBounds(R(kKnobCx[4] - 78.0f, kBandY, 156.0f, 30.0f));
    widthBand.setBounds (R(kKnobCx[5] - 78.0f, kBandY, 156.0f, 30.0f));
    // Tertiary rotaries (TIME / DECAY / CENTER) below each band, sat LEFT of the column
    // centre with their label to the right (paired knob/name, see paint()).
    auto placeTer = [&](juce::Slider& k, int idx, const char* label)
    { juce::Rectangle<float> kb, lb; terUnitLayout(idx, label, kb, lb); k.setBounds(R(kb.getX(), kb.getY(), kb.getWidth(), kb.getHeight())); };
    placeTer(echoTimeKnob,    3, "TIME");
    placeTer(reverbDecayKnob, 4, "DECAY");
    placeTer(widthMidKnob,    5, "CENTER");

    // Master DRY/WET strip, bottom-right in the bottom band (caption painted above).
    // Narrowed (228→166) to make room for the A/B monitor cluster; right edge held at
    // x1601.
    mixSlider.setBounds(R(1435, 858, 166, 26));   // x1435..1601

    // Bottom band (812..914), left→right: EQ · PRESET · [gainMatch|BYPASS] · CLEAR ·
    // MIX. EQ keeps its 351 width; PRESET is halved (351→168) so the freed span holds
    // the iZotope-style A/B cluster. All strips 46 tall, vertically centred in the band.
    eqBand.setBounds   (R(600,  840, 351, 46));   // x600..951
    presetBar.setBounds(R(979,  840, 168, 46));   // x979..1147, equal 28px gaps to EQ + pill
    gainMatchButton.setBounds(R(1175, 840,  55, 46));  // pill left  (fader glyph), +20% to breathe
    bypassButton.setBounds   (R(1230, 840, 101, 46));  // pill right ("BYPASS"); butts gainMatch
    clearButton.setBounds    (R(1345, 840,  70, 46));  // standalone "CLEAR" chip

    for (int i = 0; i < kNumKnobs; ++i)
        knobs[(size_t) i].slider.setBounds(R(kKnobCx[i] - kKnobSz * 0.5f, kKnobY, kKnobSz, kKnobSz));

    // Scale the INPUT / OUTPUT chip fonts with the window.
    const float s = (float) getWidth() / kRefW;
    auto chipFont = juce::Font(juce::FontOptions()
        .withName(juce::Font::getDefaultMonospacedFontName()).withHeight(22.0f * s));
    inputValue.setFont(chipFont);
    outputValue.setFont(chipFont);
    pitchValue.setFont(chipFont);
}
