#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

#include "Phase.h"
#include "AudioFileIO.h"
#include "ModelManager.h"
#include "StemCache.h" // StemIndex / stemOrderFor

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

    double bpm = 0.0;  // 0 = undetected
    juce::String key;  // "F# MIN" etc., empty = undetected

    struct StemInfo
    {
        int kind = stemVocals;       // StemIndex
        juce::String name;           // "Vocals", … — the name it was rendered under
        juce::File wav;
        std::vector<float> waveform; // 70 columns
        float peakDb = -120.0f;
    };

    // Display order for the mode that produced them: five stems for the
    // 4-source modes, seven for 6-stem. Empty until a run completes.
    std::vector<StemInfo> stems;

    // demucs source order (drums, bass, other, vocals[, guitar, piano]) — the
    // working view's source table reads it positionally.
    std::vector<float> sourcePeaksDb;
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

    // True when this file, in the current mode, would republish from cache
    // instantly — i.e. accepting it costs no work. The editor uses it to decide
    // whether the drop needs confirming.
    bool hasCachedResult (const juce::File&) const;

    // Persistence (processor state chunk)
    juce::String sourcePathForState() const;
    void restoreFromState (const juce::String& sourcePath);

    AudioFileIO& fileIO() { return io; }

    // Export destination memory (Settings.xml)
    juce::File lastExportDir() const { return models.lastExportDir(); }
    void setLastExportDir (const juce::File& dir) { models.setLastExportDir (dir); }

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
