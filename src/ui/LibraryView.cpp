#include "LibraryView.h"
#include "Common.h"

namespace splittr
{

namespace
{
    constexpr int panelWidth = 460, panelHeight = 280;
    constexpr int panelPad = 18;
    constexpr int titleHeight = 21, titleGap = 10;
    constexpr int footerGap = 12;
    constexpr int rowHeight = 34, rowPad = 10;
    constexpr int deleteWidth = 20, deleteHeight = 16;

    juce::String metaLineFor (const CacheEntry& e)
    {
        juce::StringArray parts;

        if (e.duration > 0.0)
            parts.add (ui::mmss (e.duration));

        if (e.bpm > 0.0)
            parts.add (juce::String (e.bpm, 0) + " BPM");

        if (e.key.isNotEmpty())
            parts.add (e.key.toUpperCase());

        // Straight from the meta attribute rather than the Quality enum: the
        // entry records whatever engine wrote it, including modes we no longer
        // (or do not yet) have.
        if (e.quality.isNotEmpty())
            parts.add (e.quality.toUpperCase());

        return parts.joinIntoString (juce::String::fromUTF8 (" \xc2\xb7 "));
    }
} // namespace

void LibraryView::refresh()
{
    entries = StemCache::enumerate();

    totalBytes = 0;

    for (const auto& e : entries)
        totalBytes += e.bytes;

    hoveredRow = -1;
    hoveredDelete = false;
    repaint();
}

juce::Rectangle<int> LibraryView::panelBounds() const
{
    return juce::Rectangle<int> (panelWidth, panelHeight).withCentre (getLocalBounds().getCentre());
}

juce::Rectangle<int> LibraryView::listBounds() const
{
    auto body = panelBounds().reduced (panelPad);
    body.removeFromTop (titleHeight + titleGap);
    body.removeFromBottom (ui::TagButton::idealHeight + footerGap);
    return body;
}

juce::Rectangle<int> LibraryView::rowBounds (int index) const
{
    return listBounds().withHeight (rowHeight).translated (0, index * rowHeight);
}

juce::Rectangle<int> LibraryView::deleteBounds (int index) const
{
    const auto row = rowBounds (index);

    return { row.getRight() - rowPad - deleteWidth, row.getY() + (rowHeight - deleteHeight) / 2,
             deleteWidth, deleteHeight };
}

int LibraryView::rowCapacity() const
{
    return juce::jmax (1, listBounds().getHeight() / rowHeight);
}

int LibraryView::rowsShown() const
{
    const auto n = (int) entries.size();
    const auto cap = rowCapacity();

    return n <= cap ? n : cap - 1; // last slot becomes the "+N OLDER" line
}

void LibraryView::paint (juce::Graphics& g)
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

    // --- title ---
    {
        auto title = panel.reduced (panelPad).removeFromTop (titleHeight);

        g.setColour (theme::text);
        g.setFont (theme::mono (13.0f, 0.04f));
        g.drawText ("library", title, juce::Justification::centredLeft);

        g.setColour (theme::text40);
        g.setFont (theme::mono (9.5f, 0.08f));
        g.drawText (juce::String (entries.size()) + (entries.size() == 1 ? " SONG" : " SONGS"),
                    title, juce::Justification::centredRight);
    }

    const auto list = listBounds();

    g.setColour (theme::lineSoft10);
    g.fillRect (list.getX(), list.getY(), list.getWidth(), 1);

    if (entries.empty())
    {
        g.setColour (theme::text35);
        g.setFont (theme::mono (9.5f, 0.08f));
        g.drawText ("nothing split yet", list.withHeight (rowHeight),
                    juce::Justification::centred);
    }

    const auto shown = rowsShown();

    for (int i = 0; i < shown; ++i)
    {
        const auto& entry = entries[(size_t) i];
        const auto row = rowBounds (i);
        const bool hot = hoveredRow == i;

        if (hot)
        {
            g.setColour (theme::accentTint5);
            g.fillRect (row);
        }

        g.setColour (theme::lineSoft10);
        g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);

