#include "TuneView.h"
#include "Common.h"

namespace splittr
{

namespace
{
    // The overlay sits under the 39px header, so the scrim has 30px either
    // side and ~24px above and below to show what is being shaped.
    constexpr int panelWidth = 500, panelHeight = 292;
    constexpr int panelPad = 18;
    constexpr int titleHeight = 21, titleGap = 10;
    constexpr int footerGap = 12;

    // Two columns: PASS + EQ on the left, COMP + OUT on the right.
    constexpr int columnGap = 24;
    constexpr int sectionHeight = 16, groupGap = 8, rowHeight = 23;

    // Inside a slider row: [label][track][readout].
    constexpr int labelColumn = 52, readoutWidth = 48, innerGap = 6;

    // A 1px track is unhittable on its own, so the grab band is the row's full
    // height and a few px of slack at each end.
    constexpr int grabSlack = 4;

    constexpr float tickWidth = 2.0f, tickHeight = 7.0f;

    // Where the filled part of a track starts, so "how much this parameter is
    // doing" reads the same in every group.
    enum class Fill { fromLeft, fromCentre, fromRight };

    // The passes bypass at 0, which is outside their own range: they get a
    // short dead zone at the end of the track that reads OFF. The threshold's
    // off value (0 dB) is already the top of its range, so it needs none.
    enum class Off { none, atMin, atMax };

    enum class Unit { hz, db, ratio };

    // The parameters in signal order, which is also their layout order.
    enum Param { pHp, pLp, pLow, pMid, pMidHz, pHigh, pThresh, pRatio, pMakeup, pGain, numParams };

    struct Spec
    {
        const char* label;
        float min, max, step;
        bool logScale; // frequencies travel the track logarithmically, dB linearly
        Fill fill;
        Off off;
        bool zeroIsOff; // 0 reads OFF rather than as a value
        Unit unit;
    };

    constexpr Spec specs[] = {
        { "HP",       20.0f,   600.0f,   1.0f, true,  Fill::fromLeft,   Off::atMin, true,  Unit::hz },
        { "LP",       1000.0f, 20000.0f, 1.0f, true,  Fill::fromRight,  Off::atMax, true,  Unit::hz },
        { "LOW",      -12.0f,  12.0f,    0.5f, false, Fill::fromCentre, Off::none,  false, Unit::db },
        { "MID",      -12.0f,  12.0f,    0.5f, false, Fill::fromCentre, Off::none,  false, Unit::db },
        { "MID FREQ", 150.0f,  6000.0f,  1.0f, true,  Fill::fromLeft,   Off::none,  false, Unit::hz },
        { "HIGH",     -12.0f,  12.0f,    0.5f, false, Fill::fromCentre, Off::none,  false, Unit::db },
        { "THRESH",   -48.0f,  0.0f,     0.5f, false, Fill::fromRight,  Off::none,  true,  Unit::db },
        { "RATIO",    1.0f,    8.0f,     0.1f, false, Fill::fromLeft,   Off::none,  false, Unit::ratio },
        { "MAKEUP",   0.0f,    12.0f,    0.5f, false, Fill::fromLeft,   Off::none,  false, Unit::db },
        { "GAIN",     -24.0f,  12.0f,    0.5f, false, Fill::fromCentre, Off::none,  false, Unit::db },
    };

    static_assert (sizeof (specs) / sizeof (specs[0]) == (size_t) numParams,
                   "every parameter needs a spec");

    struct Group { const char* label; int first, count, column; };

    constexpr Group groups[] = {
        { "PASS", pHp,     2, 0 },
        { "EQ",   pLow,    4, 0 },
        { "COMP", pThresh, 3, 1 },
        { "OUT",  pGain,   1, 1 },
    };

    // 5% of the track at the bypass end. Wide enough to land on, narrow enough
    // that the useful range keeps the track.
    constexpr float deadZone = 0.05f;

