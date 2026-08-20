#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>

#include "StemEqTypes.h"

namespace splittr
{

// A cut cascades identical 12 dB/oct sections and 96 dB/oct is the steepest
// slope on offer, so eight sections is the most any one band can ask for.
constexpr int maxEqStagesPerBand = 8;

// The worst case the chain has to be able to run: every band a 96 dB/oct cut.
// Pre-sized once in prepare(), which is what lets a retune stay allocation-free.
constexpr int maxEqStages = maxEqBands * maxEqStagesPerBand; // 192

// One second-order section, normalised to a0 = 1. Doubles because a 20 Hz cut
// at 96 dB/oct parks its poles a whisker from the unit circle, where float
// coefficients move the corner audibly.
struct EqStage
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

// The band list as a fixed-capacity array rather than a std::vector.
//
// The reason is the audition's publish path (issue #9, unchanged here): the
// message thread writes a StemFxParams under a lock, and the audio thread
// copy-assigns it inside a tryLock between blocks. Copy-assigning a vector
// reaches for the heap whenever the capacity does not already match — on the
// audio thread. An inline array of maxEqBands copies as one flat block and can
// never allocate, and 24 bands is the ceiling the UI enforces anyway, so the
// only thing lost is growth nobody wanted.
//
// The accessors are the std::vector subset the rest of the plugin uses, so
// `p.bands[i]`, `for (auto& b : p.bands)`, size/push_back/erase/clear all read
// the way they would against a vector. size() returns int, as everything else
// in this codebase does.
class EqBandList
{
public:
    using iterator = EqBand*;
    using const_iterator = const EqBand*;

    int size() const noexcept { return count; }
    bool empty() const noexcept { return count == 0; }
    bool full() const noexcept { return count >= maxEqBands; }
    static constexpr int capacity() noexcept { return maxEqBands; }

    EqBand& operator[] (int i) noexcept { return items[(size_t) i]; }
    const EqBand& operator[] (int i) const noexcept { return items[(size_t) i]; }

    EqBand& back() noexcept { return items[(size_t) (count - 1)]; }
    const EqBand& back() const noexcept { return items[(size_t) (count - 1)]; }

    iterator begin() noexcept { return items.data(); }
    iterator end() noexcept { return items.data() + count; }
    const_iterator begin() const noexcept { return items.data(); }
    const_iterator end() const noexcept { return items.data() + count; }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }

    // False when the list is full: the caller decides whether that is worth
    // saying out loud, rather than a silent 25th band that never sounds.
    bool push_back (const EqBand& b) noexcept
    {
        if (full())
            return false;

        items[(size_t) count++] = b;
        return true;
    }

    void pop_back() noexcept
    {
        if (count > 0)
            items[(size_t) --count] = EqBand {};
    }

    void erase (int index) noexcept
    {
        if (! juce::isPositiveAndBelow (index, count))
            return;

        for (int i = index; i < count - 1; ++i)
            items[(size_t) i] = items[(size_t) (i + 1)];

        pop_back();
    }

    iterator erase (iterator it) noexcept
    {
        erase ((int) (it - begin()));
        return it;
    }

    void clear() noexcept
    {
        items = {};
        count = 0;
    }

    void resize (int n) noexcept
    {
        n = juce::jlimit (0, maxEqBands, n);

        for (int i = n; i < count; ++i)
            items[(size_t) i] = EqBand {};

        count = n;
    }

    bool operator== (const EqBandList&) const;
    bool operator!= (const EqBandList& o) const { return ! (*this == o); }

private:
    std::array<EqBand, maxEqBands> items {};
    int count = 0;
};

//==============================================================================

// Per-stem sound shaping: up to 24 EQ bands of any shape, one compressor with
// fixed attack/release, phase invert, auto gain, output gain (issues #9, #25).
// A default-constructed value is flat, i.e. "no processing": flat stems
// audition and leave the plugin as the raw render, byte for byte.
//
// The same params drive two paths: the realtime audition chain (hear while
// tweaking) and the offline bounce that is dragged out or exported. That is
// the point of the feature — what lands in the DAW is what was heard.
struct StemFxParams
{
    EqBandList bands;

