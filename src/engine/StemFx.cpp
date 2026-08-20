#include "StemFx.h"

#include <cmath>
#include <limits>

namespace splittr
{

namespace
{
    // How far from neutral a control has to be before it counts as on. isFlat(),
    // the descriptor and the chain's band switches all read the same tolerance,
    // so what is heard, what is written and what is processed cannot disagree.
    constexpr float dbEpsilon = 0.05f;

    bool gainOn (float db) { return std::abs (db) > dbEpsilon; }
    bool passOn (float hz) { return hz > 1.0f; } // v1's passes: 0 = off

    // Long enough to swallow a slider step, short enough that the gain still
    // feels like it is under the mouse. A polarity flip rides the same ramp,
    // which turns a click into a 20 ms fade through zero.
    constexpr double gainRampSeconds = 0.02;

    // The v1 strip's one Q, which is also what a fresh band defaults to.
    constexpr float eqQ = 0.7071f;

    // flatTilt hinges here — the geometric centre of 20 Hz .. 20 kHz — so the
    // tilt is symmetric on the log axis the UI draws it against, and gentle
    // enough that the hinge is a slope rather than a bump.
    constexpr double flatTiltHz = 632.5;
    constexpr double flatTiltQ = 0.4;

    // Auto gain is a level match, not a rescue: past this the average of a
    // steep cut runs away and the "compensation" becomes the loudest thing in
    // the session. Capped rather than clever, and deliberately so.
    constexpr double autoGainLimitDb = 12.0;

    // Below this the ride is not worth the trig it would cost to rebuild.
    constexpr double rideRetuneDb = 0.01;

    bool dynamicShape (EqShape s)
    {
        return s == EqShape::bell || s == EqShape::lowShelf || s == EqShape::highShelf;
    }

    // Dynamics are for the shapes that have a gain to ride; a cut or a notch
    // has nothing to move, so those bands cost nothing extra however the
    // dynamic fields are set.
    bool dynamicBand (const EqBand& b)
    {
        return b.enabled && dynamicShape (b.shape) && std::abs (b.dynRangeDb) > dbEpsilon;
    }

    // A band that does nothing is not run at unity, it is not run at all —
    // the v1 rule, one band list further on. A dynamic band counts as active
    // even at 0 dB static: the ride is what it is there for.
    bool bandActive (const EqBand& b)
    {
        if (! b.enabled)
            return false;

        switch (b.shape)
        {
            case EqShape::bell:
            case EqShape::lowShelf:
            case EqShape::highShelf:
                return gainOn (b.gainDb) || dynamicBand (b);

            case EqShape::tiltShelf:
            case EqShape::flatTilt:
                return gainOn (b.gainDb);

            case EqShape::lowCut:
            case EqShape::highCut:
            case EqShape::notch:
            case EqShape::bandPass:
            case EqShape::allPass:
            default:
                return true; // shapes with no gain to be neutral at
        }
    }

    // Keep every corner inside the band the running rate can actually
    // represent, and every Q inside what a biquad stays stable at.
    double safeHz (double hz, double rate) { return juce::jlimit (20.0, rate * 0.45, hz); }
    double safeQ (double q) { return juce::jlimit (0.05, 40.0, q); }

    int cutStages (int slopeDbOct)
    {
        // 12/24/36/48/72/96 are the offered slopes; anything else rounds to the
        // nearest whole 12 dB stage rather than being refused.
        return juce::jlimit (1, maxEqStagesPerBand, (int) std::lround (slopeDbOct / 12.0));
    }

    //==========================================================================
    // RBJ cookbook sections, stored normalised to a0 = 1. The peak and the
    // shelves are the same formulas juce::dsp::IIR::ArrayCoefficients uses, so
    // a v1 descriptor migrated onto bands lands on identical coefficients.

    EqStage normalised (double b0, double b1, double b2, double a0, double a1, double a2)
    {
        const auto inv = std::abs (a0) > 1.0e-20 ? 1.0 / a0 : 1.0;
        return { b0 * inv, b1 * inv, b2 * inv, a1 * inv, a2 * inv };
    }

    struct Trig
    {
        double cosw, sinw, alpha;
    };

    Trig trigFor (double hz, double q, double rate)
    {
        const auto w = juce::MathConstants<double>::twoPi * safeHz (hz, rate) / rate;
        const auto sinw = std::sin (w);
        return { std::cos (w), sinw, sinw / (2.0 * safeQ (q)) };
    }

    double gainA (double db) { return std::pow (10.0, juce::jlimit (-40.0, 40.0, db) / 40.0); }

    EqStage makeBell (double hz, double q, double db, double rate)
    {
        const auto t = trigFor (hz, q, rate);
        const auto a = gainA (db);
        return normalised (1.0 + t.alpha * a, -2.0 * t.cosw, 1.0 - t.alpha * a,
                           1.0 + t.alpha / a, -2.0 * t.cosw, 1.0 - t.alpha / a);
    }

