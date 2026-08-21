#pragma once

#include <functional>
#include <string>
#include <vector>

// Metal (MPSGraph) inference backend for htdemucs v4 (issue #24).
//
// This header deliberately includes neither demucs.cpp nor Eigen headers —
// DemucsSeparator.cpp is meant to stay the only TU that pays that compile
// cost (see the firewall note in DemucsSeparator.h), so the model comes in
// as a forward-declared reference and audio crosses as raw float buffers
// whose layouts happen to be exactly what Eigen col-major storage gives the
// caller for free.
namespace demucscpp
{
struct demucs_model;
}

namespace splittr
{
namespace metal
{

    // True when the MPSGraph path can run here: Apple-silicon-class GPU
    // (MTLGPUFamilyApple7, i.e. M1 or newer), macOS 14+, and not disabled via
    // SPLITTR_NO_GPU=1 (the fallback-testing switch). Cached after the first
    // call; safe from any thread.
    bool gpuAvailable();

    // One human-readable line for the diagnostics report (issue #28): the
    // device name when available, and WHICH gate refused when not — the
    // availability bool alone has proven undiagnosable from a bug report.
    std::string statusString();

    // Full-track separation on the GPU: mirrors demucs.cpp's demucs_inference
    // driver (ref-mean/std normalisation, one random shift, 7.8 s segments
    // with 0.25 overlap and transition-weighted overlap-add) with the
    // per-segment forward pass running as one compiled MPSGraph.
    //
    // interleavedStereo: 2 x numSamples col-major (ch, sample) — i.e. L/R
    // interleaved, which is what Eigen::MatrixXf(2, N).data() holds.
    // outSources: 4 x 2 x numSamples col-major (source, ch, sample) — what
    // Eigen::Tensor3dXf(4, 2, N).data() expects. 4-source models only.
    //
    // cb gets progress in [0,1] plus a "[metal] ..." message once per segment;
    // anything it throws (cancellation) propagates out unchanged. Returns
    // false — with `error` filled — on any Metal-side failure, so the caller
    // can fall back to the CPU path; it never converts a callback exception
    // into a false return.
    bool inference (const demucscpp::demucs_model& model,
                    const float* interleavedStereo,
                    int numSamples,
                    float* outSources,
                    const std::function<void (float, const std::string&)>& cb,
                    std::string& error);

    // Validation hook (used by the numeric-parity harness, not the plugin):
    // runs ONE already-normalised padded segment through the graph and reads
    // back the named intermediate tensors. spec is 4x2048x336, wave is
    // 2x343980, both row-major (last index fastest). Tap names and shapes are
    // documented next to the graph build in DemucsMetal.mm.
    bool debugSegmentTaps (const demucscpp::demucs_model& model,
                           const float* spec,
                           const float* wave,
                           const std::vector<std::string>& tapNames,
                           std::vector<std::vector<float>>& tapData,
                           std::string& error);

} // namespace metal
} // namespace splittr
