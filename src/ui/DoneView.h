#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "StemRowComponent.h"
#include "engine/SeparationEngine.h"

namespace splittr
{

// Done state: five stem rows (vocals, drums, bass, other, instrumental) plus
// a footer with the shared PLAY/PAUSE control, stats, REVEAL, EXPORT…,
// NEW FILE and the drag hint.
class DoneView : public juce::Component
{
public:
    DoneView();

    StemRowComponent& row (int stemIndex) { return *rows[stemIndex]; }

    std::function<void()> onPlayToggle;
    std::function<void()> onReveal;
    std::function<void()> onExport;
    std::function<void()> onNewFile;

    void update (const EngineSnapshot&, bool isPlaying);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::OwnedArray<StemRowComponent> rows;

    juce::Rectangle<int> playBounds, revealBounds, exportBounds, newFileBounds;
    juce::String statsText;
    bool playing = false;
    int hoveredAction = 0; // 0 none, 1 reveal, 2 export, 3 new file, 4 play/pause

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DoneView)
};

} // namespace splittr
