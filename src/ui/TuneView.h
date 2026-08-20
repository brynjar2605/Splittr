#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

#include "Theme.h"
#include "engine/SpectrumTap.h"
#include "engine/StemEqTypes.h"
#include "engine/StemFx.h"

namespace splittr
{

// >>> eqmath-begin (self-contained: <algorithm>, <cmath> and StemEqTypes.h)
//
// The magnitude response of a band list, straight from the RBJ Audio EQ
// Cookbook — the same sections StemFxChain runs, stated independently so the
// two can be nulled against each other. The panel draws its curve from
// `eqBandStages` in StemFx.h (so what is on screen is literally what is in the
// audio); this namespace is the cross-check, and the one place the display's
// axis limits live.
namespace eqmath
{

// One second-order section in cookbook form. a0 is left un-normalised; the
// magnitude divides it out, so this compares to the DSP's a0-normalised
// EqStage without either side having to convert.
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a0 = 1.0, a1 = 0.0, a2 = 0.0;
};

constexpr double twoPi = 6.283185307179586;

constexpr double minFreqHz = 20.0, maxFreqHz = 20000.0;
constexpr double minQ = 0.1, maxQ = 18.0;
constexpr double maxGainDb = 30.0; // EqBand's own range, not the display's

// Matches StemFx.cpp's flatTiltHz / flatTiltQ: the geometric centre of the
// audible band, gentle enough that the hinge is a slope rather than a bump.
constexpr double flatTiltHingeHz = 632.5;
constexpr double flatTiltQ = 0.4;

constexpr int maxStagesPerBand = 8; // 96 dB/oct of 12 dB sections

// StemFx.cpp's safeHz / safeQ / gainA, so a coefficient set built here is the
// one the chain builds down to the last clamp.
inline double safeHz (double hz, double rate)
{
    return std::min (std::max (hz, 20.0), rate * 0.45);
}

inline double safeQ (double q) { return std::min (std::max (q, 0.05), 40.0); }

inline double gainA (double db)
{
    return std::pow (10.0, std::min (std::max (db, -40.0), 40.0) / 40.0);
}

inline int numStages (const EqBand& b)
{
    switch (b.shape)
    {
        case EqShape::lowCut:
        case EqShape::highCut:
            // 12/24/36/48/72/96 are the offered slopes; anything else rounds
            // to the nearest whole 12 dB section rather than being refused.
            return (int) std::min (std::max (std::lround (b.slopeDbOct / 12.0), 1L),
                                   (long) maxStagesPerBand);

        case EqShape::tiltShelf:
        case EqShape::flatTilt:
            return 2;

        case EqShape::bell:
        case EqShape::lowShelf:
        case EqShape::highShelf:
        case EqShape::notch:
        case EqShape::bandPass:
        case EqShape::allPass:
        default:
            return 1;
    }
}

inline Biquad coefficients (const EqBand& band, int stage, double sampleRate)
{
    Biquad c;

    const auto flat = band.shape == EqShape::flatTilt;
    const auto f = safeHz (flat ? flatTiltHingeHz : (double) band.freqHz, sampleRate);

    const auto w0 = twoPi * f / sampleRate;
    const auto cw = std::cos (w0);
    const auto sw = std::sin (w0);
    const auto q = safeQ (flat ? flatTiltQ : (double) band.q);
    const auto alpha = sw / (2.0 * q);

    auto gainDb = (double) band.gainDb;

    // Both tilts are a mirrored shelf pair. tiltShelf hinges at the band's own
    // frequency and swings the full gain each way; flatTilt is pinned to the
    // spectrum's centre and splits the gain, so gainDb reads as the whole
    // swing from 20 Hz to 20 kHz.
    if (band.shape == EqShape::tiltShelf)
        gainDb = stage == 0 ? -gainDb : gainDb;
    else if (flat)
        gainDb = stage == 0 ? -0.5 * gainDb : 0.5 * gainDb;

    const auto A = gainA (gainDb);
    const auto sqA = std::sqrt (A);

    const auto lowShelf = [&]
    {
        c.b0 = A * ((A + 1.0) - (A - 1.0) * cw + 2.0 * sqA * alpha);
        c.b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
        c.b2 = A * ((A + 1.0) - (A - 1.0) * cw - 2.0 * sqA * alpha);
        c.a0 = (A + 1.0) + (A - 1.0) * cw + 2.0 * sqA * alpha;
        c.a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
        c.a2 = (A + 1.0) + (A - 1.0) * cw - 2.0 * sqA * alpha;
    };

    const auto highShelf = [&]
    {
        c.b0 = A * ((A + 1.0) + (A - 1.0) * cw + 2.0 * sqA * alpha);
        c.b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
        c.b2 = A * ((A + 1.0) + (A - 1.0) * cw - 2.0 * sqA * alpha);
        c.a0 = (A + 1.0) - (A - 1.0) * cw + 2.0 * sqA * alpha;
        c.a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
        c.a2 = (A + 1.0) - (A - 1.0) * cw - 2.0 * sqA * alpha;
    };

    const auto resonant = [&] // the shared a-side of cut/notch/band-pass
    {
        c.a0 = 1.0 + alpha;
        c.a1 = -2.0 * cw;
        c.a2 = 1.0 - alpha;
    };

    switch (band.shape)
    {
        case EqShape::bell:
            c.b0 = 1.0 + alpha * A;
            c.b1 = -2.0 * cw;
            c.b2 = 1.0 - alpha * A;
            c.a0 = 1.0 + alpha / A;
            c.a1 = -2.0 * cw;
            c.a2 = 1.0 - alpha / A;
            break;

        case EqShape::lowShelf: lowShelf(); break;
        case EqShape::highShelf: highShelf(); break;

        case EqShape::tiltShelf:
        case EqShape::flatTilt:
            if (stage == 0) lowShelf(); else highShelf();
            break;

        case EqShape::lowCut: // high-pass
            c.b0 = (1.0 + cw) * 0.5;
            c.b1 = -(1.0 + cw);
            c.b2 = (1.0 + cw) * 0.5;
            resonant();
            break;

        case EqShape::highCut: // low-pass
            c.b0 = (1.0 - cw) * 0.5;
            c.b1 = 1.0 - cw;
            c.b2 = (1.0 - cw) * 0.5;
            resonant();
            break;

        case EqShape::notch:
            c.b0 = 1.0;
            c.b1 = -2.0 * cw;
            c.b2 = 1.0;
            resonant();
            break;

        case EqShape::bandPass: // constant 0 dB peak
            c.b0 = alpha;
            c.b1 = 0.0;
            c.b2 = -alpha;
            resonant();
            break;

        case EqShape::allPass: // unity magnitude; only the phase moves
            c.b0 = 1.0 - alpha;
            c.b1 = -2.0 * cw;
            c.b2 = 1.0 + alpha;
            resonant();
            break;

        default:
            break;
    }

    return c;
}

// |H(e^jw)| in dB from precomputed cos/sin of w and 2w, so a whole curve costs
// four trig calls per x rather than four per section per x. Same real/imaginary
// split as StemFx::eqStageMagnitudeDb — an expanded |H|² would be a couple of
// multiplies cheaper and would lose a quarter of a dB in a deep notch's null,
// where the two big terms very nearly cancel.
inline double magnitudeDbAt (const Biquad& c, double cosW, double sinW,
                             double cos2W, double sin2W)
{
    const auto nr = c.b0 + c.b1 * cosW + c.b2 * cos2W;
    const auto ni = -(c.b1 * sinW + c.b2 * sin2W);
    const auto dr = c.a0 + c.a1 * cosW + c.a2 * cos2W;
    const auto di = -(c.a1 * sinW + c.a2 * sin2W);

    return 10.0 * std::log10 (std::max (nr * nr + ni * ni, 1.0e-30)
                              / std::max (dr * dr + di * di, 1.0e-30));
}

inline double magnitudeDb (const Biquad& c, double freqHz, double sampleRate)
{
    const auto w = twoPi * std::min (std::max (freqHz, 1.0), sampleRate * 0.4999) / sampleRate;
    return magnitudeDbAt (c, std::cos (w), std::sin (w), std::cos (2.0 * w), std::sin (2.0 * w));
}

// StemFx.cpp's bandActive: a band that does nothing is not run at unity, it is
// not run at all. It only shows at the margins — a bell parked at 0.02 dB — but
// it is what keeps this a mirror of the chain rather than a second opinion.
inline bool bandActive (const EqBand& b)
{
    constexpr float dbEpsilon = 0.05f;

    if (! b.enabled)
        return false;

    switch (b.shape)
    {
        case EqShape::bell:
        case EqShape::lowShelf:
        case EqShape::highShelf:
            return std::abs (b.gainDb) > dbEpsilon || std::abs (b.dynRangeDb) > dbEpsilon;

        case EqShape::tiltShelf:
        case EqShape::flatTilt:
            return std::abs (b.gainDb) > dbEpsilon;

        case EqShape::lowCut:
        case EqShape::highCut:
        case EqShape::notch:
        case EqShape::bandPass:
        case EqShape::allPass:
        default:
            return true; // shapes with no gain to be neutral at
    }
}

// One band's contribution at freqHz. Bands the chain would not run, and shapes
// that carry no magnitude of their own, contribute nothing.
inline double bandGainDb (const EqBand& band, double freqHz, double sampleRate)
{
    if (! bandActive (band) || band.shape == EqShape::allPass)
        return 0.0;

    auto sum = 0.0;

    for (int s = 0, n = numStages (band); s < n; ++s)
        sum += magnitudeDb (coefficients (band, s, sampleRate), freqHz, sampleRate);

    return sum;
}

inline double sumGainDb (const EqBand* bands, int count, double freqHz, double sampleRate)
{
    auto sum = 0.0;

    for (int i = 0; i < count; ++i)
        sum += bandGainDb (bands[i], freqHz, sampleRate);

    return sum;
}

// The span a band actually acts on, in Hz — what "solo this band" lights up and
// what a dynamic bracket straddles. Bells and notches get their -3 dB skirts
// (RBJ's bandwidth/Q relation); shelves and cuts get the half they own.
inline void bandSpanHz (const EqBand& b, double& loHz, double& hiHz)
{
    const auto octaves = (2.0 / std::log (2.0))
                         * std::asinh (1.0 / (2.0 * std::max (0.05, (double) b.q)));

    const auto f = (double) b.freqHz;

    switch (b.shape)
    {
        case EqShape::bell:
        case EqShape::notch:
        case EqShape::bandPass:
            loHz = f * std::pow (2.0, -0.5 * octaves);
            hiHz = f * std::pow (2.0, 0.5 * octaves);
            break;

        case EqShape::lowShelf:
        case EqShape::highCut:
            loHz = minFreqHz;
            hiHz = f;
            break;

        case EqShape::highShelf:
        case EqShape::lowCut:
            loHz = f;
            hiHz = maxFreqHz;
            break;

        case EqShape::tiltShelf:
        case EqShape::flatTilt:
        case EqShape::allPass:
        default:
            loHz = minFreqHz;
            hiHz = maxFreqHz;
            break;
    }

    loHz = std::max (loHz, minFreqHz);
    hiHz = std::min (hiHz, maxFreqHz);
}

} // namespace eqmath
// <<< eqmath-end

