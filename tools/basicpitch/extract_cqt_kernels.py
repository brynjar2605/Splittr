#!/usr/bin/env python3
"""Extract the CQT front-end constants from Basic Pitch's features_model.onnx.

NeuralNote runs that graph through ONNX Runtime just to get the harmonically
stacked CQT that feeds the Basic Pitch CNN. The graph is pure DSP — nnAudio's
CQT2010v2 followed by a normalised log and a harmonic gather — with only four
real constant tensors in it, so Splittr reimplements the topology natively
(third_party/basicpitch/Features.cpp) and reads the constants from here.

This script writes third_party/basicpitch/weights/cqt_kernels.bin:

    magic   "SPCQT001"            8 bytes
    int32   numBins               36   (bins per octave)
    int32   kernelLength          256
    int32   numScaleBins          309  (bins kept after the octave concat)
    float32 kernelReal[36][256]        real CQT basis, top octave
    float32 kernelImag[36][256]        imaginary basis (the graph negates it)
    float32 lowpass[256]               octave-to-octave /2 decimation filter
    float32 binScale[309]              per-bin sqrt(window length) scaling

Run from the repo root with a venv that has `onnx` installed:

    python3 tools/basicpitch/extract_cqt_kernels.py
"""

import struct
import sys
from pathlib import Path

import numpy as np
import onnx
from onnx import numpy_helper

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "third_party" / "basicpitch" / "weights" / "features_model.onnx"
DST = ROOT / "third_party" / "basicpitch" / "weights" / "cqt_kernels.bin"

# Initialiser names inside the tf2onnx export.
REAL = "model_1/cqt2010v2_1/conv1d_15/ExpandDims_1:0"
IMAG = "model_1/cqt2010v2_1/conv1d_16/ExpandDims_1:0"
LOWPASS = "model_1/cqt2010v2_1/conv1d_17/ExpandDims_1:0"
SCALE = "model_1/cqt2010v2_1/Sqrt:0"


def main() -> int:
    model = onnx.load(str(SRC))
    init = {i.name: numpy_helper.to_array(i) for i in model.graph.initializer}

    real = init[REAL].reshape(init[REAL].shape[0], -1).astype(np.float32)
    imag = init[IMAG].reshape(init[IMAG].shape[0], -1).astype(np.float32)
    lowpass = init[LOWPASS].reshape(-1).astype(np.float32)
    scale = init[SCALE].reshape(-1).astype(np.float32)

    assert real.shape == imag.shape, (real.shape, imag.shape)
    num_bins, kernel_len = real.shape
    assert lowpass.shape == (kernel_len,), lowpass.shape

    with open(DST, "wb") as f:
        f.write(b"SPCQT001")
        f.write(struct.pack("<iii", num_bins, kernel_len, scale.shape[0]))
        f.write(real.tobytes())
        f.write(imag.tobytes())
        f.write(lowpass.tobytes())
        f.write(scale.tobytes())

    print(f"{DST.relative_to(ROOT)}: bins={num_bins} kernel={kernel_len} "
          f"scale={scale.shape[0]} bytes={DST.stat().st_size}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
