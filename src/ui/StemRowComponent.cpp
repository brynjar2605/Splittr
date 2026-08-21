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
    constexpr int muteSize = 16;
    constexpr int elapsedWidth = 26;

    // The MIDI chip: a hairline text chip in the TagButton idiom, shrunk to
    // fit the 16px title strip. Width is fixed at the widest label so the
    // transport cluster does not shuffle sideways when a transcription starts.
    constexpr int midiChipHeight = 13;
    constexpr int midiChipGap = 8;
    constexpr float midiChipTextSize = 8.5f;

    // The busy label is the wider of the two, so it is what the chip is
    // measured against.
    juce::String midiLabel (bool working)
    {
        return working ? juce::String::fromUTF8 ("MIDI\xe2\x80\xa6") : juce::String ("MIDI");
    }

    // The CLEAN chip (issue #29) borrows the MIDI chip's metrics wholesale, so
    // the two read as one family on the vocals row.
    juce::String cleanLabel (bool working)
    {
        return working ? juce::String::fromUTF8 ("CLEAN\xe2\x80\xa6") : juce::String ("CLEAN");
    }

    // The mix icon in the grip column (issue #9): three fader tracks with
    // their caps at different points along them, which is what makes three
    // lines read as a mixer rather than a hamburger.
    constexpr float mixIconWidth = 12.0f, mixIconPitch = 5.0f;
    constexpr float mixIconHeight = 2.0f * mixIconPitch + 1.0f;
    constexpr float mixIconCap[3] = { 2.0f, 7.0f, 4.0f };

    // A click is a mouse-up that never travelled this far; anything beyond is
    // a drag, and drags belong to the external file drag-out.
    constexpr int clickSlop = 5;

    constexpr float mutedAlpha = 0.4f;

    // Non-zero while a file drag this plugin started is in flight. The editor
    // refuses file drops for as long as it is, so releasing dragged-out stems
    // over the plugin window cannot discard the current session. A count rather
    // than a flag: the rows and DoneView's DRAG ALL both raise it.
    int externalDragsInFlight = 0;
} // namespace

bool StemRowComponent::isExternalDragInProgress()
{
    return externalDragsInFlight > 0;
}

void StemRowComponent::beginExternalDrag()
{
    ++externalDragsInFlight;
}

