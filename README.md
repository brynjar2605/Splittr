# Splittr

AU / VST3 plugin for Ableton Live (macOS) that splits a song into stems with
AI source separation — drop an audio file on the plugin, audition the stems,
drag them onto your tracks or export them to a folder.

Separation runs fully offline on your machine using Meta's pretrained
**Demucs v4 (htdemucs)** model via the C++ port
[demucs.cpp](https://github.com/sevagh/demucs.cpp) — no cloud, no account,
no training — on the CPU, or many times faster than realtime on the Apple
Silicon GPU (Metal). A scan produces **five stems**: the four htdemucs sources
(**Vocals, Drums, Bass, Other**) plus **Instrumental** (everything but the
voice, summed) — or **seven** in 6-stem mode, where `htdemucs_6s` adds
**Guitar** and **Piano**. It detects the track's **BPM and key** along the way.

Every stem can then be shaped before it leaves: **TUNE** opens a per-stem
parametric EQ — up to 24 bands over a live spectrum, each optionally dynamic —
plus a compressor and output gain, heard live and rendered into what you drag
out.

A separated stem can also leave as **MIDI**: Spotify's **Basic Pitch** runs
in-process on the pitched stems, and the resulting `.mid` drags onto a track
the same way the audio does.

The vocal stem has one more, experimental, trick: **CLEAN VOCAL** runs it
through a chain of open restoration models — de-reverb, de-echo, and a
restoration pass (**Apollo**, or **AudioSR**, a diffusion model) that
regenerates the frequencies separation lost — for a dry vocal that no longer
carries the mix's room. Opt-in, non-destructive, offline like everything
else; see the setup section below.

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

It also transcribes a single stem and reports what the written file actually
contains, parsed back with `juce::MidiFile`:

```sh
build/splittr_cli_artefacts/Release/splittr_cli --midi <stem.wav> <out.mid> [bpm]
```

## Model weights

On first run the plugin downloads the pretrained weights once
(`ggml-model-htdemucs-4s-f16.bin`, 84 MB, MIT-licensed, converted by the
demucs.cpp author) to `~/Library/Application Support/Splittr/models/`. If
you're offline, download the file yourself from
[huggingface.co/datasets/Retrobear/demucs.cpp](https://huggingface.co/datasets/Retrobear/demucs.cpp)
and use **LOCATE FILE MANUALLY…** in the plugin.

## Clean vocal (experimental)

The CLEAN chip needs a one-time local runtime the plugin does not ship: two
Python environments and the model checkpoints, several GB in total, installed
by

```sh
tools/cleanvocal/setup.sh
```

into `~/Library/Application Support/Splittr/cleanvocal/` (requires Homebrew
`python3.11`; idempotent, so rerunning after an interruption resumes). Until
it has run, the panel says so and points here. The models are fetched from
their authors' releases at setup time — community-trained checkpoints whose
licenses vary, which is one reason this stays a local, experimental sidecar
rather than something the plugin bundles. Details in
`tools/cleanvocal/README.md`.

The CLI exercises the same chain without a DAW:

```sh
build/splittr_cli_artefacts/Release/splittr_cli --clean <stem.wav> <out.wav> \
    [--dereverb] [--dereverb-echo] [--restore none|apollo|audiosr]
```

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
- **Playback** (done screen): all stems play in sync through one transport —
  the circular button in the footer starts and pauses everything, each row's
  circle mutes/unmutes that stem (muted rows dim), and clicking anywhere on a
  waveform jumps there. Audible stems "burn" behind the playhead with the
  same ember effect as the loading bar.
- **LIBRARY** (idle screen, bottom left) lists every song you've split —
  duration, BPM/key, engine mode — and reopens any of them instantly, even if
  the original audio file has since moved or been deleted. Entries can be
  deleted individually; the footer shows total cache size.
- **DRAG ALL** (done screen, left of the footer) drags every stem out at once.
  Ableton makes one track per file, so a whole stem session lands in the set in
  a single gesture. Dragging a single row still drags just that stem.
- **MIDI chip** (done screen, on the right of a stem's title strip): click it to
  transcribe that stem, then drag the chip onto a track to drop the `.mid` — a
  MIDI clip rather than an audio one. It appears on **Vocals, Bass, Other,
  Guitar and Piano**; drums are onset classification rather than pitch, and the
  summed instrumental is a full mix, so neither offers it. A stem takes a few
  seconds (a 2½-minute stem, about 2). Transcriptions live beside their stems
  in the cache, so reopening a session finds them already done;
  cmd-click a finished chip to run it again.
- **CLEAN chip** (done screen, on the Vocals row beside MIDI — experimental):
  opens a panel with two independent stage toggles — **DE-REVERB** (MelBand
  RoFormer by anvuew) and **DE-REVERB + ECHO** (Sucial v2, better on delay
  throws) — and a restoration selector: **NONE**, **APOLLO** (single-pass
  music restoration, seconds), or **AUDIOSR** (latent diffusion, minutes —
  the panel is honest about it). RUN renders the chain offline; while a
  render is active it *is* the vocal — audition, drag, export, TUNE's bounce
  and MIDI all serve it, under the stem's normal export name — and REVERT
  returns to the raw render, which is never touched. Each toggle combination
  caches separately and the choice survives reopening the session.
- **TUNE** (done screen, the mix icon on a stem row's left edge) opens the
  stem's shaping panel: an interactive parametric EQ — up to 24 bands of any
  shape (bells, shelves, cuts to 96 dB/oct, notch, band-pass, tilts,
  all-pass), each optionally dynamic (gain rides the band's own level, with
  attack/release) — drawn over a live pre/post spectrum of the playing stem.
  Double-click to add a band, drag it, scroll for Q, right-click for shape,
  slope, dynamics and solo; click a peak in the spectrum to grab it. The
  strip below keeps the compressor, output gain, phase invert and auto gain.
  Everything is heard live through the audition, and a shaped stem drags and
  exports as exactly what you heard; a flat stem stays the raw render byte
  for byte. Sessions reopen with their shaping intact — including entries
  tuned before this panel existed.
- **EXPORT…** (done screen) copies every stem to a folder you choose
  (remembered for next time), with each stem's `.mid` beside it where one
  exists; **REVEAL** opens the internal cache in Finder.
- Stems that leave the plugin — dragged or exported — are named
  `<song> - <Stem> - 124BPM - F#m.wav`, and carry the same values in their
  WAV LIST/INFO metadata. Whatever came back undetected is simply left out, so
  a rubato track gets no BPM part and an atonal one no key part.
- **SETTINGS** (the mark at the header's right, reachable from any state):
  dark or light THEME, a full-spectrum ACCENT hue strip — click or drag to
  recolor every accent in the plugin live — the FIRE grain of the burn
  effect (fine matrix cells or the original coarse bars), and PROCESSING,
  which runs separation on the CPU or on the Apple Silicon GPU (Metal, greyed
  out where it isn't available, and falling back to the CPU for any run it
  can't take). DIAGNOSTICS in the panel's corner opens a copyable report —
  build, machine, compute state, the last run and the engine's event log —
  for pasting into a bug report; nothing is sent anywhere by itself.
  Preferences are global and persist.
- **BPM** comes from a multi-band onset/tempogram detector with octave
  disambiguation, validated to ±2% on steady-tempo material; when the pulse
  is too ambiguous (rubato, free time) it reports nothing rather than a
  number you shouldn't trust. **Key** (chromagram) remains a hint — best on
  steady tonal music.
- Input: wav / aiff / mp3 / flac / m4a / ogg, up to 10 minutes (v1 limit).
- Separation speed follows the PROCESSING setting: on the GPU (Apple Silicon,
  macOS 14+) it runs at roughly 15-20x realtime — a 4-minute song in well
  under half a minute — and on the CPU at very roughly realtime, a few minutes
  for the same song. Either way Live keeps playing; the work happens on a
  low-priority background thread and survives closing the plugin window.
- Rendered stems are 32-bit float 44.1 kHz WAVs cached in
  `~/Library/Application Support/Splittr/cache/` so drags keep working after
  reopening a set. Use Live's "Collect All and Save" if you move projects
  between machines.
- Re-dropping an unchanged file is an instant cache hit.

## Licenses

- Splittr: **AGPLv3** (uses JUCE 8 under its open-source license)
- [demucs.cpp](https://github.com/sevagh/demucs.cpp): MIT · Eigen: MPL2
  (`third_party/demucs-overrides/` contains files derived from demucs.cpp,
  same MIT terms — see its README for what changed and why)
- htdemucs weights: MIT (Meta AI research model, ggml conversion by @sevagh)
- Basic Pitch model and weights: Apache-2.0
  ([spotify/basic-pitch](https://github.com/spotify/basic-pitch)), vendored via
  [NeuralNote](https://github.com/DamRsn/NeuralNote) (Apache-2.0) with RTNeural
  (BSD-3) and nlohmann/json (MIT) — see `third_party/basicpitch/PROVENANCE.md`
- DM Mono font: SIL Open Font License
