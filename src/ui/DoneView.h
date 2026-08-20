#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "StemRowComponent.h"
#include "engine/SeparationEngine.h"

namespace splittr
{

// Done state: one row per rendered stem — five in the 4-source modes (vocals,
// drums, bass, other, instrumental), seven in 6-stem (guitar and piano join) —
// plus a footer with the DRAG ALL grip, the shared PLAY/PAUSE control, stats,
// REVEAL, EXPORT…, NEW FILE and the drag hint.
//
// The rows are rebuilt whenever the snapshot's stem set changes, and their
// height is computed from how many there are: 53px for five, ~40px for seven.
//
// The footer's own drag gesture is DRAG ALL: one external drag carrying every
// rendered WAV, which a DAW turns into one track per file (issue #10). It goes
// through this view's mouse handlers rather than a child component because a
// drag source needs mouseDrag, and it shares the rows' self-drop guard.
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
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
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

    juce::Rectangle<int> playBounds, revealBounds, exportBounds, newFileBounds, dragAllBounds;
    juce::String statsText;
    bool playing = false;
    int hoveredAction = 0; // 0 none, 1 reveal, 2 export, 3 new file, 4 play/pause, 5 drag all

    // Every rendered WAV in row order — the DRAG ALL payload, refreshed from
    // each snapshot so reopening a library entry re-points it.
    juce::StringArray stemPaths;
    bool draggingAll = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DoneView)
};

} // namespace splittr
