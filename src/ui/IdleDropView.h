#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include "engine/ModelManager.h" // Quality

namespace splittr
{

// Idle / drop-target state (borrowed from concept 2c per the handoff):
// dashed purple panel, "drop an audio file into this window", the ENGINE
// mode switch (FAST / BEST / 6-STEM) above it and the LIBRARY entry point
// below. THEME/ACCENT live in the settings overlay; ENGINE stays here because
// it is a per-song choice. Also renders the short error reason when the
// engine is in the Error phase.
class IdleDropView : public juce::Component
{
public:
    IdleDropView();

    std::function<void()> onBrowse;
    std::function<void()> onLibrary;
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

    std::vector<juce::Rectangle<int>> engineBounds; // FAST | BEST | 6-STEM, computed in paint
    juce::Rectangle<int> libraryBounds;             //

    // 0 none; 1..3 engine cells; 4 library.
    int hoveredOption = 0;

    static constexpr int engineHoverBase = 1;
    static constexpr int libraryHoverId = 4;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IdleDropView)
};

} // namespace splittr
