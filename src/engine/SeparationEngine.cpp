#include "SeparationEngine.h"
#include "DemucsSeparator.h"
#include "MidiTranscriber.h"
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

    // Bumped to 4 when the stem set became variable: an entry now records the
    // stems it holds, so it can be reopened without consulting the current
    // engine mode. (3: normalised, sqrt-mapped waveform columns.)
    //
    // Deliberately NOT bumped for the export naming (issue #11): the filenames
    // are an extra attribute, and a v4 entry without it holds exactly the
    // <Name>.wav layout entryIn falls back to. Bumping would re-separate every
    // cached song to gain nothing.
    constexpr int metaVersion = 4;

    // The names the entry's stems actually live under, parallel to "stems".
    // Absent before the export pattern existed; entryIn then falls back.
    //
    // Comma-joined like "stems": stemFileName runs the one free-form part (the
    // song title) through createLegalFileName, which strips commas.
    juce::StringArray stemFilesFrom (const juce::XmlElement& xml)
    {
        return juce::StringArray::fromTokens (xml.getStringAttribute ("stemFiles"), ",", {});
    }

    // What travels inside the file as well as in its name. LIST/INFO only:
    // an 'acid' chunk would carry the tempo too, but it also marks the file as
    // a loop, and a DAW that reads it would warp a whole stem to the project
    // tempo on import.
    juce::StringPairArray stemMetadata (const juce::String& songName,
                                        const juce::String& stem,
                                        double bpm, const juce::String& key)
    {
        juce::StringPairArray values;
        values.set (juce::WavAudioFormat::riffInfoTitle, songName + " - " + stem);
        values.set (juce::WavAudioFormat::riffInfoSoftware, "Splittr");

        juce::StringArray found;

        if (bpm > 0.0)
            found.add (juce::String (juce::roundToInt (bpm)) + " BPM");

        if (key.isNotEmpty())
            found.add (key);

        if (! found.isEmpty())
            values.set (juce::WavAudioFormat::riffInfoKeywords, found.joinIntoString (", "));

        return values;
    }

    // An entry is usable only when its meta is the current version and every
    // stem it names is still on disk. On success `entry` is re-pointed at the
    // recorded stem set. Both the republish path and the editor's
    // "is this an instant hit?" check go through here, so they cannot disagree.
    std::unique_ptr<juce::XmlElement> readCachedEntry (const juce::File& source, Quality quality,
                                                       CachedStems& entry,
                                                       juce::StringArray& stemNames)
    {
        entry = StemCache::entryFor (source, quality);

        if (! entry.meta.existsAsFile())
            return nullptr;

        auto xml = juce::parseXML (entry.meta);

        if (xml == nullptr || xml->getIntAttribute ("version") != metaVersion)
            return nullptr;

        stemNames = juce::StringArray::fromTokens (xml->getStringAttribute ("stems"), ",", {});

        if (stemNames.isEmpty())
            return nullptr;

        entry = StemCache::entryIn (entry.dir, stemNames, stemFilesFrom (*xml));

        return entry.complete() ? std::move (xml) : nullptr;
    }

    // A transcription lives beside its stem, so an entry that was transcribed
    // in an earlier session comes back with its chips already lit — no meta
    // version bump needed, and none that could go stale (issue #19).
    void attachExistingMidi (EngineSnapshot::StemInfo& info)
    {
        if (! MidiTranscriber::supports (info.kind))
            return;

        const auto midi = MidiTranscriber::midiFileFor (info.wav);

        if (midi.existsAsFile())
        {
            info.midi = midi;
            info.midiState = EngineSnapshot::MidiState::ready;
        }
    }

    // --- per-stem shaping (issue #9) ---------------------------------------
    // Two attributes per stem, keyed on the canonical name like the waveforms
    // and peaks already are: "fxVocals" is what the user dialled in,
    // "fxBouncedVocals" is what the file in tuned/ was rendered from. Both are
    // absent on a stem that was never tuned, and no meta version bump — an
    // entry without them is one that predates the feature, exactly as with the
    // export names (issue #11) and the transcriptions (issue #19).
    juce::String fxAttribute (const juce::String& stemName) { return "fx" + stemName; }
    juce::String fxBouncedAttribute (const juce::String& stemName) { return "fxBounced" + stemName; }

    // The shaped twin of a raw stem: same filename, one directory down, so what
    // lands in the DAW keeps the name the raw render would have had.
    juce::File tunedFileFor (const juce::File& wav)
    {
        return wav.getParentDirectory().getChildFile ("tuned").getChildFile (wav.getFileName());
    }

    // --- clean vocal (issue #29) -------------------------------------------
    // One attribute in the fx tradition, keyed on the canonical name even
    // though only Vocals ever carries it: "cleanVocals" holds the params the
    // active render was made with, and its absence means the raw render.
    juce::String cleanAttribute (const juce::String& stemName) { return "clean" + stemName; }

    // Where a render for these params lives: <entry>/clean/, named by the
    // params themselves (VocalCleanParams::cacheTag), so identical params can
    // re-activate without a rerun and a human can read the dir.
    juce::File cleanFileFor (const juce::File& stemWav, const VocalCleanParams& params)
    {
        return stemWav.getParentDirectory().getChildFile ("clean")
            .getChildFile ("Vocals_" + params.cacheTag() + ".wav");
    }

    // A clean render recorded in the meta comes back active on reopen when its
    // file survived — the same contract the transcriptions and fx have, and no
    // meta version bump for the same reason (issue #29).
    void attachExistingClean (EngineSnapshot& s, const juce::XmlElement& xml)
    {
        s.vocalClean = {};

        for (const auto& stem : s.stems)
        {
            if (stem.kind != stemVocals)
                continue;

            const auto params = VocalCleanParams::fromDescriptor (
                xml.getStringAttribute (cleanAttribute (stem.name)));

            if (params.isNoop())
                return;

            const auto render = cleanFileFor (stem.wav, params);

            if (render.existsAsFile())
            {
                s.vocalClean.state = EngineSnapshot::VocalClean::State::done;
                s.vocalClean.progress = 1.0f;
                s.vocalClean.params = params;
                s.vocalClean.file = render;
            }

            return;
        }
    }

    // Rewrite an entry's meta.xml with `values` set on it; an empty value
    // removes its attribute. Parse-and-write rather than regenerate: the meta
    // is written by the job that produced the entry, and tuning must not be
    // able to lose anything else it recorded.
    bool writeMetaAttributes (const juce::File& meta, const juce::StringPairArray& values)
    {
        auto xml = juce::parseXML (meta);

        if (xml == nullptr)
            return false;

        for (const auto& key : values.getAllKeys())
        {
            if (values[key].isEmpty())
                xml->removeAttribute (key);
            else
                xml->setAttribute (key, values[key]);
        }

        return xml->writeTo (meta);
    }

    // Render a stem through its chain, offline, at the file's own rate and in
    // its own float32 WAV format. Streamed in blocks: a stem is a hundred-odd
    // megabytes, and there is no reason for two copies of it to exist at once.
    //
    // Synchronous by design — see processedStemFile.
    bool bounceStem (const juce::File& source, const juce::File& target, const StemFxParams& fx)
    {
        constexpr int blockSamples = 8192;

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));

        if (reader == nullptr || reader->numChannels == 0 || reader->sampleRate <= 0.0
            || reader->lengthInSamples <= 0)
            return false;

        target.getParentDirectory().createDirectory();

        // The bounce is written beside its destination and moved into place
        // only once it is complete, so a drag can never pick up a half file.
        auto part = target.getSiblingFile (target.getFileName() + ".part");
        part.deleteFile();

        std::unique_ptr<juce::OutputStream> stream = part.createOutputStream();

        if (stream == nullptr)
            return false;

        auto options = juce::AudioFormatWriterOptions {}
                           .withSampleRate (reader->sampleRate)
                           .withNumChannels (2)
                           .withBitsPerSample (32)
                           .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

        // Carry the raw stem's LIST/INFO across: the tuned file is the same
        // stem of the same song, and should say so in the DAW.
        for (const auto* key : { juce::WavAudioFormat::riffInfoTitle,
                                 juce::WavAudioFormat::riffInfoSoftware,
                                 juce::WavAudioFormat::riffInfoKeywords })
            if (reader->metadataValues.containsKey (key))
                options = options.withMetadata (key, reader->metadataValues[key]);

        juce::WavAudioFormat wavFormat;
        auto writer = wavFormat.createWriterFor (stream, options);

        if (writer == nullptr)
        {
            part.deleteFile();
            return false;
        }

        // Params before prepare: prepare() snaps the output gain to whatever
        // the chain is already set to, so the bounce does not open on a ramp
        // the audition never had.
        StemFxChain chain;
        chain.setParams (fx);
        chain.prepare (reader->sampleRate, blockSamples);

        // Stems are always rendered stereo; a mono one is doubled rather than
        // run through the chain twice with one buffer.
        const auto stereoSource = reader->numChannels > 1;
        juce::AudioBuffer<float> block (2, blockSamples);

        auto ok = true;

        for (juce::int64 pos = 0; pos < reader->lengthInSamples;)
        {
            const auto todo = (int) juce::jmin ((juce::int64) blockSamples,
                                                reader->lengthInSamples - pos);

            if (! reader->read (&block, 0, todo, pos, true, stereoSource))
            {
                ok = false;
                break;
            }

            if (! stereoSource)
                block.copyFrom (1, 0, block, 0, 0, todo);

            chain.process (block.getWritePointer (0), block.getWritePointer (1), todo);

            if (! writer->writeFromAudioSampleBuffer (block, 0, todo))
            {
                ok = false;
                break;
            }

            pos += todo;
        }

        writer.reset(); // flush and close before the file is moved

        if (! ok)
        {
            part.deleteFile();
            return false;
        }

        target.deleteFile();

        if (part.moveFileTo (target))
            return true;

        part.deleteFile();
        return false;
    }
} // namespace

