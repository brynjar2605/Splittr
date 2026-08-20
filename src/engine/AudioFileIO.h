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

    // `metadata` is keyed on juce::WavAudioFormat's property names; whatever
    // the format recognises lands in the corresponding chunk (LIST/INFO for
    // the riffInfo* keys) and the rest is ignored.
    bool writeWavFloat32 (const juce::File& file,
                          const juce::AudioBuffer<float>& buffer,
                          double sampleRate,
                          juce::String& error,
                          const juce::StringPairArray& metadata = {});

    // Downsampled waveform columns, 0..1 — for the UI waveforms. Per column
    // RMS, normalised to the buffer's loudest column and sqrt-mapped so loud
    // (limited) material still shows dynamics instead of a solid block.
    static std::vector<float> peakColumns (const juce::AudioBuffer<float>& buffer, int numColumns);

    juce::AudioFormatManager& formats() { return formatManager; }

    static constexpr double kTargetSampleRate = 44100.0;

private:
    juce::AudioFormatManager formatManager;
};

} // namespace splittr
