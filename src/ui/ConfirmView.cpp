#include "ConfirmView.h"
#include "Common.h"

namespace splittr
{

namespace
{
    constexpr int panelWidth = 372, panelHeight = 140;
    constexpr int panelPad = 18;
    constexpr int actionGap = 12;
} // namespace

void ConfirmView::setPendingFile (const juce::File& file)
{
    const auto line = file.getFileName().toUpperCase();

    if (line != fileLine)
    {
        fileLine = line;
        repaint();
    }
}

juce::Rectangle<int> ConfirmView::panelBounds() const
{
    return juce::Rectangle<int> (panelWidth, panelHeight).withCentre (getLocalBounds().getCentre());
}

void ConfirmView::paint (juce::Graphics& g)
{
    // Scrim: the stems underneath stay faintly visible, so it is clear what is
    // about to be replaced.
    g.setColour (theme::bg.withAlpha (0.90f));
    g.fillAll();

    const auto panel = panelBounds();

    g.setColour (theme::bg);
    g.fillRect (panel);
    g.setColour (theme::bgPanelAlt);
    g.fillRect (panel);
    g.setColour (theme::lineStrong);
    g.drawRect (panel, 1);

    auto copy = panel.reduced (panelPad);

    g.setColour (theme::text);
    g.setFont (theme::mono (13.0f, 0.04f));
    g.drawText ("start a new split?", copy.removeFromTop (21), juce::Justification::centredLeft);

    g.setColour (theme::text50);
    g.drawText ("the current stems will be replaced.", copy.removeFromTop (21),
                juce::Justification::centredLeft);

    if (fileLine.isNotEmpty())
    {
        copy.removeFromTop (6);
        g.setColour (theme::text35);
        g.setFont (theme::mono (9.5f, 0.08f));
        g.drawText (juce::String::fromUTF8 ("\xe2\x86\x92 ") + fileLine, copy.removeFromTop (13),
                    juce::Justification::centredLeft);
    }

    // --- footer actions ---
    auto footer = panel.reduced (panelPad).removeFromBottom (ui::TagButton::idealHeight);
    const auto font = theme::mono (9.5f, 0.08f);

    const auto drawAction = [&] (const juce::String& label, int actionId,
                                 juce::Colour border, juce::Colour textColour)
    {
        const juce::Rectangle<int> r (footer.getX(), footer.getY(),
                                      (int) std::ceil (ui::textWidth (font, label)) + 24,
                                      ui::TagButton::idealHeight);

        g.setColour (border);
        g.drawRect (r, 1);

        g.setColour (hoveredAction == actionId ? theme::text : textColour);
        g.setFont (font);
        g.drawText (label, r, juce::Justification::centred);

        footer.removeFromLeft (r.getWidth() + actionGap);
        return r;
    };

    confirmBounds = drawAction ("CONFIRM", 1, theme::accentTint40, theme::accentSoft);
    cancelBounds = drawAction ("CANCEL", 2, theme::line, theme::text55);
}

void ConfirmView::mouseUp (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (confirmBounds.contains (p))
    {
        if (onConfirm != nullptr)
            onConfirm();

        return;
    }

    // CANCEL, or a click on the scrim outside the panel — the safe way out.
    if ((cancelBounds.contains (p) || ! panelBounds().contains (p)) && onCancel != nullptr)
        onCancel();
}

void ConfirmView::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    const auto action = confirmBounds.contains (p) ? 1
                        : cancelBounds.contains (p) ? 2
                                                    : 0;

    if (action != hoveredAction)
    {
        hoveredAction = action;
        setMouseCursor (action != 0 ? juce::MouseCursor::PointingHandCursor
                                    : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void ConfirmView::mouseExit (const juce::MouseEvent&)
{
    if (hoveredAction != 0)
    {
        hoveredAction = 0;
        repaint();
    }
}

} // namespace splittr
