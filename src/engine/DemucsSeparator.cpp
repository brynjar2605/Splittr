#include "DemucsSeparator.h"
#include "DemucsMetal.h"

#include <model.hpp>

#include <mutex>
#include <thread>

#if JUCE_MAC
 #include <sys/sysctl.h>
#endif

namespace splittr
{

namespace
{
    struct CancelledException final : public std::exception {};

    float peakToDb (float peak)
    {
        return juce::Decibels::gainToDecibels (peak, -120.0f);
    }

    // Chunked multi-threaded inference, after demucs.cpp's
    // cli-apps/threaded_inference.hpp: the track is split into N contiguous
    // chunks padded with 0.75 s of neighbouring audio on each side; each chunk
    // runs a full demucs_inference on its own thread; the outputs are
    // crossfaded over the overlap and weight-normalised.
    constexpr int kOverlapSamples = (int) (0.75 * 44100.0);

    // A chunk's work rounds up to whole 7.8 s model segments, so chunks much
    // shorter than ~2 segment strides (~11.7 s) spend their time on padding:
    // on a 45 s file, 3 chunks measured 1.62x faster than 1, but 4 chunks was
    // slower than 3 (issue #24). 12 s keeps every chunk at least 2 strides.
    constexpr int kMinSamplesPerChunk = 12 * 44100;

    // Wall-clock gains flatten out well before the core count — the workers
    // contend for the shared matrix hardware — and each extra chunk costs
    // ~0.5 GB peak RSS (issue #24: 4 -> 10 chunks bought 11% for +3 GB).
    constexpr int kMaxChunks = 8;

    // Performance cores only: the stitch waits for the slowest chunk, and a
    // chunk scheduled onto an efficiency core would gate the whole result.
    int performanceCoreCount()
    {
       #if JUCE_MAC
        int cores = 0;
        auto size = sizeof (cores);

        if (sysctlbyname ("hw.perflevel0.logicalcpu", &cores, &size, nullptr, 0) == 0
            && cores > 0)
            return cores;
       #endif

        return (int) std::thread::hardware_concurrency() / 2; // pre-hybrid CPUs
    }

    int chooseNumChunks (int numSamples)
    {
        const auto byCpu = juce::jlimit (1, kMaxChunks, performanceCoreCount());

        // Never regress below the pre-#24 cap of 4, but don't let the wider
        // cap balloon peak RSS either: with the contiguous layer rewrites the
        // workers overlap more, and 8 chunks measured ~15 GB peak on a 194 s
        // song — too much of a 24 GB machine for a plugin that lives beside a
        // DAW session. A quarter of physical RAM in chunks keeps 16 GB
        // machines at today's four and gives bigger machines headroom instead
        // of the last few percent.
        const auto physicalGB = (int) (juce::SystemStats::getMemorySizeInMegabytes() / 1024);
        const auto byMemory = juce::jmax (4, physicalGB / 4);

        const auto byLength = juce::jmax (1, numSamples / kMinSamplesPerChunk);

        return juce::jmin (byCpu, byMemory, byLength);
    }

