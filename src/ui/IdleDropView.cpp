#include "IdleDropView.h"
#include "Common.h"

namespace splittr
{

IdleDropView::IdleDropView()
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void IdleDropView::setDragOver (bool over)
{
    if (dragOver != over)
    {
        dragOver = over;
        repaint();
    }
}

void IdleDropView::setErrorMessage (const juce::String& message)
{
    if (errorMessage != message)
    {
        errorMessage = message;
        repaint();
    }
}

void IdleDropView::setQuality (Quality q)
{
    if (quality != q)
    {
        quality = q;
        repaint();
    }
}

void IdleDropView::paint (juce::Graphics& g)
{
    auto panel = getLocalBounds().reduced (16, 14).toFloat();

    g.setColour (theme::accentTint5);
    g.fillRect (panel);

    // 1px dashed border; drag-over brightens to full purple.
    {
        juce::Path outline;
        outline.addRectangle (panel.reduced (0.5f));

        juce::Path dashed;
        const float dashes[] = { 4.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);

        g.setColour (dragOver ? theme::accent : theme::accentTint40);
        g.fillPath (dashed);
    }

    auto centre = panel.getCentre();

    g.setColour (theme::accentSoft);
    g.setFont (theme::mono (12.5f, 0.1f));
    g.drawText ("drop an audio file into this window",
                juce::Rectangle<float> (panel.getX(), centre.y - 16.0f, panel.getWidth(), 15.0f),
                juce::Justification::centred);

    g.setColour (theme::text35);
    g.setFont (theme::mono (9.5f));
    g.drawText (juce::String::fromUTF8 ("wav \xc2\xb7 aiff \xc2\xb7 mp3 \xc2\xb7 flac"),
                juce::Rectangle<float> (panel.getX(), centre.y + 2.0f, panel.getWidth(), 12.0f),
                juce::Justification::centred);

    if (errorMessage.isNotEmpty())
    {
        g.setColour (theme::text50);
        g.setFont (theme::mono (9.5f, 0.08f));
        g.drawText (errorMessage,
                    juce::Rectangle<float> (panel.getX(), centre.y + 26.0f, panel.getWidth(), 12.0f),
                    juce::Justification::centred);
    }

    // --- ENGINE quality toggle, pinned to the panel bottom ---
    {
        const auto labelFont = theme::mono (8.5f, 0.12f);
        const auto optionFont = theme::mono (9.5f, 0.1f);

        const auto rowY = (int) (panel.getBottom() - 42.0f);
        const auto captionY = rowY + 17;

        const auto labelW = ui::textWidth (labelFont, "ENGINE");
        const auto fastW = ui::textWidth (optionFont, "FAST");
        const auto bestW = ui::textWidth (optionFont, "BEST");
        const auto dotW = ui::textWidth (optionFont, juce::String::fromUTF8 ("\xc2\xb7"));
        const auto gap = 10.0f;

        const auto totalW = labelW + gap + fastW + gap + dotW + gap + bestW;
        auto x = (float) getWidth() / 2.0f - totalW / 2.0f;

        g.setFont (labelFont);
        g.setColour (theme::text35);
        g.drawText ("ENGINE", (int) x, rowY, (int) labelW + 2, 13, juce::Justification::centredLeft);
        x += labelW + gap;

        fastBounds = { (int) x - 4, rowY - 4, (int) fastW + 8, 21 };
        g.setFont (optionFont);
        g.setColour (quality == Quality::fast ? theme::accentSoft
                                              : (hoveredOption == 1 ? theme::text60 : theme::text35));
        g.drawText ("FAST", (int) x, rowY, (int) fastW + 2, 13, juce::Justification::centredLeft);
        x += fastW + gap;

        g.setColour (theme::text35);
        g.drawText (juce::String::fromUTF8 ("\xc2\xb7"), (int) x, rowY, (int) dotW + 2, 13,
                    juce::Justification::centredLeft);
        x += dotW + gap;

        bestBounds = { (int) x - 4, rowY - 4, (int) bestW + 8, 21 };
        g.setColour (quality == Quality::best ? theme::accentSoft
                                              : (hoveredOption == 2 ? theme::text60 : theme::text35));
        g.drawText ("BEST", (int) x, rowY, (int) bestW + 2, 13, juce::Justification::centredLeft);

        g.setFont (theme::mono (8.0f, 0.08f));
        g.setColour (theme::text35);
        g.drawText (quality == Quality::fast
                        ? juce::String::fromUTF8 ("htdemucs \xc2\xb7 about realtime")
                        : juce::String::fromUTF8 ("fine-tuned \xc3\x97 4 models \xc2\xb7 best quality \xc2\xb7 ~4\xc3\x97 slower"),
                    (int) panel.getX(), captionY, (int) panel.getWidth(), 11,
                    juce::Justification::centred);
    }
}

void IdleDropView::mouseUp (const juce::MouseEvent& e)
{
    if (fastBounds.contains (e.getPosition()))
    {
        if (onQualityChange != nullptr)
            onQualityChange (Quality::fast);
        return;
    }

    if (bestBounds.contains (e.getPosition()))
    {
        if (onQualityChange != nullptr)
            onQualityChange (Quality::best);
        return;
    }

    if (onBrowse != nullptr && getLocalBounds().contains (e.getPosition()))
        onBrowse();
}

void IdleDropView::mouseMove (const juce::MouseEvent& e)
{
    const auto option = fastBounds.contains (e.getPosition()) ? 1
                        : bestBounds.contains (e.getPosition()) ? 2
                                                                : 0;
    if (option != hoveredOption)
    {
        hoveredOption = option;
        repaint();
    }
}

void IdleDropView::mouseExit (const juce::MouseEvent&)
{
    if (hoveredOption != 0)
    {
        hoveredOption = 0;
        repaint();
    }
}

} // namespace splittr
