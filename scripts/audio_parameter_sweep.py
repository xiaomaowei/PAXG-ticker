"""Compare analysis windows on the offline calibration and evaluation cases."""
import argparse
import json
from pathlib import Path

import fishing_audio_templates as dsp

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("references", nargs=3, type=Path)
args = parser.parse_args()
clips = [dsp.read(path) for path in args.references]
edges = dsp.EDGES
for fft, hop, frames in [(512, 480, 16), (1024, 240, 31), (1024, 480, 12),
                         (1024, 480, 16), (1024, 480, 24), (2048, 480, 16)]:
    dsp.FFT, dsp.HOP, dsp.FRAMES = fft, hop, frames
    dsp.EDGES = [round(edge * fft / 1024) for edge in edges]
    dsp.REFRACTORY = round(0.6 * dsp.RATE / hop)
    templates, offsets = dsp.generate(clips)
    threshold, calibration = dsp.calibrate(clips, templates, offsets)
    report = dsp.evaluate(clips, templates, offsets, [], threshold)
    print(json.dumps(dict(fft=fft, hop=hop, frames=frames,
                          window_ms=1000 * (fft + (frames-1) * hop) / dsp.RATE,
                          threshold=threshold, TP=report["TP"], FP=report["FP"], FN=report["FN"],
                          butterflies_per_second=round(fft / 2 * (fft.bit_length()-1) * dsp.RATE / hop))))
