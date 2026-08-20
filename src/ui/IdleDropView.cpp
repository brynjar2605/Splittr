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

std::vector<juce::Rectangle<int>>
IdleDropView::drawSwitch (juce::Graphics& g, int x, int y, const juce::StringArray& labels,
                          int activeIndex, int hoverIdBase, int cellWidth) const
{
    constexpr int cellH = 22;

    const auto numCells = labels.size();

    std::vector<juce::Rectangle<int>> cells;

    for (int i = 0; i < numCells; ++i)
        cells.push_back ({ x + i * cellWidth, y, cellWidth, cellH });

    g.setColour (theme::accentTint15);

    if (juce::isPositiveAndBelow (activeIndex, numCells))
        g.fillRect (cells[(size_t) activeIndex]);

    g.setColour (theme::lineStrong);
    g.drawRect (juce::Rectangle<int> (x, y, cellWidth * numCells, cellH), 1);

    for (int i = 1; i < numCells; ++i)
        g.fillRect (x + i * cellWidth, y + 1, 1, cellH - 2); // dividers

    g.setFont (theme::mono (9.5f, 0.1f));

    for (int i = 0; i < numCells; ++i)
    {
        g.setColour (i == activeIndex
                         ? theme::accentSoft
                         : (hoveredOption == hoverIdBase + i ? theme::text60 : theme::text40));
        g.drawText (labels[i], cells[(size_t) i], juce::Justification::centred);
    }

    return cells;
}

int IdleDropView::cellAt (const std::vector<juce::Rectangle<int>>& cells, juce::Point<int> p)
{
    for (size_t i = 0; i < cells.size(); ++i)
        if (cells[i].contains (p))
            return (int) i;

    return -1;
}

void IdleDropView::paint (juce::Graphics& g)
{
    const auto labelFont = theme::mono (8.5f, 0.12f);

    const auto drawLabel = [&] (const juce::String& label, int x, int y)
    {
        g.setFont (labelFont);
        g.setColour (theme::text35);
        g.drawText (label, x, y, (int) ui::textWidth (labelFont, label) + 2, 22,
                    juce::Justification::centredLeft);
        return x + (int) ui::textWidth (labelFont, label) + 12;
    };

    // --- ENGINE mode row above the drop panel ---
    {
        const auto x = drawLabel ("ENGINE", 16, 14);
        const auto active = quality == Quality::best      ? 1
                            : quality == Quality::sixStem ? 2
                                                          : 0;

        engineBounds = drawSwitch (g, x, 14, { "FAST", "BEST", "6-STEM" }, active,
                                   engineHoverBase, 54);

        const auto caption = [&]
        {
            switch (quality)
            {
                case Quality::best:
                    return juce::String::fromUTF8 (
                        "fine-tuned \xc3\x97 4 \xc2\xb7 best quality \xc2\xb7 ~4\xc3\x97 slower");
                case Quality::sixStem:
                    // The extra targets are the honest caveat: upstream scores
                    // guitar and piano well below the four standard sources.
                    return juce::String::fromUTF8 (
                        "htdemucs_6s \xc2\xb7 guitar + piano are weaker than the 4 core stems");
                case Quality::fast:
                    break;
            }

            return juce::String::fromUTF8 ("htdemucs \xc2\xb7 about realtime");
        }();

        g.setFont (theme::mono (8.0f, 0.08f));
        g.setColour (theme::text35);
        g.drawText (caption, juce::Rectangle<int> (0, 14, getWidth() - 16, 22),
                    juce::Justification::centredRight);
    }

    // --- THEME switch / ACCENT hue strip below the drop panel ---
    {
        const auto y = getHeight() - 14 - 22;

        const auto x1 = drawLabel ("THEME", 16, y);
        themeBounds = drawSwitch (g, x1, y, { "DARK", "LIGHT" },
                                  theme::currentMode() == theme::Mode::light ? 1 : 0,
                                  themeHoverBase, 54);

        // hue strip: full spectrum, click or drag to pick the accent colour
        constexpr int stripW = 168;
        const auto accentLabelX = getWidth() - 16 - stripW
                                  - ((int) ui::textWidth (labelFont, "ACCENT") + 12);
        const auto x2 = drawLabel ("ACCENT", accentLabelX, y);

        hueStripBounds = { x2, y, stripW, 22 };

        const auto sat = 0.72f;
        const auto bri = theme::currentMode() == theme::Mode::dark ? 0.92f : 0.80f;

        for (int i = 0; i < stripW; ++i)
        {
            g.setColour (juce::Colour::fromHSV ((float) i / (float) stripW, sat, bri, 1.0f));
            g.fillRect (hueStripBounds.getX() + i, y + 1, 1, 20);
        }

        g.setColour (theme::lineStrong);
        g.drawRect (hueStripBounds, 1);

        // marker at the current hue
        const auto markerX = hueStripBounds.getX()
                             + (int) (theme::currentAccentHue() * (float) stripW);
        g.setColour (theme::bg);
        g.fillRect (markerX - 2, y - 2, 5, 26);
        g.setColour (theme::accentMid);
        g.fillRect (markerX - 1, y - 2, 3, 26);
    }

    auto panel = getLocalBounds().withTrimmedTop (30).withTrimmedBottom (30).reduced (16, 14).toFloat();

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

void IdleDropView::sendHue (const juce::MouseEvent& e, bool commit)
{
    if (onAccentHueChange != nullptr)
        onAccentHueChange (juce::jlimit (0.0f, 1.0f,
                                         (float) (e.x - hueStripBounds.getX())
                                             / (float) hueStripBounds.getWidth()),
                           commit);
}

void IdleDropView::mouseDown (const juce::MouseEvent& e)
{
    draggingHue = hueStripBounds.contains (e.getPosition());

    if (draggingHue)
        sendHue (e, false);
}

void IdleDropView::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingHue)
        sendHue (e, false);
}

void IdleDropView::mouseUp (const juce::MouseEvent& e)
{
    if (draggingHue)
    {
        sendHue (e, true); // commit -> persisted
        draggingHue = false;
        return;
    }

    const auto p = e.getPosition();

    if (const auto cell = cellAt (engineBounds, p); cell >= 0)
    {
        static const Quality qualities[] = { Quality::fast, Quality::best, Quality::sixStem };

        if (onQualityChange != nullptr)
            onQualityChange (qualities[cell]);

        return;
    }

    if (const auto cell = cellAt (themeBounds, p); cell >= 0)
    {
        if (onModeChange != nullptr)
            onModeChange (cell == 0 ? theme::Mode::dark : theme::Mode::light);

        return;
    }

    if (onBrowse != nullptr && getLocalBounds().contains (p))
        onBrowse();
}

void IdleDropView::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    const auto engineCell = cellAt (engineBounds, p);
    const auto themeCell = cellAt (themeBounds, p);

    const auto option = engineCell >= 0  ? engineHoverBase + engineCell
                        : themeCell >= 0 ? themeHoverBase + themeCell
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
