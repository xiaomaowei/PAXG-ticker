# Experimental fishing audio matching

The integrated ESP32-S3 firmware defaults to Legacy Amplitude. On the Fishing web page, open **Audio detection settings** and select **Spectral Matching (monitor only)** to analyze USB playback. Entering spectral mode requests a stop and disarm before persisting the change. The physical and web controls that start fishing are gated in this mode. Events appear in the page counter and serial log. Returning to Legacy Amplitude or restoring defaults requires a fresh arm action.

Bag-full detection uses a separate spectral template generated from the addon's raid-warning OGG, in both modes. A score of 0.90 or higher registers a pulse, with a 1.30 s duplicate-suppression interval. Two consecutive intervals of 2.65–3.35 s latch BAG_FULL. The bag similarity is available as bag_similarity in GET /api/audio. Regenerate its template with scripts/generate_bag_audio_template.py. A local build needs both Wi-Fi settings and the optional MacroDroid webhook setting in its own ignored .env.

Settings are stored in NVS under `paxg_audio/match_v1`. Defaults are Legacy Amplitude, a 74% threshold, all three templates enabled, and debug disabled. Template toggles affect best-match selection and event decisions, while individual scores remain visible. The open section polls `GET /api/audio` at most once per second with a single poll in flight. Debug adds maximum DSP time and the DSP task's stack low-water mark. Buffer usage and drops refer to the DSP ring in mono PCM frames. Existing USB packet counters remain in Fishing telemetry.

## Reproduce analysis, templates and synthetic evaluation

Install the offline dependencies with `python -m pip install numpy scipy soundfile`. Supply the original OGGs in template order. The analysis outputs contain waveform PCM and should remain local.

```powershell
python scripts/analyze_fishing_audio.py path/to/FishingBobber_ver2_1.ogg path/to/FishingBobber_ver2_2.ogg path/to/FishingBobber_ver2_3.ogg --output audio-analysis
python scripts/fishing_audio_templates.py path/to/FishingBobber_ver2_1.ogg path/to/FishingBobber_ver2_2.ogg path/to/FishingBobber_ver2_3.ogg --header test/arduino_idf_integration_poc/main/audio_match_reference.h --report audio-analysis/offline.json
python scripts/audio_parameter_sweep.py path/to/FishingBobber_ver2_1.ogg path/to/FishingBobber_ver2_2.ogg path/to/FishingBobber_ver2_3.ogg
```

Pass `--background` followed by independent audio files to add background-only and mixed cases. Calibration uses different gain/offset settings, noise seed and negative fixtures from evaluation. The three exact-source self-checks are tracked separately and excluded from aggregate metrics. Both synthetic splits share the original sound identities and cannot estimate live accuracy. Fragment cases omit part of the selected signature and are labeled as rejection challenges.

## Measured source characteristics

All three inputs are 22,050 Hz mono. RMS and peak below use full scale = 1. The 1% cumulative energy point describes onset energy without picking an arbitrary sample-level threshold.

| Template | Duration | Peak | RMS | FFT peak | Spectral centroid / bandwidth | 1% energy | Strongest STFT frame | Max sample |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3.493 s | 0.8874 | 0.0619 | 581 Hz | 2,186 / 2,035 Hz | 188 ms | 499 ms | 1,607 ms |
| 2 | 3.161 s | 0.9631 | 0.1049 | 640 Hz | 2,008 / 2,056 Hz | 181 ms | 894 ms | 1,012 ms |
| 3 | 3.030 s | 0.9910 | 0.0988 | 610 Hz | 1,925 / 2,061 Hz | 160 ms | 952 ms | 944 ms |

| Band | Template 1 | Template 2 | Template 3 |
|---|---:|---:|---:|
| 0–250 Hz | 0.26% | 1.66% | 3.39% |
| 250–500 Hz | 4.29% | 8.65% | 10.72% |
| 500–1,000 Hz | 27.52% | 28.03% | 30.97% |
| 1,000–2,000 Hz | 33.74% | 31.56% | 26.33% |
| 2,000–4,000 Hz | 19.55% | 17.38% | 16.01% |
| 4,000–8,000 Hz | 11.42% | 9.36% | 9.51% |
| 8,000 Hz–Nyquist | 3.22% | 3.35% | 3.07% |

## DSP and resource choices

USB already supplies stereo PCM16 at 48 kHz. The existing bridge downmixes it to mono and forwards a bounded copy to a 4,096-sample ring. The USB task performs no FFT or spectral matching. A separate priority-1 DSP task uses a 4 KiB stack and precomputes the Hann window and FFT twiddles. The worker discards queued pre-break audio and clears the overlap/matching history on discontinuities. The DSP task and buffers use internal memory with no PSRAM requirement.

