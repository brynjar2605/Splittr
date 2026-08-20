#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

// Design tokens from design_handoff_stemsplit_bench_dark/README.md (option 3a
// "Bench, dark"). The dark/purple values are the design's; light mode and the
// pink accent are derived variants selectable at runtime via theme::apply().
namespace splittr::theme
{

inline juce::Colour rgba (juce::uint8 r, juce::uint8 g, juce::uint8 b, float a)
{
    return juce::Colour::fromRGB (r, g, b).withAlpha (a);
}

// --- runtime-switchable palette (defaults = design dark/purple) ---------
inline juce::Colour bg { 0xff141118 };
inline juce::Colour bgHeader { 0xff1B1622 };
inline juce::Colour bgPanel = rgba (221, 214, 230, 0.03f);
inline juce::Colour bgPanelAlt = rgba (221, 214, 230, 0.04f);

inline juce::Colour accent { 0xff9C63EE };     // oklch(.68 .19 300)
inline juce::Colour accentMid { 0xffAB7CF2 };  // oklch(.72 .17 300)
inline juce::Colour accentSoft { 0xffC09AF6 }; // oklch(.78 .14 300)
inline juce::Colour accentPale { 0xffCDB2F8 }; // oklch(.82 .11 300)

inline juce::Colour accentTint6 = rgba (168, 132, 240, 0.06f);
inline juce::Colour accentTint35 = rgba (168, 132, 240, 0.35f);
inline juce::Colour accentTint40 = rgba (168, 132, 240, 0.40f);
inline juce::Colour accentTint5 = rgba (168, 132, 240, 0.05f);
inline juce::Colour accentTint15 = rgba (168, 132, 240, 0.15f);
inline juce::Colour gridLine = rgba (168, 132, 240, 0.09f);

// Fixed green for the scanning trail in the working view -- deliberately not
// accent-derived, so "burning through the song" reads the same whatever hue
// the user picked. Adjusted per mode so it stays legible on both grounds.
inline juce::Colour scanEmber { 0xff5CC463 };

inline juce::Colour text { 0xffDDD6E6 };
inline juce::Colour text60 = rgba (221, 214, 230, 0.60f);
inline juce::Colour text55 = rgba (221, 214, 230, 0.55f);
inline juce::Colour text50 = rgba (221, 214, 230, 0.50f);
inline juce::Colour text45 = rgba (221, 214, 230, 0.45f);
inline juce::Colour text40 = rgba (221, 214, 230, 0.40f);
inline juce::Colour text35 = rgba (221, 214, 230, 0.35f);
inline juce::Colour text70 = rgba (221, 214, 230, 0.70f);

inline juce::Colour lineStrong = rgba (221, 214, 230, 0.20f);
inline juce::Colour line = rgba (221, 214, 230, 0.18f);
inline juce::Colour lineSoft15 = rgba (221, 214, 230, 0.15f);
inline juce::Colour lineSoft10 = rgba (221, 214, 230, 0.10f);
inline juce::Colour border35 = rgba (221, 214, 230, 0.35f);

// --- theme selection ------------------------------------------------------
enum class Mode { dark, light };

Mode currentMode();
float currentAccentHue(); // 0..1 (juce hue); default = the design purple
float defaultAccentHue();

// Rebuild every token above for the given mode/accent hue. Callers must
// repaint afterwards. persist=false skips the Settings.xml write (use for
// live drag preview; commit with persist=true on release).
void apply (Mode, float accentHue, bool persist = true);

// Read the persisted choice (Settings.xml) and apply it. Cheap; call from
// editor construction.
void loadFromSettings();

// --- type ----------------------------------------------------------------
inline const juce::Typeface::Ptr& monoRegular()
{
    static juce::Typeface::Ptr t = juce::Typeface::createSystemTypefaceFor (
        BinaryData::DMMonoRegular_ttf, BinaryData::DMMonoRegular_ttfSize);
    return t;
}

inline const juce::Typeface::Ptr& monoMedium()
{
    static juce::Typeface::Ptr t = juce::Typeface::createSystemTypefaceFor (
        BinaryData::DMMonoMedium_ttf, BinaryData::DMMonoMedium_ttfSize);
    return t;
}

inline juce::Font mono (float size, float trackingEm = 0.0f, bool medium = false)
{
    return juce::Font (juce::FontOptions (medium ? monoMedium() : monoRegular())
                           .withHeight (size)
                           .withKerningFactor (trackingEm));
}

// --- layout constants ------------------------------------------------------
constexpr int editorWidth = 560;
constexpr int editorHeight = 380;
constexpr int headerHeight = 39;

} // namespace splittr::theme
