#include "SpectrumTap.h"

#include <cmath>

namespace splittr
{

namespace
{
    // 1000, i.e. 20 Hz .. 20 kHz. Named because it appears in the layout, in
    // its inverse and in the slice width, and the three have to agree.
    constexpr float decades = SpectrumTap::highestHz / SpectrumTap::lowestHz;

    constexpr int lastPoint = SpectrumTap::numPoints - 1;

    // A Hann-windowed tone reads up to 1.42 dB low when it falls exactly
    // between two bins -- the window's scalloping loss -- and the parabola
    // below reads that worst case back about a third of a dB high, so 2 dB is
    // the whole range any honest recovery can need. The cap is what keeps it
    // from "recovering" a noise bin whose neighbours happen to sit 40 dB apart;
    // it has to sit clear of the legitimate range, because clipping the apex
    // flattens the top of a peak and lets a neighbouring point tie with it.
    constexpr float maxScallopDb = 2.0f;

    // What a display point makes of the strongest bin in its slice.
    //
    // Reading the bin's own level has two faults, and one parabola fixes both.
    // A tone falling between bins reads up to 1.4 dB low, which is a poor thing
    // to aim a cut at. And at the bottom of the range, where several points
    // share one pair of bins, every one of them reads the same number: a tone
    // at 50 Hz comes out as a flat shelf from 44 to 56 Hz with no peak in it to
    // point at.
    //
    // So: fit a parabola through the strongest bin and its neighbours -- which
    // is what a log-magnitude main lobe looks like near its top -- and read it
    // where that lobe peaks, or at the near edge of this point's own slice when
    // the peak belongs to a neighbouring point. The level comes out within
    // about a third of a dB wherever the tone sits, and the shelf becomes a
    // hump with its top over the tone.
    //
    // `lowBin` and `highBin` are the point's slice of the axis in fractional
    // bins; `bin` is the strongest bin found in it.
    float pointDb (const float* magnitudes, float scale, int bin,
                   float lowBin, float highBin, int lastBin) noexcept
    {
        const auto db = [magnitudes, scale] (int b) {
            return juce::Decibels::gainToDecibels (magnitudes[b] * scale, SpectrumTap::floorDb);
        };

        const auto here = db (bin);

        if (bin <= 1 || bin >= lastBin)
            return here;

        const auto below = db (bin - 1);
        const auto above = db (bin + 1);
        const auto curvature = below - 2.0f * here + above;

        if (curvature > -1.0e-6f)
            return here; // flat: three bins on the floor, or a degenerate fit

        // Where the three bins say the lobe tops out, in bins from `bin`...
        const auto apex = juce::jlimit (-0.5f, 0.5f, 0.5f * (below - above) / curvature);

        // ...pulled back into this point's own slice if it belongs to another.
        const auto t = juce::jlimit (lowBin - (float) bin, highBin - (float) bin, apex);
        const auto value = here + 0.5f * t * (above - below) + 0.5f * t * t * curvature;

        return juce::jmin (value, here + maxScallopDb);
    }
} // namespace

//==============================================================================

float SpectrumTap::pointFrequency (int point) noexcept
{
    const auto t = (float) juce::jlimit (0, lastPoint, point) / (float) lastPoint;
    return lowestHz * std::pow (decades, t);
}

int SpectrumTap::nearestPoint (float hz) noexcept
{
    const auto t = std::log (juce::jmax (hz, 1.0e-3f) / lowestHz) / std::log (decades);
    return juce::jlimit (0, lastPoint, (int) std::lround (t * (float) lastPoint));
}

//==============================================================================

void SpectrumTap::prepare (double sampleRate)
{
    rate = sampleRate > 0.0 ? sampleRate : 44100.0;

    if (fft == nullptr)
        fft = std::make_unique<juce::dsp::FFT> (fftOrder);

    ring.assign ((size_t) fftSize, 0.0f);
    scratch.assign ((size_t) fftSize * 2, 0.0f);

    if (window.empty())
    {
        window.resize ((size_t) fftSize);

        // Our own Hann rather than dsp::WindowingFunction: the readout is
        // calibrated against the window's sum, and that calibration must not
        // depend on which way JUCE's `normalise` flag happens to scale the
        // table. Periodic (n / fftSize, not n / (fftSize - 1)), which is the
        // form that sums to exactly half the window length.
        for (int n = 0; n < fftSize; ++n)
            window[(size_t) n] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                                         * (float) n / (float) fftSize);

        double sum = 0.0;

        for (auto w : window)
            sum += (double) w;

        // A sine of amplitude A through this window peaks at A * sum / 2 in the
        // magnitude spectrum, so 2 / sum turns that peak back into A and a
        // full-scale sine reads 0 dBFS. Measured off the table rather than
        // assumed, so the two can never drift apart.
        magScale = (float) (2.0 / sum);
    }

