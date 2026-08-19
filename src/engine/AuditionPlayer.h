#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

namespace splittr
{

// In-plugin stem audition: plays one rendered WAV at a time through the
// plugin's own transport (never the host's), mixed additively into
// processBlock. File reading happens on a background TimeSliceThread; the
// audio thread only pulls from the buffered transport source.
class AuditionPlayer
{
public:
    explicit AuditionPlayer (juce::AudioFormatManager&);
    ~AuditionPlayer();

    // audio thread
    void prepareToPlay (double sampleRate, int samplesPerBlock);
    void releaseResources();
    void processAddTo (juce::AudioBuffer<float>&);

    // message thread
    void toggle (const juce::File&, int stemId); // play, or stop if already playing this stem
    void stop();
    void seekFraction (double fraction01);
    void stopIfFinished();

    int currentStemId() const { return stemId.load(); } // -1 = idle
    double positionSeconds() const;
    double lengthSeconds() const;

private:
    void play (const juce::File&, int id);

    juce::AudioFormatManager& formats;
    juce::TimeSliceThread readAhead { "Splittr Audition" };
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioBuffer<float> temp;

    std::atomic<int> stemId { -1 };
    double hostRate = 44100.0;
    int hostBlockSize = 512;
    bool prepared = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuditionPlayer)
};

} // namespace splittr
