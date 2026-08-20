#include "PluginEditor.h"
#include "ui/Theme.h"
#include "engine/StemCache.h"

using namespace splittr;

SplittrAudioProcessorEditor::SplittrAudioProcessorEditor (SplittrAudioProcessor& p)
    : AudioProcessorEditor (&p), splittrProcessor (p)
{
    splittr::theme::loadFromSettings();

    addAndMakeVisible (content);
    content.addAndMakeVisible (headerBar);
    content.addChildComponent (idleView);
    content.addChildComponent (downloadView);
    content.addChildComponent (workingView);
    content.addChildComponent (doneView);
    content.addChildComponent (confirmView);
    content.addChildComponent (settingsView); // added last: sits on top

    // Fixed design-space layout; the window scales this surface uniformly.
    content.setBounds (0, 0, theme::editorWidth, theme::editorHeight);
    {
        auto bounds = content.getLocalBounds();
        headerBar.setBounds (bounds.removeFromTop (theme::headerHeight));
        idleView.setBounds (bounds);
        downloadView.setBounds (bounds);
        workingView.setBounds (bounds);
        doneView.setBounds (bounds);
        confirmView.setBounds (bounds);
        settingsView.setBounds (bounds);
    }

    auto& engine = splittrProcessor.getEngine();
    auto& audition = splittrProcessor.getAudition();

    idleView.onBrowse = [this] { browseForAudioFile(); };
    idleView.onQualityChange = [&engine] (splittr::Quality q) { engine.setQuality (q); };

    // Settings is reachable from every state — the header is always up.
    headerBar.onSettings = [this] { showSettings = ! showSettings; };

    settingsView.onClose = [this] { showSettings = false; };

    settingsView.onModeChange = [this] (splittr::theme::Mode m)
    {
        splittr::theme::apply (m, splittr::theme::currentAccentHue());
        repaint();
    };

    settingsView.onAccentHueChange = [this] (float hue, bool commit)
    {
        splittr::theme::apply (splittr::theme::currentMode(), hue, commit);
        repaint();
    };

    downloadView.onCancel = [&engine] { engine.requestCancel(); };
    downloadView.onDownload = [&engine] { engine.startModelDownload(); };
    downloadView.onLocate = [this] { browseForModelFile(); };

    doneView.onReveal = [&engine]
    {
        const auto file = engine.getSnapshot().stems[splittr::stemVocals].wav;
        if (file.existsAsFile())
            file.revealToUser();
    };

    doneView.onExport = [this] { exportStems(); };

    doneView.onNewFile = [&engine, &audition]
    {
        audition.stop();
        engine.requestNewFile();
    };

    confirmView.onConfirm = [this] { startSeparation (pendingFile); };

    confirmView.onCancel = [this]
    {
        pendingFile = juce::File();
        confirmView.setVisible (false);
    };

    doneView.onPlayToggle = [this] { splittrProcessor.getAudition().togglePlay (stemFiles()); };

    for (int i = 0; i < splittr::numStems; ++i)
    {
        auto& row = doneView.row (i);

        row.onMuteToggle = [&audition, i] { audition.setMuted (i, ! audition.isMuted (i)); };

        // Seeking is how you audition a spot, so a click on a cold transport
        // starts the shared playback rather than silently moving the playhead.
        row.onSeek = [this] (double f)
        {
            auto& player = splittrProcessor.getAudition();

            if (! player.isLoaded())
                player.play (stemFiles());

            player.seekFraction (f);
        };
    }

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
    // A stem row is mid external drag, so this is one of our own files coming
    // straight back: refuse it rather than discarding the session for it.
    if (splittr::StemRowComponent::isExternalDragInProgress())
        return false;

    return files.size() == 1
           && isSupportedAudioFile (files[0])
           && ! splittr::StemCache::isInsideCache (juce::File (files[0]));
}

void SplittrAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    idleView.setDragOver (false);

    if (files.isEmpty())
        return;

    const juce::File file (files[0]);

    // Belt and braces: rendered stems live in the cache and are never a
    // meaningful thing to separate.
    if (splittr::StemCache::isInsideCache (file))
        return;

    requestSeparationFor (file);
}

void SplittrAudioProcessorEditor::requestSeparationFor (const juce::File& file)
{
    if (! file.existsAsFile())
        return;

    const auto snap = splittrProcessor.getEngine().getSnapshot();

    const bool workToLose = snap.phase == Phase::Ready || snap.phase == Phase::Decoding
                            || snap.phase == Phase::Separating || snap.phase == Phase::Rendering;

    // A cache hit republishes instantly without discarding any work, so it
    // never needs confirming.
    const bool cached = splittr::StemCache::entryFor (file, snap.quality).complete();

    if (workToLose && ! cached)
    {
        pendingFile = file;
        confirmView.setPendingFile (file);
        return; // visibility follows pendingFile in timerCallback
    }

    startSeparation (file);
}

