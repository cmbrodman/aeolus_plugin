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

## VIM4 live comparison — 2026-10-17

Fresh native kernel/UBSan and audio/MIDI regressions passed before these tests. Two reference/NEON/reference sequences used native GCC 13.3 Release/IPO builds, the same copied organ/settings and direct FiiO USB ALSA output at 44,100 Hz / 512 frames (11.610 ms nominal buffer period). Each selected window contains 1,724 processor calls over approximately 20 seconds after five seconds settling. The reference executable is the same one used in the earlier delay comparison; NEON was linked against the same common baseline objects, changing the arithmetic dispatch/kernels. Test configurations and data remained separate from the regular installation.

Reverb-off runs sustained 399 voices. Reverb-on runs sustained 282 voices; saved state confirmed the actual wet setting was 42% (requested approximately 40%) and IR index 2. All saved processor parameters and IR matched exactly across the three reverb-on runs.

| Run | Voices | Reverb | Median ms | p99 ms | Worst ms | Processor budget exceeded | Native xrun increase |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| reference-1 | 399 | 0% | 10.349 | 11.126 | 11.472 | 0 | 0 |
| neon-1 | 399 | 0% | 10.371 | 11.105 | 11.677 | 2 | 0 |
| reference-2 | 399 | 0% | 10.345 | 11.176 | 12.201 | 6 | 0 |
| reverb-reference-1 | 282 | 42% | 8.279 | 12.269 | 12.780 | 188 | 0 |
| reverb-neon-1 | 282 | 42% | 7.925 | 10.940 | 11.386 | 0 | 0 |
| reverb-reference-2 | 282 | 42% | 8.260 | 12.420 | 12.821 | 182 | 0 |

At 399 voices with reverb off, the NEON median was about 0.2% slower than the fresh reference runs, with small p99 differences: no useful improvement is demonstrated. Fresh reference medians of 10.345–10.349 ms and p99 of 11.126–11.176 ms fall within the earlier reference ranges of 10.302–10.376 ms and 11.023–11.241 ms.

With reverb enabled, NEON reduced median processor time by 4.1–4.3% and p99 by 10.8–11.9% relative to the two references. Processor-budget exceedances fell from 188 / 182 to zero in the selected windows. These are measured improvements for this workload, not a demonstrated increase in maximum sustainable polyphony. The reverb-off result is consistent with the current SIMD call sites being in convolution.

The listener reported all six runs clean. All captures closed normally with zero telemetry drops, sequence gaps or native xrun increases. The main CPU cluster held 2.208 GHz throughout the measured windows; SoC temperatures were 49.3–54.3 C overall and 50.6–52.9 C for the reverb-on comparison. No governor, priority, affinity or thermal settings were changed for live tests. All 88 original organ/settings files were verified unchanged by hashes.

Processor duration excludes surrounding device handling and ALSA writes. A processor call exceeding the nominal buffer period is not a confirmed device xrun; the two observations remain separate. These short sustained-note captures do not replace attack/release, longer-duration or maximum-load testing.

## Paired-ear spatial filtering

`AEOLUS_ENABLE_SPATIAL_NEON=ON` selects a paired-ear spatial filter on supported GCC/Clang ARM64 targets when the global NEON option is enabled. `OFF` keeps the original sample-by-sample spatial processing; single-sample `tick` remains the scalar reference in both builds. Other platforms retain the scalar spatial path.

The two vector lanes represent left/right ears, retaining independent coefficients, input/output histories, attenuation and delay reads. Delay lengths, wrapping, filter recurrence order and voice release lifetimes are unchanged. Coefficients/history are packed once per block and histories committed before return, so resets and transitions to scalar `tick` remain valid. Multiply/add contraction is disabled in the candidate translation unit; float32 precision is retained. No new allocations, locks or I/O occur in the audio path.