// One analyzer read, pre and post the stem's own chain. Field for field
// AuditionPlayer::SpectrumFrames — the panel mirrors it rather than including
// the audition player, so the two only meet at integration's one-line
// spectrumSource. SpectrumTap already publishes on the display's own log
// layout (512 points, 20 Hz .. 20 kHz), so nothing here resamples anything.
struct SpectrumFrame
{
    SpectrumTap::Frame pre, post;
    bool hasPre = false, hasPost = false;
};

// Per-stem sound shaping (issues #9, #25): an interactive parametric EQ over a
// live spectrum, plus the compressor and output gain the first version shipped
// with. Opened by clicking the mix icon on a stem row.
//
// Same overlay idiom as LibraryView and SettingsView: scrim, centred hairline
// panel, everything drawn in paint() against hit rects rather than built from
// child components, so it follows live theme/accent changes. A click on the
// scrim closes it.
//
// The gesture grammar is the bench's: a press picks something, a drag moves it
// live with commit=false, the release commits once, a double-click resets. The
// two halves of the feature want different things — the audition chain wants
// every intermediate value, the offline bounce only the one the user settled
// on.
//
// The view owns no engine state: it edits a copy of the params it was shown
// with and reports every change through onChange.
class TuneView : public juce::Component,
                 private juce::Timer
{
public:
    TuneView();

    // (stem, params, commit) — commit=false while a gesture is running, true
    // once on release. RESET sends flat params with commit=true.
    std::function<void (int, const StemFxParams&, bool)> onChange;
    std::function<void()> onClose;

    // Workstream B's tap, handed over at integration: fill the frame with the
    // newest analyzer data for the stem being tuned, return false when there
    // is nothing new. Left unset, the panel synthesises a plausible spectrum
    // so the display can be built and reviewed before the tap lands.
    std::function<bool (SpectrumFrame&)> spectrumSource;

    // Point the panel at a stem. The editor makes the view visible.
    void show (int stemIndex, const juce::String& stemName, const StemFxParams&);

    // Which stem the panel is currently editing, or -1 before the first show().
    int currentStem() const { return stem; }

    void paint (juce::Graphics&) override;
    void visibilityChanged() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;

    // --- geometry -------------------------------------------------------
    struct HeaderCells
    {
        std::array<juce::Rectangle<int>, 4> range {}; // ±3 / ±6 / ±12 / ±30
        std::array<juce::Rectangle<int>, 3> speed {}; // fast / med / slow
        juce::Rectangle<int> freeze;
        int speedLabelX = 0;
    };

    juce::Rectangle<int> panelBounds() const;
    juce::Rectangle<int> bodyBounds() const;
    juce::Rectangle<int> controlsRow() const;
    juce::Rectangle<int> gridArea() const;        // plot plus its tick gutters
    juce::Rectangle<int> plotBounds() const;      // the curve/spectrum canvas
    juce::Rectangle<int> stripRow (int param) const;
    juce::Rectangle<int> stripTrack (int param) const;
    juce::Rectangle<int> stripGrab (int param) const;
    juce::Rectangle<int> toggleRow() const;
    juce::Rectangle<int> phaseBounds() const;
    juce::Rectangle<int> autoBounds() const;
    juce::Rectangle<int> footerRow() const;
    HeaderCells headerCells() const;

    float xForFreq (double hz) const;
    double freqForX (float x) const;
    float yForDb (double db) const;
    double dbForY (float y) const;
    float yForSpecDb (double db) const;
    float nodeY (const EqBand&) const;
    juce::Rectangle<int> nodeMenuBounds (int band) const;

    int bandAt (juce::Point<int>) const;   // node under the mouse, or -1
    int stripParamAt (juce::Point<int>) const;
    int cellAtPoint (juce::Point<int>) const;

    // --- painting -------------------------------------------------------
    void paintGrid (juce::Graphics&, float physicalScale);
    void paintSpectrum (juce::Graphics&);
    void paintCurves (juce::Graphics&);
    void paintNodes (juce::Graphics&);
    void paintReadout (juce::Graphics&);
    void paintStrip (juce::Graphics&);
    void paintPopover (juce::Graphics&);

    // --- editing --------------------------------------------------------
    void send (bool commit);
    void rebuildCurve();
    int addBandAt (double hz, double db, bool commit);
    void deleteSelection();
    void setSelection (int band);
    bool isSelected (int band) const;
    void clampBand (EqBand&) const;
    void flash (const juce::String&);
    void openPopover (int band);
    void layoutPopover();
    void closePopover();
    void applySolo (int band, bool on);
    void clearSolo();

    void beginNodeDrag (const juce::MouseEvent&, bool qMode);
    void dragNodes (const juce::MouseEvent&);
    void dragStrip (const juce::MouseEvent&, bool commit);
    void dragPopSlider (const juce::MouseEvent&, bool commit);
    bool popoverDown (const juce::MouseEvent&);
    void popoverUp (const juce::MouseEvent&);

    // --- spectrum -------------------------------------------------------
    bool pullSpectrum (SpectrumFrame&);   // the one workstream B seam
    void synthesiseSpectrum (SpectrumFrame&);
    void advanceSpectrum();
    void applySpectrumSpeed();
    void findPeaks();
    double displayRate() const;

    static constexpr int curvePoints = 256;
    static constexpr int specPoints = SpectrumTap::numPoints;

    int stem = -1;
    juce::String name;
    StemFxParams fx;   // the panel's own copy; the engine hears it via onChange

    // --- selection / gesture state --------------------------------------
    enum class Grab { none, node, nodeQ, rubber, strip, popSlider };

    Grab grab = Grab::none;
    std::vector<int> selection;
    int activeBand = -1;
    int hoverBand = -1;
    int hoverCell = 0;      // header/strip/footer cell ids, 0 = none
    int hoverPopCell = 0;
    int hoverStrip = -1;
    int hoverPopSlider = -1;

    struct Anchor { int index; float freqHz, gainDb, q; };
    std::vector<Anchor> anchors;
    std::vector<int> rubberBase; // what a shift rubber band adds to
    juce::Point<int> pressPoint;
    double pressFreq = 1000.0, pressDb = 0.0;
    bool gestureMoved = false;
    bool pressedInPopover = false, pressedOutside = false;

    int stripParam = -1;
    float stripStart = 0.0f;
    int popSlider = -1;

    juce::Rectangle<int> rubberRect;

    bool popoverOpen = false;
    int popoverBand = -1;
    juce::Rectangle<int> popoverBounds;

    int wheelCommitFrames = 0;   // wheel has no release; commit once it settles
    int noticeFrames = 0;        // "24 BANDS MAX" and friends
    juce::String notice;

    // --- display options ------------------------------------------------
    int rangeIndex = 2;   // ±3 / ±6 / ±12 / ±30
    int speedIndex = 1;   // fast / med / slow
    bool frozen = false;

    // --- spectrum state -------------------------------------------------
    // The ballistics and the tilt are workstream B's, on the consumer's side
    // of the hand-off where they belong: the tap publishes the measurement,
    // the panel decides how fast the picture moves and how far it leans.
    SpectrumFrame frame;
    SpectrumSmoother preSpectrum, postSpectrum;
    std::array<float, specPoints> pointFreq {}, tiltDb {};
    std::array<bool, specPoints> isPeak {};
    bool spectrumPrimed = false;
    int ghostPoint = -1;         // spectrum-grab candidate under the mouse
    double clock = 0.0;

    // --- paint caches ---------------------------------------------------
    juce::Image gridImage;
    float gridScale = 0.0f;
    int gridRange = -1;
    juce::uint32 gridInk = 0;
    juce::Rectangle<int> gridFor;

    std::array<double, curvePoints> cosW {}, sinW {}, cos2W {}, sin2W {}, curveFreq {};
    double cosSampleRate = 0.0;

    std::vector<eqmath::Biquad> stages;      // reused: bands x sections
    std::array<int, (size_t) maxEqBands> stageFirst {}, stageCount {};
    std::array<float, curvePoints> sumCurve {};
    float autoGainOffset = 0.0f;
    juce::Path curvePath, ghostPath, specPath;

    juce::Rectangle<int> closeBounds, resetBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuneView)
};

} // namespace splittr
