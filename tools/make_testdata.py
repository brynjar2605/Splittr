"""Generate ground-truth test audio for the BPM detector (issue #15).

Synthetic drum-machine patterns at exactly known tempos, written as 16-bit
stereo WAV into testdata/ (git-ignored). Pure stdlib (wave + struct + math +
random), same spirit as tools/make_icon.py.

    python3 tools/make_testdata.py [out-dir]

Every file name encodes its ground truth: <name>_<bpm*10>bpm.wav, e.g.
basic_1280bpm.wav is 128.0 BPM. tools/bpm_validate.sh parses that back out.
"""
import math
import os
import random
import struct
import sys
import wave

SR = 44100
DURATION = 32.0          # seconds per file
random.seed(20250815)    # reproducible noise


# --- tiny signal helpers ---------------------------------------------------

def one_pole_lp(sig, cutoff):
    a = math.exp(-2.0 * math.pi * cutoff / SR)
    out, z = [], 0.0
    for s in sig:
        z = (1.0 - a) * s + a * z
        out.append(z)
    return out


def one_pole_hp(sig, cutoff):
    lp = one_pole_lp(sig, cutoff)
    return [s - l for s, l in zip(sig, lp)]


def noise(n):
    return [random.uniform(-1.0, 1.0) for _ in range(n)]


def env_exp(n, decay, attack=0.002):
    """Exponential decay with a short linear attack (click-free)."""
    a = max(1, int(attack * SR))
    out = []
    for i in range(n):
        e = math.exp(-i / (decay * SR))
        if i < a:
            e *= i / a
        out.append(e)
    return out


# --- one-shot drum voices --------------------------------------------------

def make_kick(gain=1.0):
    n = int(0.45 * SR)
    e = env_exp(n, 0.11, 0.001)
    out, phase = [], 0.0
    for i in range(n):
        t = i / SR
        f = 45.0 + 95.0 * math.exp(-t / 0.020)     # pitch drop 140 -> 45 Hz
        phase += 2.0 * math.pi * f / SR
        click = math.exp(-t / 0.0015) * 0.35
        out.append((math.sin(phase) + click) * e[i] * gain)
    return out


def make_snare(gain=1.0, decay=0.13):
    n = int(0.35 * SR)
    body = one_pole_hp(noise(n), 900.0)
    body = one_pole_lp(body, 7500.0)
    e = env_exp(n, decay, 0.0008)
    tone = env_exp(n, decay * 0.45, 0.0008)
    out = []
    for i in range(n):
        t = i / SR
        drum = 0.5 * math.sin(2.0 * math.pi * 190.0 * t) * tone[i]
        out.append((0.85 * body[i] + drum) * e[i] * gain)
    return out


def make_hat(gain=1.0, decay=0.028):
    n = int(max(0.06, decay * 6) * SR)
    h = one_pole_hp(noise(n), 6500.0)
    e = env_exp(n, decay, 0.0004)
    return [h[i] * e[i] * gain for i in range(n)]


KICK = make_kick(0.95)
SNARE = make_snare(0.75)
HAT_C = make_hat(0.22, 0.026)
HAT_O = make_hat(0.18, 0.13)
GHOST = make_snare(0.16, 0.05)
RIDE = make_hat(0.10, 0.045)


# --- mixing ----------------------------------------------------------------

def mix(buf, voice, at_sec, gain=1.0):
    start = int(round(at_sec * SR))
    if start < 0:
        return
    end = min(len(buf), start + len(voice))
    for i in range(start, end):
        buf[i] += voice[i - start] * gain


def sine_note(buf, freq, at_sec, dur, gain, decay=None):
    start = int(round(at_sec * SR))
    n = int(dur * SR)
    if start >= len(buf):
        return
    atk = int(0.006 * SR)
    rel = int(0.03 * SR)
    for i in range(n):
        j = start + i
        if j >= len(buf):
            break
        e = 1.0
        if i < atk:
            e = i / atk
        elif i > n - rel:
            e = max(0.0, (n - i) / rel)
        if decay:
            e *= math.exp(-i / (decay * SR))
        buf[j] += math.sin(2.0 * math.pi * freq * i / SR) * e * gain


def write_wav(path, buf):
    peak = max(1e-9, max(abs(s) for s in buf))
    scale = 0.89 / peak
    frames = bytearray()
    for s in buf:
        v = int(max(-1.0, min(1.0, s * scale)) * 32767.0)
        frames += struct.pack('<hh', v, v)
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(bytes(frames))
    print(f"  {os.path.basename(path)}  ({len(buf) / SR:.1f}s)")


