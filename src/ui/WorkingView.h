#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "MatrixBar.h"
#include "engine/SeparationEngine.h"

namespace splittr
{

// Working state: ruled source waveform with sweeping HEAD playhead on the
// left, fixed 196px source table with a small matrix bar on the right. The
// table lists four sources in the 4-source modes and six with htdemucs_6s,
// tightening its rows to fit.
//
// Under the waveform sits CANCEL, in the slot the first-run footer puts it in
// (issue #20) — a running job is otherwise only escapable by waiting it out.
// Drawn in paint() and hit-tested here rather than built from a child
// component, so it follows live theme changes like the other bench actions.
class WorkingView : public juce::Component
{
public:
    WorkingView();

    std::function<void()> onCancel;

    void update (double clockSeconds, const EngineSnapshot&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void visibilityChanged() override;

private:
    MatrixBar bar { 56 };
    EngineSnapshot snap;
    double clock = 0.0;

    juce::Rectangle<int> cancelBounds; // computed in paint
    bool hoveredCancel = false;

    // Held from the click until the job actually lets go — up to one model
    // segment — with the action inert and saying so. Cleared per run, so a
    // later job never comes back to a spent cancel.
    bool cancelling = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WorkingView)
};

} // namespace splittr
