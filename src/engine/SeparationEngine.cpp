#include "SeparationEngine.h"
#include "DemucsSeparator.h"
#include "StemCache.h"

namespace splittr
{

namespace
{
    enum class DownloadOutcome { ok, cancelled, failed };

    juce::String metaCsv (const std::vector<float>& values)
    {
        juce::StringArray parts;
        for (auto v : values)
            parts.add (juce::String (v, 4));
        return parts.joinIntoString (",");
    }

    std::vector<float> metaUncsv (const juce::String& csv)
    {
        std::vector<float> values;
        for (const auto& part : juce::StringArray::fromTokens (csv, ",", {}))
            values.push_back (part.getFloatValue());
        return values;
    }
} // namespace

//==============================================================================
// Jobs

class SeparationEngine::DownloadJob : public juce::ThreadPoolJob
{
public:
    DownloadJob (SeparationEngine& e, int gen)
        : ThreadPoolJob ("model download"), engine (e), generation (gen) {}

    static DownloadOutcome download (juce::ThreadPoolJob& job, SeparationEngine& engine,
                                     int gen, juce::String& error)
    {
        const auto target = ModelManager::defaultModelFile();
        target.getParentDirectory().createDirectory();

        auto part = target.getSiblingFile (target.getFileName() + ".part");
        part.deleteFile();

        auto task = juce::URL (ModelManager::downloadUrl)
                        .downloadToFile (part, juce::URL::DownloadTaskOptions());

        if (task == nullptr)
        {
            error = "COULD NOT START DOWNLOAD";
            return DownloadOutcome::failed;
        }

        const auto startMs = juce::Time::getMillisecondCounterHiRes();

        while (! task->isFinished())
        {
            if (job.shouldExit())
            {
                task.reset();
                part.deleteFile();
                return DownloadOutcome::cancelled;
            }

            juce::Thread::sleep (100);

            const auto done = task->getLengthDownloaded();
            const auto total = task->getTotalLength() > 0 ? task->getTotalLength()
                                                          : ModelManager::expectedBytes;
            const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;

            engine.mutate (gen, [done, total, elapsed] (EngineSnapshot& s)
            {
                s.phase = Phase::Downloading;
                s.downloadDone = done;
                s.downloadTotal = total;
                s.downloadMBps = elapsed > 0.5 ? (double) done / (1024.0 * 1024.0) / elapsed : 0.0;
                s.progress = total > 0 ? (float) ((double) done / (double) total) : 0.0f;
            });
        }

        if (task->hadError() || part.getSize() != ModelManager::expectedBytes)
        {
            part.deleteFile();
            error = task->hadError() ? "DOWNLOAD FAILED — CHECK CONNECTION"
                                     : "DOWNLOAD INCOMPLETE — RETRY";
            return DownloadOutcome::failed;
        }

        target.deleteFile();

        if (! part.moveFileTo (target))
        {
            part.deleteFile();
            error = "COULD NOT MOVE MODEL INTO PLACE";
            return DownloadOutcome::failed;
        }

        return DownloadOutcome::ok;
    }

    JobStatus runJob() override
    {
        juce::String error;

        switch (download (*this, engine, generation, error))
        {
            case DownloadOutcome::ok:
                engine.mutate (generation, [] (EngineSnapshot& s)
                {
                    s.phase = Phase::Idle;
                    s.message.clear();
                    s.progress = 0.0f;
                });
                break;

            case DownloadOutcome::cancelled:
                engine.mutate (generation, [] (EngineSnapshot& s)
                {
                    s.phase = Phase::NeedsModel;
                    s.message.clear();
                    s.progress = 0.0f;
                });
                break;

            case DownloadOutcome::failed:
                engine.mutate (generation, [error] (EngineSnapshot& s)
                {
                    s.phase = Phase::NeedsModel;
                    s.message = error;
                    s.progress = 0.0f;
                });
                break;
        }

        return jobHasFinished;
    }

private:
    SeparationEngine& engine;
    const int generation;
};

class SeparationEngine::SeparationJob : public juce::ThreadPoolJob
{
public:
    SeparationJob (SeparationEngine& e, juce::File f, int gen)
        : ThreadPoolJob ("separation"), engine (e), source (std::move (f)), generation (gen) {}

