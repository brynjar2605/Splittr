#!/bin/bash
# Builds the clean-vocal runtime root (issue #29) — see PROTOCOL.md for the
# layout contract. Everything here codifies the 2026-08 bake-off recipe that
# actually worked on this machine class (Apple Silicon, Homebrew python3.11);
# versions and the Apollo commit are pinned so a rerun months from now builds
# the same root instead of whatever upstream has drifted to.
#
# Idempotent: each step checks for the artifact it produces and skips itself,
# so a second run is a fast no-op and a half-finished run resumes cleanly.

set -euo pipefail

PY311="/opt/homebrew/bin/python3.11"
ROOT="${SPLITTR_CLEANVOCAL_ROOT:-$HOME/Library/Application Support/Splittr/cleanvocal}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Pinned 2026-08-21: the exact Apollo tree the bake-off validated. The repo
# has no tags, so a commit hash is the only stable reference.
APOLLO_COMMIT="b17bf536b11fa527cddce5dd6694c4b04a44d3f6"

# The two audio-separator checkpoints the pipeline shells out to. Fetched at
# setup time because the alternative — a model download in the middle of the
# user's first render — looks like a hang.
SEP_MODELS=(
    "dereverb_mel_band_roformer_anvuew_sdr_19.1729.ckpt"
    "dereverb-echo_mel_band_roformer_sdr_13.4843_v2.ckpt"
)

say() { printf '== %s\n' "$*"; }

if [ ! -x "$PY311" ]; then
    echo "error: $PY311 not found." >&2
    echo "The clean-vocal venvs need Homebrew Python 3.11 (audiosr's pins" >&2
    echo "predate 3.12). Install it with: brew install python@3.11" >&2
    exit 1
fi

say "runtime root: $ROOT"
mkdir -p "$ROOT" "$ROOT/models"

# --- venv-sep: audio-separator (de-reverb / de-reverb+echo stages) ---------
# The console script only exists once pip finished, so its presence is the
# "this venv is complete" marker.
if [ -x "$ROOT/venv-sep/bin/audio-separator" ]; then
    say "venv-sep: already built"
else
    say "venv-sep: building (audio-separator 0.44.5)"
    "$PY311" -m venv "$ROOT/venv-sep"
    "$ROOT/venv-sep/bin/pip" install --quiet --upgrade pip
    "$ROOT/venv-sep/bin/pip" install --quiet "audio-separator[cpu]==0.44.5"
fi

# --- venv-restore: audiosr + Apollo's runtime (restore stages) -------------
# audiosr's requirements are incomplete and partly wrong: it forgets
# matplotlib, and its pinned librosa 0.9.2 needs pkg_resources, which
# setuptools 81 removed — hence the second install and the <81 cap.
#
# The skip marker is written only once BOTH installs have landed: the audiosr
# console script appears after the first, so testing it (as this block once
# did) let an interruption between the installs resume as "already built"
# while the venv stayed unimportable.
if [ -f "$ROOT/venv-restore/.complete" ]; then
    say "venv-restore: already built"
else
    if [ ! -x "$ROOT/venv-restore/bin/audiosr" ]; then
        say "venv-restore: building (audiosr 0.0.7 + deps it forgot to declare)"
        "$PY311" -m venv "$ROOT/venv-restore"
        "$ROOT/venv-restore/bin/pip" install --quiet --upgrade pip
        "$ROOT/venv-restore/bin/pip" install --quiet "audiosr==0.0.7"
    fi

    # A root built by the pre-marker script lands here whole but unmarked:
    # when the forgotten deps already import, it just gains the marker.
    # Anything else re-runs the dep install, which is fast and idempotent.
    if "$ROOT/venv-restore/bin/python" -c \
        "import omegaconf, soundfile, matplotlib, pkg_resources" 2>/dev/null; then
        say "venv-restore: forgotten deps present"
    else
        say "venv-restore: installing the deps audiosr forgot"
        "$ROOT/venv-restore/bin/pip" install --quiet "setuptools<81" omegaconf soundfile matplotlib
    fi

    touch "$ROOT/venv-restore/.complete"
fi

# Always run the bit-rot patches: they are idempotent and sub-second, and
# running unconditionally means a venv restored from backup gets fixed too.
say "venv-restore: applying audiosr bit-rot patches"
"$ROOT/venv-restore/bin/python" "$SCRIPT_DIR/patch_audiosr.py" "$ROOT/venv-restore"

# --- Apollo: repo at the pinned commit + its checkpoint --------------------
if [ -d "$ROOT/apollo/.git" ]; then
    say "apollo: repo present"
else
    say "apollo: cloning JusperLee/Apollo"
    git clone --quiet https://github.com/JusperLee/Apollo.git "$ROOT/apollo"
fi
if [ "$(git -C "$ROOT/apollo" rev-parse HEAD)" != "$APOLLO_COMMIT" ]; then
    say "apollo: pinning $APOLLO_COMMIT"
    git -C "$ROOT/apollo" fetch --quiet origin
    git -C "$ROOT/apollo" checkout --quiet "$APOLLO_COMMIT"
else
    say "apollo: already at pinned commit"
fi
# No third venv: Apollo runs fine on venv-restore's torch despite the 2.11
# pin in its requirements-macos-arm64.txt (proven in the bake-off).

if [ -f "$ROOT/apollo/models/pytorch_model.bin" ]; then
    say "apollo: checkpoint present"
else
    say "apollo: fetching checkpoint (JusperLee/Apollo pytorch_model.bin)"
    mkdir -p "$ROOT/apollo/models"
    "$ROOT/venv-restore/bin/python" - "$ROOT/apollo/models/pytorch_model.bin" <<'PY'
import shutil, sys
from huggingface_hub import hf_hub_download
cached = hf_hub_download(repo_id="JusperLee/Apollo", filename="pytorch_model.bin")
shutil.copyfile(cached, sys.argv[1])
PY
fi

# --- audio-separator checkpoints into the persistent model dir -------------
# (its built-in default is /tmp, which vanishes on reboot)
for model in "${SEP_MODELS[@]}"; do
    if [ -f "$ROOT/models/$model" ]; then
        say "models: $model present"
    else
        say "models: downloading $model"
        "$ROOT/venv-sep/bin/audio-separator" --download_model_only \
            -m "$model" --model_file_dir "$ROOT/models" >/dev/null
    fi
done

# --- install the pipeline CLI ----------------------------------------------
if cmp -s "$SCRIPT_DIR/clean_vocal.py" "$ROOT/clean_vocal.py" 2>/dev/null; then
    say "clean_vocal.py: up to date"
else
    say "clean_vocal.py: installing"
    cp "$SCRIPT_DIR/clean_vocal.py" "$ROOT/clean_vocal.py"
fi

say "ready: $ROOT"
