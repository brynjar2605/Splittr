#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "MatrixBar.h"
#include "engine/SeparationEngine.h"

namespace splittr
{

// Working state: ruled source waveform with sweeping HEAD playhead on the
// left, fixed 196px four-source table with a small matrix bar on the right.
class WorkingView : public juce::Component
{
public:
    WorkingView();

    void update (double clockSeconds, const EngineSnapshot&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    MatrixBar bar { 56 };
    EngineSnapshot snap;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WorkingView)
};

} // namespace splittr