    buildPointMap();

    for (auto& slot : slots)
    {
        slot.db.fill (floorDb);
        slot.sampleRate = rate;
        slot.framesPerSecond = framesPerSecond();
        slot.sequence = 0;
    }

    shared.store (0, std::memory_order_release);
    writeSlot = 1;
    readSlot = 2;
    nextSequence = 1;

    ringWrite = 0;
    sinceHop = 0;
    primed = 0;
    prepared = true;
}

void SpectrumTap::buildPointMap()
{
    pointBins.resize ((size_t) numPoints);

    const auto binHz = rate / (double) fftSize;
    const auto nyquistBin = fftSize / 2;

    // Half a point's width as a frequency ratio: a point owns the axis from
    // centre / halfStep to centre * halfStep, which is its share of a layout
    // whose neighbours are one full step apart.
    const auto halfStep = std::pow ((double) decades, 0.5 / (double) lastPoint);

    for (int p = 0; p < numPoints; ++p)
    {
        const auto centre = (double) pointFrequency (p);
        const auto lowBin = centre / halfStep / binHz;
        const auto highBin = centre * halfStep / binHz;

        // The bins whose centres land inside the slice...
        auto first = (int) std::ceil (lowBin);
        auto last = (int) std::floor (highBin);

        // ...and, where the slice is narrower than a bin, the two that bracket
        // it instead. Below roughly 1 kHz at 44.1 kHz every slice is narrower
        // than a bin, and a point that owned no bin at all would drop a tone
        // sitting between its neighbours.
        if (last < first)
        {
            first = (int) std::floor (centre / binHz);
            last = first + 1;
        }

        // Bin 0 is DC: a stem with a hint of offset would otherwise plant a
        // permanent wall at the left edge of the display.
        first = juce::jlimit (1, nyquistBin, first);
        last = juce::jlimit (first, nyquistBin, last);

        pointBins[(size_t) p] = { first, last, (float) lowBin, (float) highBin };
    }
}

void SpectrumTap::reset() noexcept
{
    if (! prepared)
        return;

    std::fill (ring.begin(), ring.end(), 0.0f);
    ringWrite = 0;
    sinceHop = 0;
    primed = 0;
}

//==============================================================================

void SpectrumTap::push (const float* left, const float* right, int numSamples) noexcept
{
    if (! prepared || left == nullptr || numSamples <= 0)
        return;

    // Walked in hop-sized pieces rather than pushed wholesale and checked at
    // the end: a host block longer than the hop crosses more than one boundary,
    // and a frame published at the end of the call would be looking at samples
    // from after the boundary it belongs to.
    for (int i = 0; i < numSamples;)
    {
        const auto take = juce::jmin (numSamples - i, hopSize - sinceHop);

        for (int n = 0; n < take; ++n)
        {
            const auto l = left[i + n];
            const auto r = right != nullptr ? right[i + n] : l;

            ring[(size_t) ringWrite] = 0.5f * (l + r);

            if (++ringWrite == fftSize)
                ringWrite = 0;
        }

        i += take;
        sinceHop += take;
        primed = juce::jmin (primed + take, fftSize);

        if (sinceHop < hopSize)
            continue;

        sinceHop = 0;

        // Nothing goes out until the window is full of real audio. The first
        // frame after a reset costs 93 ms, which is the price of not showing a
        // half-window of zeros as a broadband click.
        if (primed >= fftSize)
            publishFrame();
    }
}

