# Handoff: StemSplit plugin UI — option 3a "Bench, dark"

## Overview
UI for a macOS AU/VST3 audio plugin (JUCE) that performs offline AI stem separation. The user drops
an audio file on the plugin, a background worker runs htdemucs (demucs.cpp) inference, and the result
is two rendered WAV stems — **Vocals** and **Instrumental** — that the user auditions in the plugin
and drags out onto Ableton tracks.

Option 3a is the approved direction: the technical "bench instrument" layout (labelled readouts,
ruled waveform, four-source table) in a dark purple terminal palette, with dot-matrix progress bars
that flicker like embers.

## About the Design Files
The files in this bundle are **design references created in HTML** — prototypes showing intended
look and behaviour, not production code to copy. The target environment here is **C++17 / JUCE 8**,
so the task is to recreate these layouts with JUCE `Component` subclasses and custom `paint()` code
(or a JUCE-native styling approach of your choosing). No HTML, CSS, or web runtime should ship in
the plugin. Treat the HTML as the spec for geometry, colour, type, and motion.

`Stem Plugin Concepts.dc.html` contains every concept explored in the session. **Turn 3 / option 3a**
is the one to build; turns 1 and 2 are kept only as context for why 3a looks the way it does.

## Fidelity
**High-fidelity.** Colours, type sizes, spacing, and motion are final within ±1px / ±0.5px type.
Recreate faithfully. Two things are deliberately unresolved:
- the **idle / drop-file** state was not drawn for 3a (see "Missing states" below);
- the plugin **name and mark** are placeholders (`STEMSPLIT`, a 7×7px purple square).

## Canvas
Plugin editor is designed at **560 × 380 px** (1×). It should be resizable with a fixed aspect
constraint, or at minimum non-resizable at this size for v1. Nothing in the layout depends on
viewport units.

## Screens / Views

All three states share a **header bar** and a **content area**.

### Header bar (all states)
- Height: 39px total — padding `11px 16px`, background `#1B1622`, bottom border `1px solid rgba(221,214,230,.18)`.
- Left group, 9px gap: state mark, then plugin name.
  - Mark: 7×7px square, fill `oklch(0.68 0.19 300)` ≈ `#9C63EE`. In the **working** state the square is
    replaced by an animated `✳` glyph, 12px, same purple (see Motion → sparkle).
  - Name: `STEMSPLIT`, 11px, weight 500, letter-spacing `.16em`, colour `#DDD6E6`.
- Right: status meta, 9.5px, letter-spacing `.12em`, colour `rgba(221,214,230,.5)`:
  - first run → `FIRST RUN`
  - working → `4-SOURCE ANALYSIS`
  - done → `OUTPUT 2 × WAV · 24-BIT · 44.1 kHz`

---

### 1. First run — model download
**Purpose:** the 168 MB htdemucs weights are not on disk yet; fetch them once.

**Layout:** content padding `22px 20px`, vertical flex, 18px gap.
1. Copy block, 13px / line-height 1.6, letter-spacing `.04em`:
   - line 1 `Model weights are not on this machine.` colour `#DDD6E6`
   - line 2 `htdemucs_f16.bin — 168 MB — one time only.` colour `rgba(221,214,230,.5)`
2. Progress group, 7px gap:
   - **Matrix bar** (see Components → Matrix bar), wide variant, inside a box with
     `1px solid rgba(221,214,230,.2)`, background `rgba(221,214,230,.04)`, padding `6px 7px`.
   - Caption row, 9.5px, `rgba(221,214,230,.5)`: left `41% · 168 MB · 4.1 MB/s` (percentage is live),
     right `from huggingface.co`.
3. Footer row pinned to bottom, 12px gap, 9.5px type, letter-spacing `.08em`:
   - `CANCEL` — 1px border `rgba(221,214,230,.35)`, padding `6px 12px`
   - `LOCATE FILE MANUALLY…` — 1px border `rgba(221,214,230,.18)`, text `rgba(221,214,230,.55)`
   - right-aligned path hint `→ ~/…/StemSplit/models`, `rgba(221,214,230,.4)`

