#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "engine/SeparationEngine.h"
#include "engine/AuditionPlayer.h"

class SplittrAudioProcessor : public juce::AudioProcessor
{
public:
    SplittrAudioProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    splittr::SeparationEngine& getEngine() { return engine; }
    splittr::AuditionPlayer& getAudition() { return audition; }

    float getEditorScale() const { return editorScale.load(); }
    void setEditorScale (float s) { editorScale.store (s); }

private:
    splittr::SeparationEngine engine;
    splittr::AuditionPlayer audition { engine.fileIO().formats() };
    std::atomic<float> editorScale { 1.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplittrAudioProcessor)
};
