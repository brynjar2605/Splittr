#include "StemFx.h"

#include <cmath>

namespace splittr
{

namespace
{
    // How far from neutral a control has to be before it counts as on. isFlat(),
    // the descriptor and the chain's band switches all read the same tolerance,
    // so what is heard, what is written and what is processed cannot disagree.
    constexpr float dbEpsilon = 0.05f;

    bool passOn (float hz) { return hz > 1.0f; } // 0 = off
    bool gainOn (float db) { return std::abs (db) > dbEpsilon; }

    // Shelves and the peak share one Q: wide enough to be a tone control rather
    // than a surgical cut, which is what the three-band strip is for.
    constexpr float eqQ = 0.7071f;

    // Long enough to swallow a slider step, short enough that the gain still
    // feels like it is under the mouse.
    constexpr double gainRampSeconds = 0.02;

    // Two decimals, trailing zeros dropped — "90", "2.5", "-18".
    juce::String descriptorNumber (float value)
    {
        auto text = juce::String (value, 2);

        if (text.containsChar ('.'))
            text = text.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");

        return text;
    }
} // namespace

//==============================================================================

bool StemFxParams::isFlat() const
{
    return ! passOn (hpHz) && ! passOn (lpHz)
           && ! gainOn (lowDb) && ! gainOn (midDb) && ! gainOn (highDb)
           && ! compOn() && ! gainOn (gainDb);
}

bool StemFxParams::operator== (const StemFxParams& o) const
{
    const auto same = [] (float a, float b) { return juce::approximatelyEqual (a, b); };

    return same (hpHz, o.hpHz) && same (lpHz, o.lpHz)
           && same (lowDb, o.lowDb)
           && same (midDb, o.midDb) && same (midHz, o.midHz)
           && same (highDb, o.highDb)
           && same (thresholdDb, o.thresholdDb) && same (ratio, o.ratio)
           && same (makeupDb, o.makeupDb)
           && same (gainDb, o.gainDb);
}

juce::String StemFxParams::toDescriptor() const
{
    if (isFlat())
        return {};

    juce::StringArray parts;

    const auto add = [&parts] (const char* key, float value)
    {
        parts.add (juce::String (key) + ":" + descriptorNumber (value));
    };

    if (passOn (hpHz)) add ("hp", hpHz);
    if (passOn (lpHz)) add ("lp", lpHz);

    if (gainOn (lowDb)) add ("low", lowDb);

    // The peak's frequency only travels with a peak that does something —
    // otherwise a stem that sounds flat would not serialise to the empty string.
    if (gainOn (midDb))
    {
        add ("mid", midDb);
        add ("mhz", midHz);
    }

    if (gainOn (highDb)) add ("high", highDb);

    if (compOn())
    {
        add ("th", thresholdDb);
        add ("ra", ratio);

        if (gainOn (makeupDb))
            add ("mk", makeupDb);
    }

    if (gainOn (gainDb)) add ("g", gainDb);

    return parts.joinIntoString (";");
}

StemFxParams StemFxParams::fromDescriptor (const juce::String& descriptor)
{
    StemFxParams p;

    for (const auto& token : juce::StringArray::fromTokens (descriptor, ";", {}))
    {
        const auto key = token.upToFirstOccurrenceOf (":", false, false).trim();
        const auto value = token.fromFirstOccurrenceOf (":", false, false).getFloatValue();

        // Anything unrecognised is left out rather than guessed at, so a
        // descriptor from a future build degrades to the part this one knows.
        if (key == "hp")        p.hpHz = value;
        else if (key == "lp")   p.lpHz = value;
        else if (key == "low")  p.lowDb = value;
        else if (key == "mid")  p.midDb = value;
        else if (key == "mhz")  p.midHz = value;
        else if (key == "high") p.highDb = value;
        else if (key == "th")   p.thresholdDb = value;
        else if (key == "ra")   p.ratio = value;
        else if (key == "mk")   p.makeupDb = value;
        else if (key == "g")    p.gainDb = value;
    }

    return p;
}

//==============================================================================

void StemFxChain::prepare (double sampleRate, int maxBlockSamples)
{
    rate = sampleRate > 0.0 ? sampleRate : 44100.0;

    const juce::dsp::ProcessSpec spec { rate,
                                        (juce::uint32) juce::jmax (1, maxBlockSamples),
                                        (juce::uint32) 2 };

    for (int band = 0; band < numBands; ++band)
    {
        // One coefficient object per band, shared by both channels and then
        // rewritten in place by setParams: the filters keep their own state, so
        // only the numbers are common, and retuning never has to allocate.
        // Starts as a unity peak — a biquad that passes everything through.
        filter[band][0].coefficients = new juce::dsp::IIR::Coefficients<float> (
            juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter (rate, 1000.0f, eqQ, 1.0f));

        filter[band][1].coefficients = filter[band][0].coefficients;

        for (auto& f : filter[band])
            f.prepare (spec);
    }

    comp.prepare (spec);
    comp.setAttack (StemFxParams::attackMs);
    comp.setRelease (StemFxParams::releaseMs);

    outGain.reset (rate, gainRampSeconds);
    outGain.setCurrentAndTargetValue (1.0f);

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
    for (auto& band : filter)
        for (auto& f : band)
            f.reset();

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

    using Coeffs = juce::dsp::IIR::ArrayCoefficients<float>;

    // Both channels point at the same coefficients, so one write retunes the
    // band. No new Coefficients object, hence no allocation on the audio thread.
    const auto tune = [this] (int band, const std::array<float, 6>& values)
    {
        *filter[band][0].coefficients = values;
    };

    // Keep every corner inside the band the running rate can actually represent.
    const auto safeHz = [this] (float hz)
    {
        return juce::jlimit (20.0f, (float) (rate * 0.45), hz);
    };

    // A bypassed band still holds whatever state it stopped on, and dropping
    // that into the middle of a stream is a click: a band being switched back
    // on starts empty.
    const auto engage = [this] (int band, bool on)
    {
        if (on && ! bandOn[band])
            for (auto& f : filter[band])
                f.reset();

        bandOn[band] = on;
    };

    engage (bandHp, passOn (p.hpHz));
    engage (bandLp, passOn (p.lpHz));
    engage (bandLow, gainOn (p.lowDb));
    engage (bandMid, gainOn (p.midDb));
    engage (bandHigh, gainOn (p.highDb));

    // 2nd-order Butterworth: the default Q of the pass filters is 1/sqrt(2).
    if (bandOn[bandHp])
        tune (bandHp, Coeffs::makeHighPass (rate, safeHz (p.hpHz)));

    if (bandOn[bandLp])
        tune (bandLp, Coeffs::makeLowPass (rate, safeHz (p.lpHz)));

    if (bandOn[bandLow])
        tune (bandLow, Coeffs::makeLowShelf (rate, safeHz (StemFxParams::lowShelfHz), eqQ,
                                             juce::Decibels::decibelsToGain (p.lowDb)));

    if (bandOn[bandMid])
        tune (bandMid, Coeffs::makePeakFilter (rate, safeHz (p.midHz), eqQ,
                                               juce::Decibels::decibelsToGain (p.midDb)));

    if (bandOn[bandHigh])
        tune (bandHigh, Coeffs::makeHighShelf (rate, safeHz (StemFxParams::highShelfHz), eqQ,
                                               juce::Decibels::decibelsToGain (p.highDb)));

    if (p.compOn())
    {
        if (! previous.compOn())
            comp.reset(); // same reasoning as the bands: no envelope from last time

        comp.setThreshold (p.thresholdDb);
        comp.setRatio (juce::jmax (1.0f, p.ratio));
    }

    // Makeup is part of the compressor, so it only counts while the compressor
    // does; the two ride the same smoother, which is what keeps a slider drag
    // from stepping the level.
    const auto totalDb = (p.compOn() ? p.makeupDb : 0.0f) + p.gainDb;
    outGain.setTargetValue (juce::Decibels::decibelsToGain (totalDb));
}

void StemFxChain::process (float* left, float* right, int numSamples)
{
    if (! prepared || numSamples <= 0 || current.isFlat())
        return;

    float* channels[2] = { left, right };
    juce::dsp::AudioBlock<float> block (channels, 2, (size_t) numSamples);

    for (int band = 0; band < numBands; ++band)
    {
        if (! bandOn[band])
            continue; // bypassed bands are not processed at all, not run at unity

        for (int ch = 0; ch < 2; ++ch)
        {
            auto single = block.getSingleChannelBlock ((size_t) ch);
            juce::dsp::ProcessContextReplacing<float> context (single);
            filter[band][ch].process (context);
        }
    }

    if (current.compOn())
    {
        // One instance over both channels: the envelope is per channel, but the
        // stem is one source and this is a preview strip, not a mastering bus.
        juce::dsp::ProcessContextReplacing<float> context (block);
        comp.process (context);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const auto g = outGain.getNextValue();
        left[i] *= g;
        right[i] *= g;
    }
}

} // namespace splittr
