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

} // namespace splittr::ui