---

### 2. Working — 4-source analysis
**Purpose:** show that inference is running, on what, and how the four htdemucs sources map to two stems.

**Layout:** content padding `14px 16px`, horizontal flex, 14px gap. Left column flexes; right column
is a fixed **196px** table.

**Left column** (8px gap):
- Ruler row, 9px, letter-spacing `.1em`, `rgba(221,214,230,.45)`: `0:00` · `MIDNIGHT_TAKE3.WAV` · `3:48`, space-between.
- **Waveform box**, flexes to fill. Border `1px solid rgba(221,214,230,.2)`, background
  `rgba(221,214,230,.03)` over a 13×13px grid drawn with 1px lines at `rgba(168,132,240,.09)`.
  Inner padding `0 4px`. Bars: 60 columns, 1px gap, colour `rgba(221,214,230,.55)`, height = |sample| as
  a percentage of box height, vertically centred.
  - Playhead: 1px vertical line at `progress%`, colour `oklch(0.72 0.17 300)` ≈ `#AB7CF2`, full height.
  - `HEAD` label at the playhead, 8px, letter-spacing `.06em`, colour `oklch(0.78 0.14 300)` ≈ `#C09AF6`,
    background `#141118`, padding `1px 3px`, offset 3px from the top. **Clamp its x to ≤84%** so it never
    overflows the box.
- Tick strip below, 9px tall: 1px marks every 13px at `rgba(221,214,230,.5)`.

**Right column — source table.** Border `1px solid rgba(221,214,230,.2)`, background `rgba(221,214,230,.03)`.
- Header row: padding `6px 8px`, bottom border `1px solid rgba(221,214,230,.2)`, 8.5px, letter-spacing `.12em`,
  `rgba(221,214,230,.5)`: `SOURCE` / `PEAK`.
- Four rows, padding `7px 8px`, bottom border `1px solid rgba(221,214,230,.1)`:

  | source | peak | role |
  |---|---|---|
  | vocals | -6.2 dB | stem 1 |
  | drums | -9.8 dB | sum → stem 2 |
  | bass | -11.4 dB | sum → stem 2 |
  | other | -7.9 dB | sum → stem 2 |

  Name/peak line 10px (`#DDD6E6` / `rgba(221,214,230,.55)`); role line 8px, letter-spacing `.08em`,
  `rgba(221,214,230,.4)`.
- Footer block pinned to bottom, padding 8px, 6px gap:
  - Label row 9px letter-spacing `.08em`: `INFERENCE` `rgba(221,214,230,.6)` / `41%` in `#C09AF6`.
  - **Matrix bar**, small variant (56 columns), inside a 20px-tall box.
  - Caption row 8px `rgba(221,214,230,.4)`: `seg 3/7` / `eta 62s`.

---

### 3. Done — two stems
**Purpose:** audition each stem and drag it onto a track.

**Layout:** content padding `14px 16px`, vertical flex, 11px gap: vocals row, instrumental row, footer.

**Stem row** (identical geometry for both):
- Outer: horizontal flex, `cursor: grab`, and it is the **external drag source** for its WAV.
  - Vocals: border `1px solid rgba(168,132,240,.35)`, background `rgba(168,132,240,.06)`.
  - Instrumental: border `1px solid rgba(221,214,230,.2)`, no fill.
- **Grip column**, 34px wide, right border `1px solid rgba(221,214,230,.15)`, centred 9×17px dot grid
  (1px dots on a 4px pitch, `rgba(221,214,230,.4)`) — the affordance for dragging out.
- **Body**, padding `9px 11px`, 7px gap:
  - Title row, 12px gap, centre-aligned:
    - stem name 12px, letter-spacing `.1em` — vocals in `oklch(0.82 0.11 300)` ≈ `#CDB2F8`, instrumental in `#DDD6E6`
    - subtitle 8.5px `rgba(221,214,230,.45)` — `source 1 of 4 · isolated` / `drums + bass + other, summed`
    - **transport**, right-aligned, 8px gap: 20px circle play button (1px border, `▶` 7.5px, purple for
      vocals / `rgba(221,214,230,.5)` for instrumental); 74×3px track `rgba(221,214,230,.18)` with a
      34%-width played fill; 2px×9px playhead tick (vocals only); elapsed `1:17`, 8px, `rgba(221,214,230,.45)`.
  - Waveform, 36px tall, 70 columns, 1.5px gap — vocals `#AB7CF2`, instrumental `rgba(221,214,230,.6)`.
