#include "TuneView.h"
#include "Common.h"

namespace splittr
{

namespace
{
    // --- panel geometry ---------------------------------------------------
    // The overlay sits under the 39px header, so the scrim keeps 20px either
    // side and 7px above and below: enough to click through, little enough
    // that the curve gets the room it needs.
    constexpr int panelWidth = 520, panelHeight = 327;
    constexpr int panelPad = 14;
    constexpr int titleHeight = 18, titleGap = 6, controlsGap = 5;
    constexpr int plotGap = 8, stripGap = 8;
    constexpr int dbGutter = 22, freqLabelHeight = 11;
    constexpr int stripHeight = 66;

    // The bottom strip keeps the v1 layout: COMP down the left, OUT down the
    // right, which is the order the chain runs in.
    constexpr int columnGap = 24, sectionHeight = 12, rowHeight = 18;
    constexpr int labelColumn = 52, readoutWidth = 48, innerGap = 6;
    constexpr int grabSlack = 4;
    constexpr float tickWidth = 2.0f, tickHeight = 7.0f;

    constexpr float nodeRadius = 4.0f, nodeHit = 7.5f;
    constexpr int menuChipW = 13, menuChipH = 7;

    // --- popover ----------------------------------------------------------
    constexpr int popWidth = 208, popPad = 9;
    constexpr int popShapeCellW = 38, popSlopeCellW = 31, popCellH = 14;
    constexpr int popLabelH = 11, popRowH = 16, popFootH = 15;
    constexpr int popLabelCol = 40, popReadout = 44, popInner = 5;

    // --- spectrum ---------------------------------------------------------
    // The window the analyzer is drawn in, and how far the picture leans.
    // spectrumTiltDb pivots at 1 kHz, so the lean only decides which way the
    // picture tips, never where it sits.
    constexpr double specFloorDb = -96.0, specCeilDb = 6.0;
    constexpr double specTiltDbPerOct = 4.5;

    // --- hover / hit ids --------------------------------------------------
    enum Cell
    {
        cellNone = 0,
        cellRange = 100, // +0..3
        cellSpeed = 110, // +0..2
        cellFreeze = 120,
        cellPhase,
        cellAuto,
        cellClose,
        cellReset,
        cellNodeMenu,

        popShapeId = 200, // +0..9
        popSlopeId = 220, // +0..5
        popSoloId = 240,
        popBypassId,
        popDeleteId
    };

    // --- sliders (shared by the bottom strip and the popover) -------------
    enum class Fill { fromLeft, fromCentre, fromRight };
    enum class Unit { hz, db, ratio, ms };

    struct Spec
    {
        const char* label;
        float min, max, step;
        bool logScale; // frequencies travel the track logarithmically, dB linearly
        Fill fill;
        bool zeroIsOff; // 0 reads OFF rather than as a value
        Unit unit;
    };

    enum Strip { pThresh, pRatio, pMakeup, pGain, numStrip };

    constexpr Spec stripSpecs[] = {
        { "THRESH", -48.0f, 0.0f,  0.5f, false, Fill::fromRight,  true,  Unit::db },
        { "RATIO",  1.0f,   8.0f,  0.1f, false, Fill::fromLeft,   false, Unit::ratio },
        { "MAKEUP", 0.0f,   12.0f, 0.5f, false, Fill::fromLeft,   false, Unit::db },
        { "GAIN",   -24.0f, 12.0f, 0.5f, false, Fill::fromCentre, false, Unit::db },
    };

    static_assert (sizeof (stripSpecs) / sizeof (stripSpecs[0]) == (size_t) numStrip,
                   "every strip parameter needs a spec");

    enum Dyn { dRange, dThresh, dAttack, dRelease, numDyn };

    constexpr Spec dynSpecs[] = {
        { "RANGE",   -24.0f, 24.0f,   0.5f, false, Fill::fromCentre, true,  Unit::db },
        { "THRESH",  -60.0f, 0.0f,    1.0f, false, Fill::fromRight,  false, Unit::db },
        { "ATTACK",  1.0f,   250.0f,  1.0f, true,  Fill::fromLeft,   false, Unit::ms },
        { "RELEASE", 10.0f,  1000.0f, 1.0f, true,  Fill::fromLeft,   false, Unit::ms },
    };

    static_assert (sizeof (dynSpecs) / sizeof (dynSpecs[0]) == (size_t) numDyn,
                   "every dynamics parameter needs a spec");

    float normOf (const Spec& s, float v)
    {
        v = juce::jlimit (s.min, s.max, v);

        return s.logScale ? std::log (v / s.min) / std::log (s.max / s.min)
                          : (v - s.min) / (s.max - s.min);
    }

    float valueOf (const Spec& s, float n)
    {
        n = juce::jlimit (0.0f, 1.0f, n);

        const auto v = s.logScale ? s.min * std::pow (s.max / s.min, n)
                                  : s.min + n * (s.max - s.min);

        return juce::jlimit (s.min, s.max, std::round (v / s.step) * s.step);
    }

