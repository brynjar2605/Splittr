#include "PluginEditor.h"
#include "ui/Theme.h"

using namespace splittr;

SplittrAudioProcessorEditor::SplittrAudioProcessorEditor (SplittrAudioProcessor& p)
    : AudioProcessorEditor (&p), splittrProcessor (p)
{
    addAndMakeVisible (content);
    content.addAndMakeVisible (headerBar);
    content.addChildComponent (idleView);
    content.addChildComponent (downloadView);
    content.addChildComponent (workingView);
    content.addChildComponent (doneView);

    // Fixed design-space layout; the window scales this surface uniformly.
    content.setBounds (0, 0, theme::editorWidth, theme::editorHeight);
    {
        auto bounds = content.getLocalBounds();
        headerBar.setBounds (bounds.removeFromTop (theme::headerHeight));
        idleView.setBounds (bounds);
        downloadView.setBounds (bounds);
        workingView.setBounds (bounds);
        doneView.setBounds (bounds);
    }

    auto& engine = splittrProcessor.getEngine();
    auto& audition = splittrProcessor.getAudition();

    idleView.onBrowse = [this] { browseForAudioFile(); };

    downloadView.onCancel = [&engine] { engine.requestCancel(); };
    downloadView.onDownload = [&engine] { engine.startModelDownload(); };
    downloadView.onLocate = [this] { browseForModelFile(); };

    doneView.onReveal = [&engine]
    {
        const auto file = engine.getSnapshot().vocalsWav;
        if (file.existsAsFile())
            file.revealToUser();
    };

    doneView.onNewFile = [&engine, &audition]
    {
        audition.stop();
        engine.requestNewFile();
    };

    doneView.vocalsRow.onPlayToggle = [&engine, &audition]
    { audition.toggle (engine.getSnapshot().vocalsWav, 0); };

    doneView.instrumentalRow.onPlayToggle = [&engine, &audition]
    { audition.toggle (engine.getSnapshot().instrumentalWav, 1); };

    doneView.vocalsRow.onSeek = [&engine, &audition] (double f)
    {
        if (audition.currentStemId() != 0)
            audition.toggle (engine.getSnapshot().vocalsWav, 0);
        audition.seekFraction (f);
    };

    doneView.instrumentalRow.onSeek = [&engine, &audition] (double f)
    {
        if (audition.currentStemId() != 1)
            audition.toggle (engine.getSnapshot().instrumentalWav, 1);
        audition.seekFraction (f);
    };

    setResizable (true, true); // native window edges + bottom-right corner grip
    getConstrainer()->setFixedAspectRatio ((double) theme::editorWidth / (double) theme::editorHeight);
    setResizeLimits (theme::editorWidth, theme::editorHeight,
                     theme::editorWidth * 4, theme::editorHeight * 4);

    const auto scale = juce::jlimit (1.0f, 4.0f, splittrProcessor.getEditorScale());
    setSize (juce::roundToInt (theme::editorWidth * scale),
             juce::roundToInt (theme::editorHeight * scale));

    startTimerHz (30);

    engine.editorOpened();
}

SplittrAudioProcessorEditor::~SplittrAudioProcessorEditor() = default;

void SplittrAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
}

void SplittrAudioProcessorEditor::resized()
{
    const auto scale = (float) getWidth() / (float) theme::editorWidth;

    content.setTransform (juce::AffineTransform::scale (scale));
    splittrProcessor.setEditorScale (scale);
}

bool SplittrAudioProcessorEditor::isSupportedAudioFile (const juce::String& path)
{
    static const juce::StringArray extensions { ".wav", ".aiff", ".aif", ".mp3", ".flac", ".m4a", ".ogg" };

    for (const auto& ext : extensions)
        if (path.endsWithIgnoreCase (ext))
            return true;

    return false;
}

bool SplittrAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && isSupportedAudioFile (files[0]);
}

void SplittrAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    idleView.setDragOver (false);

    if (files.isEmpty())
        return;

    splittrProcessor.getAudition().stop();
    splittrProcessor.getEngine().requestSeparation (juce::File (files[0]));
}

void SplittrAudioProcessorEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    idleView.setDragOver (true);
}

void SplittrAudioProcessorEditor::fileDragExit (const juce::StringArray&)
{
    idleView.setDragOver (false);
}

void SplittrAudioProcessorEditor::browseForAudioFile()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Choose an audio file to split",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        "*.wav;*.aiff;*.aif;*.mp3;*.flac;*.m4a;*.ogg");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult().existsAsFile())
                              {
                                  splittrProcessor.getAudition().stop();
                                  splittrProcessor.getEngine().requestSeparation (fc.getResult());
                              }
                          });
}