    float normOf (const Spec& s, float v)
    {
        const auto scale = [&s] (float x)
        {
            x = juce::jlimit (s.min, s.max, x);
            return s.logScale ? std::log (x / s.min) / std::log (s.max / s.min)
                              : (x - s.min) / (s.max - s.min);
        };

        if (s.off == Off::atMin)
            return v <= 0.0f ? 0.0f : deadZone + (1.0f - deadZone) * scale (v);

        if (s.off == Off::atMax)
            return v <= 0.0f ? 1.0f : (1.0f - deadZone) * scale (v);

        return scale (v);
    }

    float valueOf (const Spec& s, float n)
    {
        n = juce::jlimit (0.0f, 1.0f, n);

        const auto unscale = [&s] (float x)
        {
            const auto v = s.logScale ? s.min * std::pow (s.max / s.min, x)
                                      : s.min + x * (s.max - s.min);
            return std::round (v / s.step) * s.step;
        };

        if (s.off == Off::atMin)
            return n <= deadZone ? 0.0f : unscale ((n - deadZone) / (1.0f - deadZone));

        if (s.off == Off::atMax)
            return n >= 1.0f - deadZone ? 0.0f : unscale (n / (1.0f - deadZone));

        return unscale (n);
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

            case Unit::ratio:
                return juce::String (v, 1) + ":1";

            case Unit::db:
            default:
                return juce::String (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB";
        }
    }

    float paramValue (const StemFxParams& p, int id)
    {
        switch (id)
        {
            case pHp:     return p.hpHz;
            case pLp:     return p.lpHz;
            case pLow:    return p.lowDb;
            case pMid:    return p.midDb;
            case pMidHz:  return p.midHz;
            case pHigh:   return p.highDb;
            case pThresh: return p.thresholdDb;
            case pRatio:  return p.ratio;
            case pMakeup: return p.makeupDb;
            case pGain:   return p.gainDb;
            default:      break;
        }

        return 0.0f;
    }

    void setParamValue (StemFxParams& p, int id, float v)
    {
        switch (id)
        {
            case pHp:     p.hpHz = v; break;
            case pLp:     p.lpHz = v; break;
            case pLow:    p.lowDb = v; break;
            case pMid:    p.midDb = v; break;
            case pMidHz:  p.midHz = v; break;
            case pHigh:   p.highDb = v; break;
            case pThresh: p.thresholdDb = v; break;
            case pRatio:  p.ratio = v; break;
            case pMakeup: p.makeupDb = v; break;
            case pGain:   p.gainDb = v; break;
            default:      break;
        }
    }