- Footer row, 16px gap, 9px, letter-spacing `.08em`, `rgba(221,214,230,.5)`:
  `96.4 s · 2.1 GB PEAK` · `REVEAL` · `NEW FILE` · right-aligned `DRAG A ROW TO A TRACK` in `#C09AF6`.

---

### Missing states (must be designed/derived before shipping)
- **Idle / drop target** — reuse option 2c's treatment: a dashed `1px rgba(168,132,240,.4)` panel with
  `rgba(168,132,240,.05)` fill, `drop an audio file into this window` in `#C09AF6` 12.5px, and
  `wav · aiff · mp3 · flac` in `rgba(221,214,230,.35)` 9.5px.
- **Drag-over highlight** — brighten the panel border to full purple.
- **Error** — unsupported file, decode failure, download failure, out-of-memory. Use the header meta slot
  for the short reason and the footer row for the retry action.
- **Cancelled** — returns to idle; partially written stems deleted.

## Interactions & Behavior
- **Drop in:** accept `.wav .aiff .aif .mp3 .flac`. On drop, cancel any running job and start a new one.
  Decode with `AudioFormatManager`, resample to 44.1 kHz stereo.
- **Progress:** worker publishes a normalised float via `std::atomic<float>`; editor polls at 30 Hz
  (`juce::Timer`) and drives percentage text, matrix-bar fill, playhead x, and ETA. ETA is derived from
  elapsed/progress, not shown until progress > 0.05.
- **Cancel:** `CANCEL` in first-run aborts the download; in working the same slot aborts the job. Closing the
  editor must not cancel the job (job lives on the processor).
- **Play:** each stem row's play button auditions that stem through the plugin's own transport
  (`AudioTransportSource` in the processor), not the host. Only one stem plays at a time; starting one
  stops the other. Scrubber is draggable; playhead reflects position.
- **Drag out:** press-and-drag anywhere in a stem row (grip is the visual cue) calls
  `DragAndDropContainer::performExternalDragDropOfFiles({wavPath}, false)`. Stems live in
  `~/Library/Application Support/StemSplit/cache/<hash>/` so drag still works after the editor reopens.
- **Hover:** rows lighten their border by ~10% alpha; text buttons (`REVEAL`, `NEW FILE`, `CANCEL`) go to
  full-opacity `#DDD6E6`. No motion on hover.

## Motion
Three separate behaviours, all cheap:

1. **Matrix bar fill** — a soft-edged wipe. The lit cell field is masked by a horizontal gradient:
   opaque to 33.3% of the mask, `.5` alpha at 36.5%, transparent by 40.5%, mask sized 300% of the bar and
   positioned at `50 − progress/2` percent. Net effect: the boundary is a ~7%-wide dither ramp, not a hard
   edge. In JUCE: draw cells and multiply each cell's alpha by that ramp function of its x.
2. **Ember flicker** — every lit cell also cycles its own alpha through
   `1 → .32 → .86 → .5 → 1` with `steps(1, end)` (discrete snaps, no tweening), period **0.34–1.24 s** and a
   negative start delay, both derived per cell from a deterministic hash of (col, row). No two neighbours are
   in phase; the field reads as fire. In JUCE: per-cell phase/period arrays computed once at construction,
   sampled against a single 30 Hz clock.
3. **Sparkle** — the working-state `✳` rotates 0→180° and scales `.88→1.12` with alpha `.45→1`, 1.4 s,
   ease-in-out, infinite.

The mock also advances a fake progress counter (+5% every 160 ms, wrapping) purely so the animation is
visible; the real value comes from the engine callback.

