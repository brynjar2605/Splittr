#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <vector>

#include "Phase.h"
#include "AudioFileIO.h"
#include "ModelManager.h"

namespace splittr
{

// Everything the UI needs, copied out whole under a lock at 30 Hz.
struct EngineSnapshot
{
    Phase phase = Phase::Idle;
    float progress = 0.0f; // composite 0..1 for the current activity
    juce::String message;  // error reason (short, upper-case-able)
    Quality quality = Quality::fast;

    juce::File sourceFile;
    double sourceDuration = 0.0;       // seconds
    std::vector<float> sourceWaveform; // 60 columns, 0..1

    juce::File vocalsWav, instrumentalWav;
    std::vector<float> vocalsWaveform, instrumentalWaveform;         // 70 columns
    std::array<float, 4> sourcePeaksDb { -120.f, -120.f, -120.f, -120.f }; // drums,bass,other,vocals
    bool peaksKnown = false;

    int segment = 0, segmentCount = 0;
    double etaSeconds = -1.0; // <0: unknown / hidden
    double inferenceSeconds = 0.0;
    double realtimeFactor = 0.0;

    juce::int64 downloadDone = 0, downloadTotal = 0;
    double downloadMBps = 0.0;
};

// Owned by the AudioProcessor so jobs survive editor open/close.
// All work runs on a single low-priority pool thread; the audio thread is
// never touched.
class SeparationEngine
{
public:
    SeparationEngine();
    ~SeparationEngine();

    void editorOpened();                          // auto-start first-run model download
    void requestSeparation (const juce::File&);   // cancels any running job first
    void requestCancel();
    void requestNewFile();                        // done -> idle
    void startModelDownload();
    void locateModel (const juce::File&);         // manual "LOCATE FILE MANUALLY…"
    void setQuality (Quality);                    // FAST / BEST toggle

    EngineSnapshot getSnapshot() const;

    // Persistence (processor state chunk)
    juce::String sourcePathForState() const;
    void restoreFromState (const juce::String& sourcePath);

    AudioFileIO& fileIO() { return io; }

    static constexpr double maxInputSeconds = 600.0; // v1 memory guard

private:
    class DownloadJob;
    class SeparationJob;
    friend class DownloadJob;
    friend class SeparationJob;

    bool modelReady (Quality q) const { return models.missingFor (q).empty(); }
    bool modelReady() const { return modelReady (models.quality()); }

    // Publish a change; ignored when `generation` no longer matches (a newer
    // request superseded this job).
    void mutate (int generation, const std::function<void (EngineSnapshot&)>&);
    int currentGeneration() const { return generation.load(); }

    bool tryPublishCached (const juce::File& source, int gen, Quality quality);

    AudioFileIO io;
    ModelManager models;

    std::atomic<int> generation { 0 };
    std::atomic<bool> autoDownloadTried { false };

    juce::ThreadPool pool;
    mutable juce::CriticalSection lock;
    EngineSnapshot snapshot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeparationEngine)
};

} // namespace splittr
