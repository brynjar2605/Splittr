# clean_vocal pipeline contract (issue #29)

The seam between the plugin (`src/engine/VocalCleaner`) and the Python
sidecar (`tools/cleanvocal/clean_vocal.py`). Both sides implement exactly
this; neither assumes anything about the other beyond it.

## Runtime root

Default `~/Library/Application Support/Splittr/cleanvocal/`, overridable via
the `SPLITTR_CLEANVOCAL_ROOT` environment variable (the plugin passes
`--root` explicitly when set). Created by `tools/cleanvocal/setup.sh`:

```
<root>/
  clean_vocal.py       # installed copy of the pipeline CLI
  venv-sep/            # python3.11: audio-separator[cpu]
  venv-restore/        # python3.11: audiosr (patched), omegaconf, soundfile,
                       #   matplotlib, setuptools<81
  apollo/              # JusperLee/Apollo clone (pinned commit)
  apollo/models/pytorch_model.bin
  models/              # audio-separator checkpoints (NOT the /tmp default)
```

The plugin treats a missing `<root>/clean_vocal.py` as "not set up" and says
so in the UI; it never attempts setup itself.

## Invocation

```
<root>/venv-sep/bin/python <root>/clean_vocal.py \
    --input <abs wav> --output <abs wav> \
    [--dereverb] [--dereverb-echo] --restore none|apollo|audiosr \
    [--root <dir>] [--seed N]
```

- Stage order is fixed: dereverb → dereverb-echo → restore. Flags select
  which run; at least one must be requested (a no-op invocation is an ERROR —
  the caller guards with `VocalCleanParams::isNoop()`).
- `--seed` (default 42) pins AudioSR's sampler so stereo channels and reruns
  agree.
- Output: same sample rate and channel count as the input, float32 WAV,
  never clipped: every WAV the plugin itself serves is float32, and the
  restoration stages routinely overshoot 0 dBFS, so anything narrower here
  would make the cleaned render the one clipped file in the chain.
  AudioSR's native output is mono/48k: stereo inputs run once per channel
  with the same seed and recombine; the result is resampled back.
- Exit 0 on success, non-zero on any failure.

## stdout protocol

Line-buffered, one message per line, parsed by the plugin. stderr is logged,
never parsed.

```
STAGE <name> <i> <n>   # entering stage i of n; name is one of
                       #   dereverb | dereverb-echo | restore-apollo |
                       #   restore-audiosr
PROGRESS <p>           # integer 0-100, monotonic across the whole run;
                       #   stage boundaries at minimum, finer when cheap
DONE <abs path>        # last line on success; path == --output
ERROR <message>        # last line on failure, one line, human-readable
```

Anything else on stdout is ignored (models print freely; the parser matches
only these four prefixes at line start).

## splittr_cli test hook

`splittr_cli --clean <in.wav> <out.wav> [--dereverb] [--dereverb-echo]
[--restore none|apollo|audiosr]` drives VocalCleaner end-to-end and mirrors
the protocol lines to its own stdout, so the whole seam is testable without
a DAW.
