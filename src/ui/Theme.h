#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

// Design tokens from design_handoff_stemsplit_bench_dark/README.md (option 3a
// "Bench, dark"). Values are verbatim; do not tweak here without updating the
// design reference.
namespace splittr::theme
{

// --- colours -----------------------------------------------------------
inline juce::Colour rgba (juce::uint8 r, juce::uint8 g, juce::uint8 b, float a)
{
    return juce::Colour::fromRGB (r, g, b).withAlpha (a);
}

inline const juce::Colour bg { 0xff141118 };
inline const juce::Colour bgHeader { 0xff1B1622 };
inline const juce::Colour bgPanel = rgba (221, 214, 230, 0.03f);
inline const juce::Colour bgPanelAlt = rgba (221, 214, 230, 0.04f);

inline const juce::Colour accent { 0xff9C63EE };     // oklch(.68 .19 300)
inline const juce::Colour accentMid { 0xffAB7CF2 };  // oklch(.72 .17 300)
inline const juce::Colour accentSoft { 0xffC09AF6 }; // oklch(.78 .14 300)
inline const juce::Colour accentPale { 0xffCDB2F8 }; // oklch(.82 .11 300)

inline const juce::Colour accentTint6 = rgba (168, 132, 240, 0.06f);
inline const juce::Colour accentTint35 = rgba (168, 132, 240, 0.35f);
inline const juce::Colour accentTint40 = rgba (168, 132, 240, 0.40f); // idle drop border
inline const juce::Colour accentTint5 = rgba (168, 132, 240, 0.05f);  // idle drop fill
inline const juce::Colour gridLine = rgba (168, 132, 240, 0.09f);

inline const juce::Colour text { 0xffDDD6E6 };
inline const juce::Colour text60 = rgba (221, 214, 230, 0.60f);
inline const juce::Colour text55 = rgba (221, 214, 230, 0.55f);
inline const juce::Colour text50 = rgba (221, 214, 230, 0.50f);
inline const juce::Colour text45 = rgba (221, 214, 230, 0.45f);
inline const juce::Colour text40 = rgba (221, 214, 230, 0.40f);
inline const juce::Colour text35 = rgba (221, 214, 230, 0.35f);

inline const juce::Colour lineStrong = rgba (221, 214, 230, 0.20f);
inline const juce::Colour line = rgba (221, 214, 230, 0.18f);
inline const juce::Colour lineSoft15 = rgba (221, 214, 230, 0.15f);
inline const juce::Colour lineSoft10 = rgba (221, 214, 230, 0.10f);
inline const juce::Colour border35 = rgba (221, 214, 230, 0.35f); // CANCEL button

// --- type --------------------------------------------------------------
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

// size in px, tracking in em (CSS letter-spacing)
inline juce::Font mono (float size, float trackingEm = 0.0f, bool medium = false)
{
    return juce::Font (juce::FontOptions (medium ? monoMedium() : monoRegular())
                           .withHeight (size)
                           .withKerningFactor (trackingEm));
}

// --- layout constants ---------------------------------------------------
constexpr int editorWidth = 560;
constexpr int editorHeight = 380;
constexpr int headerHeight = 39;

} // namespace splittr::theme
