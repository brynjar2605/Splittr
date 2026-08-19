#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace splittr
{

struct TrackAnalysis
{
    double bpm = 0.0;  // 0 = undetected
    juce::String key;  // e.g. "F# MIN", "Bb MAJ"; empty = undetected
};

// Lightweight key + tempo estimation, run once per scan on the decoded mix.
//   bpm: onset-envelope autocorrelation (spectral-flux style energy flux)
//   key: chromagram correlated against Krumhansl-Kessler major/minor profiles
// Works best on steady-tempo tonal music; rubato/atonal material may come
// back empty or approximate — treat as a hint, not ground truth.
class TrackAnalyzer
{
public:
    static TrackAnalysis analyze (const juce::AudioBuffer<float>& stereo44k);
};

} // namespace splittr