    juce::String readoutFor (const Spec& s, float v)
    {
        if (s.zeroIsOff && std::abs (v) < 0.001f)
            return "OFF";

        switch (s.unit)
        {
            case Unit::hz:
                return v >= 1000.0f ? juce::String (v / 1000.0f, 1) + " kHz"
                                    : juce::String ((int) std::round (v)) + " Hz";

            case Unit::ratio: return juce::String (v, 1) + ":1";
            case Unit::ms:    return juce::String ((int) std::round (v)) + " ms";

            case Unit::db:
            default:
                return juce::String (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB";
        }
    }

    // The one slider look in the plugin: [label][1px track][readout], filled
    // from the parameter's rest position so an untouched slider draws no fill.
    void drawSlider (juce::Graphics& g, const Spec& s, juce::Rectangle<int> row,
                     juce::Rectangle<int> track, float value, bool hot, bool live,
                     int labelW, int readoutW)
    {
        const auto markCol = ! live ? theme::text35
                             : hot  ? theme::accentSoft
                                    : theme::accentMid;

        g.setFont (theme::mono (8.5f, 0.12f));
        g.setColour (hot ? theme::text : (live ? theme::text40 : theme::text35));
        g.drawText (s.label, row.withWidth (labelW), juce::Justification::centredLeft);

        g.setColour (theme::lineSoft15);
        g.fillRect (track);

        const auto x = (float) track.getX() + normOf (s, value) * (float) (track.getWidth() - 1);
        const auto origin = s.fill == Fill::fromLeft    ? (float) track.getX()
                            : s.fill == Fill::fromRight ? (float) track.getRight() - 1.0f
                                                        : (float) track.getCentreX();

        const auto fillL = juce::jmin (origin, x);
        const auto fillR = juce::jmax (origin, x) + 1.0f;

        g.setColour (markCol);
        g.fillRect (fillL, (float) track.getY(), fillR - fillL, (float) track.getHeight());
        g.fillRect (x - tickWidth * 0.5f + 0.5f, (float) track.getCentreY() - tickHeight * 0.5f,
                    tickWidth, tickHeight);

        g.setFont (theme::mono (8.0f));
        g.setColour (! live ? theme::text35 : (hot ? theme::text : theme::text45));
        g.drawText (readoutFor (s, value), row.withTrimmedLeft (row.getWidth() - readoutW),
                    juce::Justification::centredRight);
    }

    int drawTinyLabel (juce::Graphics& g, const juce::String& text, int x, int y, int h,
                       juce::Colour colour = theme::text35)
    {
        const auto font = theme::mono (8.5f, 0.12f);
        const auto w = (int) ui::textWidth (font, text) + 2;

        g.setFont (font);
        g.setColour (colour);
        g.drawText (text, x, y, w, h, juce::Justification::centredLeft);

        return x + w + 8;
    }

    int tinyLabelWidth (const juce::String& text)
    {
        return (int) ui::textWidth (theme::mono (8.5f, 0.12f), text) + 10;
    }

    // --- shapes -----------------------------------------------------------
    bool hasGain (EqShape s)
    {
        return s == EqShape::bell || s == EqShape::lowShelf || s == EqShape::highShelf
               || s == EqShape::tiltShelf || s == EqShape::flatTilt;
    }

    bool isCut (EqShape s) { return s == EqShape::lowCut || s == EqShape::highCut; }

    // Per issue #25 dynamics ride the gain, so only the shapes that have one
    // can be dynamic.
    bool canBeDynamic (EqShape s)
    {
        return s == EqShape::bell || s == EqShape::lowShelf || s == EqShape::highShelf;
    }

    bool isDynamic (const EqBand& b)
    {
        return canBeDynamic (b.shape) && std::abs (b.dynRangeDb) > 0.05f;
    }

    const char* shapeName (EqShape s)
    {
        switch (s)
        {
            case EqShape::bell:      return "bell";
            case EqShape::lowShelf:  return "low shelf";
            case EqShape::highShelf: return "high shelf";
            case EqShape::lowCut:    return "low cut";
            case EqShape::highCut:   return "high cut";
            case EqShape::notch:     return "notch";
            case EqShape::bandPass:  return "band pass";
            case EqShape::tiltShelf: return "tilt";
            case EqShape::flatTilt:  return "flat tilt";
            case EqShape::allPass:   return "all pass";
        }

        return "bell";
    }

    constexpr EqShape shapeOrder[] = {
        EqShape::bell,  EqShape::lowShelf, EqShape::highShelf, EqShape::lowCut,  EqShape::highCut,
        EqShape::notch, EqShape::bandPass, EqShape::tiltShelf, EqShape::flatTilt, EqShape::allPass
    };

    const char* shapeCell (int i)
    {
        static const char* cells[] = { "BELL", "LO SH", "HI SH", "LO CUT", "HI CUT",
                                       "NOTCH", "BAND", "TILT", "FLAT", "APASS" };
        return cells[i];
    }

    constexpr int slopeChoices[] = { 12, 24, 36, 48, 72, 96 };

    juce::String freqCaption (double hz)
    {
        if (hz >= 10000.0) return juce::String (hz / 1000.0, 1) + " kHz";
        if (hz >= 1000.0)  return juce::String (hz / 1000.0, 2) + " kHz";
        return juce::String ((int) std::round (hz)) + " Hz";
    }

    juce::String freqTick (double hz)
    {
        if (hz >= 1000.0) return juce::String ((int) std::round (hz / 1000.0)) + "k";
        return juce::String ((int) std::round (hz));
    }

    constexpr double freqTicks[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };

    constexpr float rangeChoices[] = { 3.0f, 6.0f, 12.0f, 30.0f };
    constexpr float rangeSteps[] = { 1.0f, 2.0f, 3.0f, 10.0f };

    // --- popover layout ---------------------------------------------------
    struct PopLayout
    {
        juce::Rectangle<int> shapeLabel, slopeLabel, dynLabel;
        std::array<juce::Rectangle<int>, 10> shape {};
        std::array<juce::Rectangle<int>, 6> slope {};
        std::array<juce::Rectangle<int>, (size_t) numDyn> row {}, track {};
        juce::Rectangle<int> solo, bypass, del;
        bool hasSlope = false;
    };

    int popoverHeight (bool cut)
    {
        return popPad + popLabelH + 2 * popCellH + (cut ? popLabelH + popCellH : 0)
               + 6 + popLabelH + numDyn * popRowH + 6 + popFootH + popPad;
    }

    PopLayout popLayout (juce::Rectangle<int> bounds, bool cut)
    {
        PopLayout L;
        L.hasSlope = cut;

        auto inner = bounds.reduced (popPad);

        L.shapeLabel = inner.removeFromTop (popLabelH);

        const auto rowA = inner.removeFromTop (popCellH);
        const auto rowB = inner.removeFromTop (popCellH);

        for (int i = 0; i < 5; ++i)
        {
            L.shape[(size_t) i] = { rowA.getX() + i * popShapeCellW, rowA.getY(),
                                    popShapeCellW, popCellH };
            L.shape[(size_t) (i + 5)] = { rowB.getX() + i * popShapeCellW, rowB.getY(),
                                          popShapeCellW, popCellH };
        }

        if (cut)
        {
            L.slopeLabel = inner.removeFromTop (popLabelH);
            const auto rowS = inner.removeFromTop (popCellH);

            for (int i = 0; i < 6; ++i)
                L.slope[(size_t) i] = { rowS.getX() + i * popSlopeCellW, rowS.getY(),
                                        popSlopeCellW, popCellH };
        }

        inner.removeFromTop (6);
        L.dynLabel = inner.removeFromTop (popLabelH);

        for (int i = 0; i < numDyn; ++i)
        {
            const auto r = inner.removeFromTop (popRowH);
            L.row[(size_t) i] = r;
            L.track[(size_t) i] = { r.getX() + popLabelCol + popInner, r.getCentreY(),
                                    r.getWidth() - popLabelCol - popReadout - 2 * popInner, 1 };
        }

        inner.removeFromTop (6);
        auto foot = inner.removeFromTop (popFootH);

        L.solo = foot.removeFromLeft (58);
        foot.removeFromLeft (8);
        L.bypass = foot.removeFromLeft (62);
        foot.removeFromLeft (8);
        L.del = foot.removeFromLeft (54);

        return L;
    }

    float dynValue (const EqBand& b, int id)
    {
        switch (id)
        {
            case dRange:   return b.dynRangeDb;
            case dThresh:  return b.dynThresholdDb;
            case dAttack:  return b.dynAttackMs;
            case dRelease: return b.dynReleaseMs;
            default:       return 0.0f;
        }
    }

    void setDynValue (EqBand& b, int id, float v)
    {
        switch (id)
        {
            case dRange:   b.dynRangeDb = v; break;
            case dThresh:  b.dynThresholdDb = v; break;
            case dAttack:  b.dynAttackMs = v; break;
            case dRelease: b.dynReleaseMs = v; break;
            default: break;
        }
    }

    float stripValue (const StemFxParams& s, int id)
    {
        switch (id)
        {
            case pThresh: return s.thresholdDb;
            case pRatio:  return s.ratio;
            case pMakeup: return s.makeupDb;
            case pGain:   return s.gainDb;
            default:      return 0.0f;
        }
    }

    void setStripValue (StemFxParams& s, int id, float v)
    {
        switch (id)
        {
            case pThresh: s.thresholdDb = v; break;
            case pRatio:  s.ratio = v; break;
            case pMakeup: s.makeupDb = v; break;
            case pGain:   s.gainDb = v; break;
            default: break;
        }
    }

    bool stripLive (const StemFxParams& s, int id)
    {
        return (id != pRatio && id != pMakeup) || s.compOn();
    }
} // namespace

// ===========================================================================
// lifecycle
// ===========================================================================

TuneView::TuneView()
{
    setWantsKeyboardFocus (true);
    stages.reserve ((size_t) maxEqBands * (size_t) eqmath::maxStagesPerBand);

    // SpectrumTap's layout is a pure function of its own constants, so the
    // panel's x mapping and its tilt table are built once, here, and never
    // again — not per frame, and not when the host changes rate.
    for (int i = 0; i < specPoints; ++i)
    {
        pointFreq[(size_t) i] = SpectrumTap::pointFrequency (i);
        tiltDb[(size_t) i] = spectrumTiltDb (pointFreq[(size_t) i], (float) specTiltDbPerOct);
    }

    preSpectrum.setTickRate (30.0);
    postSpectrum.setTickRate (30.0);
    applySpectrumSpeed();
}

void TuneView::show (int stemIndex, const juce::String& stemName, const StemFxParams& params)
{
    stem = stemIndex;
    name = stemName;
    fx = params;

    grab = Grab::none;
    selection.clear();
    activeBand = fx.bands.size() > 0 ? 0 : -1;
    hoverBand = -1;
    hoverCell = cellNone;
    hoverPopCell = cellNone;
    hoverStrip = -1;
    hoverPopSlider = -1;
    stripParam = -1;
    popSlider = -1;
    ghostPoint = -1;
    notice = {};
    noticeFrames = 0;
    wheelCommitFrames = 0;
    closePopover();

    if (activeBand >= 0)
        selection.push_back (activeBand);

    // A fresh stem must not slide down from the last one's picture.
    spectrumPrimed = false;
    preSpectrum.reset();
    postSpectrum.reset();
    advanceSpectrum();
    repaint();
}

void TuneView::visibilityChanged()
{
    if (isVisible())
    {
        startTimerHz (30);

        if (isShowing())
            grabKeyboardFocus();
    }
    else
    {
        stopTimer();

        // The panel can be taken away without going through CLOSE — the editor
        // drops it when the phase leaves Ready. Solo is a listen mode, so it
        // must not survive that, and a wheel gesture still counting down owes
        // the bounce its commit.
        if (wheelCommitFrames > 0)
        {
            wheelCommitFrames = 0;
            send (true);
        }

        clearSolo();

        grab = Grab::none;
        closePopover();
    }
}

void TuneView::timerCallback()
{
    clock += 1.0 / 30.0;

    advanceSpectrum();

    if (wheelCommitFrames > 0 && --wheelCommitFrames == 0)
        send (true); // the wheel has no release, so a settled wheel commits

    if (noticeFrames > 0)
        --noticeFrames;

    repaint (panelBounds());
}

void TuneView::send (bool commit)
{
    if (onChange != nullptr)
        onChange (stem, fx, commit);
}

// ===========================================================================
// geometry
// ===========================================================================

juce::Rectangle<int> TuneView::panelBounds() const
{
    return juce::Rectangle<int> (panelWidth, panelHeight).withCentre (getLocalBounds().getCentre());
}

juce::Rectangle<int> TuneView::bodyBounds() const
{
    return panelBounds().reduced (panelPad);
}

juce::Rectangle<int> TuneView::footerRow() const
{
    return bodyBounds().removeFromBottom (ui::TagButton::idealHeight);
}

juce::Rectangle<int> TuneView::controlsRow() const
{
    const auto body = bodyBounds();
    return { body.getX(), body.getY() + titleHeight + titleGap,
             body.getWidth(), ui::miniCellHeight };
}

// The strip's top edge — everything above it belongs to the display.
static int stripTopFor (juce::Rectangle<int> body)
{
    return body.getBottom() - ui::TagButton::idealHeight - stripGap - stripHeight;
}

juce::Rectangle<int> TuneView::gridArea() const
{
    const auto body = bodyBounds();
    const auto top = controlsRow().getBottom() + controlsGap;
    return { body.getX(), top, body.getWidth(), stripTopFor (body) - plotGap - top };
}

juce::Rectangle<int> TuneView::plotBounds() const
{
    auto a = gridArea();
    a.removeFromBottom (freqLabelHeight);
    a.removeFromLeft (dbGutter);
    return a;
}

juce::Rectangle<int> TuneView::stripRow (int param) const
{
    const auto body = bodyBounds();
    const auto colW = (body.getWidth() - columnGap) / 2;
    const auto top = stripTopFor (body) + sectionHeight;

    if (param == pGain)
        return { body.getX() + colW + columnGap, top, colW, rowHeight };

    return { body.getX(), top + param * rowHeight, colW, rowHeight };
}

juce::Rectangle<int> TuneView::stripTrack (int param) const
{
    const auto row = stripRow (param);

    return { row.getX() + labelColumn + innerGap, row.getCentreY(),
             row.getWidth() - labelColumn - readoutWidth - 2 * innerGap, 1 };
}

juce::Rectangle<int> TuneView::stripGrab (int param) const
{
    const auto row = stripRow (param);
    const auto track = stripTrack (param);

    return { track.getX() - grabSlack, row.getY(),
             track.getWidth() + 2 * grabSlack, row.getHeight() };
}

juce::Rectangle<int> TuneView::toggleRow() const
{
    const auto gain = stripRow (pGain);
    return gain.translated (0, rowHeight + 6);
}

float TuneView::xForFreq (double hz) const
{
    const auto plot = plotBounds();
    const auto n = std::log (juce::jlimit (eqmath::minFreqHz, eqmath::maxFreqHz, hz)
                             / eqmath::minFreqHz)
                   / std::log (eqmath::maxFreqHz / eqmath::minFreqHz);

    return (float) plot.getX() + (float) n * (float) plot.getWidth();
}

double TuneView::freqForX (float x) const
{
    const auto plot = plotBounds();
    const auto n = juce::jlimit (0.0, 1.0,
                                 ((double) x - (double) plot.getX()) / (double) plot.getWidth());

    return eqmath::minFreqHz * std::pow (eqmath::maxFreqHz / eqmath::minFreqHz, n);
}

float TuneView::yForDb (double db) const
{
    const auto plot = plotBounds();
    const auto range = (double) rangeChoices[rangeIndex];

    return (float) plot.getCentreY()
           - (float) (juce::jlimit (-range, range, db) / range) * (float) plot.getHeight() * 0.5f;
}

double TuneView::dbForY (float y) const
{
    const auto plot = plotBounds();
    const auto range = (double) rangeChoices[rangeIndex];

    return juce::jlimit (-range, range,
                         ((double) plot.getCentreY() - (double) y)
                             / ((double) plot.getHeight() * 0.5) * range);
}

float TuneView::yForSpecDb (double db) const
{
    const auto plot = plotBounds();
    const auto n = juce::jlimit (0.0, 1.0, (db - specFloorDb) / (specCeilDb - specFloorDb));

    return (float) plot.getBottom() - (float) n * (float) plot.getHeight();
}

float TuneView::nodeY (const EqBand& b) const
{
    const auto plot = plotBounds();
    const auto db = hasGain (b.shape) ? (double) b.gainDb : 0.0;
    const auto y = yForDb (db);

    // A band parked outside the current dB range still has to be grabbable —
    // and it gets a little more room than one merely sitting at the top of the
    // range, so the caret saying "it goes on past here" has somewhere to go.
    const auto inset = std::abs (db) > (double) rangeChoices[rangeIndex] + 0.05 ? 10.0f : 3.0f;

    return juce::jlimit ((float) plot.getY() + inset, (float) plot.getBottom() - inset, y);
}

juce::Rectangle<int> TuneView::nodeMenuBounds (int band) const
{
    if (! juce::isPositiveAndBelow (band, fx.bands.size()))
        return {};

    const auto& b = fx.bands[band];
    const auto x = (int) std::round (xForFreq (b.freqHz));
    const auto y = (int) std::round (nodeY (b)) + (int) nodeRadius + 4;
    const auto plot = plotBounds();

    return juce::Rectangle<int> (x - menuChipW / 2, juce::jmin (y, plot.getBottom() - menuChipH - 1),
                                 menuChipW, menuChipH);
}

int TuneView::bandAt (juce::Point<int> p) const
{
    auto best = -1;
    auto bestDist = nodeHit * nodeHit;

    for (int i = fx.bands.size(); --i >= 0;)
    {
        const auto& b = fx.bands[i];
        const auto dx = xForFreq (b.freqHz) - (float) p.x;
        const auto dy = nodeY (b) - (float) p.y;
        const auto d = dx * dx + dy * dy;

        if (d <= bestDist)
        {
            bestDist = d;
            best = i;
        }
    }

    return best;
}

int TuneView::stripParamAt (juce::Point<int> p) const
{
    for (int id = 0; id < numStrip; ++id)
        if (stripGrab (id).contains (p))
            return id;

    return -1;
}

TuneView::HeaderCells TuneView::headerCells() const
{
    HeaderCells h;

    const auto row = controlsRow();
    const auto rangeLabel = juce::String ("RANGE ") + juce::String::fromUTF8 ("\xc2\xb1");
    auto x = row.getX() + tinyLabelWidth (rangeLabel);

    for (int i = 0; i < 4; ++i)
        h.range[(size_t) i] = { x + i * 26, row.getY(), 26, ui::miniCellHeight };

    const auto freezeW = 48;
    auto right = row.getRight() - freezeW;
    h.freeze = { right, row.getY(), freezeW, ui::miniCellHeight };

    right -= 6 + 3 * 32;

    for (int i = 0; i < 3; ++i)
        h.speed[(size_t) i] = { right + i * 32, row.getY(), 32, ui::miniCellHeight };

    h.speedLabelX = right - tinyLabelWidth ("ANALYZER");

    return h;
}

int TuneView::cellAtPoint (juce::Point<int> p) const
{
    const auto h = headerCells();

    for (int i = 0; i < 4; ++i)
        if (h.range[(size_t) i].contains (p))
            return cellRange + i;

    for (int i = 0; i < 3; ++i)
        if (h.speed[(size_t) i].contains (p))
            return cellSpeed + i;

    if (h.freeze.contains (p)) return cellFreeze;
    if (phaseBounds().contains (p)) return cellPhase;
    if (autoBounds().contains (p)) return cellAuto;
    if (! closeBounds.isEmpty() && closeBounds.contains (p)) return cellClose;
    if (! resetBounds.isEmpty() && resetBounds.contains (p)) return cellReset;

    if (activeBand >= 0 && nodeMenuBounds (activeBand).contains (p))
        return cellNodeMenu;

    return cellNone;
}

juce::Rectangle<int> TuneView::phaseBounds() const
{
    const auto row = toggleRow();
    return { row.getX(), row.getY() + (rowHeight - ui::miniCellHeight) / 2, 52, ui::miniCellHeight };
}

juce::Rectangle<int> TuneView::autoBounds() const
{
    const auto p = phaseBounds();
    return p.withX (p.getRight() + 8).withWidth (72);
}

// ===========================================================================
// spectrum
// ===========================================================================

// --- workstream B seam ------------------------------------------------------
// The one call integration replaces, and it replaces it with one line:
//
//   tuneView.spectrumSource = [&audition] (splittr::SpectrumFrame& f)
//   {
//       splittr::AuditionPlayer::SpectrumFrames s;
//       if (! audition.readSpectrum (s))
//           return false;
//       f.pre = s.pre; f.post = s.post;
//       f.hasPre = s.hasPre; f.hasPost = s.hasPost;
//       return true;
//   };
//
// Left unset, the panel synthesises a plausible frame on exactly SpectrumTap's
// layout, so every consumer below this line — the ballistics, the tilt, the
// peak finder, Spectrum Grab, the pixel mapping — is the code that will run
// against the real tap.
bool TuneView::pullSpectrum (SpectrumFrame& out)
{
    out.hasPre = out.hasPost = false;

    if (spectrumSource != nullptr)
        return spectrumSource (out);

    synthesiseSpectrum (out);
    return true;
}

void TuneView::synthesiseSpectrum (SpectrumFrame& out)
{
    out.pre.sampleRate = 48000.0;
    out.pre.framesPerSecond = out.pre.sampleRate / (double) SpectrumTap::hopSize;
    out.pre.sequence++;
    out.post.sampleRate = out.pre.sampleRate;
    out.post.framesPerSecond = out.pre.framesPerSecond;
    out.post.sequence = out.pre.sequence;
    out.hasPre = out.hasPost = true;

    const auto bump = [] (double f, double centre, double widthOct)
    {
        const auto d = std::log2 (f / centre) / widthOct;
        return std::exp (-d * d);
    };

    const auto frameSeed = (juce::uint32) (juce::int64) (clock * 30.0);

    for (int i = 0; i < specPoints; ++i)
    {
        const auto f = (double) pointFreq[(size_t) i];

        // A stem's worth of pink-ish material, around -14 dBFS overall: falls
        // gently to a shoulder, steeply above it, with the resonances a mixed
        // source actually has.
        auto db = f <= 900.0 ? -34.0 - 3.0 * std::log2 (f / 50.0)
                             : -34.0 - 3.0 * std::log2 (18.0) - 7.5 * std::log2 (f / 900.0);

        db += 8.0 * bump (f, 55.0, 0.35) + 5.0 * bump (f, 112.0, 0.40)
              + 4.5 * bump (f, 430.0, 0.50) + 3.0 * bump (f, 1350.0, 0.60);

        if (f < 30.0)
            db -= 22.0 * std::log2 (30.0 / f);

        db += 2.4 * std::sin (clock * 1.7 + std::log2 (f) * 2.1)
              + 1.7 * std::sin (clock * 0.9 - std::log2 (f) * 3.3);

        auto h = (juce::uint32) i * 2654435761u ^ frameSeed * 2246822519u;
        h ^= h >> 13;
        h *= 3266489917u;
        h ^= h >> 16;

        db += (double) h / 4294967296.0 * 5.0 - 2.5;

        // The post tap measures the curve the panel is already drawing, so the
        // synthetic one reads it off sumCurve rather than paying for the maths
        // a second time.
        const auto c = sumCurve[(size_t) juce::jlimit (0, curvePoints - 1,
                                                       juce::roundToInt ((double) i
                                                                         * (curvePoints - 1)
                                                                         / (specPoints - 1)))];

        out.pre.db[(size_t) i] = (float) db;
        out.post.db[(size_t) i] = (float) db + c;
    }
}

void TuneView::applySpectrumSpeed()
{
    const SpectrumSmoother::Speed speeds[] = { SpectrumSmoother::Speed::fast,
                                               SpectrumSmoother::Speed::medium,
                                               SpectrumSmoother::Speed::slow };

    preSpectrum.setSpeed (speeds[speedIndex]);
    postSpectrum.setSpeed (speeds[speedIndex]);
}

void TuneView::advanceSpectrum()
{
    if (frozen)
        return; // freeze holds the picture, ballistics and all

    const auto got = pullSpectrum (frame);

    preSpectrum.update (got && frame.hasPre ? &frame.pre : nullptr);
    postSpectrum.update (got && frame.hasPost ? &frame.post : nullptr);

    if (got && (frame.hasPre || frame.hasPost))
        spectrumPrimed = true;

    findPeaks();
}

// A point is a peak when it tops its neighbourhood and stands clear of the
// local floor — the candidates Spectrum Grab offers a node for. The
// neighbourhood is in points, and SpectrumTap's layout is ~51 points to the
// octave, so `near` is a sixth of an octave either side and `wide` is most of
// one.
void TuneView::findPeaks()
{
    constexpr int near = 8, wide = 34;
    constexpr float prominenceDb = 6.0f;

    const auto& level = postSpectrum.level();

    for (int i = 0; i < specPoints; ++i)
    {
        auto peak = i >= near && i < specPoints - near;

        for (int k = -near; peak && k <= near; ++k)
            if (level[(size_t) juce::jlimit (0, specPoints - 1, i + k)] > level[(size_t) i])
                peak = false;

        if (peak)
        {
            auto sum = 0.0f;
            auto count = 0;

            for (int k = -wide; k <= wide; ++k)
            {
                const auto j = i + k;

                if (juce::isPositiveAndBelow (j, specPoints) && std::abs (k) > near)
                {
                    sum += level[(size_t) j];
                    ++count;
                }
            }

            peak = count > 0 && level[(size_t) i] - sum / (float) count >= prominenceDb;
        }

        isPeak[(size_t) i] = peak;
    }
}

// ===========================================================================
// curve
// ===========================================================================

// The rate the curve is drawn at is the rate the chain is running at, so what
// is on screen is what the audio is doing. Before a frame has arrived — and in
// the offline render harness — 48 kHz stands in.
double TuneView::displayRate() const
{
    return frame.pre.sampleRate > 0.0 ? frame.pre.sampleRate : 48000.0;
}

void TuneView::rebuildCurve()
{
    const auto rate = displayRate();

    if (! juce::approximatelyEqual (cosSampleRate, rate))
    {
        const auto span = std::log (eqmath::maxFreqHz / eqmath::minFreqHz);

        for (int i = 0; i < curvePoints; ++i)
        {
            const auto f = eqmath::minFreqHz
                           * std::exp (span * (double) i / (double) (curvePoints - 1));
            const auto w = eqmath::twoPi * f / rate;

            curveFreq[(size_t) i] = f;
            cosW[(size_t) i] = std::cos (w);
            sinW[(size_t) i] = std::sin (w);
            cos2W[(size_t) i] = std::cos (2.0 * w);
            sin2W[(size_t) i] = std::sin (2.0 * w);
        }

        cosSampleRate = rate;
    }

    // The sections come from StemFx's own builder, so the curve on screen is
    // not a second opinion about the filters — it is the filters. (eqmath in
    // the header states the same maths independently; the two are nulled
    // against each other rather than one trusting the other.)
    stages.clear();

    std::array<EqStage, (size_t) maxEqStagesPerBand> built {};

    for (int i = 0; i < fx.bands.size(); ++i)
    {
        const auto& b = fx.bands[i];
        const auto n = b.shape == EqShape::allPass ? 0 : eqBandStages (b, rate, built.data());

        stageFirst[(size_t) i] = (int) stages.size();
        stageCount[(size_t) i] = n;

        for (int s = 0; s < n; ++s)
        {
            const auto& e = built[(size_t) s];
            stages.push_back ({ e.b0, e.b1, e.b2, 1.0, e.a1, e.a2 });
        }
    }

    for (int i = 0; i < curvePoints; ++i)
    {
        auto sum = 0.0;

        for (int band = 0; band < fx.bands.size(); ++band)
        {
            if (! fx.bands[band].enabled)
                continue;

            for (int s = 0; s < stageCount[(size_t) band]; ++s)
                sum += eqmath::magnitudeDbAt (stages[(size_t) (stageFirst[(size_t) band] + s)],
                                              cosW[(size_t) i], sinW[(size_t) i],
                                              cos2W[(size_t) i], sin2W[(size_t) i]);
        }

        sumCurve[(size_t) i] = (float) sum;
    }

    // Auto gain is a best-effort offset: the log-averaged lift of the static
    // curve, taken back out again. Showing it on the curve is what makes the
    // toggle legible — the nodes stay where the user put them.
    autoGainOffset = 0.0f;

    if (fx.autoGain)
    {
        auto mean = 0.0;

        for (int i = 0; i < curvePoints; ++i)
            mean += sumCurve[(size_t) i];

        autoGainOffset = (float) (-mean / (double) curvePoints);
    }
}

// ===========================================================================
// painting
// ===========================================================================

void TuneView::paintGrid (juce::Graphics& g, float physicalScale)
{
    const auto area = gridArea();
    const auto scale = juce::jlimit (1.0f, 3.0f, physicalScale);

    const auto ink = theme::bg.getARGB() ^ theme::text.getARGB() ^ theme::accent.getARGB();

    if (gridImage.isNull() || gridFor != area || ! juce::approximatelyEqual (gridScale, scale)
        || gridRange != rangeIndex || gridInk != ink)
    {
        gridImage = juce::Image (juce::Image::ARGB,
                                 juce::jmax (1, juce::roundToInt ((float) area.getWidth() * scale)),
                                 juce::jmax (1, juce::roundToInt ((float) area.getHeight() * scale)),
                                 true);

        juce::Graphics gg (gridImage);
        gg.addTransform (juce::AffineTransform::translation ((float) -area.getX(),
                                                             (float) -area.getY())
                             .scaled (scale, scale));

        const auto plot = plotBounds();

        // Minor frequency lines first, so the labelled decades sit on top.
        gg.setColour (theme::lineSoft10.withMultipliedAlpha (0.45f));

        for (double decade : { 10.0, 100.0, 1000.0, 10000.0 })
            for (int m = 2; m <= 9; ++m)
            {
                const auto f = decade * m;

                if (f > eqmath::minFreqHz && f < eqmath::maxFreqHz)
                    gg.fillRect ((int) std::round (xForFreq (f)), plot.getY(), 1, plot.getHeight());
            }

        gg.setColour (theme::lineSoft15);

        for (double f : { 100.0, 1000.0, 10000.0 })
            gg.fillRect ((int) std::round (xForFreq (f)), plot.getY(), 1, plot.getHeight());

        // dB lines, with the zero line carrying a touch more ink.
        const auto range = rangeChoices[rangeIndex];
        const auto step = rangeSteps[rangeIndex];

        gg.setFont (theme::mono (8.0f));

        for (auto d = -range; d <= range + 0.01f; d += step)
        {
            const auto y = (int) std::round (yForDb (d));
            const auto zero = std::abs (d) < 0.01f;

            gg.setColour (zero ? theme::lineSoft15 : theme::lineSoft10.withMultipliedAlpha (0.55f));
            gg.fillRect (plot.getX(), y, plot.getWidth(), 1);

            // The outermost caption sits on the frame itself, so its box is
            // nudged back inside rather than half-clipped by the plot edge.
            gg.setColour (zero ? theme::text40 : theme::text35);
            gg.drawText (juce::String (d > 0.0f ? "+" : "") + juce::String ((int) d),
                         area.getX(), juce::jlimit (plot.getY(), plot.getBottom() - 12, y - 6),
                         dbGutter - 5, 12, juce::Justification::centredRight);
        }

        gg.setColour (theme::line);
        gg.drawRect (plot, 1);

        // Frequency captions in the strip below the plot.
        gg.setFont (theme::mono (8.0f));
        gg.setColour (theme::text35);

        for (double f : freqTicks)
        {
            const auto x = (int) std::round (xForFreq (f));
            const auto label = freqTick (f);
            const auto w = (int) ui::textWidth (theme::mono (8.0f), label) + 4;

            gg.drawText (label,
                         juce::jlimit (plot.getX(), plot.getRight() - w, x - w / 2),
                         plot.getBottom() + 1, w, freqLabelHeight - 1,
                         juce::Justification::centred);
        }

        gridFor = area;
        gridScale = scale;
        gridRange = rangeIndex;
        gridInk = ink;
    }

    g.drawImage (gridImage, area.toFloat(), juce::RectanglePlacement::stretchToFit, false);
}

void TuneView::paintSpectrum (juce::Graphics& g)
{
    const auto plot = plotBounds();

    const auto buildPath = [this, plot] (juce::Path& path, const std::array<float, specPoints>& data,
                                         bool closeToFloor)
    {
        path.clear();
        path.preallocateSpace (specPoints * 3 + 8);

        for (int i = 0; i < specPoints; ++i)
        {
            const auto x = xForFreq ((double) pointFreq[(size_t) i]);
            const auto y = yForSpecDb ((double) (data[(size_t) i] + tiltDb[(size_t) i]));

            if (i == 0)
                path.startNewSubPath (x, y);
            else
                path.lineTo (x, y);
        }

        if (closeToFloor)
        {
            path.lineTo ((float) plot.getRight(), (float) plot.getBottom());
            path.lineTo ((float) plot.getX(), (float) plot.getBottom());
            path.closeSubPath();
        }
    };

    buildPath (specPath, preSpectrum.level(), true);
    g.setColour (theme::text.withAlpha (0.10f));
    g.fillPath (specPath);

    buildPath (specPath, postSpectrum.peak(), false);
    g.setColour (theme::text.withAlpha (0.12f));
    g.strokePath (specPath, juce::PathStrokeType (0.7f));

    buildPath (specPath, postSpectrum.level(), false);
    g.setColour (theme::text.withAlpha (0.30f));
    g.strokePath (specPath, juce::PathStrokeType (0.9f));

    // Spectrum Grab: the peak under the pointer offers itself as a node.
    if (ghostPoint >= 0)
    {
        const auto x = xForFreq ((double) pointFreq[(size_t) ghostPoint]);
        const auto y = yForSpecDb ((double) (postSpectrum.level()[(size_t) ghostPoint]
                                             + tiltDb[(size_t) ghostPoint]));

        g.setColour (theme::accentTint40);
        g.fillRect (x, (float) plot.getY(), 1.0f, (float) plot.getHeight());

        g.setColour (theme::accentSoft.withAlpha (0.55f));
        g.drawEllipse (x - nodeRadius, y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f, 1.0f);
        g.drawEllipse (x - nodeRadius - 3.0f, y - nodeRadius - 3.0f,
                       nodeRadius * 2.0f + 6.0f, nodeRadius * 2.0f + 6.0f, 0.6f);
    }
}

void TuneView::paintCurves (juce::Graphics& g)
{
    const auto plot = plotBounds();

    // Solo dims everything but the band's own region, so what is being
    // listened to is what is lit.
    auto soloBand = -1;

    for (int i = 0; i < fx.bands.size(); ++i)
        if (fx.bands[i].solo)
            soloBand = i;

    if (soloBand >= 0)
    {
        double lo = 0.0, hi = 0.0;
        eqmath::bandSpanHz (fx.bands[soloBand], lo, hi);

        const auto xa = (int) std::round (xForFreq (lo));
        const auto xb = (int) std::round (xForFreq (hi));

        g.setColour (theme::bg.withAlpha (0.62f));
        g.fillRect (plot.getX(), plot.getY(), xa - plot.getX(), plot.getHeight());
        g.fillRect (xb, plot.getY(), plot.getRight() - xb, plot.getHeight());
    }

    const auto strokeCurve = [this, plot] (juce::Path& path, auto&& valueAt, int stride)
    {
        path.clear();
        path.preallocateSpace (curvePoints * 3 + 8);

        auto started = false;

        for (int i = 0; i < curvePoints; i += stride)
        {
            const auto x = xForFreq (curveFreq[(size_t) i]);
            const auto y = juce::jlimit ((float) plot.getY() - 40.0f,
                                         (float) plot.getBottom() + 40.0f,
                                         yForDb (valueAt (i)));

            if (! started)
            {
                path.startNewSubPath (x, y);
                started = true;
            }
            else
            {
                path.lineTo (x, y);
            }
        }
    };

    // Per-band ghosts behind the sum, the active one brighter than the rest.
    for (int band = 0; band < fx.bands.size(); ++band)
    {
        const auto& b = fx.bands[band];

        if (! b.enabled || stageCount[(size_t) band] == 0 || fx.bands.size() < 2)
            continue;

        strokeCurve (ghostPath,
                     [this, band] (int i)
                     {
                         auto sum = 0.0;

                         for (int s = 0; s < stageCount[(size_t) band]; ++s)
                             sum += eqmath::magnitudeDbAt (
                                 stages[(size_t) (stageFirst[(size_t) band] + s)],
                                 cosW[(size_t) i], sinW[(size_t) i],
                                 cos2W[(size_t) i], sin2W[(size_t) i]);

                         return sum;
                     },
                     2);

        const auto selected = isSelected (band);

        g.setColour (selected ? theme::accentSoft.withAlpha (0.45f) : theme::accentTint35);
        g.strokePath (ghostPath, juce::PathStrokeType (selected ? 1.0f : 0.7f));
    }

    strokeCurve (curvePath,
                 [this] (int i) { return (double) (sumCurve[(size_t) i] + autoGainOffset); }, 1);

    g.setColour (theme::accent);
    g.strokePath (curvePath, juce::PathStrokeType (1.6f));
}

void TuneView::paintNodes (juce::Graphics& g)
{
    for (int i = 0; i < fx.bands.size(); ++i)
    {
        const auto& b = fx.bands[i];
        const auto x = xForFreq (b.freqHz);
        const auto y = nodeY (b);
        const auto selected = isSelected (i);
        const auto active = i == activeBand;

        // A dynamic band brackets the ground its gain can cover.
        if (isDynamic (b))
        {
            const auto y2 = yForDb ((double) b.gainDb + (double) b.dynRangeDb);

            g.setColour (theme::accentTint40);
            g.fillRect (x - 0.5f, juce::jmin (y, y2), 1.0f, std::abs (y2 - y));
            g.fillRect (x - 3.0f, y2 - 0.5f, 6.0f, 1.0f);
        }

        if (selected)
        {
            g.setColour (theme::accentTint15);
            g.fillEllipse (x - nodeRadius - 4.0f, y - nodeRadius - 4.0f,
                           nodeRadius * 2.0f + 8.0f, nodeRadius * 2.0f + 8.0f);

            g.setColour (theme::accent);
            g.drawEllipse (x - nodeRadius - 3.0f, y - nodeRadius - 3.0f,
                           nodeRadius * 2.0f + 6.0f, nodeRadius * 2.0f + 6.0f, 1.0f);
        }

        g.setColour (theme::bg);
        g.fillEllipse (x - nodeRadius, y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f);

        const auto ring = ! b.enabled ? theme::text35
                          : active    ? theme::accent
                          : hoverBand == i ? theme::accentSoft
                                           : theme::accentMid;

        g.setColour (ring);
        g.drawEllipse (x - nodeRadius, y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f,
                       b.enabled ? 1.4f : 0.9f);

        if (b.solo)
        {
            g.setColour (theme::accentSoft);
            g.fillEllipse (x - 1.6f, y - 1.6f, 3.2f, 3.2f);
        }

        // A band louder than the axis is showing sits on the edge with a caret
        // pointing at where it really is, rather than pretending to be there.
        if (hasGain (b.shape) && std::abs (b.gainDb) > rangeChoices[rangeIndex] + 0.05f)
        {
            const auto up = b.gainDb > 0.0f;
            const auto tip = up ? y - nodeRadius - 4.0f : y + nodeRadius + 4.0f;

            juce::Path caret;
            caret.startNewSubPath (x - 3.0f, up ? tip + 3.0f : tip - 3.0f);
            caret.lineTo (x, tip);
            caret.lineTo (x + 3.0f, up ? tip + 3.0f : tip - 3.0f);

            g.setColour (theme::accentSoft);
            g.strokePath (caret, juce::PathStrokeType (1.0f));
        }
    }

    // The way into the popover without a right-click.
    if (activeBand >= 0)
    {
        const auto chip = nodeMenuBounds (activeBand);

        g.setColour (hoverCell == cellNodeMenu ? theme::accentSoft : theme::accentTint40);

        for (int i = 0; i < 3; ++i)
            g.fillRect (chip.getX() + 2 + i * 4, chip.getCentreY() - 1, 2, 2);
    }

    if (grab == Grab::rubber)
    {
        g.setColour (theme::accentTint6);
        g.fillRect (rubberRect);
        g.setColour (theme::accentTint40);
        g.drawRect (rubberRect, 1);
    }
}

void TuneView::paintReadout (juce::Graphics& g)
{
    const auto plot = plotBounds();
    auto line = plot.reduced (6, 4).removeFromTop (11);

    g.setFont (theme::mono (8.0f));

    if (noticeFrames > 0)
    {
        g.setColour (theme::accentSoft);
        g.drawText (notice, line, juce::Justification::centredLeft);
        return;
    }

    if (juce::isPositiveAndBelow (activeBand, fx.bands.size()))
    {
        const auto& b = fx.bands[activeBand];

        juce::String text (shapeName (b.shape));
        const auto dot = juce::String::fromUTF8 (" \xc2\xb7 ");

        text += dot + freqCaption (b.freqHz);

        if (hasGain (b.shape))
            text += dot + juce::String (b.gainDb > 0.0f ? "+" : "") + juce::String (b.gainDb, 1) + " dB";

        if (isCut (b.shape))
            text += dot + juce::String (b.slopeDbOct) + " dB/oct";

        text += dot + "q " + juce::String (b.q, 2);

        if (isDynamic (b))
            text += dot + "dyn " + juce::String (b.dynRangeDb > 0.0f ? "+" : "")
                    + juce::String (b.dynRangeDb, 1) + " dB";

        if (! b.enabled)
            text += dot + "off";

        g.setColour (b.enabled ? theme::text55 : theme::text35);
        g.drawText (text, line, juce::Justification::centredLeft);

        if (selection.size() > 1)
        {
            g.setColour (theme::accentSoft);
            g.drawText (juce::String ((int) selection.size()) + " selected", line,
                        juce::Justification::centredRight);
        }

        return;
    }

    g.setColour (theme::text35);
    g.drawText ("double-click to add a band", line, juce::Justification::centredLeft);
}

void TuneView::paintStrip (juce::Graphics& g)
{
    const auto font = theme::mono (8.5f, 0.12f);

    // Section labels, each with a hairline out to its column edge.
    for (int i = 0; i < 2; ++i)
    {
        const auto label = stripRow (i == 0 ? pThresh : pGain)
                               .translated (0, -sectionHeight)
                               .withHeight (sectionHeight);

        const auto text = i == 0 ? "COMP" : "OUT";

        g.setFont (font);
        g.setColour (theme::text35);
        g.drawText (text, label, juce::Justification::centredLeft);

        const auto ruleX = label.getX() + (int) ui::textWidth (font, text) + 8;
        g.setColour (theme::lineSoft10);
        g.fillRect (ruleX, label.getCentreY(), label.getRight() - ruleX, 1);
    }

    for (int id = 0; id < numStrip; ++id)
        drawSlider (g, stripSpecs[id], stripRow (id), stripTrack (id), stripValue (fx, id),
                    hoverStrip == id || stripParam == id, stripLive (fx, id),
                    labelColumn, readoutWidth);

    ui::drawMiniToggle (g, phaseBounds().getX(), phaseBounds().getY(), "PHASE", fx.phaseInvert,
                        hoverCell == cellPhase, phaseBounds().getWidth());

    ui::drawMiniToggle (g, autoBounds().getX(), autoBounds().getY(), "AUTO GAIN", fx.autoGain,
                        hoverCell == cellAuto, autoBounds().getWidth());
}

void TuneView::paintPopover (juce::Graphics& g)
{
    if (! popoverOpen || ! juce::isPositiveAndBelow (popoverBand, fx.bands.size()))
        return;

    auto& b = fx.bands[popoverBand];
    const auto L = popLayout (popoverBounds, isCut (b.shape));

    g.setColour (theme::bg);
    g.fillRect (popoverBounds);
    g.setColour (theme::bgPanelAlt);
    g.fillRect (popoverBounds);
    g.setColour (theme::accentTint40);
    g.drawRect (popoverBounds, 1);

    g.setFont (theme::mono (8.5f, 0.12f));
    g.setColour (theme::text35);
    g.drawText ("SHAPE", L.shapeLabel, juce::Justification::centredLeft);

    for (int i = 0; i < 10; ++i)
    {
        const auto on = shapeOrder[i] == b.shape;
        ui::drawMiniToggle (g, L.shape[(size_t) i].getX(), L.shape[(size_t) i].getY(),
                            shapeCell (i), on, hoverPopCell == popShapeId + i,
                            L.shape[(size_t) i].getWidth(), popCellH);
    }

    if (L.hasSlope)
    {
        g.setFont (theme::mono (8.5f, 0.12f));
        g.setColour (theme::text35);
        g.drawText ("SLOPE dB/OCT", L.slopeLabel, juce::Justification::centredLeft);

        for (int i = 0; i < 6; ++i)
            ui::drawMiniToggle (g, L.slope[(size_t) i].getX(), L.slope[(size_t) i].getY(),
                                juce::String (slopeChoices[i]), b.slopeDbOct == slopeChoices[i],
                                hoverPopCell == popSlopeId + i, L.slope[(size_t) i].getWidth(),
                                popCellH);
    }

    const auto dynamic = canBeDynamic (b.shape);

    g.setFont (theme::mono (8.5f, 0.12f));
    g.setColour (theme::text35);
    g.drawText (dynamic ? "DYNAMIC" : "DYNAMIC (BELL AND SHELVES ONLY)", L.dynLabel,
                juce::Justification::centredLeft);

    for (int i = 0; i < numDyn; ++i)
        drawSlider (g, dynSpecs[i], L.row[(size_t) i], L.track[(size_t) i], dynValue (b, i),
                    hoverPopSlider == i || popSlider == i,
                    dynamic && (i == dRange || isDynamic (b)), popLabelCol, popReadout);

    ui::drawMiniToggle (g, L.solo.getX(), L.solo.getY(), "SOLO", b.solo,
                        hoverPopCell == popSoloId, L.solo.getWidth(), popFootH);

    ui::drawMiniToggle (g, L.bypass.getX(), L.bypass.getY(), "BYPASS", ! b.enabled,
                        hoverPopCell == popBypassId, L.bypass.getWidth(), popFootH);

    ui::drawMiniToggle (g, L.del.getX(), L.del.getY(), "DELETE", false,
                        hoverPopCell == popDeleteId, L.del.getWidth(), popFootH);
}

void TuneView::paint (juce::Graphics& g)
{
    // Scrim: the row being shaped stays faintly visible underneath.
    g.setColour (theme::bg.withAlpha (0.90f));
    g.fillAll();

    const auto panel = panelBounds();

    g.setColour (theme::bg);
    g.fillRect (panel);
    g.setColour (theme::bgPanelAlt);
    g.fillRect (panel);
    g.setColour (theme::lineStrong);
    g.drawRect (panel, 1);

    // --- title: which stem, and whether it is carrying anything ---
    {
        auto title = bodyBounds().removeFromTop (titleHeight);

        g.setColour (theme::text);
        g.setFont (theme::mono (13.0f, 0.04f));
        g.drawText ("tune " + juce::String::fromUTF8 ("\xe2\x80\x94") + " " + name.toLowerCase(),
                    title, juce::Justification::centredLeft);

        const auto flat = fx.isFlat();
        const auto count = fx.bands.size();

        const auto tag = flat    ? juce::String ("FLAT")
                         : count > 0 ? juce::String (count) + (count == 1 ? " BAND" : " BANDS")
                                     : juce::String ("PROCESSED");

        g.setColour (flat ? theme::text35 : theme::accentSoft);
        g.setFont (theme::mono (9.5f, 0.08f));
        g.drawText (tag, title, juce::Justification::centredRight);
    }

    // --- display controls ---
    {
        const auto h = headerCells();
        const auto row = controlsRow();

        drawTinyLabel (g, juce::String ("RANGE ") + juce::String::fromUTF8 ("\xc2\xb1"),
                       row.getX(), row.getY(), row.getHeight());

        ui::drawMiniCells (g, h.range[0].getX(), row.getY(), { "3", "6", "12", "30" },
                           rangeIndex, hoverCell, cellRange, 26);

        drawTinyLabel (g, "ANALYZER", h.speedLabelX, row.getY(), row.getHeight());

        ui::drawMiniCells (g, h.speed[0].getX(), row.getY(), { "FAST", "MED", "SLOW" },
                           speedIndex, hoverCell, cellSpeed, 32);

        ui::drawMiniToggle (g, h.freeze.getX(), h.freeze.getY(), "FREEZE", frozen,
                            hoverCell == cellFreeze, h.freeze.getWidth());
    }

    rebuildCurve();

    paintGrid (g, g.getInternalContext().getPhysicalPixelScaleFactor());

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (plotBounds());

        paintSpectrum (g);
        paintCurves (g);
        paintNodes (g);
        paintReadout (g);
    }