    // May throw CancelledException (user cancel, via sharedCb) or
    // std::runtime_error (a worker failed).
    Eigen::Tensor3dXf chunkedInference (const demucscpp::demucs_model& model,
                                        const Eigen::MatrixXf& audio,
                                        int numChunks,
                                        const demucscpp::ProgressCallback& sharedCb)
    {
        const auto total = (int) audio.cols();
        const auto numSources = model.is_4sources ? 4 : 6;

        if (numChunks <= 1)
            return demucscpp::demucs_inference (model, audio, sharedCb);

        const auto chunkLen = (total + numChunks - 1) / numChunks;

        // --- build padded chunks ---
        std::vector<Eigen::MatrixXf> chunks;

        for (int i = 0; i < numChunks; ++i)
        {
            const auto start = i * chunkLen;
            const auto end = juce::jmin (total, start + chunkLen);

            Eigen::MatrixXf chunk = Eigen::MatrixXf::Zero (2, end - start + 2 * kOverlapSamples);

            if (i == 0) // repeat the first sample as head padding
                chunk.block (0, 0, 2, kOverlapSamples).colwise() = audio.col (0);
            else
                chunk.block (0, 0, 2, kOverlapSamples) =
                    audio.block (0, start - kOverlapSamples, 2, kOverlapSamples);

            if (i == numChunks - 1) // whatever remains (zero-padded tail)
            {
                const auto remaining = juce::jmin (kOverlapSamples, total - end);
                if (remaining > 0)
                    chunk.block (0, end - start + kOverlapSamples, 2, remaining) =
                        audio.block (0, end, 2, remaining);
            }
            else
            {
                chunk.block (0, end - start + kOverlapSamples, 2, kOverlapSamples) =
                    audio.block (0, end, 2, kOverlapSamples);
            }

            chunk.block (0, kOverlapSamples, 2, end - start) = audio.block (0, start, 2, end - start);
            chunks.push_back (std::move (chunk));
        }

        // --- run workers ---
        std::vector<Eigen::Tensor3dXf> outs ((size_t) numChunks);
        std::vector<float> progress ((size_t) numChunks, 0.0f);
        std::mutex cbMutex;
        std::atomic<bool> cancelled { false };
        std::atomic<bool> failed { false };
        std::string firstError;

        std::vector<std::thread> workers;

        for (int i = 0; i < numChunks; ++i)
        {
            workers.emplace_back ([&, i]
            {
                demucscpp::ProgressCallback chunkCb =
                    [&, i] (float p, const std::string& message)
                {
                    const std::lock_guard<std::mutex> lock (cbMutex);

                    if (cancelled.load())
                        throw CancelledException {};

                    progress[(size_t) i] = p;

                    auto sum = 0.0f;
                    for (auto v : progress)
                        sum += v;

                    sharedCb (sum / (float) numChunks, message); // may throw CancelledException
                };

                try
                {
                    outs[(size_t) i] = demucscpp::demucs_inference (model, chunks[(size_t) i], chunkCb);
                }
                catch (const CancelledException&)
                {
                    cancelled.store (true);
                }
                catch (const std::exception& e)
                {
                    const std::lock_guard<std::mutex> lock (cbMutex);
                    if (firstError.empty())
                        firstError = e.what();
                    failed.store (true);
                    cancelled.store (true); // make siblings bail at their next callback
                }
            });
        }

        for (auto& worker : workers)
            worker.join();

        if (failed.load())
            throw std::runtime_error (firstError.empty() ? "inference failed" : firstError);

        if (cancelled.load())
            throw CancelledException {};

        // --- crossfade-stitch ---
        Eigen::Tensor3dXf result (numSources, 2, total);
        result.setZero();

        std::vector<float> weightSum ((size_t) total, 0.0f);

        for (int i = 0; i < numChunks; ++i)
        {
            const auto start = i * chunkLen;
            const auto paddedLen = (int) outs[(size_t) i].dimension (2);

            for (int j = 0; j < paddedLen; ++j)
            {
                const auto g = start + j - kOverlapSamples;

                if (g < 0 || g >= total)
                    continue;

                auto w = 1.0f;
                if (j < kOverlapSamples)
                    w = (float) (j + 1) / (float) (kOverlapSamples + 1);
                else if (j >= paddedLen - kOverlapSamples)
                    w = (float) (paddedLen - j) / (float) (kOverlapSamples + 1);

                for (int t = 0; t < numSources; ++t)
                    for (int ch = 0; ch < 2; ++ch)
                        result (t, ch, g) += outs[(size_t) i] (t, ch, j) * w;

                weightSum[(size_t) g] += w;
            }
        }

        for (int g = 0; g < total; ++g)
            if (weightSum[(size_t) g] > 0.0f)
                for (int t = 0; t < numSources; ++t)
                    for (int ch = 0; ch < 2; ++ch)
                        result (t, ch, g) /= weightSum[(size_t) g];

        return result;
    }
} // namespace

int DemucsSeparator::expectedSegments (int numSamples)
{
    const auto segment = (int) (demucscpp::SEGMENT_LEN_SECS * demucscpp::SUPPORTED_SAMPLE_RATE);
    const auto stride = (int) ((1.0f - demucscpp::OVERLAP) * (float) segment);
    const auto perPass = juce::jmax (1, (numSamples + stride - 1) / stride);

    return 2 * perPass; // two shift passes
}

bool DemucsSeparator::gpuAvailable()
{
    // Delegated to DemucsMetal, which checks for an Apple-silicon-class GPU
    // on a recent enough macOS (and honours the SPLITTR_NO_GPU test switch);
    // the answer is cached over there.
   #if JUCE_MAC
    return metal::gpuAvailable();
   #else
    return false;
   #endif
}

DemucsSeparator::Status DemucsSeparator::separate (const std::vector<juce::File>& modelFiles,
                                                   const juce::AudioBuffer<float>& stereo44k,
                                                   const std::function<void (float, const juce::String&)>& onProgress,
                                                   const std::function<bool()>& shouldContinue,
                                                   SeparationResult& out,
                                                   juce::String& error,
                                                   ComputeBackend backend)
{
    const bool useGpu = backend == ComputeBackend::gpu && gpuAvailable();
    bool allOnGpu = useGpu; // falsified by any pass that takes the CPU path
    jassert (stereo44k.getNumChannels() == 2);
    jassert (modelFiles.size() == 1 || modelFiles.size() == 4);

    const auto numSamples = stereo44k.getNumSamples();

    if (numSamples <= 0 || modelFiles.empty())
    {
        error = "No audio to separate";
        return Status::failed;
    }

    Eigen::MatrixXf audio (2, numSamples);

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto* src = stereo44k.getReadPointer (ch);

        for (int i = 0; i < numSamples; ++i)
            audio (ch, i) = src[i];
    }

