#include "AuditionPlayer.h"

namespace splittr
{

AuditionPlayer::AuditionPlayer (juce::AudioFormatManager& fm) : formats (fm)
{
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
    temp.setSize (2, hostBlockSize);
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
    if (stemId.load() < 0 || ! prepared)
        return;

    const auto numSamples = buffer.getNumSamples();
    const auto tempSamples = temp.getNumSamples();

    for (int offset = 0; offset < numSamples;)
    {
        const auto chunk = juce::jmin (numSamples - offset, tempSamples);

        juce::AudioSourceChannelInfo info (&temp, 0, chunk);
        transport.getNextAudioBlock (info);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.addFrom (ch, offset, temp, juce::jmin (ch, temp.getNumChannels() - 1), 0, chunk);

        offset += chunk;
    }
}

void AuditionPlayer::play (const juce::File& file, int id)
{
    stop();

    auto* reader = formats.createReaderFor (file);

    if (reader == nullptr)
        return;

    const auto fileRate = reader->sampleRate;

    readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader, true);

    transport.setSource (readerSource.get(), 65536, &readAhead, fileRate);

    if (prepared)
        transport.prepareToPlay (hostBlockSize, hostRate);

    transport.setPosition (0.0);
    transport.start();
    stemId.store (id);
}

void AuditionPlayer::toggle (const juce::File& file, int id)
{
    if (stemId.load() == id)
        stop();
    else
        play (file, id);
}

void AuditionPlayer::stop()
{
    stemId.store (-1);
    transport.stop();
    transport.setSource (nullptr);
    readerSource.reset();
}

void AuditionPlayer::seekFraction (double fraction01)
{
    if (stemId.load() >= 0)
        transport.setPosition (juce::jlimit (0.0, 1.0, fraction01) * transport.getLengthInSeconds());
}

void AuditionPlayer::stopIfFinished()
{
    if (stemId.load() >= 0 && transport.hasStreamFinished())
        stop();
}

double AuditionPlayer::positionSeconds() const
{
    return stemId.load() >= 0 ? transport.getCurrentPosition() : 0.0;
}

double AuditionPlayer::lengthSeconds() const
{
    return stemId.load() >= 0 ? transport.getLengthInSeconds() : 0.0;
}

} // namespace splittr
