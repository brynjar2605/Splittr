#!/usr/bin/env python3
"""Clean-vocal pipeline CLI (issue #29). See tools/cleanvocal/PROTOCOL.md.

Runs under <root>/venv-sep/bin/python but deliberately imports only the
stdlib: every model and every piece of audio I/O lives in one of the two
venvs, and this script's job is orchestration — shell into the right venv,
keep the stdout protocol clean, clean up after itself. Keeping it
stdlib-only means the installed copy never breaks when a venv's packages
shift under it.

stdout carries only protocol lines (STAGE/PROGRESS/DONE/ERROR). Child
process output — which models fill with their own chatter, including lines
that could collide with protocol prefixes — is forwarded to stderr, which
the plugin logs but never parses.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from collections import deque
from pathlib import Path

DEFAULT_ROOT = Path.home() / "Library/Application Support/Splittr/cleanvocal"

# Bake-off winners (2026-08): each stage is one specific checkpoint, and the
# wanted output is identified by the stem marker audio-separator puts in the
# filename ("(reverb)"/"(noreverb)" etc. — the marker parens keep the match
# unambiguous against the discarded sibling stem).
SEP_STAGES = {
    "dereverb": ("dereverb_mel_band_roformer_anvuew_sdr_19.1729.ckpt", "(noreverb)"),
    "dereverb-echo": ("dereverb-echo_mel_band_roformer_sdr_13.4843_v2.ckpt", "(dry)"),
}

# Relative stage costs (45 s stereo: separator stages ~14 s, Apollo ~13 s,
# AudioSR ~100 s per channel) so PROGRESS tracks wall time, not stage count.
STAGE_WEIGHT = {"dereverb": 1.0, "dereverb-echo": 1.0, "restore-apollo": 1.0}
AUDIOSR_WEIGHT_PER_CHANNEL = 7.0

# --- helpers run inside venv-restore (soundfile/scipy live there, not here) ---

PROBE_SRC = """
import json, sys, soundfile as sf
i = sf.info(sys.argv[1])
print(json.dumps({"sr": i.samplerate, "ch": i.channels, "frames": i.frames}))
"""

SPLIT_SRC = """
import sys, soundfile as sf
inp, *outs = sys.argv[1:]
d, sr = sf.read(inp, dtype="float32", always_2d=True)
assert d.shape[1] == len(outs), f"expected {len(outs)} channels, file has {d.shape[1]}"
for i, o in enumerate(outs):
    sf.write(o, d[:, i], sr, subtype="FLOAT")
"""

# Final conform: whatever the last stage produced (AudioSR: one mono 48 kHz
# file per channel; the others: one file at the input rate) becomes exactly
# the input's sample rate, channel count and length, float32 — the
# contract's definition of "the same file, cleaned". Float32 and unclipped,
# like every WAV the plugin serves: restoration stages overshoot 0 dBFS,
# and this pass must not be the one place the chain hard-clips.
CONFORM_SRC = """
import sys
from fractions import Fraction
import numpy as np, soundfile as sf
from scipy.signal import resample_poly

out, sr, ch, frames = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
chans = []
for f in sys.argv[5:]:
    d, fsr = sf.read(f, dtype="float64", always_2d=True)
    for c in range(d.shape[1]):
        x = d[:, c]
        if fsr != sr:
            r = Fraction(sr, fsr)
            x = resample_poly(x, r.numerator, r.denominator)
        chans.append(x)
n = min(len(x) for x in chans)
y = np.stack([x[:n] for x in chans], axis=1)
if y.shape[1] != ch:
    assert y.shape[1] == 1 and ch == 2, f"cannot map {y.shape[1]} channels to {ch}"
    y = np.repeat(y, 2, axis=1)
y = y[:frames]
if y.shape[0] < frames:
    y = np.pad(y, ((0, frames - y.shape[0]), (0, 0)))
