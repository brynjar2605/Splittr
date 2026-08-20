#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include "Theme.h"
#include "engine/StemCache.h"

namespace splittr
{

// Everything Splittr has ever split, read back out of the stem cache: click a
// row to republish it instantly, ✕ to delete it from disk. Same overlay idiom
// as ConfirmView — scrim, centred hairline panel, drawn in paint().
//
// When the cache outgrows the panel the list wheel-scrolls, with a hairline
// thumb on its right edge; each open starts back at the top (newest first).
// It used to cap at one page with a "+N OLDER" line, which left the older
// entries unreachable.
class LibraryView : public juce::Component
{
public:
    LibraryView() = default;

    std::function<void (juce::File)> onOpenEntry; // the entry's cache directory
    std::function<void()> onClose;

    // Re-scan the cache. Call every time the view opens: entries appear as
    // songs are split, and disappear when prune() runs.
    void refresh();

    void paint (juce::Graphics&) override;
    void visibilityChanged() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    juce::Rectangle<int> panelBounds() const;
    juce::Rectangle<int> listBounds() const;
    juce::Rectangle<int> rowBounds (int index) const; // scroll-adjusted
    juce::Rectangle<int> deleteBounds (int index) const;
    int maxScroll() const; // 0 while everything fits

    std::vector<CacheEntry> entries;
    juce::int64 totalBytes = 0;
    float scrollY = 0.0f; // pixels of list scrolled off the top

    juce::Rectangle<int> closeBounds; // computed in paint
    int hoveredRow = -1;
    bool hoveredDelete = false;
    bool hoveredClose = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryView)
};

} // namespace splittr
