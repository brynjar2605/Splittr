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

// Segmented switch of any width: draws [a|b|…] at (x, y) and returns one
// bounds per cell. Cell n answers to hover id hoverIdBase + n against the
// host's current `hovered` id for the dimmed-to-bright roll.
inline std::vector<juce::Rectangle<int>>
drawSwitchCells (juce::Graphics& g, int x, int y, const juce::StringArray& labels,
                 int activeIndex, int hovered, int hoverIdBase, int cellWidth)
{
    std::vector<juce::Rectangle<int>> cells;

    for (int i = 0; i < labels.size(); ++i)
        cells.emplace_back (x + i * cellWidth, y, cellWidth, switchHeight);

    if (juce::isPositiveAndBelow (activeIndex, labels.size()))
    {
        g.setColour (theme::accentTint15);
        g.fillRect (cells[(size_t) activeIndex]);
    }

    g.setColour (theme::lineStrong);
    g.drawRect (juce::Rectangle<int> (x, y, cellWidth * labels.size(), switchHeight), 1);

    for (int i = 1; i < labels.size(); ++i)
        g.fillRect (x + i * cellWidth, y + 1, 1, switchHeight - 2); // dividers

    g.setFont (theme::mono (9.5f, 0.1f));

    for (int i = 0; i < labels.size(); ++i)
    {
        g.setColour (i == activeIndex ? theme::accentSoft
                                      : (hovered == hoverIdBase + i ? theme::text60 : theme::text40));
        g.drawText (labels[i], cells[(size_t) i], juce::Justification::centred);
    }

    return cells;
}

// Index of the cell containing p, or -1.
inline int cellAt (const std::vector<juce::Rectangle<int>>& cells, juce::Point<int> p)
{
    for (size_t i = 0; i < cells.size(); ++i)
        if (cells[i].contains (p))
            return (int) i;

    return -1;
}

// Two-cell convenience used by the THEME switch.
inline std::pair<juce::Rectangle<int>, juce::Rectangle<int>>
drawSwitch (juce::Graphics& g, int x, int y, const juce::String& a, const juce::String& b,
            bool bActive, int hovered, int hoverIdA, int hoverIdB, int cellWidth)
{
    juce::ignoreUnused (hoverIdB);
    const auto cells = drawSwitchCells (g, x, y, { a, b }, bActive ? 1 : 0,
                                        hovered, hoverIdA, cellWidth);
    return { cells[0], cells[1] };
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