    paintStrip (g);

    // --- footer: the way out at the house corner, RESET opposite it ---
    auto footer = footerRow();

    closeBounds = ui::drawTagAction (g, "CLOSE", footer.getX(), footer.getY(),
                                     hoverCell == cellClose, theme::line, theme::text55);

    resetBounds = {};

    if (! fx.isFlat())
    {
        const auto width = (int) std::ceil (ui::textWidth (theme::mono (9.5f, 0.08f), "RESET")) + 24;

        resetBounds = ui::drawTagAction (g, "RESET", footer.getRight() - width, footer.getY(),
                                         hoverCell == cellReset, theme::accentTint40,
                                         theme::accentSoft);
    }

    paintPopover (g);
}

// ===========================================================================
// editing
// ===========================================================================

bool TuneView::isSelected (int band) const
{
    return std::find (selection.begin(), selection.end(), band) != selection.end();
}

void TuneView::setSelection (int band)
{
    selection.clear();

    if (band >= 0)
        selection.push_back (band);

    activeBand = band;
}

// The band's own limits, never the display's: the dB range is a zoom, and a
// band parked at +11 has to survive a look at it through the ±3 axis.
void TuneView::clampBand (EqBand& b) const
{
    b.freqHz = juce::jlimit ((float) eqmath::minFreqHz, (float) eqmath::maxFreqHz, b.freqHz);
    b.gainDb = juce::jlimit ((float) -eqmath::maxGainDb, (float) eqmath::maxGainDb, b.gainDb);
    b.q = juce::jlimit ((float) eqmath::minQ, (float) eqmath::maxQ, b.q);
    b.dynRangeDb = juce::jlimit (-24.0f, 24.0f, b.dynRangeDb);
}

void TuneView::flash (const juce::String& text)
{
    notice = text;
    noticeFrames = 45; // ~1.5 s at the panel's 30 Hz
}

int TuneView::addBandAt (double hz, double db, bool commit)
{
    if (fx.bands.full())
    {
        flash (juce::String (maxEqBands) + " bands is the limit");
        return -1;
    }

    EqBand b;
    b.freqHz = (float) juce::jlimit (eqmath::minFreqHz, eqmath::maxFreqHz, hz);
    b.gainDb = (float) db;
    b.q = 1.0f;

    // Out at the edges of the axis a cut is almost always what is wanted, so
    // that is what a double-click leaves there.
    const auto n = std::log (b.freqHz / eqmath::minFreqHz)
                   / std::log (eqmath::maxFreqHz / eqmath::minFreqHz);

    if (n < 0.06)
    {
        b.shape = EqShape::lowCut;
        b.gainDb = 0.0f;
        b.q = 0.7071f;
        b.slopeDbOct = 24;
    }
    else if (n > 0.94)
    {
        b.shape = EqShape::highCut;
        b.gainDb = 0.0f;
        b.q = 0.7071f;
        b.slopeDbOct = 24;
    }

    clampBand (b);

    if (! fx.bands.push_back (b))
        return -1;

    const auto index = fx.bands.size() - 1;
    setSelection (index);
    send (commit);

    return index;
}

void TuneView::deleteSelection()
{
    if (selection.empty())
        return;

    auto sorted = selection;
    std::sort (sorted.begin(), sorted.end(), std::greater<int>());

    for (auto i : sorted)
        fx.bands.erase (i);

    selection.clear();
    activeBand = -1;
    closePopover();
    send (true);
}

void TuneView::applySolo (int band, bool on)
{
    for (int i = 0; i < fx.bands.size(); ++i)
        fx.bands[i].solo = on && i == band;

    send (false); // transient: solo never rides out to the bounce
}

void TuneView::clearSolo()
{
    auto any = false;

    for (int i = 0; i < fx.bands.size(); ++i)
        any = any || fx.bands[i].solo;

    if (any)
        applySolo (-1, false);
}

void TuneView::openPopover (int band)
{
    if (! juce::isPositiveAndBelow (band, fx.bands.size()))
        return;

    popoverOpen = true;
    popoverBand = band;
    layoutPopover();
}

void TuneView::layoutPopover()
{
    if (! juce::isPositiveAndBelow (popoverBand, fx.bands.size()))
        return;

    const auto& b = fx.bands[popoverBand];
    const auto h = popoverHeight (isCut (b.shape));
    const auto anchorX = (int) std::round (xForFreq (b.freqHz));
    const auto anchorY = (int) std::round (nodeY (b));

    const auto room = panelBounds().reduced (6);

    auto y = anchorY + 14;

    if (y + h > room.getBottom())
        y = anchorY - 14 - h; // no room below: flip above the node

    y = juce::jlimit (room.getY(), juce::jmax (room.getY(), room.getBottom() - h), y);

    const auto x = juce::jlimit (room.getX(), juce::jmax (room.getX(), room.getRight() - popWidth),
                                 anchorX - popWidth / 2);

    popoverBounds = { x, y, popWidth, h };
}

void TuneView::closePopover()
{
    popoverOpen = false;
    popoverBand = -1;
    popSlider = -1;
    hoverPopCell = cellNone;
    hoverPopSlider = -1;
    popoverBounds = {};
}

// ===========================================================================
// mouse
// ===========================================================================

void TuneView::beginNodeDrag (const juce::MouseEvent& e, bool qMode)
{
    grab = qMode ? Grab::nodeQ : Grab::node;
    gestureMoved = false;
    pressPoint = e.getPosition();
    pressFreq = freqForX ((float) e.x);
    pressDb = dbForY ((float) e.y);

    anchors.clear();

    for (auto i : selection)
        if (juce::isPositiveAndBelow (i, fx.bands.size()))
            anchors.push_back ({ i, fx.bands[i].freqHz, fx.bands[i].gainDb, fx.bands[i].q });
}

void TuneView::dragNodes (const juce::MouseEvent& e)
{
    if (anchors.empty())
        return;

    if (grab == Grab::nodeQ)
    {
        const auto factor = std::exp ((double) (pressPoint.y - e.y) * 0.012);

        for (const auto& a : anchors)
            fx.bands[a.index].q = juce::jlimit ((float) eqmath::minQ, (float) eqmath::maxQ,
                                                (float) (a.q * factor));
    }
    else
    {
        // Freq travels together in log space, gain together in dB, and the
        // delta itself is clamped by whichever member of the group hits an
        // axis limit first — so a group drag never squashes its own shape.
        auto dLogF = std::log (freqForX ((float) e.x) / pressFreq);
        auto dGain = dbForY ((float) e.y) - pressDb;

        auto loF = -1.0e9, hiF = 1.0e9, loG = -1.0e9, hiG = 1.0e9;

        for (const auto& a : anchors)
        {
            loF = juce::jmax (loF, std::log (eqmath::minFreqHz / (double) a.freqHz));
            hiF = juce::jmin (hiF, std::log (eqmath::maxFreqHz / (double) a.freqHz));

            if (hasGain (fx.bands[a.index].shape))
            {
                loG = juce::jmax (loG, -eqmath::maxGainDb - (double) a.gainDb);
                hiG = juce::jmin (hiG, eqmath::maxGainDb - (double) a.gainDb);
            }
        }

        dLogF = juce::jlimit (loF, hiF, dLogF);
        dGain = juce::jlimit (juce::jmin (loG, 0.0), juce::jmax (hiG, 0.0), dGain);

        for (const auto& a : anchors)
        {
            auto& b = fx.bands[a.index];
            b.freqHz = (float) ((double) a.freqHz * std::exp (dLogF));

            if (hasGain (b.shape))
                b.gainDb = (float) ((double) a.gainDb + dGain);

            clampBand (b);
        }
    }

    gestureMoved = true;

    if (popoverOpen)
        layoutPopover();

    send (false);
    repaint (panelBounds());
}

void TuneView::dragStrip (const juce::MouseEvent& e, bool commit)
{
    if (stripParam < 0)
        return;

    const auto track = stripTrack (stripParam);
    const auto span = juce::jmax (1, track.getWidth() - 1);
    const auto n = (float) (e.x - track.getX()) / (float) span;

    setStripValue (fx, stripParam, valueOf (stripSpecs[stripParam], n));
    send (commit);
    repaint (panelBounds());
}

void TuneView::dragPopSlider (const juce::MouseEvent& e, bool commit)
{
    if (popSlider < 0 || ! juce::isPositiveAndBelow (popoverBand, fx.bands.size()))
        return;

    const auto L = popLayout (popoverBounds, isCut (fx.bands[popoverBand].shape));
    const auto track = L.track[(size_t) popSlider];
    const auto span = juce::jmax (1, track.getWidth() - 1);
    const auto n = (float) (e.x - track.getX()) / (float) span;

    setDynValue (fx.bands[popoverBand], popSlider, valueOf (dynSpecs[popSlider], n));
    send (commit);
    repaint (panelBounds());
}

bool TuneView::popoverDown (const juce::MouseEvent& e)
{
    if (! juce::isPositiveAndBelow (popoverBand, fx.bands.size()))
        return false;

    auto& b = fx.bands[popoverBand];
    const auto L = popLayout (popoverBounds, isCut (b.shape));
    const auto p = e.getPosition();

    for (int i = 0; i < numDyn; ++i)
    {
        auto hit = L.row[(size_t) i];
        hit = hit.withTrimmedLeft (popLabelCol).withTrimmedRight (popReadout - 4);

        if (hit.contains (p))
        {
            grab = Grab::popSlider;
            popSlider = i;
            dragPopSlider (e, false);
            return true;
        }
    }

    return true; // the popover swallows everything else until mouse-up
}

void TuneView::popoverUp (const juce::MouseEvent& e)
{
    if (! juce::isPositiveAndBelow (popoverBand, fx.bands.size()))
        return;

    auto& b = fx.bands[popoverBand];
    const auto L = popLayout (popoverBounds, isCut (b.shape));
    const auto p = e.getPosition();

    for (int i = 0; i < 10; ++i)
    {
        if (! L.shape[(size_t) i].contains (p))
            continue;

        if (b.shape == shapeOrder[i])
            return;

        b.shape = shapeOrder[i];

        if (isCut (b.shape))
        {
            b.gainDb = 0.0f;

            if (b.slopeDbOct < 12)
                b.slopeDbOct = 24;
        }

        clampBand (b);
        layoutPopover(); // the slope row appears or goes
        send (true);
        return;
    }

    if (L.hasSlope)
        for (int i = 0; i < 6; ++i)
            if (L.slope[(size_t) i].contains (p))
            {
                b.slopeDbOct = slopeChoices[i];
                send (true);
                return;
            }

    if (L.solo.contains (p))
    {
        applySolo (popoverBand, ! b.solo);
        return;
    }

    if (L.bypass.contains (p))
    {
        b.enabled = ! b.enabled;
        send (true);
        return;
    }

    if (L.del.contains (p))
    {
        setSelection (popoverBand);
        deleteSelection();
        return;
    }
}

void TuneView::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    grab = Grab::none;
    gestureMoved = false;
    stripParam = -1;
    popSlider = -1;
    pressedInPopover = false;
    pressedOutside = ! panelBounds().contains (p);

