#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include "engine/ModelManager.h" // Quality

namespace splittr
{

// Idle / drop-target state (borrowed from concept 2c per the handoff):
// dashed purple panel, "drop an audio file into this window", the ENGINE
// mode switch (FAST / BEST / 6-STEM) above it and the LIBRARY entry point
// centred below it. THEME/ACCENT live in the settings overlay; ENGINE stays
// here because it is a per-song choice. Also renders the short error reason
// when the engine is in the Error phase.
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

    // Re-count what the stem cache holds, for the number on the LIBRARY tag.
    // A directory scan, so it is done on entering the idle state rather than
    // in paint(); the view calls it itself (see visibilityChanged), and the
    // editor calls it when the library overlay closes — deletes in there can
    // stale the count while this view sits visible underneath.
    void refreshLibraryCount();

    void paint (juce::Graphics&) override;
    void visibilityChanged() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    bool dragOver = false;
    juce::String errorMessage;
    Quality quality = Quality::fast;

    int libraryCount = 0; // cached entry count behind the LIBRARY tag

    std::vector<juce::Rectangle<int>> engineBounds; // FAST | BEST | 6-STEM, computed in paint
    juce::Rectangle<int> libraryBounds;             //

    // 0 none; 1..3 engine cells; 4 library.
    int hoveredOption = 0;

    static constexpr int engineHoverBase = 1;
    static constexpr int libraryHoverId = 4;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IdleDropView)
};

} // namespace splittr
