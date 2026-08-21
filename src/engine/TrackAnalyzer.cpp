#include "TrackAnalyzer.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <numeric>

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
    // Tempo detection, in four stages:
    //   1. multi-band spectral-flux onset envelope (log-compressed magnitudes)
    //   2. autocorrelation -> comb score over a fine BPM grid, so a tempo is
    //      credited for the support of its own metrical multiples
    //   3. candidate rescoring: pulse-train alignment (precision/recall
    //      F-measure) times a log-normal perceptual prior -> octave choice
    //   4. parabolic refinement of the winning peak + a confidence measure
    //==========================================================================
    constexpr int kOnsetFftOrder = 10;
    constexpr int kOnsetFrame = 1 << kOnsetFftOrder;  // 1024 samples, ~23 ms
    constexpr int kOnsetHop = 256;                    // ~172 envelope frames/s
    constexpr double kOnsetFps = kSampleRate / kOnsetHop;
    constexpr double kMinBpm = 50.0;
    constexpr double kMaxBpm = 210.0;
    constexpr double kPreferredBpm = 120.0;           // prior centre (log-normal)
    constexpr double kPriorWidthOctaves = 0.95;       // prior sigma, in log2 units
    constexpr double kMinConfidence = 0.30;           // below this we report nothing

    struct OnsetEnvelope
    {
        std::vector<double> values;      // half-wave rectified, local mean removed
        double framesPerSecond = 0.0;
    };

    // Bands are wide and few: enough to keep a soft hi-hat from being buried
    // under a loud bass line, without slicing the spectrum so finely that
    // steady tonal partials start to look like onsets.
    struct Band { double lowHz, highHz, weight; };

    constexpr Band kBands[] = { {  30.0,   100.0, 1.5 },   // kick fundamental
                                { 100.0,   250.0, 1.1 },   // snare body, bass
                                { 250.0,   600.0, 0.8 },
                                { 600.0,  1500.0, 0.8 },
                                { 1500.0, 3500.0, 1.1 },   // snare crack
                                { 3500.0, 8000.0, 1.4 },   // hats, transients
                                { 8000.0, 16000.0, 1.2 } };

    constexpr int kNumBands = (int) (sizeof (kBands) / sizeof (Band));

    OnsetEnvelope onsetEnvelope (const juce::AudioBuffer<float>& mono)
    {
        OnsetEnvelope out;
        out.framesPerSecond = kOnsetFps;

        const auto numFrames = (mono.getNumSamples() - kOnsetFrame) / kOnsetHop;

        if (numFrames < (int) (8 * out.framesPerSecond))
            return out;

        juce::dsp::FFT fft (kOnsetFftOrder);
        juce::dsp::WindowingFunction<float> window ((size_t) kOnsetFrame,
                                                    juce::dsp::WindowingFunction<float>::hann);

        const auto binHz = kSampleRate / kOnsetFrame;
        const auto numBins = kOnsetFrame / 2;

        std::array<int, kNumBands + 1> bandEdge {};
        for (int b = 0; b < kNumBands; ++b)
            bandEdge[(size_t) b] = juce::jlimit (1, numBins - 1, (int) std::round (kBands[b].lowHz / binHz));
        bandEdge[kNumBands] = juce::jlimit (1, numBins, (int) std::round (kBands[kNumBands - 1].highHz / binHz));

        // Log compression: the ear's loudness curve, and it stops one loud
        // kick from swamping every other onset in the envelope.
        constexpr double logGamma = 200.0;

        std::vector<float> fftData ((size_t) kOnsetFrame * 2, 0.0f);
        std::vector<double> current ((size_t) numBins, 0.0), previous ((size_t) numBins, 0.0);
        std::vector<std::vector<double>> bandFlux ((size_t) kNumBands,
                                                   std::vector<double> ((size_t) numFrames, 0.0));

        const auto* samples = mono.getReadPointer (0);

        for (int n = 0; n < numFrames; ++n)
        {
            std::fill (fftData.begin(), fftData.end(), 0.0f);
            std::copy (samples + n * kOnsetHop, samples + n * kOnsetHop + kOnsetFrame, fftData.begin());

            window.multiplyWithWindowingTable (fftData.data(), (size_t) kOnsetFrame);
            fft.performFrequencyOnlyForwardTransform (fftData.data());

            for (int bin = bandEdge[0]; bin < bandEdge[kNumBands]; ++bin)
                current[(size_t) bin] = std::log1p (logGamma * (double) fftData[(size_t) bin]);

            for (int b = 0; b < kNumBands; ++b)
            {
                double flux = 0.0;

                for (int bin = bandEdge[(size_t) b]; bin < bandEdge[(size_t) b + 1]; ++bin)
                    flux += juce::jmax (0.0, current[(size_t) bin] - previous[(size_t) bin]);

                bandFlux[(size_t) b][(size_t) n] = flux;
            }

            current.swap (previous);
        }

        // Each band contributes on its own scale: a band with more bins, or
        // simply more energy, must not decide the envelope by itself.
        out.values.assign ((size_t) numFrames, 0.0);

        for (int b = 0; b < kNumBands; ++b)
        {
            const auto& flux = bandFlux[(size_t) b];
            const auto mean = std::accumulate (flux.begin(), flux.end(), 0.0) / (double) flux.size();

            if (mean <= 1.0e-12)
                continue;

            const auto gain = kBands[b].weight / mean;

            for (size_t n = 0; n < flux.size(); ++n)
                out.values[n] += flux[n] * gain;
        }

        // Subtract a running local mean and rectify: kills slow loudness
        // drift, leaves peaks that stand out against their own neighbourhood.
        const auto half = juce::jmax (1, (int) std::round (0.25 * out.framesPerSecond));
        std::vector<double> smoothed ((size_t) numFrames, 0.0);
        double running = 0.0;

        for (int n = 0; n < juce::jmin (numFrames, half); ++n)
            running += out.values[(size_t) n];

        for (int n = 0; n < numFrames; ++n)
        {
            const auto add = n + half;
            const auto drop = n - half - 1;

            if (add < numFrames)
                running += out.values[(size_t) add];
            if (drop >= 0)
                running -= out.values[(size_t) drop];

            const auto lo = juce::jmax (0, n - half);
            const auto hi = juce::jmin (numFrames - 1, n + half);
            smoothed[(size_t) n] = running / (double) (hi - lo + 1);
        }

        for (int n = 0; n < numFrames; ++n)
            out.values[(size_t) n] = juce::jmax (0.0, out.values[(size_t) n] - smoothed[(size_t) n]);

        return out;
    }

    //==========================================================================
    // Unbiased autocorrelation of the (mean-removed) onset envelope,
    // normalised so lag 0 is 1.0.
    std::vector<double> autocorrelation (const std::vector<double>& envelope, int maxLag)
    {
        const auto n = (int) envelope.size();
        const auto mean = std::accumulate (envelope.begin(), envelope.end(), 0.0) / (double) n;

        std::vector<double> centred ((size_t) n, 0.0);
        for (int i = 0; i < n; ++i)
            centred[(size_t) i] = envelope[(size_t) i] - mean;

        std::vector<double> acf ((size_t) maxLag + 1, 0.0);

        for (int lag = 0; lag <= maxLag; ++lag)
        {
            double sum = 0.0;
            for (int i = 0; i + lag < n; ++i)
                sum += centred[(size_t) i] * centred[(size_t) (i + lag)];

            acf[(size_t) lag] = sum / (double) (n - lag);
        }

        if (acf[0] > 0.0)
            for (auto& v : acf)
                v /= acf[0];

        return acf;
    }

    double interpolatedAt (const std::vector<double>& values, double index)
    {
        if (index < 0.0 || index >= (double) values.size() - 1.0)
            return 0.0;

        const auto i = (size_t) index;
        const auto f = index - (double) i;
        return values[i] * (1.0 - f) + values[i + 1] * f;
    }

    // Comb score: a tempo is only believable if the envelope also repeats at
    // its slower metrical multiples (half-bar, bar, ...), which is what
    // separates a real pulse from an isolated autocorrelation bump.
    // `slack` widens each multiple into a small plateau, absorbing tempo drift
    // while candidates are compared; the refinement pass runs with slack 0 so
    // the plateau cannot bias the final figure.
    double combScore (const std::vector<double>& acf, double lag, double slack = 0.01)
    {
        constexpr double weights[] = { 1.0, 0.75, 0.5, 0.4 };
        constexpr int numMultiples = (int) (sizeof (weights) / sizeof (double));

        double sum = 0.0, weightSum = 0.0;

        for (int k = 0; k < numMultiples; ++k)
        {
            const auto multiple = lag * (k + 1);

            if (multiple >= (double) acf.size() - 1.0)
                break;

            auto best = interpolatedAt (acf, multiple);

            if (slack > 0.0)
            {
                const auto width = juce::jmax (0.5, slack * multiple);
                best = juce::jmax (best, juce::jmax (interpolatedAt (acf, multiple - width),
                                                     interpolatedAt (acf, multiple + width)));
            }

            sum += weights[k] * best;
            weightSum += weights[k];
        }

        return weightSum > 0.0 ? sum / weightSum : 0.0;
    }

    // Perceptual tempo prior: log-normal around 120 BPM. Deliberately gentle —
    // it breaks ties between octaves, it does not overrule the evidence.
    double tempoPrior (double bpm)
    {
        const auto z = std::log2 (bpm / kPreferredBpm) / kPriorWidthOctaves;
        return std::exp (-0.5 * z * z);
    }

    //==========================================================================
    // How well a pulse train at this tempo explains the onset envelope.
    //
    //   recall    = share of onset energy that lands on a beat
    //   precision = how evenly the beats are filled (a doubled tempo leaves
    //               every other beat empty; a halved one leaves onsets out)
    //
    // Their F-measure is the octave discriminator: too slow costs recall,
    // too fast costs precision.
    struct PulseFit
    {
        double fMeasure = 0.0;
        double precision = 0.0;
        double recall = 0.0;
        double alternation = 1.0;  // weak-beat / strong-beat energy, period 2
    };

    PulseFit pulseFit (const std::vector<double>& envelope,
                       const std::vector<double>& prefixSum,
                       double lag)
    {
        PulseFit fit;

        const auto n = (int) envelope.size();
        const auto total = prefixSum.back();

        // The guard also keeps the tolerance window narrower than the lag, so
        // neighbouring pulse windows can never overlap and double-count.
        if (lag < 8.0 || total <= 0.0 || (double) n < 4.0 * lag)
            return fit;

        const auto tolerance = juce::jlimit (1, (int) (lag * 0.2), (int) std::round (0.06 * lag));

        const auto windowSum = [&] (double centre)
        {
            const auto lo = juce::jlimit (0, n, (int) std::round (centre) - tolerance);
            const auto hi = juce::jlimit (0, n, (int) std::round (centre) + tolerance + 1);
            return prefixSum[(size_t) hi] - prefixSum[(size_t) lo];
        };

        // The phase is fitted per segment, not once for the whole track: real
        // players drift, and a pulse train locked at 0:00 would be half a beat
        // out by 0:30 and score like noise for it.
        const auto segFrames = juce::jmax ((int) std::ceil (8.0 * lag),
                                           (int) std::round (7.0 * kOnsetFps));
        const auto numSegments = juce::jmax (1, n / segFrames);

        std::vector<double> pulses;
        double alternationSum = 0.0, alternationWeight = 0.0;

        for (int s = 0; s < numSegments; ++s)
        {
            const auto from = (int) ((int64_t) n * s / numSegments);
            const auto to = (int) ((int64_t) n * (s + 1) / numSegments);

            auto bestPhase = 0.0;
            auto bestTotal = -1.0;

            for (int phase = 0; phase < (int) std::ceil (lag); ++phase)
            {
                double sum = 0.0;
                for (double p = from + phase; p < (double) to; p += lag)
                    sum += windowSum (p);

                if (sum > bestTotal)
                {
                    bestTotal = sum;
                    bestPhase = from + phase;
                }
            }

            double even = 0.0, odd = 0.0;
            int numEven = 0, numOdd = 0;

            for (double p = bestPhase; p < (double) to; p += lag)
            {
                const auto value = windowSum (p);
                pulses.push_back (value);

                // Parity is counted inside the segment: each segment has its
                // own phase, so a global index would mean nothing.
                if ((numEven + numOdd) % 2 == 0) { even += value; ++numEven; }
                else                             { odd += value; ++numOdd; }
            }

            if (numEven > 0 && numOdd > 0)
            {
                even /= (double) numEven;
                odd /= (double) numOdd;

                const auto strong = juce::jmax (even, odd);
                const auto weight = even + odd;

                alternationSum += weight * (strong > 0.0 ? juce::jmin (even, odd) / strong : 1.0);
                alternationWeight += weight;
            }
        }

        if (pulses.size() < 4)
            return fit;

        const auto captured = std::accumulate (pulses.begin(), pulses.end(), 0.0);

        auto sorted = pulses;
        std::sort (sorted.begin(), sorted.end());
        const auto strong = sorted[(size_t) ((sorted.size() * 3) / 4)]; // 75th percentile

        fit.recall = juce::jlimit (0.0, 1.0, captured / total);
        fit.precision = strong > 0.0
                            ? juce::jlimit (0.0, 1.0, (captured / (double) pulses.size()) / strong)
                            : 0.0;
        fit.fMeasure = (fit.precision + fit.recall) > 0.0
                           ? 2.0 * fit.precision * fit.recall / (fit.precision + fit.recall)
                           : 0.0;

        if (alternationWeight > 0.0)
            fit.alternation = alternationSum / alternationWeight;

        return fit;
    }

    //==========================================================================
    // Does the whole track agree on this tempo? Each chunk is re-analysed on
    // its own; music keeps landing on the same tempo (or a metrical relative
    // of it) chunk after chunk, while rubato and arrhythmic material scatter.
    // Tolerant of drift by design — a chunk only has to agree to within a few
    // percent, not stay phase-locked.
    double tempoAgreement (const std::vector<double>& env, double bpm)
    {
        const auto n = (int) env.size();
        const auto chunk = (int) std::round (11.0 * kOnsetFps);
        const auto numChunks = juce::jlimit (1, 6, n / chunk);

        if (numChunks < 2)
            return 1.0;

        static const double relatives[] = { 1.0, 0.5, 2.0, 1.5, 2.0 / 3.0 };

        auto agreeing = 0;

        for (int c = 0; c < numChunks; ++c)
        {
            const auto from = (size_t) ((int64_t) n * c / numChunks);
            const auto to = (size_t) ((int64_t) n * (c + 1) / numChunks);

            const std::vector<double> part (env.begin() + (long) from, env.begin() + (long) to);
            const auto maxLag = juce::jmin ((int) (part.size() / 3),
                                            (int) std::ceil (4.0 * 60.0 * kOnsetFps / kMinBpm));

            if (maxLag < (int) std::ceil (60.0 * kOnsetFps / kMinBpm))
                continue;

            const auto acf = autocorrelation (part, maxLag);

            auto bestBpm = 0.0, best = 0.0;

            for (int i = 0; i < 200; ++i)
            {
                const auto candidate = kMinBpm * std::pow (kMaxBpm / kMinBpm, i / 199.0);
                const auto s = combScore (acf, 60.0 * kOnsetFps / candidate) * tempoPrior (candidate);

                if (s > best)
                {
                    best = s;
                    bestBpm = candidate;
                }
            }

            for (auto r : relatives)
            {
                if (bestBpm > 0.0 && std::abs (std::log2 (bestBpm / (bpm * r))) < 0.045) // ~3%
                {
                    ++agreeing;
                    break;
                }
            }
        }

        return (double) agreeing / (double) numChunks;
    }

    //==========================================================================
    double detectBpm (const juce::AudioBuffer<float>& mono, double& confidenceOut)
    {
        confidenceOut = 0.0;

        const auto onsets = onsetEnvelope (mono);
        const auto& env = onsets.values;
        const auto fps = onsets.framesPerSecond;

        if (env.size() < (size_t) (10 * fps)) // need ~10s to say anything
            return 0.0;

        const auto energy = std::accumulate (env.begin(), env.end(), 0.0);

        if (energy <= 0.0)
            return 0.0;

        const auto lagFor = [fps] (double bpm) { return 60.0 * fps / bpm; };

        const auto maxLag = juce::jmin ((int) (env.size() / 3),
                                        (int) std::ceil (4.0 * lagFor (kMinBpm)));

        if (maxLag < (int) std::ceil (lagFor (kMinBpm)))
            return 0.0;

        const auto acf = autocorrelation (env, maxLag);

        std::vector<double> prefixSum (env.size() + 1, 0.0);
        for (size_t i = 0; i < env.size(); ++i)
            prefixSum[i + 1] = prefixSum[i] + env[i];

        // --- stage 2: comb score over a logarithmic BPM grid ----------------
        constexpr int gridSize = 600; // ~0.24% steps across 50-210 BPM
        std::vector<double> grid ((size_t) gridSize, 0.0), gridScore ((size_t) gridSize, 0.0);

        for (int i = 0; i < gridSize; ++i)
        {
            const auto bpm = kMinBpm * std::pow (kMaxBpm / kMinBpm, (double) i / (gridSize - 1));
            grid[(size_t) i] = bpm;
            gridScore[(size_t) i] = combScore (acf, lagFor (bpm));
        }

        const auto peakComb = *std::max_element (gridScore.begin(), gridScore.end());

        if (peakComb <= 0.0)
            return 0.0;

        // --- stage 3: candidates = comb peaks and their metrical relatives --
        std::vector<double> candidates;

        const auto addCandidate = [&] (double bpm)
        {
            if (bpm < kMinBpm || bpm > kMaxBpm)
                return;

            for (auto existing : candidates)
                if (std::abs (existing - bpm) < 0.005 * bpm)
                    return;

            candidates.push_back (bpm);
        };

        for (int i = 1; i < gridSize - 1; ++i)
        {
            if (gridScore[(size_t) i] <= gridScore[(size_t) i - 1]
                || gridScore[(size_t) i] < gridScore[(size_t) i + 1]
                || gridScore[(size_t) i] < 0.35 * peakComb)
                continue;

            static const double relatives[] = { 1.0, 0.5, 2.0, 1.5, 2.0 / 3.0, 1.0 / 3.0, 3.0 };

            for (auto r : relatives)
                addCandidate (grid[(size_t) i] * r);
        }

        if (candidates.empty())
            return 0.0;

        auto bestBpm = 0.0, bestScore = 0.0, bestF = 0.0, runnerUp = 0.0;

        // A margin rival is a candidate on a genuinely DIFFERENT grid. Pure
        // octave relatives (2x, 1/2x) are the same pulse read at another
        // level — that choice is the F-measure's and alternation's to make,
        // and being torn between levels says nothing about whether the grid
        // itself is trustworthy. 3:2 and 4:3 relatives stay rivals: those
        // grids clash, and a track torn between them deserves its low margin.
        const auto rivalOf = [] (double bpm, double best)
        {
            const auto r = std::abs (std::log2 (bpm / best));
            return r > 0.08 && std::abs (r - 1.0) > 0.08;
        };

        for (auto bpm : candidates)
        {
            const auto fit = pulseFit (env, prefixSum, lagFor (bpm));
            const auto comb = combScore (acf, lagFor (bpm)) / peakComb;


            // Comb support gates the candidate, the F-measure and the
            // alternation penalty decide the octave (this is the half/double
            // check, applied to every candidate rather than only at the end),
            // and the prior breaks what is left over.
            const auto score = (0.35 + 0.65 * comb)
                               * fit.fMeasure
                               * std::pow (fit.alternation, 0.7)
                               * tempoPrior (bpm);

            if (score > bestScore)
            {
                // Only count an unrelated candidate as the runner-up, so a
                // near-duplicate peak does not deflate the confidence.
                if (bestBpm > 0.0 && rivalOf (bpm, bestBpm))
                    runnerUp = juce::jmax (runnerUp, bestScore);

                bestScore = score;
                bestBpm = bpm;
                bestF = fit.fMeasure;
            }
            else if (bestBpm > 0.0 && rivalOf (bpm, bestBpm))
            {
                runnerUp = juce::jmax (runnerUp, score);
            }
        }

        if (bestBpm <= 0.0)
            return 0.0;

        // --- stage 4: refine the peak, then measure confidence --------------
        auto refined = bestBpm;
        auto refinedScore = combScore (acf, lagFor (bestBpm), 0.0);

        for (int i = -30; i <= 30; ++i)
        {
            const auto bpm = bestBpm * (1.0 + i * 0.0008); // +-2.4% in 0.08% steps
            const auto s = combScore (acf, lagFor (bpm), 0.0);

            if (s > refinedScore)
            {
                refinedScore = s;
                refined = bpm;
            }
        }

        // Sub-step accuracy: fit a parabola through the winning grid point and
        // its two neighbours and take its vertex.
        const auto step = refined * 0.0008;
        const auto left = combScore (acf, lagFor (refined - step), 0.0);
        const auto right = combScore (acf, lagFor (refined + step), 0.0);
        const auto curvature = left - 2.0 * refinedScore + right;

        if (curvature < 0.0)
            refined += step * 0.5 * (left - right) / curvature;

        // Confidence: how strongly the winning lag stands out from the rest of
        // the autocorrelation, how cleanly a pulse train fits, and how far
        // ahead of the best rival-grid candidate it is.
        const auto lagRange = (int) std::ceil (lagFor (kMinBpm));
        auto acfMean = 0.0;
        auto acfPeak = 0.0;

        for (int lag = (int) std::floor (lagFor (kMaxBpm)); lag <= lagRange; ++lag)
        {
            acfMean += acf[(size_t) lag];
            acfPeak = juce::jmax (acfPeak, acf[(size_t) lag]);
        }

        acfMean /= (double) juce::jmax (1, lagRange - (int) std::floor (lagFor (kMaxBpm)) + 1);

        const auto prominence = juce::jlimit (0.0, 1.0, (acfPeak - acfMean) / 0.22);
        const auto margin = bestScore > 0.0 ? juce::jlimit (0.0, 1.0, 1.0 - runnerUp / bestScore) : 0.0;
        const auto agreement = tempoAgreement (env, bestBpm);
        // F is softened the same way comb is in the candidate score: its job
        // is to pick the metrical level, not to gate detection. Syncopated
        // material (rap especially) puts onset energy off the quarter grid on
        // purpose, so even the true tempo's absolute recall sits low there —
        // and when prominence and whole-track agreement are strong, that must
        // not read as "no pulse" (issue #26: two rap tracks with perfect
        // agreement declined at F ~0.43, leaving stems unlabeled and Ableton
        // free to mis-warp them).
        const auto confidence = juce::jlimit (0.0, 1.0,
                                              std::sqrt (prominence)
                                                  * (0.35 + 0.65 * bestF)
                                                  * (0.3 + 0.7 * agreement)
                                                  * (0.55 + 0.45 * margin));

        confidenceOut = confidence;


        if (confidence < kMinConfidence)
            return 0.0;

        return std::round (juce::jlimit (kMinBpm, kMaxBpm, refined) * 10.0) / 10.0;
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

    result.bpm = detectBpm (mono, result.bpmConfidence);
    result.key = detectKey (mono);

    return result;
}

} // namespace splittr
