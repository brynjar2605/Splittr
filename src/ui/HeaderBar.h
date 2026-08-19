#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "engine/Phase.h"

namespace splittr
{

// 39px header: state mark (square, or animated sparkle while working),
// plugin name, right-aligned status meta.
class HeaderBar : public juce::Component
{
public:
    HeaderBar();

    // Called from the editor's 30 Hz timer. seconds is a monotonic clock
    // shared by all animations.
    void update (double seconds, Phase phase, const juce::String& meta);

    void paint (juce::Graphics& g) override;

private:
    Phase currentPhase = Phase::Idle;
    juce::String metaText;
    double clockSeconds = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBar)
};

} // namespace splittr