void SplittrAudioProcessorEditor::startSeparation (juce::File file)
{
    pendingFile = juce::File();
    confirmView.setVisible (false);

    if (! file.existsAsFile())
        return;

    splittrProcessor.getAudition().stop();
    splittrProcessor.getEngine().requestSeparation (file);
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
                              requestSeparationFor (fc.getResult());
                          });
}

void SplittrAudioProcessorEditor::exportStems()
{
    auto& engine = splittrProcessor.getEngine();
    const auto snap = engine.getSnapshot();

    if (snap.phase != splittr::Phase::Ready)
        return;

    const auto startDir = engine.lastExportDir().isDirectory()
                              ? engine.lastExportDir()
                              : juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    chooser = std::make_unique<juce::FileChooser> ("Choose a folder for the stems", startDir);

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectDirectories,
                          [this, snap] (const juce::FileChooser& fc)
                          {
                              const auto dir = fc.getResult();

                              if (! dir.isDirectory())
                                  return;

                              splittrProcessor.getEngine().setLastExportDir (dir);

                              const auto base = snap.sourceFile.getFileNameWithoutExtension();

                              for (int i = 0; i < splittr::numStems; ++i)
                              {
                                  const auto& wav = snap.stems[(size_t) i].wav;

                                  if (wav.existsAsFile())
                                      wav.copyFileTo (dir.getChildFile (
                                          base + " - " + splittr::stemName (i) + ".wav"));
                              }

                              dir.revealToUser(); // confirmation: show the result in Finder
                          });
}

splittr::AuditionPlayer::StemFiles SplittrAudioProcessorEditor::stemFiles() const
{
    const auto snap = splittrProcessor.getEngine().getSnapshot();

    splittr::AuditionPlayer::StemFiles files;

    for (int i = 0; i < splittr::numStems; ++i)
        files[(size_t) i] = snap.stems[(size_t) i].wav;

    return files;
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

    static const float previewPeaks[splittr::numStems] = { -6.2f, -9.8f, -11.4f, -7.9f, -4.1f };

    for (int i = 0; i < splittr::numStems; ++i)
    {
        s.stems[(size_t) i].waveform = fakeWave (70, 2.0f + (float) i);
        s.stems[(size_t) i].peakDb = previewPeaks[i];
    }

    s.sourcePeaksDb = { -9.8f, -11.4f, -7.9f, -6.2f };
    s.bpm = 124.0;
    s.key = "F# MIN";
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
    const bool preview = applyUiPreview (snap, animationClock);

    // header meta per state
    juce::String meta;

    switch (snap.phase)
    {
        case Phase::NeedsModel:
        case Phase::Downloading: meta = "FIRST RUN"; break;
        case Phase::Decoding:
        case Phase::Separating:
        case Phase::Rendering:
            meta = snap.quality == splittr::Quality::best
                       ? juce::String::fromUTF8 ("4-SOURCE ANALYSIS \xc2\xb7 FINE-TUNED")
                       : juce::String ("4-SOURCE ANALYSIS");
            break;
        case Phase::Ready:
            meta = juce::String::fromUTF8 ("OUTPUT 5 \xc3\x97 WAV \xc2\xb7 32-BIT FLOAT \xc2\xb7 44.1 kHz");
            break;
        case Phase::Error: meta = snap.message; break;
        case Phase::Idle: break;
    }

    const bool confirming = pendingFile != juce::File();

    if (confirming)
        meta = "CONFIRM TO REPLACE";

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
    confirmView.setVisible (confirming);
    settingsView.setVisible (showSettings);

    if (showIdle)
    {
        idleView.setErrorMessage (snap.phase == Phase::Error ? snap.message : juce::String());
        idleView.setQuality (snap.quality);
    }

    if (showDownload)
        downloadView.update (animationClock, snap);

    if (showWorking)
        workingView.update (animationClock, snap);

    if (showDone)
    {
        auto playing = audition.isPlaying();
        auto pos = audition.positionSeconds();
        auto len = audition.lengthSeconds();

        if (len <= 0.0)
            len = snap.sourceDuration;

        // The preview harness has no audio, so fake a running transport (and
        // one muted stem) to keep the flame and the dimming inspectable.
        if (preview)
        {
            playing = true;
            len = snap.sourceDuration;
            pos = snap.progress * len;
        }

        doneView.update (snap, playing);

        for (int i = 0; i < splittr::numStems; ++i)
        {
            const auto& stem = snap.stems[(size_t) i];
            const auto muted = preview ? i == splittr::stemBass : audition.isMuted (i);

            doneView.row (i).update (stem.wav, stem.waveform, stem.peakDb,
                                     muted, playing, pos, len, animationClock);
        }
    }
}
