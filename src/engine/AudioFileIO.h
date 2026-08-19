#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

namespace splittr
{

struct DecodedAudio
{
    juce::AudioBuffer<float> buffer; // stereo, 44100 Hz
    double originalSampleRate = 0.0;
    double durationSeconds = 0.0;
};

class AudioFileIO
{
public:
    AudioFileIO();

    bool decodeToStereo44k (const juce::File& file, DecodedAudio& out, juce::String& error);

    bool writeWavFloat32 (const juce::File& file,
                          const juce::AudioBuffer<float>& buffer,
                          double sampleRate,
                          juce::String& error);

    // Downsampled |peak| per column, 0..1 — for the UI waveforms.
    static std::vector<float> peakColumns (const juce::AudioBuffer<float>& buffer, int numColumns);

    juce::AudioFormatManager& formats() { return formatManager; }

    static constexpr double kTargetSampleRate = 44100.0;

private:
    juce::AudioFormatManager formatManager;
};

} // namespace splittr
