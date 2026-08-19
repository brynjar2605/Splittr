#include "DoneView.h"
#include "Common.h"

namespace splittr
{

namespace
{
    constexpr int padX = 16, padY = 14, rowGap = 11;
    constexpr int rowHeight = 9 + 20 + 7 + 36 + 9; // pad + title + gap + wave + pad
    constexpr int footerHeight = 13;
} // namespace

DoneView::DoneView()
{
    addAndMakeVisible (vocalsRow);
    addAndMakeVisible (instrumentalRow);
}

void DoneView::update (const EngineSnapshot& s)
{
    statsText = juce::String (s.inferenceSeconds, 1) + " s"
                + juce::String::fromUTF8 (" \xc2\xb7 ")
                + juce::String (s.realtimeFactor, 2) + "x RT";
    repaint();
}

void DoneView::resized()
{
    auto area = getLocalBounds().reduced (padX, padY);

    vocalsRow.setBounds (area.removeFromTop (rowHeight));
    area.removeFromTop (rowGap);
    instrumentalRow.setBounds (area.removeFromTop (rowHeight));
}

void DoneView::paint (juce::Graphics& g)
{
    auto footer = getLocalBounds().reduced (padX, padY).removeFromBottom (footerHeight);

    const auto font = theme::mono (9.0f, 0.08f);
    g.setFont (font);

    auto x = footer.getX();

    g.setColour (theme::text50);
    g.drawText (statsText, x, footer.getY(), (int) ui::textWidth (font, statsText) + 2,
                footer.getHeight(), juce::Justification::centredLeft);
    x += (int) ui::textWidth (font, statsText) + 16;

    revealBounds = { x, footer.getY(), (int) ui::textWidth (font, "REVEAL") + 2, footer.getHeight() };
    g.setColour (hoveredAction == 1 ? theme::text : theme::text50);
    g.drawText ("REVEAL", revealBounds, juce::Justification::centredLeft);
    x = revealBounds.getRight() + 16;

    newFileBounds = { x, footer.getY(), (int) ui::textWidth (font, "NEW FILE") + 2, footer.getHeight() };
    g.setColour (hoveredAction == 2 ? theme::text : theme::text50);
    g.drawText ("NEW FILE", newFileBounds, juce::Justification::centredLeft);

    g.setColour (theme::accentSoft);
    g.drawText ("DRAG A ROW TO A TRACK", footer, juce::Justification::centredRight);
}

void DoneView::mouseUp (const juce::MouseEvent& e)
{
    if (revealBounds.contains (e.getPosition()) && onReveal != nullptr)
        onReveal();
    else if (newFileBounds.contains (e.getPosition()) && onNewFile != nullptr)
        onNewFile();
}

void DoneView::mouseMove (const juce::MouseEvent& e)
{
    const auto action = revealBounds.contains (e.getPosition()) ? 1
                        : newFileBounds.contains (e.getPosition()) ? 2
                                                                   : 0;
    if (action != hoveredAction)
    {
        hoveredAction = action;
        setMouseCursor (action != 0 ? juce::MouseCursor::PointingHandCursor
                                    : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void DoneView::mouseExit (const juce::MouseEvent&)
{
    if (hoveredAction != 0)
    {
        hoveredAction = 0;
        repaint();
    }
}

} // namespace splittr
