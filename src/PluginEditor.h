#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include "ui/HeaderBar.h"

class SplittrAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    public juce::DragAndDropContainer,
                                    private juce::Timer
{
public:
    explicit SplittrAudioProcessorEditor (SplittrAudioProcessor&);
    ~SplittrAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    SplittrAudioProcessor& splittrProcessor;
    splittr::HeaderBar headerBar;

    double animationClock = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplittrAudioProcessorEditor)
};
