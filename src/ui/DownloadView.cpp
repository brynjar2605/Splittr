#include "DownloadView.h"
#include "engine/ModelManager.h"

namespace splittr
{

DownloadView::DownloadView()
{
    addAndMakeVisible (bar);
    addAndMakeVisible (primaryButton);
    addAndMakeVisible (locateButton);

    primaryButton.onClick = [this]
    {
        if (downloading && onCancel != nullptr)
            onCancel();
        else if (! downloading && onDownload != nullptr)
            onDownload();
    };

    locateButton.onClick = [this]
    {
        if (onLocate != nullptr)
            onLocate();
    };
}

void DownloadView::update (double clockSeconds, const EngineSnapshot& s)
{
    const bool nowDownloading = (s.phase == Phase::Downloading);

    if (nowDownloading != downloading)
    {
        downloading = nowDownloading;
        primaryButton.setLabel (downloading ? "CANCEL" : "DOWNLOAD");
    }

    if (downloading)
    {
        const auto pct = (int) std::round (s.progress * 100.0f);
        const auto mb = juce::String (ModelManager::expectedBytes / 1000000) + " MB";
        const auto speed = s.downloadMBps > 0.05 ? juce::String (s.downloadMBps, 1) + " MB/s"
                                                 : juce::String ("--");
        captionLeft = juce::String (pct) + juce::String::fromUTF8 ("% \xc2\xb7 ") + mb
                      + juce::String::fromUTF8 (" \xc2\xb7 ") + speed;
    }
    else
    {
        captionLeft = message.isNotEmpty() ? message : juce::String ("waiting to download");
    }

    if (message != s.message)
    {
        message = s.message;
        repaint();
    }

    bar.update (clockSeconds, downloading ? s.progress : 0.0f);
    repaint (0, 0, getWidth(), getHeight()); // caption text is cheap to redraw
}

void DownloadView::resized()
{
    // content padding 22px 20px
    auto area = getLocalBounds().reduced (20, 22);

    // copy block (2 lines of 13px / 1.6) + 18px gap
    auto barBox = juce::Rectangle<int> (area.getX(), area.getY() + 42 + 18,
                                        area.getWidth(), MatrixBar::barHeight + 14);

    bar.setBounds (barBox.reduced (7, 6).withHeight (MatrixBar::barHeight));

    auto footer = area.removeFromBottom (ui::TagButton::idealHeight);
    primaryButton.setBounds (footer.removeFromLeft (primaryButton.idealWidth()));
    footer.removeFromLeft (12);
    locateButton.setBounds (footer.removeFromLeft (locateButton.idealWidth()));
}

void DownloadView::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().reduced (20, 22);

    // --- copy block ---
    g.setColour (theme::text);
    g.setFont (theme::mono (13.0f, 0.04f));
    g.drawText ("Model weights are not on this machine.",
                area.getX(), area.getY(), area.getWidth(), 21, juce::Justification::centredLeft);

    g.setColour (theme::text50);
    g.drawText (juce::String::fromUTF8 ("ggml-htdemucs-4s-f16.bin \xe2\x80\x94 84 MB \xe2\x80\x94 one time only."),
                area.getX(), area.getY() + 21, area.getWidth(), 21, juce::Justification::centredLeft);

    // --- matrix bar box ---
    auto barBox = juce::Rectangle<int> (area.getX(), area.getY() + 42 + 18,
                                        area.getWidth(), MatrixBar::barHeight + 14);
    g.setColour (theme::bgPanelAlt);
    g.fillRect (barBox);
    g.setColour (theme::lineStrong);
    g.drawRect (barBox, 1);

    // --- caption row ---
    auto caption = juce::Rectangle<int> (barBox.getX(), barBox.getBottom() + 7, barBox.getWidth(), 13);
    g.setColour (theme::text50);
    g.setFont (theme::mono (9.5f));
    g.drawText (captionLeft, caption, juce::Justification::centredLeft);
    g.drawText ("from huggingface.co", caption, juce::Justification::centredRight);

    // --- footer path hint ---
    auto footer = area.removeFromBottom (ui::TagButton::idealHeight);
    g.setColour (theme::text40);
    g.setFont (theme::mono (9.5f, 0.08f));
    g.drawText (juce::String::fromUTF8 ("\xe2\x86\x92 ~/\xe2\x80\xa6/Splittr/models"),
                footer, juce::Justification::centredRight);
}

} // namespace splittr
