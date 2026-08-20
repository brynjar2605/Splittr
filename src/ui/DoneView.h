#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "StemRowComponent.h"
#include "engine/SeparationEngine.h"

namespace splittr
{

// Done state: one row per rendered stem — five in the 4-source modes (vocals,
// drums, bass, other, instrumental), seven in 6-stem (guitar and piano join) —
// plus a footer with the shared PLAY/PAUSE control, stats, REVEAL, EXPORT…,
// NEW FILE and the drag hint.
//
// The rows are rebuilt whenever the snapshot's stem set changes, and their
// height is computed from how many there are: 53px for five, ~40px for seven.
class DoneView : public juce::Component
{
public:
    DoneView();

    int numRows() const { return rows.size(); }
    StemRowComponent& row (int index) { return *rows[index]; }

    std::function<void()> onPlayToggle;
    std::function<void()> onReveal;
    std::function<void()> onExport;
    std::function<void()> onNewFile;

    // Stem index within the current row set.
    std::function<void (int)> onMuteToggle;
    std::function<void (int, double)> onSeek; // (stem, fraction 0..1)

    void update (const EngineSnapshot&, bool isPlaying);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    // No-op when the names already match, so the rows survive the 30 Hz
    // snapshot poll and keep their hover/drag state.
    void rebuildRows (const std::vector<EngineSnapshot::StemInfo>&);

    juce::OwnedArray<StemRowComponent> rows;
    juce::StringArray rowNames;

    int rowHeight = StemRowComponent::maxRowHeight; // recomputed per row count
    int rowGap = 6;

    juce::Rectangle<int> playBounds, revealBounds, exportBounds, newFileBounds;
    juce::String statsText;
    bool playing = false;
    int hoveredAction = 0; // 0 none, 1 reveal, 2 export, 3 new file, 4 play/pause

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DoneView)
};

} // namespace splittr
