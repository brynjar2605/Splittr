#include "HeaderBar.h"
#include "Theme.h"

namespace splittr
{

namespace
{
    constexpr int inset = 16;    // matches the wordmark inset
    constexpr int markWidth = 11, markHeight = 9, markRowGap = 4;
    constexpr int metaGap = 12;  // between the status meta and the mark
} // namespace

HeaderBar::HeaderBar()
{
    // Only the settings mark takes clicks (hitTest); the rest of the bar stays
    // transparent to the mouse so it never eats a drop or a browse click.
    setInterceptsMouseClicks (true, false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

juce::Rectangle<int> HeaderBar::markBounds() const
{
    return { getWidth() - inset - markWidth, (getHeight() - 1 - markHeight) / 2,
             markWidth, markHeight };
}

juce::Rectangle<int> HeaderBar::settingsBounds() const
{
    return markBounds().expanded (6, 8);
}

bool HeaderBar::hitTest (int x, int y)
{
    return settingsBounds().contains (x, y);
}

void HeaderBar::update (double seconds, Phase phase, const juce::String& meta)
{
    juce::ignoreUnused (seconds);

    // Nothing in the header animates, so only repaint when the content changes.
    if (phase != currentPhase || meta != metaText)
    {
        currentPhase = phase;
        metaText = meta;
        repaint();
    }
}

void HeaderBar::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    g.setColour (theme::bgHeader);
    g.fillRect (bounds);

    g.setColour (theme::line);
    g.fillRect (bounds.removeFromBottom (1));

    // --- left: wordmark at the 16px inset (no mark until a real logomark exists) ---
    const float nameX = (float) inset;

    g.setColour (theme::text);
    g.setFont (theme::mono (11.0f, 0.16f, true));
    g.drawText ("SPLITTR",
                juce::Rectangle<float> (nameX, 0.0f, 200.0f, (float) getHeight() - 1.0f),
                juce::Justification::centredLeft);

    // --- far right: settings mark (three hairlines) ---
    const auto mark = markBounds();

    g.setColour (settingsHovered ? theme::text : theme::text40);

    for (int i = 0; i < 3; ++i)
        g.fillRect (mark.getX(), mark.getY() + i * markRowGap, mark.getWidth(), 1);

    // --- right: status meta, stopping short of the mark ---
    if (metaText.isNotEmpty())
    {
        g.setColour (theme::text50);
        g.setFont (theme::mono (9.5f, 0.12f));
        g.drawText (metaText,
                    juce::Rectangle<float> (0.0f, 0.0f, (float) (mark.getX() - metaGap),
                                            (float) getHeight() - 1.0f),
                    juce::Justification::centredRight);
    }
}

void HeaderBar::mouseUp (const juce::MouseEvent& e)
{
    if (onSettings != nullptr && settingsBounds().contains (e.getPosition()))
        onSettings();
}

void HeaderBar::mouseEnter (const juce::MouseEvent&)
{
    settingsHovered = true;
    repaint();
}

void HeaderBar::mouseExit (const juce::MouseEvent&)
{
    settingsHovered = false;
    repaint();
}

} // namespace splittr
