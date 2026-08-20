#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <vector>

namespace splittr
{

// One draggable stem row (done state): grip dot-grid, name + peak, mute toggle
// + scrubber, waveform. Press-and-drag anywhere that isn't the transport starts
// an external file drag of the rendered WAV.
//
// The row renders to whatever height DoneView gives it — 53px for the five-stem
// set, ~40px when seven have to fit. The 16px title strip is fixed; the
// waveform takes what is left, and the vertical padding tightens below 46px.
//
// All stems share one transport, so the row only reports intent: the circle
// toggles this stem's mute, a plain click on the waveform (mouse-up under 5px
// of travel) seeks the shared transport, and the 60px scrubber drag-scrubs it.
// Anything with real travel is still the file drag-out.
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

    // The tallest row DoneView ever hands out, and the point below which the
    // padding tightens to keep the waveform legible.
    static constexpr int maxRowHeight = 53;
    static constexpr int compactBelow = 46;

    // True while a file drag this plugin started is still in flight. The editor
    // checks this to refuse file drops, so a stem dragged out and released over
    // the plugin window cannot discard the current stems (issue #8).
    static bool isExternalDragInProgress();

    // Bracket one external file drag. The rows hold this around their own
    // drag-out and DoneView's DRAG ALL holds it around the multi-file drag, so
    // both gestures raise the one guard instead of keeping two flags in step.
    // Counted, not flagged: it lifts when the last drag in flight ends.
    static void beginExternalDrag();
    static void endExternalDrag();

private:
    juce::Rectangle<int> muteButtonBounds() const;
    juce::Rectangle<int> scrubberBounds() const;
    juce::Rectangle<int> waveformBounds() const;

    int verticalPad() const;  // tightens on short rows
    int titleGap() const;     // ditto

    const juce::String name;
    const bool accent;
    const int emberRow; // per-row ember seed, so the rows don't flicker in step

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
