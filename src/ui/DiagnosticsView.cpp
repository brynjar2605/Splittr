#include "DiagnosticsView.h"
#include "Common.h"

namespace splittr
{

namespace
{
    constexpr int panelMargin = 26;
    constexpr int panelPad = 18;
    constexpr int titleHeight = 21, titleGap = 8;
    constexpr int footerGap = 10;
    constexpr int lineHeight = 12;
    constexpr float textSize = 9.0f;
} // namespace

void DiagnosticsView::show (const juce::String& newReport)
{
    report = newReport;

    lines.clear();
    lines.addLines (report);

    scrollY = 0.0f;
    copied = false;
    repaint();
}

juce::Rectangle<int> DiagnosticsView::panelBounds() const
{
    return getLocalBounds().reduced (panelMargin);
}

juce::Rectangle<int> DiagnosticsView::listBounds() const
{
    auto body = panelBounds().reduced (panelPad);
    body.removeFromTop (titleHeight + titleGap);
    body.removeFromBottom (ui::TagButton::idealHeight + footerGap);
    return body;
}

int DiagnosticsView::maxScroll() const
{
    return juce::jmax (0, lines.size() * lineHeight - listBounds().getHeight());
}

void DiagnosticsView::paint (juce::Graphics& g)
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
    g.drawText ("diagnostics", body.removeFromTop (titleHeight), juce::Justification::centredLeft);

    // --- the report, wheel-scrolled, clipped to its own box ---
    {
        const auto list = listBounds();
        const juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (list);

        g.setFont (theme::mono (textSize));

        const auto first = juce::jmax (0, (int) (scrollY / (float) lineHeight));
        const auto last = juce::jmin (lines.size() - 1,
                                      first + list.getHeight() / lineHeight + 1);

        for (int i = first; i <= last; ++i)
        {
            const auto& text = lines[i];
            const auto y = list.getY() + i * lineHeight - (int) scrollY;

            // Section headings carry their own weight; the rest is body text.
            g.setColour (text.startsWith ("--") ? theme::accentMid : theme::text55);
            g.drawText (text, list.getX(), y, list.getWidth(), lineHeight,
                        juce::Justification::centredLeft);
        }

        // A hairline where content continues past the box, LibraryView-style.
        if (maxScroll() > 0)
        {
            g.setColour (theme::lineSoft15);

            if (scrollY > 0.5f)
                g.fillRect (list.getX(), list.getY(), list.getWidth(), 1);

            if (scrollY < (float) maxScroll() - 0.5f)
                g.fillRect (list.getX(), list.getBottom() - 1, list.getWidth(), 1);
        }
    }

    // --- footer: COPY REPORT + hint, CLOSE on the right ---
    auto footer = panel.reduced (panelPad).removeFromBottom (ui::TagButton::idealHeight);

    copyBounds = ui::drawTagAction (g, copied ? "COPIED" : "COPY REPORT",
                                    footer.getX(), footer.getY(),
                                    hoveredAction == 1 || copied,
                                    copied ? theme::accentMid : theme::line,
                                    copied ? theme::accentPale : theme::text55);

    g.setColour (theme::text40);
    g.setFont (theme::mono (8.0f, 0.08f));
    g.drawText (juce::String::fromUTF8 ("paste into a bug report \xc2\xb7 nothing is sent anywhere"),
                footer.withTrimmedLeft (copyBounds.getWidth() + 12)
                      .withTrimmedRight (70),
                juce::Justification::centredLeft);

    const auto closeWidth =
        (int) std::ceil (ui::textWidth (theme::mono (9.5f, 0.08f), "CLOSE")) + 24;

    closeBounds = ui::drawTagAction (g, "CLOSE",
                                     footer.getRight() - closeWidth, footer.getY(),
                                     hoveredAction == 2, theme::line, theme::text55);
}

void DiagnosticsView::mouseUp (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (copyBounds.contains (p))
    {
        juce::SystemClipboard::copyTextToClipboard (report);
        copied = true;
        repaint();
        return;
    }

    if ((closeBounds.contains (p) || ! panelBounds().contains (p)) && onClose != nullptr)
        onClose();
}

void DiagnosticsView::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    const auto action = copyBounds.contains (p) ? 1 : closeBounds.contains (p) ? 2 : 0;

    if (action != hoveredAction)
    {
        hoveredAction = action;

        // Moving off the button lets COPIED settle back to the verb, so the
        // flash reads as an acknowledgement rather than a renamed control.
        if (action != 1)
            copied = false;

        repaint();
    }
}

void DiagnosticsView::mouseExit (const juce::MouseEvent&)
{
    hoveredAction = 0;
    copied = false;
    repaint();
}

void DiagnosticsView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    scrollY = juce::jlimit (0.0f, (float) maxScroll(), scrollY - wheel.deltaY * 240.0f);
    repaint();
}

} // namespace splittr