The existing spatial tests compare impulse/noise/silent-tail output and history at asymmetric positions and varied rates/block sizes, including zero-length blocks, resets, in-place buffers, aliased outputs and mixed `process`/`tick` calls. They require finite output with peak sample error below 1e-6 and no C++ heap operations during processing. `aeolus_tests --benchmark-spatial` alternates scalar and block processing for an offline kernel comparison. Target-device processor timing and listening remain necessary to assess the full application; a kernel speedup is not a voice-count gain.

## VIM4 sustained-voice capacity — 2026-10-17

The highest verified clean load in these tests was **450 sustained voices** with paired-ear NEON, including a continuous two-minute capture. The previous spatial reference broke up at the same 450-voice chord, organ and saved settings. Relative to the earlier 399-voice clean reference, the demonstrated clean load increased by 51 voices (approximately 12.8%).

Settings: direct FiiO USB ALSA, 44,100 Hz / 512 frames, reverb off, native GCC 13.3 Release/IPO builds with arithmetic NEON enabled in both variants. The configured 512-voice pool was unchanged. Higher-load search trials used adjusted registrations/chords; voice cost varies, so this is an observed workload limit, not a universal exact cutoff.

| Build / trial | Voices | Observed span s | Median processor ms | p99 ms | Listening result |
| --- | ---: | ---: | ---: | ---: | --- |
| Paired-ear NEON, first pass | 450 | 60.00 | 10.767 | 11.534 | Clean |
| Paired-ear NEON, confirmation | 450 | 120.00 | 10.735 | 11.494 | Clean throughout |
| Paired-ear NEON, search trial | 464 | 5.37 | 11.877 | 12.775 | Audible breakup |
| Paired-ear NEON, search trial | 480 | 8.58 | 12.174 | 13.258 | Audible breakup |
| Previous spatial reference | 450 | 4.78 | 12.134 | 12.856 | Audible breakup |

Successful captures exclude five seconds settling and measure 60 / 120 seconds respectively. Failed trials were stopped on audible breakup; their short observed intervals include transients and are not equal-duration steady-state comparisons. No intermediate clean maximum between 450 and the failed 464-voice trial was established. A fresh candidate launch restored the original registration for the two-minute confirmation; its saved processor parameters, IR and division state matched the subsequent 450-voice reference exactly.

At 450 voices, candidate p99 of 11.494 ms was close to the 11.610 ms nominal buffer period; 54 of 10,337 processor calls exceeded that period during the two-minute window, which still sounded clean throughout. At 464 / 480 voices, observed medians of 11.877 / 12.174 ms exceeded the period and the listener reported breakup. The previous spatial reference at 450 likewise exceeded the period (12.134 ms median) and sounded broken up.

**The native xrun counter remained zero even during audible failures.** It is not an exhaustive dropout detector on this output path. The bundled JUCE ALSA backend configures silence insertion and increments its underrun counter on `-EPIPE` write errors; counter silence must not be treated as proof of clean output. Listening, processor duration and start intervals were kept as separate observations.

Both successful 450-voice windows sampled the main CPU cluster at 2.208 GHz; SoC temperatures were 47.2–52.6 C across those windows. No governor, scheduling, affinity, thermal, precision, tail or voice-pool changes were made. All captures closed normally with zero telemetry drops or sequence gaps. All 88 original organ/settings files were hash-verified unchanged. Raw data, partial hardware streams and listening reports remain in the ignored local profiling directory.

## Multicore scaling experiment

The VIM4 reports performance CPUs 0–3 (capacity 1024, maximum 2.208 GHz)
and efficiency CPUs 4–7 (capacity 577, maximum 2.016 GHz). These indices are
specific to the tested board/kernel; discover topology before applying affinity
on another system.

An explicit, Linux-only offline probe is available in test-enabled builds:

```sh
./build/aeolus_multicore_probe --benchmark-multicore 0
# Use 4 instead of 0 to compare the efficiency cluster.
```