The reference generator resamples OGG audio to 48 kHz offline. The firmware uses a 1,024-sample Hann STFT, 480-sample hop and 16-frame history spanning 171.33 ms. Each frame sums power into 18 bands from 234 Hz to just under 9.9 kHz, takes square roots, normalizes and subtracts the band mean. The sliding dot product over the feature stack, divided by the norms, is cosine similarity and a centered normalized correlation in feature space. Hann limits leakage between adjacent buckets. Per-frame normalization reduces gain sensitivity. The silence gate uses maximum frame RMS above 0.0001 full scale. The three 171 ms templates start at 450, 730 and 820 ms. A global 600 ms refractory interval limits duplicate events and may suppress close successive events.

The spectrum uses a local radix-2 FFT with cached coefficients. The pinned PlatformIO environment has no existing DSP component dependency. This keeps the build reproducible with the current toolchain; compare ESP-DSP on target if the 10 ms per-hop budget is tight. The radix-2 workload is 512,000 butterflies/sec. A 16 kHz runtime pipeline would need anti-alias filtering and decimation, and eliminate the 8–9.9 kHz evidence. A 24 kHz pipeline would retain that band with filtering and decimation, and could halve the FFT size at similar frequency resolution. Runtime resampling was avoided for this experiment. The rate cases in Python exercise filtering and reconstruction before feeding the native-rate model.

| FFT / hop / frames | Window | Butterflies/sec | TP / FP / FN |
|---|---:|---:|---:|
| 512 / 480 / 16 | 161 ms | 230,400 | 8 / 0 / 13 |
| 1024 / 240 / 31 | 171 ms | 1,024,000 | 21 / 0 / 0 |
| 1024 / 480 / 12 | 131 ms | 512,000 | 19 / 0 / 2 |
| 1024 / 480 / 16 | 171 ms | 512,000 | 20 / 0 / 1 |
| 1024 / 480 / 24 | 251 ms | 512,000 | 21 / 0 / 0 |
| 2048 / 480 / 16 | 193 ms | 1,126,400 | 19 / 0 / 2 |

The 16-frame configuration keeps a shorter window than the 24-frame candidate at the same FFT cost. The 240-sample hop both doubles FFT cost and increases the matching work. The comparison is exploratory because all candidates use the same three source identities. The calibrated 0.74 threshold yielded 20/0/1 on the separate synthetic cases (precision 100%, recall 95.2%). The miss was template 2 at 16 kHz. Observed latency for the 20 synthetic hits was 615–996 ms from clip start, averaging 834 ms. On the Windows host, Python reported up to 15.6 ms process CPU per case (coarse process-clock granularity) and about 15.1 MiB peak traced allocations. These figures describe the batch Python model. PlatformIO reported 88,944 bytes of static RAM and 1,235,757 bytes of flash use for the experimental build, versus 61,008 and 1,219,645 bytes for the OTA baseline. The linker map attributes 19,668 bytes to the analyzer, in addition to the 8,192-byte ring and 3,456 bytes of read-only template vectors. Each OTA application slot has 6,553,600 bytes. For the firmware, measure per-hop time, heap and stack directly with the Web UI.

## Device acceptance and rollback

1. On a spare ESP32-S3, install the experimental image through the existing OTA flow in [OTA.md](OTA.md). Verify the expected image and selected OTA slot before enabling gameplay controls.
2. Confirm the host enumerates both the UAC speaker and HID keyboard. Confirm the ticker, TFT, Wi-Fi, Fishing page and OTA status are usable.
3. Select Spectral Matching. Verify the device reports disarmed, the arm/start commands are blocked, and an audio event never issues a HID press. Play each original through the UAC endpoint, then play real Eternal fishing and unrelated mixed playback while logging scores, counters, heap and stack. Label the expected game events independently.
4. With Wi-Fi and UI polling active, check sustained UAC queue occupancy/drops, USB stream interruptions, DSP duration relative to the 10 ms hop, watchdog/reset counters and Wi-Fi responsiveness. Reboot to verify persisted settings and use Restore Default Settings to validate Legacy Amplitude reentry.
5. To roll back, reinstall the known-good previous firmware image with the existing OTA flow, or write it to the correct app slot with a verified image using the existing device recovery procedure. Verify the chosen slot and version, then confirm Legacy Amplitude behavior with a deliberate arm action. The experimental settings occupy their own NVS key.
