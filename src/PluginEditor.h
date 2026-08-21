#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include "ui/HeaderBar.h"
#include "ui/IdleDropView.h"
#include "ui/DownloadView.h"
#include "ui/WorkingView.h"
#include "ui/DoneView.h"
#include "ui/ConfirmView.h"
#include "ui/DiagnosticsView.h"
#include "ui/SettingsView.h"
#include "ui/LibraryView.h"
#include "ui/TuneView.h"

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

    // Space toggles the stem transport while a session is up — the same
    // gesture the footer button offers, without the mouse trip.
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;

private:
    void timerCallback() override;
    void browseForAudioFile();
    void browseForModelFile();
    void exportStems();

    // Asks first when accepting the file would discard the current stems,
    // otherwise starts straight away.
    void requestSeparationFor (const juce::File&);
    void startSeparation (juce::File); // by value: the confirm path passes pendingFile

    splittr::AuditionPlayer::StemFiles stemFiles() const; // current snapshot's rendered WAVs

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
    splittr::ConfirmView confirmView;   // overlays the above while confirming
    splittr::TuneView tuneView;         // per-stem shaping, opened from a row's mix icon
    splittr::LibraryView libraryView;   // cache browser, opened from the idle screen
    splittr::SettingsView settingsView; // prefs, opened from the header mark
    splittr::DiagnosticsView diagView;  // the copyable report, opened from settings

    std::unique_ptr<juce::FileChooser> chooser;
    double animationClock = 0.0;

    // Set while the confirmation overlay is up; empty otherwise.
    juce::File pendingFile;

    // Overlay visibility, applied in timerCallback alongside the phase views.
    bool showSettings = false;
    bool showLibrary = false;
    bool showTune = false; // only meaningful while the done view is up
    bool showDiag = false; // sits above settings, which stays open behind it

    // The stem set the mute defaults were last applied to (vocals +
    // instrumental audible, sources muted): re-applied only when the set
    // itself changes, so the user's own toggles survive within a session.
    juce::StringArray defaultedStems;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplittrAudioProcessorEditor)
};
