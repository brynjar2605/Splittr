# Splittr

AU / VST3 plugin for Ableton Live (macOS) that splits a song into **Vocals** and
**Instrumental** stems with AI source separation — drop an audio file on the
plugin, audition the stems, drag them out onto your tracks.

Separation runs fully offline on your machine using Meta's pretrained
**Demucs v4 (htdemucs)** model via the C++ port
[demucs.cpp](https://github.com/sevagh/demucs.cpp) — no cloud, no account,
no training. The four htdemucs sources map to two stems: `vocals` is used
as-is; `drums + bass + other` are summed into the instrumental.

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

A CLI harness is also built for testing the engine directly:

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