        auto text = row.reduced (rowPad, 0).withTrimmedRight (deleteWidth + rowPad);

        // name + date share the upper line, meta sits under them
        auto nameRow = text.withY (row.getY() + 4).withHeight (15);

        g.setColour (theme::text35);
        g.setFont (theme::mono (8.5f, 0.08f));
        const auto date = entry.modified.formatted ("%d %b %y").toUpperCase();
        const auto dateWidth = (int) ui::textWidth (theme::mono (8.5f, 0.08f), date) + 4;
        g.drawText (date, nameRow, juce::Justification::centredRight);

        g.setColour (hot && ! hoveredDelete ? theme::accentSoft : theme::text);
        g.setFont (theme::mono (10.5f, 0.04f));
        g.drawText (entry.sourceName.toUpperCase(), nameRow.withTrimmedRight (dateWidth + 10),
                    juce::Justification::centredLeft, true);

        g.setColour (theme::text40);
        g.setFont (theme::mono (8.5f, 0.08f));
        g.drawText (metaLineFor (entry), text.withY (row.getY() + 19).withHeight (12),
                    juce::Justification::centredLeft);

        // ✕ — deletes this entry's stems from disk
        g.setColour (hot && hoveredDelete ? theme::text : theme::text35);
        g.setFont (theme::mono (11.0f));
        g.drawText (juce::String::fromUTF8 ("\xc3\x97"), deleteBounds (i),
                    juce::Justification::centred);
    }

    if (shown < (int) entries.size())
    {
        g.setColour (theme::text35);
        g.setFont (theme::mono (8.5f, 0.08f));
        g.drawText ("+" + juce::String ((int) entries.size() - shown) + " OLDER",
                    rowBounds (shown).reduced (rowPad, 0), juce::Justification::centredLeft);
    }

    // --- footer: what the cache is costing, and the way out ---
    auto footer = panel.reduced (panelPad).removeFromBottom (ui::TagButton::idealHeight);

    closeBounds = ui::drawTagAction (g, "CLOSE", footer.getX(), footer.getY(), hoveredClose,
                                     theme::line, theme::text55);

    g.setColour (theme::text40);
    g.setFont (theme::mono (9.5f, 0.08f));
    g.drawText (juce::File::descriptionOfSizeInBytes (totalBytes).toUpperCase() + " OF "
                    + juce::File::descriptionOfSizeInBytes (StemCache::defaultMaxBytes).toUpperCase(),
                footer, juce::Justification::centredRight);
}

void LibraryView::mouseUp (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    for (int i = 0; i < rowsShown(); ++i)
    {
        if (! rowBounds (i).contains (p))
            continue;

        if (deleteBounds (i).contains (p))
        {
            // The live session keeps its own file handles by path, so deleting
            // the entry it came from is harmless: the rows simply show up as
            // missing the next time they are used.
            entries[(size_t) i].dir.deleteRecursively();
            refresh();
            return;
        }

        if (onOpenEntry != nullptr)
            onOpenEntry (entries[(size_t) i].dir);

        return;
    }

    // CLOSE, or a click on the scrim outside the panel.
    if ((closeBounds.contains (p) || ! panelBounds().contains (p)) && onClose != nullptr)
        onClose();
}

void LibraryView::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    int row = -1;

    for (int i = 0; i < rowsShown(); ++i)
        if (rowBounds (i).contains (p))
            row = i;

    const bool overDelete = row >= 0 && deleteBounds (row).contains (p);
    const bool overClose = closeBounds.contains (p);

    if (row != hoveredRow || overDelete != hoveredDelete || overClose != hoveredClose)
    {
        hoveredRow = row;
        hoveredDelete = overDelete;
        hoveredClose = overClose;

        setMouseCursor (row >= 0 || overClose ? juce::MouseCursor::PointingHandCursor
                                              : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void LibraryView::mouseExit (const juce::MouseEvent&)
{
    if (hoveredRow >= 0 || hoveredClose)
    {
        hoveredRow = -1;
        hoveredDelete = false;
        hoveredClose = false;
        repaint();
    }
}

} // namespace splittr
