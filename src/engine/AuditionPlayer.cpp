#include "AuditionPlayer.h"

namespace splittr
{

// ---------------------------------------------------------------------------
// Five stereo readers as one 10-channel positionable stream. Every method runs
// either on the message thread (open/prepare, before the transport is handed
// the source) or on the transport's read-ahead TimeSliceThread -- never on the
// audio thread, which sees only the buffered result.
class AuditionPlayer::StemMixerSource : public juce::PositionableAudioSource
{
public:
    StemMixerSource() = default;

    // Returns false when not a single stem could be opened.
    bool open (juce::AudioFormatManager& fm, const StemFiles& files)
    {
        for (int i = 0; i < numStems; ++i)
        {
            if (auto* reader = fm.createReaderFor (files[(size_t) i]))
            {
                readers[(size_t) i] = std::make_unique<juce::AudioFormatReaderSource> (reader, true);
                length = juce::jmax (length, reader->lengthInSamples);
                rate = juce::jmax (rate, reader->sampleRate);
            }
        }

        return rate > 0.0 && length > 0;
    }

    double sourceSampleRate() const { return rate; }

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override
    {
        for (auto& r : readers)
            if (r != nullptr)
                r->prepareToPlay (samplesPerBlockExpected, sampleRate);
    }

    void releaseResources() override
    {
        for (auto& r : readers)
            if (r != nullptr)
                r->releaseResources();
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        const auto numOut = info.buffer->getNumChannels();

        for (int s = 0; s < numStems; ++s)
        {
            const auto left = s * 2, right = s * 2 + 1;

            if (readers[(size_t) s] == nullptr || right >= numOut)
            {
                for (int ch = left; ch <= juce::jmin (right, numOut - 1); ++ch)
                    info.buffer->clear (ch, info.startSample, info.numSamples);

                continue;
            }

            // A non-owning two-channel view onto this stem's slot: no copy, no
            // allocation (AudioBuffer keeps <= 32 channel pointers inline).
            float* channels[2] = { info.buffer->getWritePointer (left) + info.startSample,
                                   info.buffer->getWritePointer (right) + info.startSample };

            juce::AudioBuffer<float> view (channels, 2, info.numSamples);
            const juce::AudioSourceChannelInfo slot (&view, 0, info.numSamples);

            readers[(size_t) s]->getNextAudioBlock (slot);
        }

        // Every reader advances by numSamples whether or not it ran out of
        // file, so the stems can never drift apart.
        position.fetch_add (info.numSamples);
    }

    void setNextReadPosition (juce::int64 newPosition) override
    {
        position.store (newPosition);

        for (auto& r : readers)
            if (r != nullptr)
                r->setNextReadPosition (newPosition);
    }

    juce::int64 getNextReadPosition() const override { return position.load(); }
    juce::int64 getTotalLength() const override { return length; }
    bool isLooping() const override { return false; }
    void setLooping (bool) override {}

private:
    std::array<std::unique_ptr<juce::AudioFormatReaderSource>, numStems> readers;
    std::atomic<juce::int64> position { 0 };
    juce::int64 length = 0;
    double rate = 0.0;
};

// ---------------------------------------------------------------------------

AuditionPlayer::AuditionPlayer (juce::AudioFormatManager& fm) : formats (fm)
{
    for (auto& m : muted)
        m.store (false);

    for (auto& g : gain)
        g.setCurrentAndTargetValue (1.0f);

    readAhead.startThread (juce::Thread::Priority::normal);
}

AuditionPlayer::~AuditionPlayer()
{
    stop();
    readAhead.stopThread (2000);
}

void AuditionPlayer::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hostRate = sampleRate;
    hostBlockSize = juce::jmax (16, samplesPerBlock);
    temp.setSize (mixChannels, hostBlockSize);

    for (int i = 0; i < numStems; ++i)
    {
        gain[(size_t) i].reset (hostRate, muteRampSeconds);
        gain[(size_t) i].setCurrentAndTargetValue (muted[(size_t) i].load() ? 0.0f : 1.0f);
    }

    transport.prepareToPlay (hostBlockSize, hostRate);
    prepared = true;
}

