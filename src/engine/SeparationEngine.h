#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <deque>
#include <vector>

#include "Phase.h"
#include "AudioFileIO.h"
#include "ComputeBackend.h"
#include "ModelManager.h"
#include "StemCache.h" // StemIndex / stemOrderFor
#include "StemFx.h"
#include "VocalCleaner.h"

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

    // How the bpm figure came to be (issue #28: the diagnostics report should
    // say "declined on the mix, the drums answered" rather than leave a bare
    // number to be argued with).
    double bpmConfidence = 0.0;
    bool bpmFromDrums = false;

    // A stem's transcription: idle until asked for, done once the .mid is on
    // disk beside the WAV. Detected on reopen, so a cached entry comes back
    // with its MIDI already there (issue #19).
    enum class MidiState { none, running, ready };

    struct StemInfo
    {
        int kind = stemVocals;       // StemIndex
        juce::String name;           // "Vocals", … — the name it was rendered under
        juce::File wav;
        std::vector<float> waveform; // 70 columns
        float peakDb = -120.0f;

        // Empty File until a transcription exists. `midiState` is what the row
        // draws; `midiProgress` is only meaningful while running.
        juce::File midi;
        MidiState midiState = MidiState::none;
        float midiProgress = 0.0f;

        // Sound shaping for this stem (issue #9): flat until the user tunes it,
        // and read back from the entry's meta.xml on reopen. What the audition
        // hears and what the bounce writes both come from here.
        StemFxParams fx;
    };

    // Display order for the mode that produced them: five stems for the
    // 4-source modes, seven for 6-stem. Empty until a run completes.
    std::vector<StemInfo> stems;

    // The vocal stem's offline clean chain (issue #29). One block on the
    // snapshot rather than a per-stem field: the feature is vocals-only by
    // design, and the sidecar can run exactly one job at a time anyway.
    //
    // `done` means ACTIVE — the render in `file` has taken the raw vocals'
    // place in audition, drag and export until reverted. notSetUp is the
    // sidecar runtime missing, which the panel phrases as an install hint
    // rather than an error.
    struct VocalClean
    {
        enum class State { none, notSetUp, running, done, failed };

        State state = State::none;
        float progress = 0.0f;   // running only
        juce::String stage;      // protocol stage name while running
        VocalCleanParams params; // running: in flight; done: what `file` was rendered with
        juce::File file;         // done: the active render in <entry>/clean/
        juce::String error;      // failed only
    };

    VocalClean vocalClean;

    // demucs source order (drums, bass, other, vocals[, guitar, piano]) — the
    // working view's source table reads it positionally.
    std::vector<float> sourcePeaksDb;
    bool peaksKnown = false;

    int segment = 0, segmentCount = 0;
    double etaSeconds = -1.0; // <0: unknown / hidden
    double inferenceSeconds = 0.0;
    double realtimeFactor = 0.0;

    // What actually ran the separation (issue #27): a gpu request can fall
    // back to the CPU (no Metal, 6-STEM, a mid-run failure), and the done
    // view should say so rather than leave the user guessing at the speed.
    bool usedGpu = false;
    bool gpuRequested = false; // false on reopened entries: nothing to flag

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

    // Reopen a cache directory chosen in the library: publishes Ready straight
    // from its meta.xml + stem WAVs, without consulting (or needing) the
    // original source file. False when the directory is not a usable entry.
    bool openCachedEntry (const juce::File& cacheDir);

    // Transcribe one stem of the current result to MIDI (issue #19). Queued on
    // the same pool thread as everything else, so it cannot collide with a
    // separation; a no-op for a stem that is already running, already done, or
    // is not one Basic Pitch handles.
    // `force` re-runs a stem whose .mid already exists (the modifier-click on a
    // finished chip); without it a finished stem is left alone.
    void requestMidi (int stemIndex, bool force = false);

    // Per-stem sound shaping (issue #9). Only meaningful in Ready; always
    // updates the snapshot, which is what the UI drags a slider through at
    // frame rate. `commit` — the mouse-up — additionally writes the params into
    // the entry's meta.xml, so they survive a reopen, and drops the shaped
    // bounce when they land back on flat.
    void setStemFx (int stemIndex, const StemFxParams&, bool commit);

    // The file a stem should leave the plugin as — dragged out or exported.
    // Flat params: the raw render, byte for byte. Otherwise the shaped bounce
    // in <entry>/tuned/, rendered here and now when it is missing or stale, so
    // the first drag after a tweak pays a beat and the rest are instant. Falls
    // back to the raw stem if the bounce cannot be written, and returns an
    // empty File when there is no such stem to hand over.
    //
    // An active clean render (issue #29) is the vocals' SOURCE here: it is
    // what a flat stem hands over, and what a shaped one bounces from.
    juce::File processedStemFile (int stemIndex);

    // CLEAN VOCAL (issue #29): run the vocals stem through the offline
    // sidecar, or re-activate instantly when a render for these exact params
    // is already in <entry>/clean/. Only meaningful in Ready; a no-op while a
    // clean job is running, and for params that ask for nothing.
    void requestVocalClean (const VocalCleanParams&);

    // Kill the running sidecar; the previously active render (if any) comes
    // back. A no-op when nothing is running.
    void cancelVocalClean();

    // Back to the raw render everywhere, and forget the choice in meta.xml.
    // The cached file stays on disk, so re-activating the same params later
    // is instant. Also how the panel dismisses a failed / not-set-up state.
    void revertVocalClean();

    // True when the sidecar runtime is installed — forwards to VocalCleaner
    // so the UI keeps talking to the engine layer, the gpuAvailable pattern.
    static bool vocalCleanSetUp();

    void requestCancel();
    void requestNewFile();                        // done -> idle
    void startModelDownload();
    void locateModel (const juce::File&);         // manual "LOCATE FILE MANUALLY…"
    void setQuality (Quality);                    // FAST / BEST toggle

    // PROCESSING (issue #24): where inference runs, persisted in Settings.xml
    // beside the theme prefs. Static because the settings overlay reads and
    // writes it without holding an engine; a job reads it when it starts, so a
    // change lands on the next run rather than the one in flight.
    static ComputeBackend backend();
    static void setBackend (ComputeBackend);

    // True when the Metal path can run on this machine — forwards to
    // DemucsSeparator so the UI keeps talking to the engine layer. Asking for
    // `gpu` without it is not an error: inference falls back to the CPU path.
    static bool gpuAvailable();

    EngineSnapshot getSnapshot() const;

    // The rolling event log behind the DIAGNOSTICS report (issue #28):
    // timestamped one-liners at the engine's own state transitions — phases,
    // downloads, analysis verdicts, backend fallbacks, renders, errors. Ring
    // of ~200; logEvent is safe from any thread (same lock as the snapshot,
    // never held while doing work).
    void logEvent (const juce::String& line);
    juce::StringArray eventLog() const;

    // Read-only, for the diagnostics report's models section.
    const ModelManager& modelManager() const { return models; }

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
    class MidiJob;
    friend class DownloadJob;
    friend class SeparationJob;
    friend class MidiJob;

    bool modelReady (Quality q) const { return models.missingFor (q).empty(); }
    bool modelReady() const { return modelReady (models.quality()); }

    // Publish a change; ignored when `generation` no longer matches (a newer
    // request superseded this job).
    void mutate (int generation, const std::function<void (EngineSnapshot&)>&);
    int currentGeneration() const { return generation.load(); }

    bool tryPublishCached (const juce::File& source, int gen, Quality quality);

    // Publish a render as the active clean vocal and record the choice in the
    // entry's meta — the shared tail of a fresh run and a cache hit.
    void activateVocalClean (int gen, const juce::String& stemName,
                             const juce::File& stemWav, const VocalCleanParams&,
                             const juce::File& render);

    AudioFileIO io;
    ModelManager models;
    VocalCleaner cleaner;

    // meta.xml is rewritten from the message thread (fx commits) and the
    // cleaner's thread (a finished run); the writes are parse-and-write, so
    // unserialised they could drop each other's attributes.
    juce::CriticalSection metaLock;

    std::atomic<int> generation { 0 };
    std::atomic<bool> autoDownloadTried { false };

    juce::ThreadPool pool;
    mutable juce::CriticalSection lock;
    std::deque<juce::String> events; // guarded by `lock`, capped in logEvent
    EngineSnapshot snapshot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeparationEngine)
};

} // namespace splittr