    // Delete needs the keys, and a plugin editor's focus is never guaranteed
    // to have landed where the panel wants it.
    if (! pressedOutside && ! hasKeyboardFocus (false))
        grabKeyboardFocus();

    if (popoverOpen)
    {
        if (popoverBounds.contains (p))
        {
            pressedInPopover = true;
            popoverDown (e);
            return;
        }

        closePopover(); // and let the click land where it was aimed
    }

    if (! panelBounds().contains (p))
        return; // the scrim closes on mouse-up

    const auto sp = stripParamAt (p);

    if (sp >= 0)
    {
        grab = Grab::strip;
        stripParam = sp;
        stripStart = stripValue (fx, sp);
        dragStrip (e, false); // live preview; the bounce waits for the release
        return;
    }

    if (! plotBounds().contains (p))
        return; // header cells, toggles and the footer act on mouse-up

    const auto band = bandAt (p);

    if (band >= 0)
    {
        if (e.mods.isPopupMenu())
        {
            setSelection (band);
            openPopover (band);
            repaint (panelBounds());
            return;
        }

        if (e.mods.isAltDown())
        {
            fx.bands[band].enabled = ! fx.bands[band].enabled;
            activeBand = band;
            send (true);
            repaint (panelBounds());
            return;
        }

        if (e.mods.isShiftDown())
        {
            const auto it = std::find (selection.begin(), selection.end(), band);

            if (it != selection.end())
                selection.erase (it);
            else
                selection.push_back (band);

            activeBand = selection.empty() ? -1 : band;
            repaint (panelBounds());
            return;
        }

        if (! isSelected (band))
            setSelection (band);
        else
            activeBand = band;

        beginNodeDrag (e, e.mods.isCommandDown());
        repaint (panelBounds());
        return;
    }

