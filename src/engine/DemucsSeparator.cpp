#include "DemucsSeparator.h"

#include <model.hpp>

namespace splittr
{

namespace
{
    struct CancelledException final : public std::exception {};

    float peakToDb (float peak)
    {
        return juce::Decibels::gainToDecibels (peak, -120.0f);
    }
} // namespace

DemucsSeparator::Status DemucsSeparator::separate (const juce::File& modelFile,
                                                   const juce::AudioBuffer<float>& stereo44k,
                                                   const std::function<void (float, const juce::String&)>& onProgress,
                                                   const std::function<bool()>& shouldContinue,
                                                   SeparationResult& out,
                                                   juce::String& error)
{
    jassert (stereo44k.getNumChannels() == 2);

    const auto numSamples = stereo44k.getNumSamples();

    if (numSamples <= 0)
    {
        error = "No audio to separate";
        return Status::failed;
    }

    demucscpp::demucs_model model {};

    if (! demucscpp::load_demucs_model (modelFile.getFullPathName().toStdString(), &model))
    {
        error = "Failed to load model: " + modelFile.getFullPathName();
        return Status::failed;
    }

    if (! model.is_4sources)
    {
        error = "Expected a 4-source htdemucs model";
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

    Eigen::Tensor3dXf targets;

    try
    {
        targets = demucscpp::demucs_inference (model, audio, progressCallback);
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
