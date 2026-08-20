#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

namespace splittr
{

// Preferences overlay, opened from the settings mark in the header (so it is
// reachable from every state). Hosts THEME and the accent hue strip — both
// lifted off the idle screen, which now only carries per-song controls.
//
// Same idiom as ConfirmView: added last to the content component, 90% scrim,
// centred hairline panel, every control drawn in paint() rather than built
// from child components so they follow live theme/accent changes.
class SettingsView : public juce::Component
{
public:
    SettingsView() = default;

    std::function<void (theme::Mode)> onModeChange;
    std::function<void (float, bool)> onAccentHueChange; // (hue 0..1, commit)
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> panelBounds() const;
    void sendHue (const juce::MouseEvent&, bool commit);

    // computed in paint
    juce::Rectangle<int> darkBounds, lightBounds, hueStripBounds, closeBounds;

    bool draggingHue = false;
    int hoveredAction = 0; // 0 none, 1 dark, 2 light, 3 close

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsView)
};

} // namespace splittr
