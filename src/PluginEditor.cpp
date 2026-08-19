#include "PluginEditor.h"
#include "ui/Theme.h"

SplittrAudioProcessorEditor::SplittrAudioProcessorEditor (SplittrAudioProcessor& p)
    : AudioProcessorEditor (&p), splittrProcessor (p)
{
    addAndMakeVisible (headerBar);

    setSize (splittr::theme::editorWidth, splittr::theme::editorHeight);
    startTimerHz (30);
}

SplittrAudioProcessorEditor::~SplittrAudioProcessorEditor() = default;

void SplittrAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (splittr::theme::bg);
}

void SplittrAudioProcessorEditor::resized()
{
    headerBar.setBounds (getLocalBounds().removeFromTop (splittr::theme::headerHeight));
}

void SplittrAudioProcessorEditor::timerCallback()
{
    animationClock += 1.0 / 30.0;

    headerBar.update (animationClock, splittr::Phase::Idle, {});
}
