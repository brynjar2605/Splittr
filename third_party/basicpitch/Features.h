//
// Splittr replacement for NeuralNote's Lib/Model/Features.{h,cpp}.
//
// Upstream runs Basic Pitch's `features_model.onnx` through ONNX Runtime to
// turn 22.05 kHz audio into the harmonically stacked CQT the CNN eats. That
// graph is pure DSP — nnAudio CQT2010v2, a normalised log, and a harmonic
// gather — with four constant tensors and no learned parameters, so linking a
// ~100 MB inference runtime to evaluate it is all cost and no benefit. This
// reimplements the same 201-node graph natively; the constants are lifted out
// of the .onnx by tools/basicpitch/extract_cqt_kernels.py and shipped as
// weights/cqt_kernels.bin (76 KB).
//
// Verified against onnxruntime on the original model: max abs difference
// 5e-4 over the whole output, i.e. float32 summation-order noise.
//
// One deviation from upstream, for memory: the harmonic stacking is not
// materialised. A four-minute stem is ~20k frames, and 20k x 264 x 8 floats is
// 175 MB, so `compute` keeps only the normalised-log CQT (frames x 309, ~25 MB)
// and `harmonicStack` gathers a single frame on demand.
//

#pragma once

#include <cstddef>
#include <vector>

namespace basicpitch
{

class Features
{
public:
    Features() = default;

    /** Reads weights/cqt_kernels.bin. False when the blob is not what we expect. */
    bool loadKernels (const void* data, size_t numBytes);

    /**
     * Runs the CQT front end over a whole clip.
     * @param audio mono, 22050 Hz.
     * @param numSamples at least minSamples; shorter input must be zero-padded
     *        by the caller (the deepest octave reflect-pads by 128 samples).
     * @return number of frames computed, or 0 on failure.
     */
    int compute (const float* audio, int numSamples);

    int numFrames() const { return frames; }

    /** Writes NUM_FREQ_IN * NUM_HARMONICS floats, laid out [frequency][harmonic]. */
    void harmonicStack (int frame, float* out) const;

    void clear();

    // Nine octaves of halving leave the deepest one at numSamples/256 samples,
    // and its reflect pad needs more than 128 of those.
    static constexpr int minSamples = 129 * 256;

private:
    static constexpr int numOctaves = 9;
    static constexpr int hop = 256;

    int binsPerOctave = 0, kernelLength = 0, numScaleBins = 0;

    // [kernelLength][2 * binsPerOctave]: real basis then imaginary, so one
    // matrix multiply per octave produces both halves.
    std::vector<float> kernel;
    std::vector<float> lowpass; // octave-to-octave /2 decimation filter
    std::vector<float> binScale;

    // Normalised-log CQT, frames x numScaleBins, row-major.
    std::vector<float> cqt;
    int frames = 0;
};

} // namespace basicpitch
