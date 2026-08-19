#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <functional>

namespace splittr
{

// htdemucs source order
enum class Source { drums = 0, bass = 1, other = 2, vocals = 3 };

struct SeparationResult
{
    juce::AudioBuffer<float> vocals;       // stereo, 44.1 kHz
    juce::AudioBuffer<float> instrumental; // drums + bass + other, summed
    std::array<float, 4> sourcePeaksDb {}; // drums, bass, other, vocals
};

// Wraps demucs.cpp. This is the ONLY translation unit that touches
// demucs.cpp / Eigen headers — keep it that way (compile firewall).
class DemucsSeparator
{
public:
    enum class Status { ok, cancelled, failed };

    // onProgress: [0,1] + message. shouldContinue: poll for cancellation
    // (checked from the demucs progress callback, i.e. per segment).
    static Status separate (const juce::File& modelFile,
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
