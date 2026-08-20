#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include "engine/ModelManager.h" // Quality
#include "Theme.h"

namespace splittr
{

// Idle / drop-target state (borrowed from concept 2c per the handoff):
// dashed purple panel, "drop an audio file into this window", plus the
// ENGINE mode switch (FAST / BEST / 6-STEM) pinned to the panel bottom.
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

    // Segmented switch of any width: draws [a|b|…] at x and returns one bounds
    // per cell. Cell n answers to hover id hoverIdBase + n.
    std::vector<juce::Rectangle<int>>
    drawSwitch (juce::Graphics&, int x, int y, const juce::StringArray& labels,
                int activeIndex, int hoverIdBase, int cellWidth) const;

    // Index of the cell containing p, or -1.
    static int cellAt (const std::vector<juce::Rectangle<int>>&, juce::Point<int> p);

    void sendHue (const juce::MouseEvent&, bool commit);

    std::vector<juce::Rectangle<int>> engineBounds; // FAST | BEST | 6-STEM, computed in paint
    std::vector<juce::Rectangle<int>> themeBounds;  // DARK | LIGHT
    juce::Rectangle<int> hueStripBounds;            //
    bool draggingHue = false;

    // 0 none; 1..3 engine cells; 4..5 theme cells.
    int hoveredOption = 0;

    static constexpr int engineHoverBase = 1;
    static constexpr int themeHoverBase = 4;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IdleDropView)
};

} // namespace splittr
