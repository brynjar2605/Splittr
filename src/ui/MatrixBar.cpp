#include "MatrixBar.h"
#include "Ember.h"
#include "Theme.h"

namespace splittr
{

MatrixBar::MatrixBar (int numColumns) : columns (numColumns)
{
    setInterceptsMouseClicks (false, false);

    cells.resize ((size_t) (columns * rows));

    for (int row = 0; row < rows; ++row)
        for (int col = 0; col < columns; ++col)
            cells[(size_t) (row * columns + col)] = ui::emberFor (col, row);
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

    for (int row = 0; row < rows; ++row)
    {
        const auto y = (float) (row * 3);

        for (int col = 0; col < columns; ++col)
        {
            const auto& cell = cells[(size_t) (row * columns + col)];
            const auto x = (float) col * (cellW + 1.0f);

            // Soft-edged wipe: fully lit until progress - 3.5%, off after + 3.5%.
            const auto centre = ((float) col + 0.5f) / (float) columns;
            const auto ramp = ui::emberRamp (centre, progress);

            g.setColour (theme::accentMid.withAlpha (ui::emberAlpha (cell, clock, ramp)));
            g.fillRect (x, y, cellW, 2.0f);
        }
    }
}

} // namespace splittr
