"""Run splittr_cli --analyze over the ground-truth test set and score it.

    python3 tools/make_testdata.py testdata
    cmake --build build --target splittr_cli -j8
    python3 tools/bpm_validate.py [extra-audio-file ...]

Ground truth comes from the file name (<name>_<bpm*10>bpm.wav). A case passes
when the detected tempo is within +-2% of it; half/double-tempo answers are
reported separately but still count as failures. Extra files listed on the
command line are analysed without a pass/fail verdict (real music spot-checks).
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(ROOT, 'build', 'splittr_cli_artefacts', 'Release', 'splittr_cli')
TESTDATA = os.path.join(ROOT, 'testdata')
TOLERANCE = 0.02

if not os.path.exists(CLI):
    alt = os.path.join(ROOT, 'build', 'splittr_cli_artefacts', 'splittr_cli')
    CLI = alt if os.path.exists(alt) else CLI


def truth_from_name(name):
    m = re.search(r'_(\d+)bpm\.wav$', name)
    return int(m.group(1)) / 10.0 if m else None


def verdict(truth, got):
    if truth is None:
        return '', ''
    if got <= 0:
        return 'FAIL', 'no tempo'
    err = (got - truth) / truth
    if abs(err) <= TOLERANCE:
        return 'ok', f'{err * 100:+.2f}%'
    for ratio, label in ((2.0, 'double'), (0.5, 'half'), (1.5, '3/2'),
                         (2 / 3, '2/3'), (3.0, 'triple'), (1 / 3, 'third')):
        if abs(got - truth * ratio) / (truth * ratio) <= TOLERANCE:
            return 'FAIL', label
    return 'FAIL', f'{err * 100:+.1f}%'


def main():
    files = sorted(os.path.join(TESTDATA, f) for f in os.listdir(TESTDATA)
                   if f.endswith('.wav')) if os.path.isdir(TESTDATA) else []
    files += sys.argv[1:]

    if not files:
        print('No test files. Run tools/make_testdata.py first.')
        return 1

    out = subprocess.run([CLI, '--analyze'] + files,
                         capture_output=True, text=True)
    if out.returncode != 0 and not out.stdout:
        print(out.stderr)
        return 1

    print(f"{'file':<24}{'truth':>8}{'detected':>10}{'conf':>7}  {'':<6}{'note':<10}key")
    print('-' * 78)

    passed = total = 0
    for line in out.stdout.strip().splitlines():
        parts = line.split('\t')
        if len(parts) < 4:
            continue
        name, bpm, conf, key = parts[0], float(parts[1]), float(parts[2]), parts[3]
        truth = truth_from_name(name)
        mark, note = verdict(truth, bpm)
        if truth is not None:
            total += 1
            passed += mark == 'ok'
        print(f"{name:<24}{(f'{truth:.1f}' if truth else '-'):>8}{bpm:>10.2f}"
              f"{conf:>7.2f}  {mark:<6}{note:<10}{key}")

    print('-' * 78)
    print(f'{passed}/{total} within +-{TOLERANCE * 100:.0f}%')
    return 0 if passed == total else 1


if __name__ == '__main__':
    sys.exit(main())
