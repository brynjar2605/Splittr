#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "engine/ModelManager.h" // Quality
#include "Theme.h"

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
    std::function<void (theme::Mode)> onModeChange;
    std::function<void (float, bool)> onAccentHueChange; // (hue 0..1, commit)

    void setDragOver (bool);
    void setErrorMessage (const juce::String&);
    void setQuality (Quality);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    bool dragOver = false;
    juce::String errorMessage;
    Quality quality = Quality::fast;

    // Segmented switch: draws [a|b] at x, returns bounds of both cells.
    std::pair<juce::Rectangle<int>, juce::Rectangle<int>>
    drawSwitch (juce::Graphics&, int x, int y, const juce::String& a, const juce::String& b,
                bool bActive, int hoverIdA, int hoverIdB, int cellWidth) const;

    void sendHue (const juce::MouseEvent&, bool commit);

    juce::Rectangle<int> fastBounds, bestBounds;  // computed in paint
    juce::Rectangle<int> darkBounds, lightBounds; //
    juce::Rectangle<int> hueStripBounds;          //
    bool draggingHue = false;
    int hoveredOption = 0; // 0 none, 1 fast, 2 best, 3 dark, 4 light

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IdleDropView)
};

} // namespace splittr
