#include "IdleDropView.h"
#include "Common.h"
#include "engine/StemCache.h"

namespace splittr
{

namespace
{
    constexpr int padX = 16;
    constexpr int topRowY = 14;              // ENGINE row
    constexpr int bottomRowHeight = ui::TagButton::idealHeight; // LIBRARY row
    constexpr int bottomRowInset = 14;       // its distance from the bottom edge
    constexpr int panelGap = 8;              // between the panel and that row
    constexpr int panelTop = 44;             // below the ENGINE row
    constexpr int panelBottomInset = bottomRowInset + bottomRowHeight + panelGap;

    // The count is the point of the promotion: it says there is something
    // behind the word. An empty cache has nothing to advertise, so the tag
    // falls back to the bare label rather than promising "· 0" — the entry
    // point stays put, the scent only appears once there is a trail.
    juce::String libraryLabel (int count)
    {
        if (count <= 0)
            return "LIBRARY";

        return juce::String::fromUTF8 ("LIBRARY \xc2\xb7 ") + juce::String (count);
    }

    // ui::drawTagAction takes a left edge, so centring one means measuring it
    // the same way it does: label plus 12px of padding either side.
    int tagWidth (const juce::String& label)
    {
        return (int) std::ceil (ui::textWidth (theme::mono (9.5f, 0.08f), label)) + 24;
    }
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

void IdleDropView::refreshLibraryCount()
{
    const auto count = (int) StemCache::enumerate().size();

    if (libraryCount != count)
    {
        libraryCount = count;
        repaint();
    }
}

void IdleDropView::visibilityChanged()
{
    // Once per entry into the idle state: a split that just finished, or the
    // editor opening. Nothing else can add an entry while this view is up.
    if (isVisible())
        refreshLibraryCount();
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
    // A hairline tag in the CLOSE/CANCEL idiom rather than a bare text action,
    // centred under the panel so it lands in the eye path on the way down from
    // the drop target instead of hiding in a corner. Neutral line/text55: the
    // accent belongs to the panel, so this reads as the second way in.
    {
        const auto label = libraryLabel (libraryCount);

        libraryBounds = ui::drawTagAction (g, label, (getWidth() - tagWidth (label)) / 2,
                                           getHeight() - bottomRowInset - bottomRowHeight,
                                           hoveredOption == libraryHoverId,
                                           theme::line, theme::text55);
    }
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