# --- patterns --------------------------------------------------------------
# All patterns are 4/4; `beat` is the quarter-note period, so the ground-truth
# tempo is always the quarter-note rate. Positions are in beats within a bar.

def pattern_basic(buf, bpm):
    """Backbeat: kick 1+3, snare 2+4, closed hats on eighths."""
    beat = 60.0 / bpm
    for bar in range(int(DURATION / (beat * 4)) + 1):
        t0 = bar * 4 * beat
        for b in (0.0, 2.0):
            mix(buf, KICK, t0 + b * beat)
        for b in (1.0, 3.0):
            mix(buf, SNARE, t0 + b * beat)
        for i in range(8):
            mix(buf, HAT_C, t0 + i * 0.5 * beat, 1.0 if i % 2 == 0 else 0.7)


def pattern_swing(buf, bpm):
    """Swung offbeat hats (2:1 triplet feel) + shuffled kick."""
    beat = 60.0 / bpm
    for bar in range(int(DURATION / (beat * 4)) + 1):
        t0 = bar * 4 * beat
        for b in (0.0, 2.0, 2.75):
            mix(buf, KICK, t0 + b * beat)
        for b in (1.0, 3.0):
            mix(buf, SNARE, t0 + b * beat)
        for i in range(4):
            mix(buf, HAT_C, t0 + i * beat, 1.0)
            mix(buf, HAT_C, t0 + (i + 2.0 / 3.0) * beat, 0.65)   # swung "and"


def pattern_sparse(buf, bpm):
    """Kick + snare only, no hats — nothing between the quarters."""
    beat = 60.0 / bpm
    for bar in range(int(DURATION / (beat * 4)) + 1):
        t0 = bar * 4 * beat
        for b in (0.0, 2.0):
            mix(buf, KICK, t0 + b * beat)
        for b in (1.0, 3.0):
            mix(buf, SNARE, t0 + b * beat)


def pattern_busy(buf, bpm):
    """16th hats, ghost snares, syncopated kicks, open hat on the 'and' of 4."""
    beat = 60.0 / bpm
    for bar in range(int(DURATION / (beat * 4)) + 1):
        t0 = bar * 4 * beat
        for b in (0.0, 0.75, 2.0, 2.5, 3.25):
            mix(buf, KICK, t0 + b * beat, 0.95 if b in (0.0, 2.0) else 0.7)
        for b in (1.0, 3.0):
            mix(buf, SNARE, t0 + b * beat)
        for b in (1.75, 3.5):
            mix(buf, GHOST, t0 + b * beat)
        for i in range(16):
            if abs(i * 0.25 - 3.5) < 1e-6:
                mix(buf, HAT_O, t0 + i * 0.25 * beat, 0.9)
            else:
                mix(buf, HAT_C, t0 + i * 0.25 * beat, 1.0 if i % 4 == 0 else 0.55)


def pattern_ride(buf, bpm):
    """Softer kit: ride on eighths, brushed snare — weak percussive transients."""
    beat = 60.0 / bpm
    for bar in range(int(DURATION / (beat * 4)) + 1):
        t0 = bar * 4 * beat
        for b in (0.0, 2.5):
            mix(buf, KICK, t0 + b * beat, 0.7)
        for b in (1.0, 3.0):
            mix(buf, GHOST, t0 + b * beat, 1.6)
        for i in range(8):
            mix(buf, RIDE, t0 + i * 0.5 * beat, 1.0 if i % 2 == 0 else 0.6)


BASS_SCALE = [55.00, 61.74, 65.41, 73.42, 82.41, 87.31, 98.00]   # A minor-ish


def pattern_bassline(buf, bpm):
    """Full kit + a melodic sine bassline and sustained pad over the top."""
    pattern_basic(buf, bpm)
    beat = 60.0 / bpm
    steps = [0, 0, 2, 0, 4, 4, 2, 3]
    for bar in range(int(DURATION / (beat * 4)) + 1):
        t0 = bar * 4 * beat
        for i, s in enumerate(steps):
            sine_note(buf, BASS_SCALE[s % len(BASS_SCALE)],
                      t0 + i * 0.5 * beat, 0.5 * beat * 0.9, 0.30, decay=0.35)
        # slow pad: one chord per bar, no percussive attack
        root = BASS_SCALE[[0, 4, 2, 3][bar % 4]] * 4.0
        for mult in (1.0, 1.5, 2.0):
            sine_note(buf, root * mult, t0, 4 * beat, 0.055)


