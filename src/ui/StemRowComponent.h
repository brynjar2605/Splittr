#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <vector>

namespace splittr
{

// One draggable stem row (done state): grip dot-grid, title/subtitle,
// play/scrub transport, 70-column waveform. Press-and-drag anywhere that
// isn't the transport starts an external file drag of the rendered WAV.
class StemRowComponent : public juce::Component
{
public:
    enum class Kind { vocals, instrumental };

    explicit StemRowComponent (Kind);

    std::function<void()> onPlayToggle;
    std::function<void (double)> onSeek; // fraction 0..1

    void update (const juce::File& wav, const std::vector<float>& waveform,
                 bool isPlaying, double positionSeconds, double lengthSeconds);

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> playButtonBounds() const;
    juce::Rectangle<int> scrubberBounds() const;

    const Kind kind;

    juce::File wavFile;
    std::vector<float> wave;
    bool playing = false;
    double position = 0.0, length = 0.0;

    bool hovered = false;
    bool draggingOut = false;
    bool scrubbing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemRowComponent)
};

} // namespace splittr
