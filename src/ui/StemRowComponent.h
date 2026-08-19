#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <vector>

namespace splittr
{

// One draggable stem row (done state, compact 53px variant): grip dot-grid,
// name + peak, play/scrub transport, waveform. Press-and-drag anywhere that
// isn't the transport starts an external file drag of the rendered WAV.
class StemRowComponent : public juce::Component
{
public:
    StemRowComponent (juce::String displayName, bool accented);

    std::function<void()> onPlayToggle;
    std::function<void (double)> onSeek; // fraction 0..1

    void update (const juce::File& wav, const std::vector<float>& waveform, float peakDb,
                 bool isPlaying, double positionSeconds, double lengthSeconds);

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    static constexpr int rowHeight = 53;

private:
    juce::Rectangle<int> playButtonBounds() const;
    juce::Rectangle<int> scrubberBounds() const;

    const juce::String name;
    const bool accent;

    juce::File wavFile;
    std::vector<float> wave;
    float peakDb = -120.0f;
    bool playing = false;
    double position = 0.0, length = 0.0;

    bool hovered = false;
    bool draggingOut = false;
    bool scrubbing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemRowComponent)
};

} // namespace splittr
