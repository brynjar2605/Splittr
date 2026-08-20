// M1 harness: proves the separation engine end-to-end before any plugin code.
//
//   splittr_cli <audio-in> <model.bin> <out-dir>
//   splittr_cli --analyze <audio-in> [<audio-in> ...]
//
// Separation mode writes every stem the model produces (32-bit float, 44.1 kHz)
// to out-dir: five files for a 4-source model, seven for htdemucs_6s (guitar
// and piano join). Analyse mode prints tempo/key/confidence per file — the
// validation harness for TrackAnalyzer.

#include "engine/AudioFileIO.h"
#include "engine/DemucsSeparator.h"
#include "engine/TrackAnalyzer.h"

#include <iostream>

namespace
{
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

    if (argc != 4)
    {
        std::cerr << "Usage: splittr_cli <audio-in> <model.bin> <out-dir>\n"
                  << "       splittr_cli --analyze <audio-in> [<audio-in> ...]\n";
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

    const auto inferenceStart = juce::Time::getMillisecondCounterHiRes();

    const auto status = splittr::DemucsSeparator::separate (
        { modelFile },
        decoded.buffer,
        [] (float progress, const juce::String& message)
        {
            std::cout << "[" << juce::String (progress * 100.0f, 1) << "%] "
                      << message << "\n";
        },
        [] { return true; },
        result,
        error);

    if (status != splittr::DemucsSeparator::Status::ok)
    {
        std::cerr << (error.isNotEmpty() ? error : juce::String ("cancelled")) << "\n";
        return 1;
    }

    const auto inferenceSeconds = (juce::Time::getMillisecondCounterHiRes() - inferenceStart) / 1000.0;

    std::cout << "Inference: " << juce::String (inferenceSeconds, 1) << " s ("
              << juce::String (decoded.durationSeconds / inferenceSeconds, 2) << "x realtime)\n";

    std::cout << "Sources: " << result.numSources << "\n";

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
