#include "SeparationEngine.h"
#include "DemucsSeparator.h"
#include "StemCache.h"
#include "TrackAnalyzer.h"

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
    DownloadJob (SeparationEngine& e, int gen, Quality q)
        : ThreadPoolJob ("model download"), engine (e), generation (gen), quality (q) {}

    // Fetch every missing weight file for `quality` (1 for fast, up to 4 for
    // best), with progress aggregated across files by bytes.
    static DownloadOutcome download (juce::ThreadPoolJob& job, SeparationEngine& engine,
                                     int gen, Quality quality, juce::String& error)
    {
        ModelManager::modelsDir().createDirectory();

        const auto missing = engine.models.missingFor (quality);

        if (missing.empty())
            return DownloadOutcome::ok;

        const auto totalBytes = (juce::int64) missing.size() * ModelManager::bytesPerModel;
        juce::int64 doneBytes = 0;

        const auto startMs = juce::Time::getMillisecondCounterHiRes();

        for (const auto& missingFile : missing)
        {
            // Always download into the managed models dir (the user-located
            // dir stays read-only for us).
            const auto target = ModelManager::modelsDir().getChildFile (missingFile.getFileName());
            auto part = target.getSiblingFile (target.getFileName() + ".part");
            part.deleteFile();

            auto task = juce::URL (ModelManager::urlFor (target.getFileName()))
                            .downloadToFile (part, juce::URL::DownloadTaskOptions());

            if (task == nullptr)
            {
                error = "COULD NOT START DOWNLOAD";
                return DownloadOutcome::failed;
            }

            while (! task->isFinished())
            {
                if (job.shouldExit())
                {
                    task.reset();
                    part.deleteFile(); // completed files stay: resume-friendly
                    return DownloadOutcome::cancelled;
                }

                juce::Thread::sleep (100);

                const auto done = doneBytes + task->getLengthDownloaded();
                const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;

                engine.mutate (gen, [done, totalBytes, elapsed] (EngineSnapshot& s)
                {
                    s.phase = Phase::Downloading;
                    s.downloadDone = done;
                    s.downloadTotal = totalBytes;
                    s.downloadMBps = elapsed > 0.5 ? (double) done / (1024.0 * 1024.0) / elapsed : 0.0;
                    s.progress = (float) ((double) done / (double) totalBytes);
                });
            }

            if (task->hadError() || part.getSize() != ModelManager::bytesPerModel)
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

            doneBytes += ModelManager::bytesPerModel;
        }

        return DownloadOutcome::ok;
    }

    JobStatus runJob() override
    {
        juce::String error;

        switch (download (*this, engine, generation, quality, error))
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
    const Quality quality;
};

