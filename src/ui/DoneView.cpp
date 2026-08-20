#include "DoneView.h"
#include "Common.h"

namespace splittr
{

namespace
{
    constexpr int padX = 16, padY = 14;
    constexpr int footerHeight = 13;

    // Five rows keep the design's 53px and its 6px gutter; seven only fit by
    // halving the gutter, which lands them at ~40px.
    constexpr int wideRowGap = 6, tightRowGap = 3;
    constexpr int tightAbove = 5; // row counts above this use the tight gutter
} // namespace

DoneView::DoneView() = default;

void DoneView::rebuildRows (const std::vector<EngineSnapshot::StemInfo>& stems)
{
    juce::StringArray names;

    for (const auto& stem : stems)
        names.add (stem.name);

    if (names == rowNames)
        return;

    rowNames = names;
    rows.clear();

    for (int i = 0; i < names.size(); ++i)
    {
        auto* r = rows.add (new StemRowComponent (names[i].toUpperCase(),
                                                  stems[(size_t) i].kind == stemVocals));

        r->onMuteToggle = [this, i] { if (onMuteToggle != nullptr) onMuteToggle (i); };
        r->onSeek = [this, i] (double f) { if (onSeek != nullptr) onSeek (i, f); };

        addAndMakeVisible (r);
    }

    resized();
}

void DoneView::update (const EngineSnapshot& s, bool isPlaying)
{
    const auto dot = juce::String::fromUTF8 (" \xc2\xb7 ");

    rebuildRows (s.stems);

    playing = isPlaying;
    statsText.clear();

    if (s.bpm > 0.0)
        statsText << juce::String (s.bpm, 0) << " BPM" << dot;
    if (s.key.isNotEmpty())
        statsText << s.key << dot;

    statsText << juce::String (s.realtimeFactor, 2) + "x RT";
    repaint();
}

void DoneView::resized()
{
    auto area = getLocalBounds().reduced (padX, padY);

    const auto count = rows.size();

    if (count <= 0)
        return;

    // Rows and their gutters share what the footer leaves; the design height
    // is the ceiling, so five rows are unchanged and seven shrink to fit.
    rowGap = count > tightAbove ? tightRowGap : wideRowGap;
    rowHeight = juce::jlimit (24, StemRowComponent::maxRowHeight,
                              (area.getHeight() - footerHeight - count * rowGap) / count);

    for (auto* r : rows)
    {
        r->setBounds (area.removeFromTop (rowHeight));
        area.removeFromTop (rowGap);
    }
}

void DoneView::paint (juce::Graphics& g)
{
    auto footer = getLocalBounds().reduced (padX, padY).removeFromBottom (footerHeight);

    const auto font = theme::mono (9.0f, 0.08f);
    g.setFont (font);

    auto x = footer.getX();

    const auto drawAction = [&] (const juce::String& label, int actionId,
                                 juce::Colour idle) -> juce::Rectangle<int>
    {
        const juce::Rectangle<int> r (x, footer.getY(), (int) ui::textWidth (font, label) + 2,
                                      footer.getHeight());
        g.setColour (hoveredAction == actionId ? theme::text : idle);
        g.drawText (label, r, juce::Justification::centredLeft);
        x = r.getRight() + 14;
        return r;
    };

    // Leftmost: the one global transport control for every stem.
    playBounds = drawAction (playing ? "PAUSE" : "PLAY", 4,
                             playing ? theme::accentSoft : theme::text50);

    g.setColour (theme::text50);
    g.drawText (statsText, x, footer.getY(), (int) ui::textWidth (font, statsText) + 2,
                footer.getHeight(), juce::Justification::centredLeft);
    x += (int) ui::textWidth (font, statsText) + 14;

    revealBounds = drawAction ("REVEAL", 1, theme::text50);
    exportBounds = drawAction (juce::String::fromUTF8 ("EXPORT\xe2\x80\xa6"), 2, theme::text50);
    newFileBounds = drawAction ("NEW FILE", 3, theme::text50);

    g.setColour (theme::accentSoft);
    g.drawText ("DRAG A ROW TO A TRACK", footer, juce::Justification::centredRight);
}

void DoneView::mouseUp (const juce::MouseEvent& e)
{
    if (playBounds.contains (e.getPosition()) && onPlayToggle != nullptr)
        onPlayToggle();
    else if (revealBounds.contains (e.getPosition()) && onReveal != nullptr)
        onReveal();
    else if (exportBounds.contains (e.getPosition()) && onExport != nullptr)
        onExport();
    else if (newFileBounds.contains (e.getPosition()) && onNewFile != nullptr)
        onNewFile();
}

void DoneView::mouseMove (const juce::MouseEvent& e)
{
    const auto action = playBounds.contains (e.getPosition())      ? 4
                        : revealBounds.contains (e.getPosition())  ? 1
                        : exportBounds.contains (e.getPosition())  ? 2
                        : newFileBounds.contains (e.getPosition()) ? 3
                                                                   : 0;
    if (action != hoveredAction)
    {
        hoveredAction = action;
        setMouseCursor (action != 0 ? juce::MouseCursor::PointingHandCursor
                                    : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void DoneView::mouseExit (const juce::MouseEvent&)
{
    if (hoveredAction != 0)
    {
        hoveredAction = 0;
        repaint();
    }
}

} // namespace splittr
