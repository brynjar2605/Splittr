#include "DemucsSeparator.h"

#include <model.hpp>

#include <mutex>
#include <thread>

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
    constexpr int kMinSamplesPerChunk = 30 * 44100; // don't over-chunk short files
    constexpr int kMaxChunks = 4;

    int chooseNumChunks (int numSamples)
    {
        const auto byCpu = juce::jlimit (1, kMaxChunks,
                                         (int) std::thread::hardware_concurrency() / 2);
        const auto byLength = juce::jmax (1, numSamples / kMinSamplesPerChunk);
        return juce::jmin (byCpu, byLength);
    }

    // May throw CancelledException (user cancel, via sharedCb) or
    // std::runtime_error (a worker failed).
    Eigen::Tensor3dXf chunkedInference (const demucscpp::demucs_model& model,
                                        const Eigen::MatrixXf& audio,
                                        int numChunks,
                                        const demucscpp::ProgressCallback& sharedCb)
    {
        const auto total = (int) audio.cols();

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
        Eigen::Tensor3dXf result (4, 2, total);
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

                for (int t = 0; t < 4; ++t)
                    for (int ch = 0; ch < 2; ++ch)
                        result (t, ch, g) += outs[(size_t) i] (t, ch, j) * w;

                weightSum[(size_t) g] += w;
            }
        }

        for (int g = 0; g < total; ++g)
            if (weightSum[(size_t) g] > 0.0f)
                for (int t = 0; t < 4; ++t)
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

DemucsSeparator::Status DemucsSeparator::separate (const std::vector<juce::File>& modelFiles,
                                                   const juce::AudioBuffer<float>& stereo44k,
                                                   const std::function<void (float, const juce::String&)>& onProgress,
                                                   const std::function<bool()>& shouldContinue,
                                                   SeparationResult& out,
                                                   juce::String& error)
{
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

            if (! model.is_4sources)
            {
                error = "Expected a 4-source htdemucs model";
                return Status::failed;
            }

            demucscpp::ProgressCallback scopedCallback =
                numModels == 1
                    ? progressCallback
                    : [&, m] (float p, const std::string& message)
                      { progressCallback (((float) m + p) / (float) numModels, message); };

            auto modelOut = chunkedInference (model, audio, chooseNumChunks (numSamples), scopedCallback);

            if (numModels == 1)
            {
                targets = std::move (modelOut);
            }
            else
            {
                // Fine-tuned specialist m contributes only target m.
                if (targets.size() == 0)
                {
                    targets = Eigen::Tensor3dXf (4, 2, numSamples);
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

    out.vocals.setSize (2, numSamples);
    out.instrumental.setSize (2, numSamples);

    std::array<float, 4> peaks {};

    for (int ch = 0; ch < 2; ++ch)
    {
        auto* vocals = out.vocals.getWritePointer (ch);
        auto* instrumental = out.instrumental.getWritePointer (ch);

        for (int i = 0; i < numSamples; ++i)
        {
            const float drums = targets (0, ch, i);
            const float bass = targets (1, ch, i);
            const float other = targets (2, ch, i);
            const float vox = targets (3, ch, i);

            vocals[i] = vox;
            instrumental[i] = drums + bass + other;

            peaks[0] = juce::jmax (peaks[0], std::abs (drums));
            peaks[1] = juce::jmax (peaks[1], std::abs (bass));
            peaks[2] = juce::jmax (peaks[2], std::abs (other));
            peaks[3] = juce::jmax (peaks[3], std::abs (vox));
        }
    }

    for (size_t s = 0; s < peaks.size(); ++s)
        out.sourcePeaksDb[s] = peakToDb (peaks[s]);

    return Status::ok;
}

} // namespace splittr
