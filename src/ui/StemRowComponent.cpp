#include "StemRowComponent.h"
#include "Common.h"
#include "Ember.h"

namespace splittr
{

namespace
{
    constexpr int gripWidth = 26;
    constexpr int padX = 10;
    constexpr int titleRowHeight = 16; // fixed: the name/peak/transport strip
    constexpr int scrubberWidth = 60;
    constexpr int muteSize = 16;
    constexpr int elapsedWidth = 26;

    // A click is a mouse-up that never travelled this far; anything beyond is
    // a drag, and drags belong to the external file drag-out.
    constexpr int clickSlop = 5;

    constexpr float mutedAlpha = 0.4f;

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
    : name (std::move (displayName)), accent (accented), emberRow (name.hashCode())
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void StemRowComponent::update (const juce::File& file, const std::vector<float>& waveform,
                               float peak, bool isMuted, bool isPlaying, double positionSeconds,
                               double lengthSeconds, double emberClock)
{
    wavFile = file;
    wave = waveform;
    peakDb = peak;
    muted = isMuted;
    playing = isPlaying;
    position = positionSeconds;
    length = lengthSeconds;
    clock = emberClock;
    repaint();
}

// Seven rows only fit by giving up vertical padding; the title strip stays put
// and the waveform absorbs the rest of the loss.
int StemRowComponent::verticalPad() const
{
    return getHeight() < compactBelow ? 4 : 6;
}

int StemRowComponent::titleGap() const
{
    return getHeight() < compactBelow ? 2 : 4;
}

juce::Rectangle<int> StemRowComponent::muteButtonBounds() const
{
    const auto body = getLocalBounds().withTrimmedLeft (gripWidth).reduced (padX, verticalPad());
    const auto x = body.getRight() - elapsedWidth - 8 - scrubberWidth - 8 - muteSize;
    return { x, body.getY(), muteSize, muteSize };
}

juce::Rectangle<int> StemRowComponent::scrubberBounds() const
{
    const auto mute = muteButtonBounds();
    return { mute.getRight() + 8, mute.getY(), scrubberWidth, muteSize };
}

juce::Rectangle<int> StemRowComponent::waveformBounds() const
{
    auto body = getLocalBounds().withTrimmedLeft (gripWidth).reduced (padX, verticalPad());
    body.removeFromTop (titleRowHeight + titleGap());
    return body;
}

void StemRowComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Muting dims the identity of the row (name + waveform) so the audible mix
    // is readable at a glance; the transport cluster stays at full strength.
    const auto rowAlpha = muted ? mutedAlpha : 1.0f;

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

    auto body = bounds.reduced (padX, verticalPad());

    // --- title row: name + peak, transport on the right ---
    auto titleRow = body.removeFromTop (titleRowHeight);

    const auto nameFont = theme::mono (10.5f, 0.1f);

    g.setColour ((accent ? theme::accentPale : theme::text).withMultipliedAlpha (rowAlpha));
    g.setFont (nameFont);
    g.drawText (name, titleRow, juce::Justification::centredLeft);

    if (peakDb > -119.0f)
    {
        g.setColour (theme::text45.withMultipliedAlpha (rowAlpha));
        g.setFont (theme::mono (8.0f));
        g.drawText (juce::String (peakDb, 1) + " dB",
                    titleRow.withTrimmedLeft ((int) ui::textWidth (nameFont, name) + 10),
                    juce::Justification::centredLeft);
    }

    // --- transport cluster: mute dot + shared scrubber ---
    {
        const auto mute = muteButtonBounds().toFloat();
        const auto ringCol = muted ? theme::text35
                                   : (accent ? theme::accentMid : theme::text50);

        g.setColour (ringCol);
        g.drawEllipse (mute.reduced (0.5f), 1.0f);

        // Lit centre = audible, empty ring = muted.
        if (! muted)
        {
            g.setColour (accent ? theme::accentSoft : theme::text);
            g.fillEllipse (mute.reduced (5.0f));
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

    // --- waveform: 70 columns, 1.5px gap, centred ---
    {
        const auto waveArea = waveformBounds().toFloat();
        const int cols = (int) wave.size();

        if (cols > 0 && waveArea.getHeight() > 2.0f)
        {
            const auto colW = (waveArea.getWidth() - 1.5f * (float) (cols - 1)) / (float) cols;
            const auto head = length > 0.0 ? (float) juce::jlimit (0.0, 1.0, position / length) : 0.0f;

            // Burning only reads as "audible": muted rows stay static + dimmed.
            const bool burning = playing && ! muted;

            const auto baseCol = (accent ? theme::accentMid : theme::text60)
                                     .withMultipliedAlpha (rowAlpha);
            const auto flameCol = theme::accentSoft;

            for (int i = 0; i < cols; ++i)
            {
                const auto h = juce::jmax (1.0f, wave[(size_t) i] * waveArea.getHeight());
                const auto x = waveArea.getX() + (float) i * (colW + 1.5f);
                const auto y = waveArea.getCentreY() - h * 0.5f;

                const auto centre = ((float) i + 0.5f) / (float) cols;
                const auto ramp = burning ? ui::emberRamp (centre, head) : 0.0f;

                // The flame no longer covers the whole bar, so the base is only
                // half-dimmed under it: the gaps between the ember cells are
                // what carries the burned silhouette's weight now.
                g.setColour (baseCol.withMultipliedAlpha (1.0f - 0.45f * ramp));
                g.fillRect (x, y, colW, h);

                // Behind the playhead the bar burns at the matrix bar's grain:
                // ~2px cells, split in two across the width where it fits.
                if (ramp > 0.0f)
                    ui::paintEmberBar (g, flameCol, { x, y, colW, h }, i, emberRow, clock, ramp);
            }

            // Shared playhead: the same line on every row ties them together.
            if (playing)
            {
                g.setColour (theme::accentMid);
                g.fillRect (waveArea.getX() + head * waveArea.getWidth(), waveArea.getY(),
                            1.0f, waveArea.getHeight());
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
        && ! muteButtonBounds().contains (e.getMouseDownPosition())
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
    if (! draggingOut && ! scrubbing)
    {
        const auto waveArea = waveformBounds();

        if (muteButtonBounds().contains (e.getPosition()) && onMuteToggle != nullptr)
        {
            onMuteToggle();
        }
        else if (e.getDistanceFromDragStart() < clickSlop
                 && waveArea.contains (e.getPosition())
                 && waveArea.getWidth() > 0 && onSeek != nullptr)
        {
            onSeek (juce::jlimit (0.0, 1.0,
                                  (double) (e.x - waveArea.getX()) / (double) waveArea.getWidth()));
        }
    }

    draggingOut = false;
    scrubbing = false;
}

void StemRowComponent::mouseEnter (const juce::MouseEvent&) { hovered = true; repaint(); }
void StemRowComponent::mouseExit (const juce::MouseEvent&) { hovered = false; repaint(); }

} // namespace splittr