    if (activeBand >= 0 && nodeMenuBounds (activeBand).contains (p))
    {
        openPopover (activeBand);
        repaint (panelBounds());
        return;
    }

    // Spectrum Grab: the hovered peak either wakes the band already sitting on
    // it or drops a new one there, and the same press keeps dragging it.
    if (ghostPoint >= 0 && ! e.mods.isPopupMenu())
    {
        const auto hz = (double) pointFreq[(size_t) ghostPoint];
        auto found = -1;

        for (int i = 0; i < fx.bands.size() && found < 0; ++i)
            if (std::abs (std::log2 ((double) fx.bands[i].freqHz / hz)) <= 1.0 / 6.0)
                found = i;

        if (found >= 0)
            setSelection (found);
        else if (addBandAt (hz, 0.0, false) < 0)
            return;

        beginNodeDrag (e, false);
        repaint (panelBounds());
        return;
    }

    if (e.mods.isPopupMenu())
        return;

    // A shift rubber band adds to what was already picked; a plain one starts
    // the selection over.
    rubberBase.clear();

    if (e.mods.isShiftDown())
        rubberBase = selection;
    else
        setSelection (-1);

    grab = Grab::rubber;
    pressPoint = p;
    rubberRect = { p.x, p.y, 0, 0 };
    repaint (panelBounds());
}

