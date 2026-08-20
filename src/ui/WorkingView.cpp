#include "WorkingView.h"
#include "Common.h"
#include "Ember.h"

namespace splittr
{

namespace
{
    constexpr int tableWidth = 196;
    constexpr int padX = 16, padY = 14, gap = 14;

    struct SourceRow
    {
        const char* name;
        int peakIndex; // into snapshot.sourcePeaksDb (drums,bass,other,vocals)
        const char* role;
    };

    // Design order: vocals first.
    constexpr SourceRow sourceRows[] = {
        { "vocals", 3, "stem 1" },
        { "drums", 0, "sum \xe2\x86\x92 stem 2" },
        { "bass", 1, "sum \xe2\x86\x92 stem 2" },
        { "other", 2, "sum \xe2\x86\x92 stem 2" },
    };
} // namespace

WorkingView::WorkingView()
{
    addAndMakeVisible (bar);
}

void WorkingView::update (double clockSeconds, const EngineSnapshot& s)
{
    snap = s;
    clock = clockSeconds;
    bar.update (clockSeconds, s.progress);
    repaint();
}

void WorkingView::resized()
{
    auto table = getLocalBounds().reduced (padX, padY).removeFromRight (tableWidth);

    // footer block: 8px padding, label row 13, 6 gap, bar 20, 6 gap, caption 11
    const auto barY = table.getBottom() - 8 - 11 - 6 - MatrixBar::barHeight;
    bar.setBounds (table.getX() + 8, barY, table.getWidth() - 16, MatrixBar::barHeight);
}

void WorkingView::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().reduced (padX, padY);
    auto table = area.removeFromRight (tableWidth);
    auto left = area.withTrimmedRight (gap);

    // ===== left column =====
    auto ruler = left.removeFromTop (12);
    left.removeFromTop (8);
    auto ticks = left.removeFromBottom (9);
    left.removeFromBottom (8);
    auto box = left; // waveform box fills the rest

    g.setColour (theme::text45);
    g.setFont (theme::mono (9.0f, 0.1f));
    g.drawText ("0:00", ruler, juce::Justification::centredLeft);

    auto centreLabel = snap.sourceFile.getFileName().toUpperCase();
    if (snap.bpm > 0.0)
        centreLabel += juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (snap.bpm, 0) + " BPM";
    if (snap.key.isNotEmpty())
        centreLabel += juce::String::fromUTF8 (" \xc2\xb7 ") + snap.key;

    g.drawText (centreLabel, ruler, juce::Justification::centred);
    g.drawText (ui::mmss (snap.sourceDuration), ruler, juce::Justification::centredRight);

    // waveform box: panel + 13px grid
    g.setColour (theme::bgPanel);
    g.fillRect (box);

    g.setColour (theme::gridLine);
    for (int x = box.getX() + 13; x < box.getRight(); x += 13)
        g.fillRect (x, box.getY(), 1, box.getHeight());
    for (int y = box.getY() + 13; y < box.getBottom(); y += 13)
        g.fillRect (box.getX(), y, box.getWidth(), 1);

    g.setColour (theme::lineStrong);
    g.drawRect (box, 1);

    // 60 bars, 1px gap, centred vertically, inner padding 0 4px. Columns the
    // HEAD has already burned through get the ember flicker in green; the ones
    // ahead of it stay exactly as they were.
    {
        auto inner = box.reduced (5, 1).toFloat();
        const int cols = (int) snap.sourceWaveform.size();

        if (cols > 0)
        {
            const auto colW = (inner.getWidth() - (float) (cols - 1)) / (float) cols;

            // The playhead is drawn across the whole box, so express it in the
            // bar strip's coordinates or the flame lags the line.
            const auto headX = (float) box.getX()
                               + juce::jlimit (0.0f, 1.0f, snap.progress) * (float) box.getWidth();
            const auto head = (headX - inner.getX()) / juce::jmax (1.0f, inner.getWidth());

            for (int i = 0; i < cols; ++i)
            {
                const auto h = juce::jmax (1.0f, snap.sourceWaveform[(size_t) i] * inner.getHeight());
                const auto x = inner.getX() + (float) i * (colW + 1.0f);
                const auto y = inner.getCentreY() - h * 0.5f;

                const auto centre = ((float) i + 0.5f) / (float) cols;
                const auto ramp = ui::emberRamp (centre, head);

                g.setColour (theme::text55.withMultipliedAlpha (1.0f - 0.75f * ramp));
                g.fillRect (x, y, colW, h);

                if (ramp > 0.0f)
                {
                    g.setColour (theme::scanEmber.withAlpha (
                        ui::emberAlpha (ui::emberFor (i), clock, ramp, 0.0f)));
                    g.fillRect (x, y, colW, h);
                }
            }
        }
    }