    EqStage makeLowShelf (double hz, double q, double db, double rate)
    {
        const auto t = trigFor (hz, q, rate);
        const auto a = gainA (db);
        const auto beta = 2.0 * std::sqrt (a) * t.alpha;

        return normalised (a * ((a + 1.0) - (a - 1.0) * t.cosw + beta),
                           2.0 * a * ((a - 1.0) - (a + 1.0) * t.cosw),
                           a * ((a + 1.0) - (a - 1.0) * t.cosw - beta),
                           (a + 1.0) + (a - 1.0) * t.cosw + beta,
                           -2.0 * ((a - 1.0) + (a + 1.0) * t.cosw),
                           (a + 1.0) + (a - 1.0) * t.cosw - beta);
    }

    EqStage makeHighShelf (double hz, double q, double db, double rate)
    {
        const auto t = trigFor (hz, q, rate);
        const auto a = gainA (db);
        const auto beta = 2.0 * std::sqrt (a) * t.alpha;

        return normalised (a * ((a + 1.0) + (a - 1.0) * t.cosw + beta),
                           -2.0 * a * ((a - 1.0) + (a + 1.0) * t.cosw),
                           a * ((a + 1.0) + (a - 1.0) * t.cosw - beta),
                           (a + 1.0) - (a - 1.0) * t.cosw + beta,
                           2.0 * ((a - 1.0) - (a + 1.0) * t.cosw),
                           (a + 1.0) - (a - 1.0) * t.cosw - beta);
    }

    EqStage makeHighPass (double hz, double q, double rate)
    {
        const auto t = trigFor (hz, q, rate);
        return normalised ((1.0 + t.cosw) * 0.5, -(1.0 + t.cosw), (1.0 + t.cosw) * 0.5,
                           1.0 + t.alpha, -2.0 * t.cosw, 1.0 - t.alpha);
    }

    EqStage makeLowPass (double hz, double q, double rate)
    {
        const auto t = trigFor (hz, q, rate);
        return normalised ((1.0 - t.cosw) * 0.5, 1.0 - t.cosw, (1.0 - t.cosw) * 0.5,
                           1.0 + t.alpha, -2.0 * t.cosw, 1.0 - t.alpha);
    }

    EqStage makeNotch (double hz, double q, double rate)
    {
        const auto t = trigFor (hz, q, rate);
        return normalised (1.0, -2.0 * t.cosw, 1.0,
                           1.0 + t.alpha, -2.0 * t.cosw, 1.0 - t.alpha);
    }

    // Constant 0 dB at the centre, so a band-pass reads as a window on the
    // material rather than a level change — which is what solo needs too.
    EqStage makeBandPass (double hz, double q, double rate)
    {
        const auto t = trigFor (hz, q, rate);
        return normalised (t.alpha, 0.0, -t.alpha,
                           1.0 + t.alpha, -2.0 * t.cosw, 1.0 - t.alpha);
    }

    EqStage makeAllPass (double hz, double q, double rate)
    {
        const auto t = trigFor (hz, q, rate);
        return normalised (1.0 - t.alpha, -2.0 * t.cosw, 1.0 + t.alpha,
                           1.0 + t.alpha, -2.0 * t.cosw, 1.0 - t.alpha);
    }

    //==========================================================================

    // One band as sections. `extraGainDb` is the dynamic ride: 0 for the static
    // curve, whatever the follower is asking for while a band is riding.
    int buildStages (const EqBand& b, double rate, double extraGainDb, EqStage* out)
    {
        if (! bandActive (b))
            return 0;

        const auto hz = (double) b.freqHz;
        const auto q = (double) b.q;
        const auto db = (double) b.gainDb + extraGainDb;

        switch (b.shape)
        {
            case EqShape::bell:
                out[0] = makeBell (hz, q, db, rate);
                return 1;

            case EqShape::lowShelf:
                out[0] = makeLowShelf (hz, q, db, rate);
                return 1;

            case EqShape::highShelf:
                out[0] = makeHighShelf (hz, q, db, rate);
                return 1;

            case EqShape::lowCut:
            {
                // N identical 12 dB sections rather than a Butterworth split:
                // the slope is what is being asked for, and stacking the same
                // section keeps the band's Q meaning "how it turns the corner"
                // at every slope instead of only at the shallowest.
                const auto n = cutStages (b.slopeDbOct);

                for (int i = 0; i < n; ++i)
                    out[i] = makeHighPass (hz, q, rate);

                return n;
            }

            case EqShape::highCut:
            {
                const auto n = cutStages (b.slopeDbOct);

                for (int i = 0; i < n; ++i)
                    out[i] = makeLowPass (hz, q, rate);

                return n;
            }

            case EqShape::notch:
                out[0] = makeNotch (hz, q, rate);
                return 1;

            case EqShape::bandPass:
                out[0] = makeBandPass (hz, q, rate);
                return 1;

            case EqShape::allPass:
                out[0] = makeAllPass (hz, q, rate);
                return 1;

            case EqShape::tiltShelf:
                // Hinged at the band's own frequency: the highs go up by the
                // band's gain, the lows mirror it down, so the hinge stays put
                // while the tilt swings around it.
                out[0] = makeLowShelf (hz, q, -db, rate);
                out[1] = makeHighShelf (hz, q, db, rate);
                return 2;

            case EqShape::flatTilt:
                // The same tilt pinned to the spectrum's centre with a gentle
                // Q, so gainDb reads as the whole swing from 20 Hz to 20 kHz:
                // half of it down at the bottom, half of it up at the top.
                out[0] = makeLowShelf (flatTiltHz, flatTiltQ, -db * 0.5, rate);
                out[1] = makeHighShelf (flatTiltHz, flatTiltQ, db * 0.5, rate);
                return 2;
        }

        return 0;
    }

