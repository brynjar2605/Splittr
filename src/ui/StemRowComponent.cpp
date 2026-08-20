#include "StemRowComponent.h"
#include "Common.h"

namespace splittr
{

namespace
{
    constexpr int gripWidth = 26;
    constexpr int padX = 10, padY = 6;
    constexpr int titleRowHeight = 16;
    constexpr int titleGap = 4;
    constexpr int scrubberWidth = 60;
    constexpr int playSize = 16;
    constexpr int elapsedWidth = 26;

    // Set while a row is running an external file drag. The editor refuses file
    // drops for as long as it is set, so releasing a dragged-out stem over the
    // plugin window cannot discard the current session.
    bool externalDragInProgress = false;
} // namespace

bool StemRowComponent::isExternalDragInProgress()
{
    return externalDragInProgress;
}

StemRowComponent::StemRowComponent (juce::String displayName, bool accented)
    : name (std::move (displayName)), accent (accented)
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void StemRowComponent::update (const juce::File& file, const std::vector<float>& waveform,
                               float peak, bool isPlaying, double positionSeconds,
                               double lengthSeconds)
{
    wavFile = file;
    wave = waveform;
    peakDb = peak;
    playing = isPlaying;
    position = positionSeconds;
    length = lengthSeconds;
    repaint();
}

juce::Rectangle<int> StemRowComponent::playButtonBounds() const
{
    const auto body = getLocalBounds().withTrimmedLeft (gripWidth).reduced (padX, padY);
    const auto x = body.getRight() - elapsedWidth - 8 - scrubberWidth - 8 - playSize;
    return { x, body.getY(), playSize, playSize };
}

juce::Rectangle<int> StemRowComponent::scrubberBounds() const
{
    const auto play = playButtonBounds();
    return { play.getRight() + 8, play.getY(), scrubberWidth, playSize };
}

void StemRowComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    if (accent)
    {
        g.setColour (theme::accentTint6);
        g.fillRect (bounds);
    }

    auto borderColour = accent ? theme::accentTint35 : theme::lineStrong;
    g.setColour (hovered ? borderColour.withAlpha (juce::jmin (1.0f, borderColour.getFloatAlpha() + 0.1f))
                         : borderColour);
    g.drawRect (bounds, 1);

    // --- grip column ---
    auto grip = bounds.removeFromLeft (gripWidth);
    g.setColour (theme::lineSoft15);
    g.fillRect (grip.getRight() - 1, grip.getY() + 1, 1, grip.getHeight() - 2);

    {
        g.setColour (theme::text40);
        const auto cx = (float) grip.getCentreX() - 4.5f;
        const auto cy = (float) grip.getCentreY() - 8.5f;

        for (int row = 0; row < 5; ++row)
            for (int col = 0; col < 3; ++col)
                g.fillEllipse (cx + (float) col * 4.0f, cy + (float) row * 4.0f, 1.5f, 1.5f);
    }

    auto body = bounds.reduced (padX, padY);

    // --- title row: name + peak, transport on the right ---
    auto titleRow = body.removeFromTop (titleRowHeight);

    const auto nameFont = theme::mono (10.5f, 0.1f);

    g.setColour (accent ? theme::accentPale : theme::text);
    g.setFont (nameFont);
    g.drawText (name, titleRow, juce::Justification::centredLeft);

    if (peakDb > -119.0f)
    {
        g.setColour (theme::text45);
        g.setFont (theme::mono (8.0f));
        g.drawText (juce::String (peakDb, 1) + " dB",
                    titleRow.withTrimmedLeft ((int) ui::textWidth (nameFont, name) + 10),
                    juce::Justification::centredLeft);
    }