void TuneView::mouseDrag (const juce::MouseEvent& e)
{
    switch (grab)
    {
        case Grab::node:
        case Grab::nodeQ:
            dragNodes (e);
            break;

        case Grab::strip:
            dragStrip (e, false);
            break;

        case Grab::popSlider:
            dragPopSlider (e, false);
            break;

        case Grab::rubber:
        {
            rubberRect = juce::Rectangle<int> (pressPoint, e.getPosition())
                             .getIntersection (plotBounds());

            selection = rubberBase;

            for (int i = 0; i < fx.bands.size(); ++i)
            {
                const juce::Point<int> node ((int) std::round (xForFreq (fx.bands[i].freqHz)),
                                             (int) std::round (nodeY (fx.bands[i])));

                if (rubberRect.contains (node) && ! isSelected (i))
                    selection.push_back (i);
            }

            activeBand = selection.empty() ? -1 : selection.front();
            repaint (panelBounds());
            break;
        }

        case Grab::none:
        default:
            break;
    }
}

void TuneView::mouseUp (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (grab == Grab::popSlider)
    {
        grab = Grab::none;
        popSlider = -1;
        pressedInPopover = false;
        send (true);
        repaint (panelBounds());
        return;
    }

    if (pressedInPopover)
    {
        // A press that started in the popover only ever acts on the popover:
        // dragging off a cell cancels it rather than firing whatever the
        // popover happens to be covering.
        grab = Grab::none;
        pressedInPopover = false;

        if (popoverOpen && popoverBounds.contains (p))
            popoverUp (e);

        repaint (panelBounds());
        return;
    }

    if (grab == Grab::node || grab == Grab::nodeQ)
    {
        grab = Grab::none;

        if (gestureMoved)
            send (true); // commit -> what drag-out and export will carry

        repaint (panelBounds());
        return;
    }

    if (grab == Grab::rubber)
    {
        grab = Grab::none;
        rubberRect = {};
        repaint (panelBounds());
        return;
    }

    if (grab == Grab::strip)
    {
        // Only a gesture that actually moved the slider commits: a commit is
        // what makes the editor re-bounce, and a press that landed back on the
        // value it started from has nothing to re-bounce.
        const auto moved = ! juce::approximatelyEqual (stripValue (fx, stripParam), stripStart);

        grab = Grab::none;
        stripParam = -1;

        if (moved)
            send (true);

        repaint (panelBounds());
        return;
    }

    switch (cellAtPoint (p))
    {
        case cellFreeze:
            frozen = ! frozen;
            repaint (panelBounds());
            return;

        case cellPhase:
            fx.phaseInvert = ! fx.phaseInvert;
            send (true);
            repaint (panelBounds());
            return;

        case cellAuto:
            fx.autoGain = ! fx.autoGain;
            send (true);
            repaint (panelBounds());
            return;

        case cellNodeMenu:
            openPopover (activeBand);
            repaint (panelBounds());
            return;

        case cellReset:
            fx = {};
            setSelection (-1);
            closePopover();
            send (true);
            repaint (panelBounds());
            return;

        case cellClose:
            clearSolo();

            if (onClose != nullptr)
                onClose();

            return;

        default:
            break;
    }

    const auto cell = cellAtPoint (p);

    if (cell >= cellRange && cell < cellRange + 4)
    {
        rangeIndex = cell - cellRange;

        if (popoverOpen)
            layoutPopover();

        repaint (panelBounds());
        return;
    }

    if (cell >= cellSpeed && cell < cellSpeed + 3)
    {
        speedIndex = cell - cellSpeed;
        applySpectrumSpeed();
        repaint (panelBounds());
        return;
    }

    // A click on the scrim outside the panel is the other way out — but only a
    // click that began there, so a gesture that slipped off the panel edge on
    // its way to nothing does not shut the panel.
    if (pressedOutside && ! panelBounds().contains (p))
    {
        clearSolo();

        if (onClose != nullptr)
            onClose();
    }
}

