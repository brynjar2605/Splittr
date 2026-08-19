#include "IdleDropView.h"
#include "Theme.h"

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
}

void IdleDropView::mouseUp (const juce::MouseEvent& e)
{
    if (onBrowse != nullptr && getLocalBounds().contains (e.getPosition()))
        onBrowse();
}

} // namespace splittr