    // playhead + HEAD label (x clamped to <= 84%)
    {
        const auto p = juce::jlimit (0.0f, 1.0f, snap.progress);
        const auto x = (float) box.getX() + p * (float) box.getWidth();

        g.setColour (theme::accentMid);
        g.fillRect (juce::Rectangle<float> (x, (float) box.getY(), 1.0f, (float) box.getHeight()));

        const auto labelX = (float) box.getX() + juce::jmin (p, 0.84f) * (float) box.getWidth();
        const auto font = theme::mono (8.0f, 0.06f);
        const auto labelW = ui::textWidth (font, "HEAD") + 6.0f;

        g.setColour (theme::bg);
        g.fillRect (labelX, (float) box.getY() + 3.0f, labelW, 12.0f);
        g.setColour (theme::accentSoft);
        g.setFont (font);
        g.drawText ("HEAD", juce::Rectangle<float> (labelX + 3.0f, (float) box.getY() + 3.0f, labelW, 12.0f),
                    juce::Justification::centredLeft);
    }

    // tick strip: 1px marks every 13px
    g.setColour (theme::text50);
    for (int x = ticks.getX(); x < ticks.getRight(); x += 13)
        g.fillRect (x, ticks.getY(), 1, ticks.getHeight());

    // ===== right column: source table =====
    g.setColour (theme::bgPanel);
    g.fillRect (table);
    g.setColour (theme::lineStrong);
    g.drawRect (table, 1);

    auto rows = table.reduced (1);

    auto header = rows.removeFromTop (24);
    g.setColour (theme::lineStrong);
    g.fillRect (header.getX(), header.getBottom() - 1, header.getWidth(), 1);
    g.setColour (theme::text50);
    g.setFont (theme::mono (8.5f, 0.12f));
    g.drawText ("SOURCE", header.reduced (8, 0), juce::Justification::centredLeft);
    g.drawText ("PEAK", header.reduced (8, 0), juce::Justification::centredRight);

    for (const auto& row : sourceRows)
    {
        auto r = rows.removeFromTop (36);

        g.setColour (theme::lineSoft10);
        g.fillRect (r.getX(), r.getBottom() - 1, r.getWidth(), 1);

        auto inner = r.reduced (8, 7);
        auto nameLine = inner.removeFromTop (12);

        g.setFont (theme::mono (10.0f));
        g.setColour (theme::text);
        g.drawText (row.name, nameLine, juce::Justification::centredLeft);

        g.setColour (theme::text55);
        const auto peak = snap.peaksKnown
                              ? juce::String (snap.sourcePeaksDb[(size_t) row.peakIndex], 1) + " dB"
                              : juce::String::fromUTF8 ("\xe2\x80\x94");
        g.drawText (peak, nameLine, juce::Justification::centredRight);

        inner.removeFromTop (2);
        g.setFont (theme::mono (8.0f, 0.08f));
        g.setColour (theme::text40);
        g.drawText (juce::String::fromUTF8 (row.role), inner, juce::Justification::centredLeft);
    }

    // footer block
    auto footer = table.reduced (1).removeFromBottom (8 + 13 + 6 + MatrixBar::barHeight + 6 + 11 + 8)
                      .reduced (8, 8);

    auto labelRow = footer.removeFromTop (13);
    g.setFont (theme::mono (9.0f, 0.08f));
    g.setColour (theme::text60);
    g.drawText ("INFERENCE", labelRow, juce::Justification::centredLeft);
    g.setColour (theme::accentSoft);
    g.drawText (juce::String ((int) std::round (snap.progress * 100.0f)) + "%",
                labelRow, juce::Justification::centredRight);

    auto caption = footer.removeFromBottom (11);
    g.setFont (theme::mono (8.0f));
    g.setColour (theme::text40);
    g.drawText ("seg " + juce::String (snap.segment) + "/" + juce::String (snap.segmentCount),
                caption, juce::Justification::centredLeft);

    if (snap.etaSeconds >= 0.0)
        g.drawText ("eta " + juce::String ((int) std::round (snap.etaSeconds)) + "s",
                    caption, juce::Justification::centredRight);
}

} // namespace splittr
