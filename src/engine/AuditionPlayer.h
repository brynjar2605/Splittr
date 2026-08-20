#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "StemCache.h" // maxStems

namespace splittr
{

// In-plugin stem audition: every rendered WAV of the current run plays at once,
// sample-locked, through the plugin's own transport (never the host's), mixed
// additively into processBlock.
//
// The stems are rendered from the same source, so they are identical in length
// and rate: reading them as one interleaved multi-channel stream keeps them
// locked for free. StemMixerSource presents the stereo stems as a single
// multi-channel PositionableAudioSource; AudioTransportSource wraps it in a
// BufferingAudioSource so every disk read happens on the background
// TimeSliceThread. The audio thread only pulls the ready-made block and applies
// the per-stem mute gains, which is why muting is instant instead of lagging by
// the read-ahead buffer.
//
// The stream is always laid out for the worst case (maxStems = 7, i.e. 14
// channels) so the buffer never has to be resized under a running transport;
// slots past the loaded stem count are simply cleared and skipped.
class AuditionPlayer
{
public:
    explicit AuditionPlayer (juce::AudioFormatManager&);
    ~AuditionPlayer();

    // Display order, as published in the snapshot; 5 files in the 4-source
    // modes, 7 in 6-stem. Longer lists are truncated to maxStems.
    using StemFiles = std::vector<juce::File>;

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
    // The stereo stems presented as one multi-channel stream (stem s occupies
    // channels 2s and 2s+1). Read on the transport's background thread.
    class StemMixerSource;

    bool load (const StemFiles&); // no-op when the same files are already open

    juce::AudioFormatManager& formats;
    juce::TimeSliceThread readAhead { "Splittr Audition" };
    juce::AudioTransportSource transport;
    std::unique_ptr<StemMixerSource> mixer;
    StemFiles openFiles;
    juce::AudioBuffer<float> temp; // mixChannels, one host block

    std::array<std::atomic<bool>, maxStems> muted {};
    std::array<juce::SmoothedValue<float>, maxStems> gain;

    // How many stems the loaded set actually has; read on the audio thread.
    std::atomic<int> activeStems { 0 };

    std::atomic<bool> loaded { false };
    double hostRate = 44100.0;
    int hostBlockSize = 512;
    bool prepared = false;

    static constexpr int mixChannels = maxStems * 2; // 14
    static constexpr int readAheadSamples = 65536;
    static constexpr double muteRampSeconds = 0.010;
    static constexpr int gainChunkSamples = 64; // ramp resolution, host-block independent

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuditionPlayer)
};

} // namespace splittr