def pattern_human(buf, bpm):
    """Backbeat played by hand: timing jitter and velocity variation."""
    beat = 60.0 / bpm
    jitter = lambda: random.gauss(0.0, 0.008)          # ~8 ms, like a drummer
    vel = lambda: 0.75 + 0.45 * random.random()
    for bar in range(int(DURATION / (beat * 4)) + 1):
        t0 = bar * 4 * beat
        for b in (0.0, 2.0):
            mix(buf, KICK, t0 + b * beat + jitter(), vel())
        for b in (1.0, 3.0):
            mix(buf, SNARE, t0 + b * beat + jitter(), vel())
        for i in range(8):
            mix(buf, HAT_C, t0 + i * 0.5 * beat + jitter(),
                vel() * (1.0 if i % 2 == 0 else 0.7))


def pattern_drift(buf, bpm):
    """Backbeat whose tempo breathes +-2% — no click track."""
    beats, t, k = [], 0.0, 0
    while t < DURATION + 4.0:
        beats.append(t)
        t += 60.0 / (bpm * (1.0 + 0.02 * math.sin(2.0 * math.pi * t / 11.0)))
        k += 1

    def at(x):                                          # fractional beat index
        i = int(x)
        if i + 1 >= len(beats):
            return DURATION + 1.0
        return beats[i] + (x - i) * (beats[i + 1] - beats[i])

    for bar in range(len(beats) // 4):
        b0 = bar * 4
        for b in (0, 2):
            mix(buf, KICK, at(b0 + b))
        for b in (1, 3):
            mix(buf, SNARE, at(b0 + b))
        for i in range(8):
            mix(buf, HAT_C, at(b0 + i * 0.5), 1.0 if i % 2 == 0 else 0.7)


def pattern_rubato(buf, _bpm):
    """Free-time chords, no pulse at all — must come back with no tempo."""
    t = 0.0
    while t < DURATION:
        root = BASS_SCALE[random.randrange(len(BASS_SCALE))] * 2.0
        dur = random.uniform(0.55, 1.9)
        for mult in (1.0, 1.25, 1.5, 2.0):
            sine_note(buf, root * mult, t, dur * 1.1, 0.16, decay=1.2)
        t += dur


def pattern_noise(buf, _bpm):
    """Slow filtered-noise swells — energy, no rhythm."""
    n = len(buf)
    src = one_pole_lp(one_pole_hp(noise(n), 200.0), 4000.0)
    t = 0.0
    while t < DURATION:
        start = int(t * SR)
        length = int(random.uniform(1.5, 3.5) * SR)
        for i in range(length):
            j = start + i
            if j >= n:
                break
            e = math.sin(math.pi * i / length) ** 2
            buf[j] += src[j] * e * 0.6
        t += random.uniform(1.0, 2.5)


PATTERNS = {
    'basic': pattern_basic,
    'swing': pattern_swing,
    'sparse': pattern_sparse,
    'busy': pattern_busy,
    'ride': pattern_ride,
    'bassline': pattern_bassline,
    'human': pattern_human,
    'drift': pattern_drift,
    'rubato': pattern_rubato,
    'noise': pattern_noise,
}

CASES = [
    ('basic', 70.0), ('basic', 85.0), ('basic', 100.0), ('basic', 120.0),
    ('basic', 128.0), ('basic', 140.0), ('basic', 174.0),
    ('swing', 95.0), ('swing', 128.0),
    ('sparse', 90.0), ('sparse', 132.0),
    ('busy', 150.0), ('busy', 105.0),
    ('ride', 82.0),
    ('bassline', 112.0), ('bassline', 160.0),
    ('human', 92.0), ('drift', 118.0),
    ('rubato', 0.0), ('noise', 0.0),   # 0 = must report no tempo at all
]


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else 'testdata'
    os.makedirs(out_dir, exist_ok=True)
    print(f"Writing {len(CASES)} test files to {out_dir}/")

    for name, bpm in CASES:
        buf = [0.0] * int(DURATION * SR)
        PATTERNS[name](buf, bpm)
        path = os.path.join(out_dir, f"{name}_{int(round(bpm * 10))}bpm.wav")
        write_wav(path, buf)


if __name__ == '__main__':
    main()
