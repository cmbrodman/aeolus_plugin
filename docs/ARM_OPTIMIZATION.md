# ARM optimization and verification

Updated 2026-10-17. The target is the Khadas VIM4 running 64-bit Ubuntu 24.04.3 LTS. Native ARM64 CI uses Ubuntu 22.04; it is not a VIM4 hardware benchmark.

## First optimization pass

Release link-time optimization is available through `AEOLUS_ENABLE_IPO=ON` and enabled for ARM64 CI. CMake checks compiler/linker support and fails clearly if unavailable. The portable ARM64 baseline remains unchanged; no host-specific `-march=native`, fast-math, lower precision, voice reduction or shortened reverb is introduced.

The proposed block-local spatial-filter experiment was implemented and measured, then reverted: native ARM64 CI with IPO showed reference median 80.242 us versus candidate 80.322 us (0.999x), with zero peak sample difference. That does not support retaining the extra DSP code. Source snapshot: `7fb1210`; CI run [39055091538](https://github.com/cmbrodman/aeolus_plugin/actions/runs/39055091538). This is hosted ARM hardware, not the VIM4. The original spatial-processing implementation remains; tests and the benchmark provide a baseline for future experiments.

The VIM4 handoff reports a substantial improvement from LTO on the board, but this update does not establish a new measured full-engine improvement. Keep those observations separate from CI kernel timings.

Tests compare block processing against `tick` over impulses, deterministic noise, silent tails, asymmetric positions, 44.1/48 kHz geometry, varied/zero block sizes, in-place/overlapping buffers, resets and transitions back to `tick`. They reject peak sample error of 1e-6 or greater and check that processing allocates/frees no C++ heap objects. Existing MIDI, voice-release and convolution regressions still run.

## Reproducible kernel measurement

```sh
cmake -S . -B build-arm -DCMAKE_BUILD_TYPE=Release \
  -DAEOLUS_ENABLE_IPO=ON -DAEOLUS_BUILD_TESTS=ON
cmake --build build-arm --parallel 2
xvfb-run -a ctest --test-dir build-arm --output-on-failure
xvfb-run -a build-arm/aeolus_tests --benchmark-spatial
```

The benchmark alternates reference and block processing order, warms both, uses 300 independent spatial sources with 64-frame blocks, consumes outputs, and reports median, p99 and worst elapsed microseconds. It is an offline spatial-kernel measurement, not a full-engine voice-capacity or callback-deadline result. Virtual-machine scheduling and a different ARM CPU can affect results. Run several times on the target board and inspect dispersion before accepting a performance claim.

## VIM4 comparison

Preserve the working display/touch configuration and existing organ files. Use the same organ, registration, notes/velocities, sample rate, 512-sample buffer and direct USB ALSA device for baseline and candidate. Confirm the current governor, cluster frequencies and thermal state; log them rather than silently changing the system. The handoff reports throttling, so uncontrolled sequential runs can obscure small changes.

Compare an unchanged build and an IPO-only build first; measure each subsequent DSP candidate separately. Record uncapped callback median/p99/worst times and deadline misses/xruns, sustained voices, note-off/release behavior and reverb settings. Listen to repeatable passages and tails. The existing capped/smoothed audio meter and process CPU alone do not establish an improvement. Do not claim the handoff's observed polyphony as a new validated result.

The older VIM4 delay-index experiment is not imported: it reportedly gave little observable improvement and remains a local experiment on that machine. The GCC declaration correction is already in the repository. Convolution idle suspension, NEON kernels, multicore synthesis and algorithmic reverb remain separate work; none is implied by enabling IPO.

## Voice-delay experiment and processor telemetry

`AEOLUS_USE_BLOCK_DELAY=ON` selects a voice-delay implementation that computes its read cursor once per block and advances read/write cursors together. It retains the original ring length, write-before-read semantics (including delay equal to ring length), gain operation, chiff and spatial processing. `OFF` retains the sample-by-sample reference for A/B builds. Tests compare samples bit-for-bit and verify ring history after changing delays, wrapping, resets, zero-length blocks and interleaved use of the original API. There is no power-of-two resizing or tail truncation.

Run `aeolus_tests --benchmark-delay` to measure only this kernel, using alternating-order reference/candidate processing for 399 delay lines and 64-frame blocks. A kernel speedup is not an equivalent full-engine speedup.

For device testing, build with `AEOLUS_PROFILE_BUILD=ON`. This gives the standalone a separate `Aeolus-Profile` application/settings name. In a test-enabled build, also set `AEOLUS_TEST_DATA_DIRECTORY` to a **test-owned copy** of the organ directory and settings. This override does not redirect standalone audio-device settings, which are isolated by the separate application name. Do not run normal and profile applications against the same exclusive ALSA device simultaneously.

Opt into timing with a fresh, absolute `.csv` path:

```sh
AEOLUS_TIMING_FILE=/tmp/aeolus-baseline-unique.csv ./Aeolus-Profile
python3 tools/analyze_callback_profile.py /tmp/aeolus-baseline-unique.csv \
  --voices 399 --warmup-seconds 5
```

Relative paths, existing files and non-CSV extensions are refused. Without the environment variable, no profiling queue or output stream is allocated. The audio thread publishes fixed-size records to a bounded SPSC queue; an owned message-thread timer writes the file every 200 ms. Queue overflow drops telemetry rather than blocking audio; sequence gaps and a final dropped-record count expose incomplete captures. Close the application normally to drain queued records and write the footer. The current UI CPU meter remains unchanged.

Records use a monotonic clock and include processor duration, block size, engine/device sample rate, active voice count and the native device xrun count. The native count is read on the device audio thread (important for JUCE's ALSA counters); `-1` means unavailable. No file I/O, allocation or blocking synchronization is added to processing.

The analyzer reports median/p99/worst processor time and budget utilization, processor calls longer than `frames/sample_rate`, native xrun counter increases across the selected span, counter resets and telemetry drops. **Processor-budget exceedances are not the same as confirmed device deadline misses.** This measures `processBlock`, excluding surrounding JUCE device handling, format conversion and ALSA writes. Native xruns provide a separate device-level observation, and an xrun after the final recorded processor call may not be observed. Timing publication and file-writing overhead should be held constant across A/B runs. Inspect note attacks/releases as well as sustained 399-voice periods.