## State Management
Processor-owned (survives editor close):
- `enum class Phase { NeedsModel, Downloading, Idle, Decoding, Separating, Rendering, Ready, Error }`
- `std::atomic<float> progress` (0–1), `std::atomic<Phase>`, `String errorMessage`
- `File sourceFile`, `File vocalsWav`, `File instrumentalWav`, source peak dBFS ×4
- `AudioThumbnail` (or a cached downsampled peak array) per stem for the waveforms
- transport: which stem is playing, position

Editor-owned: 30 Hz timer, animation phase, hover/press states, scrubber drag.

Threading: separation runs on a `ThreadPoolJob`; `processBlock` is pass-through (plus optional audition
playback). Never touch model state from the audio thread.

## Design Tokens

Colours — oklch is authoritative, hex is the sRGB approximation:

| token | value | hex ≈ | use |
|---|---|---|---|
| bg | `#141118` | `#141118` | editor background |
| bg-header | `#1B1622` | `#1B1622` | header bar |
| bg-panel | `rgba(221,214,230,.03)` | — | waveform box, table |
| bg-panel-alt | `rgba(221,214,230,.04)` | — | matrix bar box |
| accent | `oklch(0.68 0.19 300)` | `#9C63EE` | mark, active borders |
| accent-mid | `oklch(0.72 0.17 300)` | `#AB7CF2` | matrix cells, playhead, vocals wave |
| accent-soft | `oklch(0.78 0.14 300)` | `#C09AF6` | live numbers, HEAD label, hint text |
| accent-pale | `oklch(0.82 0.11 300)` | `#CDB2F8` | VOCALS title |
| accent-tint-6 | `rgba(168,132,240,.06)` | — | vocals row fill |
| accent-tint-35 | `rgba(168,132,240,.35)` | — | vocals row border |
| grid-line | `rgba(168,132,240,.09)` | — | 13px waveform grid |
| text | `#DDD6E6` | `#DDD6E6` | primary |
| text-60 / 50 / 45 / 40 | `rgba(221,214,230,.6/.5/.45/.4)` | — | secondary → tertiary |
| line-strong | `rgba(221,214,230,.2)` | — | panel borders |
| line | `rgba(221,214,230,.18)` | — | header rule |
| line-soft | `rgba(221,214,230,.15/.1)` | — | dividers, table rows |

Type — one family, monospace. Mock uses **DM Mono**; ship a bundled mono (JetBrains Mono or DM Mono
via `BinaryData`) rather than relying on system fonts.

| role | size | weight | letter-spacing |
|---|---|---|---|
| plugin name | 11px | 500 | .16em |
| header meta | 9.5px | 400 | .12em |
| body copy | 13px | 400 | .04em |
| stem title | 12px | 400 | .1em |
| table row | 10px | 400 | 0 |
| caption / footer | 9–9.5px | 400 | .08–.1em |
| micro (role, eta, time) | 8–8.5px | 400 | .08–.14em |

Spacing scale: 2, 4, 6, 7, 8, 9, 11, 12, 14, 16, 18, 20, 22px.
Radius: **0 everywhere**, except the 20px play button and the 7×7 mark's dot grid (circular dots).
Shadows: none.
Strokes: hairline 1px only.
Matrix bar: cell 2px tall × 1fr wide, 1px gap, 7 rows; 168 columns (wide) / 56 columns (small);
base layer alpha `.13`, lit cell alpha `0.3–1.0` from a per-cell hash.

## Assets
- No production assets. The mark is a 7×7px filled square; the sparkle is the `✳` glyph; the play button is
  the `▶` glyph; grips are procedural dot grids. Replace the mark and glyphs with real vector assets before
  release.
- `assets/herd.png` in this bundle is the user's own reference artwork, used only in concept 2a. Not part of 3a.

## Files
- `Stem Plugin Concepts.dc.html` — all concepts. **Turn 3 → option 3a** is the design to build; turn 2
  (`2a` sleeve, `2b` light bench, `2c` terminal) and turn 1 are context, including 2c's idle state which 3a
  should borrow.
- `assets/herd.png` — reference artwork for concept 2a only.
- Open the HTML directly in a browser to see the live motion.