    JobStatus runJob() override
    {
        const auto fail = [this] (juce::String reason)
        {
            engine.mutate (generation, [reason] (EngineSnapshot& s)
            {
                s.phase = Phase::Error;
                s.message = reason;
                s.progress = 0.0f;
            });
            return jobHasFinished;
        };

        // --- ensure model -------------------------------------------------
        if (! engine.modelReady())
        {
            juce::String error;

            switch (DownloadJob::download (*this, engine, generation, error))
            {
                case DownloadOutcome::ok: break;
                case DownloadOutcome::cancelled: return jobHasFinished; // requestCancel published state
                case DownloadOutcome::failed: return fail (error);
            }
        }

        // --- decode -------------------------------------------------------
        engine.mutate (generation, [this] (EngineSnapshot& s)
        {
            s.phase = Phase::Decoding;
            s.progress = 0.0f;
            s.message.clear();
            s.sourceFile = source;
            s.peaksKnown = false;
            s.segment = 0;
            s.segmentCount = 0;
            s.etaSeconds = -1.0;
        });

        DecodedAudio decoded;
        juce::String error;

        if (! engine.io.decodeToStereo44k (source, decoded, error))
            return fail ("UNSUPPORTED OR UNREADABLE FILE");

        if (decoded.durationSeconds > SeparationEngine::maxInputSeconds)
            return fail ("FILE TOO LONG — 10 MIN MAX");

        if (shouldExit())
            return jobHasFinished;

        auto sourceWave = AudioFileIO::peakColumns (decoded.buffer, 60);
        const auto duration = decoded.durationSeconds;
        const auto segmentCount = DemucsSeparator::expectedSegments (decoded.buffer.getNumSamples());

        engine.mutate (generation, [&] (EngineSnapshot& s)
        {
            s.phase = Phase::Separating;
            s.progress = 0.05f;
            s.sourceDuration = duration;
            s.sourceWaveform = sourceWave;
            s.segmentCount = segmentCount;
        });

        // --- separate -----------------------------------------------------
        const auto inferenceStart = juce::Time::getMillisecondCounterHiRes();
        int segmentsDone = 0;

        SeparationResult result;

        const auto status = DemucsSeparator::separate (
            engine.models.resolve(),
            decoded.buffer,
            [&] (float p, const juce::String& message)
            {
                if (message.contains ("apply model"))
                    ++segmentsDone;

                const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - inferenceStart) / 1000.0;
                const auto eta = p > 0.05f ? elapsed * (1.0 - p) / p : -1.0;

                engine.mutate (generation, [&] (EngineSnapshot& s)
                {
                    s.progress = 0.15f + 0.80f * p;
                    s.segment = juce::jmin (segmentsDone, s.segmentCount);
                    s.etaSeconds = eta;
                });
            },
            [this] { return ! shouldExit(); },
            result,
            error);

        if (status == DemucsSeparator::Status::cancelled || shouldExit())
            return jobHasFinished; // requestCancel already published Idle

        if (status == DemucsSeparator::Status::failed)
            return fail (error.toUpperCase().substring (0, 48));

        const auto inferenceSeconds = (juce::Time::getMillisecondCounterHiRes() - inferenceStart) / 1000.0;

        // --- render + cache -----------------------------------------------
        engine.mutate (generation, [] (EngineSnapshot& s)
        {
            s.phase = Phase::Rendering;
            s.progress = 0.95f;
        });

        auto entry = StemCache::entryFor (source);
        entry.dir.createDirectory();

