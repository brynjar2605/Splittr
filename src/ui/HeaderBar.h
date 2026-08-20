#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "engine/Phase.h"

namespace splittr
{

// 39px header: plugin wordmark, right-aligned status meta, and a settings
// mark at the far right. The bar is click-through except for that mark
// (see hitTest), so drops and clicks still reach the views underneath.
class HeaderBar : public juce::Component
{
public:
    HeaderBar();

    std::function<void()> onSettings;

    // Called from the editor's 30 Hz timer. seconds is a monotonic clock
    // shared by all animations (currently unused here — the header is static).
    void update (double seconds, Phase phase, const juce::String& meta);

    void paint (juce::Graphics& g) override;
    bool hitTest (int x, int y) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> markBounds() const;    // the three hairlines
    juce::Rectangle<int> settingsBounds() const; // padded-out hit rect

    Phase currentPhase = Phase::Idle;
    juce::String metaText;
    bool settingsHovered = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBar)
};

} // namespace splittr