void TuneView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (popoverOpen && popoverBounds.contains (p))
        return;

    const auto sp = stripParamAt (p);

    if (sp >= 0)
    {
        // JUCE delivers this after the second mouse-up, which has already
        // moved the slider: this puts that one parameter back to its default
        // and commits, which is the state the gesture ends in either way.
        const StemFxParams flat;

        grab = Grab::none;
        stripParam = -1;

        if (juce::approximatelyEqual (stripValue (fx, sp), stripValue (flat, sp)))
            return;

        setStripValue (fx, sp, stripValue (flat, sp));
        send (true);
        repaint (panelBounds());
        return;
    }

    if (! plotBounds().contains (p))
        return;

    const auto band = bandAt (p);

    grab = Grab::none;

    if (band >= 0)
    {
        auto& b = fx.bands[band];

        if (! hasGain (b.shape) || juce::approximatelyEqual (b.gainDb, 0.0f))
            return;

        b.gainDb = 0.0f;
        setSelection (band);
        send (true);
        repaint (panelBounds());
        return;
    }

    addBandAt (freqForX ((float) e.x), dbForY ((float) e.y), true);
    repaint (panelBounds());
}

void TuneView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto band = bandAt (e.getPosition());

    if (band < 0 || ! plotBounds().contains (e.getPosition()))
        return;

    const auto delta = wheel.isReversed ? -wheel.deltaY : wheel.deltaY;

    if (std::abs (delta) < 1.0e-4f)
        return;

    activeBand = band;

    if (! isSelected (band))
        setSelection (band);

    for (auto i : selection)
    {
        auto& b = fx.bands[i];
        b.q = juce::jlimit ((float) eqmath::minQ, (float) eqmath::maxQ,
                            (float) ((double) b.q * std::exp ((double) delta * 2.2)));
    }

    send (false);

    // The wheel has no release, so the commit waits for it to settle.
    wheelCommitFrames = 9;
    repaint (panelBounds());
}

