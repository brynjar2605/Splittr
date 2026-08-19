#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "engine/ModelManager.h" // Quality

namespace splittr
{

// Idle / drop-target state (borrowed from concept 2c per the handoff):
// dashed purple panel, "drop an audio file into this window", plus the
// ENGINE quality-vs-speed toggle (FAST / BEST) pinned to the panel bottom.
// Also renders the short error reason when the engine is in the Error phase.
class IdleDropView : public juce::Component
{
public:
    IdleDropView();

    std::function<void()> onBrowse;
    std::function<void (Quality)> onQualityChange;

    void setDragOver (bool);
    void setErrorMessage (const juce::String&);
    void setQuality (Quality);

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    bool dragOver = false;
    juce::String errorMessage;
    Quality quality = Quality::fast;

    juce::Rectangle<int> fastBounds, bestBounds; // computed in paint
    int hoveredOption = 0;                       // 0 none, 1 fast, 2 best

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IdleDropView)
};

} // namespace splittr
