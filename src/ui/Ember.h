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

inline Ember emberFor (int col, int row = 0, int seed = 0) noexcept
{
    // Deterministic per-cell hash so no two neighbours are in phase.
    juce::uint32 h = (juce::uint32) col * 73856093u ^ (juce::uint32) row * 19349663u
                     ^ (juce::uint32) seed * 83492791u;
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

// --- burning waveform bars -------------------------------------------------
// One waveform column is far too coarse to read as fire: a 5-9px bar blinking
// as a unit is a blinking bar. `paintEmberBar` dices the bar into the matrix
// bar's grain instead -- ~2px cells with 1px gaps, plus a second sub-column
// when the bar is wide enough for the seam to read -- and hashes an ember per
// sub-cell from (column, sub-column, cell row, `seed`). The gaps leave the
// dimmed base bar showing, so the silhouette stays solid; only the fire is
// subdivided.
//
// This is the burned region's cost and nothing else's: callers must keep it
// behind their `ramp > 0` test so columns ahead of the head stay one fillRect.
//
// `fine == false` restores the original coarse burn (issue #21's before-state,
// kept as a setting): the whole bar is a single ember, no subdivision. The
// matching base-bar dim for each grain lives in fireBaseDim().
inline void paintEmberBar (juce::Graphics& g, juce::Colour colour,
                           juce::Rectangle<float> bar, int col, int seed,
                           double clockSeconds, float ramp, bool fine = true)
{
    if (! fine)
    {
        const auto e = emberFor (col, 0, seed);
        g.setColour (colour.withAlpha (emberAlpha (e, clockSeconds, ramp, 0.0f)));
        g.fillRect (bar);
        return;
    }

    constexpr float cellGap = 1.0f;
    constexpr float cellHeight = 2.0f; // MatrixBar's row height

    // Narrow bars would split into sub-pixel slivers; leave those whole.
    const int subCols = bar.getWidth() > 4.0f ? 2 : 1;
    const auto subWidth = (bar.getWidth() - cellGap * (float) (subCols - 1)) / (float) subCols;

    // Round rather than floor so the cells span the bar exactly: the top and
    // bottom of the silhouette must not creep in as the wave moves.
    const int cellRows = juce::jmax (1, juce::roundToInt ((bar.getHeight() + cellGap)
                                                          / (cellHeight + cellGap)));
    const auto cellH = (bar.getHeight() - cellGap * (float) (cellRows - 1)) / (float) cellRows;

    for (int sub = 0; sub < subCols; ++sub)
    {
        const auto x = bar.getX() + (float) sub * (subWidth + cellGap);

        for (int cell = 0; cell < cellRows; ++cell)
        {
            const auto e = emberFor (col * 2 + sub, cell, seed);

            g.setColour (colour.withAlpha (emberAlpha (e, clockSeconds, ramp, 0.0f)));
            g.fillRect (x, bar.getY() + (float) cell * (cellH + cellGap), subWidth, cellH);
        }
    }
}

// How hard the stem rows dim the base bar under the flame. The fine grain
// covers ~55% of the bar, so the original 75% dim would read as erosion; the
// coarse burn covers all of it and wants the original value. (The working
// view keeps a flat 75% in both grains: green-on-near-dark defines its own
// shape.)
inline float fireBaseDim (bool fine) noexcept
{
    return fine ? 0.45f : 0.75f;
}

} // namespace splittr::ui