//==============================================================================
// Jobs

class SeparationEngine::DownloadJob : public juce::ThreadPoolJob
{
public:
    DownloadJob (SeparationEngine& e, int gen, Quality q)
        : ThreadPoolJob ("model download"), engine (e), generation (gen), quality (q) {}

    // Fetch every missing weight file for `quality` (1 for fast and 6-stem, up
    // to 4 for best), with progress aggregated across files by bytes. Sizes are
    // per file: the 6-source export is not the same size as the 4-source ones.
    static DownloadOutcome download (juce::ThreadPoolJob& job, SeparationEngine& engine,
                                     int gen, Quality quality, juce::String& error)
    {
        ModelManager::modelsDir().createDirectory();

        const auto missing = engine.models.missingFor (quality);

        if (missing.empty())
            return DownloadOutcome::ok;

        engine.logEvent ("downloading " + juce::String ((int) missing.size()) + " model file(s)");

        juce::int64 totalBytes = 0;

        for (const auto& missingFile : missing)
            totalBytes += ModelManager::expectedBytes (missingFile.getFileName());

        juce::int64 doneBytes = 0;

        const auto startMs = juce::Time::getMillisecondCounterHiRes();

        for (const auto& missingFile : missing)
        {
            // Always download into the managed models dir (the user-located
            // dir stays read-only for us).
            const auto target = ModelManager::modelsDir().getChildFile (missingFile.getFileName());
            const auto expectedBytes = ModelManager::expectedBytes (target.getFileName());
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

            if (task->hadError() || part.getSize() != expectedBytes)
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

            doneBytes += expectedBytes;
            engine.logEvent ("downloaded " + target.getFileName());
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
            engine.logEvent ("error: " + reason);
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
        const auto numModels = (int) engine.models.filesFor (quality).size();
        const auto segmentCount = numModels * DemucsSeparator::expectedSegments (decoded.buffer.getNumSamples());

        engine.logEvent ("decoded " + source.getFileName() + ": "
                         + juce::String (duration, 1) + " s @ "
                         + juce::String (decoded.originalSampleRate, 0) + " Hz");

        auto analysis = TrackAnalyzer::analyze (decoded.buffer);
        auto bpmFromDrums = false;

        engine.logEvent (analysis.bpm > 0.0
                             ? "mix analysis: " + juce::String (analysis.bpm, 1) + " BPM (conf "
                                   + juce::String (analysis.bpmConfidence, 2) + "), key "
                                   + (analysis.key.isNotEmpty() ? analysis.key : juce::String ("none"))
                             : "mix analysis: bpm declined (conf "
                                   + juce::String (analysis.bpmConfidence, 2) + "), key "
                                   + (analysis.key.isNotEmpty() ? analysis.key : juce::String ("none")));

        engine.mutate (generation, [&] (EngineSnapshot& s)
        {
            s.phase = Phase::Separating;
            s.progress = 0.05f;
            s.sourceDuration = duration;
            s.sourceWaveform = sourceWave;
            s.segmentCount = segmentCount;
            s.bpm = analysis.bpm;
            s.bpmConfidence = analysis.bpmConfidence;
            s.key = analysis.key;
        });

        engine.logEvent ("separating: " + juce::String (ModelManager::qualityToken (quality))
                         + ", backend requested "
                         + (SeparationEngine::backend() == ComputeBackend::gpu ? "gpu" : "cpu"));

        // --- separate -----------------------------------------------------
        const auto inferenceStart = juce::Time::getMillisecondCounterHiRes();
        int segmentsDone = 0;

        SeparationResult result;

        // Read here, not at construction: flipping PROCESSING applies to the
        // next run, the same contract the quality switch has.
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
            error,
            SeparationEngine::backend());

        if (status == DemucsSeparator::Status::cancelled || shouldExit())
            return jobHasFinished; // requestCancel already published Idle

        if (status == DemucsSeparator::Status::failed)
            return fail (error.toUpperCase().substring (0, 48));

        const auto inferenceSeconds = (juce::Time::getMillisecondCounterHiRes() - inferenceStart) / 1000.0;

        engine.logEvent ("separated in " + juce::String (inferenceSeconds, 1) + " s ("
                         + juce::String (decoded.durationSeconds / juce::jmax (0.001, inferenceSeconds), 2)
                         + "x RT) on " + (result.ranOnGpu ? "gpu" : "cpu"));

        // The weights decide how many sources came back; if that disagrees with
        // the mode the user picked, the model set on disk is not what its
        // filename claims.
        if (result.numSources != sourceCountFor (quality))
            return fail ("MODEL DOES NOT MATCH ENGINE MODE");

        // The mix can be genuinely ambiguous — issue #27's example is torn
        // 4:3 between two readings and rightly declines — but the separation
        // just manufactured the cleanest tempo evidence there is. When the
        // mix said nothing, ask the drums stem; measured on that example, the
        // drums answer decisively (and on tracks where the mix does detect,
        // the two agree to the decimal). The key stays the mix's — a
        // drums-stem "key" is noise.
        if (analysis.bpm <= 0.0)
        {
            const auto drums = result.indexOfKind (stemDrums);

            if (drums >= 0)
            {
                const auto fromDrums = TrackAnalyzer::analyze (result.stems[(size_t) drums]);
                analysis.bpm = fromDrums.bpm;
                analysis.bpmConfidence = fromDrums.bpmConfidence;
                bpmFromDrums = analysis.bpm > 0.0;

                engine.logEvent (bpmFromDrums
                                     ? "drums fallback: " + juce::String (analysis.bpm, 1)
                                           + " BPM (conf " + juce::String (analysis.bpmConfidence, 2) + ")"
                                     : juce::String ("drums fallback: declined too"));
            }
        }

        // --- render + cache -----------------------------------------------
        engine.mutate (generation, [] (EngineSnapshot& s)
        {
            s.phase = Phase::Rendering;
            s.progress = 0.95f;
        });

        // Stems leave the plugin by being dragged straight out of the cache, so
        // they are written under their export name from the start rather than
        // copied to a named twin later — that would double the disk for nothing
        // (issue #11). The canonical names stay in the meta as the identity the
        // UI and the per-stem attributes are keyed on.
        const auto songName = source.getFileNameWithoutExtension();

        juce::StringArray stemNames, stemFiles;

        for (auto kind : result.kinds)
        {
            stemNames.add (stemName (kind));
            stemFiles.add (stemFileName (songName, stemName (kind), analysis.bpm, analysis.key));
        }

        auto entry = StemCache::entryIn (StemCache::dirFor (source, quality), stemNames, stemFiles);
        entry.dir.createDirectory();

        // A re-render replaces the raw stems, so any shaped bounce beside them
        // is stale by definition — and the meta about to be written carries no
        // fx attributes to remember it by (issue #9). Clean renders were made
        // from the previous raw vocals, so they go the same way (issue #29).
        entry.dir.getChildFile ("tuned").deleteRecursively();
        entry.dir.getChildFile ("clean").deleteRecursively();

        const auto numStems = (int) result.stems.size();

        jassert (numStems == (int) entry.stems.size());

        std::vector<std::vector<float>> waves ((size_t) numStems);

        for (int i = 0; i < numStems; ++i)
        {
            if (! engine.io.writeWavFloat32 (entry.stems[(size_t) i], result.stems[(size_t) i],
                                             AudioFileIO::kTargetSampleRate, error,
                                             stemMetadata (songName, stemNames[i],
                                                           analysis.bpm, analysis.key)))
            {
                entry.dir.deleteRecursively();
                return fail ("COULD NOT WRITE STEMS");
            }

            waves[(size_t) i] = AudioFileIO::peakColumns (result.stems[(size_t) i], 70);
        }

        const auto rtFactor = inferenceSeconds > 0 ? duration / inferenceSeconds : 0.0;

        juce::XmlElement meta ("SplittrStems");
        meta.setAttribute ("version", metaVersion);
        meta.setAttribute ("sourcePath", source.getFullPathName());
        meta.setAttribute ("quality", ModelManager::qualityToken (quality));
        meta.setAttribute ("stems", stemNames.joinIntoString (","));
        meta.setAttribute ("stemFiles", stemFiles.joinIntoString (","));
        meta.setAttribute ("duration", duration);
        meta.setAttribute ("inferenceSeconds", inferenceSeconds);
        meta.setAttribute ("realtimeFactor", rtFactor);
        meta.setAttribute ("backend", result.ranOnGpu ? "gpu" : "cpu");
        meta.setAttribute ("bpm", analysis.bpm);
        meta.setAttribute ("key", analysis.key);
        meta.setAttribute ("sourceWave", metaCsv (sourceWave));

        for (int i = 0; i < numStems; ++i)
        {
            meta.setAttribute ("wave" + stemNames[i], metaCsv (waves[(size_t) i]));
            meta.setAttribute ("peak" + stemNames[i], (double) result.peaksDb[(size_t) i]);
        }

        meta.writeTo (entry.meta);

        engine.logEvent (juce::String (numStems) + " stems written to cache/"
                         + entry.dir.getFileName());

        StemCache::prune();

        engine.mutate (generation, [&] (EngineSnapshot& s)
        {
            s.phase = Phase::Ready;
            s.progress = 1.0f;
            s.stems.clear();

            for (int i = 0; i < numStems; ++i)
            {
                EngineSnapshot::StemInfo info;
                info.kind = result.kinds[(size_t) i];
                info.name = stemNames[i];
                info.wav = entry.stems[(size_t) i];
                info.waveform = waves[(size_t) i];
                info.peakDb = result.peaksDb[(size_t) i];
                attachExistingMidi (info);
                s.stems.push_back (std::move (info));
            }

            s.sourcePeaksDb.clear();

            for (int src = 0; src < result.numSources; ++src)
                s.sourcePeaksDb.push_back (result.sourcePeakDb (src));

            s.peaksKnown = true;
            s.inferenceSeconds = inferenceSeconds;
            s.realtimeFactor = rtFactor;
            s.etaSeconds = -1.0;

            // Republished: the drums fallback may have filled in a tempo the
            // pre-separation pass could not (issue #27).
            s.bpm = analysis.bpm;
            s.bpmConfidence = analysis.bpmConfidence;
            s.bpmFromDrums = bpmFromDrums;

            s.usedGpu = result.ranOnGpu;
            s.gpuRequested = SeparationEngine::backend() == ComputeBackend::gpu;
        });

        return jobHasFinished;
    }

private:
    SeparationEngine& engine;
    const juce::File source;
    const int generation;
    const Quality quality;
};

