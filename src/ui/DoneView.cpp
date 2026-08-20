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

    // DRAG ALL's dot grid: the rows' grip at 3 rows instead of 5, which is all
    // a 13px footer has room for. Same 4px pitch, same 1.5px dots.
    constexpr int dragGripWidth = 10, dragGripGap = 6;

    // A press that never travelled this far is a click on a footer action;
    // beyond it, the gesture belongs to the file drag — the rows' threshold.
    constexpr int dragThreshold = 6;
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

    // Rebuilt every poll rather than with the rows: reopening a library entry
    // can keep the same stem names while pointing at different files.
    stemPaths.clear();

    for (const auto& stem : s.stems)
        stemPaths.add (stem.wav.getFullPathName());

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

    // Leftmost: the whole session in one gesture. A grip rather than a button,
    // because it is dragged and not clicked — the rows carry the same dot grid.
    {
        const auto gripLeft = x;
        const auto dotX = (float) x;
        const auto dotY = (float) footer.getCentreY() - 4.75f;

        g.setColour (hoveredAction == 5 ? theme::text : theme::accentSoft);

        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                g.fillEllipse (dotX + (float) col * 4.0f, dotY + (float) row * 4.0f, 1.5f, 1.5f);

        x += dragGripWidth + dragGripGap;
        dragAllBounds = drawAction ("DRAG ALL", 5, theme::accentSoft).withLeft (gripLeft);
    }

    // The one global transport control for every stem.
    playBounds = drawAction (playing ? "PAUSE" : "PLAY", 4,
                             playing ? theme::accentSoft : theme::text50);

    g.setColour (theme::text50);
    g.drawText (statsText, x, footer.getY(), (int) ui::textWidth (font, statsText) + 2,
                footer.getHeight(), juce::Justification::centredLeft);
    x += (int) ui::textWidth (font, statsText) + 14;

    revealBounds = drawAction ("REVEAL", 1, theme::text50);
    exportBounds = drawAction (juce::String::fromUTF8 ("EXPORT\xe2\x80\xa6"), 2, theme::text50);
    newFileBounds = drawAction ("NEW FILE", 3, theme::text50);

    // The per-row hint keeps its place at the right, but only while the actions
    // leave it a full gutter: a long stats line wins over a reminder.
    const juce::String hint ("DRAG A ROW TO A TRACK");
    const auto hintWidth = (int) ui::textWidth (font, hint) + 2;

    if (x + hintWidth <= footer.getRight())
    {
        g.setColour (theme::accentSoft);
        g.drawText (hint, footer, juce::Justification::centredRight);
    }
}

void DoneView::mouseDown (const juce::MouseEvent&)
{
    draggingAll = false;
}

void DoneView::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingAll
        || e.getDistanceFromDragStart() <= dragThreshold
        || ! dragAllBounds.contains (e.getMouseDownPosition()))
        return;

    juce::StringArray files;

    for (const auto& path : stemPaths)
        if (juce::File (path).existsAsFile())
            files.add (path);

    if (files.isEmpty())
        return;

    draggingAll = true;
    StemRowComponent::beginExternalDrag();

    // One drag, every stem: a DAW that takes a multi-file drop makes a track
    // per file, so a whole session lands in the set at once. Literally all of
    // them — muting is an audition control, not an export filter.
    //
    // The guard is lifted from the end-of-drag callback, so releasing the
    // bundle back over the plugin cannot start a re-separation (issue #8).
    const auto started = juce::DragAndDropContainer::performExternalDragDropOfFiles (
        files, false, this, [] { StemRowComponent::endExternalDrag(); });

    if (! started)
    {
        StemRowComponent::endExternalDrag();
        draggingAll = false;
    }
}

void DoneView::mouseUp (const juce::MouseEvent& e)
{
    if (draggingAll)
    {
        draggingAll = false;
        return; // the gesture was the drag-out, not a click on whatever is under it
    }

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
    const auto action = dragAllBounds.contains (e.getPosition())   ? 5
                        : playBounds.contains (e.getPosition())    ? 4
                        : revealBounds.contains (e.getPosition())  ? 1
                        : exportBounds.contains (e.getPosition())  ? 2
                        : newFileBounds.contains (e.getPosition()) ? 3
                                                                   : 0;
    if (action != hoveredAction)
    {
        hoveredAction = action;

        // DRAG ALL is a drag, so it gets the rows' cursor rather than a click's.
        setMouseCursor (action == 5   ? juce::MouseCursor::DraggingHandCursor
                        : action != 0 ? juce::MouseCursor::PointingHandCursor
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