        if (! engine.io.writeWavFloat32 (entry.vocals, result.vocals, AudioFileIO::kTargetSampleRate, error)
            || ! engine.io.writeWavFloat32 (entry.instrumental, result.instrumental, AudioFileIO::kTargetSampleRate, error))
        {
            entry.dir.deleteRecursively();
            return fail ("COULD NOT WRITE STEMS");
        }

        auto vocalsWave = AudioFileIO::peakColumns (result.vocals, 70);
        auto instrumentalWave = AudioFileIO::peakColumns (result.instrumental, 70);
        const auto rtFactor = inferenceSeconds > 0 ? duration / inferenceSeconds : 0.0;

        juce::XmlElement meta ("SplittrStems");
        meta.setAttribute ("version", 1);
        meta.setAttribute ("sourcePath", source.getFullPathName());
        meta.setAttribute ("duration", duration);
        meta.setAttribute ("inferenceSeconds", inferenceSeconds);
        meta.setAttribute ("realtimeFactor", rtFactor);
        meta.setAttribute ("peaksDb", juce::String (result.sourcePeaksDb[0], 2) + ","
                                          + juce::String (result.sourcePeaksDb[1], 2) + ","
                                          + juce::String (result.sourcePeaksDb[2], 2) + ","
                                          + juce::String (result.sourcePeaksDb[3], 2));
        meta.setAttribute ("sourceWave", metaCsv (sourceWave));
        meta.setAttribute ("vocalsWave", metaCsv (vocalsWave));
        meta.setAttribute ("instrumentalWave", metaCsv (instrumentalWave));
        meta.writeTo (entry.meta);

        StemCache::prune();

        engine.mutate (generation, [&] (EngineSnapshot& s)
        {
            s.phase = Phase::Ready;
            s.progress = 1.0f;
            s.vocalsWav = entry.vocals;
            s.instrumentalWav = entry.instrumental;
            s.vocalsWaveform = vocalsWave;
            s.instrumentalWaveform = instrumentalWave;
            s.sourcePeaksDb = result.sourcePeaksDb;
            s.peaksKnown = true;
            s.inferenceSeconds = inferenceSeconds;
            s.realtimeFactor = rtFactor;
            s.etaSeconds = -1.0;
        });

        return jobHasFinished;
    }

private:
    SeparationEngine& engine;
    const juce::File source;
    const int generation;
};

//==============================================================================

SeparationEngine::SeparationEngine()
    : pool (juce::ThreadPoolOptions {}
                .withNumberOfThreads (1)
                .withThreadName ("Splittr Separation")
                .withDesiredThreadPriority (juce::Thread::Priority::low))
{
    snapshot.phase = modelReady() ? Phase::Idle : Phase::NeedsModel;
}

SeparationEngine::~SeparationEngine()
{
    generation.fetch_add (1);
    pool.removeAllJobs (true, 20000);
}

void SeparationEngine::mutate (int gen, const std::function<void (EngineSnapshot&)>& fn)
{
    if (gen != generation.load())
        return;

    const juce::ScopedLock sl (lock);
    fn (snapshot);
}

EngineSnapshot SeparationEngine::getSnapshot() const
{
    const juce::ScopedLock sl (lock);
    return snapshot;
}

void SeparationEngine::editorOpened()
{
    if (! modelReady() && ! autoDownloadTried.exchange (true))
        startModelDownload();
}

void SeparationEngine::startModelDownload()
{
    if (modelReady())
        return;

    const auto gen = generation.fetch_add (1) + 1;

    pool.removeAllJobs (true, 0);

    {
        const juce::ScopedLock sl (lock);
        snapshot.phase = Phase::Downloading;
        snapshot.message.clear();
        snapshot.progress = 0.0f;
        snapshot.downloadDone = 0;
        snapshot.downloadTotal = ModelManager::expectedBytes;
        snapshot.downloadMBps = 0.0;
    }

    pool.addJob (new DownloadJob (*this, gen), true);
}