void SplittrAudioProcessorEditor::browseForModelFile()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Locate ggml htdemucs model", juce::File(), "*.bin");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult().existsAsFile())
                                  splittrProcessor.getEngine().locateModel (fc.getResult());
                          });
}

// Dev harness: SPLITTR_UI_PREVIEW=firstrun|working|done renders that state
// with synthetic data for side-by-side comparison against the design mock.
static bool applyUiPreview (splittr::EngineSnapshot& s, double clock)
{
    static const auto mode = juce::SystemStats::getEnvironmentVariable ("SPLITTR_UI_PREVIEW", {});

    if (mode.isEmpty())
        return false;

    const auto fakeWave = [] (int n, float seed)
    {
        std::vector<float> w ((size_t) n);
        for (int i = 0; i < n; ++i)
            w[(size_t) i] = 0.15f + 0.8f * std::abs (std::sin (seed + (float) i * 0.37f)
                                                     * std::cos ((float) i * 0.11f));
        return w;
    };

    const auto p = (float) std::fmod (clock * 0.05, 1.0);

    s.sourceFile = juce::File ("~/Music/MIDNIGHT_TAKE3.WAV");
    s.sourceDuration = 228.0;
    s.sourceWaveform = fakeWave (60, 1.0f);
    s.vocalsWaveform = fakeWave (70, 2.0f);
    s.instrumentalWaveform = fakeWave (70, 3.0f);
    s.sourcePeaksDb = { -9.8f, -11.4f, -7.9f, -6.2f };
    s.progress = p;

    if (mode == "firstrun")
    {
        s.phase = splittr::Phase::Downloading;
        s.downloadDone = (juce::int64) (p * 84e6);
        s.downloadTotal = 84000000;
        s.downloadMBps = 4.1;
    }
    else if (mode == "working")
    {
        s.phase = splittr::Phase::Separating;
        s.peaksKnown = true;
        s.segment = 1 + (int) (p * 6.9f);
        s.segmentCount = 7;
        s.etaSeconds = 62.0 * (1.0 - (double) p);
    }
    else if (mode == "done")
    {
        s.phase = splittr::Phase::Ready;
        s.peaksKnown = true;
        s.inferenceSeconds = 96.4;
        s.realtimeFactor = 0.87;
    }

    return true;
}

void SplittrAudioProcessorEditor::timerCallback()
{
    animationClock += 1.0 / 30.0;

    auto& engine = splittrProcessor.getEngine();
    auto& audition = splittrProcessor.getAudition();

    audition.stopIfFinished();

    auto snap = engine.getSnapshot();
    applyUiPreview (snap, animationClock);

    // header meta per state
    juce::String meta;

    switch (snap.phase)
    {
        case Phase::NeedsModel:
        case Phase::Downloading: meta = "FIRST RUN"; break;
        case Phase::Decoding:
        case Phase::Separating:
        case Phase::Rendering: meta = "4-SOURCE ANALYSIS"; break;
        case Phase::Ready:
            meta = juce::String::fromUTF8 ("OUTPUT 2 \xc3\x97 WAV \xc2\xb7 32-BIT FLOAT \xc2\xb7 44.1 kHz");
            break;
        case Phase::Error: meta = snap.message; break;
        case Phase::Idle: break;
    }

    headerBar.update (animationClock, snap.phase, meta);

    const bool showDownload = snap.phase == Phase::NeedsModel || snap.phase == Phase::Downloading;
    const bool showWorking = snap.phase == Phase::Decoding || snap.phase == Phase::Separating
                             || snap.phase == Phase::Rendering;
    const bool showDone = snap.phase == Phase::Ready;
    const bool showIdle = ! showDownload && ! showWorking && ! showDone;

    idleView.setVisible (showIdle);
    downloadView.setVisible (showDownload);
    workingView.setVisible (showWorking);
    doneView.setVisible (showDone);

    if (showIdle)
        idleView.setErrorMessage (snap.phase == Phase::Error ? snap.message : juce::String());

    if (showDownload)
        downloadView.update (animationClock, snap);

    if (showWorking)
        workingView.update (animationClock, snap);

    if (showDone)
    {
        doneView.update (snap);

        const auto playingStem = audition.currentStemId();
        const auto pos = audition.positionSeconds();
        const auto len = audition.lengthSeconds();

        doneView.vocalsRow.update (snap.vocalsWav, snap.vocalsWaveform,
                                   playingStem == 0, pos,
                                   playingStem == 0 ? len : snap.sourceDuration);
        doneView.instrumentalRow.update (snap.instrumentalWav, snap.instrumentalWaveform,
                                         playingStem == 1, pos,
                                         playingStem == 1 ? len : snap.sourceDuration);
    }
}