The probe runs without a graphical session. The requested CPUs must be available in the process's permitted affinity mask.
Close audio applications before measuring. The probe pins only its participating
threads, restores their previous masks on exit, and does not change governors,
process priorities, system affinity, organ files or standalone settings.

The probe runs 450 independent existing spatial sources, eight 64-sample quanta
per timed batch, and retains serial voice-order stereo summation. It compares
one through four participants using fixed slices or dynamically claimed batches
of 16 sources. Three trials reverse participant order on the middle trial.
Before each measurement, 33- and 450-source comparisons require bit-identical
serial/parallel output across excitation and silent spatial tails. Worker threads
persist during measurement; setup and correctness checks are outside timing.

This is **spatial-only, offline throughput research**. It does not parallelize
the application's voices or establish a new clean voice limit. It excludes pipe
playback, chiff, MIDI, resampling, division modulation, reverb and audio-device
I/O. Workers busy-poll, and completion uses an unbounded atomic rendezvous:
**do not put this experimental scheduler in an audio callback**. It gives useful
scaling evidence while deliberately leaving the existing live renderer intact.

For a live design, use the audio callback plus three persistent DSP workers on
the four performance cores. Prepare shared control and randomness serially;
render independent voice batches into preallocated private buffers; reduce in
original voice order; reclaim voices only after rendering completes. Splitting
by division alone can leave cores unused when a dense registration concentrates
on one division. Keep a single-thread path for small loads and measure two,
three and four participants rather than assuming four is always fastest.

The current pipe-instability and chiff generators have shared random streams.
Concurrent calls can change which voice receives each random value even if the
random generator itself tolerates concurrency. Preserve draw order before
parallelizing those stages. Worker completion, preemption and lifetime rules
need a separate real-time design; dynamic claims redistribute unstarted work,
but do not recover a job already held by a descheduled worker. Added quantum
latency, dropped voices or omitted tails are not acceptable hidden fallbacks.

A VIM4-specific application policy can place the UI and ordinary background
threads on CPUs 4–7 while explicitly assigning audio participants to CPUs 0–3.
Audio-critical convolution work needs its own scheduling decision. Per-thread
pinning does not prevent unrelated processes or interrupts from using the
performance cores; exclusive isolation would be a separate OS experiment.

### VIM4 spatial-only results, 17 October 2026

Native GCC 13.3 Release/LTO, both existing NEON paths enabled, 450 spatial
sources and 512 frames per timed batch. Values below are the median of three
trial medians, including serial summation and worker coordination:

| Performance-core participants | Fixed slices, ms | Dynamic 16-source batches, ms |
| --- | ---: | ---: |
| 1 | 3.602 | — |
| 2 | 1.931 | 1.939 |
| 3 | 1.369 | 1.435 |
| 4 | 1.104 | 1.107 |

Four fixed slices gave 3.26× spatial-stage throughput relative to one core.
Across the three four-participant trials, p99 was 1.327–1.340 ms and the worst
observed batch was 1.489 ms. Every serial/parallel comparison was bit-identical.
These measurements support starting with evenly balanced slices for this
uniform spatial workload; they do not show how unequal full-voice workloads
will balance. They are not complete callback timings, a production scheduler
validation or an increased sustainable voice-count claim.

Preserve division preparation/reclamation boundaries in an initial live
prototype: the serial engine can return completed voices to its shared pool
before the next division triggers new pipes. Preparing every division before
rendering any could change allocation at the 512-voice limit. Parallel batches
within each prepared division avoid that change while still allowing a heavily
loaded single division to use all four cores.

For comparison, four efficiency cores measured 2.497 ms with fixed slices
(2.472 ms with dynamic batches); one efficiency core measured 7.769 ms. These
are medians of three trial medians under the existing governor and ordinary
thread scheduling. No real-time priority or exclusive core isolation was enabled.
All 88 original organ/settings files matched their pre-test SHA-256 hashes.
