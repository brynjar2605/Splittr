#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

namespace splittr
{

// Inline confirmation shown over the working / done views when a new file
// arrives and accepting it would throw the current stems away. Deliberately
// not a native alert window: the plugin never opens OS dialogs. The actions
// are drawn in paint() (TagButton idiom) rather than built from child
// components, so they follow live theme/accent changes.
class ConfirmView : public juce::Component
{
public:
    ConfirmView() = default;

    std::function<void()> onConfirm;
    std::function<void()> onCancel;

    void setPendingFile (const juce::File&);

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> panelBounds() const;

    juce::String fileLine;
    juce::Rectangle<int> confirmBounds, cancelBounds; // computed in paint
    int hoveredAction = 0;                            // 0 none, 1 confirm, 2 cancel

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConfirmView)
};

} // namespace splittr
