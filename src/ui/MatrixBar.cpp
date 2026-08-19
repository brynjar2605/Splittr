#include "MatrixBar.h"
#include "Theme.h"

namespace splittr
{

MatrixBar::MatrixBar (int numColumns) : columns (numColumns)
{
    setInterceptsMouseClicks (false, false);

    cells.resize ((size_t) (columns * rows));

    for (int row = 0; row < rows; ++row)
    {
        for (int col = 0; col < columns; ++col)
        {
            // Deterministic per-cell hash so no two neighbours are in phase.
            juce::uint32 h = (juce::uint32) col * 73856093u ^ (juce::uint32) row * 19349663u;
            h ^= h >> 13;
            h *= 0x5bd1e995u;
            h ^= h >> 15;

            auto& cell = cells[(size_t) (row * columns + col)];
            cell.baseAlpha = 0.3f + 0.7f * (float) (h & 0xffu) / 255.0f;
            cell.period = 0.34f + 0.90f * (float) ((h >> 8) & 0xffu) / 255.0f;
            cell.phase = (float) ((h >> 16) & 0xffu) / 255.0f;
        }
    }
}

void MatrixBar::update (double clockSeconds, float progress01)
{
    clock = clockSeconds;
    progress = juce::jlimit (0.0f, 1.0f, progress01);
    repaint();
}

void MatrixBar::paint (juce::Graphics& g)
{
    const auto w = (float) getWidth();
    const auto cellW = (w - (float) (columns - 1)) / (float) columns;

    static constexpr float emberSteps[] = { 1.0f, 0.32f, 0.86f, 0.5f };

    for (int row = 0; row < rows; ++row)
    {
        const auto y = (float) (row * 3);

        for (int col = 0; col < columns; ++col)
        {
            const auto& cell = cells[(size_t) (row * columns + col)];
            const auto x = (float) col * (cellW + 1.0f);

            // Soft-edged wipe: fully lit until progress - 3.5%, off after + 3.5%.
            const auto centre = ((float) col + 0.5f) / (float) columns;
            const auto ramp = juce::jlimit (0.0f, 1.0f, (progress + 0.035f - centre) / 0.07f);

            // Discrete ember flicker (steps(1,end), no tweening).
            const auto t = std::fmod (clock / (double) cell.period + (double) cell.phase, 1.0);
            const auto ember = emberSteps[juce::jlimit (0, 3, (int) (t * 4.0))];

            const auto alpha = juce::jmax (0.13f, cell.baseAlpha * ember * ramp);

            g.setColour (theme::accentMid.withAlpha (alpha));
            g.fillRect (x, y, cellW, 2.0f);
        }
    }
}

} // namespace splittr