void StemRowComponent::endExternalDrag()
{
    externalDragsInFlight = juce::jmax (0, externalDragsInFlight - 1);
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

void StemRowComponent::setMidi (MidiChip state, const juce::File& file, float progress)
{
    if (midi == state && midiFile == file && juce::approximatelyEqual (midiProgress, progress))
        return;

    midi = state;
    midiFile = file;
    midiProgress = progress;
    repaint();
}

void StemRowComponent::setClean (CleanChip state, float progress)
{
    if (clean == state && juce::approximatelyEqual (cleanProgress, progress))
        return;

    clean = state;
    cleanProgress = progress;
    repaint();
}

void StemRowComponent::setFx (bool nonFlat)
{
    if (fxNonFlat == nonFlat)
        return;

    fxNonFlat = nonFlat;
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

juce::Rectangle<int> StemRowComponent::gripBounds() const
{
    return getLocalBounds().withWidth (gripWidth);
}

juce::Rectangle<int> StemRowComponent::muteButtonBounds() const
{
    const auto body = getLocalBounds().withTrimmedLeft (gripWidth).reduced (padX, verticalPad());
    const auto x = body.getRight() - elapsedWidth - 8 - muteSize;
    return { x, body.getY(), muteSize, muteSize };
}

juce::Rectangle<int> StemRowComponent::midiChipBounds() const
{
    if (midi == MidiChip::hidden)
        return {};

    const auto font = theme::mono (midiChipTextSize, 0.08f);
    const auto width = (int) std::ceil (ui::textWidth (font, midiLabel (true))) + 12;
    const auto mute = muteButtonBounds();

    return { mute.getX() - midiChipGap - width,
             mute.getCentreY() - midiChipHeight / 2, width, midiChipHeight };
}

juce::Rectangle<int> StemRowComponent::cleanChipBounds() const
{
    if (clean == CleanChip::hidden)
        return {};

    const auto font = theme::mono (midiChipTextSize, 0.08f);
    const auto width = (int) std::ceil (ui::textWidth (font, cleanLabel (true))) + 12;

    // Left of the MIDI chip when there is one (on vocals there always is);
    // straight off the mute cluster otherwise.
    const auto anchor = midi == MidiChip::hidden ? muteButtonBounds() : midiChipBounds();

    return { anchor.getX() - midiChipGap - width,
             muteButtonBounds().getCentreY() - midiChipHeight / 2, width, midiChipHeight };
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

    // The mix icon: click to shape this stem, drag to hand the DAW the result
    // (issue #9). It stays at full strength on a muted row — what the drag
    // carries has nothing to do with what the audition is playing.
    {
        const auto stroke = fxNonFlat ? (gripHovered ? theme::accentSoft : theme::accentMid)
                                      : (gripHovered ? theme::text : theme::text40);

        const auto left = (float) grip.getCentreX() - mixIconWidth * 0.5f;
        const auto top = (float) grip.getCentreY() - mixIconHeight * 0.5f;

        for (int fader = 0; fader < 3; ++fader)
        {
            const auto y = top + (float) fader * mixIconPitch;

            g.setColour (stroke.withMultipliedAlpha (0.55f));
            g.fillRect (left, y, mixIconWidth, 1.0f);

            g.setColour (stroke);
            g.fillRect (left + mixIconCap[fader], y - 2.0f, 3.0f, 5.0f);
        }
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

    // --- MIDI chip: idle outline, accent while working, filled once written ---
    if (midi != MidiChip::hidden)
    {
        const auto chip = midiChipBounds();
        const auto working = midi == MidiChip::busy;
        const auto done = midi == MidiChip::done;

        // Progress is the chip filling from the left rather than a separate
        // bar: there is no room for one, and a stem is seconds of work.
        if (working)
        {
            g.setColour (theme::accentTint15);
            g.fillRect (chip.toFloat().withWidth ((float) chip.getWidth()
                                                  * juce::jlimit (0.0f, 1.0f, midiProgress)));
        }
        else if (done)
        {
            g.setColour (theme::accentTint15);
            g.fillRect (chip);
        }

        g.setColour (done || working ? theme::accentMid
                     : midiHovered   ? theme::border35
                                     : theme::lineSoft15);
        g.drawRect (chip, 1);

        g.setColour (working ? theme::accentSoft
                     : done  ? theme::accentPale
                     : midiHovered ? theme::text
                                   : theme::text40);

        g.setFont (theme::mono (midiChipTextSize, 0.08f));
        g.drawText (midiLabel (working), chip, juce::Justification::centred);
    }

    // --- CLEAN chip: the MIDI chip's grammar, on the vocals row (issue #29) ---
    if (clean != CleanChip::hidden)
    {
        const auto chip = cleanChipBounds();
        const auto working = clean == CleanChip::busy;
        const auto active = clean == CleanChip::active;

        if (working)
        {
            g.setColour (theme::accentTint15);
            g.fillRect (chip.toFloat().withWidth ((float) chip.getWidth()
                                                  * juce::jlimit (0.0f, 1.0f, cleanProgress)));
        }
        else if (active)
        {
            g.setColour (theme::accentTint15);
            g.fillRect (chip);
        }

        g.setColour (active || working ? theme::accentMid
                     : cleanHovered    ? theme::border35
                                       : theme::lineSoft15);
        g.drawRect (chip, 1);

        g.setColour (working ? theme::accentSoft
                     : active ? theme::accentPale
                     : cleanHovered ? theme::text
                                    : theme::text40);

        g.setFont (theme::mono (midiChipTextSize, 0.08f));
        g.drawText (cleanLabel (working), chip, juce::Justification::centred);
    }

    // --- transport cluster: mute dot + elapsed ---
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

        // The readout sits where the mini scrubber used to (issue #22): the
        // waveform is the scrubber now, so the strip carries only the number.
        g.setColour (theme::text45);
        g.setFont (theme::mono (8.0f));
        g.drawText (ui::mmss (playing ? position : length),
                    juce::Rectangle<float> (mute.getRight() + 8.0f, mute.getY(),
                                            (float) elapsedWidth, mute.getHeight()),
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

                // The fine flame doesn't cover the whole bar, so the base dims
                // less under it than under the coarse one — the gaps between
                // the ember cells carry the burned silhouette's weight.
                const auto fine = theme::fineFire();

                g.setColour (baseCol.withMultipliedAlpha (1.0f - ui::fireBaseDim (fine) * ramp));
                g.fillRect (x, y, colW, h);

                // Behind the playhead the bar burns at the matrix bar's grain
                // (or as one coarse ember per bar, per the FIRE setting).
                if (ramp > 0.0f)
                    ui::paintEmberBar (g, flameCol, { x, y, colW, h }, i, emberRow, clock, ramp, fine);
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
    pressedMidi = midiChipBounds().contains (e.getPosition());
    pressedClean = cleanChipBounds().contains (e.getPosition());
    pressedGrip = gripBounds().contains (e.getPosition());
}

void StemRowComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingOut || e.getDistanceFromDragStart() <= 6)
        return;

    // The chip is its own drag source: dragging a finished one hands the DAW
    // the .mid, which lands as a MIDI clip instead of an audio one. Pressing
    // an unfinished chip drags nothing — a stem with no transcription yet has
    // nothing to give, and falling through to the WAV would be a lie.
    if (pressedMidi)
    {
        if (midi != MidiChip::done || ! midiFile.existsAsFile())
            return;

        startFileDrag (midiFile);
        return;
    }

    // The CLEAN chip drags nothing at all: what it stands for already rides
    // the row's own drag once the render is active (issue #29).
    if (pressedClean)
        return;

    if (! muteButtonBounds().contains (e.getMouseDownPosition()))
        startStemDrag();
}

void StemRowComponent::startStemDrag()
{
    auto file = wavFile;

    // Resolving may take a beat — the editor can bounce the shaped stem to
    // disk synchronously here (issue #9) — which is fine: nothing is in flight
    // until performExternalDragDropOfFiles is called below.
    if (resolveDragFile != nullptr)
    {
        const auto resolved = resolveDragFile();

        if (resolved.existsAsFile())
            file = resolved;
    }

    if (file.existsAsFile())
        startFileDrag (file);
}

void StemRowComponent::startFileDrag (const juce::File& file)
{
    draggingOut = true;
    beginExternalDrag();

    // The drag is asynchronous: the guard is lifted from the end-of-drag
    // callback, which macOS delivers after the drop has been handled.
    const auto started = juce::DragAndDropContainer::performExternalDragDropOfFiles (
        { file.getFullPathName() }, false, this,
        [] { endExternalDrag(); });

    if (! started)
        endExternalDrag();
}

void StemRowComponent::mouseUp (const juce::MouseEvent& e)
{
    if (pressedMidi)
    {
        // A plain click on a finished chip does nothing — the gesture that
        // chip is for is the drag. Only a modifier asks for it again.
        if (! draggingOut && e.getDistanceFromDragStart() < clickSlop
            && midiChipBounds().contains (e.getPosition())
            && midi != MidiChip::busy && onMidiClick != nullptr)
            onMidiClick (e.mods.isCommandDown() || e.mods.isAltDown());

        pressedMidi = false;
        pressedClean = false;
        pressedGrip = false;
        draggingOut = false;
        return;
    }

    if (pressedClean)
    {
        // Every state opens the panel: idle to set a run up, busy for its
        // progress and cancel, active for the revert (issue #29).
        if (! draggingOut && e.getDistanceFromDragStart() < clickSlop
            && cleanChipBounds().contains (e.getPosition()) && onCleanClick != nullptr)
            onCleanClick();

        pressedClean = false;
        pressedGrip = false;
        draggingOut = false;
        return;
    }

    if (pressedGrip)
    {
        // A click on the mix icon opens the shaping panel; a press with travel
        // was the drag-out and has already happened.
        if (! draggingOut && e.getDistanceFromDragStart() < clickSlop
            && gripBounds().contains (e.getPosition()) && onTune != nullptr)
            onTune();

        pressedGrip = false;
        draggingOut = false;
        return;
    }

    if (! draggingOut)
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
            // Seeking is click-jumps only now the mini scrubber is gone — drag
            // on the waveform is the file drag-out. If continuous scrubbing is
            // missed, ⌘-drag here is where it goes.
            onSeek (juce::jlimit (0.0, 1.0,
                                  (double) (e.x - waveArea.getX()) / (double) waveArea.getWidth()));
        }
    }

    draggingOut = false;
}

void StemRowComponent::mouseMove (const juce::MouseEvent& e)
{
    const auto overChip = midiChipBounds().contains (e.getPosition());
    const auto overClean = cleanChipBounds().contains (e.getPosition());
    const auto overGrip = gripBounds().contains (e.getPosition());

    // The mix icon is clicked, a finished chip is dragged, an unstarted one is
    // clicked, the CLEAN chip is always clicked, and the rest of the row is
    // the file drag-out; the cursor says which before the press does.
    setMouseCursor (overGrip || overClean || (overChip && midi == MidiChip::idle)
                        ? juce::MouseCursor::PointingHandCursor
                        : juce::MouseCursor::DraggingHandCursor);

    if (overChip == midiHovered && overClean == cleanHovered && overGrip == gripHovered)
        return;

    midiHovered = overChip;
    cleanHovered = overClean;
    gripHovered = overGrip;
    repaint();
}

void StemRowComponent::mouseEnter (const juce::MouseEvent&) { hovered = true; repaint(); }

void StemRowComponent::mouseExit (const juce::MouseEvent&)
{
    hovered = false;
    midiHovered = false;
    cleanHovered = false;
    gripHovered = false;
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    repaint();
}

} // namespace splittr