    // A parameter the chain is currently ignoring — the mid frequency with a
    // flat mid band, the ratio and makeup with the comp bypassed — greys out
    // rather than disappearing: it is still where it was left.
    bool paramLive (const StemFxParams& p, int id)
    {
        if (id == pMidHz)
            return std::abs (p.midDb) > 0.001f;

        if (id == pRatio || id == pMakeup)
            return p.compOn();

        return true;
    }
} // namespace

void TuneView::show (int stemIndex, const juce::String& stemName, const StemFxParams& params)
{
    stem = stemIndex;
    name = stemName;
    fx = params;

    activeParam = -1;
    hoveredParam = -1;
    hoveredAction = 0;
    repaint();
}

void TuneView::send (bool commit)
{
    if (onChange != nullptr)
        onChange (stem, fx, commit);
}

juce::Rectangle<int> TuneView::panelBounds() const
{
    return juce::Rectangle<int> (panelWidth, panelHeight).withCentre (getLocalBounds().getCentre());
}

juce::Rectangle<int> TuneView::columnsBounds() const
{
    auto body = panelBounds().reduced (panelPad);
    body.removeFromTop (titleHeight + titleGap);
    body.removeFromBottom (ui::TagButton::idealHeight + footerGap);
    return body;
}

// The column walk, run once per parameter rather than cached: ten rows is
// nothing, and paint() and the hit tests cannot drift apart this way. The
// section labels are derived from it too — each one sits directly above its
// group's first row.
juce::Rectangle<int> TuneView::rowBoundsFor (int param) const
{
    const auto body = columnsBounds();
    const auto colW = (body.getWidth() - columnGap) / 2;

    int y[2] = { body.getY(), body.getY() };

    for (const auto& grp : groups)
    {
        auto& top = y[grp.column];
        top += sectionHeight;

        for (int i = 0; i < grp.count; ++i, top += rowHeight)
            if (grp.first + i == param)
                return { body.getX() + grp.column * (colW + columnGap), top, colW, rowHeight };

        top += groupGap;
    }

    return {};
}

juce::Rectangle<int> TuneView::trackBoundsFor (int param) const
{
    const auto row = rowBoundsFor (param);

    if (row.isEmpty())
        return {};

    return { row.getX() + labelColumn + innerGap, row.getCentreY(),
             row.getWidth() - labelColumn - readoutWidth - 2 * innerGap, 1 };
}

juce::Rectangle<int> TuneView::grabBoundsFor (int param) const
{
    const auto row = rowBoundsFor (param);
    const auto track = trackBoundsFor (param);

    if (track.isEmpty())
        return {};

    return { track.getX() - grabSlack, row.getY(),
             track.getWidth() + 2 * grabSlack, row.getHeight() };
}

int TuneView::paramAt (juce::Point<int> p) const
{
    for (int id = 0; id < numParams; ++id)
        if (grabBoundsFor (id).contains (p))
            return id;

    return -1;
}

void TuneView::paintSlider (juce::Graphics& g, int id)
{
    const auto& s = specs[id];
    const auto row = rowBoundsFor (id);
    const auto track = trackBoundsFor (id);
    const auto value = paramValue (fx, id);

    const auto hot = hoveredParam == id || activeParam == id;
    const auto live = paramLive (fx, id);

    const auto markCol = ! live ? theme::text35
                         : hot  ? theme::accentSoft
                                : theme::accentMid;

    g.setFont (theme::mono (8.5f, 0.12f));
    g.setColour (hot ? theme::text : (live ? theme::text40 : theme::text35));
    g.drawText (s.label, row.withWidth (labelColumn), juce::Justification::centredLeft);

    g.setColour (theme::lineSoft15);
    g.fillRect (track);

    // The filled portion runs from the parameter's rest position to its value,
    // so an untouched slider draws no fill at all.
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
    g.drawText (readoutFor (s, value), row.withTrimmedLeft (row.getWidth() - readoutWidth),
                juce::Justification::centredRight);
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
        auto title = panel.reduced (panelPad).removeFromTop (titleHeight);

        g.setColour (theme::text);
        g.setFont (theme::mono (13.0f, 0.04f));
        g.drawText ("tune " + juce::String::fromUTF8 ("\xe2\x80\x94") + " " + name.toLowerCase(),
                    title, juce::Justification::centredLeft);

        const auto flat = fx.isFlat();

        g.setColour (flat ? theme::text35 : theme::accentSoft);
        g.setFont (theme::mono (9.5f, 0.08f));
        g.drawText (flat ? "FLAT" : "PROCESSED", title, juce::Justification::centredRight);
    }

    // --- section labels, each with a hairline out to its column edge ---
    {
        const auto font = theme::mono (8.5f, 0.12f);
        g.setFont (font);

        for (const auto& grp : groups)
        {
            const auto label = rowBoundsFor (grp.first).translated (0, -sectionHeight)
                                                       .withHeight (sectionHeight);

            g.setColour (theme::text35);
            g.drawText (grp.label, label, juce::Justification::centredLeft);

            const auto ruleX = label.getX() + (int) ui::textWidth (font, grp.label) + 8;
            g.setColour (theme::lineSoft10);
            g.fillRect (ruleX, label.getCentreY(), label.getRight() - ruleX, 1);
        }
    }