    // The listen filter behind solo: the region a band works on, with the band
    // itself out of the picture. Solo answers "what is under this node", not
    // "what did this node do to it" — for a cut the second question has the
    // answer "nothing", which is exactly what you cannot listen to.
    int buildListenStages (const EqBand& b, double rate, EqStage* out)
    {
        const auto hz = (double) b.freqHz;
        const auto q = (double) b.q;

        switch (b.shape)
        {
            case EqShape::bell:
            case EqShape::notch:
            case EqShape::bandPass:
            case EqShape::allPass:
                // Two sections, not one: a single band-pass leaks 6 dB/oct in
                // both directions and solo then sounds like the whole stem
                // with a tilt on it.
                out[0] = makeBandPass (hz, q, rate);
                out[1] = makeBandPass (hz, q, rate);
                return 2;

            case EqShape::lowShelf:
                out[0] = makeLowPass (hz, q, rate);
                out[1] = makeLowPass (hz, q, rate);
                return 2;

            case EqShape::highShelf:
                out[0] = makeHighPass (hz, q, rate);
                out[1] = makeHighPass (hz, q, rate);
                return 2;

            case EqShape::lowCut:
            {
                // The complement, at the cut's own slope: what the band throws
                // away is what there is to hear.
                const auto n = cutStages (b.slopeDbOct);

                for (int i = 0; i < n; ++i)
                    out[i] = makeLowPass (hz, q, rate);

                return n;
            }

            case EqShape::highCut:
            {
                const auto n = cutStages (b.slopeDbOct);

                for (int i = 0; i < n; ++i)
                    out[i] = makeHighPass (hz, q, rate);

                return n;
            }

            case EqShape::tiltShelf:
            case EqShape::flatTilt:
                return 0; // the region is the whole spectrum: nothing to isolate
        }

        return 0;
    }

    //==========================================================================

    bool sameFloat (float a, float b) { return juce::approximatelyEqual (a, b); }

    // Two decimals, trailing zeros dropped — "90", "2.5", "-18".
    juce::String descriptorNumber (float value, int decimals = 2)
    {
        auto text = juce::String (value, decimals);

        if (text.containsChar ('.'))
            text = text.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");

        return text;
    }

    // Q gets a third decimal: 0.7071 rounded to "0.71" would move every
    // default band a little on the first save, and a descriptor is supposed to
    // be a record of what was heard.
    juce::String descriptorQ (float q) { return descriptorNumber (q, 3); }

    bool numericField (const juce::String& field)
    {
        const auto c = field.trim()[0];
        return c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9');
    }

