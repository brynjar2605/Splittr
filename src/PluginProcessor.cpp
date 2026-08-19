#include "PluginProcessor.h"
#include "PluginEditor.h"

SplittrAudioProcessor::SplittrAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

void SplittrAudioProcessor::prepareToPlay (double, int) {}
void SplittrAudioProcessor::releaseResources() {}

bool SplittrAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void SplittrAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Pass-through utility: input stays untouched; clear any surplus outputs.
    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
}

juce::AudioProcessorEditor* SplittrAudioProcessor::createEditor()
{
    return new SplittrAudioProcessorEditor (*this);
}

void SplittrAudioProcessor::getStateInformation (juce::MemoryBlock&) {}
void SplittrAudioProcessor::setStateInformation (const void*, int) {}

// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SplittrAudioProcessor();
}
