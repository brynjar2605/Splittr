# demucs.cpp overrides

third_party/demucs.cpp is a pristine submodule. Nothing in it is patched, and
nothing in it should be — a dirty submodule makes the next upgrade a merge
instead of a pull. But issue #24 measured single-threaded scalar code *inside*
it as the per-thread ceiling, so some of it has to be rewritten anyway.

So: a source file dropped in this directory replaces the upstream file of the
same name. cmake/DemucsCpp.cmake globs upstream's `src/*.cpp`, swaps out any
file that also exists here, and puts this directory ahead of upstream's `src`
on the include path. That file already ignores upstream's own CMakeLists (it
wants OpenMP and `-Ofast -march=native`), so this is one more deliberate
divergence in a place that already has one, rather than a new kind of thing.

**An override starts as a byte-identical copy of the upstream file.** Copy it
in, commit that, then rewrite. Otherwise the diff that justifies the override
is invisible, and nobody can tell what was changed from what was merely
transcribed.

When bumping the submodule: re-copy each upstream file listed below, re-apply
the changes described, and re-verify. `git log` on this directory is the record
of what the rewrites were.

## layers.cpp

From demucs.cpp commit `f1206e9adeea103aef4a636b9e62297cf1f8e34e`.

Everything here is a memory-order and vectorisation change. No function
signature moved, no maths changed beyond floating-point association, and the
demucs_v3 code paths in the same file are untouched.

`Eigen::Tensor3dXf` is column-major, so `x(i, c, w)` lives at
`i + freq * c + freq * channels * w` — the first index is the fast one. Upstream
writes every one of these loop nests with `w` innermost, which advances a whole
`freq * channels` plane per element. On the frequency branch that plane is 96 KB,
so essentially every element access was a cache miss. The rewrites turn each
into a pass over whole contiguous planes, and run the per-plane arithmetic
through Accelerate's vDSP/vForce (guarded by `#ifdef __APPLE__`, with the scalar
loop as the fallback, so the file still builds elsewhere).

- **`group_norm`, `group_norm_fused_gelu`** — share one `group_norm_impl`.
  Three contiguous passes (sum, sum of squares about the mean, one fused affine
  write) replace the per-`(i, group)` `x.slice(...)` / `x.chip<0>(i)`
  materialisations plus the strided read-modify-write. Normalise-scale-shift is
  folded into a single `x * scale + shift`. GeLU stays scalar — vForce has
  `vvexpf` but no `erf` — but now runs over contiguous memory.
- **`layer_norm`** — same shape of rewrite. Its statistic runs along the last
  dimension, i.e. exactly the strided one, so accumulating a plane at a time
  computes every `(i, c)` mean at once instead of materialising a
  `x.chip(i, 0).chip(c, 0)` vector per pair.
- **`glu`** — both halves are contiguous runs whichever dimension the split is
  on, so the sigmoid becomes `vvexpf` over a straight vector instead of a
  scalar `unaryExpr` over an Eigen slice expression.

Not changed, deliberately: the attention softmax in `common_encoder_layer`. The
column-major rewrite of it verified fine but measured 0.8% — inside the noise —
and it is not worth carrying code upstream doesn't have for that.

`<Accelerate/Accelerate.h>` is *not* what gets included: it drags in
`fortran_blas.h`, whose prototypes conflict with the ones Eigen declares under
`EIGEN_USE_BLAS`. `<vecLib/vDSP.h>` and `<vecLib/vForce.h>` are the same
framework without the clash.

### Verified

Per-stem SNR 83.5–107.7 dB against the same tree built with the upstream file
(bar was 60 dB; these are association-order differences, nothing structural),
and near-silent input still behaves — 45 s of −60 dB noise and 10 s of digital
silence both give the same stem peaks as before.

This verification originally had to run on a clip short enough to stay in one
chunk: `DemucsSeparator` runs each chunk on its own thread, and upstream
`model_apply.cpp` drew the per-chunk shift offset from the global `rand()`, so
on a multi-chunk clip which chunk got which shift varied between runs and the
same binary twice only agreed to ~20 dB. The model_apply.cpp override below
fixed the offset, so every clip is now bit-identical run to run.

## model_apply.cpp

From demucs.cpp commit `f1206e9adeea103aef4a636b9e62297cf1f8e34e`.

One change, in both copies of `shift_inference` (v4 and the unused v3): the
shift offset is upstream's own commented-out fixed `1337` instead of
`rand() % max_shift`. With the chunked driver running one `demucs_inference`
per worker thread, the global `rand()` handed each chunk a different offset
depending on thread scheduling — same binary, same input, ~20 dB run-to-run
self-agreement. A single shift pass averages nothing, so any in-range offset
is as valid as a random one, and a fixed one makes separation
bit-reproducible: cache hits, SNR verification and CPU↔GPU comparisons all
depend on that. `DemucsMetal.mm` uses the same constant so the two backends
stay directly comparable.