sf.write(out, y, sr, subtype="FLOAT")
"""


def emit(line):
    print(line, flush=True)


def fail(message):
    # One line, and the last line: collapse whatever a traceback or child
    # tail contains so the parser's single-line contract holds.
    emit("ERROR " + " ".join(str(message).split()))
    sys.exit(1)


class StageError(Exception):
    pass


def run_logged(cmd, cwd=None):
    """Run a child, streaming all of its output to stderr (never stdout)."""
    # Echo the command so the plugin's log shows what actually ran — seeds,
    # checkpoints and paths are otherwise invisible once this ships.
    sys.stderr.write("+ " + " ".join(str(c) for c in cmd) + "\n")
    sys.stderr.flush()
    tail = deque(maxlen=12)
    try:
        proc = subprocess.Popen(
            [str(c) for c in cmd], cwd=cwd, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except OSError as e:
        raise StageError(f"failed to start {cmd[0]}: {e}")
    assert proc.stdout is not None
    for line in proc.stdout:
        sys.stderr.write(line)
        sys.stderr.flush()
        if line.strip():
            tail.append(line.strip())
    if proc.wait() != 0:
        raise StageError(
            f"{Path(str(cmd[0])).name} exited {proc.returncode}: "
            + " | ".join(list(tail)[-3:]))


def run_captured(cmd):
    """Run a quick child and return its stdout (kept off our stdout)."""
    r = subprocess.run([str(c) for c in cmd], text=True, capture_output=True)
    if r.returncode != 0:
        raise StageError(
            f"{Path(str(cmd[0])).name} exited {r.returncode}: "
            + " ".join(r.stderr.split()[-40:]))
    return r.stdout


class Pipeline:
    def __init__(self, root, seed, tmp):
        self.root = root
        self.seed = seed
        self.tmp = tmp
        self.sep_bin = root / "venv-sep/bin/audio-separator"
        self.restore_py = root / "venv-restore/bin/python"
        self.audiosr_bin = root / "venv-restore/bin/audiosr"

    def probe(self, path):
        import json
        return json.loads(run_captured([self.restore_py, "-c", PROBE_SRC, path]))

    def run_separator(self, name, cur):
        model, marker = SEP_STAGES[name]
        outdir = self.tmp / f"out-{name}"
        outdir.mkdir()
        # --model_file_dir: the package default is /tmp, which vanishes on
        # reboot; <root>/models is the persistent home setup.sh pre-fills.
        run_logged([
            self.sep_bin, cur, "-m", model,
            "--model_file_dir", self.root / "models",
            "--output_dir", outdir, "--output_format", "WAV"])
        hits = sorted(p for p in outdir.iterdir() if marker in p.name)
        if len(hits) != 1:
            raise StageError(
                f"expected one '{marker}' stem from {model}, "
                f"got {[p.name for p in hits] or 'none'}")
        return hits[0]

    def run_apollo(self, cur):
        out = self.tmp / "apollo.wav"
        apollo = self.root / "apollo"
        # cwd matters: inference.py imports the repo-local look2hear package.
        # Chunking settings are the bake-off's proven fit for 24 GB + MPS.
        run_logged([
            self.restore_py, apollo / "inference.py", "--device=mps",
            "--checkpoint=" + str(apollo / "models/pytorch_model.bin"),
            "--in_wav=" + str(cur), "--out_wav=" + str(out),
            "--chunk-seconds=6", "--overlap-seconds=1",
            "--chunk-batch-size=2"], cwd=apollo)
        if not out.exists():
            raise StageError("apollo produced no output file")
        return out

    def run_audiosr_channel(self, chfile, outdir):
        outdir.mkdir()
        # Through the interpreter, not the launcher itself: audiosr installs
        # a legacy script whose rewritten shebang is the raw interpreter
        # path, and the default root lives under "Application Support" —
        # the kernel truncates shebangs at the first space.
        run_logged([
            self.restore_py, self.audiosr_bin, "-i", chfile, "-s", outdir, "-d", "mps",
            "--model_name", "basic", "--ddim_steps", "50",
            "--seed", str(self.seed), "--suffix", "_audiosr"])
        # Output lands in a timestamped subdirectory of -s.
        hits = sorted(outdir.rglob("*.wav"))
        if len(hits) != 1:
            raise StageError(
                f"expected one audiosr output, got {[p.name for p in hits] or 'none'}")
        return hits[0]

    def run_audiosr(self, cur, channels, progress_between):
        """One diffusion pass per channel, same seed, strictly sequential.

        AudioSR is mono and memory-heavy enough to hit swap on this machine,
        so channels never run in parallel. Returns the per-channel mono
        48 kHz files; the conform step recombines them in one float pass,
        so nothing on the way to the output is ever quantised.
        """
        chfiles = [self.tmp / f"audiosr-ch{i}.wav" for i in range(channels)]
        run_captured([self.restore_py, "-c", SPLIT_SRC, cur, *chfiles])
        outs = []
        for i, chfile in enumerate(chfiles):
            outs.append(self.run_audiosr_channel(chfile, self.tmp / f"audiosr-out{i}"))
            if i + 1 < channels:
                progress_between(i + 1)
        return outs

    def conform(self, parts, out, sr, ch, frames):
        run_captured([self.restore_py, "-c", CONFORM_SRC,
                      out, sr, ch, frames, *parts])


def parse_args(argv):
    class ProtocolParser(argparse.ArgumentParser):
        # argparse's own error path prints usage and exits without an ERROR
        # line; the plugin only understands the protocol, so route through it.
        def error(self, message):
            fail(f"bad arguments: {message}")

    p = ProtocolParser(prog="clean_vocal.py", add_help=True)
    p.add_argument("--input", required=True)
    p.add_argument("--output", required=True)
    p.add_argument("--dereverb", action="store_true")
    p.add_argument("--dereverb-echo", action="store_true")
    p.add_argument("--restore", choices=["none", "apollo", "audiosr"],
                   default="none")
    p.add_argument("--root")
    p.add_argument("--seed", type=int, default=42)
    return p.parse_args(argv)


def resolve_root(arg_root):
    if arg_root:
        return Path(arg_root).expanduser()
    env = os.environ.get("SPLITTR_CLEANVOCAL_ROOT")
    if env:
        return Path(env).expanduser()
    # The installed copy lives in the root itself, so a relocated root still
    # works without flags; the repo copy falls through to the default.
    here = Path(__file__).resolve().parent
    if (here / "venv-sep").is_dir():
        return here
    return DEFAULT_ROOT


def main(argv):
    args = parse_args(argv)

    root = resolve_root(args.root)
    for needed in ("venv-sep/bin/audio-separator", "venv-restore/bin/python"):
        if not (root / needed).exists():
            fail(f"runtime root not set up: {root} is missing {needed} "
                 f"(run tools/cleanvocal/setup.sh)")

    inp = Path(args.input).expanduser().resolve()
    if not inp.is_file():
        fail(f"input not found: {inp}")
    out = Path(args.output).expanduser().resolve()

    stages = []
    if args.dereverb:
        stages.append("dereverb")
    if args.dereverb_echo:
        stages.append("dereverb-echo")
    if args.restore != "none":
        stages.append(f"restore-{args.restore}")
    if not stages:
        fail("no stages requested: pass --dereverb, --dereverb-echo "
             "and/or --restore apollo|audiosr")

    tmp = Path(tempfile.mkdtemp(prefix="cleanvocal-"))
    try:
        pipe = Pipeline(root, args.seed, tmp)
        info = pipe.probe(inp)
        sr, ch, frames = info["sr"], info["ch"], info["frames"]

        # Weighted boundaries scaled to 98 leave headroom for the conform
        # pass, so 100 only appears once the output file actually exists.
        weights = [STAGE_WEIGHT.get(s, AUDIOSR_WEIGHT_PER_CHANNEL * ch)
                   for s in stages]
        total = sum(weights)
        done_weight = 0.0
        emit("PROGRESS 0")

        parts = [inp]  # channel-ordered file list feeding the next stage
        for i, stage in enumerate(stages, start=1):
            emit(f"STAGE {stage} {i} {len(stages)}")
            if stage in SEP_STAGES:
                parts = [pipe.run_separator(stage, parts[0])]
            elif stage == "restore-apollo":
                parts = [pipe.run_apollo(parts[0])]
            else:  # restore-audiosr

                def between(channels_done, base=done_weight, w=weights[i - 1]):
                    frac = (base + w * channels_done / ch) / total
                    emit(f"PROGRESS {int(round(frac * 98))}")

                parts = pipe.run_audiosr(parts[0], ch, between)
            done_weight += weights[i - 1]
            emit(f"PROGRESS {int(round(done_weight / total * 98))}")

        out.parent.mkdir(parents=True, exist_ok=True)
        conformed = tmp / "final.wav"
        pipe.conform(parts, conformed, sr, ch, frames)
        # copy+replace rather than move: the output may cross filesystems,
        # and a half-written file must never sit at the destination path.
        part = str(out) + ".part"
        shutil.copyfile(conformed, part)
        os.replace(part, out)
        emit("PROGRESS 100")
        emit(f"DONE {out}")
    except StageError as e:
        fail(e)
    except Exception as e:  # noqa: BLE001 - anything else still owes ERROR
        fail(f"{type(e).__name__}: {e}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main(sys.argv[1:])