bool SeparationEngine::tryPublishCached (const juce::File& source, int gen)
{
    const auto entry = StemCache::entryFor (source);

    if (! entry.complete())
        return false;

    auto xml = juce::parseXML (entry.meta);

    if (xml == nullptr || xml->getIntAttribute ("version") != 1)
        return false;

    auto peaks = metaUncsv (xml->getStringAttribute ("peaksDb"));

    if (peaks.size() != 4)
        return false;

    mutate (gen, [&] (EngineSnapshot& s)
    {
        s.phase = Phase::Ready;
        s.progress = 1.0f;
        s.message.clear();
        s.sourceFile = source;
        s.sourceDuration = xml->getDoubleAttribute ("duration");
        s.sourceWaveform = metaUncsv (xml->getStringAttribute ("sourceWave"));
        s.vocalsWav = entry.vocals;
        s.instrumentalWav = entry.instrumental;
        s.vocalsWaveform = metaUncsv (xml->getStringAttribute ("vocalsWave"));
        s.instrumentalWaveform = metaUncsv (xml->getStringAttribute ("instrumentalWave"));
        s.sourcePeaksDb = { peaks[0], peaks[1], peaks[2], peaks[3] };
        s.peaksKnown = true;
        s.inferenceSeconds = xml->getDoubleAttribute ("inferenceSeconds");
        s.realtimeFactor = xml->getDoubleAttribute ("realtimeFactor");
    });

    return true;
}

void SeparationEngine::requestSeparation (const juce::File& file)
{
    const auto gen = generation.fetch_add (1) + 1;

    pool.removeAllJobs (true, 0);

    if (tryPublishCached (file, gen))
        return;

    {
        const juce::ScopedLock sl (lock);
        snapshot.phase = Phase::Decoding;
        snapshot.progress = 0.0f;
        snapshot.message.clear();
        snapshot.sourceFile = file;
        snapshot.peaksKnown = false;
    }

    pool.addJob (new SeparationJob (*this, file, gen), true);
}

void SeparationEngine::requestCancel()
{
    const auto wasDownloadOnly = [this]
    {
        const juce::ScopedLock sl (lock);
        return snapshot.phase == Phase::Downloading && snapshot.sourceFile == juce::File();
    }();

    generation.fetch_add (1);
    pool.removeAllJobs (true, 0);

    const juce::ScopedLock sl (lock);
    snapshot.phase = wasDownloadOnly || ! modelReady() ? Phase::NeedsModel : Phase::Idle;
    snapshot.progress = 0.0f;
    snapshot.message.clear();
    snapshot.etaSeconds = -1.0;
}

void SeparationEngine::requestNewFile()
{
    generation.fetch_add (1);
    pool.removeAllJobs (true, 0);

    const juce::ScopedLock sl (lock);
    snapshot = EngineSnapshot {};
    snapshot.phase = modelReady() ? Phase::Idle : Phase::NeedsModel;
}

void SeparationEngine::locateModel (const juce::File& file)
{
    if (! file.existsAsFile())
        return;

    models.setUserOverride (file);

    generation.fetch_add (1);
    pool.removeAllJobs (true, 0);

    const juce::ScopedLock sl (lock);

    if (snapshot.phase == Phase::NeedsModel || snapshot.phase == Phase::Downloading)
    {
        snapshot.phase = Phase::Idle;
        snapshot.message.clear();
        snapshot.progress = 0.0f;
    }
}

juce::String SeparationEngine::sourcePathForState() const
{
    const juce::ScopedLock sl (lock);
    return snapshot.sourceFile.getFullPathName();
}

void SeparationEngine::restoreFromState (const juce::String& sourcePath)
{
    if (sourcePath.isEmpty())
        return;

    const juce::File source (sourcePath);

    if (! source.existsAsFile())
        return;

    const auto gen = generation.fetch_add (1) + 1;

    if (! tryPublishCached (source, gen))
    {
        const juce::ScopedLock sl (lock);
        snapshot.sourceFile = source;
    }
}

} // namespace splittr