// One stem to MIDI. Short next to a separation — seconds, not minutes — but it
// takes the same queue, so it can never run beside one.
class SeparationEngine::MidiJob : public juce::ThreadPoolJob
{
public:
    MidiJob (SeparationEngine& e, int gen, int index, juce::File file, int stemKind, double tempo)
        : ThreadPoolJob ("midi"), engine (e), generation (gen), stemIndex (index),
          wav (std::move (file)), kind (stemKind), bpm (tempo) {}

    JobStatus runJob() override
    {
        const auto midi = MidiTranscriber::midiFileFor (wav);

        engine.logEvent ("transcribing " + wav.getFileName());

        MidiTranscription info;
        juce::String error;

        const auto status = MidiTranscriber::transcribe (
            wav, midi, bpm, kind,
            [this] (float p) { publish ([p] (EngineSnapshot::StemInfo& s) { s.midiProgress = p; }); },
            [this] { return ! shouldExit(); },
            info, error);

        if (status == MidiTranscriber::Status::cancelled || shouldExit())
        {
            publish ([] (EngineSnapshot::StemInfo& s)
            {
                s.midiState = EngineSnapshot::MidiState::none;
                s.midiProgress = 0.0f;
            });

            return jobHasFinished;
        }

        if (status != MidiTranscriber::Status::ok)
        {
            // The chip falls back to idle rather than growing an error state.
            // Ready has no error surface — `message` is only ever read in the
            // Error phase — and a failed transcription costs nothing to retry,
            // so the honest thing is to look untried again.
            jassertquiet (error.isNotEmpty());
            engine.logEvent ("transcription failed: " + error);

            publish ([] (EngineSnapshot::StemInfo& s)
            {
                s.midiState = EngineSnapshot::MidiState::none;
                s.midiProgress = 0.0f;
            });

            return jobHasFinished;
        }

        engine.logEvent ("transcribed: " + midi.getFileName()
                         + " (" + juce::String (info.noteCount) + " notes)");

        publish ([midi] (EngineSnapshot::StemInfo& s)
        {
            s.midi = midi;
            s.midiState = EngineSnapshot::MidiState::ready;
            s.midiProgress = 1.0f;
        });

        return jobHasFinished;
    }

private:
    // Every write goes through the generation guard and re-checks the index:
    // the stem set can be replaced under us by a new run or a library open.
    void publish (const std::function<void (EngineSnapshot::StemInfo&)>& fn)
    {
        engine.mutate (generation, [this, &fn] (EngineSnapshot& s)
        {
            if (juce::isPositiveAndBelow (stemIndex, (int) s.stems.size())
                && s.stems[(size_t) stemIndex].wav == wav)
                fn (s.stems[(size_t) stemIndex]);
        });
    }