class SeparationEngine::SeparationJob : public juce::ThreadPoolJob
{
public:
    SeparationJob (SeparationEngine& e, juce::File f, int gen, Quality q)
        : ThreadPoolJob ("separation"), engine (e), source (std::move (f)),
          generation (gen), quality (q) {}

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
        if (! engine.modelReady (quality))
        {
            juce::String error;

            switch (DownloadJob::download (*this, engine, generation, quality, error))
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
        const auto numModels = quality == Quality::best ? 4 : 1;
        const auto segmentCount = numModels * DemucsSeparator::expectedSegments (decoded.buffer.getNumSamples());

        const auto analysis = TrackAnalyzer::analyze (decoded.buffer);

        engine.mutate (generation, [&] (EngineSnapshot& s)
        {
            s.phase = Phase::Separating;
            s.progress = 0.05f;
            s.sourceDuration = duration;
            s.sourceWaveform = sourceWave;
            s.segmentCount = segmentCount;
            s.bpm = analysis.bpm;
            s.key = analysis.key;
        });

        // --- separate -----------------------------------------------------
        const auto inferenceStart = juce::Time::getMillisecondCounterHiRes();
        int segmentsDone = 0;

        SeparationResult result;

        const auto status = DemucsSeparator::separate (
            engine.models.filesFor (quality),
            decoded.buffer,
            [&] (float p, const juce::String& message)
            {
                if (message.contains ("apply model"))
                    ++segmentsDone;

                const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - inferenceStart) / 1000.0;
                const auto eta = p > 0.02f ? elapsed * (1.0 - p) / p : -1.0;

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

        auto entry = StemCache::entryFor (source, quality);
        entry.dir.createDirectory();

        const juce::AudioBuffer<float>* buffers[numStems] = {
            &result.vocals, &result.drums, &result.bass, &result.other, &result.instrumental
        };

        // StemIndex order; peaks come from the source order drums,bass,other,vocals.
        const float stemPeaksDb[numStems] = {
            result.sourcePeaksDb[3], result.sourcePeaksDb[0], result.sourcePeaksDb[1],
            result.sourcePeaksDb[2], result.instrumentalPeakDb
        };

        std::array<std::vector<float>, numStems> waves;

        for (int i = 0; i < numStems; ++i)
        {
            if (! engine.io.writeWavFloat32 (entry.stems[(size_t) i], *buffers[i],
                                             AudioFileIO::kTargetSampleRate, error))
            {
                entry.dir.deleteRecursively();
                return fail ("COULD NOT WRITE STEMS");
            }

            waves[(size_t) i] = AudioFileIO::peakColumns (*buffers[i], 70);
        }

        const auto rtFactor = inferenceSeconds > 0 ? duration / inferenceSeconds : 0.0;

        juce::XmlElement meta ("SplittrStems");
        meta.setAttribute ("version", 2);
        meta.setAttribute ("sourcePath", source.getFullPathName());
        meta.setAttribute ("quality", quality == Quality::best ? "best" : "fast");
        meta.setAttribute ("duration", duration);
        meta.setAttribute ("inferenceSeconds", inferenceSeconds);
        meta.setAttribute ("realtimeFactor", rtFactor);
        meta.setAttribute ("bpm", analysis.bpm);
        meta.setAttribute ("key", analysis.key);
        meta.setAttribute ("sourceWave", metaCsv (sourceWave));

        for (int i = 0; i < numStems; ++i)
        {
            meta.setAttribute ("wave" + juce::String (stemName (i)), metaCsv (waves[(size_t) i]));
            meta.setAttribute ("peak" + juce::String (stemName (i)), (double) stemPeaksDb[i]);
        }

        meta.writeTo (entry.meta);

        StemCache::prune();

        engine.mutate (generation, [&] (EngineSnapshot& s)
        {
            s.phase = Phase::Ready;
            s.progress = 1.0f;

            for (int i = 0; i < numStems; ++i)
            {
                s.stems[(size_t) i].wav = entry.stems[(size_t) i];
                s.stems[(size_t) i].waveform = waves[(size_t) i];
                s.stems[(size_t) i].peakDb = stemPeaksDb[i];
            }

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
    const Quality quality;
};

//==============================================================================

SeparationEngine::SeparationEngine()
    : pool (juce::ThreadPoolOptions {}
                .withNumberOfThreads (1)
                .withThreadName ("Splittr Separation")
                .withDesiredThreadPriority (juce::Thread::Priority::low))
{
    snapshot.quality = models.quality();
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
    const auto quality = models.quality();

    if (modelReady (quality))
        return;

    const auto gen = generation.fetch_add (1) + 1;

    pool.removeAllJobs (true, 0);

    {
        const juce::ScopedLock sl (lock);
        snapshot.phase = Phase::Downloading;
        snapshot.message.clear();
        snapshot.progress = 0.0f;
        snapshot.downloadDone = 0;
        snapshot.downloadTotal = (juce::int64) models.missingFor (quality).size()
                                 * ModelManager::bytesPerModel;
        snapshot.downloadMBps = 0.0;
    }

    pool.addJob (new DownloadJob (*this, gen, quality), true);
}

void SeparationEngine::setQuality (Quality q)
{
    if (models.quality() == q)
        return;

    models.setQuality (q);

    bool interruptible = false;

    {
        const juce::ScopedLock sl (lock);
        snapshot.quality = q;
        interruptible = snapshot.phase == Phase::Idle || snapshot.phase == Phase::NeedsModel
                        || snapshot.phase == Phase::Downloading || snapshot.phase == Phase::Error;
    }

    // Mid-job or on the done view the switch only affects the next run.
    if (! interruptible)
        return;

    generation.fetch_add (1);
    pool.removeAllJobs (true, 0);

    if (modelReady (q))
    {
        const juce::ScopedLock sl (lock);
        snapshot.phase = Phase::Idle;
        snapshot.message.clear();
        snapshot.progress = 0.0f;
    }
    else
    {
        startModelDownload();
    }
}

bool SeparationEngine::tryPublishCached (const juce::File& source, int gen, Quality quality)
{
    const auto entry = StemCache::entryFor (source, quality);

    if (! entry.complete())
        return false;

    auto xml = juce::parseXML (entry.meta);

    if (xml == nullptr || xml->getIntAttribute ("version") != 2)
        return false;

    mutate (gen, [&] (EngineSnapshot& s)
    {
        s.phase = Phase::Ready;
        s.progress = 1.0f;
        s.message.clear();
        s.sourceFile = source;
        s.sourceDuration = xml->getDoubleAttribute ("duration");
        s.sourceWaveform = metaUncsv (xml->getStringAttribute ("sourceWave"));

        for (int i = 0; i < numStems; ++i)
        {
            s.stems[(size_t) i].wav = entry.stems[(size_t) i];
            s.stems[(size_t) i].waveform = metaUncsv (xml->getStringAttribute ("wave" + juce::String (stemName (i))));
            s.stems[(size_t) i].peakDb = (float) xml->getDoubleAttribute ("peak" + juce::String (stemName (i)), -120.0);
        }

        // source order drums,bass,other,vocals for the working table
        s.sourcePeaksDb = { s.stems[stemDrums].peakDb, s.stems[stemBass].peakDb,
                            s.stems[stemOther].peakDb, s.stems[stemVocals].peakDb };
        s.peaksKnown = true;
        s.inferenceSeconds = xml->getDoubleAttribute ("inferenceSeconds");
        s.realtimeFactor = xml->getDoubleAttribute ("realtimeFactor");
        s.bpm = xml->getDoubleAttribute ("bpm");
        s.key = xml->getStringAttribute ("key");
    });

    return true;
}

void SeparationEngine::requestSeparation (const juce::File& file)
{
    const auto gen = generation.fetch_add (1) + 1;
    const auto quality = models.quality();

    pool.removeAllJobs (true, 0);

    if (tryPublishCached (file, gen, quality))
        return;

    {
        const juce::ScopedLock sl (lock);
        snapshot.phase = Phase::Decoding;
        snapshot.progress = 0.0f;
        snapshot.message.clear();
        snapshot.sourceFile = file;
        snapshot.peaksKnown = false;
    }

    pool.addJob (new SeparationJob (*this, file, gen, quality), true);
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
    snapshot.quality = models.quality();
    snapshot.phase = modelReady() ? Phase::Idle : Phase::NeedsModel;
}

void SeparationEngine::locateModel (const juce::File& file)
{
    if (! file.existsAsFile())
        return;

    models.setUserModelDir (file.getParentDirectory());

    if (! modelReady())
        return; // still incomplete (e.g. best mode needs all 4 files present)

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

    if (! tryPublishCached (source, gen, models.quality()))
    {
        const juce::ScopedLock sl (lock);
        snapshot.sourceFile = source;
    }
}

} // namespace splittr