    // --- transport cluster ---
    {
        const auto play = playButtonBounds().toFloat();
        const auto accentCol = accent ? theme::accentMid : theme::text50;

        g.setColour (accentCol);
        g.drawEllipse (play.reduced (0.5f), 1.0f);

        g.setColour (accent ? theme::accentSoft : theme::text);

        if (playing)
        {
            const auto c = play.getCentre();
            g.fillRect (c.x - 2.5f, c.y - 3.0f, 1.5f, 6.0f);
            g.fillRect (c.x + 1.0f, c.y - 3.0f, 1.5f, 6.0f);
        }
        else
        {
            g.setFont (theme::mono (6.5f));
            g.drawText (juce::String::fromUTF8 ("\xe2\x96\xb6"),
                        play.translated (1.0f, 0.0f), juce::Justification::centred);
        }

        const auto scrub = scrubberBounds().toFloat();
        const auto track = juce::Rectangle<float> (scrub.getX(), scrub.getCentreY() - 1.5f,
                                                   (float) scrubberWidth, 3.0f);
        g.setColour (theme::line);
        g.fillRect (track);

        const auto frac = length > 0.0 ? juce::jlimit (0.0, 1.0, position / length) : 0.0;

        g.setColour (accent ? theme::accentMid : theme::rgba (221, 214, 230, 0.7f));
        g.fillRect (track.withWidth ((float) frac * track.getWidth()));

        if (accent && playing)
        {
            g.setColour (theme::accentPale);
            g.fillRect (track.getX() + (float) frac * track.getWidth() - 1.0f,
                        track.getY() - 3.0f, 2.0f, 9.0f);
        }

        g.setColour (theme::text45);
        g.setFont (theme::mono (8.0f));
        g.drawText (ui::mmss (playing ? position : length),
                    juce::Rectangle<float> (scrub.getRight() + 8.0f, scrub.getY(),
                                            (float) elapsedWidth, scrub.getHeight()),
                    juce::Justification::centredLeft);
    }

    body.removeFromTop (titleGap);

    // --- waveform: 70 columns, 1.5px gap, centred ---
    {
        auto waveArea = body.toFloat();
        const int cols = (int) wave.size();

        if (cols > 0 && waveArea.getHeight() > 2.0f)
        {
            const auto colW = (waveArea.getWidth() - 1.5f * (float) (cols - 1)) / (float) cols;

            g.setColour (accent ? theme::accentMid : theme::text60);

            for (int i = 0; i < cols; ++i)
            {
                const auto h = juce::jmax (1.0f, wave[(size_t) i] * waveArea.getHeight());
                const auto x = waveArea.getX() + (float) i * (colW + 1.5f);
                g.fillRect (x, waveArea.getCentreY() - h * 0.5f, colW, h);
            }
        }
    }
}

void StemRowComponent::mouseDown (const juce::MouseEvent& e)
{
    draggingOut = false;
    scrubbing = scrubberBounds().contains (e.getPosition());

    if (scrubbing && onSeek != nullptr)
        onSeek (juce::jlimit (0.0, 1.0,
                              (double) (e.x - scrubberBounds().getX()) / scrubberWidth));
}

void StemRowComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (scrubbing)
    {
        if (onSeek != nullptr)
            onSeek (juce::jlimit (0.0, 1.0,
                                  (double) (e.x - scrubberBounds().getX()) / scrubberWidth));
        return;
    }

    if (! draggingOut && e.getDistanceFromDragStart() > 6
        && ! playButtonBounds().contains (e.getMouseDownPosition())
        && wavFile.existsAsFile())
    {
        draggingOut = true;
        externalDragInProgress = true;

        // The drag is asynchronous: the flag is cleared from the end-of-drag
        // callback, which macOS delivers after the drop has been handled.
        const auto started = juce::DragAndDropContainer::performExternalDragDropOfFiles (
            { wavFile.getFullPathName() }, false, this,
            [] { externalDragInProgress = false; });

        if (! started)
            externalDragInProgress = false;
    }
}

void StemRowComponent::mouseUp (const juce::MouseEvent& e)
{
    if (! draggingOut && ! scrubbing
        && playButtonBounds().contains (e.getPosition())
        && onPlayToggle != nullptr)
        onPlayToggle();

    draggingOut = false;
    scrubbing = false;
}

void StemRowComponent::mouseEnter (const juce::MouseEvent&) { hovered = true; repaint(); }
void StemRowComponent::mouseExit (const juce::MouseEvent&) { hovered = false; repaint(); }

} // namespace splittr
