#include "VocalCleanPanel.h"
#include "Common.h"

namespace splittr
{

namespace
{
    using CleanState = EngineSnapshot::VocalClean::State;

    constexpr int panelWidth = 392, panelHeight = 222;
    constexpr int panelPad = 18;
    constexpr int titleHeight = 21, titleGap = 10;
    constexpr int rowPitch = 24;
    constexpr int statusGap = 8, statusLine = 13, barHeight = 3;

    constexpr int toggleCellWidth = 36, restoreCellWidth = 56;

    // The stage names the protocol speaks, phrased for the status line.
    juce::String stageLabel (const juce::String& stage)
    {
        if (stage == "dereverb")        return "DE-REVERB";
        if (stage == "dereverb-echo")   return "DE-REVERB + ECHO";
        if (stage == "restore-apollo")  return "APOLLO RESTORE";
        if (stage == "restore-audiosr") return "AUDIOSR RESTORE";
        return stage.toUpperCase();
    }
} // namespace

void VocalCleanPanel::show (const EngineSnapshot::VocalClean& state, bool sidecarSetUp)
{
    engineState = state;
    setUp = sidecarSetUp;

    // What is running or active is the truth the toggles should open on;
    // otherwise whatever was dialled in last time stands.
    if (state.state == CleanState::running || state.state == CleanState::done)
        params = state.params;

    hoveredAction = 0;
    repaint();
}

void VocalCleanPanel::update (const EngineSnapshot::VocalClean& state, bool sidecarSetUp)
{
    engineState = state;
    setUp = sidecarSetUp;
    repaint();
}

juce::Rectangle<int> VocalCleanPanel::panelBounds() const
{
    return getLocalBounds().withSizeKeepingCentre (panelWidth, panelHeight);
}

bool VocalCleanPanel::runnable() const
{
    if (params.isNoop() || engineState.state == CleanState::running)
        return false;

    // Rerunning what is already active would only reproduce the file.
    return ! (engineState.state == CleanState::done && engineState.params == params);
}

void VocalCleanPanel::paint (juce::Graphics& g)
{
    g.setColour (theme::bg.withAlpha (0.90f));
    g.fillAll();

    const auto panel = panelBounds();

    g.setColour (theme::bg);
    g.fillRect (panel);
    g.setColour (theme::bgPanelAlt);
    g.fillRect (panel);
    g.setColour (theme::lineStrong);
    g.drawRect (panel, 1);

    auto body = panel.reduced (panelPad);

    // --- title: name left, the honesty tag right ---
    {
        const auto titleRow = body.removeFromTop (titleHeight);

        g.setColour (theme::text);
        g.setFont (theme::mono (13.0f, 0.04f));
        g.drawText ("clean vocal", titleRow, juce::Justification::centredLeft);

        const auto tagFont = theme::mono (8.5f, 0.1f);
        const auto tagWidth = (int) std::ceil (ui::textWidth (tagFont, "EXPERIMENTAL")) + 12;
        const juce::Rectangle<int> tag (titleRow.getRight() - tagWidth,
                                        titleRow.getCentreY() - ui::miniCellHeight / 2,
                                        tagWidth, ui::miniCellHeight);

        g.setColour (theme::accentTint15);
        g.fillRect (tag);
        g.setColour (theme::accentTint40);
        g.drawRect (tag, 1);
        g.setColour (theme::accentSoft);
        g.setFont (tagFont);
        g.drawText ("EXPERIMENTAL", tag, juce::Justification::centred);
    }

    body.removeFromTop (titleGap);

    // --- the chain, one choice per row, switches right-aligned ---
    const auto choiceRow = [&] (const juce::String& label, const juce::StringArray& cells,
                                int active, int cellWidth, int hoverBase,
                                juce::Rectangle<int>* outCells)
    {
        const auto row = body.removeFromTop (rowPitch);
        const auto y = row.getCentreY() - ui::miniCellHeight / 2;
        const auto x = row.getRight() - cellWidth * cells.size();

        g.setColour (theme::text35);
        g.setFont (theme::mono (8.5f, 0.12f));
        g.drawText (label, row, juce::Justification::centredLeft);

        ui::drawMiniCells (g, x, y, cells, active, hoveredAction, hoverBase, cellWidth);

        for (int i = 0; i < cells.size(); ++i)
            outCells[i] = { x + i * cellWidth, y, cellWidth, ui::miniCellHeight };
    };

    choiceRow ("DE-REVERB", { "OFF", "ON" }, params.dereverb ? 1 : 0,
               toggleCellWidth, 1, dereverbCells);
    choiceRow ("DE-REVERB + ECHO", { "OFF", "ON" }, params.dereverbEcho ? 1 : 0,
               toggleCellWidth, 3, echoCells);
    choiceRow ("RESTORE", { "NONE", "APOLLO", "AUDIOSR" },
               params.restore == VocalRestoreMode::apollo    ? 1
               : params.restore == VocalRestoreMode::audiosr ? 2
                                                             : 0,
               restoreCellWidth, 5, restoreCells);

    body.removeFromTop (statusGap);

    // --- status: what the engine is doing, and what the toggles will cost ---
    {
        juce::String line1, line2;
        auto colour1 = theme::text55;

        switch (engineState.state)
        {
            case CleanState::running:
                line1 = "RUNNING"
                        + (engineState.stage.isNotEmpty()
                               ? " " + stageLabel (engineState.stage)
                               : juce::String())
                        + juce::String::fromUTF8 (" \xc2\xb7 ")
                        + juce::String ((int) std::lround (engineState.progress * 100.0f)) + "%";
                colour1 = theme::accentSoft;
                line2 = params.restore == VocalRestoreMode::audiosr
                            ? "AUDIOSR TAKES MINUTES — THE RUN CONTINUES IF YOU CLOSE THIS PANEL"
                            : "OFFLINE PYTHON SIDECAR — THE RUN CONTINUES IF YOU CLOSE THIS PANEL";
                break;

            case CleanState::done:
                if (engineState.error.isNotEmpty())
                {
                    // A rerun died and the engine restored the previous
                    // render: the failure needs saying, but so does the fact
                    // that a clean vocal is still what everyone hears.
                    line1 = "FAILED — " + ui::upperCased (engineState.error);
                    colour1 = theme::accentMid;
                    line2 = "THE PREVIOUS CLEAN RENDER STAYS ACTIVE — TWEAK AND RUN AGAIN";
                }
                else
                {
                    line1 = "CLEAN RENDER ACTIVE — AUDITION, DRAG AND EXPORT ALL USE IT";
                    colour1 = theme::accentPale;
                    line2 = "REVERT RETURNS THE RAW VOCALS; THE RENDER IS KEPT FOR NEXT TIME";
                }
                break;

            case CleanState::failed:
                // Failed with no render to fall back to (a failed rerun keeps
                // the previous render active instead — the `done` case above).
                line1 = "FAILED — " + ui::upperCased (engineState.error);
                colour1 = theme::accentMid;
                line2 = "THE RAW VOCALS ARE UNTOUCHED — TWEAK AND RUN AGAIN";
                break;

            case CleanState::notSetUp:
            case CleanState::none:
                if (! setUp)
                {
                    line1 = "SIDECAR NOT INSTALLED — RUN tools/cleanvocal/setup.sh FIRST";
                    line2 = "AN OFFLINE PYTHON RUNTIME WITH ITS OWN MODELS, INSTALLED ONCE";
                }
                else
                {
                    line1 = "EXPERIMENTAL — RUNS THE VOCALS THROUGH AN OFFLINE PYTHON CHAIN";
                    line2 = params.restore == VocalRestoreMode::audiosr
                                ? "AUDIOSR RESTORATION TAKES MINUTES, EVEN ON THIS MACHINE"
                            : params.restore == VocalRestoreMode::apollo
                                ? "APOLLO IS THE QUICKER RESTORATION OF THE TWO"
                                : "PICK AT LEAST ONE STAGE, THEN RUN";
                }
                break;
        }

        g.setFont (theme::mono (8.5f, 0.06f));
        g.setColour (colour1);
        g.drawText (line1, body.removeFromTop (statusLine), juce::Justification::centredLeft);
        g.setColour (theme::text40);
        g.drawText (line2, body.removeFromTop (statusLine), juce::Justification::centredLeft);

        // The bar only exists while something is actually running.
        auto barRow = body.removeFromTop (barHeight + 5);

        if (engineState.state == CleanState::running)
        {
            const auto bar = barRow.removeFromBottom (barHeight);
            g.setColour (theme::lineSoft15);
            g.fillRect (bar);
            g.setColour (theme::accentMid);
            g.fillRect (bar.withWidth ((int) ((float) bar.getWidth()
                                              * juce::jlimit (0.0f, 1.0f, engineState.progress))));
        }
    }

    // --- footer: RUN / CANCEL left, REVERT beside it, CLOSE right ---
    auto footer = panel.reduced (panelPad).removeFromBottom (ui::TagButton::idealHeight);

    if (engineState.state == CleanState::running)
    {
        runBounds = ui::drawTagAction (g, "CANCEL", footer.getX(), footer.getY(),
                                       hoveredAction == 8, theme::line, theme::text55);
    }
    else
    {
        const auto canRun = runnable() && setUp;

        runBounds = ui::drawTagAction (g, "RUN", footer.getX(), footer.getY(),
                                       hoveredAction == 8 && canRun,
                                       canRun ? theme::accentTint40 : theme::lineSoft15,
                                       canRun ? theme::accentPale : theme::text35);
    }

    revertBounds = {};

    if (engineState.state == CleanState::done || engineState.state == CleanState::failed)
        revertBounds = ui::drawTagAction (
            g, engineState.state == CleanState::done ? "REVERT" : "DISMISS",
            runBounds.getRight() + 8, footer.getY(),
            hoveredAction == 9, theme::line, theme::text55);

    const auto closeWidth =
        (int) std::ceil (ui::textWidth (theme::mono (9.5f, 0.08f), "CLOSE")) + 24;

    closeBounds = ui::drawTagAction (g, "CLOSE", footer.getRight() - closeWidth,
                                     footer.getY(), hoveredAction == 10,
                                     theme::line, theme::text55);
}

void VocalCleanPanel::mouseUp (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    for (int i = 0; i < 2; ++i)
    {
        if (dereverbCells[i].contains (p))
        {
            params.dereverb = i == 1;
            repaint();
            return;
        }

        if (echoCells[i].contains (p))
        {
            params.dereverbEcho = i == 1;
            repaint();
            return;
        }
    }

    for (int i = 0; i < 3; ++i)
    {
        if (restoreCells[i].contains (p))
        {
            params.restore = i == 1   ? VocalRestoreMode::apollo
                             : i == 2 ? VocalRestoreMode::audiosr
                                      : VocalRestoreMode::none;
            repaint();
            return;
        }
    }

    if (runBounds.contains (p))
    {
        if (engineState.state == CleanState::running)
        {
            if (onCancel != nullptr)
                onCancel();
        }
        else if (runnable() && setUp && onRun != nullptr)
        {
            onRun (params);
        }

        return;
    }

    if (revertBounds.contains (p))
    {
        if (onRevert != nullptr)
            onRevert();

        return;
    }

    if ((closeBounds.contains (p) || ! panelBounds().contains (p)) && onClose != nullptr)
        onClose();
}

void VocalCleanPanel::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    auto action = 0;

    for (int i = 0; i < 2; ++i)
    {
        if (dereverbCells[i].contains (p))
            action = 1 + i;

        if (echoCells[i].contains (p))
            action = 3 + i;
    }

    for (int i = 0; i < 3; ++i)
        if (restoreCells[i].contains (p))
            action = 5 + i;

    if (runBounds.contains (p))
        action = 8;
    else if (revertBounds.contains (p))
        action = 9;
    else if (closeBounds.contains (p))
        action = 10;

    if (action != hoveredAction)
    {
        hoveredAction = action;
        repaint();
    }
}

void VocalCleanPanel::mouseExit (const juce::MouseEvent&)
{
    hoveredAction = 0;
    repaint();
}

} // namespace splittr
