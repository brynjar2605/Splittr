// M1 harness: proves the separation engine end-to-end before any plugin code.
//
//   splittr_cli <audio-in> <model.bin> <out-dir>
//   splittr_cli --analyze <audio-in> [<audio-in> ...]
//   splittr_cli --midi <stem.wav> <out.mid> [bpm]
//   splittr_cli --clean <in.wav> <out.wav> [--dereverb] [--dereverb-echo]
//               [--restore none|apollo|audiosr]
//
// Separation mode writes every stem the model produces (32-bit float, 44.1 kHz)
// to out-dir: five files for a 4-source model, seven for htdemucs_6s (guitar
// and piano join). Analyse mode prints tempo/key/confidence per file — the
// validation harness for TrackAnalyzer. MIDI mode transcribes one stem and
// then reads the written file back with juce::MidiFile, so what it reports is
// what a DAW would see rather than what the transcriber thinks it wrote.
// Clean mode drives VocalCleaner end-to-end and mirrors the sidecar protocol
// to stdout, so the whole issue #29 seam is testable without a DAW.

#include "engine/AudioFileIO.h"
#include "engine/ComputeBackend.h"
#include "engine/DemucsSeparator.h"
#include "engine/MidiTranscriber.h"
#include "engine/TrackAnalyzer.h"
#include "engine/VocalCleaner.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
    // The stem kind decides the frequency band Basic Pitch is allowed to use,
    // and in the plugin it comes from the snapshot. Here it comes from the
    // filename, which is how the cache names its stems.
    int stemKindFromName (const juce::File& file)
    {
        const auto name = file.getFileNameWithoutExtension();

        for (int kind = 0; kind < splittr::numStemKinds; ++kind)
            if (name.containsIgnoreCase (splittr::stemName (kind)))
                return kind;

        return splittr::stemOther;
    }

    // Scientific pitch naming, i.e. middle C is C4 and A440 is A4 — what a
    // DAW's piano roll shows.
    juce::String noteName (int midiNote)
    {
        return juce::MidiMessage::getMidiNoteName (midiNote, true, true, 4);
    }

    // Parse the .mid back and print what is actually in it: the tempo track,
    // the note count, the pitch span and where the notes sit in time.
    int summariseMidiFile (const juce::File& midi, double expectedBpm)
    {
        juce::FileInputStream stream (midi);

        juce::MidiFile file;

        if (! stream.openedOk() || ! file.readFrom (stream))
        {
            std::cerr << "Could not parse " << midi.getFullPathName() << "\n";
            return 1;
        }

        const auto ticksPerQuarter = (double) file.getTimeFormat();

        if (ticksPerQuarter <= 0.0)
        {
            std::cerr << "SMPTE time format not expected here\n";
            return 1;
        }

        auto tempoBpm = 0.0;
        auto noteCount = 0;
        auto lowest = 127, highest = 0;
        auto firstTick = 0.0, lastTick = 0.0;
        auto velocitySum = 0.0;
        auto durationSum = 0.0;

        // Per-pitch tally, so the summary shows the shape of the transcription
        // and not just its size — a bass line that has quietly sprouted an
        // octave of ghosts is obvious here and invisible in a note count.
        int perPitchCount[128] = {};
        double perPitchTicks[128] = {};

        for (int t = 0; t < file.getNumTracks(); ++t)
        {
            const auto& track = *file.getTrack (t);

            for (int i = 0; i < track.getNumEvents(); ++i)
            {
                const auto& message = track.getEventPointer (i)->message;

                if (message.isTempoMetaEvent())
                    tempoBpm = 60.0 / message.getTempoSecondsPerQuarterNote();

                if (! message.isNoteOn())
                    continue;

                const auto* off = track.getEventPointer (i)->noteOffObject;
                const auto start = message.getTimeStamp();
                const auto end = off != nullptr ? off->message.getTimeStamp() : start;

                if (noteCount == 0)
                    firstTick = start;

                const auto pitch = juce::jlimit (0, 127, message.getNoteNumber());

                lowest = juce::jmin (lowest, pitch);
                highest = juce::jmax (highest, pitch);
                lastTick = juce::jmax (lastTick, end);
                velocitySum += message.getVelocity();
                durationSum += end - start;
                ++perPitchCount[pitch];
                perPitchTicks[pitch] += end - start;
                ++noteCount;
            }
        }

        const auto beatsToSeconds = tempoBpm > 0.0 ? 60.0 / tempoBpm / ticksPerQuarter : 0.0;

        std::cout << "\n"
                  << "  file            " << midi.getFileName() << " ("
                  << midi.getSize() << " bytes)\n"
                  << "  tracks          " << file.getNumTracks()
                  << "   ticks/quarter " << (int) ticksPerQuarter << "\n"
                  << "  tempo event     "
                  << (tempoBpm > 0.0 ? juce::String (tempoBpm, 2) + " BPM"
                                     : juce::String ("MISSING"))
                  << (expectedBpm > 0.0 ? "   (expected " + juce::String (expectedBpm, 2) + ")"
                                        : juce::String())
                  << "\n"
                  << "  notes           " << noteCount << "\n";

        if (noteCount > 0)
        {
            std::cout << "  pitch range     " << noteName (lowest) << " .. " << noteName (highest)
                      << "  (MIDI " << lowest << ".." << highest << ")\n"
                      << "  span            " << juce::String (firstTick * beatsToSeconds, 2)
                      << " s .. " << juce::String (lastTick * beatsToSeconds, 2) << " s\n"
                      << "  mean note       "
                      << juce::String (durationSum / noteCount * beatsToSeconds, 3) << " s"
                      << "   mean velocity " << juce::String (velocitySum / noteCount, 1) << "\n";

            std::cout << "\n  PITCH   NOTE   COUNT   SOUNDING\n";

            for (int pitch = lowest; pitch <= highest; ++pitch)
            {
                if (perPitchCount[pitch] == 0)
                    continue;

                std::cout << "  " << juce::String (pitch).paddedLeft (' ', 5)
                          << juce::String (noteName (pitch)).paddedLeft (' ', 7)
                          << juce::String (perPitchCount[pitch]).paddedLeft (' ', 8)
                          << juce::String (perPitchTicks[pitch] * beatsToSeconds, 2)
                                 .paddedLeft (' ', 10)
                          << " s\n";
            }
        }

        return tempoBpm > 0.0 && noteCount > 0 ? 0 : 1;
    }

    int transcribeStem (const juce::File& stem, const juce::File& outMidi, double bpm)
    {
        if (! stem.existsAsFile())
        {
            std::cerr << "Stem not found: " << stem.getFullPathName() << "\n";
            return 1;
        }

        const auto kind = stemKindFromName (stem);

        std::cout << "Stem: " << stem.getFileName() << "\n"
                  << "Kind: " << splittr::stemName (kind)
                  << (splittr::MidiTranscriber::supports (kind) ? "" : "  (NOT SUPPORTED)") << "\n";

        splittr::MidiTranscription info;
        juce::String error;

        const auto start = juce::Time::getMillisecondCounterHiRes();
        auto lastPercent = -1;

        const auto status = splittr::MidiTranscriber::transcribe (
            stem, outMidi, bpm, kind,
            [&lastPercent] (float p)
            {
                const auto percent = (int) (p * 100.0f) / 10 * 10;

                if (percent != lastPercent)
                {
                    lastPercent = percent;
                    std::cout << "[" << percent << "%]" << std::flush;
                }
            },
            [] { return true; },
            info, error);

        std::cout << "\n";

        if (status != splittr::MidiTranscriber::Status::ok)
        {
            std::cerr << (error.isNotEmpty() ? error : juce::String ("cancelled")) << "\n";
            return 1;
        }

        const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;

        std::cout << "Transcribed in " << juce::String (elapsed, 2) << " s"
                  << "   tempo " << juce::String (info.bpm, 2)
                  << (info.tempoDetected ? " (detected)" : " (fallback, absolute times)") << "\n";

        return summariseMidiFile (outMidi, info.bpm);
    }

    // The plugin's clean-vocal seam (issue #29), driven synchronously: start
    // the cleaner, mirror the protocol lines it parses back onto our own
    // stdout, and let the exit code say how it went.
    int cleanVocal (const juce::File& input, const juce::File& output,
                    const splittr::VocalCleanParams& params)
    {
        if (! input.existsAsFile())
        {
            std::cerr << "Input file not found: " << input.getFullPathName() << "\n";
            return 1;
        }

        if (params.isNoop())
        {
            std::cerr << "Nothing to do: pass --dereverb, --dereverb-echo and/or --restore\n";
            return 1;
        }

        splittr::VocalCleaner::Callbacks cb;

        cb.onStage = [] (const juce::String& stage, int index, int count)
        {
            std::cout << "STAGE " << stage << " " << index << " " << count << "\n" << std::flush;
        };

        cb.onProgress = [] (float p)
        {
            std::cout << "PROGRESS " << (int) std::lround (p * 100.0f) << "\n" << std::flush;
        };

        // Written on the cleaner's thread, read after waitForJob(): the join
        // is the fence.
        auto result = splittr::VocalCleaner::Result::failed;
        juce::String message;

        cb.onFinished = [&result, &message] (splittr::VocalCleaner::Result r, juce::String m)
        {
            result = r;
            message = std::move (m);
        };

        splittr::VocalCleaner cleaner;

        if (! cleaner.start (input, output, params, std::move (cb)))
        {
            std::cerr << "Could not start the cleaner\n";
            return 1;
        }

        cleaner.waitForJob();

        switch (result)
        {
            case splittr::VocalCleaner::Result::ok:
                std::cout << "DONE " << message << "\n";
                return 0;

            case splittr::VocalCleaner::Result::notSetUp:
                std::cerr << "clean-vocal runtime not set up at "
                          << splittr::VocalCleaner::root().getFullPathName()
                          << " — run tools/cleanvocal/setup.sh\n";
                return 1;

            case splittr::VocalCleaner::Result::cancelled:
                std::cerr << "cancelled\n";
                return 1;

            case splittr::VocalCleaner::Result::failed:
                break;
        }

        std::cout << "ERROR " << message << "\n";
        return 1;
    }

    int analyzeFiles (int numFiles, char** paths)
    {
        splittr::AudioFileIO io;
        auto failures = 0;

        for (int i = 0; i < numFiles; ++i)
        {
            const juce::File file (juce::File::getCurrentWorkingDirectory().getChildFile (paths[i]));

            splittr::DecodedAudio decoded;
            juce::String error;

            if (! io.decodeToStereo44k (file, decoded, error))
            {
                std::cerr << error << "\n";
                ++failures;
                continue;
            }

            const auto start = juce::Time::getMillisecondCounterHiRes();
            const auto analysis = splittr::TrackAnalyzer::analyze (decoded.buffer);
            const auto elapsed = juce::Time::getMillisecondCounterHiRes() - start;

            // Machine-readable single line: name<TAB>bpm<TAB>confidence<TAB>key<TAB>ms
            std::cout << file.getFileName() << "\t"
                      << juce::String (analysis.bpm, 2) << "\t"
                      << juce::String (analysis.bpmConfidence, 3) << "\t"
                      << (analysis.key.isNotEmpty() ? analysis.key : juce::String ("-")) << "\t"
                      << juce::String (elapsed, 0) << "\n";
        }

        return failures > 0 ? 1 : 0;
    }
} // namespace