    // Compressor, unchanged from the v1 strip: threshold 0 = off.
    float thresholdDb = 0.0f; // -48..0
    float ratio = 2.0f;       // 1..8; only used while the comp is on
    float makeupDb = 0.0f;    // 0..12; ditto

    float gainDb = 0.0f;      // -24..+12
    bool phaseInvert = false; // flips polarity, ramped rather than switched
    bool autoGain = false;    // subtract the static curve's average level

    // The v1 strip's fixed corners and its compressor ballistics. The corners
    // survive because that is what a v1 descriptor migrates onto; the
    // ballistics because the compressor itself is unchanged.
    static constexpr float lowShelfHz = 120.0f, highShelfHz = 6000.0f;
    static constexpr float attackMs = 8.0f, releaseMs = 120.0f;

    bool isFlat() const;
    bool compOn() const { return thresholdDb < -0.05f; }

    // Ignores nothing, including the per-band `solo` flag: solo reaches the
    // audio thread through the same publish as everything else, and a compare
    // that skipped it would swallow the toggle (AuditionPlayer::setStemFx
    // drops params equal to the ones already pending).
    bool operator== (const StemFxParams&) const;
    bool operator!= (const StemFxParams& o) const { return ! (*this == o); }

    // Compact one-attribute form for meta.xml and staleness checks. Flat
    // serialises to the empty string; anything unparseable reads back as flat.
    //
    // v2 grammar — tokens separated by ';', fields within a token by ',':
    //
    //   b:bell,<f>,<g>,<q>            bell
    //   b:ls,<f>,<g>,<q>              low shelf     (hs = high shelf)
    //   b:lc<slope>,<f>,<q>           low cut, slope in dB/oct: 12..96
    //                                 (hc<slope> = high cut)
    //   b:no,<f>,<q>                  notch         (bp = band-pass,
    //                                                ap = all-pass)
    //   b:tilt,<f>,<g>,<q>            tilt shelf hinged at f
    //   b:flat,<g>                    fixed full-spectrum tilt
    //   th:<db>  ra:<x>  mk:<db>      compressor    (v1 keys, unchanged)
    //   g:<db>                        output gain   (v1 key, unchanged)
    //   inv:1    ag:1                 phase invert, auto gain
    //
    // Fields past the positional ones are flags, not positions: `off` for a
    // disabled band, `d<range>/<threshold>/<attack>/<release>` for a dynamic
    // one. A missing field takes the EqBand default, so a band at the default
    // Q simply stops writing it — "b:lc24,80". Around 20 bytes a band, so a
    // full 24-band descriptor stays well under half a kilobyte.
    //
    // No version marker, because none is needed: v1's keys (hp/lp/low/mid/
    // mhz/high) are disjoint from v2's, so one reader takes both grammars and
    // every stem tuned under issue #9 reopens as the same bands, sounding
    // identical. Unknown keys and unknown fields are ignored exactly as in v1,
    // so a descriptor from a future build degrades to the part this one knows.
    //
    // Solo is never written: it is a listen mode, not a setting, and a
    // descriptor that carried it would make a perfectly good bounce look stale.
    juce::String toDescriptor() const;
    static StemFxParams fromDescriptor (const juce::String&);
};

//==============================================================================
// The band maths, shared so the curve on screen cannot drift from the curve in
// the audio: the display asks for magnitudes, the chain runs the same sections.

// Fills `out` (room for maxEqStagesPerBand) with the sections that make up one
// band and returns how many were written; 0 for a band that does nothing.
int eqBandStages (const EqBand&, double sampleRate, EqStage* out);

// Field-for-field, floats compared the way the rest of the plugin compares
// them. Includes `solo` — see StemFxParams::operator==.
bool eqBandsEqual (const EqBand&, const EqBand&);

