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
                                   juce::String& error,
                                   const juce::StringPairArray& metadata)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();

    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();

    if (stream == nullptr)
    {
        error = "Cannot write to " + file.getFullPathName();
        return false;
    }

    auto options = juce::AudioFormatWriterOptions {}
                       .withSampleRate (sampleRate)
                       .withNumChannels (buffer.getNumChannels())
                       .withBitsPerSample (32)
                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    for (const auto& key : metadata.getAllKeys())
        options = options.withMetadata (key, metadata[key]);

    juce::WavAudioFormat wavFormat;
    auto writer = wavFormat.createWriterFor (stream, options);

    if (writer == nullptr)
    {
        error = "Cannot create WAV writer for " + file.getFullPathName();
        return false;
    }

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
    const auto numChannels = buffer.getNumChannels();

    if (numSamples <= 0 || numColumns <= 0 || numChannels <= 0)
        return columns;

    float maxColumn = 0.0f;

    for (int col = 0; col < numColumns; ++col)
    {
        const auto start = (int) ((juce::int64) col * numSamples / numColumns);
        const auto end = (int) ((juce::int64) (col + 1) * numSamples / numColumns);
        const auto length = juce::jlimit (1, numSamples - start, end - start);

        // RMS rather than |peak|: a column spans seconds, and a limited master
        // touches full scale inside every one of them, so peak columns render
        // as a solid slab. RMS still follows the arrangement.
        double energy = 0.0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const double rms = buffer.getRMSLevel (ch, start, length);
            energy += rms * rms;
        }

        const auto value = (float) std::sqrt (energy / (double) numChannels);

        columns[(size_t) col] = value;
        maxColumn = juce::jmax (maxColumn, value);
    }

    if (maxColumn <= 1.0e-6f)
        return columns; // silent buffer: flat line

    // Normalise to this buffer's own loudest column, then sqrt-map (perceptual)
    // so quiet passages keep visible height. Normalisation is per buffer, so
    // the source is normalised to itself and each stem to its own loudest
    // column - stem rows are not level-comparable to each other.
    for (auto& c : columns)
        c = std::sqrt (juce::jlimit (0.0f, 1.0f, c / maxColumn));

    return columns;
}

} // namespace splittr