void TuneView::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    const auto cell = popoverOpen && popoverBounds.contains (p) ? cellNone : cellAtPoint (p);
    const auto strip = popoverOpen && popoverBounds.contains (p) ? -1 : stripParamAt (p);
    const auto band = popoverOpen && popoverBounds.contains (p) ? -1 : bandAt (p);

    int popCell = cellNone;
    int popSliderHover = -1;

    if (popoverOpen && popoverBounds.contains (p)
        && juce::isPositiveAndBelow (popoverBand, fx.bands.size()))
    {
        const auto L = popLayout (popoverBounds, isCut (fx.bands[popoverBand].shape));

        for (int i = 0; i < 10; ++i)
            if (L.shape[(size_t) i].contains (p))
                popCell = popShapeId + i;

        if (L.hasSlope)
            for (int i = 0; i < 6; ++i)
                if (L.slope[(size_t) i].contains (p))
                    popCell = popSlopeId + i;

        if (L.solo.contains (p)) popCell = popSoloId;
        if (L.bypass.contains (p)) popCell = popBypassId;
        if (L.del.contains (p)) popCell = popDeleteId;

        for (int i = 0; i < numDyn; ++i)
            if (L.row[(size_t) i].withTrimmedLeft (popLabelCol).contains (p))
                popSliderHover = i;
    }

    // A spectrum peak only offers a ghost node when nothing else is under the
    // pointer to fight it for the click.
    auto ghost = -1;

    if (band < 0 && popCell == cellNone && plotBounds().contains (p) && spectrumPrimed)
    {
        auto bestDx = 13.0f;

        for (int i = 0; i < specPoints; ++i)
        {
            if (! isPeak[(size_t) i])
                continue;

            const auto dx = std::abs (xForFreq ((double) pointFreq[(size_t) i]) - (float) p.x);

            if (dx < bestDx)
            {
                bestDx = dx;
                ghost = i;
            }
        }
    }

    setMouseCursor (band >= 0 || ghost >= 0 || strip >= 0 || cell != cellNone
                            || popCell != cellNone || popSliderHover >= 0
                        ? juce::MouseCursor::PointingHandCursor
                        : juce::MouseCursor::NormalCursor);

    if (cell == hoverCell && strip == hoverStrip && band == hoverBand && ghost == ghostPoint
        && popCell == hoverPopCell && popSliderHover == hoverPopSlider)
        return;

    hoverCell = cell;
    hoverStrip = strip;
    hoverBand = band;
    hoverPopCell = popCell;
    hoverPopSlider = popSliderHover;
    ghostPoint = ghost;
    repaint (panelBounds());
}

void TuneView::mouseExit (const juce::MouseEvent&)
{
    if (hoverCell == cellNone && hoverStrip < 0 && hoverBand < 0 && ghostPoint < 0
        && hoverPopCell == cellNone && hoverPopSlider < 0)
        return;

    hoverCell = cellNone;
    hoverStrip = -1;
    hoverBand = -1;
    hoverPopCell = cellNone;
    hoverPopSlider = -1;
    ghostPoint = -1;
    repaint (panelBounds());
}

bool TuneView::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        if (selection.empty())
            return false;

        deleteSelection();
        repaint (panelBounds());
        return true;
    }

    if (key == juce::KeyPress::escapeKey)
    {
        if (popoverOpen)
        {
            closePopover();
            repaint (panelBounds());
            return true;
        }

        clearSolo();

        if (onClose != nullptr)
            onClose();

        return true;
    }

    return false;
}

} // namespace splittr