    for (int id = 0; id < numParams; ++id)
        paintSlider (g, id);

    // --- footer: the way out at the house corner, RESET opposite it ---
    auto footer = panel.reduced (panelPad).removeFromBottom (ui::TagButton::idealHeight);

    closeBounds = ui::drawTagAction (g, "CLOSE", footer.getX(), footer.getY(),
                                     hoveredAction == 1, theme::line, theme::text55);

    // RESET only exists while there is something to undo.
    resetBounds = {};

    if (! fx.isFlat())
    {
        const auto width = (int) std::ceil (ui::textWidth (theme::mono (9.5f, 0.08f), "RESET")) + 24;

        resetBounds = ui::drawTagAction (g, "RESET", footer.getRight() - width, footer.getY(),
                                         hoveredAction == 2, theme::accentTint40, theme::accentSoft);
    }
}

void TuneView::dragTo (const juce::MouseEvent& e, bool commit)
{
    if (activeParam < 0)
        return;

    const auto track = trackBoundsFor (activeParam);
    const auto span = juce::jmax (1, track.getWidth() - 1);
    const auto n = juce::jlimit (0.0f, 1.0f, (float) (e.x - track.getX()) / (float) span);

    setParamValue (fx, activeParam, valueOf (specs[activeParam], n));
    send (commit);
    repaint();
}

void TuneView::mouseDown (const juce::MouseEvent& e)
{
    activeParam = paramAt (e.getPosition());

    if (activeParam >= 0)
    {
        activeStart = paramValue (fx, activeParam);
        dragTo (e, false); // live preview; the bounce waits for the release
    }
}

void TuneView::mouseDrag (const juce::MouseEvent& e)
{
    if (activeParam >= 0)
        dragTo (e, false);
}

void TuneView::mouseUp (const juce::MouseEvent& e)
{
    if (activeParam >= 0)
    {
        // Only a gesture that actually moved the slider commits: a commit is
        // what makes the editor re-bounce, and a press that landed back on the
        // value it started from has nothing to re-bounce.
        const auto moved = ! juce::approximatelyEqual (paramValue (fx, activeParam), activeStart);

        activeParam = -1;

        if (moved)
            send (true); // commit -> what drag-out and export will carry

        repaint();
        return;
    }

    const auto p = e.getPosition();

    if (! resetBounds.isEmpty() && resetBounds.contains (p))
    {
        fx = {};
        send (true);
        repaint();
        return;
    }

    // CLOSE, or a click on the scrim outside the panel.
    if ((closeBounds.contains (p) || ! panelBounds().contains (p)) && onClose != nullptr)
        onClose();
}

void TuneView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto id = paramAt (e.getPosition());

    if (id < 0)
        return;

    // JUCE delivers this after the second mouse-up, which has already moved
    // the slider: this puts that one parameter back to its default and
    // commits, which is the state the gesture ends in either way.
    const StemFxParams flat;
    const auto def = paramValue (flat, id);

    activeParam = -1;

    if (juce::approximatelyEqual (paramValue (fx, id), def))
        return;

    setParamValue (fx, id, def);
    send (true);
    repaint();
}

void TuneView::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    const auto param = paramAt (p);
    const auto action = closeBounds.contains (p)                                ? 1
                        : (! resetBounds.isEmpty() && resetBounds.contains (p)) ? 2
                                                                                : 0;

    setMouseCursor (param >= 0 || action != 0 ? juce::MouseCursor::PointingHandCursor
                                              : juce::MouseCursor::NormalCursor);

    if (param == hoveredParam && action == hoveredAction)
        return;

    hoveredParam = param;
    hoveredAction = action;
    repaint();
}

void TuneView::mouseExit (const juce::MouseEvent&)
{
    if (hoveredParam >= 0 || hoveredAction != 0)
    {
        hoveredParam = -1;
        hoveredAction = 0;
        repaint();
    }
}

} // namespace splittr