    demucscpp::ProgressCallback progressCallback =
        [&] (float progress, const std::string& message)
    {
        if (shouldContinue != nullptr && ! shouldContinue())
            throw CancelledException {};

        if (onProgress != nullptr)
            onProgress (juce::jlimit (0.0f, 1.0f, progress), juce::String (message));
    };

    const auto numModels = (int) modelFiles.size();
    Eigen::Tensor3dXf targets;
    auto numSources = 0;

    try
    {
        // Load models one at a time so ensemble mode doesn't hold 4 models
        // (~4 x 160 MB) in memory at once.
        for (int m = 0; m < numModels; ++m)
        {
            demucscpp::demucs_model model {};

            if (! demucscpp::load_demucs_model (modelFiles[(size_t) m].getFullPathName().toStdString(), &model))
            {
                error = "Failed to load model: " + modelFiles[(size_t) m].getFullPathName();
                return Status::failed;
            }

            const auto modelSources = model.is_4sources ? 4 : 6;

            // The fine-tuned ensemble is a set of 4-source specialists; there
            // is no 6-source equivalent, so mixing the two is a caller bug.
            if (numModels > 1 && modelSources != 4)
            {
                error = "Expected a 4-source htdemucs model in the fine-tuned ensemble";
                return Status::failed;
            }

            if (m == 0)
                numSources = modelSources;
            else if (modelSources != numSources)
            {
                error = "Model set mixes 4-source and 6-source weights";
                return Status::failed;
            }

            demucscpp::ProgressCallback scopedCallback =
                numModels == 1
                    ? progressCallback
                    : [&, m] (float p, const std::string& message)
                      { progressCallback (((float) m + p) / (float) numModels, message); };

            // GPU seam (issue #24): when useGpu, the Metal backend
            // (DemucsMetal) replaces chunkedInference here — same model, same
            // audio layout, same callback contract (progress in [0,1] and
            // CancelledException out of the callback to cancel). The Metal
            // path reports failure by returning false rather than throwing,
            // so a gpu request can never fail a separation the CPU path
            // could complete; htdemucs_6s always takes the CPU path (the
            // Metal graph is built for the 4-source topology only).
            Eigen::Tensor3dXf modelOut;
            auto ranOnGpu = false;

           #if JUCE_MAC
            if (useGpu && model.is_4sources)
            {
                // MatrixXf (2, N) is col-major == interleaved stereo, and the
                // metal side fills a (sources, 2, N) col-major tensor, so both
                // buffers cross the firewall header raw (no Eigen over there).
                modelOut = Eigen::Tensor3dXf (modelSources, 2, numSamples);
                std::string metalError;

                ranOnGpu = metal::inference (model, audio.data(), numSamples,
                                             modelOut.data(), scopedCallback, metalError);

                if (! ranOnGpu)
                    scopedCallback (0.0f, "Metal backend unavailable, taking the CPU path: " + metalError);
            }
           #else
            juce::ignoreUnused (useGpu);
           #endif

            if (! ranOnGpu)
                modelOut = chunkedInference (model, audio, chooseNumChunks (numSamples), scopedCallback);

            allOnGpu = allOnGpu && ranOnGpu;

            if (numModels == 1)
            {
                targets = std::move (modelOut);
            }
            else
            {
                // Fine-tuned specialist m contributes only target m.
                if (targets.size() == 0)
                {
                    targets = Eigen::Tensor3dXf (numSources, 2, numSamples);
                    targets.setZero();
                }

                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < numSamples; ++i)
                        targets (m, ch, i) = modelOut (m, ch, i);
            }
        }
    }
    catch (const CancelledException&)
    {
        return Status::cancelled;
    }
    catch (const std::exception& e)
    {
        error = juce::String ("Separation failed: ") + e.what();
        return Status::failed;
    }

    // --- lay the separated sources out in display order -------------------
    const auto& order = stemOrderForSources (numSources);
    const auto numStems = (int) order.size();

    out.numSources = numSources;
    out.ranOnGpu = allOnGpu;
    out.kinds = order;
    out.stems.assign ((size_t) numStems, {});
    out.peaksDb.assign ((size_t) numStems, -120.0f);

    std::vector<float> peaks ((size_t) numStems, 0.0f);

    for (auto& buffer : out.stems)
        buffer.setSize (2, numSamples);

    for (int s = 0; s < numStems; ++s)
    {
        const auto kind = order[(size_t) s];
        auto& buffer = out.stems[(size_t) s];

        for (int ch = 0; ch < 2; ++ch)
        {
            auto* dest = buffer.getWritePointer (ch);

            if (kind == stemInstrumental)
            {
                // Everything that isn't the voice, whichever mode we are in:
                // drums + bass + other, plus guitar + piano for htdemucs_6s.
                const auto vocalSource = sourceForStemKind (stemVocals);

                for (int i = 0; i < numSamples; ++i)
                    dest[i] = 0.0f;

                for (int src = 0; src < numSources; ++src)
                {
                    if (src == vocalSource)
                        continue;

                    for (int i = 0; i < numSamples; ++i)
                        dest[i] += targets (src, ch, i);
                }
            }
            else
            {
                const auto src = sourceForStemKind (kind);

                for (int i = 0; i < numSamples; ++i)
                    dest[i] = targets (src, ch, i);
            }

            for (int i = 0; i < numSamples; ++i)
                peaks[(size_t) s] = juce::jmax (peaks[(size_t) s], std::abs (dest[i]));
        }

        out.peaksDb[(size_t) s] = peakToDb (peaks[(size_t) s]);
    }

    return Status::ok;
}

} // namespace splittr