int main (int argc, char** argv)
{
    if (argc >= 3 && juce::String (argv[1]) == "--analyze")
        return analyzeFiles (argc - 2, argv + 2);

    if (argc >= 4 && juce::String (argv[1]) == "--midi")
    {
        const auto cwd = juce::File::getCurrentWorkingDirectory();

        return transcribeStem (cwd.getChildFile (argv[2]), cwd.getChildFile (argv[3]),
                               argc > 4 ? juce::String (argv[4]).getDoubleValue() : 0.0);
    }

    if (argc >= 4 && juce::String (argv[1]) == "--clean")
    {
        const auto cwd = juce::File::getCurrentWorkingDirectory();

        splittr::VocalCleanParams params;

        for (int i = 4; i < argc; ++i)
        {
            const juce::String arg (argv[i]);

            if (arg == "--dereverb")
            {
                params.dereverb = true;
            }
            else if (arg == "--dereverb-echo")
            {
                params.dereverbEcho = true;
            }
            else if (arg == "--restore" && i + 1 < argc)
            {
                const juce::String mode (argv[++i]);

                if (mode == "apollo")
                    params.restore = splittr::VocalRestoreMode::apollo;
                else if (mode == "audiosr")
                    params.restore = splittr::VocalRestoreMode::audiosr;
                else if (mode != "none")
                {
                    std::cerr << "Unknown restore mode: " << mode
                              << " (none|apollo|audiosr)\n";
                    return 1;
                }
            }
            else
            {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }

        return cleanVocal (cwd.getChildFile (argv[2]), cwd.getChildFile (argv[3]), params);
    }

    if (argc != 4)
    {
        std::cerr << "Usage: splittr_cli <audio-in> <model.bin> <out-dir>\n"
                  << "       splittr_cli --analyze <audio-in> [<audio-in> ...]\n"
                  << "       splittr_cli --midi <stem.wav> <out.mid> [bpm]\n"
                  << "       splittr_cli --clean <in.wav> <out.wav> [--dereverb]"
                     " [--dereverb-echo] [--restore none|apollo|audiosr]\n";
        return 1;
    }

    const juce::File inputFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    const juce::File modelFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    const juce::File outDir (juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]));

    if (! inputFile.existsAsFile())
    {
        std::cerr << "Input file not found: " << inputFile.getFullPathName() << "\n";
        return 1;
    }

    if (! modelFile.existsAsFile())
    {
        std::cerr << "Model file not found: " << modelFile.getFullPathName() << "\n";
        return 1;
    }

    splittr::AudioFileIO io;
    splittr::DecodedAudio decoded;
    juce::String error;

    const auto decodeStart = juce::Time::getMillisecondCounterHiRes();

    if (! io.decodeToStereo44k (inputFile, decoded, error))
    {
        std::cerr << error << "\n";
        return 1;
    }

    std::cout << "Decoded: " << decoded.durationSeconds << " s @ "
              << decoded.originalSampleRate << " Hz -> "
              << decoded.buffer.getNumSamples() << " samples @ 44100 Hz ("
              << (juce::Time::getMillisecondCounterHiRes() - decodeStart) << " ms)\n";

    splittr::SeparationResult result;

    // Issue #24: SPLITTR_BACKEND=gpu asks for the Metal (MPSGraph) backend.
    // A gpu request can still legitimately end up on the CPU (old machine,
    // SPLITTR_NO_GPU, mid-run Metal failure), so which backend actually ran
    // is inferred from the progress stream — the Metal driver prefixes every
    // message with "[metal]" and the fallback announcement mentions the CPU —
    // and printed as one machine-checkable line below.
    const auto* backendEnv = std::getenv ("SPLITTR_BACKEND");
    const auto backend = backendEnv != nullptr && juce::String (backendEnv) == "gpu"
                             ? splittr::ComputeBackend::gpu
                             : splittr::ComputeBackend::cpu;

    auto sawMetal = false;
    auto sawFallback = false;

    const auto inferenceStart = juce::Time::getMillisecondCounterHiRes();

    const auto status = splittr::DemucsSeparator::separate (
        { modelFile },
        decoded.buffer,
        [&sawMetal, &sawFallback] (float progress, const juce::String& message)
        {
            sawMetal = sawMetal || message.startsWith ("[metal]");
            sawFallback = sawFallback || message.contains ("taking the CPU path");

            std::cout << "[" << juce::String (progress * 100.0f, 1) << "%] "
                      << message << "\n";
        },
        [] { return true; },
        result,
        error,
        backend);

    if (status != splittr::DemucsSeparator::Status::ok)
    {
        std::cerr << (error.isNotEmpty() ? error : juce::String ("cancelled")) << "\n";
        return 1;
    }

    const auto inferenceSeconds = (juce::Time::getMillisecondCounterHiRes() - inferenceStart) / 1000.0;

    std::cout << "Inference: " << juce::String (inferenceSeconds, 1) << " s ("
              << juce::String (decoded.durationSeconds / inferenceSeconds, 2) << "x realtime)\n";

    std::cout << "Backend: " << (sawMetal && ! sawFallback ? "gpu (Metal)" : "cpu") << "\n";

    std::cout << "Sources: " << result.numSources << "\n";

    // The plugin's tempo flow, mirrored so it is testable from here: the mix
    // first, the separated drums when the mix declines (issue #27).
    {
        auto analysis = splittr::TrackAnalyzer::analyze (decoded.buffer);
        auto fromDrums = false;

        if (analysis.bpm <= 0.0)
        {
            const auto drums = result.indexOfKind (splittr::stemDrums);

            if (drums >= 0)
            {
                analysis.bpm = splittr::TrackAnalyzer::analyze (result.stems[(size_t) drums]).bpm;
                fromDrums = analysis.bpm > 0.0;
            }
        }

        if (analysis.bpm > 0.0)
            std::cout << "BPM: " << juce::String (analysis.bpm, 1)
                      << (fromDrums ? " (drums fallback)" : " (mix)") << "\n";
        else
            std::cout << "BPM: none\n";
    }

    outDir.createDirectory();

    for (size_t i = 0; i < result.stems.size(); ++i)
    {
        const auto name = juce::String (splittr::stemName (result.kinds[i]));
        const auto file = outDir.getChildFile (name + ".wav");

        if (! io.writeWavFloat32 (file, result.stems[i],
                                  splittr::AudioFileIO::kTargetSampleRate, error))
        {
            std::cerr << error << "\n";
            return 1;
        }

        std::cout << "Wrote " << file.getFullPathName() << " (peak "
                  << juce::String (result.peaksDb[i], 1) << " dB)\n";
    }

    return 0;
}