    SeparationEngine& engine;
    const int generation;
    const int stemIndex;
    const juce::File wav;
    const int kind;
    const double bpm;
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

    // The cleaner before the pool, and explicitly rather than via its own
    // destructor: its callbacks hold this engine, so its thread has to be
    // gone while every member is still whole.
    cleaner.cancel();
    pool.removeAllJobs (true, 20000);
}

void SeparationEngine::mutate (int gen, const std::function<void (EngineSnapshot&)>& fn)
{
    if (gen != generation.load())
        return;

    const juce::ScopedLock sl (lock);
    fn (snapshot);
}

void SeparationEngine::logEvent (const juce::String& line)
{
    // The diagnostics event log (issue #28). Formatting happens outside the
    // lock; the lock only guards the push and the cap.
    const auto stamped = juce::Time::getCurrentTime().formatted ("%H:%M:%S ") + line;

    const juce::ScopedLock sl (lock);
    events.push_back (stamped);

    while (events.size() > 200)
        events.pop_front();
}

juce::StringArray SeparationEngine::eventLog() const
{
    const juce::ScopedLock sl (lock);

    juce::StringArray lines;

    for (const auto& event : events)
        lines.add (event);

    return lines;
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
        snapshot.downloadTotal = 0;

        for (const auto& file : models.missingFor (quality))
            snapshot.downloadTotal += ModelManager::expectedBytes (file.getFileName());

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

namespace
{
    // The PROCESSING setting, mirrored in memory: the settings row reads it on
    // every paint, and Settings.xml is not a file to parse at 30 Hz. Read from
    // disk once, on first use; written through on every change — the same
    // load-once/write-through shape the theme prefs have.
    std::atomic<ComputeBackend>& backendSetting()
    {
        static std::atomic<ComputeBackend> value {
            ModelManager::getUiSetting ("processing", "cpu") == "gpu" ? ComputeBackend::gpu
                                                                     : ComputeBackend::cpu
        };

        return value;
    }
} // namespace

ComputeBackend SeparationEngine::backend()
{
    return backendSetting().load();
}

void SeparationEngine::setBackend (ComputeBackend b)
{
    backendSetting() = b;
    ModelManager::setUiSetting ("processing", b == ComputeBackend::gpu ? "gpu" : "cpu");
}

bool SeparationEngine::gpuAvailable()
{
    return DemucsSeparator::gpuAvailable();
}

bool SeparationEngine::hasCachedResult (const juce::File& source) const
{
    CachedStems entry;
    juce::StringArray stemNames;

    return readCachedEntry (source, models.quality(), entry, stemNames) != nullptr;
}

bool SeparationEngine::tryPublishCached (const juce::File& source, int gen, Quality quality)
{
    // The entry names its own stems, so a cached run is reopened by what it
    // actually holds rather than by whatever mode happens to be selected.
    CachedStems entry;
    juce::StringArray stemNames;

    const auto xml = readCachedEntry (source, quality, entry, stemNames);

    if (xml == nullptr)
        return false;

    const auto numSources = stemNames.size() - 1; // every stem but the instrumental

    mutate (gen, [&] (EngineSnapshot& s)
    {
        s.phase = Phase::Ready;
        s.progress = 1.0f;
        s.message.clear();
        s.sourceFile = source;
        s.sourceDuration = xml->getDoubleAttribute ("duration");
        s.sourceWaveform = metaUncsv (xml->getStringAttribute ("sourceWave"));
        s.stems.clear();

        for (int i = 0; i < stemNames.size(); ++i)
        {
            EngineSnapshot::StemInfo info;
            info.kind = stemKindNamed (stemNames[i]);
            info.name = stemNames[i];
            info.wav = entry.stems[(size_t) i];
            info.waveform = metaUncsv (xml->getStringAttribute ("wave" + stemNames[i]));
            info.peakDb = (float) xml->getDoubleAttribute ("peak" + stemNames[i], -120.0);
            info.fx = StemFxParams::fromDescriptor (
                xml->getStringAttribute (fxAttribute (stemNames[i])));
            attachExistingMidi (info);
            s.stems.push_back (std::move (info));
        }

        // demucs source order (drums, bass, other, vocals[, guitar, piano])
        // for the working table.
        s.sourcePeaksDb.clear();

        for (int src = 0; src < numSources; ++src)
        {
            const auto kind = stemKindForSource (src);
            auto peak = -120.0f;

            for (const auto& stem : s.stems)
                if (stem.kind == kind)
                    peak = stem.peakDb;

            s.sourcePeaksDb.push_back (peak);
        }

        s.peaksKnown = true;
        s.inferenceSeconds = xml->getDoubleAttribute ("inferenceSeconds");
        s.realtimeFactor = xml->getDoubleAttribute ("realtimeFactor");

        // Reopened entries state what ran without flagging a fallback: the
        // request that produced them is long gone (issue #27).
        s.usedGpu = xml->getStringAttribute ("backend") == "gpu";
        s.gpuRequested = s.usedGpu;
        s.bpm = xml->getDoubleAttribute ("bpm");
        s.key = xml->getStringAttribute ("key");

        attachExistingClean (s, *xml);
    });

    return true;
}

bool SeparationEngine::openCachedEntry (const juce::File& cacheDir)
{
    // Deliberately not tryPublishCached: that re-derives the cache key from the
    // source file's current size/mtime, so an entry whose source has moved or
    // been deleted would never open. The meta holds everything we need.
    auto xml = juce::parseXML (cacheDir.getChildFile ("meta.xml"));

    if (xml == nullptr || xml->getIntAttribute ("version") < 3)
        return false;

    // v4 entries record their own stem set; v3 predates variable stem counts
    // and always held the five-stem layout.
    auto stemNames = juce::StringArray::fromTokens (xml->getStringAttribute ("stems"), ",", {});

    if (stemNames.isEmpty())
        stemNames = { "Vocals", "Drums", "Bass", "Other", "Instrumental" };

    const auto entry = StemCache::entryIn (cacheDir, stemNames, stemFilesFrom (*xml));

    auto found = 0;
    for (const auto& wav : entry.stems)
        if (wav.existsAsFile())
            ++found;

    if (found == 0)
        return false; // stems gone: nothing to reopen

    logEvent ("library open: " + cacheDir.getFileName());

    const auto numSources = stemNames.size() - 1; // every stem but the instrumental
    const auto gen = generation.fetch_add (1) + 1;

    cleaner.cancel(); // the stem set is being replaced under it
    pool.removeAllJobs (true, 0);

    mutate (gen, [&] (EngineSnapshot& s)
    {
        s = EngineSnapshot {};
        s.quality = models.quality(); // the engine's setting, not the entry's
        s.phase = Phase::Ready;
        s.progress = 1.0f;

        // Existence is not required — the stems are what the session needs.
        s.sourceFile = juce::File::createFileWithoutCheckingPath (
            xml->getStringAttribute ("sourcePath"));

        s.sourceDuration = xml->getDoubleAttribute ("duration");
        s.sourceWaveform = metaUncsv (xml->getStringAttribute ("sourceWave"));
        s.stems.clear();

        for (int i = 0; i < stemNames.size(); ++i)
        {
            EngineSnapshot::StemInfo info;
            info.kind = stemKindNamed (stemNames[i]);
            info.name = stemNames[i];
            info.wav = entry.stems[(size_t) i];
            info.waveform = metaUncsv (xml->getStringAttribute ("wave" + stemNames[i]));
            info.peakDb = (float) xml->getDoubleAttribute ("peak" + stemNames[i], -120.0);
            info.fx = StemFxParams::fromDescriptor (
                xml->getStringAttribute (fxAttribute (stemNames[i])));
            attachExistingMidi (info);
            s.stems.push_back (std::move (info));
        }

        // demucs source order (drums, bass, other, vocals[, guitar, piano])
        // for the working table.
        s.sourcePeaksDb.clear();

        for (int src = 0; src < numSources; ++src)
        {
            const auto kind = stemKindForSource (src);
            auto peak = -120.0f;

            for (const auto& stem : s.stems)
                if (stem.kind == kind)
                    peak = stem.peakDb;

            s.sourcePeaksDb.push_back (peak);
        }

        s.peaksKnown = true;
        s.inferenceSeconds = xml->getDoubleAttribute ("inferenceSeconds");
        s.realtimeFactor = xml->getDoubleAttribute ("realtimeFactor");

        // Reopened entries state what ran without flagging a fallback: the
        // request that produced them is long gone (issue #27).
        s.usedGpu = xml->getStringAttribute ("backend") == "gpu";
        s.gpuRequested = s.usedGpu;
        s.bpm = xml->getDoubleAttribute ("bpm");
        s.key = xml->getStringAttribute ("key");

        attachExistingClean (s, *xml);
    });

    return true;
}

void SeparationEngine::requestSeparation (const juce::File& file)
{
    const auto gen = generation.fetch_add (1) + 1;
    const auto quality = models.quality();

    cleaner.cancel(); // the vocals it was cleaning are about to be replaced
    pool.removeAllJobs (true, 0);

    if (tryPublishCached (file, gen, quality))
    {
        logEvent ("cache hit: " + file.getFileName());
        return;
    }

    logEvent ("scan requested: " + file.getFileName());

    {
        const juce::ScopedLock sl (lock);
        snapshot.phase = Phase::Decoding;
        snapshot.progress = 0.0f;
        snapshot.message.clear();
        snapshot.sourceFile = file;
        snapshot.peaksKnown = false;
        snapshot.vocalClean = {};
    }

    pool.addJob (new SeparationJob (*this, file, gen, quality), true);
}

void SeparationEngine::requestMidi (int stemIndex, bool force)
{
    juce::File wav;
    auto kind = -1;
    auto bpm = 0.0;
    auto gen = 0;

    {
        const juce::ScopedLock sl (lock);

        if (snapshot.phase != Phase::Ready
            || ! juce::isPositiveAndBelow (stemIndex, (int) snapshot.stems.size()))
            return;

        auto& stem = snapshot.stems[(size_t) stemIndex];

        // Never interrupt one in flight, and only redo a finished one when the
        // caller meant it.
        if (stem.midiState == EngineSnapshot::MidiState::running
            || (stem.midiState == EngineSnapshot::MidiState::ready && ! force))
            return;

        if (! MidiTranscriber::supports (stem.kind) || ! stem.wav.existsAsFile())
            return;

        stem.midiState = EngineSnapshot::MidiState::running;
        stem.midiProgress = 0.0f;

        wav = stem.wav;
        kind = stem.kind;
        bpm = snapshot.bpm;
        gen = generation.load();
    }

    // Deliberately no generation bump and no removeAllJobs: transcription adds
    // to the session rather than replacing it, so it queues behind whatever is
    // already running and leaves the stems alone.
    pool.addJob (new MidiJob (*this, gen, stemIndex, wav, kind, bpm), true);
}

void SeparationEngine::setStemFx (int stemIndex, const StemFxParams& fx, bool commit)
{
    juce::File wav;
    juce::String name;

    {
        const juce::ScopedLock sl (lock);

        if (snapshot.phase != Phase::Ready
            || ! juce::isPositiveAndBelow (stemIndex, (int) snapshot.stems.size()))
            return;

        auto& stem = snapshot.stems[(size_t) stemIndex];

        // The snapshot always follows the sliders — that is what the audition
        // is fed from, and it has to keep up with the drag.
        stem.fx = fx;

        if (! commit)
            return;

        wav = stem.wav;
        name = stem.name;
    }

    // Disk work stays outside the lock: a commit is a mouse-up, but the UI is
    // still reading the snapshot at 30 Hz behind it.
    const auto meta = wav.getParentDirectory().getChildFile ("meta.xml");
    const auto descriptor = fx.toDescriptor();

    juce::StringPairArray values;
    values.set (fxAttribute (name), descriptor);

    if (descriptor.isEmpty())
    {
        // Back to flat: the entry should carry nothing at all, so the bounce
        // and the attribute that describes it both go.
        tunedFileFor (wav).deleteFile();
        values.set (fxBouncedAttribute (name), {});
    }

    const juce::ScopedLock ml (metaLock);
    writeMetaAttributes (meta, values);
}

juce::File SeparationEngine::processedStemFile (int stemIndex)
{
    juce::File wav, clean;
    juce::String name, cleanTag;
    StemFxParams fx;

    {
        const juce::ScopedLock sl (lock);

        if (snapshot.phase != Phase::Ready
            || ! juce::isPositiveAndBelow (stemIndex, (int) snapshot.stems.size()))
            return {};

        const auto& stem = snapshot.stems[(size_t) stemIndex];
        wav = stem.wav;
        name = stem.name;
        fx = stem.fx;

        // An active clean render is the vocals' source from here on: it is
        // what leaves the plugin flat, and what the bounce renders from
        // (issue #29).
        if (stem.kind == stemVocals
            && snapshot.vocalClean.state == EngineSnapshot::VocalClean::State::done)
        {
            clean = snapshot.vocalClean.file;
            cleanTag = snapshot.vocalClean.params.cacheTag();
        }
    }

    const auto source = clean.existsAsFile() ? clean : wav;

    if (fx.isFlat() || ! source.existsAsFile())
        return source; // an untuned stem leaves the plugin as its source render

    const auto meta = wav.getParentDirectory().getChildFile ("meta.xml");
    const auto tuned = tunedFileFor (wav);

    // The staleness key carries the source's identity beside the fx: a bounce
    // is only good for the params it was rendered from AND the file it was
    // rendered from, so activating or reverting a clean render invalidates it
    // exactly like a moved slider (issue #29). The value is opaque — compared
    // here, never parsed — so widening it costs old entries nothing.
    const auto bounceKey = clean.existsAsFile()
                               ? fx.toDescriptor() + "|src:clean-" + cleanTag
                               : fx.toDescriptor();

    // Anything else — including a tuned/ file left over from a run this meta
    // forgot — is rendered again.
    if (tuned.existsAsFile())
    {
        const auto xml = juce::parseXML (meta);

        if (xml != nullptr && xml->getStringAttribute (fxBouncedAttribute (name)) == bounceKey)
            return tuned;
    }

    // Rendered here, on the message thread, rather than queued on the pool:
    // the pool is where separations live, a drag cannot wait behind one, and a
    // synchronous bounce has no race with the job that owns the entry. A
    // 4-minute stereo stem is well under a second, so the first drag after a
    // tweak pays a beat and every later one is instant.
    logEvent ("bouncing tuned " + source.getFileName());

    if (! bounceStem (source, tuned, fx))
    {
        logEvent ("bounce failed, handing over the unshaped stem");
        return source; // nothing shaped to hand over: better the source than none
    }

    juce::StringPairArray values;
    values.set (fxBouncedAttribute (name), bounceKey);

    {
        const juce::ScopedLock ml (metaLock);
        writeMetaAttributes (meta, values);
    }

    return tuned;
}

void SeparationEngine::activateVocalClean (int gen, const juce::String& stemName,
                                           const juce::File& stemWav,
                                           const VocalCleanParams& params,
                                           const juce::File& render)
{
    // The choice survives reopen the way the fx do: one attribute on the
    // entry's meta, removed again by revert.
    juce::StringPairArray values;
    values.set (cleanAttribute (stemName), params.toDescriptor());

    {
        const juce::ScopedLock ml (metaLock);
        writeMetaAttributes (stemWav.getParentDirectory().getChildFile ("meta.xml"), values);
    }

    mutate (gen, [&params, &render] (EngineSnapshot& s)
    {
        s.vocalClean = {};
        s.vocalClean.state = EngineSnapshot::VocalClean::State::done;
        s.vocalClean.progress = 1.0f;
        s.vocalClean.params = params;
        s.vocalClean.file = render;
    });
}

void SeparationEngine::requestVocalClean (const VocalCleanParams& params)
{
    using CleanState = EngineSnapshot::VocalClean::State;

    if (params.isNoop())
        return;

    juce::File wav;
    juce::String name;
    EngineSnapshot::VocalClean previous;
    auto gen = 0;

    {
        const juce::ScopedLock sl (lock);

        if (snapshot.phase != Phase::Ready
            || snapshot.vocalClean.state == CleanState::running)
            return;

        // Asking for what is already active is the panel double-firing.
        if (snapshot.vocalClean.state == CleanState::done
            && snapshot.vocalClean.params == params)
            return;

        for (const auto& stem : snapshot.stems)
        {
            if (stem.kind == stemVocals)
            {
                wav = stem.wav;
                name = stem.name;
                break;
            }
        }

        if (! wav.existsAsFile())
            return;

        previous = snapshot.vocalClean;
        gen = generation.load();
    }

    if (! VocalCleaner::isSetUp())
    {
        logEvent ("clean vocal: sidecar not set up");

        mutate (gen, [] (EngineSnapshot& s)
        {
            s.vocalClean = {};
            s.vocalClean.state = CleanState::notSetUp;
        });

        return;
    }

    const auto target = cleanFileFor (wav, params);

    // Identical params with the render still on disk: activating is free, and
    // rerunning a minutes-long chain to reproduce a file we have would be rude.
    if (target.existsAsFile())
    {
        logEvent ("clean cache hit: " + target.getFileName());
        activateVocalClean (gen, name, wav, params, target);
        return;
    }

    // A finished thread can still be winding down behind a done or failed
    // state; clear it out so start() below cannot refuse a legitimate request.
    cleaner.cancel();

    mutate (gen, [&params] (EngineSnapshot& s)
    {
        s.vocalClean = {};
        s.vocalClean.state = CleanState::running;
        s.vocalClean.params = params;
    });

    logEvent ("clean vocal requested: " + params.toDescriptor());

    // The sidecar writes a sibling and the engine moves it into place once
    // the run succeeds, so a dead child can never leave a half render where
    // the cache check above looks. Same extension on the sibling: the python
    // side keys the output format on it.
    const auto part = target.getSiblingFile ("part_" + target.getFileName());

    VocalCleaner::Callbacks cb;

    cb.onStage = [this, gen] (const juce::String& stage, int index, int count)
    {
        logEvent ("clean stage " + juce::String (index) + "/" + juce::String (count)
                  + ": " + stage);
        mutate (gen, [&stage] (EngineSnapshot& s) { s.vocalClean.stage = stage; });
    };

    cb.onProgress = [this, gen] (float p)
    {
        mutate (gen, [p] (EngineSnapshot& s) { s.vocalClean.progress = p; });
    };

    cb.onFinished = [this, gen, wav, name, params, part, target, previous]
                    (VocalCleaner::Result result, juce::String message)
    {
        switch (result)
        {
            case VocalCleaner::Result::ok:
                target.deleteFile();

                if (! part.moveFileTo (target))
                {
                    part.deleteFile();
                    logEvent ("clean vocal failed: could not move the render into place");

                    mutate (gen, [] (EngineSnapshot& s)
                    {
                        s.vocalClean = {};
                        s.vocalClean.state = CleanState::failed;
                        s.vocalClean.error = "COULD NOT WRITE THE RENDER";
                    });

                    return;
                }

                logEvent ("clean vocal done: " + target.getFileName());
                activateVocalClean (gen, name, wav, params, target);
                return;

            case VocalCleaner::Result::cancelled:
                part.deleteFile();
                logEvent ("clean vocal cancelled");

                // A cancel is "never mind", not "revert": whatever render was
                // active before the run comes back.
                mutate (gen, [&previous] (EngineSnapshot& s)
                {
                    s.vocalClean = previous.state == CleanState::done
                                           && previous.file.existsAsFile()
                                       ? previous
                                       : EngineSnapshot::VocalClean {};
                });

                return;

            case VocalCleaner::Result::notSetUp:
                logEvent ("clean vocal: sidecar not set up");

                mutate (gen, [] (EngineSnapshot& s)
                {
                    s.vocalClean = {};
                    s.vocalClean.state = CleanState::notSetUp;
                });

                return;

            case VocalCleaner::Result::failed:
                part.deleteFile();
                logEvent ("clean vocal failed: " + message);

                mutate (gen, [&message] (EngineSnapshot& s)
                {
                    s.vocalClean = {};
                    s.vocalClean.state = CleanState::failed;
                    s.vocalClean.error = message;
                });

                return;
        }
    };

    if (! cleaner.start (wav, part, params, std::move (cb)))
    {
        // Unreachable after the cancel above, in principle; back off honestly
        // rather than leave a running state nothing is behind.
        mutate (gen, [] (EngineSnapshot& s) { s.vocalClean = {}; });
    }
}

void SeparationEngine::cancelVocalClean()
{
    if (! cleaner.isBusy())
        return;

    logEvent ("clean vocal: cancel requested");

    // Blocks only briefly: the kill drops the pipe at once, and onFinished's
    // cancelled path has restored the previous state by the time this returns.
    cleaner.cancel();
}

void SeparationEngine::revertVocalClean()
{
    using CleanState = EngineSnapshot::VocalClean::State;

    juce::File meta;
    juce::String name;

    {
        const juce::ScopedLock sl (lock);

        switch (snapshot.vocalClean.state)
        {
            case CleanState::none:
            case CleanState::running: // that is cancelVocalClean's job
                return;

            // Revert doubles as the panel's dismiss: nothing on disk to undo.
            case CleanState::notSetUp:
            case CleanState::failed:
                snapshot.vocalClean = {};
                return;

            case CleanState::done:
                break;
        }

        for (const auto& stem : snapshot.stems)
        {
            if (stem.kind == stemVocals)
            {
                meta = stem.wav.getParentDirectory().getChildFile ("meta.xml");
                name = stem.name;
                break;
            }
        }

        snapshot.vocalClean = {};
    }

    logEvent ("clean vocal reverted");

    // The attribute goes; the render itself stays in clean/, so re-activating
    // the same params later costs nothing.
    juce::StringPairArray values;
    values.set (cleanAttribute (name), {});

    const juce::ScopedLock ml (metaLock);
    writeMetaAttributes (meta, values);
}

bool SeparationEngine::vocalCleanSetUp()
{
    return VocalCleaner::isSetUp();
}

void SeparationEngine::requestCancel()
{
    logEvent ("cancel requested");

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
    cleaner.cancel();
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
