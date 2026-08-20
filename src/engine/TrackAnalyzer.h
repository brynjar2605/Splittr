#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace splittr
{

struct TrackAnalysis
{
    double bpm = 0.0;            // 0 = undetected
    double bpmConfidence = 0.0;  // 0..1; below the internal threshold bpm is 0
    juce::String key;            // e.g. "F# MIN", "Bb MAJ"; empty = undetected
};

// Lightweight key + tempo estimation, run once per scan on the decoded mix.
//   bpm: multi-band spectral-flux onset envelope -> autocorrelation comb over
//        50-210 BPM -> candidates rescored by pulse-train fit and a
//        log-normal tempo prior (octave choice) -> parabolic refinement
//   key: chromagram correlated against Krumhansl-Kessler major/minor profiles
// The tempo comes with a confidence: material with no steady pulse (rubato,
// free time) reports bpm 0 rather than a number nobody should trust. Key
// still works best on steady tonal music — treat it as a hint.
class TrackAnalyzer
{
public:
    static TrackAnalysis analyze (const juce::AudioBuffer<float>& stereo44k);
};

} // namespace splittr
