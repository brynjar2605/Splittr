#include "IdleDropView.h"
#include "Common.h"

namespace splittr
{

namespace
{
    constexpr int padX = 16;
    constexpr int topRowY = 14;              // ENGINE row
    constexpr int bottomRowHeight = 13;      // LIBRARY row
    constexpr int bottomRowInset = 14;       // its distance from the bottom edge
    constexpr int panelTop = 44;             // below the ENGINE row
    constexpr int panelBottomInset = 35;     // above the LIBRARY row
} // namespace

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
    // --- ENGINE mode row above the drop panel ---
    {
        const auto x = ui::drawFieldLabel (g, "ENGINE", padX, topRowY);

        const auto active = quality == Quality::fast ? 0 : quality == Quality::best ? 1 : 2;

        engineBounds = ui::drawSwitchCells (g, x, topRowY, { "FAST", "BEST", "6-STEM" },
                                            active, hoveredOption, engineHoverBase, 54);

        const auto caption = [&]
        {
            if (quality == Quality::best)
                return juce::String::fromUTF8 ("fine-tuned \xc3\x97 4 \xc2\xb7 best quality \xc2\xb7 ~4\xc3\x97 slower");

            if (quality == Quality::sixStem)
                // Upstream SDR scores guitar and piano well below the four
                // standard sources; say so up front.
                return juce::String::fromUTF8 ("htdemucs_6s \xc2\xb7 guitar + piano are weaker than the 4 core stems");

            return juce::String::fromUTF8 ("htdemucs \xc2\xb7 about realtime");
        }();

        g.setFont (theme::mono (8.0f, 0.08f));
        g.setColour (theme::text35);
        g.drawText (caption,
                    juce::Rectangle<int> (0, topRowY, getWidth() - padX, ui::switchHeight),
                    juce::Justification::centredRight);
    }

    // The panel fills everything between the two rows.
    auto panel = getLocalBounds()
                     .reduced (padX, 0)
                     .withTop (panelTop)
                     .withBottom (getHeight() - panelBottomInset)
                     .toFloat();

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

    // --- LIBRARY: everything ever split, straight out of the stem cache ---
    libraryBounds = ui::drawTextAction (g, "LIBRARY", padX,
                                        getHeight() - bottomRowInset - bottomRowHeight,
                                        bottomRowHeight, hoveredOption == libraryHoverId);
}

void IdleDropView::mouseUp (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (const auto cell = ui::cellAt (engineBounds, p); cell >= 0)
    {
        static const Quality qualities[] = { Quality::fast, Quality::best, Quality::sixStem };

        if (onQualityChange != nullptr)
            onQualityChange (qualities[cell]);

        return;
    }

    if (libraryBounds.contains (p))
    {
        if (onLibrary != nullptr)
            onLibrary();

        return;
    }

    if (onBrowse != nullptr && getLocalBounds().contains (p))
        onBrowse();
}

void IdleDropView::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    auto option = 0;

    if (const auto cell = ui::cellAt (engineBounds, p); cell >= 0)
        option = engineHoverBase + cell;
    else if (libraryBounds.contains (p))
        option = libraryHoverId;

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
