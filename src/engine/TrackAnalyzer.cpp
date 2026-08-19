#include "TrackAnalyzer.h"

#include <juce_dsp/juce_dsp.h>

namespace splittr
{

namespace
{
    constexpr double kSampleRate = 44100.0;
    constexpr int kMaxAnalysisSeconds = 90; // analyse at most this much, from the middle

    juce::AudioBuffer<float> monoSection (const juce::AudioBuffer<float>& stereo)
    {
        const auto total = stereo.getNumSamples();
        const auto maxSamples = (int) (kMaxAnalysisSeconds * kSampleRate);
        const auto length = juce::jmin (total, maxSamples);
        const auto start = juce::jmax (0, (total - length) / 2);

        juce::AudioBuffer<float> mono (1, length);
        mono.copyFrom (0, 0, stereo, 0, start, length);

        if (stereo.getNumChannels() > 1)
        {
            mono.addFrom (0, 0, stereo, 1, start, length);
            mono.applyGain (0.5f);
        }

        return mono;
    }

    //==========================================================================
    double detectBpm (const juce::AudioBuffer<float>& mono)
    {
        constexpr int frame = 1024, hop = 512;
        const auto fps = kSampleRate / hop; // ~86 envelope frames per second

        const auto numFrames = (mono.getNumSamples() - frame) / hop;

        if (numFrames < (int) (10 * fps)) // need ~10s to say anything
            return 0.0;

        // Onset envelope: rectified frame-to-frame RMS increase.
        std::vector<double> envelope ((size_t) numFrames, 0.0);
        const auto* samples = mono.getReadPointer (0);
        double previousRms = 0.0;

        for (int n = 0; n < numFrames; ++n)
        {
            double sum = 0.0;
            for (int i = 0; i < frame; ++i)
            {
                const double v = samples[n * hop + i];
                sum += v * v;
            }

            const auto rms = std::sqrt (sum / frame);
            envelope[(size_t) n] = juce::jmax (0.0, rms - previousRms);
            previousRms = rms;
        }

        // Remove DC so autocorrelation peaks mean periodicity, not loudness.
        double mean = 0.0;
        for (auto v : envelope)
            mean += v;
        mean /= (double) envelope.size();
        for (auto& v : envelope)
            v -= mean;

        // Autocorrelate over lags covering 60-190 BPM.
        const auto lagFor = [fps] (double bpm) { return (int) std::round (fps * 60.0 / bpm); };
        const auto minLag = lagFor (190.0);
        const auto maxLag = lagFor (60.0);

        double norm = 0.0;
        for (auto v : envelope)
            norm += v * v;

        if (norm <= 0.0)
            return 0.0;

        std::vector<double> score ((size_t) maxLag + 1, 0.0);
        double bestScore = 0.0;

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            double sum = 0.0;
            for (size_t n = 0; n + (size_t) lag < envelope.size(); ++n)
                sum += envelope[n] * envelope[n + (size_t) lag];

            score[(size_t) lag] = sum / norm;
            bestScore = juce::jmax (bestScore, score[(size_t) lag]);
        }

        if (bestScore <= 0.0)
            return 0.0;

        // Pick the best lag, folding tempo octaves into a preferred 70-180
        // range: a candidate near half/double the best lag with comparable
        // correlation wins if it lands in range.
        int bestLag = minLag;
        for (int lag = minLag; lag <= maxLag; ++lag)
            if (score[(size_t) lag] > score[(size_t) bestLag])
                bestLag = lag;

        auto bpm = fps * 60.0 / bestLag;

        const auto scoreNear = [&] (int lag)
        {
            double s = 0.0;
            for (int l = lag - 1; l <= lag + 1; ++l)
                if (l >= minLag && l <= maxLag)
                    s = juce::jmax (s, score[(size_t) l]);
            return s;
        };

        if (bpm < 70.0 && scoreNear (bestLag / 2) > 0.6 * score[(size_t) bestLag])
            bpm *= 2.0;
        else if (bpm > 180.0 && bestLag * 2 <= maxLag
                 && scoreNear (bestLag * 2) > 0.6 * score[(size_t) bestLag])
            bpm /= 2.0;

        return std::round (bpm * 10.0) / 10.0;
    }

    //==========================================================================
    juce::String detectKey (const juce::AudioBuffer<float>& mono)
    {
        constexpr int order = 13, frame = 1 << order, hop = frame / 2; // 8192 / 4096

        const auto numFrames = (mono.getNumSamples() - frame) / hop;

        if (numFrames < 8)
            return {};

        juce::dsp::FFT fft (order);
        juce::dsp::WindowingFunction<float> window ((size_t) frame,
                                                    juce::dsp::WindowingFunction<float>::hann);

        std::vector<float> fftData ((size_t) frame * 2, 0.0f);
        std::array<double, 12> chroma {};

        const auto* samples = mono.getReadPointer (0);

        for (int n = 0; n < numFrames; ++n)
        {
            std::fill (fftData.begin(), fftData.end(), 0.0f);
            std::copy (samples + n * hop, samples + n * hop + frame, fftData.begin());

            window.multiplyWithWindowingTable (fftData.data(), (size_t) frame);
            fft.performFrequencyOnlyForwardTransform (fftData.data());

            const auto binHz = kSampleRate / frame;
            const auto firstBin = (int) std::ceil (55.0 / binHz);   // A1
            const auto lastBin = (int) std::floor (5000.0 / binHz);

            for (int bin = firstBin; bin <= lastBin; ++bin)
            {
                const auto freq = bin * binHz;
                const auto pitch = 12.0 * std::log2 (freq / 440.0) + 69.0; // MIDI
                const auto pc = ((int) std::round (pitch) % 12 + 12) % 12;

                chroma[(size_t) pc] += fftData[(size_t) bin];
            }
        }

        // Krumhansl-Kessler key profiles.
        static const double major[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
                                          2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static const double minor[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
                                          2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

        const auto correlate = [&] (const double* profile, int tonic)
        {
            double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;

            for (int i = 0; i < 12; ++i)
            {
                const auto x = chroma[(size_t) ((i + tonic) % 12)];
                const auto y = profile[i];
                sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y;
            }

            const auto cov = 12 * sxy - sx * sy;
            const auto den = std::sqrt ((12 * sxx - sx * sx) * (12 * syy - sy * sy));
            return den > 0 ? cov / den : 0.0;
        };

        int bestTonic = 0;
        bool bestIsMinor = false;
        double best = -2.0;

        for (int tonic = 0; tonic < 12; ++tonic)
        {
            if (const auto c = correlate (major, tonic); c > best) { best = c; bestTonic = tonic; bestIsMinor = false; }
            if (const auto c = correlate (minor, tonic); c > best) { best = c; bestTonic = tonic; bestIsMinor = true; }
        }

        if (best < 0.4) // too ambiguous to claim
            return {};

        static const char* names[12] = { "C", "C#", "D", "Eb", "E", "F",
                                         "F#", "G", "Ab", "A", "Bb", "B" };

        return juce::String (names[bestTonic]) + (bestIsMinor ? " MIN" : " MAJ");
    }
} // namespace

TrackAnalysis TrackAnalyzer::analyze (const juce::AudioBuffer<float>& stereo44k)
{
    TrackAnalysis result;

    if (stereo44k.getNumSamples() < (int) kSampleRate)
        return result;

    const auto mono = monoSection (stereo44k);

    result.bpm = detectBpm (mono);
    result.key = detectKey (mono);

    return result;
}

} // namespace splittr
