#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "SpectrumTap.h"
#include "StemCache.h" // maxStems
#include "StemFx.h"

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

    // Per-stem sound shaping, heard while it is being dialled in (issue #9).
    // Cheap to call at slider rate: unchanged params are dropped here, and the
    // audio thread picks the rest up at the top of its next block.
    void setStemFx (int stem, const StemFxParams&);

    // Live spectrum for the TUNE panel (issue #25). The analyzer follows one
    // stem at a time -- the one being tuned -- and -1 parks it. A parked tap
    // costs the audio thread one branch per chunk and nothing else, so the
    // panel points it at a stem when it opens and parks it when it closes.
    void setSpectrumStem (int stemIndex);
    int spectrumStem() const { return spectrumStemIndex.load (std::memory_order_relaxed); }

    // The two measurement points around the tapped stem's chain: `pre` is the
    // stem as it was rendered, `post` is what the shaping made of it. Both sit
    // before the mute ramp, because the panel is showing what the EQ is doing,
    // not whether the stem is currently audible.
    //
    // Message thread: these hand out the consumer end of the taps (read()),
    // never the producer end.
    SpectrumTap& preSpectrum() { return spectrumPre; }
    SpectrumTap& postSpectrum() { return spectrumPost; }

    // Both taps in one call, which is all the panel ever wants. They run in
    // lockstep -- same samples, same hop boundaries -- so in practice a fresh
    // frame on one means a fresh frame on the other; the flags say so rather
    // than assuming it. A few KB: hold it, do not build it per paint.
    struct SpectrumFrames
    {
        SpectrumTap::Frame pre, post;
        bool hasPre = false, hasPost = false;
    };

    bool readSpectrum (SpectrumFrames&);

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

    // Shaping runs on the audio thread, not in the mixer source: the transport
    // reads ~1.5 s ahead, so a chain in there would only be heard that far into
    // the future — a tweak has to land on the next block or it is not audition
    // (issue #9).
    //
    // The message thread writes `pendingFx` under `fxLock` and bumps
    // `fxChanges`; the audio thread only tryEnters, so a slider drag can never
    // stall it — a failed grab just means the change lands one block later.
    std::array<StemFxChain, maxStems> chains;
    std::array<StemFxParams, maxStems> pendingFx;
    juce::CriticalSection fxLock;
    std::atomic<int> fxChanges { 0 };
    int fxSeen = 0; // audio thread's copy of the counter

    // Filter and envelope state that a jump (or a fresh load) has invalidated,
    // cleared where it is safe to touch: on the audio thread.
    std::atomic<bool> fxNeedsReset { false };

    // The analyzer is a measurement, not a stage: it reads the samples the
    // chain is about to see and the ones it produced, and changes neither.
    // `spectrumStemSeen` is the audio thread's own copy of the target, the same
    // trick as `fxSeen` -- when it moves, both taps drop their half-window of
    // history so the outgoing stem cannot smear into the new one's first frame.
    SpectrumTap spectrumPre, spectrumPost;
    std::atomic<int> spectrumStemIndex { -1 };
    int spectrumStemSeen = -1;

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
