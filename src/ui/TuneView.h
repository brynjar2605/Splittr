#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "engine/StemFx.h"

namespace splittr
{

// Per-stem sound shaping (issue #9), opened by clicking the mix icon on a stem
// row. Ten parameters — two passes, a three-band EQ, one compressor and an
// output gain — laid out as two columns of hairline sliders: the tone path
// (PASS, EQ) down the left, what happens after it (COMP, OUT) down the right,
// which is the order the chain runs in.
//
// Same overlay idiom as LibraryView and SettingsView: scrim, centred hairline
// panel, everything drawn in paint() against hit rects rather than built from
// child components, so it follows live theme/accent changes. A click on the
// scrim closes it.
//
// The sliders are the SettingsView hue strip's gesture — press picks one, drag
// moves it live, mouse-up commits — because the two halves of the feature want
// different things: the audition chain wants every intermediate value, and the
// offline bounce wants only the one the user settled on. Double-clicking a
// slider puts that single parameter back to its default.
//
// The view owns no engine state: it edits a copy of the params it was shown
// with and reports every change through onChange.
class TuneView : public juce::Component
{
public:
    TuneView() = default;

    // (stem, params, commit) — commit=false while a slider is being dragged,
    // true once on release. RESET sends a flat params with commit=true.
    std::function<void (int, const StemFxParams&, bool)> onChange;
    std::function<void()> onClose;

    // Point the panel at a stem. The editor makes the view visible.
    void show (int stemIndex, const juce::String& stemName, const StemFxParams&);

    // Which stem the panel is currently editing, or -1 before the first show().
    int currentStem() const { return stem; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    void paintSlider (juce::Graphics&, int param);

    juce::Rectangle<int> panelBounds() const;
    juce::Rectangle<int> columnsBounds() const;   // between the title and the footer
    juce::Rectangle<int> rowBoundsFor (int) const;   // label + track + readout
    juce::Rectangle<int> trackBoundsFor (int) const; // the 1px track itself
    juce::Rectangle<int> grabBoundsFor (int) const;  // the track, made hittable
    int paramAt (juce::Point<int>) const;            // -1 outside every track

    void dragTo (const juce::MouseEvent&, bool commit);
    void send (bool commit);

    int stem = -1;
    juce::String name;
    StemFxParams fx;

    juce::Rectangle<int> closeBounds, resetBounds; // computed in paint

    int activeParam = -1;    // the slider the press landed on, or -1
    float activeStart = 0.0f; // its value before the press moved it
    int hoveredParam = -1;
    int hoveredAction = 0; // 0 none, 1 close, 2 reset

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuneView)
};

} // namespace splittr
