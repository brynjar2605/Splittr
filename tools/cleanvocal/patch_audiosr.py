#!/usr/bin/env python3
"""Bit-rot patches for an audiosr venv (issue #29).

audiosr 0.0.7 is abandoned upstream and its pinned dependencies have moved
underneath it. Three things break on a current install, all proven in the
2026-08 bake-off, so setup.sh applies exactly these after `pip install`:

1. torchaudio >= 2.11 delegates decoding to TorchCodec, which we don't want
   to install or depend on. audiosr only ever loads plain WAV files, so a
   tiny soundfile-based loader is dropped in and the two
   `torchaudio.load(filename)` call sites are pointed at it.
2. NumPy removed the `np.float` / `np.complex` aliases; audiosr's vendored
   audiomae code (pos_embed.py) still uses `np.float`.
3. audiosr pins librosa 0.9.2, whose util/utils.py also uses the removed
   aliases — its istft dies inside dtype_c2r without this.

Idempotent by construction: every patch checks for its own fingerprint
before writing, so setup.sh can run this unconditionally on every pass.

Usage: patch_audiosr.py <venv_dir>
"""

import re
import sys
from pathlib import Path

# Mirrors the shim proven in the bake-off venv: soundfile returns (n, ch)
# float64 by default, torchaudio returned (ch, n) float32, so transpose and
# force the dtype to keep every downstream shape assumption intact.
SF_SHIM = """
import soundfile as _sf, torch as _torch

def _sf_load(fn):
    d, sr = _sf.read(fn, dtype='float32', always_2d=True)
    return _torch.from_numpy(d.T), sr

"""

# The lookahead keeps np.float32/np.float64/np.float_/np.floating (and the
# complex equivalents) untouched — only the bare removed aliases are hit.
ALIAS_SUBS = [
    (re.compile(r"np\.float(?![0-9A-Za-z_])"), "float"),
    (re.compile(r"np\.complex(?![0-9A-Za-z_])"), "complex"),
]


def find_site_packages(venv: Path) -> Path:
    hits = sorted(venv.glob("lib/python*/site-packages"))
    if not hits:
        sys.exit(f"patch_audiosr: no site-packages under {venv}")
    return hits[0]


def insert_shim(path: Path) -> bool:
    """Add _sf_load after the import block and retarget torchaudio.load."""
    text = path.read_text()
    changed = False
    if "_sf_load" not in text:
        # After the last top-level import that precedes the first def/class:
        # the shim needs the module namespace but must exist before any
        # function body that references it runs.
        lines = text.split("\n")
        insert_at = 0
        for i, line in enumerate(lines):
            if line.startswith(("def ", "class ")):
                break
            if line.startswith(("import ", "from ")):
                insert_at = i + 1
        lines[insert_at:insert_at] = SF_SHIM.split("\n")
        text = "\n".join(lines)
        changed = True
    new = text.replace("torchaudio.load(filename)", "_sf_load(filename)")
    if new != text:
        text = new
        changed = True
    if changed:
        path.write_text(text)
    return changed


def sub_aliases(path: Path) -> bool:
    text = path.read_text()
    new = text
    for rx, repl in ALIAS_SUBS:
        new = rx.sub(repl, new)
    if new != text:
        path.write_text(new)
        return True
    return False


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit("usage: patch_audiosr.py <venv_dir>")
    sp = find_site_packages(Path(sys.argv[1]).resolve())

    audiosr = sp / "audiosr"
    librosa_utils = sp / "librosa" / "util" / "utils.py"
    for required in (audiosr, librosa_utils):
        if not required.exists():
            sys.exit(f"patch_audiosr: {required} missing - is audiosr installed?")

    for rel in ("utils.py", "utilities/audio/tools.py"):
        target = audiosr / rel
        state = "patched" if insert_shim(target) else "already patched"
        print(f"patch_audiosr: sf loader in audiosr/{rel}: {state}")

    hit = sum(sub_aliases(p) for p in sorted(audiosr.rglob("*.py")))
    print(f"patch_audiosr: np.float/np.complex aliases in audiosr: "
          f"{'%d file(s) patched' % hit if hit else 'already patched'}")

    state = "patched" if sub_aliases(librosa_utils) else "already patched"
    print(f"patch_audiosr: np.float/np.complex aliases in librosa/util/utils.py: {state}")


if __name__ == "__main__":
    main()
