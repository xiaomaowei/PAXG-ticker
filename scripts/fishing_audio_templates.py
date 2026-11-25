"""Generate C++ spectral templates and evaluate deterministic synthetic mixtures."""
import argparse
import json
import math
from pathlib import Path
from time import process_time
import tracemalloc

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

RATE = 48000
FFT = 1024
HOP = 480
FRAMES = 16
EDGES = [5, 8, 11, 14, 17, 20, 24, 29, 35, 42, 50, 60, 72, 86, 103, 123, 147, 176, 211]
REFRACTORY = 60  # hops


def resample(samples, src, dst):
    divisor = math.gcd(src, dst)
    return resample_poly(samples, dst // divisor, src // divisor)


def extract(pcm):
    windows = np.lib.stride_tricks.sliding_window_view(pcm, FFT)[::HOP]
    power = np.abs(np.fft.rfft(windows * np.hanning(FFT))) ** 2
    bands = np.sqrt(np.add.reduceat(power[:, :EDGES[-1]], EDGES[:-1], axis=1))
    norms = np.linalg.norm(bands, axis=1, keepdims=True)
    return bands / np.where(norms > 0, norms, 1), np.mean(windows ** 2, axis=1)


def generate(clips):
    templates, offsets = [], []
    for pcm in clips:
        features, energy = extract(pcm)
        start = np.argmax(np.convolve(energy, np.ones(FRAMES), mode="valid"))
        # Center each vector to discount broadband spectral overlap.
        template = features[start:start + FRAMES]
        template = template - template.mean(axis=1, keepdims=True)
        templates.append(template / np.linalg.norm(template))
        offsets.append(int(start * HOP))
    return np.array(templates), offsets


def detect(pcm, templates, threshold):
    if pcm.size < FFT + (FRAMES - 1) * HOP:
        return []
    features, energy = extract(pcm)
    features -= features.mean(axis=1, keepdims=True)
    windows = np.lib.stride_tricks.sliding_window_view(features, FRAMES, axis=0).transpose(0, 2, 1)
    norm = np.sqrt(np.sum(windows ** 2, axis=(1, 2)))
    scores = np.einsum("tfb,nfb->nt", templates, windows) / np.where(norm > 0, norm, 1)[:, None]
    events, last = [], -REFRACTORY
    for index, row in enumerate(scores):
        if index - last >= REFRACTORY and row.max() >= threshold and energy[index:index + FRAMES].max() > 1e-8:
            events.append((int(index * HOP + (FRAMES - 1) * HOP + FFT), int(row.argmax()) + 1))
            last = index
    return events


def evaluate(clips, templates, offsets, backgrounds, threshold):
    rng = np.random.default_rng(928)
    t = np.arange(RATE * 4) / RATE
    background = 0.012 * np.sin(2 * np.pi * 330 * t) + 0.01 * np.sin(2 * np.pi * 510 * t)
    background *= 0.6 + 0.4 * np.sin(2 * np.pi * 3 * t)
    cases = [("silence", np.zeros(RATE * 2), []),
             ("white_noise", rng.normal(0, 0.02, RATE * 2), []),
             ("synthetic_background", background, []),
             ("non_target_sweep", 0.15 * np.sin(2 * np.pi * (200 * t + 300 * t*t)), [])]
    for index, clip in enumerate(clips, 1):
        for label, gain, rate, pad, noise in [
            ("source_self_check", 1, RATE, 0, 0), ("quiet", 0.1, RATE, 1727, 0),
            ("loud", 1.4, RATE, 7019, 0), ("rate_16k", 0.8, 16000, 2183, 0),
            ("rate_44k1", 0.5, 44100, 8354, 0), ("white_mix", 0.5, RATE, 6231, 0.008),
            ("background_mix", 0.7, RATE, 3777, 0),
        ]:
            signal = resample(resample(clip, RATE, rate), rate, RATE) if rate != RATE else clip
            signal = np.pad(signal * gain, (pad, RATE // 4))
            if noise:
                signal += rng.normal(0, noise, signal.size)
            if label == "background_mix":
                signal += np.resize(background, signal.size)
            cases.append((label, np.clip(signal, -1, 1), [(pad, index)]))
        # Excludes the selected signature by ending half-way into its analysis window.
        fragment_end = offsets[index - 1] + FRAMES * HOP // 2
        cases.append(("incomplete_clip_rejection", np.pad(clip[:fragment_end], (827, RATE // 2)), []))
    continuous = np.concatenate(clips)
    starts = np.cumsum([0] + [x.size for x in clips[:-1]])
    cases.append(("continuous", continuous, [(int(s), i + 1) for i, s in enumerate(starts)]))
    for background in backgrounds:
        cases.append(("provided_negative", background, []))
        for i, clip in enumerate(clips, 1):
            cases.append(("provided_background_mix", np.clip(clip + np.resize(background, clip.size), -1, 1), [(0, i)]))

    rows = []
    for name, pcm, expected in cases:
        tracemalloc.start()
        start = process_time()
        events = detect(pcm, templates, threshold)
        elapsed = process_time() - start
        _, peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        pending = expected.copy()
        delays, fp = [], 0
        for at, template in events:
            labels = [(onset, i) for onset, i in pending
                      if i == template and onset <= at <= onset + offsets[i - 1] + FFT + (FRAMES + 12) * HOP]
            if labels:
                pending.remove(labels[0])
                delays.append(1000 * (at - labels[0][0]) / RATE)
            else:
                fp += 1
        rows.append(dict(case=name, TP=len(delays), FP=fp, FN=len(pending), latency_ms=delays,
                         expected=expected, events=events, cpu_ms=1000 * elapsed, peak_python_bytes=peak))
    scored = [r for r in rows if r["case"] != "source_self_check"]
    tp, fp, fn = (sum(r[key] for r in scored) for key in ("TP", "FP", "FN"))
    return dict(TP=tp, FP=fp, FN=fn, precision=tp / max(tp + fp, 1),
                recall=tp / max(tp + fn, 1), cases=rows)


def calibrate(clips, templates, offsets):
    rng = np.random.default_rng(42)
    t = np.arange(RATE * 2) / RATE
    negatives = [rng.normal(0, 0.035, t.size), 0.08 * np.sin(2 * np.pi * 720 * t)]
    for i, clip in enumerate(clips):
        negatives.append(np.pad(clip[:offsets[i] + 3 * HOP], (133, RATE // 2)))
    positives = [(np.pad(clip * gain, (pad, 0)), i + 1)
                 for i, clip in enumerate(clips)
                 for pad, gain in [(101, 0.28), (247, 0.64), (365, 0.19)]]
    candidates = []
    for threshold in (0.70, 0.74, 0.78, 0.82, 0.86, 0.90):
        fp = sum(len(detect(pcm, templates, threshold)) for pcm in negatives)
        tp = 0
        for pcm, expected in positives:
            events = detect(pcm, templates, threshold)
            matches = any(index == expected for _, index in events)
            tp += matches
            fp += len(events) - matches
        candidates.append(dict(threshold=threshold, TP=tp, FP=fp, FN=len(positives) - tp))
    best = min(candidates, key=lambda row: (row["FP"], row["FN"], -row["threshold"]))
    return best["threshold"], candidates


def read(path):
    pcm, rate = sf.read(path, always_2d=True)
    return resample(pcm.mean(axis=1), rate, RATE)


def header(templates, threshold):
    fmt = lambda values: ", ".join(f"{value:.8f}f" for value in values)
    blocks = [",\n".join("        {" + fmt(row) + "}" for row in template) for template in templates]
    return (
        "// Generated by scripts/fishing_audio_templates.py.\n#pragma once\n"
        "namespace paxg::audio::reference {\n"
        f"inline constexpr int rate = {RATE}, fft = {FFT}, hop = {HOP}, frames = {FRAMES};\n"
        f"inline constexpr int bands = {len(EDGES) - 1}, refractory = {REFRACTORY};\n"
        f"inline constexpr float threshold = {threshold:.2f}f;\n"
        "inline constexpr int edges[] = {" + ", ".join(map(str, EDGES)) + "};\n"
        "inline constexpr float templates[3][frames][bands] = {\n    {\n"
        + "\n    }, {\n".join(blocks) + "\n    }\n};\n}\n"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("references", nargs=3, type=Path)
    parser.add_argument("--header", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--background", nargs="*", type=Path, default=[])
    args = parser.parse_args()
    clips = [read(path) for path in args.references]
    templates, offsets = generate(clips)
    threshold, calibration = calibrate(clips, templates, offsets)
    result = evaluate(clips, templates, offsets, [read(path) for path in args.background], threshold)
    result["threshold"] = threshold
    result["calibration"] = calibration
    result["template_start_ms"] = [1000 * offset / RATE for offset in offsets]
    args.header.write_text(header(templates, threshold), encoding="utf-8")
    args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
