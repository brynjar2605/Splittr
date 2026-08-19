#include "Theme.h"
#include "engine/ModelManager.h"

namespace splittr::theme
{

namespace
{
    Mode mode = Mode::dark;
    Accent accentChoice = Accent::purple;

    // Rotate the design's purple ladder to pink; darken slightly in light
    // mode so the accents keep contrast on paper.
    juce::Colour accentBase (juce::uint32 purpleArgb)
    {
        auto c = juce::Colour (purpleArgb);

        if (accentChoice == Accent::pink)
            c = c.withRotatedHue (0.135f); // ~300deg -> ~350deg (pink)

        if (mode == Mode::light)
            c = c.withMultipliedBrightness (0.78f).withMultipliedSaturation (1.15f);

        return c;
    }
} // namespace

Mode currentMode() { return mode; }
Accent currentAccent() { return accentChoice; }

void apply (Mode newMode, Accent newAccent)
{
    mode = newMode;
    accentChoice = newAccent;

    // ink = the neutral used for text/lines/panels at various alphas
    const auto ink = [] (float a)
    {
        return mode == Mode::dark ? rgba (221, 214, 230, a) : rgba (43, 36, 54, a);
    };

    bg = mode == Mode::dark ? juce::Colour (0xff141118) : juce::Colour (0xffF2EFF6);
    bgHeader = mode == Mode::dark ? juce::Colour (0xff1B1622) : juce::Colour (0xffE8E2F0);
    bgPanel = ink (mode == Mode::dark ? 0.03f : 0.04f);
    bgPanelAlt = ink (mode == Mode::dark ? 0.04f : 0.05f);

    accent = accentBase (0xff9C63EE);
    accentMid = accentBase (0xffAB7CF2);
    accentSoft = accentBase (0xffC09AF6);
    accentPale = accentBase (mode == Mode::dark ? 0xffCDB2F8 : 0xffB58FF5);

    const auto tintBase = accentMid;
    accentTint5 = tintBase.withAlpha (mode == Mode::dark ? 0.05f : 0.07f);
    accentTint6 = tintBase.withAlpha (mode == Mode::dark ? 0.06f : 0.08f);
    accentTint15 = tintBase.withAlpha (0.15f);
    accentTint35 = tintBase.withAlpha (0.35f);
    accentTint40 = tintBase.withAlpha (0.40f);
    gridLine = tintBase.withAlpha (mode == Mode::dark ? 0.09f : 0.12f);

    text = ink (mode == Mode::dark ? 1.0f : 0.92f);
    text70 = ink (0.70f);
    text60 = ink (0.60f);
    text55 = ink (0.55f);
    text50 = ink (0.50f);
    text45 = ink (0.45f);
    text40 = ink (0.40f);
    text35 = ink (0.35f);

    lineStrong = ink (mode == Mode::dark ? 0.20f : 0.25f);
    line = ink (mode == Mode::dark ? 0.18f : 0.22f);
    lineSoft15 = ink (0.15f);
    lineSoft10 = ink (0.10f);
    border35 = ink (0.35f);

    // persist
    ModelManager::setUiSetting ("uiMode", mode == Mode::light ? "light" : "dark");
    ModelManager::setUiSetting ("uiAccent", accentChoice == Accent::pink ? "pink" : "purple");
}

void loadFromSettings()
{
    const auto m = ModelManager::getUiSetting ("uiMode", "dark") == "light" ? Mode::light : Mode::dark;
    const auto a = ModelManager::getUiSetting ("uiAccent", "purple") == "pink" ? Accent::pink : Accent::purple;

    if (m != mode || a != accentChoice || true)
        apply (m, a);
}

} // namespace splittr::theme
