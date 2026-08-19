#include "AudioFileIO.h"

namespace splittr
{

AudioFileIO::AudioFileIO()
{
    // WAV/AIFF/FLAC/OGG everywhere; CoreAudio codecs (mp3, m4a, ...) on macOS.
    formatManager.registerBasicFormats();
}

bool AudioFileIO::decodeToStereo44k (const juce::File& file, DecodedAudio& out, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr)
    {
        error = "Unsupported or unreadable audio file: " + file.getFileName();
        return false;
    }

    const auto numSourceSamples = (int) reader->lengthInSamples;
    const auto sourceRate = reader->sampleRate;

    if (numSourceSamples <= 0 || sourceRate <= 0.0)
    {
        error = "File contains no audio: " + file.getFileName();
        return false;
    }

    juce::AudioBuffer<float> source (2, numSourceSamples);

    if (reader->numChannels == 1)
    {
        reader->read (&source, 0, numSourceSamples, 0, true, false);
        source.copyFrom (1, 0, source, 0, 0, numSourceSamples);
    }
    else
    {
        reader->read (&source, 0, numSourceSamples, 0, true, true);
    }

    out.originalSampleRate = sourceRate;
    out.durationSeconds = numSourceSamples / sourceRate;

    if (juce::approximatelyEqual (sourceRate, kTargetSampleRate))
    {
        out.buffer = std::move (source);
        return true;
    }

    const double speedRatio = sourceRate / kTargetSampleRate; // input samples per output sample
    const auto numOutSamples = (int) std::ceil (numSourceSamples / speedRatio);

    out.buffer.setSize (2, numOutSamples);

    for (int ch = 0; ch < 2; ++ch)
    {
        juce::LagrangeInterpolator interpolator;
        interpolator.process (speedRatio,
                              source.getReadPointer (ch),
                              out.buffer.getWritePointer (ch),
                              numOutSamples,
                              numSourceSamples,
                              0);
    }

    return true;
}

bool AudioFileIO::writeWavFloat32 (const juce::File& file,
                                   const juce::AudioBuffer<float>& buffer,
                                   double sampleRate,
                                   juce::String& error)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();

    auto stream = file.createOutputStream();

    if (stream == nullptr)
    {
        error = "Cannot write to " + file.getFullPathName();
        return false;
    }

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wavFormat.createWriterFor (stream.get(), sampleRate,
                                   (unsigned int) buffer.getNumChannels(),
                                   32, {}, 0));

    if (writer == nullptr)
    {
        error = "Cannot create WAV writer for " + file.getFullPathName();
        return false;
    }

    stream.release(); // writer owns it now

    if (! writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples()))
    {
        error = "Failed writing samples to " + file.getFullPathName();
        return false;
    }

    return true;
}

std::vector<float> AudioFileIO::peakColumns (const juce::AudioBuffer<float>& buffer, int numColumns)
{
    std::vector<float> columns ((size_t) numColumns, 0.0f);

    const auto numSamples = buffer.getNumSamples();

    if (numSamples <= 0 || numColumns <= 0)
        return columns;

    for (int col = 0; col < numColumns; ++col)
    {
        const auto start = (int) ((juce::int64) col * numSamples / numColumns);
        const auto end = (int) ((juce::int64) (col + 1) * numSamples / numColumns);
        const auto length = juce::jmax (1, end - start);

        float peak = 0.0f;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            peak = juce::jmax (peak, buffer.getMagnitude (ch, start, juce::jmin (length, numSamples - start)));

        columns[(size_t) col] = juce::jlimit (0.0f, 1.0f, peak);
    }

    return columns;
}

} // namespace splittr
