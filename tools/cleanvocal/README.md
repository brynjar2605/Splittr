# cleanvocal — Python sidecar for CLEAN VOCAL (issue #29)

The plugin's clean-vocal chain (de-reverb, de-reverb+echo, restoration) runs
in Python because the models do: two MelBand RoFormer checkpoints via
audio-separator, plus Apollo and AudioSR for restoration. This directory
holds the sidecar's source; the contract both sides implement is
[PROTOCOL.md](PROTOCOL.md).

## Setup (once)

```
brew install python@3.11   # if missing
tools/cleanvocal/setup.sh
```

Builds `~/Library/Application Support/Splittr/cleanvocal/` (override with
`SPLITTR_CLEANVOCAL_ROOT`): two venvs, the pinned Apollo clone with its
checkpoint, the separator checkpoints, and an installed copy of
`clean_vocal.py`. Idempotent — rerunning is a fast no-op, and a rerun after
a failure resumes where it stopped.

## Run

```
"<root>/venv-sep/bin/python" "<root>/clean_vocal.py" \
    --input in.wav --output out.wav \
    [--dereverb] [--dereverb-echo] [--restore none|apollo|audiosr] [--seed N]
```

Progress and results arrive as `STAGE`/`PROGRESS`/`DONE`/`ERROR` lines on
stdout (see PROTOCOL.md); everything the models print goes to stderr. The
output matches the input's sample rate and channel count, float32 and never
clipped — the restoration stages overshoot 0 dBFS, and the plugin's whole
chain is float32.

## Files

- `setup.sh` — builds/repairs the runtime root; pins every moving part
- `patch_audiosr.py` — post-install fixes for audiosr 0.0.7's bit-rot
  (torchaudio ≥ 2.11 loader, removed NumPy aliases; reasons in its docstring)
- `clean_vocal.py` — stdlib-only orchestrator the plugin invokes
