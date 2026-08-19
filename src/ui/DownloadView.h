#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "MatrixBar.h"
#include "Common.h"
#include "engine/SeparationEngine.h"

namespace splittr
{

// First-run state: the htdemucs weights are fetched once, with a wide matrix
// bar, live caption, and CANCEL / LOCATE FILE MANUALLY… / path hint footer.
// Doubles as the "needs model" state (download idle or failed) where the
// primary action becomes DOWNLOAD.
class DownloadView : public juce::Component
{
public:
    DownloadView();

    std::function<void()> onCancel;
    std::function<void()> onDownload;
    std::function<void()> onLocate;

    void update (double clockSeconds, const EngineSnapshot&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    MatrixBar bar { 168 };
    ui::TagButton primaryButton { "CANCEL", theme::border35, theme::text };
    ui::TagButton locateButton { juce::String::fromUTF8 ("LOCATE FILE MANUALLY\xe2\x80\xa6"),
                                 theme::line, theme::text55 };

    bool downloading = false;
    juce::String captionLeft, message, copyLine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DownloadView)
};

} // namespace splittr
