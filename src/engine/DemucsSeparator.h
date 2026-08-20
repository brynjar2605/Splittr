#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>
#include <vector>

#include "StemCache.h" // StemIndex / stemOrderForSources

namespace splittr
{

struct SeparationResult
{
    // One stereo 44.1 kHz buffer per stem, in the display order the model's
    // source count implies (stemOrderForSources): five stems for a 4-source
    // model, seven for htdemucs_6s. The trailing instrumental is the sum of
    // every non-vocal source in both cases.
    std::vector<juce::AudioBuffer<float>> stems;
    std::vector<int> kinds;     // StemIndex per stem, parallel to `stems`
    std::vector<float> peaksDb; // per stem, parallel to `stems`

    int numSources = 0; // 4 or 6: what the model separated

    // Position of a stem kind within `stems`, or -1 when this run has none.
    int indexOfKind (int kind) const
    {
        for (size_t i = 0; i < kinds.size(); ++i)
            if (kinds[i] == kind)
                return (int) i;

        return -1;
    }

    // Peak of a demucs source (drums, bass, other, vocals[, guitar, piano]),
    // for the working-view source table.
    float sourcePeakDb (int source) const
    {
        const auto i = indexOfKind (stemKindForSource (source));
        return i >= 0 ? peaksDb[(size_t) i] : -120.0f;
    }
};

// Wraps demucs.cpp. This is the ONLY translation unit that touches
// demucs.cpp / Eigen headers — keep it that way (compile firewall).
class DemucsSeparator
{
public:
    enum class Status { ok, cancelled, failed };

    // onProgress: [0,1] + message. shouldContinue: poll for cancellation
    // (checked from the demucs progress callback, i.e. per segment).
    //
    // modelFiles: 1 file = plain htdemucs (4-source) or htdemucs_6s; 4 files =
    // the fine-tuned htdemucs_ft ensemble in target order (drums, bass, other,
    // vocals) — each specialist is run over the whole track and contributes
    // only its own source, per upstream demucs_ft. The source count is read
    // from the weights themselves, so the caller does not have to declare it;
    // check `out.numSources` if it matters.
    static Status separate (const std::vector<juce::File>& modelFiles,
                            const juce::AudioBuffer<float>& stereo44k,
                            const std::function<void (float, const juce::String&)>& onProgress,
                            const std::function<bool()>& shouldContinue,
                            SeparationResult& out,
                            juce::String& error);

    // Rough number of progress-callback segments inference will report
    // (7.8 s windows, 25% overlap, 2 shift passes) — for "seg i/j" UI copy.
    static int expectedSegments (int numSamples);
};

} // namespace splittr
