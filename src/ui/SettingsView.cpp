#include "SettingsView.h"
#include "Common.h"

namespace splittr
{

namespace
{
    constexpr int panelWidth = 372, panelHeight = 202;
    constexpr int panelPad = 18;
    constexpr int titleHeight = 21, titleGap = 10;
    constexpr int rowGap = 12;
    constexpr int labelColumn = 76; // controls line up past the field labels
} // namespace

juce::Rectangle<int> SettingsView::panelBounds() const
{
    return juce::Rectangle<int> (panelWidth, panelHeight).withCentre (getLocalBounds().getCentre());
}

void SettingsView::paint (juce::Graphics& g)
{
    g.setColour (theme::bg.withAlpha (0.90f));
    g.fillAll();

    const auto panel = panelBounds();

    g.setColour (theme::bg);
    g.fillRect (panel);
    g.setColour (theme::bgPanelAlt);
    g.fillRect (panel);
    g.setColour (theme::lineStrong);
    g.drawRect (panel, 1);

    auto body = panel.reduced (panelPad);

    g.setColour (theme::text);
    g.setFont (theme::mono (13.0f, 0.04f));
    g.drawText ("settings", body.removeFromTop (titleHeight), juce::Justification::centredLeft);

    body.removeFromTop (titleGap);

    // --- THEME ---
    {
        const auto row = body.removeFromTop (ui::switchHeight);

        ui::drawFieldLabel (g, "THEME", row.getX(), row.getY());

        std::tie (darkBounds, lightBounds) =
            ui::drawSwitch (g, row.getX() + labelColumn, row.getY(), "DARK", "LIGHT",
                            theme::currentMode() == theme::Mode::light, hoveredAction, 1, 2, 54);
    }

    body.removeFromTop (rowGap);

    // --- ACCENT: full spectrum, click or drag to retune every accent token ---
    {
        const auto row = body.removeFromTop (ui::switchHeight);

        ui::drawFieldLabel (g, "ACCENT", row.getX(), row.getY());

        hueStripBounds = { row.getX() + labelColumn, row.getY(),
                           row.getWidth() - labelColumn, ui::switchHeight };

        ui::drawHueStrip (g, hueStripBounds);
    }

    body.removeFromTop (rowGap);

    // --- FIRE: the burn grain on playing/scanning waveforms ---
    {
        const auto row = body.removeFromTop (ui::switchHeight);

        ui::drawFieldLabel (g, "FIRE", row.getX(), row.getY());

        std::tie (fineBounds, coarseBounds) =
            ui::drawSwitch (g, row.getX() + labelColumn, row.getY(), "FINE", "COARSE",
                            ! theme::fineFire(), hoveredAction, 4, 5, 54);
    }

    // --- footer ---
    auto footer = panel.reduced (panelPad).removeFromBottom (ui::TagButton::idealHeight);

    closeBounds = ui::drawTagAction (g, "CLOSE", footer.getX(), footer.getY(),
                                     hoveredAction == 3, theme::line, theme::text55);

    g.setColour (theme::text40);
    g.setFont (theme::mono (9.5f, 0.08f));
    g.drawText (juce::String::fromUTF8 ("\xe2\x86\x92 ~/\xe2\x80\xa6/Splittr/Settings.xml"),
                footer, juce::Justification::centredRight);
}

void SettingsView::sendHue (const juce::MouseEvent& e, bool commit)
{
    if (onAccentHueChange != nullptr)
        onAccentHueChange (ui::hueForX (hueStripBounds, e.x), commit);
}

void SettingsView::mouseDown (const juce::MouseEvent& e)
{
    draggingHue = hueStripBounds.contains (e.getPosition());

    if (draggingHue)
        sendHue (e, false); // live preview, not persisted until release
}

void SettingsView::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingHue)
        sendHue (e, false);
}

void SettingsView::mouseUp (const juce::MouseEvent& e)
{
    if (draggingHue)
    {
        sendHue (e, true); // commit -> persisted
        draggingHue = false;
        return;
    }

    const auto p = e.getPosition();

    if (darkBounds.contains (p) || lightBounds.contains (p))
    {
        if (onModeChange != nullptr)
            onModeChange (darkBounds.contains (p) ? theme::Mode::dark : theme::Mode::light);

        return;
    }

    if (fineBounds.contains (p) || coarseBounds.contains (p))
    {
        if (onFireGrainChange != nullptr)
            onFireGrainChange (fineBounds.contains (p));

        return;
    }

    // CLOSE, or a click on the scrim outside the panel.
    if ((closeBounds.contains (p) || ! panelBounds().contains (p)) && onClose != nullptr)
        onClose();
}

void SettingsView::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    const auto action = darkBounds.contains (p)     ? 1
                        : lightBounds.contains (p)  ? 2
                        : closeBounds.contains (p)  ? 3
                        : fineBounds.contains (p)   ? 4
                        : coarseBounds.contains (p) ? 5
                                                    : 0;

    const bool overStrip = hueStripBounds.contains (p);

    if (action != hoveredAction)
    {
        hoveredAction = action;
        repaint();
    }

    setMouseCursor (action != 0 || overStrip ? juce::MouseCursor::PointingHandCursor
                                             : juce::MouseCursor::NormalCursor);
}

void SettingsView::mouseExit (const juce::MouseEvent&)
{
    if (hoveredAction != 0)
    {
        hoveredAction = 0;
        repaint();
    }
}

} // namespace splittr
