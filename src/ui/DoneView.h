#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "StemRowComponent.h"
#include "engine/SeparationEngine.h"

namespace splittr
{

// Done state: two stem rows plus a footer with stats, REVEAL, NEW FILE and
// the drag hint.
class DoneView : public juce::Component
{
public:
    DoneView();

    StemRowComponent vocalsRow { StemRowComponent::Kind::vocals };
    StemRowComponent instrumentalRow { StemRowComponent::Kind::instrumental };

    std::function<void()> onReveal;
    std::function<void()> onNewFile;

    void update (const EngineSnapshot&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> revealBounds, newFileBounds;
    juce::String statsText;
    int hoveredAction = 0; // 0 none, 1 reveal, 2 new file

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DoneView)
};

} // namespace splittr
