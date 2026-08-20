#pragma once

#include <juce_dsp/juce_dsp.h>

namespace splittr
{

// Per-stem sound shaping (issue #9): the bench-simple strip — high/low pass,
// three-band EQ, one compressor with fixed attack/release, output gain. A
// default-constructed value is flat, i.e. "no processing": flat stems audition
// and leave the plugin as the raw render, byte for byte.
//
// The same params drive two paths: the realtime audition chain (hear while
// tweaking) and the offline bounce that is dragged out or exported. That is
// the point of the feature — what lands in the DAW is what was heard.
struct StemFxParams
{
    // Passes: 0 = off.
    float hpHz = 0.0f; // 20..600
    float lpHz = 0.0f; // 1000..20000

    // 3-band EQ in dB (±12): shelves at fixed corners, the peak tunable.
    float lowDb = 0.0f;   // low shelf @ lowShelfHz
    float midDb = 0.0f;   // peak @ midHz
    float midHz = 800.0f; // 150..6000; only audible when midDb != 0
    float highDb = 0.0f;  // high shelf @ highShelfHz

    // Compressor: threshold 0 = off.
    float thresholdDb = 0.0f; // -48..0
    float ratio = 2.0f;       // 1..8; only used while the comp is on
    float makeupDb = 0.0f;    // 0..12; ditto

    float gainDb = 0.0f; // -24..+12

    static constexpr float lowShelfHz = 120.0f, highShelfHz = 6000.0f;
    static constexpr float attackMs = 8.0f, releaseMs = 120.0f;

    bool isFlat() const;
    bool compOn() const { return thresholdDb < -0.05f; }

    bool operator== (const StemFxParams&) const;
    bool operator!= (const StemFxParams& o) const { return ! (*this == o); }

    // Compact one-attribute form for meta.xml and staleness checks, e.g.
    // "hp:90;low:-2.5;th:-18;ra:2.5;g:-1". Flat serialises to the empty
    // string; anything unparseable reads back as flat.
    juce::String toDescriptor() const;
    static StemFxParams fromDescriptor (const juce::String&);
};

// The params as a running stereo chain. Prepared once for a rate/block size,
// retuned on the fly without allocating on the steady path, so the audition's
// audio thread can follow a slider drag; the bounce reuses it offline at the
// file's own rate.
class StemFxChain
{
public:
    void prepare (double sampleRate, int maxBlockSamples);
    void reset(); // clear filter/envelope state (seek, fresh load)

    // Retune to `p`; cheap no-op when nothing changed. Safe between blocks on
    // the audio thread.
    void setParams (const StemFxParams& p);

    // In-place stereo. A flat chain returns immediately.
    void process (float* left, float* right, int numSamples);

    const StemFxParams& params() const { return current; }

private:
    enum Band { bandHp, bandLp, bandLow, bandMid, bandHigh, numBands };

    juce::dsp::IIR::Filter<float> filter[numBands][2]; // [band][channel]
    bool bandOn[numBands] = {};

    juce::dsp::Compressor<float> comp;
    juce::SmoothedValue<float> outGain; // linear; makeup and gain combined

    StemFxParams current;
    double rate = 44100.0;
    bool prepared = false;
};

} // namespace splittr
