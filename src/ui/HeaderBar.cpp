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
    const bool wasAnimating = (currentPhase == Phase::Separating || currentPhase == Phase::Rendering
                               || currentPhase == Phase::Decoding);
    const bool animating = (phase == Phase::Separating || phase == Phase::Rendering
                            || phase == Phase::Decoding);

    clockSeconds = seconds;

    if (phase != currentPhase || meta != metaText || animating || wasAnimating)
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

    const bool working = (currentPhase == Phase::Separating || currentPhase == Phase::Rendering
                          || currentPhase == Phase::Decoding);

    // --- left group: mark + name (16px inset, 9px gap) ---
    const float markCentreY = (float) getHeight() / 2.0f - 0.5f;
    float nameX = 16.0f + 7.0f + 9.0f;

    if (working)
    {
        // Sparkle: rotate 0->180deg, scale .88->1.12, alpha .45->1, 1.4 s ease-in-out
        const float t = (float) std::fmod (clockSeconds, 1.4) / 1.4f;
        const float eased = 0.5f - 0.5f * std::cos (t * juce::MathConstants<float>::twoPi);
        const float angle = eased * juce::MathConstants<float>::pi;
        const float scale = 0.88f + eased * (1.12f - 0.88f);
        const float alpha = 0.45f + eased * 0.55f;

        g.saveState();
        g.addTransform (juce::AffineTransform::rotation (angle, 16.0f + 3.5f, markCentreY)
                            .scaled (scale, scale, 16.0f + 3.5f, markCentreY));
        g.setColour (theme::accent.withAlpha (alpha));
        g.setFont (theme::mono (12.0f));
        g.drawText (juce::String::fromUTF8 ("\xe2\x9c\xb3"),
                    juce::Rectangle<float> (16.0f - 3.0f, markCentreY - 6.5f, 13.0f, 13.0f),
                    juce::Justification::centred);
        g.restoreState();
    }
    else
    {
        g.setColour (theme::accent);
        g.fillRect (juce::Rectangle<float> (16.0f, markCentreY - 3.5f, 7.0f, 7.0f));
    }

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
