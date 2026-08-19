#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include "ui/HeaderBar.h"
#include "ui/IdleDropView.h"
#include "ui/DownloadView.h"
#include "ui/WorkingView.h"
#include "ui/DoneView.h"

class SplittrAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    public juce::DragAndDropContainer,
                                    public juce::FileDragAndDropTarget,
                                    private juce::Timer
{
public:
    explicit SplittrAudioProcessorEditor (SplittrAudioProcessor&);
    ~SplittrAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;

private:
    void timerCallback() override;
    void browseForAudioFile();
    void browseForModelFile();
    static bool isSupportedAudioFile (const juce::String& path);

    SplittrAudioProcessor& splittrProcessor;

    // All UI lives on this fixed 560x380 surface; window resizing scales it
    // uniformly via an AffineTransform (aspect ratio locked).
    juce::Component content;

    splittr::HeaderBar headerBar;
    splittr::IdleDropView idleView;
    splittr::DownloadView downloadView;
    splittr::WorkingView workingView;
    splittr::DoneView doneView;

    std::unique_ptr<juce::FileChooser> chooser;
    double animationClock = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplittrAudioProcessorEditor)
};
