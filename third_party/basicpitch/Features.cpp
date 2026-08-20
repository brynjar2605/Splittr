//
// See Features.h for why this exists instead of an ONNX Runtime session.
//
// The graph, node for node:
//
//   for octave = 0 .. 8:
//       reflect-pad the signal by 128 either side
//       conv1d with the 36x256 real basis, stride 256 >> octave   -> Re
//       conv1d with the 36x256 imaginary basis, negated           -> Im
//       zero-pad by 127 either side, conv1d with the 256-tap
//       lowpass at stride 2                                       -> next octave
//   concat the octaves deepest-first (324 bins), keep the top 309
//   scale each bin by sqrt(window length), magnitude = hypot(Re, Im)
//   normalised log: 10*log10(mag^2 + 1e-10), shifted to 0 and divided by its
//       own maximum over the whole clip
//   affine: * 2.480741 - 0.876918
//   harmonic stacking at bin offsets {-36, 0, 36, 57, 72, 84, 93, 101}
//       (harmonics 0.5, 1, 2, 3, 4, 5, 6, 7 at 36 bins per octave), trimmed
//       to the low 264 bins
//

#include "Features.h"

#include "BasicPitchConstants.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace basicpitch
{

namespace
{
    // model_1/normalized_log_1/* and model_1/tf.math.multiply_1 in the graph.
    constexpr float logEpsilon = 1.000000013351432e-10f;
    constexpr float logScale = 4.342944622039795f; // ln -> 10*log10
    constexpr float featureScale = 2.480741024017334f;
    constexpr float featureOffset = -0.8769183158874512f;

    // 36 * log2(h) rounded, for h = 1/2, 1, 2, 3, 4, 5, 6, 7.
    constexpr int harmonicShifts[NUM_HARMONICS] = { -36, 0, 36, 57, 72, 84, 93, 101 };

    using RowMajor = Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
    using StridedRows = Eigen::Map<const RowMajor, Eigen::Unaligned, Eigen::OuterStride<>>;

    // ONNX `Pad` with mode "reflect": edge sample excluded, so ...c b a | a b c
    // is wrong and ...c b | a b c is right.
    void reflectPad (const std::vector<float>& in, int pad, std::vector<float>& out)
    {
        const auto n = (int) in.size();
        out.resize ((size_t) (n + 2 * pad));

        for (int i = 0; i < pad; ++i)
            out[(size_t) i] = in[(size_t) (pad - i)];

        std::memcpy (out.data() + pad, in.data(), sizeof (float) * (size_t) n);

        for (int i = 0; i < pad; ++i)
            out[(size_t) (n + pad + i)] = in[(size_t) (n - 2 - i)];
    }

    void zeroPad (const std::vector<float>& in, int pad, std::vector<float>& out)
    {
        const auto n = (int) in.size();
        out.assign ((size_t) (n + 2 * pad), 0.0f);
        std::memcpy (out.data() + pad, in.data(), sizeof (float) * (size_t) n);
    }

    int convFrames (int paddedLength, int kernelLength, int stride)
    {
        return paddedLength < kernelLength ? 0 : (paddedLength - kernelLength) / stride + 1;
    }
} // namespace

bool Features::loadKernels (const void* data, size_t numBytes)
{
    clear();

    const auto* bytes = static_cast<const char*> (data);

    if (data == nullptr || numBytes < 20 || std::memcmp (bytes, "SPCQT001", 8) != 0)
        return false;

    int header[3] = {};
    std::memcpy (header, bytes + 8, sizeof (header));

    binsPerOctave = header[0];
    kernelLength = header[1];
    numScaleBins = header[2];

    if (binsPerOctave <= 0 || kernelLength <= 0 || numScaleBins <= 0
        || numScaleBins > numOctaves * binsPerOctave
        || (kernelLength & 1) != 0)
        return false;

    const auto floats = (size_t) binsPerOctave * (size_t) kernelLength * 2
                        + (size_t) kernelLength + (size_t) numScaleBins;

    if (numBytes != 20 + floats * sizeof (float))
        return false;

    const auto* src = reinterpret_cast<const float*> (bytes + 20);

    // Interleave the two bases into one [kernelLength][2 * bins] matrix so a
    // single GEMM per octave yields real and imaginary halves at once.
    kernel.assign ((size_t) kernelLength * (size_t) (2 * binsPerOctave), 0.0f);

    for (int bin = 0; bin < binsPerOctave; ++bin)
    {
        for (int k = 0; k < kernelLength; ++k)
        {
            const auto row = (size_t) k * (size_t) (2 * binsPerOctave);
            kernel[row + (size_t) bin] = src[(size_t) bin * (size_t) kernelLength + (size_t) k];
            kernel[row + (size_t) (binsPerOctave + bin)] =
                src[((size_t) binsPerOctave + (size_t) bin) * (size_t) kernelLength + (size_t) k];
        }
    }

    const auto* lowpassSrc = src + (size_t) binsPerOctave * (size_t) kernelLength * 2;
    lowpass.assign (lowpassSrc, lowpassSrc + kernelLength);

    const auto* scaleSrc = lowpassSrc + kernelLength;
    binScale.assign (scaleSrc, scaleSrc + numScaleBins);

    return true;
}

void Features::clear()
{
    kernel.clear();
    lowpass.clear();
    binScale.clear();
    cqt.clear();
    cqt.shrink_to_fit();
    frames = 0;
}

int Features::compute (const float* audio, int numSamples)
{
    frames = 0;
    cqt.clear();

    if (kernel.empty() || audio == nullptr || numSamples < minSamples)
        return 0;

    const auto halfKernel = kernelLength / 2;
    const auto totalBins = numOctaves * binsPerOctave;

    // Every octave halves both the signal and the stride, so all nine produce
    // the same frame count: floor(numSamples / hop) + 1.
    const auto numFrames = numSamples / hop + 1;

    // Magnitudes for the whole spectrum, [frame][bin] with bin 0 the lowest —
    // i.e. the deepest octave first, matching the graph's concat order.
    std::vector<float> magnitude ((size_t) numFrames * (size_t) totalBins, 0.0f);

    std::vector<float> signal (audio, audio + numSamples);
    std::vector<float> padded, response;

    const Eigen::Map<const RowMajor> kernelMap (kernel.data(), kernelLength, 2 * binsPerOctave);

    for (int octave = 0; octave < numOctaves; ++octave)
    {
        const auto stride = hop >> octave;

        reflectPad (signal, halfKernel, padded);

        const auto rows = convFrames ((int) padded.size(), kernelLength, stride);

        if (rows < numFrames)
            return 0; // clip too short for this many octaves

        response.assign ((size_t) rows * (size_t) (2 * binsPerOctave), 0.0f);

        {
            const StridedRows windows (padded.data(), rows, kernelLength,
                                       Eigen::OuterStride<> (stride));
            Eigen::Map<RowMajor> out (response.data(), rows, 2 * binsPerOctave);
            out.noalias() = windows * kernelMap;
        }

        // The graph negates the imaginary conv; magnitude does not care, so the
        // Neg node folds away here.
        const auto binBase = (numOctaves - 1 - octave) * binsPerOctave;

        for (int frame = 0; frame < numFrames; ++frame)
        {
            const auto* row = response.data() + (size_t) frame * (size_t) (2 * binsPerOctave);
            auto* dst = magnitude.data() + (size_t) frame * (size_t) totalBins + (size_t) binBase;

            for (int bin = 0; bin < binsPerOctave; ++bin)
            {
                const auto re = row[bin];
                const auto im = row[binsPerOctave + bin];
                dst[bin] = std::sqrt (re * re + im * im);
            }
        }

        if (octave + 1 < numOctaves)
        {
            zeroPad (signal, halfKernel - 1, padded);

            const auto down = convFrames ((int) padded.size(), kernelLength, 2);

            if (down <= 0)
                return 0;

            const StridedRows windows (padded.data(), down, kernelLength,
                                       Eigen::OuterStride<> (2));
            const Eigen::Map<const Eigen::VectorXf> filter (lowpass.data(), kernelLength);

            signal.resize ((size_t) down);
            Eigen::Map<Eigen::VectorXf> (signal.data(), down).noalias() = windows * filter;
        }
    }

    // Keep the top numScaleBins of the 324 (the graph slices [-309:]), apply
    // the per-bin sqrt(window length) scaling, and take the normalised log.
    frames = numFrames;
    cqt.assign ((size_t) numFrames * (size_t) numScaleBins, 0.0f);

    const auto binOffset = totalBins - numScaleBins;

    auto minimum = std::numeric_limits<float>::max();

    for (int frame = 0; frame < numFrames; ++frame)
    {
        const auto* src = magnitude.data() + (size_t) frame * (size_t) totalBins + (size_t) binOffset;
        auto* dst = cqt.data() + (size_t) frame * (size_t) numScaleBins;

        for (int bin = 0; bin < numScaleBins; ++bin)
        {
            const auto scaled = src[bin] * binScale[(size_t) bin];
            const auto value = std::log (scaled * scaled + logEpsilon) * logScale;
            dst[bin] = value;
            minimum = std::min (minimum, value);
        }
    }

    auto maximum = 0.0f;

    for (auto& value : cqt)
    {
        value -= minimum;
        maximum = std::max (maximum, value);
    }

    // div_no_nan: a flat (digitally silent) clip normalises to zero, not NaN.
    const auto norm = maximum != 0.0f ? 1.0f / maximum : 0.0f;

    for (auto& value : cqt)
        value = value * norm * featureScale + featureOffset;

    return frames;
}

void Features::harmonicStack (int frame, float* out) const
{
    if (frame < 0 || frame >= frames || out == nullptr)
        return;

    const auto* row = cqt.data() + (size_t) frame * (size_t) numScaleBins;

    for (int bin = 0; bin < NUM_FREQ_IN; ++bin)
    {
        auto* dst = out + (size_t) bin * NUM_HARMONICS;

        for (int harmonic = 0; harmonic < NUM_HARMONICS; ++harmonic)
        {
            const auto source = bin + harmonicShifts[harmonic];
            dst[harmonic] = (source >= 0 && source < numScaleBins) ? row[source] : 0.0f;
        }
    }
}

} // namespace basicpitch
