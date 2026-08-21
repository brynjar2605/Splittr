#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace splittr
{

// The DIAGNOSTICS report viewer (issue #28), opened from Settings. Same
// overlay idiom as LibraryView: scrim, centred hairline panel, everything
// drawn in paint(), wheel-scroll when the report outgrows the panel. COPY
// REPORT puts the exact text on the clipboard for pasting into a bug report;
// nothing leaves the machine on its own.
class DiagnosticsView : public juce::Component
{
public:
    DiagnosticsView() = default;

    std::function<void()> onClose;

    // Point the panel at a freshly built report. The editor makes the view
    // visible; reopening rebuilds, so the report is always current.
    void show (const juce::String& report);

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    juce::Rectangle<int> panelBounds() const;
    juce::Rectangle<int> listBounds() const; // between the title and the footer
    int maxScroll() const;

    juce::String report;
    juce::StringArray lines;

    float scrollY = 0.0f;
    bool copied = false; // COPY flashes "COPIED" until the pointer moves off

    juce::Rectangle<int> copyBounds, closeBounds; // computed in paint
    int hoveredAction = 0;                        // 0 none, 1 copy, 2 close

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DiagnosticsView)
};

} // namespace splittr
