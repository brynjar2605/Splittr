#pragma once

namespace splittr
{

// The shapes the EQ cascade can express (issue #25). Every one of them is a
// biquad (or a cascade of identical biquads, for the cuts), so the whole band
// list renders as one second-order-section chain — zero latency, and the same
// chain runs the audition and the offline bounce.
enum class EqShape
{
    bell,
    lowShelf,
    highShelf,
    lowCut,
    highCut,
    notch,
    bandPass,
    tiltShelf, // hinged at freqHz: gainDb tilts the highs, the lows mirror
    flatTilt,  // gentle full-spectrum tilt, gainDb across 20 Hz .. 20 kHz
    allPass
};

struct EqBand
{
    EqShape shape = EqShape::bell;

    float freqHz = 1000.0f; // 20 .. 20000, edited on a log axis
    float gainDb = 0.0f;    // -30 .. +30 (bell, shelves, tilts)
    float q = 0.7071f;      // 0.1 .. 18, log; cuts use it for resonance
    int slopeDbOct = 12;    // cuts only: 12/24/36/48/72/96 (stages of 12)

    bool enabled = true;
    bool solo = false; // UI-transient: audition the band alone; never serialised

    // Dynamic mode, off while dynRangeDb == 0: the band's gain rides from
    // gainDb toward gainDb + dynRangeDb as the band-passed level crosses
    // dynThresholdDb, followed with attack/release. Static bands cost nothing.
    float dynRangeDb = 0.0f;       // -24 .. +24
    float dynThresholdDb = -30.0f; // -60 .. 0
    float dynAttackMs = 15.0f;     // 1 .. 250
    float dynReleaseMs = 150.0f;   // 10 .. 1000
};

constexpr int maxEqBands = 24;

} // namespace splittr
