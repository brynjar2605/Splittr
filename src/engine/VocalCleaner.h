#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <memory>

#include "VocalCleanTypes.h"

namespace splittr
{

// The clean-vocal sidecar runner (issue #29): one offline python process per
// request, spoken to over the four-line stdout protocol in
// tools/cleanvocal/PROTOCOL.md. Everything model-shaped lives on the python
// side; this class only launches, parses and kills.
//
// Its own thread rather than the engine's pool: a restoration run can sit
// silent for minutes inside AudioSR, and parking the one pool thread under it
// would block every separation and transcription behind an experiment. Cancel
// is a kill — the child holds no state worth a graceful goodbye, and SIGKILL
// is the only cancel that does not wait on a model to notice.
class VocalCleaner : private juce::Thread
{
public:
    VocalCleaner();
    ~VocalCleaner() override; // cancels a running job and waits it out

    // notSetUp is a state, not a failure: nothing is wrong, the runtime just
    // is not installed, and the UI phrases those two very differently.
    enum class Result { ok, cancelled, failed, notSetUp };

    // All invoked on the cleaner's own thread; onFinished exactly once per
    // started job. The message is the rendered path on ok, the sidecar's
    // one-line reason on failed, and empty otherwise.
    struct Callbacks
    {
        std::function<void (const juce::String& stage, int index, int count)> onStage;
        std::function<void (float progress01)> onProgress;
        std::function<void (Result, juce::String message)> onFinished;
    };

    // Where the runtime lives: SPLITTR_CLEANVOCAL_ROOT when set (the bench
    // override), else ~/Library/Application Support/Splittr/cleanvocal —
    // deliberately not under ModelManager::appDataDir()'s wing, because
    // setup.sh owns this tree and the plugin never writes into it.
    static juce::File root();

    // The one thing "installed" means here: <root>/clean_vocal.py exists.
    // Anything subtler (broken venv, missing checkpoint) is the sidecar's own
    // job to report through an ERROR line.
    static bool isSetUp();

    // Launch the sidecar for one stem. False — with nothing started and no
    // onFinished owed — when a job is already running or the params ask for
    // nothing; the caller guards with isNoop() but a second guard is cheap.
    bool start (const juce::File& input, const juce::File& output,
                const VocalCleanParams&, Callbacks);

    bool isBusy() const { return isThreadRunning(); }

    // Kill the child and wait for onFinished to have run. Safe from any
    // thread but the cleaner's own; a no-op when nothing is running.
    //
    // Deliberately a join, not fire-and-forget: the engine's entry-switch
    // paths call this before letting a SeparationJob loose on the entry, and
    // the join is what keeps that job's whole-meta write from ever
    // overlapping the cleaner's own. It stays short — the kill drops the
    // pipe at once, and run()'s reap of the child is bounded.
    void cancel();

    // True once cancel() has asked the running job to stop. The engine's
    // onFinished consults this before committing an ok result to disk: the
    // worker's own last check races the finish, so a cancel that lands as
    // the child exits still wins where it matters.
    bool cancelRequested() const { return threadShouldExit(); }

    // Block until the current job reports in — the CLI's whole main loop.
    void waitForJob() { waitForThreadToExit (-1); }

private:
    void run() override;

    juce::File inputFile, outputFile;
    VocalCleanParams params;
    Callbacks callbacks;

    // Created and destroyed on the cleaner thread, killed from cancel(): the
    // lock keeps the kill from racing a child that is mid-birth or already
    // reaped.
    std::unique_ptr<juce::ChildProcess> child;
    juce::CriticalSection childLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalCleaner)
};

} // namespace splittr
