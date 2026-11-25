import argparse
import json
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.signal import stft


BAND_EDGES = [0, 250, 500, 1000, 2000, 4000, 8000, 12000, 24000]


def analyze(path, output):
    pcm, rate = sf.read(path, dtype="float32", always_2d=True)
    mono = pcm.mean(axis=1)
    fft_hz = np.fft.rfftfreq(mono.size, 1 / rate)
    fft_power = np.abs(np.fft.rfft(mono * np.hanning(mono.size))) ** 2
    hz, times, spectrum = stft(mono, rate, window="hann", nperseg=1024,
                               noverlap=768, boundary=None, padded=False)
    power = np.abs(spectrum) ** 2
    mean_power = power.mean(axis=1)
    centroid = np.average(hz, weights=mean_power)
    energy = np.cumsum(mono.astype(np.float64) ** 2)
    percentile_ms = {
        str(p): 1000 * int(np.searchsorted(energy, energy[-1] * p / 100)) / rate
        for p in (1, 5, 50, 90, 95)
    }
    np.savez_compressed(output / (path.stem + ".npz"), pcm=pcm, rate=rate,
                        fft_hz=fft_hz, fft_power=fft_power,
                        stft_hz=hz, stft_times=times, stft_power=power)
    return {
        "file": path.name,
        "sample_rate_hz": rate,
        "channels": pcm.shape[1],
        "duration_ms": 1000 * len(pcm) / rate,
        "channel_peak": np.abs(pcm).max(axis=0).tolist(),
        "channel_rms": np.sqrt(np.mean(pcm ** 2, axis=0)).tolist(),
        "mono_peak": float(np.abs(mono).max()),
        "mono_rms": float(np.sqrt(np.mean(mono ** 2))),
        "peak_time_ms": 1000 * int(np.argmax(np.abs(mono))) / rate,
        "strongest_stft_frame_center_ms": 1000 * float(times[np.argmax(power.sum(axis=0))]),
        "energy_percentile_ms": percentile_ms,
        "fft_peak_hz": float(fft_hz[np.argmax(fft_power)]),
        "spectral_centroid_hz": float(centroid),
        "spectral_bandwidth_hz": float(np.sqrt(np.average((hz - centroid) ** 2, weights=mean_power))),
        "band_energy_ratio": {
            f"{low}-{high}": float(mean_power[(hz >= low) & (hz < high)].sum() / mean_power.sum())
            for low, high in zip(BAND_EDGES, BAND_EDGES[1:])
        },
    }


def main():
    parser = argparse.ArgumentParser(description="Analyze three fishing reference audio clips.")
    parser.add_argument("references", nargs=3, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = [analyze(path, args.output) for path in args.references]
    (args.output / "analysis.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
