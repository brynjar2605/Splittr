#include "StemRowComponent.h"
#include "Common.h"

namespace splittr
{

namespace
{
    constexpr int gripWidth = 34;
    constexpr int padX = 11, padY = 9;
    constexpr int titleRowHeight = 20;
    constexpr int titleGap = 7;
    constexpr int waveHeight = 36;
    constexpr int scrubberWidth = 74;
} // namespace

StemRowComponent::StemRowComponent (Kind k) : kind (k)
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void StemRowComponent::update (const juce::File& file, const std::vector<float>& waveform,
                               bool isPlaying, double positionSeconds, double lengthSeconds)
{
    wavFile = file;
    wave = waveform;
    playing = isPlaying;
    position = positionSeconds;
    length = lengthSeconds;
    repaint();
}

juce::Rectangle<int> StemRowComponent::playButtonBounds() const
{
    const auto body = getLocalBounds().withTrimmedLeft (gripWidth).reduced (padX, padY);
    // right cluster: [play 20] 8 [track 74] 8 [elapsed ~24]
    const auto x = body.getRight() - 24 - 8 - scrubberWidth - 8 - 20;
    return { x, body.getY(), 20, 20 };
}

juce::Rectangle<int> StemRowComponent::scrubberBounds() const
{
    const auto play = playButtonBounds();
    // generous hit area around the 3px track
    return { play.getRight() + 8, play.getY(), scrubberWidth, 20 };
}

void StemRowComponent::paint (juce::Graphics& g)
{
    const bool isVocals = (kind == Kind::vocals);
    auto bounds = getLocalBounds();

    if (isVocals)
    {
        g.setColour (theme::accentTint6);
        g.fillRect (bounds);
    }

    auto borderColour = isVocals ? theme::accentTint35 : theme::lineStrong;
    g.setColour (hovered ? borderColour.withAlpha (juce::jmin (1.0f, borderColour.getFloatAlpha() + 0.1f))
                         : borderColour);
    g.drawRect (bounds, 1);

    // --- grip column: 9x17px dot grid on 4px pitch ---
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

    // --- title row ---
    auto titleRow = body.removeFromTop (titleRowHeight);

    const auto nameFont = theme::mono (12.0f, 0.1f);
    const auto name = isVocals ? "VOCALS" : "INSTRUMENTAL";

    g.setColour (isVocals ? theme::accentPale : theme::text);
    g.setFont (nameFont);
    g.drawText (name, titleRow, juce::Justification::centredLeft);

    g.setColour (theme::text45);
    g.setFont (theme::mono (8.5f));
    g.drawText (isVocals ? juce::String::fromUTF8 ("source 1 of 4 \xc2\xb7 isolated")
                         : "drums + bass + other, summed",
                titleRow.withTrimmedLeft ((int) ui::textWidth (nameFont, name) + 12),
                juce::Justification::centredLeft);

    // --- transport cluster ---
    {
        const auto play = playButtonBounds().toFloat();
        const auto accentCol = isVocals ? theme::accentMid : theme::text50;

        g.setColour (accentCol);
        g.drawEllipse (play.reduced (0.5f), 1.0f);

        g.setColour (isVocals ? theme::accentSoft : theme::text);
        g.setFont (theme::mono (7.5f));

        if (playing)
        {
            const auto c = play.getCentre();
            g.fillRect (c.x - 3.0f, c.y - 3.5f, 2.0f, 7.0f);
            g.fillRect (c.x + 1.0f, c.y - 3.5f, 2.0f, 7.0f);
        }
        else
        {
            g.drawText (juce::String::fromUTF8 ("\xe2\x96\xb6"),
                        play.translated (1.0f, 0.0f), juce::Justification::centred);
        }

        // track
        const auto scrub = scrubberBounds().toFloat();
        const auto track = juce::Rectangle<float> (scrub.getX(), scrub.getCentreY() - 1.5f,
                                                   (float) scrubberWidth, 3.0f);
        g.setColour (theme::line);
        g.fillRect (track);

        const auto frac = length > 0.0 ? juce::jlimit (0.0, 1.0, position / length) : 0.0;

        g.setColour (isVocals ? theme::accentMid : theme::rgba (221, 214, 230, 0.7f));
        g.fillRect (track.withWidth ((float) frac * track.getWidth()));

        if (isVocals && playing)
        {
            g.setColour (theme::accentPale);
            g.fillRect (track.getX() + (float) frac * track.getWidth() - 1.0f,
                        track.getY() - 3.0f, 2.0f, 9.0f);
        }

        g.setColour (theme::text45);
        g.setFont (theme::mono (8.0f));
        g.drawText (ui::mmss (playing ? position : length),
                    juce::Rectangle<float> (scrub.getRight() + 8.0f, scrub.getY(), 26.0f, scrub.getHeight()),
                    juce::Justification::centredLeft);
    }

    body.removeFromTop (titleGap);

    // --- waveform: 70 columns, 1.5px gap, centred ---
    {
        auto waveArea = body.removeFromTop (waveHeight).toFloat();
        const int cols = (int) wave.size();

        if (cols > 0)
        {
            const auto colW = (waveArea.getWidth() - 1.5f * (float) (cols - 1)) / (float) cols;

            g.setColour (isVocals ? theme::accentMid : theme::text60);

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
        juce::DragAndDropContainer::performExternalDragDropOfFiles (
            { wavFile.getFullPathName() }, false, this);
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
