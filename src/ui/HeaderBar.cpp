#include "HeaderBar.h"
#include "Theme.h"

namespace splittr
{

HeaderBar::HeaderBar()
{
    setInterceptsMouseClicks (false, false);
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
    const float nameX = 16.0f;

    g.setColour (theme::text);
    g.setFont (theme::mono (11.0f, 0.16f, true));
    g.drawText ("SPLITTR",
                juce::Rectangle<float> (nameX, 0.0f, 200.0f, (float) getHeight() - 1.0f),
                juce::Justification::centredLeft);

    // --- right: status meta ---
    if (metaText.isNotEmpty())
    {
        g.setColour (theme::text50);
        g.setFont (theme::mono (9.5f, 0.12f));
        g.drawText (metaText,
                    juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth() - 16.0f, (float) getHeight() - 1.0f),
                    juce::Justification::centredRight);
    }
}

} // namespace splittr