void AuditionPlayer::releaseResources()
{
    prepared = false;
    transport.releaseResources();
}

void AuditionPlayer::processAddTo (juce::AudioBuffer<float>& buffer)
{
    if (! prepared || ! loaded.load (std::memory_order_relaxed))
        return;

    const auto numSamples = buffer.getNumSamples();
    const auto numOut = buffer.getNumChannels();
    const auto tempSamples = temp.getNumSamples();

    for (int s = 0; s < numStems; ++s)
        gain[(size_t) s].setTargetValue (muted[(size_t) s].load (std::memory_order_relaxed) ? 0.0f : 1.0f);

    for (int offset = 0; offset < numSamples;)
    {
        const auto chunk = juce::jmin (numSamples - offset, tempSamples);

        const juce::AudioSourceChannelInfo info (&temp, 0, chunk);
        transport.getNextAudioBlock (info);

        // Gains are applied in short sub-chunks so the mute ramp lasts the same
        // ~10ms whatever block size the host hands us.
        for (int sub = 0; sub < chunk;)
        {
            const auto n = juce::jmin (chunk - sub, gainChunkSamples);

            for (int s = 0; s < numStems; ++s)
            {
                // One ramp step per stem, shared by every output channel.
                const auto startGain = gain[(size_t) s].getCurrentValue();
                gain[(size_t) s].skip (n);
                const auto endGain = gain[(size_t) s].getCurrentValue();

                if (startGain <= 0.0f && endGain <= 0.0f)
                    continue;

                for (int ch = 0; ch < numOut; ++ch)
                    buffer.addFromWithRamp (ch, offset + sub,
                                            temp.getReadPointer (s * 2 + juce::jmin (ch, 1)) + sub,
                                            n, startGain, endGain);
            }

            sub += n;
        }

        offset += chunk;
    }
}

bool AuditionPlayer::load (const StemFiles& files)
{
    if (loaded.load() && openFiles == files)
        return true;

    stop();

    auto source = std::make_unique<StemMixerSource>();

    if (! source->open (formats, files))
        return false;

    const auto fileRate = source->sourceSampleRate();
    mixer = std::move (source);

    transport.setSource (mixer.get(), readAheadSamples, &readAhead, fileRate, mixChannels);

    if (prepared)
        transport.prepareToPlay (hostBlockSize, hostRate);

    transport.setPosition (0.0);

    for (int i = 0; i < numStems; ++i)
        gain[(size_t) i].setCurrentAndTargetValue (muted[(size_t) i].load() ? 0.0f : 1.0f);

    openFiles = files;
    loaded.store (true);
    return true;
}

void AuditionPlayer::play (const StemFiles& files)
{
    if (load (files))
        transport.start();
}

void AuditionPlayer::togglePlay (const StemFiles& files)
{
    if (transport.isPlaying())
        transport.stop(); // pause: keep the readers open and the position put
    else
        play (files);
}

void AuditionPlayer::stop()
{
    loaded.store (false);
    transport.stop();
    transport.setSource (nullptr);
    mixer.reset();
    openFiles = {};
}

void AuditionPlayer::seekFraction (double fraction01)
{
    if (loaded.load())
        transport.setPosition (juce::jlimit (0.0, 1.0, fraction01) * transport.getLengthInSeconds());
}

void AuditionPlayer::stopIfFinished()
{
    if (loaded.load() && transport.hasStreamFinished())
        stop();
}

void AuditionPlayer::setMuted (int stem, bool shouldBeMuted)
{
    if (juce::isPositiveAndBelow (stem, (int) numStems))
        muted[(size_t) stem].store (shouldBeMuted);
}

bool AuditionPlayer::isMuted (int stem) const
{
    return juce::isPositiveAndBelow (stem, (int) numStems) && muted[(size_t) stem].load();
}

bool AuditionPlayer::isPlaying() const
{
    return loaded.load() && transport.isPlaying();
}

double AuditionPlayer::positionSeconds() const
{
    return loaded.load() ? transport.getCurrentPosition() : 0.0;
}

double AuditionPlayer::lengthSeconds() const
{
    return loaded.load() ? transport.getLengthInSeconds() : 0.0;
}

} // namespace splittr
