#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <vector>

namespace splittr
{

// One draggable stem row (done state): the mix icon, name + peak, mute toggle
// + elapsed time, waveform. Press-and-drag anywhere that isn't the transport
// starts an external file drag of the rendered WAV.
//
// The row renders to whatever height DoneView gives it — 53px for the five-stem
// set, ~40px when seven have to fit. The 16px title strip is fixed; the
// waveform takes what is left, and the vertical padding tightens below 46px.
//
// All stems share one transport, so the row only reports intent: the circle
// toggles this stem's mute, and a plain click on the waveform (mouse-up under
// 5px of travel) seeks the shared transport. Anything with real travel is still
// the file drag-out.
//
// The 26px column on the left carries both halves of "mix it and drag it to
// the DAW" (issue #9): clicking the mix icon opens the shaping panel, and
// dragging from it — as from anywhere else on the row — hands the DAW whatever
// resolveDragFile returns, which is the shaped bounce once the stem is no
// longer flat. The icon turns accent to say so.
class StemRowComponent : public juce::Component
{
public:
    StemRowComponent (juce::String displayName, bool accented);

    // The MIDI chip's four states. `hidden` is the stems Basic Pitch has no
    // business with — drums, and the summed instrumental (issue #19).
    enum class MidiChip { hidden, idle, busy, done };

    // The CLEAN chip (issue #29), vocals only: same shape as the MIDI chip,
    // but `active` rather than done because a lit chip means the cleaned
    // render has taken the raw stem's place, not that an artifact exists.
    enum class CleanChip { hidden, idle, busy, active };

    std::function<void()> onMuteToggle;
    std::function<void (double)> onSeek; // fraction 0..1

    // True when the click held a modifier — the only way to re-transcribe a
    // stem that already has a .mid.
    std::function<void (bool)> onMidiClick;

    // Any click on the CLEAN chip: open the clean-vocal panel, whatever state
    // the chip is in — running and active are exactly when the panel has
    // something to say (issue #29).
    std::function<void()> onCleanClick;

    // A click on the mix icon: open this stem's shaping panel (issue #9). A
    // drag from the same place is still the file drag-out.
    std::function<void()> onTune;

    // What the drag-out should actually carry. Wired, it is asked at the
    // moment the drag starts, so a shaped stem hands over its processed
    // bounce; the raw render is the fallback.
    std::function<juce::File()> resolveDragFile;

    void update (const juce::File& wav, const std::vector<float>& waveform, float peakDb,
                 bool isMuted, bool isPlaying, double positionSeconds, double lengthSeconds,
                 double emberClock);

    void setMidi (MidiChip state, const juce::File& midiFile, float progress);

    void setClean (CleanChip state, float progress);

    // Marks the mix icon accent: this stem carries processing, so the drag
    // carries it too.
    void setFx (bool nonFlat);

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
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
    // Both drag-outs raise the same self-drop guard: the row's WAV and the
    // chip's .mid are one gesture as far as issue #8 is concerned.
    void startFileDrag (const juce::File&);

    // The row's own drag: resolves the payload first, then hands it over.
    void startStemDrag();

    juce::Rectangle<int> gripBounds() const;
    juce::Rectangle<int> muteButtonBounds() const;
    juce::Rectangle<int> midiChipBounds() const;  // empty while hidden
    juce::Rectangle<int> cleanChipBounds() const; // ditto; sits left of MIDI
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

    MidiChip midi = MidiChip::hidden;
    juce::File midiFile;
    float midiProgress = 0.0f;

    CleanChip clean = CleanChip::hidden;
    float cleanProgress = 0.0f;

    bool fxNonFlat = false;

    bool hovered = false;
    bool midiHovered = false;
    bool cleanHovered = false;
    bool gripHovered = false;
    bool draggingOut = false;

    // Which control the press landed on, so a gesture that starts on the chip
    // never becomes the row's WAV drag, and one that starts on the mix icon
    // only opens the panel when it never travelled.
    bool pressedMidi = false;
    bool pressedClean = false;
    bool pressedGrip = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemRowComponent)
};

} // namespace splittr
