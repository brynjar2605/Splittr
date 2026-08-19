#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace splittr
{

// Idle / drop-target state (borrowed from concept 2c per the handoff):
// dashed purple panel, "drop an audio file into this window".
// Also renders the short error reason when the engine is in the Error phase.
class IdleDropView : public juce::Component
{
public:
    IdleDropView();

    std::function<void()> onBrowse;

    void setDragOver (bool);
    void setErrorMessage (const juce::String&);

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    bool dragOver = false;
    juce::String errorMessage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IdleDropView)
};

} // namespace splittr
