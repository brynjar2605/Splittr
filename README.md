# Splittr

AU / VST3 plugin for Ableton Live (macOS) that splits a song into stems with
AI source separation — drop an audio file on the plugin, audition the stems,
drag them onto your tracks or export them to a folder.

Separation runs fully offline on your machine using Meta's pretrained
**Demucs v4 (htdemucs)** model via the C++ port
[demucs.cpp](https://github.com/sevagh/demucs.cpp) — no cloud, no account,
no training. A scan produces **five stems**: the four htdemucs sources
(**Vocals, Drums, Bass, Other**) plus **Instrumental** (everything but the
voice, summed) — or **seven** in 6-stem mode, where `htdemucs_6s` adds
**Guitar** and **Piano**. It detects the track's **BPM and key** along the way.

## Building

Requirements: macOS 11+, Xcode command line tools, CMake ≥ 3.24.

```sh
git clone --recursive <this repo>
cd splittr
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

The build auto-installs to `~/Library/Audio/Plug-Ins/Components/Splittr.component`
and `~/Library/Audio/Plug-Ins/VST3/Splittr.vst3`. After AU rebuilds, run
`killall -9 AudioComponentRegistrar` and rescan plugins in your DAW
(Live: Settings → Plug-Ins → alt-click Rescan).

A CLI harness is also built for testing the engine directly. It writes one WAV
per stem — five for a 4-source model, seven for `htdemucs_6s`:

```sh
build/splittr_cli_artefacts/Release/splittr_cli <audio-in> <model.bin> <out-dir>
```

## Model weights

On first run the plugin downloads the pretrained weights once
(`ggml-model-htdemucs-4s-f16.bin`, 84 MB, MIT-licensed, converted by the
demucs.cpp author) to `~/Library/Application Support/Splittr/models/`. If
you're offline, download the file yourself from
[huggingface.co/datasets/Retrobear/demucs.cpp](https://huggingface.co/datasets/Retrobear/demucs.cpp)
and use **LOCATE FILE MANUALLY…** in the plugin.

## Usage notes

- **ENGINE switch** (idle screen): **FAST** uses plain htdemucs (~realtime);
  **BEST** uses the fine-tuned `htdemucs_ft` ensemble — four specialist
  models, one per source — for noticeably cleaner stems at ~4× the time
  (4 × 84 MB, downloaded once on first use); **6-STEM** uses `htdemucs_6s`
  (55 MB), which separates guitar and piano as well, at about FAST's speed.
  Upstream scores those two targets well below the four standard sources, so
  expect them to bleed — the other five stems are also slightly weaker than
  FAST's. Each mode caches separately, so switching never re-runs a mode you
  have already used on that file.
- **DRAG ALL** (done screen, left of the footer) drags every stem out at once.
  Ableton makes one track per file, so a whole stem session lands in the set in
  a single gesture. Dragging a single row still drags just that stem.
- **EXPORT…** (done screen) copies every stem, named
  `<song> - <Stem>.wav`, to a folder you choose (remembered for next time);
  **REVEAL** opens the internal cache in Finder.
- **THEME / ACCENT** (idle screen): dark or light UI, plus a full-spectrum
  hue strip — click or drag to recolor every accent in the plugin live.
  Preferences are global and persist.
- BPM/key detection is a hint (onset autocorrelation + chromagram): reliable
  on steady-tempo tonal material, approximate on rubato or atonal tracks.
- Input: wav / aiff / mp3 / flac / m4a / ogg, up to 10 minutes (v1 limit).
- Separation runs at very roughly realtime speed on Apple Silicon — a 4-minute
  song takes a few minutes. Live keeps playing; the work happens on a
  low-priority background thread and survives closing the plugin window.
- Rendered stems are 32-bit float 44.1 kHz WAVs cached in
  `~/Library/Application Support/Splittr/cache/` so drags keep working after
  reopening a set. Use Live's "Collect All and Save" if you move projects
  between machines.
- Re-dropping an unchanged file is an instant cache hit.

## Licenses

- Splittr: **AGPLv3** (uses JUCE 8 under its open-source license)
- [demucs.cpp](https://github.com/sevagh/demucs.cpp): MIT · Eigen: MPL2
- htdemucs weights: MIT (Meta AI research model, ggml conversion by @sevagh)
- DM Mono font: SIL Open Font License
