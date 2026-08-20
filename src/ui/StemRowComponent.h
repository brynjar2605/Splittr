#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <vector>

namespace splittr
{

// One draggable stem row (done state, compact 53px variant): grip dot-grid,
// name + peak, mute toggle + scrubber, waveform. Press-and-drag anywhere that
// isn't the transport starts an external file drag of the rendered WAV.
//
// All five stems share one transport, so the row only reports intent: the
// circle toggles this stem's mute, a plain click on the waveform (mouse-up
// under 5px of travel) seeks the shared transport, and the 60px scrubber
// drag-scrubs it. Anything with real travel is still the file drag-out.
class StemRowComponent : public juce::Component
{
public:
    StemRowComponent (juce::String displayName, bool accented);

    std::function<void()> onMuteToggle;
    std::function<void (double)> onSeek; // fraction 0..1

    void update (const juce::File& wav, const std::vector<float>& waveform, float peakDb,
                 bool isMuted, bool isPlaying, double positionSeconds, double lengthSeconds,
                 double emberClock);

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    static constexpr int rowHeight = 53;

private:
    juce::Rectangle<int> muteButtonBounds() const;
    juce::Rectangle<int> scrubberBounds() const;
    juce::Rectangle<int> waveformBounds() const;

    const juce::String name;
    const bool accent;
    const int emberRow; // per-row ember seed, so the five rows don't flicker in step

    juce::File wavFile;
    std::vector<float> wave;
    float peakDb = -120.0f;
    bool muted = false;
    bool playing = false;
    double position = 0.0, length = 0.0;
    double clock = 0.0;

    bool hovered = false;
    bool draggingOut = false;
    bool scrubbing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemRowComponent)
};

} // namespace splittr
