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

## VIM4 delay experiment: rejected

A block-based delay candidate preserved output/history bit-for-bit but did not improve full-engine performance on the VIM4. The candidate is removed; the original voice-delay implementation remains. Experimental source is retained in Git history at `4ece46c`.

On 2026-10-17, native GCC 13.3 Release/IPO builds were tested in reference/candidate/reference order with the same copied organ/settings, reverb down, direct FiiO USB ALSA output, 399 sustained voices and 44,100 Hz / 512 frames (11.610 ms nominal budget). Each measured window contained 1,724 processor calls over approximately 20 seconds after five seconds of settling.

| Run | Median ms | p99 ms | Worst ms | Processor budget exceeded | Native xrun increase |
| --- | ---: | ---: | ---: | ---: | ---: |
| Reference 1 | 10.376 | 11.241 | 11.924 | 4 | 0 |
| Candidate | 10.570 | 11.394 | 12.919 | 5 | 0 |
| Reference 2 | 10.302 | 11.023 | 11.759 | 3 | 0 |

The candidate median was approximately 1.9–2.6% slower than the two reference runs, so it was rejected. This short comparison does not establish a universal regression or a new maximum voice count. The listener reported all three runs clean. All captures closed normally with zero telemetry drops or sequence gaps. Processor-budget exceedances are distinct from native xruns. The main CPU cluster held 2.208 GHz during measured load; no governor, priority or thermal settings were changed. Original organ and application settings were verified unchanged by hashes.

The hosted ARM delay microbenchmark had suggested about 1.54x kernel throughput, but the native VIM4 kernel test was approximately unchanged. This illustrates why hosted kernel speedups must be checked in the full application on target hardware.

## Processor telemetry

For device testing, build with `AEOLUS_PROFILE_BUILD=ON`. This gives the standalone a separate `Aeolus-Profile` application/settings name. In a test-enabled build, also set `AEOLUS_TEST_DATA_DIRECTORY` to a **test-owned copy** of the organ directory and settings. This override does not redirect standalone audio-device settings, which are isolated by the separate application name. Do not run normal and profile applications against the same exclusive ALSA device simultaneously.

Opt into timing with a fresh, absolute `.csv` path:

```sh
AEOLUS_TIMING_FILE=/tmp/aeolus-baseline-unique.csv ./Aeolus-Profile
python3 tools/analyze_callback_profile.py /tmp/aeolus-baseline-unique.csv \
  --voices 399 --warmup-seconds 5
```

Relative paths, existing files and non-CSV extensions are refused. Without the environment variable, no profiling queue or output stream is allocated. The audio thread publishes fixed-size records to a bounded SPSC queue; an owned message-thread timer writes the file every 200 ms. Queue overflow drops telemetry rather than blocking audio; sequence gaps and a final dropped-record count expose incomplete captures. Close the application normally to drain queued records and write the footer. The current UI CPU meter remains unchanged.

Records use a monotonic clock and include processor duration, block size, engine/device sample rate, active voice count and the native device xrun count. The native count is read on the device audio thread (important for JUCE's ALSA counters); `-1` means unavailable. No file I/O, allocation or blocking synchronization is added to processing.

The analyzer reports median/p99/worst processor time, consecutive processor-start intervals and budget utilization, processor calls longer than `frames/sample_rate`, native xrun counter increases across the selected span, counter resets and telemetry drops. **Processor-budget exceedances are not the same as confirmed device deadline misses.** This measures `processBlock`, excluding surrounding JUCE device handling, format conversion and ALSA writes. Native xruns provide a separate device-level observation, and an xrun after the final recorded processor call may not be observed. Timing publication and file-writing overhead should be held constant across A/B runs. Inspect note attacks/releases as well as sustained 399-voice periods.

## ARM64 NEON arithmetic

`AEOLUS_ENABLE_NEON=ON` (default) selects GCC/Clang ARM64 Advanced SIMD kernels for all existing arithmetic dispatch entries: add, scaled add, add then scale, scale, aligned/unaligned dot products, complex multiplication, conjugated complex product and FFT butterflies. ARM64 kernels use baseline intrinsics from the [Arm ACLE reference](https://arm-software.github.io/acle/neon_intrinsics/). Other architectures retain their existing dispatch. `OFF` retains scalar ARM dispatch for controlled comparisons.

The kernels retain float32 precision and use separate multiply/add operations, with floating-point contraction disabled for their translation unit. Dot products use four accumulators and a pairwise reduction, so addition order differs from scalar and small rounding differences are expected, as with existing SSE. Complex conjugation follows the existing engine convention `conj(a*b)`, not `a*conj(b)`. Complex and FFT counts are numbers of floats and must be even. Kernels handle zero counts, unaligned addresses and scalar tails without padding reads. Exact aliases are supported for mixing and complex products; partial overlap is outside the contract.

With tests enabled, `aeolus_neon_tests` checks every kernel across varied sizes, offsets, aliases, guards and tails. Elementwise, complex and FFT outputs must match separate-operation reference results exactly; dot products are checked against a double-precision reference with a bounded error. The full audio suite checks that NEON dispatch is selected on ARM64 and compares convolution output against scalar rendering (peak difference below 1e-5). Run `aeolus_neon_tests --benchmark` for alternating scalar/NEON kernel timings; compiler auto-vectorization remains allowed for the scalar baseline. These are kernel measurements, not voice-capacity claims.

Current production call sites use dot products, conjugated complex multiplication and FFT butterflies in convolution. The other arithmetic entries are implemented for completeness but currently have no synthesis call sites. No improvement to reverb-off polyphony is implied. Device-level A/B timing remains necessary before claiming a sustained voice-count gain.

Initial VIM4 GCC 13.3 Release/IPO kernel measurements, warmed and pinned to CPU 0, showed dot-product median speedups of approximately 2.74x / 3.51x / 3.93x at 64 / 256 / 4,096 floats. Conjugated complex multiplication was approximately 0.99x / 0.97x / 0.98x and FFT butterflies 1.06x / 0.99x / 1.06x: these kernels do not show a uniform improvement over GCC's auto-vectorized scalar code. Alternate load/shuffle layouts were slower and discarded. No whole-engine speedup is established by these measurements.

Native standalone kernel tests and UBSan passed. The board's ASan runtime could not initialize its shadow allocator, so that launch is not a passing ASan result; hosted ARM64 CI runs ASan/UBSan separately. Test builds and copied data are isolated from the regular VIM4 installation.
