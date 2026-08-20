# third_party/basicpitch

Audio-to-MIDI transcription for Splittr: Spotify's **Basic Pitch** model, run
in-process with no Python and no ONNX Runtime.

Vendored as a copied subtree rather than a submodule because what we need is a
slice of two repositories — NeuralNote's transcription core and RTNeural's
headers — and neither upstream build can produce just that slice.

## Where each file came from

| Path | Upstream | Commit | License |
|---|---|---|---|
| `BasicPitchCNN.{h,cpp}` | [DamRsn/NeuralNote](https://github.com/DamRsn/NeuralNote) `Lib/Model/` | `f979e51` | Apache-2.0 |
| `BasicPitchConstants.h` | NeuralNote `Lib/Model/` | `f979e51` | Apache-2.0 |
| `Notes.{h,cpp}` | NeuralNote `Lib/Model/` | `f979e51` | Apache-2.0 |
| `weights/cnn_*.json` | NeuralNote `Lib/ModelData/` | `f979e51` | Apache-2.0 |
| `weights/features_model.onnx` | NeuralNote `Lib/ModelData/` | `f979e51` | Apache-2.0 |
| `RTNeural/**` | [jatinchowdhury18/RTNeural](https://github.com/jatinchowdhury18/RTNeural), via NeuralNote's `ThirdParty/RTNeural/RTNeural/` | as pinned by NeuralNote `f979e51` | BSD-3-Clause (`RTNeural/LICENSE`) |
| `modules/json/json.hpp` | [nlohmann/json](https://github.com/nlohmann/json), via NeuralNote's `ThirdParty/RTNeural/modules/json/` | as pinned by NeuralNote `f979e51` | MIT (in the header) |

`modules/json/` keeps that name because RTNeural's `model_loader.h` includes
`"../modules/json/json.hpp"` relative to itself.

The model weights themselves are Spotify's, from
[spotify/basic-pitch](https://github.com/spotify/basic-pitch) (Apache-2.0);
NeuralNote redistributes them converted to RTNeural's JSON format.

Apache-2.0, BSD-3-Clause and MIT are all compatible with Splittr's AGPL-3.0.
`LICENSE-NeuralNote` is NeuralNote's copy of Apache-2.0.

## What Splittr wrote

| Path | Why |
|---|---|
| `Features.{h,cpp}` | Replaces NeuralNote's ONNX-Runtime-backed `Lib/Model/Features.{h,cpp}`. |
| `BasicPitch.{h,cpp}` | Replaces NeuralNote's `Lib/Model/BasicPitch.{h,cpp}`. |
| `NoteUtils.h` | Replaces NeuralNote's `Lib/Utils/NoteUtils.h`, minus its JUCE dependency. |
| `weights/cqt_kernels.bin` | Generated from `weights/features_model.onnx` by `tools/basicpitch/extract_cqt_kernels.py`. |

### Why the ONNX Runtime dependency is gone

Basic Pitch is two stages: a CQT front end that turns audio into a
harmonically stacked constant-Q spectrogram, and a small CNN that turns that
into onset/note/contour posteriorgrams. NeuralNote runs the first stage as an
ONNX graph (`features_model.onnx`) and the second through RTNeural.

That first graph has no learned parameters. It is nnAudio's `CQT2010v2`
exported by tf2onnx — 201 nodes of reflect-pad, strided conv1d, magnitude, a
normalised log and a harmonic gather — and only four constant tensors, 76 KB in
total. Linking ONNX Runtime for it costs a nine-figure byte count and a
custom-built static library that NeuralNote has to download from a third-party
release; `third_party/basicpitch/Features.cpp` reimplements the topology
directly instead. `tools/basicpitch/extract_cqt_kernels.py` lifts the four
tensors into `weights/cqt_kernels.bin`, and the `.onnx` stays in the tree so
the extraction can be re-run and audited.

Checked against the original graph under `onnxruntime` on tone-plus-noise
inputs of several lengths: maximum absolute difference `5e-4` across the whole
output, which is float32 summation-order noise.

### Local edits to vendored files

`Notes.{h,cpp}`, `BasicPitchConstants.h`, `RTNeural/**` and
`modules/json/json.hpp` are byte-identical to upstream. `BasicPitchCNN.{h,cpp}`
has one change, marked `Splittr change:` in the source: the four model JSONs are
passed to the constructor instead of being read out of JUCE's generated
`BinaryData.h`.

Upstream's `Lib/Model/Utils.h` is not vendored — nothing that survived here
calls its one function.
