#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

#include "Ember.h"

namespace splittr
{

// Dot-matrix progress bar: 7 rows of 2px cells with 1px gaps (20px tall).
// Fill sweeps with a ~7%-wide dithered leading edge; every lit cell flickers
// through discrete alpha steps ("ember"), phase/period hashed per cell.
class MatrixBar : public juce::Component
{
public:
    explicit MatrixBar (int numColumns);

    void update (double clockSeconds, float progress01);

    void paint (juce::Graphics&) override;

    static constexpr int rows = 7;
    static constexpr int barHeight = rows * 2 + (rows - 1); // 20px

private:
    const int columns;
    std::vector<ui::Ember> cells;
    double clock = 0.0;
    float progress = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MatrixBar)
};

} // namespace splittr