    // What a descriptor is allowed to ask for. A file that says otherwise is
    // damaged or from a build that moved the ranges; either way the chain gets
    // something it can run rather than something it has to survive.
    void sanitise (EqBand& b)
    {
        b.freqHz = juce::jlimit (20.0f, 20000.0f, b.freqHz);
        b.gainDb = juce::jlimit (-30.0f, 30.0f, b.gainDb);
        b.q = juce::jlimit (0.1f, 18.0f, b.q);
        b.slopeDbOct = juce::jlimit (12, 96, b.slopeDbOct);
        b.dynRangeDb = juce::jlimit (-24.0f, 24.0f, b.dynRangeDb);
        b.dynThresholdDb = juce::jlimit (-60.0f, 0.0f, b.dynThresholdDb);
        b.dynAttackMs = juce::jlimit (1.0f, 250.0f, b.dynAttackMs);
        b.dynReleaseMs = juce::jlimit (10.0f, 1000.0f, b.dynReleaseMs);
    }
} // namespace

//==============================================================================

bool eqBandsEqual (const EqBand& a, const EqBand& b)
{
    return a.shape == b.shape
           && sameFloat (a.freqHz, b.freqHz) && sameFloat (a.gainDb, b.gainDb)
           && sameFloat (a.q, b.q) && a.slopeDbOct == b.slopeDbOct
           && a.enabled == b.enabled && a.solo == b.solo
           && sameFloat (a.dynRangeDb, b.dynRangeDb)
           && sameFloat (a.dynThresholdDb, b.dynThresholdDb)
           && sameFloat (a.dynAttackMs, b.dynAttackMs)
           && sameFloat (a.dynReleaseMs, b.dynReleaseMs);
}

bool EqBandList::operator== (const EqBandList& o) const
{
    if (count != o.count)
        return false;

    for (int i = 0; i < count; ++i)
        if (! eqBandsEqual (items[(size_t) i], o.items[(size_t) i]))
            return false;

    return true;
}

//==============================================================================

int eqBandStages (const EqBand& b, double sampleRate, EqStage* out)
{
    return buildStages (b, sampleRate > 0.0 ? sampleRate : 44100.0, 0.0, out);
}

double eqStageMagnitudeDb (const EqStage& s, double hz, double sampleRate)
{
    const auto rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    const auto w = juce::MathConstants<double>::twoPi
                   * juce::jlimit (1.0, rate * 0.4999, hz) / rate;

    // H(e^jw) with z^-1 = cos w - j sin w; the magnitudes squared go straight
    // into 10*log10, which is 20*log10 of the magnitude without the sqrt.
    const auto c1 = std::cos (w), s1 = std::sin (w);
    const auto c2 = std::cos (2.0 * w), s2 = std::sin (2.0 * w);

    const auto nr = s.b0 + s.b1 * c1 + s.b2 * c2;
    const auto ni = -(s.b1 * s1 + s.b2 * s2);
    const auto dr = 1.0 + s.a1 * c1 + s.a2 * c2;
    const auto di = -(s.a1 * s1 + s.a2 * s2);

    return 10.0 * std::log10 (juce::jmax (nr * nr + ni * ni, 1.0e-30)
                              / juce::jmax (dr * dr + di * di, 1.0e-30));
}

double eqBandMagnitudeDb (const EqBand& b, double hz, double sampleRate)
{
    EqStage stages[maxEqStagesPerBand];
    const auto n = eqBandStages (b, sampleRate, stages);

    auto db = 0.0;

    for (int i = 0; i < n; ++i)
        db += eqStageMagnitudeDb (stages[i], hz, sampleRate);

    return db;
}

double eqMagnitudeDb (const StemFxParams& p, double hz, double sampleRate)
{
    auto db = 0.0;

    for (const auto& b : p.bands)
        db += eqBandMagnitudeDb (b, hz, sampleRate);

    return db;
}

//==============================================================================

bool StemFxParams::isFlat() const
{
    for (const auto& b : bands)
        if (bandActive (b))
            return false;

    return ! compOn() && ! gainOn (gainDb) && ! phaseInvert;
}

bool StemFxParams::operator== (const StemFxParams& o) const
{
    return bands == o.bands
           && sameFloat (thresholdDb, o.thresholdDb) && sameFloat (ratio, o.ratio)
           && sameFloat (makeupDb, o.makeupDb) && sameFloat (gainDb, o.gainDb)
           && phaseInvert == o.phaseInvert && autoGain == o.autoGain;
}

juce::String StemFxParams::toDescriptor() const
{
    if (isFlat())
        return {};

    juce::StringArray parts;

    for (const auto& b : bands)
    {
        juce::StringArray fields;

        const auto addFreq = [&fields, &b] { fields.add (descriptorNumber (b.freqHz)); };
        const auto addGain = [&fields, &b] { fields.add (descriptorNumber (b.gainDb)); };
        const auto addQ = [&fields, &b] { fields.add (descriptorQ (b.q)); };

        switch (b.shape)
        {
            case EqShape::bell:      fields.add ("bell"); addFreq(); addGain(); addQ(); break;
            case EqShape::lowShelf:  fields.add ("ls");   addFreq(); addGain(); addQ(); break;
            case EqShape::highShelf: fields.add ("hs");   addFreq(); addGain(); addQ(); break;
            case EqShape::tiltShelf: fields.add ("tilt"); addFreq(); addGain(); addQ(); break;
            case EqShape::notch:     fields.add ("no");   addFreq(); addQ(); break;
            case EqShape::bandPass:  fields.add ("bp");   addFreq(); addQ(); break;
            case EqShape::allPass:   fields.add ("ap");   addFreq(); addQ(); break;
            case EqShape::flatTilt:  fields.add ("flat"); addGain(); break;

            case EqShape::lowCut:
            case EqShape::highCut:
                // The slope rides on the key rather than in a field: it is what
                // the shape *is*, and "lc24" reads at a glance.
                fields.add ((b.shape == EqShape::lowCut ? "lc" : "hc")
                            + juce::String (cutStages (b.slopeDbOct) * 12));
                addFreq();
                addQ();
                break;
        }

        if (fields.isEmpty())
            continue;

        if (! b.enabled)
            fields.add ("off");

        // Solo is a listen mode, not a setting: writing it would make a
        // perfectly good bounce look stale the moment someone auditioned a band.
        if (dynamicShape (b.shape) && std::abs (b.dynRangeDb) > dbEpsilon)
            fields.add ("d" + descriptorNumber (b.dynRangeDb)
                        + "/" + descriptorNumber (b.dynThresholdDb)
                        + "/" + descriptorNumber (b.dynAttackMs)
                        + "/" + descriptorNumber (b.dynReleaseMs));

        parts.add ("b:" + fields.joinIntoString (","));
    }

    const auto add = [&parts] (const char* key, float value)
    {
        parts.add (juce::String (key) + ":" + descriptorNumber (value));
    };

    if (compOn())
    {
        add ("th", thresholdDb);
        add ("ra", ratio);

        if (gainOn (makeupDb))
            add ("mk", makeupDb);
    }

    if (phaseInvert) parts.add ("inv:1");
    if (autoGain)    parts.add ("ag:1");

    if (gainOn (gainDb)) add ("g", gainDb);

    return parts.joinIntoString (";");
}

StemFxParams StemFxParams::fromDescriptor (const juce::String& descriptor)
{
    StemFxParams p;

    // The v1 strip, collected as it arrives and turned into bands at the end:
    // "mid" needs "mhz", and a descriptor makes no promise about token order.
    struct
    {
        float hpHz = 0.0f, lpHz = 0.0f;
        float lowDb = 0.0f, midDb = 0.0f, midHz = 800.0f, highDb = 0.0f;
        bool seen = false;
    } v1;

    for (const auto& token : juce::StringArray::fromTokens (descriptor, ";", {}))
    {
        const auto key = token.upToFirstOccurrenceOf (":", false, false).trim();
        const auto body = token.fromFirstOccurrenceOf (":", false, false).trim();
        const auto value = body.getFloatValue();

        if (key == "b")
        {
            auto fields = juce::StringArray::fromTokens (body, ",", {});
            fields.trim();

            if (fields.isEmpty())
                continue;

            const auto shapeKey = fields[0].toLowerCase();
            EqBand band;
            auto positional = 0; // how many numbers this shape reads

            const auto isCut = shapeKey.startsWith ("lc") || shapeKey.startsWith ("hc");

            if (shapeKey == "bell")      { band.shape = EqShape::bell;      positional = 3; }
            else if (shapeKey == "ls")   { band.shape = EqShape::lowShelf;  positional = 3; }
            else if (shapeKey == "hs")   { band.shape = EqShape::highShelf; positional = 3; }
            else if (shapeKey == "tilt") { band.shape = EqShape::tiltShelf; positional = 3; }
            else if (shapeKey == "no")   { band.shape = EqShape::notch;     positional = 2; }
            else if (shapeKey == "bp")   { band.shape = EqShape::bandPass;  positional = 2; }
            else if (shapeKey == "ap")   { band.shape = EqShape::allPass;   positional = 2; }
            else if (shapeKey == "flat") { band.shape = EqShape::flatTilt;  positional = 1; }
            else if (isCut)
            {
                band.shape = shapeKey.startsWith ("lc") ? EqShape::lowCut : EqShape::highCut;
                band.slopeDbOct = shapeKey.substring (2).getIntValue();
                positional = 2;
            }
            else
            {
                continue; // a shape this build does not know: dropped, not guessed
            }

            // Positional fields run until one stops looking like a number, so a
            // band at its defaults can simply stop writing them ("b:lc24,80")
            // and the flags that follow are never mistaken for values.
            auto next = 1;

            const auto number = [&fields, &next, positional] () -> float
            {
                if (next < fields.size() && next <= positional && numericField (fields[next]))
                    return fields[next++].getFloatValue();

                return std::numeric_limits<float>::quiet_NaN();
            };

            const auto readInto = [&number] (float& target)
            {
                const auto v = number();

                if (! std::isnan (v))
                    target = v;
            };

            if (band.shape == EqShape::flatTilt)
            {
                readInto (band.gainDb);
            }
            else
            {
                readInto (band.freqHz);

                if (positional == 3)
                    readInto (band.gainDb);

                readInto (band.q);
            }

            for (int i = next; i < fields.size(); ++i)
            {
                const auto flag = fields[i].toLowerCase();

                if (flag == "off")
                {
                    band.enabled = false;
                }
                else if (flag.startsWithChar ('d'))
                {
                    const auto dyn = juce::StringArray::fromTokens (flag.substring (1), "/", {});

                    if (dyn.size() > 0) band.dynRangeDb = dyn[0].getFloatValue();
                    if (dyn.size() > 1) band.dynThresholdDb = dyn[1].getFloatValue();
                    if (dyn.size() > 2) band.dynAttackMs = dyn[2].getFloatValue();
                    if (dyn.size() > 3) band.dynReleaseMs = dyn[3].getFloatValue();
                }
                // Anything else is a flag from a later build: ignored, like v1.
            }

            sanitise (band);
            p.bands.push_back (band); // silently full at 24: the UI's own ceiling
        }
        // The v1 strip (issue #9). Its keys are disjoint from v2's, so one
        // reader takes both and every stem tuned before this rework reopens
        // sounding the same.
        else if (key == "hp")   { v1.hpHz = value;   v1.seen = true; }
        else if (key == "lp")   { v1.lpHz = value;   v1.seen = true; }
        else if (key == "low")  { v1.lowDb = value;  v1.seen = true; }
        else if (key == "mid")  { v1.midDb = value;  v1.seen = true; }
        else if (key == "mhz")  { v1.midHz = value;  v1.seen = true; }
        else if (key == "high") { v1.highDb = value; v1.seen = true; }
        // Shared with v1, unchanged.
        else if (key == "th")   p.thresholdDb = value;
        else if (key == "ra")   p.ratio = value;
        else if (key == "mk")   p.makeupDb = value;
        else if (key == "g")    p.gainDb = value;
        else if (key == "inv")  p.phaseInvert = value > 0.5f;
        else if (key == "ag")   p.autoGain = value > 0.5f;
        // Anything unrecognised is left out rather than guessed at, so a
        // descriptor from a future build degrades to the part this one knows.
    }

    if (v1.seen)
    {
        // v1's chain order, onto the shapes that reproduce it exactly: its
        // passes were 12 dB/oct Butterworth, its shelves sat at fixed corners
        // and everything shared one Q.
        const auto addBand = [&p] (EqShape shape, float hz, float db)
        {
            EqBand b;
            b.shape = shape;
            b.freqHz = hz;
            b.gainDb = db;
            b.q = eqQ;
            b.slopeDbOct = 12;
            sanitise (b);
            p.bands.push_back (b);
        };

        if (passOn (v1.hpHz)) addBand (EqShape::lowCut, v1.hpHz, 0.0f);
        if (passOn (v1.lpHz)) addBand (EqShape::highCut, v1.lpHz, 0.0f);
        if (gainOn (v1.lowDb)) addBand (EqShape::lowShelf, lowShelfHz, v1.lowDb);
        if (gainOn (v1.midDb)) addBand (EqShape::bell, v1.midHz, v1.midDb);
        if (gainOn (v1.highDb)) addBand (EqShape::highShelf, highShelfHz, v1.highDb);
    }

    return p;
}

//==============================================================================

void StemFxChain::Section::process (float* data, int channel, int numSamples)
{
    // Transposed direct form II, unrolled into locals so the state only makes
    // one trip to memory per block.
    auto s1 = z1[channel], s2 = z2[channel];

    for (int i = 0; i < numSamples; ++i)
    {
        const auto x = (double) data[i];
        const auto y = c.b0 * x + s1;

        s1 = c.b1 * x - c.a1 * y + s2;
        s2 = c.b2 * x - c.a2 * y;

        data[i] = (float) y;
    }

    z1[channel] = s1;
    z2[channel] = s2;
}

void StemFxChain::prepare (double sampleRate, int maxBlockSamples)
{
    rate = sampleRate > 0.0 ? sampleRate : 44100.0;

    const juce::dsp::ProcessSpec spec { rate,
                                        (juce::uint32) juce::jmax (1, maxBlockSamples),
                                        (juce::uint32) 2 };

    comp.prepare (spec);
    comp.setAttack (StemFxParams::attackMs);
    comp.setRelease (StemFxParams::releaseMs);

    outGain.reset (rate, gainRampSeconds);
    outGain.setCurrentAndTargetValue (1.0f);

    // Auto gain averages the curve over a fixed log-frequency grid. The trig
    // only depends on the rate, so it is computed here and setParams does
    // nothing but multiply-add — which is what keeps a 24-band retune cheap
    // enough to do on the audio thread.
    const auto lowHz = 20.0;
    const auto highHz = juce::jmin (20000.0, rate * 0.45);
    const auto ratioPerStep = std::pow (highHz / lowHz, 1.0 / (autoGainPoints - 1));

    auto hz = lowHz;

    for (int k = 0; k < autoGainPoints; ++k, hz *= ratioPerStep)
    {
        const auto w = juce::MathConstants<double>::twoPi * hz / rate;
        agCosW[(size_t) k] = std::cos (w);
        agSinW[(size_t) k] = std::sin (w);
        agCos2W[(size_t) k] = std::cos (2.0 * w);
        agSin2W[(size_t) k] = std::sin (2.0 * w);
    }

    for (auto& s : section)
        s.clear();

    for (auto& s : soloSection)
        s.clear();

    prepared = true;

    // The params outlive a rate change, so the chain has to re-derive every
    // coefficient at the new rate: clearing `current` first is what makes
    // setParams see a change rather than early out.
    const auto p = current;
    current = StemFxParams {};
    setParams (p);

    outGain.setCurrentAndTargetValue (outGain.getTargetValue()); // fresh start: no ramp
}

void StemFxChain::reset()
{
    for (auto& s : section)
        s.clear();

    for (auto& s : soloSection)
        s.clear();

    // A ride that survived a seek would open the next block on whatever the
    // last one was doing, which is the one thing a fresh start must not do.
    for (int i = 0; i < planCount; ++i)
    {
        auto& pl = plan[(size_t) i];
        pl.scZ1 = pl.scZ2 = 0.0;
        pl.rideDb = 0.0;

        if (pl.dynamic)
            tuneBand (pl, 0.0f);
    }

    comp.reset();
    outGain.setCurrentAndTargetValue (outGain.getTargetValue());
}

void StemFxChain::setParams (const StemFxParams& p)
{
    if (p == current)
        return;

    const auto previous = current;
    current = p;

    if (! prepared)
        return;

    rebuild();
    rebuildSolo();

    if (p.compOn())
    {
        if (! previous.compOn())
            comp.reset(); // same reasoning as the bands: no envelope from last time

        comp.setThreshold (p.thresholdDb);
        comp.setRatio (juce::jmax (1.0f, p.ratio));
    }

    // Makeup is part of the compressor, so it only counts while the compressor
    // does; auto gain is part of the curve and only counts while there is one.
    // All of it rides the same smoother, which is what keeps a slider drag from
    // stepping the level — and what turns a polarity flip into a fade through
    // zero rather than a click.
    const auto totalDb = (p.compOn() ? p.makeupDb : 0.0f) + p.gainDb
                         + (float) (p.autoGain ? staticCurveOffsetDb() : 0.0);

    const auto sign = p.phaseInvert ? -1.0f : 1.0f;
    outGain.setTargetValue (sign * juce::Decibels::decibelsToGain (totalDb));
}

void StemFxChain::rebuild()
{
    // The plan as it stands, so a band that is still the same band keeps its
    // filter state: a drag has to retune under a running signal without
    // clicking, and only a band that arrived, left or changed shape starts
    // empty (the v1 rule, per band instead of per strip).
    std::array<BandPlan, maxEqBands> previous = plan;
    const auto previousCount = planCount;

    planCount = 0;
    dynamicCount = 0;
    activeSections = 0;

    // Dynamics retune once per control block, so the block is what quantises
    // the attack. 32 samples is 0.7 ms at 44.1k — inaudible against a 15 ms
    // attack, but most of a 1 ms one, so a fast band shortens it.
    auto fastestTauSeconds = 1.0;

    for (const auto& b : current.bands)
        if (dynamicBand (b))
            fastestTauSeconds = juce::jmin (fastestTauSeconds, (double) b.dynAttackMs * 0.001);

    controlSamples = juce::jlimit (4, 32, (int) (fastestTauSeconds * rate / 8.0));

    EqStage stages[maxEqStagesPerBand];

    for (int i = 0; i < current.bands.size(); ++i)
    {
        const auto& band = current.bands[i];
        const auto n = buildStages (band, rate, 0.0, stages);

        if (n == 0 || activeSections + n > maxEqStages)
            continue;

        auto& pl = plan[(size_t) planCount];
        const auto slot = planCount;

        pl = BandPlan {};
        pl.bandIndex = i;
        pl.shape = band.shape;
        pl.firstStage = activeSections;
        pl.numStages = n;
        pl.dynamic = dynamicBand (band);

        for (int s = 0; s < n; ++s)
            section[(size_t) (pl.firstStage + s)].c = stages[s];

        // Same band, same shape, same place in the cascade: keep what it was
        // running on. Anything else and the sections start from silence.
        const auto& was = previous[(size_t) slot];
        const auto continues = slot < previousCount
                               && was.bandIndex == pl.bandIndex && was.shape == pl.shape
                               && was.firstStage == pl.firstStage && was.numStages == pl.numStages;

        if (continues)
        {
            pl.scZ1 = was.scZ1;
            pl.scZ2 = was.scZ2;
            pl.rideDb = was.rideDb;
        }
        else
        {
            for (int s = 0; s < n; ++s)
                section[(size_t) (pl.firstStage + s)].clear();
        }

        if (pl.dynamic)
        {
            // The sidechain listens at the band's own frequency and Q, off the
            // chain input: a band should react to what the stem is doing, not
            // to what the bands before it already did about it.
            pl.sidechain = makeBandPass (band.freqHz, band.q, rate);

            const auto coefFor = [this] (double ms)
            {
                return 1.0 - std::exp (-controlSamples / (juce::jmax (0.1, ms) * 0.001 * rate));
            };

            pl.attackCoef = coefFor (band.dynAttackMs);
            pl.releaseCoef = coefFor (band.dynReleaseMs);
            ++dynamicCount;
        }

        // Whatever the ride was, the sections above were just built static, so
        // the next control block has to see them as needing a retune.
        pl.tunedRideDb = 0.0;

        activeSections += n;
        ++planCount;
    }
}

void StemFxChain::rebuildSolo()
{
    // A chain only listens when it was told to. soloFromParams is the audition
    // asking to follow the flags the UI publishes; a plain index is a caller
    // driving it directly; anything else — including every chain the bounce
    // builds — never solos at all.
    auto index = soloOverride;

    if (soloOverride == soloFromParams)
    {
        index = -1;

        for (int i = 0; i < current.bands.size(); ++i)
            if (current.bands[i].solo)
            {
                index = i;
                break;
            }
    }

    const auto valid = juce::isPositiveAndBelow (index, current.bands.size());

    EqStage stages[maxEqStagesPerBand];
    const auto sections = valid ? buildListenStages (current.bands[index], rate, stages) : 0;

    // A different band is a different signal: only a solo that stayed on the
    // same node keeps its filter state (its frequency may be under the mouse).
    if (index != soloIndex)
        for (auto& s : soloSection)
            s.clear();

    for (int i = 0; i < sections; ++i)
        soloSection[(size_t) i].c = stages[i];

    soloIndex = valid ? index : -1;
    soloSections = sections;
    soloActive = sections > 0;
}

void StemFxChain::tuneBand (BandPlan& pl, float extraGainDb)
{
    EqStage stages[maxEqStagesPerBand];
    const auto n = buildStages (current.bands[pl.bandIndex], rate, extraGainDb, stages);

    for (int s = 0; s < juce::jmin (n, pl.numStages); ++s)
        section[(size_t) (pl.firstStage + s)].c = stages[s];

    pl.tunedRideDb = extraGainDb;
}

void StemFxChain::updateDynamics (const float* left, const float* right, int numSamples)
{
    for (int i = 0; i < planCount; ++i)
    {
        auto& pl = plan[(size_t) i];

        if (! pl.dynamic)
            continue;

        const auto& band = current.bands[pl.bandIndex];

        // The detector: the control block's peak through the band's own
        // band-pass, in dB. One number per block is all the ride needs — the
        // smoothing that follows is what the attack and release settings mean.
        auto peak = 0.0;
        auto s1 = pl.scZ1, s2 = pl.scZ2;
        const auto& c = pl.sidechain;

        for (int n = 0; n < numSamples; ++n)
        {
            const auto x = 0.5 * ((double) left[n] + (double) right[n]);
            const auto y = c.b0 * x + s1;

            s1 = c.b1 * x - c.a1 * y + s2;
            s2 = c.b2 * x - c.a2 * y;

            peak = juce::jmax (peak, std::abs (y));
        }

        pl.scZ1 = s1;
        pl.scZ2 = s2;

        const auto peakDb = peak > 1.0e-7 ? 20.0 * std::log10 (peak) : -140.0;

        // A dB of movement per dB over the threshold, up to the range: the
        // band gives back exactly what the material asked for and no more.
        const auto range = (double) band.dynRangeDb;
        const auto over = peakDb - (double) band.dynThresholdDb;
        const auto target = juce::jlimit (0.0, std::abs (range), over)
                            * (range < 0.0 ? -1.0 : 1.0);

        // Smoothing the gain rather than the detector is what makes attack and
        // release mean what they say: the ride is a one-pole toward the target,
        // so it covers 63% of any move in exactly the time that is set.
        const auto coef = std::abs (target) > std::abs (pl.rideDb) ? pl.attackCoef
                                                                   : pl.releaseCoef;
        pl.rideDb += coef * (target - pl.rideDb);

        if (std::abs (pl.rideDb - pl.tunedRideDb) > rideRetuneDb)
            tuneBand (pl, (float) pl.rideDb);
    }
}

double StemFxChain::staticCurveOffsetDb() const
{
    // Auto gain: the average of the static curve over log frequency, negated.
    //
    //   autoGainDb = -(1/N) * sum over k of 20*log10 |H(f_k)|
    //
    // with f_k log-spaced across 20 Hz .. 20 kHz, which is the discrete form of
    // -(1/ln(f2/f1)) * integral of H_dB d(ln f). Equal weight per octave is the
    // closest cheap stand-in for "perceived level": a boost an octave wide
    // counts the same at 100 Hz as at 10 kHz. The compressor and the dynamic
    // ride are deliberately not in it — this is a static estimate of the curve,
    // not a loudness meter.
    auto sum = 0.0;

    for (int k = 0; k < autoGainPoints; ++k)
    {
        const auto c1 = agCosW[(size_t) k], s1 = agSinW[(size_t) k];
        const auto c2 = agCos2W[(size_t) k], s2 = agSin2W[(size_t) k];

        for (int i = 0; i < activeSections; ++i)
        {
            const auto& c = section[(size_t) i].c;

            const auto nr = c.b0 + c.b1 * c1 + c.b2 * c2;
            const auto ni = -(c.b1 * s1 + c.b2 * s2);
            const auto dr = 1.0 + c.a1 * c1 + c.a2 * c2;
            const auto di = -(c.a1 * s1 + c.a2 * s2);

            sum += 10.0 * std::log10 (juce::jmax (nr * nr + ni * ni, 1.0e-30)
                                      / juce::jmax (dr * dr + di * di, 1.0e-30));
        }
    }

    return juce::jlimit (-autoGainLimitDb, autoGainLimitDb, -sum / autoGainPoints);
}

void StemFxChain::setSoloBand (int index)
{
    if (index == soloOverride)
        return;

    soloOverride = index;

    if (prepared)
        rebuildSolo();
}

void StemFxChain::process (float* left, float* right, int numSamples)
{
    if (! prepared || numSamples <= 0)
        return;

    juce::ScopedNoDenormals noDenormals;

    if (soloActive)
    {
        // Solo is an inspection mode: the region alone, none of the other
        // bands and none of the compressor. The output gain stays on so the
        // listen can be levelled, and so leaving solo does not jump.
        for (int s = 0; s < soloSections; ++s)
        {
            soloSection[(size_t) s].process (left, 0, numSamples);
            soloSection[(size_t) s].process (right, 1, numSamples);
        }
    }
    else
    {
        if (current.isFlat())
            return;

        for (int pos = 0; pos < numSamples;)
        {
            // Without dynamics the whole block is one pass; with them the
            // block is walked in control-sized steps so the riding bands can
            // retune inside it — never per sample.
            const auto len = dynamicCount > 0 ? juce::jmin (controlSamples, numSamples - pos)
                                              : numSamples - pos;

            if (dynamicCount > 0)
                updateDynamics (left + pos, right + pos, len);

            for (int s = 0; s < activeSections; ++s)
            {
                section[(size_t) s].process (left + pos, 0, len);
                section[(size_t) s].process (right + pos, 1, len);
            }

            pos += len;
        }

        if (current.compOn())
        {
            // One instance over both channels: the envelope is per channel, but
            // the stem is one source and this is a preview strip, not a
            // mastering bus.
            float* channels[2] = { left, right };
            juce::dsp::AudioBlock<float> block (channels, 2, (size_t) numSamples);
            juce::dsp::ProcessContextReplacing<float> context (block);
            comp.process (context);
        }
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const auto g = outGain.getNextValue();
        left[i] *= g;
        right[i] *= g;
    }
}

} // namespace splittr