// Magnitude of one section / one band / a whole param set, in dB at `hz`.
// Static curve only: the dynamic ride and the compressor are not in it.
double eqStageMagnitudeDb (const EqStage&, double hz, double sampleRate);
double eqBandMagnitudeDb (const EqBand&, double hz, double sampleRate);
double eqMagnitudeDb (const StemFxParams&, double hz, double sampleRate);

//==============================================================================

// The params as a running stereo chain: a cascade of second-order sections
// derived from the band list, then the compressor, then polarity and gain.
// Prepared once for a rate/block size, retuned on the fly without allocating
// on the steady path, so the audition's audio thread can follow a slider drag;
// the bounce reuses it offline at the file's own rate.
class StemFxChain
{
public:
    void prepare (double sampleRate, int maxBlockSamples);
    void reset(); // clear filter/envelope state (seek, fresh load)

    // Retune to `p`; cheap no-op when nothing changed. Safe between blocks on
    // the audio thread: every buffer it writes into was sized by prepare().
    void setParams (const StemFxParams& p);

    // In-place stereo. A flat chain returns immediately.
    void process (float* left, float* right, int numSamples);

    const StemFxParams& params() const { return current; }

    // Follow the `solo` flags on the bands themselves — how the UI's solo can
    // reach the audio thread on the params publish that already exists,
    // instead of needing a second route to it.
    static constexpr int soloFromParams = -2;

    // Listen to one band's region alone, the rest of the chain out of the way:
    // an index, soloFromParams, or -1 for none.
    //
    // Off by default, and deliberately: the bounce builds its chain from the
    // same params the audition is running, so a chain that soloed on its own
    // say-so would render a listen into the file that leaves the plugin. Only
    // a chain that was *told* to listen ever does.
    void setSoloBand (int index);
    int soloBand() const { return soloActive ? soloIndex : -1; }

private:
    // Transposed direct form II, one instance per section per channel. State is
    // double for the same reason the coefficients are.
    struct Section
    {
        EqStage c;
        double z1[2] {}, z2[2] {};

        void clear() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
        void process (float* data, int channel, int numSamples);
    };

    // What setParams worked out about one active band, so process() never has
    // to look at shapes or gains again.
    struct BandPlan
    {
        int bandIndex = 0;  // into current.bands
        int firstStage = 0; // into `section`
        int numStages = 0;
        EqShape shape = EqShape::bell; // what the sections were built as
        bool dynamic = false;

        // Dynamic mode: a band-passed sidechain off the chain input feeds the
        // gain, which rides between gainDb and gainDb + dynRangeDb.
        EqStage sidechain;
        double scZ1 = 0.0, scZ2 = 0.0;
        double rideDb = 0.0;    // where the ride is now, in dB
        double tunedRideDb = 0.0; // what the sections were last built for
        double attackCoef = 1.0, releaseCoef = 1.0;
    };

    void rebuild();                 // params -> plan + sections
    void rebuildSolo();             // params + solo index -> listen sections
    void updateDynamics (const float* left, const float* right, int numSamples);
    void tuneBand (BandPlan&, float extraGainDb);
    double staticCurveOffsetDb() const; // auto gain: -(mean dB over log f)

    std::array<Section, maxEqStages> section {};
    int activeSections = 0;

    std::array<BandPlan, maxEqBands> plan {};
    int planCount = 0, dynamicCount = 0;
    int controlSamples = 32; // how often a dynamic band may retune

    std::array<Section, maxEqStagesPerBand> soloSection {};
    int soloSections = 0, soloIndex = -1, soloOverride = -1;
    bool soloActive = false;

    // Auto gain integrates over a fixed log-frequency grid; the trig for it
    // only depends on the rate, so prepare() computes it once and setParams
    // does nothing but multiply-add.
    static constexpr int autoGainPoints = 96;
    std::array<double, autoGainPoints> agCosW {}, agSinW {}, agCos2W {}, agSin2W {};

    juce::dsp::Compressor<float> comp;
    juce::SmoothedValue<float> outGain; // linear, signed; makeup, gain, polarity

    StemFxParams current;
    double rate = 44100.0;
    bool prepared = false;
};

} // namespace splittr
