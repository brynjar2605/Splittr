#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <memory>

#include "StemCache.h" // numStems

namespace splittr
{

// In-plugin stem audition: all five rendered WAVs play at once, sample-locked,
// through the plugin's own transport (never the host's), mixed additively into
// processBlock.
//
// The five stems are rendered from the same source, so they are identical in
// length and rate: reading them as one interleaved multi-channel stream keeps
// them locked for free. StemMixerSource presents the five stereo stems as a
// single 10-channel PositionableAudioSource; AudioTransportSource wraps it in a
// 10-channel BufferingAudioSource so every disk read happens on the background
// TimeSliceThread. The audio thread only pulls the ready-made block and applies
// the per-stem mute gains, which is why muting is instant instead of lagging by
// the read-ahead buffer.
class AuditionPlayer
{
public:
    explicit AuditionPlayer (juce::AudioFormatManager&);
    ~AuditionPlayer();

    using StemFiles = std::array<juce::File, numStems>;

    // audio thread
    void prepareToPlay (double sampleRate, int samplesPerBlock);
    void releaseResources();
    void processAddTo (juce::AudioBuffer<float>&);

    // message thread
    void play (const StemFiles&);       // load if needed, then run the transport
    void togglePlay (const StemFiles&); // pause if running, else play()
    void stop();                        // stop and unload
    void seekFraction (double fraction01);
    void stopIfFinished();

    void setMuted (int stem, bool);
    bool isMuted (int stem) const;

    bool isLoaded() const { return loaded.load(); }
    bool isPlaying() const;

    double positionSeconds() const;
    double lengthSeconds() const;

private:
    // Five stereo stems presented as one 10-channel stream (stem s occupies
    // channels 2s and 2s+1). Read on the transport's background thread.
    class StemMixerSource;

    bool load (const StemFiles&); // no-op when the same files are already open

    juce::AudioFormatManager& formats;
    juce::TimeSliceThread readAhead { "Splittr Audition" };
    juce::AudioTransportSource transport;
    std::unique_ptr<StemMixerSource> mixer;
    StemFiles openFiles;
    juce::AudioBuffer<float> temp; // numStems * 2 channels, one host block

    std::array<std::atomic<bool>, numStems> muted {};
    std::array<juce::SmoothedValue<float>, numStems> gain;

    std::atomic<bool> loaded { false };
    double hostRate = 44100.0;
    int hostBlockSize = 512;
    bool prepared = false;

    static constexpr int mixChannels = numStems * 2;
    static constexpr int readAheadSamples = 65536;
    static constexpr double muteRampSeconds = 0.010;
    static constexpr int gainChunkSamples = 64; // ramp resolution, host-block independent

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuditionPlayer)
};

} // namespace splittr
