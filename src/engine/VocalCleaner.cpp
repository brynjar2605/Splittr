#include "VocalCleaner.h"

namespace splittr
{

VocalCleaner::VocalCleaner() : juce::Thread ("Splittr VocalClean") {}

VocalCleaner::~VocalCleaner()
{
    cancel();
}

juce::File VocalCleaner::root()
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("SPLITTR_CLEANVOCAL_ROOT", {});

    if (env.isNotEmpty())
        return juce::File (env);

    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support")
        .getChildFile ("Splittr")
        .getChildFile ("cleanvocal");
}

bool VocalCleaner::isSetUp()
{
    return root().getChildFile ("clean_vocal.py").existsAsFile();
}

bool VocalCleaner::start (const juce::File& input, const juce::File& output,
                          const VocalCleanParams& p, Callbacks cb)
{
    if (isThreadRunning() || p.isNoop())
        return false;

    inputFile = input;
    outputFile = output;
    params = p;
    callbacks = std::move (cb);

    startThread();
    return true;
}

void VocalCleaner::cancel()
{
    {
        const juce::ScopedLock sl (childLock);

        // Kill first: the dying child closes the pipe, so the read loop falls
        // out at once and the join below is bounded by the reap rather than
        // by however long a model was still going to run. The signal shares
        // the lock hold so run()'s launch guard can never slip a fresh child
        // in between the two.
        if (child != nullptr)
            child->kill();

        signalThreadShouldExit();
    }

    // The join is the contract (see the header): callers may rewrite the
    // entry's meta wholesale the moment this returns, so onFinished must be
    // completely done — not merely signalled — by then.
    waitForThreadToExit (-1);
}

void VocalCleaner::run()
{
    const auto finish = [this] (Result result, juce::String message)
    {
        if (callbacks.onFinished != nullptr)
            callbacks.onFinished (result, std::move (message));
    };

    // Checked at launch, not at start(): the user can run setup.sh with the
    // plugin open, and the next request should simply work.
    if (! isSetUp())
    {
        finish (Result::notSetUp, {});
        return;
    }

    const auto rootDir = root();

    juce::StringArray args;
    args.add (rootDir.getChildFile ("venv-sep").getChildFile ("bin").getChildFile ("python")
                  .getFullPathName());
    args.add (rootDir.getChildFile ("clean_vocal.py").getFullPathName());
    args.add ("--input");
    args.add (inputFile.getFullPathName());
    args.add ("--output");
    args.add (outputFile.getFullPathName());

    if (params.dereverb)
        args.add ("--dereverb");

    if (params.dereverbEcho)
        args.add ("--dereverb-echo");

    args.add ("--restore");
    args.add (params.restore == VocalRestoreMode::apollo    ? "apollo"
              : params.restore == VocalRestoreMode::audiosr ? "audiosr"
                                                            : "none");

    // Explicit even when it is the default, per the contract: the sidecar
    // must never have to guess where its own models live.
    args.add ("--root");
    args.add (rootDir.getFullPathName());

    outputFile.getParentDirectory().createDirectory();

    {
        const juce::ScopedLock sl (childLock);

        // A cancel that landed before the launch: obey it rather than racing
        // it with a fresh process the kill already missed.
        if (threadShouldExit())
        {
            finish (Result::cancelled, {});
            return;
        }

        child = std::make_unique<juce::ChildProcess>();

        // stderr rides along so a wordless death still leaves a clue; the
        // prefix match below keeps it out of the protocol's way.
        if (! child->start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            child.reset();
            finish (Result::failed, "could not launch the sidecar python");
            return;
        }
    }

    juce::String donePath, errorMessage, lastNoise;

    // Only the four protocol prefixes mean anything; models print freely and
    // all of it lands here too. The last non-protocol line is kept as the
    // fallback reason for a child that dies without an ERROR line.
    const auto parseLine = [&] (const juce::String& raw)
    {
        const auto line = raw.trimEnd();

        if (line.startsWith ("STAGE "))
        {
            const auto t = juce::StringArray::fromTokens (line, " ", {});

            if (t.size() >= 4 && callbacks.onStage != nullptr)
                callbacks.onStage (t[1], t[2].getIntValue(), t[3].getIntValue());
        }
        else if (line.startsWith ("PROGRESS "))
        {
            if (callbacks.onProgress != nullptr)
                callbacks.onProgress (
                    (float) juce::jlimit (0, 100, line.substring (9).trim().getIntValue())
                    / 100.0f);
        }
        else if (line.startsWith ("DONE "))
        {
            donePath = line.substring (5).trim();
        }
        else if (line.startsWith ("ERROR "))
        {
            errorMessage = line.substring (6).trim();
        }
        else if (line.isNotEmpty())
        {
            lastNoise = line;
        }
    };

    juce::MemoryBlock buffer (4096);
    juce::String pending;

    for (;;)
    {
        // Blocks until output or EOF; a cancel kills the child, which is what
        // ends the wait. No polling loop could cancel faster.
        const auto n = child->readProcessOutput (buffer.getData(), (int) buffer.getSize());

        if (n <= 0)
            break;

        pending += juce::String::fromUTF8 (static_cast<const char*> (buffer.getData()), n);

        for (auto nl = pending.indexOf ("\n"); nl >= 0; nl = pending.indexOf ("\n"))
        {
            parseLine (pending.substring (0, nl));
            pending = pending.substring (nl + 1);
        }
    }

    if (pending.isNotEmpty())
        parseLine (pending);

    // The wait's verdict matters: a child that outlives its closed pipe by
    // ten seconds is hung and has to be killed, and JUCE's POSIX getExitCode
    // reads 0 for a child that is still alive or died to a signal — so a
    // zero on its own proves nothing.
    const auto exitedInTime = child->waitForProcessToFinish (10000);

    if (! exitedInTime)
    {
        child->kill();
        child->waitForProcessToFinish (2000); // reap the corpse, briefly
    }

    const auto exitCode = child->getExitCode();

    {
        const juce::ScopedLock sl (childLock);
        child.reset();
    }

    if (threadShouldExit())
    {
        outputFile.deleteFile(); // a killed sidecar can leave a torso behind
        finish (Result::cancelled, {});
        return;
    }

    // Success is earned three ways — a timely, normal exit AND code 0 AND
    // the protocol's DONE line — because each covers the others' blind spot:
    // a signal-killed child reads exit code 0, and the output file can exist
    // half-written under a child we had to kill ourselves. A zero exit
    // without the file is still a failure — success is a render.
    if (! exitedInTime || exitCode != 0 || donePath.isEmpty()
        || ! outputFile.existsAsFile())
    {
        outputFile.deleteFile();

        // A kill of our own, or a signal death wearing exit code 0, outranks
        // whatever the child managed to say: it did not exit cleanly, and
        // that is the story.
        finish (Result::failed,
                ! exitedInTime || (exitCode == 0 && donePath.isEmpty())
                                            ? "the sidecar did not exit cleanly"
                : errorMessage.isNotEmpty() ? errorMessage
                : lastNoise.isNotEmpty()    ? lastNoise
                                            : "sidecar exited with code " + juce::String (exitCode));
        return;
    }

    finish (Result::ok, donePath);
}

} // namespace splittr
