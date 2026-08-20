#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

namespace splittr::ui
{

inline float textWidth (const juce::Font& font, const juce::String& text)
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (font, text, 0.0f, 0.0f);
    return glyphs.getBoundingBox (0, -1, true).getWidth();
}

inline juce::String mmss (double seconds)
{
    const auto total = juce::jmax (0, (int) std::round (seconds));
    return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
}

// --- shared bench controls ------------------------------------------------
// Drawn rather than built from child components, so they follow live
// theme/accent changes and can be hit-tested by whichever view hosts them.

constexpr int switchHeight = 22;

// Segmented switch: draws [a|b] at (x, y) and returns the bounds of both
// cells. `hovered` is the host's current hover id, compared against
// hoverIdA/hoverIdB for the dimmed-to-bright roll.
inline std::pair<juce::Rectangle<int>, juce::Rectangle<int>>
drawSwitch (juce::Graphics& g, int x, int y, const juce::String& a, const juce::String& b,
            bool bActive, int hovered, int hoverIdA, int hoverIdB, int cellWidth)
{
    const juce::Rectangle<int> aBounds (x, y, cellWidth, switchHeight);
    const juce::Rectangle<int> bBounds (x + cellWidth, y, cellWidth, switchHeight);

    g.setColour (theme::accentTint15);
    g.fillRect (bActive ? bBounds : aBounds);

    g.setColour (theme::lineStrong);
    g.drawRect (juce::Rectangle<int> (x, y, cellWidth * 2, switchHeight), 1);
    g.fillRect (x + cellWidth, y + 1, 1, switchHeight - 2); // divider

    g.setFont (theme::mono (9.5f, 0.1f));
    g.setColour (! bActive ? theme::accentSoft
                           : (hovered == hoverIdA ? theme::text60 : theme::text40));
    g.drawText (a, aBounds, juce::Justification::centred);

    g.setColour (bActive ? theme::accentSoft
                         : (hovered == hoverIdB ? theme::text60 : theme::text40));
    g.drawText (b, bBounds, juce::Justification::centred);

    return { aBounds, bBounds };
}

// Full-spectrum accent picker: 1px hue columns, hairline frame, marker at the
// live theme hue. Click or drag anywhere along it.
inline void drawHueStrip (juce::Graphics& g, juce::Rectangle<int> strip)
{
    const auto sat = 0.72f;
    const auto bri = theme::currentMode() == theme::Mode::dark ? 0.92f : 0.80f;
    const auto w = strip.getWidth();

    for (int i = 0; i < w; ++i)
    {
        g.setColour (juce::Colour::fromHSV ((float) i / (float) w, sat, bri, 1.0f));
        g.fillRect (strip.getX() + i, strip.getY() + 1, 1, strip.getHeight() - 2);
    }

    g.setColour (theme::lineStrong);
    g.drawRect (strip, 1);

    const auto markerX = strip.getX() + (int) (theme::currentAccentHue() * (float) w);
    g.setColour (theme::bg);
    g.fillRect (markerX - 2, strip.getY() - 2, 5, strip.getHeight() + 4);
    g.setColour (theme::accentMid);
    g.fillRect (markerX - 1, strip.getY() - 2, 3, strip.getHeight() + 4);
}

inline float hueForX (const juce::Rectangle<int>& strip, int x)
{
    return juce::jlimit (0.0f, 1.0f, (float) (x - strip.getX()) / (float) strip.getWidth());
}

// 8.5px field label (ENGINE, THEME, ACCENT…) on a switch-height row.
// Returns the x where an adjacent control should start.
inline int drawFieldLabel (juce::Graphics& g, const juce::String& label, int x, int y)
{
    const auto font = theme::mono (8.5f, 0.12f);

    g.setFont (font);
    g.setColour (theme::text35);
    g.drawText (label, x, y, (int) textWidth (font, label) + 2, switchHeight,
                juce::Justification::centredLeft);

    return x + (int) textWidth (font, label) + 12;
}

// Bare text action (LIBRARY, PLAY, …): text40 rolling to full text on hover.
inline juce::Rectangle<int> drawTextAction (juce::Graphics& g, const juce::String& label,
                                            int x, int y, int height, bool hovered,
                                            juce::Colour idle = theme::text40)
{
    const auto font = theme::mono (9.5f, 0.08f);
    const juce::Rectangle<int> r (x, y, (int) textWidth (font, label) + 2, height);

    g.setFont (font);
    g.setColour (hovered ? theme::text : idle);
    g.drawText (label, r, juce::Justification::centredLeft);

    return r;
}

// Hairline-bordered clickable label (CANCEL, LOCATE FILE MANUALLY…, …).
// 9.5px mono, .08em tracking, 6x12px padding, hover -> full-opacity text.
class TagButton : public juce::Component
{
public:
    TagButton (juce::String label, juce::Colour borderColour, juce::Colour textColour)
        : text (std::move (label)), border (borderColour), textCol (textColour)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    std::function<void()> onClick;

    int idealWidth() const
    {
        return (int) std::ceil (textWidth (theme::mono (9.5f, 0.08f), text)) + 24;
    }

    static constexpr int idealHeight = 25; // 6px pad + ~13px line + 6px pad

    void setLabel (const juce::String& newText)
    {
        if (text != newText)
        {
            text = newText;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (border);
        g.drawRect (getLocalBounds(), 1);

        g.setColour (hovered ? theme::text : textCol);
        g.setFont (theme::mono (9.5f, 0.08f));
        g.drawText (text, getLocalBounds(), juce::Justification::centred);
    }

    void mouseEnter (const juce::MouseEvent&) override { hovered = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (onClick != nullptr && getLocalBounds().contains (e.getPosition()))
            onClick();
    }

private:
    juce::String text;
    juce::Colour border, textCol;
    bool hovered = false;
};

// The TagButton look drawn straight into paint() — for overlays whose actions
// have to follow live theme changes. Returns the action's hit rect.
inline juce::Rectangle<int> drawTagAction (juce::Graphics& g, const juce::String& label,
                                           int x, int y, bool hovered,
                                           juce::Colour border, juce::Colour textColour)
{
    const auto font = theme::mono (9.5f, 0.08f);
    const juce::Rectangle<int> r (x, y, (int) std::ceil (textWidth (font, label)) + 24,
                                  TagButton::idealHeight);

    g.setColour (border);
    g.drawRect (r, 1);

    g.setColour (hovered ? theme::text : textColour);
    g.setFont (font);
    g.drawText (label, r, juce::Justification::centred);

    return r;
}

} // namespace splittr::ui
