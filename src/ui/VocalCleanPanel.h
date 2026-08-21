#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/SeparationEngine.h" // EngineSnapshot::VocalClean
#include "engine/VocalCleanTypes.h"

namespace splittr
{

// The CLEAN VOCAL panel (issue #29), opened from the vocals row's CLEAN chip.
// Same overlay idiom as DiagnosticsView: scrim, centred hairline panel,
// everything drawn in paint() against hit rects, click on the scrim closes.
//
// The panel owns only the toggles being dialled in: what is actually running
// or active lives in the engine snapshot, re-fed through update() every tick
// while visible, so progress and completion appear without the panel asking.
class VocalCleanPanel : public juce::Component
{
public:
    VocalCleanPanel() = default;

    std::function<void (const VocalCleanParams&)> onRun;
    std::function<void()> onCancel; // kill the running sidecar
    std::function<void()> onRevert; // back to the raw render / dismiss a failure
    std::function<void()> onClose;

    // Open on the current engine state. An active or running set of params
    // becomes the toggles' starting point, so reopening the panel shows what
    // is actually in effect rather than the last thing fiddled with.
    void show (const EngineSnapshot::VocalClean&, bool sidecarSetUp);

    // The editor streams the snapshot's clean block through here at 30 Hz
    // while the panel is up.
    void update (const EngineSnapshot::VocalClean&, bool sidecarSetUp);

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> panelBounds() const;

    bool runnable() const; // the RUN action would actually do something

    VocalCleanParams params; // what the toggles say; the engine hears it on RUN
    EngineSnapshot::VocalClean engineState;
    bool setUp = false;

    // Hit rects, computed in paint like the sibling overlays.
    juce::Rectangle<int> dereverbCells[2], echoCells[2], restoreCells[3];
    juce::Rectangle<int> runBounds, revertBounds, closeBounds;

    // 0 none, 1..2 dereverb off/on, 3..4 echo off/on, 5..7 restore cells,
    // 8 run/cancel, 9 revert, 10 close.
    int hoveredAction = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalCleanPanel)
};

} // namespace splittr
