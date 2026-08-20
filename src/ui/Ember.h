#pragma once

#include <juce_graphics/juce_graphics.h>
#include <cmath>

namespace splittr::ui
{

// Ember/flame flicker, shared by the loading matrix bar, the scanning waveform
// in the working view and the played portion of the stem waveforms.
//
// Per cell a deterministic hash yields a base alpha, a flicker period and a
// phase offset, so no two neighbours are ever in step. The flicker itself is
// discrete -- CSS steps(1,end) semantics, four alpha multipliers, no tweening.
// Everything is a pure function of (column, row, clock), so views only need the
// 30 Hz animation clock they already receive; no extra timers, no state.
struct Ember
{
    float baseAlpha = 1.0f; // 0.30 - 1.00
    float period = 1.0f;    // 0.34 - 1.24 s
    float phase = 0.0f;     // 0 - 1
};

inline Ember emberFor (int col, int row = 0) noexcept
{
    // Deterministic per-cell hash so no two neighbours are in phase.
    juce::uint32 h = (juce::uint32) col * 73856093u ^ (juce::uint32) row * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;

    Ember e;
    e.baseAlpha = 0.3f + 0.7f * (float) (h & 0xffu) / 255.0f;
    e.period = 0.34f + 0.90f * (float) ((h >> 8) & 0xffu) / 255.0f;
    e.phase = (float) ((h >> 16) & 0xffu) / 255.0f;
    return e;
}

// Discrete flicker multiplier at `clockSeconds` (steps(1,end), no tweening).
inline float emberFlicker (const Ember& e, double clockSeconds) noexcept
{
    static constexpr float steps[] = { 1.0f, 0.32f, 0.86f, 0.5f };

    const auto t = std::fmod (clockSeconds / (double) e.period + (double) e.phase, 1.0);
    return steps[juce::jlimit (0, 3, (int) (t * 4.0))];
}

// Soft dithered leading edge: 1 well behind the head, 0 well ahead of it,
// ramping across `width` (a fraction of the whole strip) centred on the head.
// `centre01` is the centre of the column, also as a fraction of the strip.
inline float emberRamp (float centre01, float head01, float width = 0.07f) noexcept
{
    return juce::jlimit (0.0f, 1.0f, (head01 + width * 0.5f - centre01) / width);
}

// Alpha for one burning cell. `floorAlpha` keeps unlit cells faintly visible
// (the matrix bar wants 0.13); pass 0 where "ahead of the head" must render
// exactly as it did before.
inline float emberAlpha (const Ember& e, double clockSeconds, float ramp,
                         float floorAlpha = 0.13f) noexcept
{
    return juce::jmax (floorAlpha, e.baseAlpha * emberFlicker (e, clockSeconds) * ramp);
}

} // namespace splittr::ui