void SpectrumTap::publishFrame() noexcept
{
    auto* buf = scratch.data();

    // Unwrap oldest-first and window in the same pass -- the ring has to stay
    // intact, the next frame overlaps this one by half.
    for (int n = 0, r = ringWrite; n < fftSize; ++n)
    {
        buf[n] = ring[(size_t) r] * window[(size_t) n];

        if (++r == fftSize)
            r = 0;
    }

    std::fill (buf + fftSize, buf + fftSize * 2, 0.0f);

    // In place, on preallocated tables, and told not to bother mirroring the
    // negative frequencies: magnitudes land in bins 0 .. fftSize / 2.
    fft->performFrequencyOnlyForwardTransform (buf, true);

    auto& frame = slots[(size_t) writeSlot];
    const auto nyquistBin = fftSize / 2;

    for (int p = 0; p < numPoints; ++p)
    {
        const auto bins = pointBins[(size_t) p];
        auto peakBin = bins.first;

        // Search in magnitude and convert afterwards: the maximum survives the
        // conversion, so the logarithms come out at three per displayed point
        // rather than one per bin of a 2049-bin spectrum.
        for (int b = bins.first + 1; b <= bins.last; ++b)
            if (buf[(size_t) b] > buf[(size_t) peakBin])
                peakBin = b;

        frame.db[(size_t) p] = pointDb (buf, magScale, peakBin, bins.lowBin, bins.highBin, nyquistBin);
    }

    frame.sampleRate = rate;
    frame.framesPerSecond = framesPerSecond();
    frame.sequence = nextSequence++;

    // Hand the filled slot over and take whatever the reader left behind. One
    // exchange, no branch, nothing to wait on.
    const auto previous = shared.exchange ((unsigned) writeSlot | freshFlag, std::memory_order_acq_rel);
    writeSlot = (int) (previous & slotMask);
}

bool SpectrumTap::read (Frame& dest) noexcept
{
    if ((shared.load (std::memory_order_acquire) & freshFlag) == 0)
        return false;

    // Only this thread clears the flag, so having seen it set, it is still set:
    // the exchange cannot come back with a slot we have already read.
    const auto previous = shared.exchange ((unsigned) readSlot, std::memory_order_acq_rel);
    readSlot = (int) (previous & slotMask);

    dest = slots[(size_t) readSlot];
    return true;
}

//==============================================================================

SpectrumSmoother::SpectrumSmoother()
{
    reset();
    updateCoefficients();
}

void SpectrumSmoother::setSpeed (Speed s)
{
    if (s == current)
        return;

    current = s;
    updateCoefficients();
}

void SpectrumSmoother::setTickRate (double hz)
{
    const auto clamped = juce::jlimit (1.0, 240.0, hz);

    if (juce::approximatelyEqual (clamped, tickRate))
        return;

    tickRate = clamped;
    updateCoefficients();
}

void SpectrumSmoother::updateCoefficients()
{
    struct Times
    {
        double attackMs, releaseMs;
    };

    const auto times = current == Speed::fast     ? Times { 10.0, 150.0 }
                       : current == Speed::medium ? Times { 30.0, 450.0 }
                                                  : Times { 90.0, 1400.0 };

    // One-pole per tick: a step is 63% covered after one time constant,
    // whatever rate update() is being called at.
    const auto coefficient = [this] (double ms) {
        return (float) (1.0 - std::exp (-1.0 / juce::jmax (1.0e-3, ms * 0.001 * tickRate)));
    };

    attackCoefficient = coefficient (times.attackMs);
    releaseCoefficient = coefficient (times.releaseMs);
}

void SpectrumSmoother::reset()
{
    target.fill (SpectrumTap::floorDb);
    smoothed.fill (SpectrumTap::floorDb);
    peaks.fill (SpectrumTap::floorDb);
    holdLeft.fill (0.0f);
}

void SpectrumSmoother::update (const SpectrumTap::Frame* frame)
{
    if (frame != nullptr)
        target = frame->db;

    const auto tick = (float) (1.0 / tickRate);
    const auto fall = peakFallDbPerSecond * tick;

    for (int p = 0; p < SpectrumTap::numPoints; ++p)
    {
        const auto want = target[(size_t) p];
        auto& level = smoothed[(size_t) p];

        level += (want - level) * (want > level ? attackCoefficient : releaseCoefficient);

        // The ghost: hold the highest reading, then let it walk down at a fixed
        // dB per second, which reads as a falling line rather than a fade.
        auto& peak = peaks[(size_t) p];
        auto& hold = holdLeft[(size_t) p];

        if (level >= peak)
        {
            peak = level;
            hold = peakHoldSeconds;
        }
        else if (hold > 0.0f)
        {
            hold -= tick;
        }
        else
        {
            peak = juce::jmax (level, peak - fall);
        }
    }
}

} // namespace splittr
