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

Relative paths, existing files and non-CSV extensions are refused. Without the environment variable, no profiling queue or writer is allocated. The audio thread publishes fixed-size records to a bounded SPSC queue; a background writer now formats and writes the file about every 200 ms, without periodic `fsync`. Queue overflow drops telemetry rather than blocking audio; sequence gaps and a final dropped-record count expose incomplete captures. Normal shutdown gives the writer 100 ms to finish, then allows independently owned writer state to finish asynchronously. A blocked disk or process exit can leave a missing footer or incomplete capture. The current UI CPU meter remains unchanged. Earlier measurements used the GUI-timer writer; keep logger implementations consistent in new A/B comparisons.

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

### Rejected whole-state snapshot prototype

A second offline prototype gives workers private copies of spatial state. The
caller never waits for completion: it adopts finished state/output or renders
from its original state when a worker is late. Worker-owned snapshots remain
untouched until the entire worker batch has finished, so late work cannot
modify a voice's next quantum. Idle workers sleep on POSIX semaphores; the caller
only posts notifications. Run it with:

```sh
./build/aeolus_multicore_probe --benchmark-snapshot 0
./build/aeolus_multicore_probe --check-snapshot
```

The correctness command leaves affinity unchanged unless a CPU base is supplied.
Linux CTest includes it as `multicore_snapshot`. It checks 33 and 450 sources,
one through four participants, and silent tails while deliberately delaying one
worker by 2 ms every seventh batch. Exact serial output was preserved with both
late-job fallbacks and whole-batch reuse deferrals observed.

The VIM4 measurements reject this implementation for live use:

| Participants | Snapshot prototype median, ms |
| --- | ---: |
| 1, serial baseline in this harness | 3.084 |
| 2 | 6.099 |
| 3 | 6.997 |
| 4 | 7.647 |

These are medians of three trial medians at 450 sources/512 frames, including
state copies, notifications, result adoption/fallback and complete serial
summation. At four participants, 98.3% of worker-assigned sources were adopted,
yet it was 2.48× slower than its own serial baseline. Completion rate alone is
therefore insufficient evidence of a useful optimization. Use each harness's
own serial baseline; their loop organization/compiler output differs.

This is a fixed-geometry research tool: its copied delay buffers reuse existing
capacity. It does not implement safe capacity changes for a live application.
Copying entire delay-buffer state and transferring it between cores every
64 samples is unsuitable here. A future nonwaiting design should prepare small,
self-contained work packets and transfer only the state/data needed for that
quantum, or investigate persistent voice ownership with an explicit deadline
policy. Keep deterministic randomness, original summation order, division pool
reuse, full tails and the serial fallback. Neither offline scheduler is connected
to the application's audio callback; the working single-thread renderer and
verified 450-voice result remain the current live baseline.

The local snapshot correctness checks also passed AddressSanitizer/
UndefinedBehaviorSanitizer and ThreadSanitizer with the probe plus spatial,
delay and filter translation units instrumented. Other linked JUCE/shared-code
objects were not instrumented; these checks do not establish whole-application
thread safety.

### Live filter-packet batches: rejected for release (2026-10-17)

An experimental Linux implementation uses three persistent workers for small
spatial-filter packets. Pipe synthesis, chiff and spatial delay preparation stay
serial; results are mixed in original voice order. The callback never waits for
worker completion: a late result is recomputed locally, with private worker
storage retained until safe to reuse. This does not add a quantum of latency or
discard voices. It also does not parallelize whole voices.

The experiment requires `AEOLUS_EXPERIMENTAL_SPATIAL_BATCHES=ON` at build time
(default OFF), plus `AEOLUS_SPATIAL_WORKERS=3` at launch. Unset leaves the original
renderer active; `0` selects a serial packet control. Optional
`AEOLUS_PERFORMANCE_CPU_BASE=0` pins workers to CPUs 1–3. These controls are for
experiments, not recommended release settings.

Live A/B/A testing on the VIM4 used 450 voices, reverb off and 44,100/512.
The audio thread was pinned to performance CPU 0, workers to performance CPUs
1–3, and UI/helpers to efficiency CPUs 4–7. No governor or priority changes were
made. Both references used the same new binary with batching unset.

| Run | Processor p50 / p99 / worst (ms) | Listening |
| --- | --- | --- |
| Reference 1, 60 seconds | 10.793 / 11.721 / 12.634 | Clean |
| Three-worker candidate, 658 callbacks at 450 voices | 12.845 / 13.633 / 14.784 | Breakup |
| Reference 2, 60 seconds | 10.802 / 12.132 / 15.661 | Brief breakup about ten seconds into the run |

The candidate was stopped after audible failure; its short selection is not an
equal-duration steady-state benchmark. Nevertheless, every recorded 450-voice
candidate callback exceeded the 11.610 ms budget, and its median was about 19%
slower than either reference. Across the candidate's entire session, workers
supplied 2,793,824 results, 32,357 assigned packets fell back locally, and 300
busy-lane events occurred. High result adoption did not produce a speedup.

The listener suspected simultaneous attacks contributed to the final reference
glitch; this is not established by the capture. Its measured first ten seconds
(after five seconds settling) included a 15.661 ms callback, versus a 12.365 ms
worst in the final forty seconds. Sustained capacity and attack-burst headroom
need separate tests; the earlier clean 450-voice result is not a guarantee for
every note-on pattern. Native xrun counters remained zero despite audible
breakup and must not be used alone to judge reliability.

The experiment remains disabled and is not selected for the ARM USB package.
Packet preparation/copying and coordination are plausible overhead sources,
not separately measured causes. Local and native regression checks passed;
packet-specific sanitizer checks do not establish whole-application safety.
All captures closed normally, all 88 original organ/settings files matched their
hashes, and the pre-test profiling preferences were restored. Raw captures and
listening reports are retained under the ignored local profiling directory.

### Matched attack-burst replay (2026-10-17)

The listener's chord was captured from the Impact GX61: MIDI channel 1, notes
45, 47, 48, 50, 52, 53, 79, 81, 83 and 84, all at velocity 127. Automated MIDI
replay through an isolated profile's MIDI Through input compared simultaneous
note-ons with 100 ms spacing between notes. Four rounds used the order
simultaneous / staggered / staggered / simultaneous, holding the completed chord
for eight seconds and allowing five seconds after note-off. Every round reached
exactly 450 voices. Batching remained unset; audio stayed at 44,100/512 with
reverb off and the same thread placement as the reference above.

| Round | Attack | Worst callback in first second from first sounding note | Listening |
| --- | --- | --- | --- |
| 1 | Simultaneous | 66.230 ms | Breakup |
| 2 | 100 ms per note | 15.066 ms | Breakup |
| 3 | 100 ms per note | 14.876 ms | Breakup; settled near the end |
| 4 | Simultaneous | 78.649 ms | Breakup |

First-second windows are anchored to the first callback with active voices, not
the MIDI player's launch time. Staggered rounds ramp up over about 900 ms, so
their first-second workload is smaller. Even their first full second at 450
voices exceeded the callback budget on every call. From one to seven seconds
after reaching 450 voices, round medians were 11.510, 11.620, 11.664 and
11.678 ms respectively. These short repetitions demonstrate both severe
simultaneous-attack spikes and insufficient sustained margin in this session;
staggering did not establish reliable operation. No thermal/frequency stream was
collected for this replay, so its sustained timing difference from earlier runs
cannot be attributed to a particular system condition.

The 450-voice figure remains a previously demonstrated clean sustained load,
not a guaranteed attack-safe capacity. This experiment does not authorize
automatic onset staggering or changes to chiff, timbre or note timing. The next
diagnostic is profiling trigger/setup and early-voice processing separately from
steady rendering, then checking a lower load with the same repeatable MIDI
sequence. All 88 original organ/settings hashes remained unchanged, the profiler
closed with zero dropped records, and pre-test profiling preferences were
restored. Raw MIDI, replay timing, CSV and listening results are retained locally.

#### Follow-up: 500 ms between notes

At the listener's suggestion, two further rounds replayed the identical ten
notes with 500 ms between note-ons, holding the completed 450-voice chord for
eight seconds. Both rounds sounded clean throughout, including the final notes
and held chord. The measured ramp to 450 voices took 4.505 / 4.493 seconds.

| Round | Ramp worst (ms) | First full-load second worst (ms) | Sustain p50 / p99 / worst (ms) | Listening |
| --- | --- | --- | --- | --- |
| 1 | 12.086 | 12.423 | 10.959 / 11.680 / 12.125 | Clean |
| 2 | 11.994 | 12.276 | 11.429 / 12.118 / 12.651 | Clean |

Sustain windows cover seconds one through seven after first reaching 450 voices.
These windows contained 10 / 145 calls over the 11.610 ms budget despite clean
listening reports. The ramp worst and earlier simultaneous first-second worst
are different windows, so their comparison is descriptive, not an equal-load
speedup measurement. Thermal/frequency telemetry was not collected here either.
The result supports sensitivity to attack density; it does not identify the
expensive code or prove that 500 ms spacing guarantees reliability. Normal
playing timing remains unchanged. Both rounds closed with zero profiler drops,
all 88 original files matched their hashes, and profiling preferences were
restored after the test.

#### Sampling diagnostic of simultaneous attacks

Two further simultaneous replays with `perf` attached to the audio thread both
broke up audibly. The first active callbacks took 65.722 / 85.173 ms; these are
instrumented diagnostic observations, not comparable performance benchmarks.
The 1,999 Hz user-mode CPU-clock sample stream contained only 1 / 4 samples
inside those first callbacks and 34 / 47 in their first-second windows. This is
insufficient to attribute the attack spikes. Timestamp windows used the same
monotonic time basis as callback telemetry, but sample scarcity remains unresolved;
elapsed callback duration must not be treated as sampled CPU execution time.

Steady windows at three to seven seconds had approximately 7,400 samples each:
about 57% landed in `Voice::prepareSignal` and 39% in `Division::process`, whose
optimized symbols include inlined work. These totals do not isolate note setup
or establish the attack bottleneck. A next diagnostic should directly time
trigger/setup and early rendering separately. No onset-delay or quality change
was made. Captures closed normally with zero telemetry drops; all 88 original
files matched their hashes and pre-test preferences were restored.

For direct stage diagnosis, configure `AEOLUS_ATTACK_TIMING=ON` (default OFF)
and select a fresh `AEOLUS_TIMING_FILE` as for callback telemetry. The current
CSV adds `midi_ns`, `render_ns`, `chiff_setup_ns`, `trigger_ns`, `chiff_clear_ns`,
`spatial_setup_ns`, `reserved_ns` (zero), `triggers` and `render_calls`. Counters
are thread-local and copied into the existing bounded telemetry queue; the
background writer writes the file (the original captures used the message thread). Chiff clear is included in chiff setup, which
is included in trigger time. Triggers can occur inside MIDI handling or rendering.
Do not sum these columns as independent costs. Timers measure elapsed wall time,
including descheduling, and cannot alone distinguish CPU work from preemption.
The current implementation times initialization rather than repeatedly timing
individual voices during steady rendering. Clock reads still add overhead;
this diagnostic is not a release optimization or a substitute for uninstrumented
timing comparisons. The option does not delay note-ons or alter DSP calculations.

The first version additionally timed per-voice mono rendering, spatial filtering,
division setup and recycling. The following initial results use that earlier
CSV schema and its more intrusive probes.

The first native direct-timing replay at 450 voices recorded 450 triggers in
one callback: 90.339 ms total, 56.991 ms in MIDI handling, including 51.477 ms
inside `Voice::trigger`; rendering accounted for another 33.262 ms. The 500 ms
replay split triggers into groups of 51 or 36. Its first group spent 6.259 ms
in triggers; later groups took 0.867–1.985 ms each. These are instrumented wall
times with nested scopes, not uninstrumented CPU costs. Both rounds reached
450 voices. Local regressions passed with timing ON and OFF; native regressions
passed with the diagnostic build and batching unset / enabled.

Per-voice clock reads were much more intrusive than useful for capacity testing:
sustained callback medians rose to roughly 32 ms. No listening outcome from
this copy should replace the normal build's earlier clean 500 ms replay.
The listener reported massive breakup in the simultaneous round and breakup
after the first three to five notes in the 500 ms round. The application closed
normally with zero telemetry drops; all 88 original file hashes matched and
pre-test profiling preferences were restored.
The next measurement should retain coarse MIDI/trigger timing and remove the
per-voice mono/spatial probes, or use a controlled governor comparison on the
uninstrumented renderer.

Frequency samples approximately 110 ms apart showed the performance cluster
at 500 MHz before the simultaneous chord, 1.2 GHz about 110 ms after launch,
and 2.208 GHz by about 230 ms. The 500 ms round also began from 500 MHz and
reached 2.208 GHz about 720 ms after launch. Sampled temperatures ranged from
42.6 to 48.5 C. Governor ramp-up is therefore a candidate contributor to cold
attacks, alongside initialization work; this correlation does not establish
causality or an improvement from changing governors. No governor was changed.
Source inspection also found a full 44,100-float chiff resonator clear in each
trigger; its individual cost has not yet been isolated.

#### Temporary performance-governor test

The normal binary (intrusive stage timing disabled, batching unset) replayed
simultaneous / 500 ms / simultaneous rounds with policy0, CPUs 0–3, temporarily
set to the performance governor. Policy4, CPUs 4–7, remained conservative.
All performance-cluster frequency samples were 2.208 GHz. Each round reached
450 voices, using the same notes, registration, 44,100/512 and reverb-off setting.

| Round | Attack | Attack-window worst (ms) | Sustain p50 / p99 / worst (ms) | Listening |
| --- | --- | --- | --- | --- |
| 1 | Simultaneous | 21.962 | 11.020 / 12.076 / 12.256 | Breakup; almost settled by end |
| 2 | 500 ms per note | 12.485 | 11.301 / 12.234 / 12.500 | Clean |
| 3 | Simultaneous | 26.521 | 11.567 / 12.662 / 13.161 | Breakup throughout |

Attack windows run from first active callback through one second after first
reaching 450 voices; sustain windows cover seconds one through seven at that
load. The simultaneous peaks were smaller than the earlier normal-binary
conservative-governor peaks of 66.230 / 78.649 ms, but remained above the
11.610 ms budget. This supports frequency ramp-up as a contributor, not the
only constraint. It does not establish a reliable simultaneous-attack capacity
or isolate initialization cost at fixed frequency. The differing sustained
results also caution against treating a single clean hold as a guaranteed limit.

The user ran a temporary sudo helper because unattended SSH sudo was unavailable.
It saved the original policy0 governor and restored it on completion, exit or
after a 15-minute timeout. Restoration to conservative was verified after the
test; policy4 was unchanged. Profiling preferences were restored, all 88 original
file hashes matched, and the capture closed with zero telemetry drops. The next
code diagnostic should isolate trigger initialization at fixed frequency using
coarse timers rather than expensive per-voice rendering timers.

#### Initialization-only timing at fixed frequency

The revised diagnostic removed timers from steady per-voice rendering and split
trigger initialization into chiff setup, resonator clearing and spatial setup.
Local regressions passed with the diagnostic ON and OFF; both native regression
runs passed. At 44,100/512, reverb off, all three replay rounds reached 450 voices.
Policy0 frequency samples stayed at 2.208 GHz and sampled temperatures ranged
from 42.9 to 50.6 C. The efficiency-core policy remained unchanged.

| Round | Attack | First active callback (ms) | Trigger (ms) | Chiff setup including clear (ms) | Chiff clear (ms) | Spatial setup (ms) | Listening |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | Simultaneous | 26.740 | 16.181 | 9.949 | 8.478 | 4.291 | Breakup |
| 2 | 500 ms per note, first 51 voices | 2.082 | 1.846 | 1.061 | 0.890 | 0.589 | Clean throughout |
| 3 | Simultaneous | 32.213 | 14.053 | 9.862 | 8.285 | 2.385 | Breakup |

Both simultaneous callbacks started all 450 voices; the staggered round started
groups of 51 or 36. Its final 36-voice trigger took 1.302 ms within a 13.471 ms
callback at 450 voices. Steady callback medians were 10.862 / 11.314 / 11.432 ms,
restoring timings close to the normal renderer rather than the initial heavily
instrumented diagnostic's 32 ms. These remain instrumented elapsed times.

Resonator clearing accounted for roughly 52–59% of measured simultaneous trigger
time, identifying it as a substantial initialization cost. Spatial setup also
contributed. The next optimization candidate is reducing unnecessary clearing
while preserving every resonator read, including interpolation, wraparound,
retriggering and releases. No delay-buffer behavior, sound quality or note timing
has been changed by this diagnostic. The temporary governor was restored to
conservative after the replay.
After normal application exit was verified, profiling preferences were restored,
all 88 original file hashes matched, and zero dropped telemetry records were
confirmed. Final captures and listening reports were saved locally.

### Bounded chiff resonator reset

Chiff trigger initialization now uses `DelayLine::resetForReadDelay(_pipeDelay)`.
Rather than clearing all 44,100 floats, it resets the write index and clears the
initial prefix through `floor(delay)+1`, inclusive. Writes advance backwards;
every sample outside that prefix is written before a bounded read can reach it.
Both interpolation operands are cleared even for integer delays, so stale NaNs
cannot enter through a nominally zero-weight neighbor. Reads before writes, as
used by chiff feedback, are covered. Large, negative or non-finite bounds fall
back to the original full clear. The full `reset()` and `resize()` behavior is
unchanged.

This optimization relies on the existing voice contract: chiff frequency is set
before triggering and stays fixed throughout that voice, including its release.
The bounded delay API requires reads to remain within the chosen bound until
another reset or resize. It does not support changing to a larger read delay
mid-history without that reset. Existing voice reuse triggers a fresh reset for
the new frequency. General-purpose delay users retain the full reset API.

There is no new per-sample branch or state, no smaller allocated buffer, and no
change to noise generation, filtering, interpolation, envelopes or note timing.
`AEOLUS_ENABLE_BOUNDED_CHIFF_CLEAR=OFF` builds the original full trigger clear
for reference comparisons; its default is ON.

Local regression tests compared full and bounded resets bit for bit using dirty
and NaN-filled histories, read-before-write and read-after-write feedback,
integer/fractional/shorter delays, buffer-sized and wrapped delays, changing
bounds across resets and more than three buffer revolutions. They also checked
invalid-bound fallback and no allocation/free in reset or feedback processing.
Targeted AddressSanitizer/UndefinedBehaviorSanitizer checks passed with the delay
translation unit instrumented; these are not whole-application sanitizer results.
Both native ARM regression runs also passed, including bit-identical bounded
reset histories.

#### Live normal-build candidate

The candidate disabled intrusive stage timing and experimental batching. It
replayed the same simultaneous / 500 ms / simultaneous sequence at 450 voices,
44,100/512, reverb off. Performance-cluster samples stayed at 2.208 GHz.

| Round | First active callback (ms) | Attack-window worst (ms) | Sustain p50 / p99 / worst (ms) | Listening |
| --- | --- | --- | --- | --- |
| 1, simultaneous | 13.199 | 16.749 | 11.293 / 12.364 / 12.612 | Breakup |
| 2, 500 ms spacing | 0.547 at first 51 voices | 12.506 | 11.417 / 12.429 / 12.889 | Clean |
| 3, simultaneous | 17.408 | 17.408 | 11.633 / 12.896 / 13.609 | Breakup |

Windows use the same definitions as the earlier normal full-speed governor
test: attack runs from first active callback through one second after reaching
450 voices; sustain covers seconds one through seven at 450 voices. Relative
to that earlier reference's first callbacks of 21.962 / 26.521 ms, the candidate
reduced the simultaneous initial spikes by 8.763 / 9.113 ms (about 40% / 34%).
These are separate-session whole-build observations, not an isolated microbenchmark
or a fresh A/B/A confirmation. The first-second candidate medians still exceeded
the 11.610 ms budget in both simultaneous rounds, and listening outcomes were
unchanged. No higher clean capacity was demonstrated. This optimization reduces
initialization cost but does not solve sustained or simultaneous-attack overload.

The temporary governor helper had restored conservative before the proposed
final reference rerun, so no additional reference was captured. The earlier
fixed-frequency reference remains the comparison. The app exited normally,
profiling preferences were restored, all 88 original files matched their hashes,
and telemetry closed with zero drops. Raw captures, source/binary hashes and
listening reports are retained locally. The regular ARM USB package was not
replaced by this profiling candidate.

### Active chiff: prepared fractional-delay reads

An offline ARM probe repeatedly rendered 450 chiff generators, retriggering
every 128 blocks of 64 samples. Frequencies ranged from 27.5 to 1,760 Hz with
the voice's existing attack/decay/release and gain formulas. A fixed-period
1 ms user CPU-clock profile collected over 11,000 samples without reported
loss. About 22% landed in `Envelope::next`; the inlined caller also contained
per-sample delay-index division and integer/fraction conversion. This isolates
active chiff, unlike the earlier mostly steady pipe-playback profile.

`DelayLine::prepareRead` now computes the integer delay, fractional part and
wrapped offset once. `readPrepared` uses that offset and one conditional wrap,
retaining the original interpolation expression and both neighboring reads.
Chiff refreshes its prepared position when frequency is set. No random draws,
envelope equations, filtering, gains or skipping thresholds change. The cache
contains values, not buffer pointers; reset preserves it and resize requires
re-preparation. The original generic read remains available.

Prepared-only offline trials on CPU 0, in reference / candidate / candidate /
reference order, produced these six-second whole-probe block means:

| Variant | Trial means (ms per 450-generator/64-sample block) |
| --- | --- |
| Original fractional read | 0.779775 / 0.778083 |
| Prepared read | 0.664221 / 0.663201 |

This was about a 14.8% reduction in the probe workload, which includes triggering,
output clearing and a small output consumer. It excludes pipe synthesis, spatial
rendering, reverb and live audio I/O. The existing conservative governor was
unchanged; no per-trial frequency stream was recorded. It is not a whole-callback
speedup or a new playable voice-count result.

A generated comparison used preserved reference chiff/envelope code with
independently seeded identical JUCE random streams. All 3,248,000 compared output
samples matched bit for bit across attacks, quiet/Off behavior, resets,
frequency changes on retrigger, releases and variable frame counts. Generic
versus prepared delay tests also passed for wraparound, integer/fractional and
multi-buffer delays, reset reuse and re-preparation after resizing. Targeted
AddressSanitizer/UndefinedBehaviorSanitizer checks passed with the delay code
instrumented; local audio regressions passed.

An additional experiment forced envelope stepping inline. Its complete chiff
comparison failed at block 1, voice 9, so it was rejected and the envelope
implementation restored. The failure was not established to be harmless; that
variant is absent from the candidate. Probe sources, flags, binaries and results
are retained in the ignored profiling directory. The prepared-only normal ARM
build completed, and its native audio/MIDI regressions passed in both test runs.

A normal-rendering reference / candidate / reference live comparison used
450 voices, 44.1 kHz / 512 samples and zero reverb. Each capture replayed
simultaneous attacks, notes spaced 500 ms apart, then simultaneous attacks again.
The audio thread ran on CPU 0 and UI/helpers on CPUs 4–7; experimental spatial
workers and intrusive attack timers were disabled. Monitored performance-core
frequency stayed at 2.208 GHz throughout each capture. The temporary governor
lease was renewed between captures when needed.

| Simultaneous round | Reference 1 attack p50 (ms) | Prepared candidate (ms) | Reference 2 (ms) |
| --- | --- | --- | --- |
| First | 13.994 | 12.962 | 13.322 |
| Last | 13.986 | 13.285 | 13.727 |

Candidate attack medians were 5–7% lower than the first reference and about 3%
lower than the repeat reference. This supports a modest live improvement, with
visible run-to-run variation. Both simultaneous rounds still broke up in all
three captures; the user described the candidate as a little better. All 500 ms
rounds sounded clean. The candidate's simultaneous attack worst callbacks were
15.367 / 16.833 ms, still above the 11.610 ms block budget. No higher clean voice
limit or reliable simultaneous 450-voice onset was demonstrated.

All captures reached exactly 450 voices and closed with zero dropped profiler
records. These counters do not establish absence of audible breakup. The 88
original organ/settings files matched their saved hashes; profiling preferences
and the original governor were restored. Captures, hardware/timeline logs and
listening reports are retained under
`build/profiles/vim4-active-chiff-2026-10-17/`. The existing ARM USB package has
not been replaced by this profiling build.

### Sampled attack-stage diagnostic

`AEOLUS_ATTACK_TIMING` now also samples one serial voice-render call in 67.
The prime stride and continuous cursor walk all positions of the fixed
450-voice registration rather than repeatedly selecting even or odd voices.
The same selected call times three nonoverlapping stages: pipe synthesis plus
its initial delay/gain, chiff processing, and spatial processing/output copy.
The CSV records raw sampled nanoseconds, total voice calls and sampled calls.
The cursor continues across callbacks; disabled profiling does not advance it.
Experimental packet-worker rendering is outside this sampler's coverage and
must remain disabled for these captures.

Per-callback estimates multiply sampled stage time by the actual ratio of total
to sampled calls. They include clock overhead and systematic sampling variance;
they are estimates, not exact whole-engine stage totals. A VIM4 calibration with
the conservative governor measured about 1,244 ns inside an empty timer scope
and 2,492 ns wall time per scope. A user-accessible architectural-counter probe
also incurred substantial overhead (about 1,547 ns per read pair), so this
diagnostic retains the portable clock. Each live replay records three empty-scope
calibrations before and after playback at the test CPU speed. Analysis reports
raw estimates and approximate ranges after subtracting the observed empty-scope
cost per sampled call. Those ranges are not confidence intervals: timer cost
inside DSP can differ, and results below that uncertainty need another method.
The existing MIDI, trigger, chiff setup/clear and spatial setup timers remain nested inclusive
measurements and must not be added together. Normal builds compile these timers
out. Diagnostic callback times and listening results do not establish production
capacity or a new clean voice limit.

The comparison probe changes only the generic versus prepared chiff read
expression in a generated, test-owned reference copy. Both variants retain the
prepared-read member and frequency-time preparation, so their layouts and all
other instrumentation match. Windows are split into the first 100 ms, the next
400 ms, attack through one second after reaching 450 voices, and seconds 1–7
of the held chord. Local CTest passed all three checks with diagnostics enabled
and disabled, including sampler phase continuity and bounded profiler snapshots.
A synthetic analysis check recovered known stage costs after calibration. Both
normal-rendering ARM diagnostic apps built successfully. The prepared-read
variant passed two native audio/MIDI regression runs and the generic-read
reference passed one.

Two live captures, generic-read reference then prepared-read candidate, each
replayed simultaneous / 500 ms / simultaneous attacks at 44.1 kHz / 512 samples
with zero reverb. Every timed round logged 450 triggers and a peak of 450 voices;
performance-core frequency stayed at 2.208 GHz. Empty-scope interval means ranged
from 1,288.74–1,298.31 ns for the reference and 1,289.94–1,303.55 ns for the
candidate. Both builds broke up audibly in all three rounds, including the spaced
round that was clean in the earlier normal builds. The user described the
reference diagnostic as worse. This protocol is too intrusive for capacity
validation, so a third diagnostic reference replay was not pursued.

Approximate clock-adjusted stage means across the three held-chord windows were:

| Stage | Generic-read reference (ms/callback) | Prepared-read candidate (ms/callback) |
| --- | --- | --- |
| Pipe synthesis, initial delay and gain | 7.84–8.02 | 7.51–7.95 |
| Chiff | 1.46–1.73 | 1.18–1.55 |
| Spatial processing | 3.56–3.63 | 3.71–3.84 |

These estimated components do **not** reconcile exactly with whole-callback
measurements (held-chord p50s 12.11–12.27 ms reference, 11.75–12.17 ms candidate).
Empty-loop calibration does not remove all timing/cache/code-generation effects
inside the DSP. The table supports a tentative ordering of costs, not additive
CPU percentages, precise speedup claims or confidence bounds. Chiff's first
100 ms estimate rose to roughly 5.7–6.8 ms in the reference and 4.9–5.6 ms in the
candidate, consistent with additional attack work. Setup scopes are inclusive,
nested and instrumented too; their peaks must not be treated as normal-build
setup costs.

Analysis includes a setup-only callback when triggers arrive before the next
64-sample synthesis quantum. Omitting such a callback would incorrectly report
zero setup work for some simultaneous attacks. A synthetic check covers this
case as well as scaling and calibration arithmetic. Zero-voice-call callbacks
contribute zero estimated render time. Application activity outside the timed
MIDI windows is excluded from the registration/capacity checks.

The next focused experiment is integer delay indexing, starting with the pipe's
initial delay. Inspection of the prior normal prepared-chiff binary confirms
`udiv`/`msub` wrapping in both playing and post-release `Voice::prepareSignal`
loops. Stereo spatial delay reads also use generic nearest-read modulo in
source. Caching or fast-pathing bounded integer offsets could remove repeated
modulo operations without changing samples; this remains a proposed experiment,
not a measured gain. Preserve generic offset behavior and compare exact output,
wraps, reset/retrigger histories and tails before any normal-build live test.

Both logs closed with zero dropped records and contiguous sequence numbers.
The 88 original organ/settings files still matched their saved hashes. Profiling
preferences were restored, the governor is back to `conservative`, and the app
is closed. Raw logs, timelines, hardware samples, listening reports, calibration,
analysis scripts and source/build evidence are retained under
`build/profiles/vim4-sampled-stages-2026-10-17/`. The normal ARM USB package remains
unchanged.

### Prepared integer delay reads

An initial bounded fast path in generic `readNearest` improved the isolated pipe
case by about 4% but slowed the stereo-delay case by about 1.6%. That version was
rejected and the generic reader restored. Its measurements remain in the ignored
profiling evidence directory.

The retained candidate adds `prepareNearestRead(int)` and
`readNearestPrepared(size_t)`. Preparation normalizes an exact nonnegative integer
offset once per process block; each sample then uses one conditional wrap and
loads the same float directly. This avoids float conversion, interpolation and
per-sample modulo/range checks in Release builds. Generic signed and arbitrary-offset behavior
remains available unchanged. Prepared offsets must be recomputed after resize;
reset and writes preserve them. Callers keep them local to a process call, so
there are no new voice fields, persistent caches or allocations.

Pipe synthesis prepares its initial delay once per 64-sample subframe. Scalar
and paired-ear NEON spatial processing prepare their left/right offsets once per
call; the experimental packet preparation path does likewise. Scalar single-sample
`tick` retains the generic reader. Zero/negative frame-count processing remains
a no-op. Full-buffer delays retain the original wrap-to-current-position behavior.
No gains, filter equations, envelopes, random draws, routing or voice limits change.

Local CTest passed all three checks. Targeted AddressSanitizer and
UndefinedBehaviorSanitizer checks instrumented the actual delay implementation.
Integer-delay tests compare an independent legacy modulo/history model with the
generic and prepared readers, including one-sample/non-power-of-two buffers,
multiple wraps, cached reads across reset, re-preparation after resize, signed
zero, NaN payloads, infinities/subnormals, negative generic offsets and `INT_MAX`.
Preparation, reads, writes and reset are checked for audio-side allocation/free.

Generated ARM comparisons used preserved reference delay/spatial code and the
candidate's actual code. Both paired-ear NEON and scalar spatial variants matched
bit for bit across 4,328,640 channel samples each, including variable frame counts,
44.1/48/96 kHz spatial configuration, geometry/resizing, resets, aliased input or
output buffers and an uninterrupted final zero-input tail.

A CPU-0 probe rendered 450 delay lines in reference / candidate / candidate /
reference order, with two seconds of warm-up followed by three seconds of timing
per trial. The conservative governor was unchanged; frequency samples inside
every timed interval reported 2.208 GHz. Means per 450-line/64-sample block were:

| Delay-only workload | Reference means (ms) | Prepared candidate means (ms) |
| --- | --- | --- |
| Pipe: one read, 44,100-sample rings | 0.227845 / 0.225791 | 0.220258 / 0.220336 |
| Stereo: two reads, 668-sample rings | 0.204161 / 0.203980 | 0.186408 / 0.186012 |

These are about 3% and 9% reductions respectively. The probe includes writes,
output stores, offset preparation and an external output consumer, but excludes
pipe wavetable synthesis, chiff, spatial filters, reverb and device I/O. It does
not establish a whole-callback gain or a higher clean voice count. Both complete
spatial-pipeline comparisons were equivalence checks, not speed measurements.

The normal ARM application built from all project source files with stage timing
and experimental spatial workers disabled; unchanged JUCE/resource objects were
reused. Its native audio/MIDI regressions passed in two runs.

A normal-build reference / candidate / reference live comparison used 450 voices,
44.1 kHz / 512 frames, reverb off, audio on CPU 0 and helpers on CPUs 4–7.
Performance-core frequency stayed at 2.208 GHz throughout every timed round.
Each capture played simultaneous attacks, then notes 500 ms apart, then another
simultaneous attack. Median sustained callback times were:

| Round | First reference (ms) | Prepared candidate (ms) | Final reference (ms) |
| --- | --- | --- | --- |
| 1: simultaneous | 11.030 | 10.727 | 11.066 |
| 2: 500 ms | 11.205 | 11.018 | 11.207 |
| 3: simultaneous | 11.378 | 11.032 | 11.310 |

That is roughly 2–3% less sustained rendering time. Simultaneous-attack medians
improved only about 1–2%, and still exceeded the 11.61 ms callback deadline.
The listener reported breakup in rounds 1 and 3 and a clean round 2 in both
references; the candidate also broke up initially in rounds 1 and 3, with round
2 clean. This is a modest timing gain, not a demonstrated increase in clean voice
capacity or a solution to simultaneous-attack breakup.

All three logs closed with zero dropped records and contiguous sequence numbers.
The application is closed, profiling preferences were restored, both CPU policies
are back to `conservative`, and the 88 original organ/settings files still match
their saved hashes. Evidence is retained under
`build/profiles/vim4-nearest-delay-2026-10-17/`. The normal USB package is unchanged.

### Persistent voice ownership prototype

`aeolus_voice_probe` is a separate Linux test executable, not an application mode.
It assigns each voice to one persistent participant for its complete lifetime,
including release, and processes pipe wavetable playback, initial pipe delay,
chiff and stereo spatial processing together. Lane zero participates while up to
three workers render the other voices. Initial assignment balances voice counts
in round-robin order; it does not yet predict or measure each voice's cost.
The coordinator applies trigger/release/reset changes only between completed
quanta; per-owner MIDI/control command queues are not implemented here.
Cache-aligned, preallocated per-voice output buffers allow the coordinator to sum
voices in their original order. Four independently accumulated partial mixes
would change floating-point rounding, so this first prototype keeps ordered
mixing on lane zero.

Both pipe drift and chiff normally use shared static random generators. New
explicit-stream overloads allow this probe to give each voice two exclusively
owned, fixed-seed JUCE streams; the normal application keeps its existing shared
streams and serial processing. This is not an equivalence claim against the
application's original cross-voice random sequence. Checks compare the same
seeded voices rendered serially and with persistent owners, sample for sample,
and compare their ordered mix and completion state. They cover 33 and 450 voices,
one through four participants, staggered releases, reset/retrigger, completed
release tails and an injected 2 ms worker delay. The fixture uses three embedded
stop models and the ten pitches used in the live capacity chord, where supported
by each model. It is not the user's complete 45-pipe-per-note registration.

Build with `AEOLUS_BUILD_TESTS=ON`. On headless Linux, run:

```sh
xvfb-run -a ctest --test-dir build --output-on-failure
AEOLUS_TEST_DATA_DIRECTORY="$PWD/build/voice-probe-data" \
  xvfb-run -a build/aeolus_voice_probe --benchmark 0
```

The benchmark requires four available CPUs starting at the supplied index; `0`
is correct for the VIM4's performance cluster but is not universal. Use only a
test-owned data directory: the probe writes its embedded fixture there. Trials
run core counts 1/2/3/4 then 4/3/2/1, warm up for two seconds each, reset voices
and seeds, and time 450 complete voices plus ordered mixing in eight-quantum
(512-frame) blocks. The first 86 blocks cover approximately one second after
trigger; the next 256 form the later-render window. Trigger/reset preparation,
MIDI dispatch, division modulation, resampling, reverb and device I/O are excluded.

Workers busy-poll and the coordinator waits for every completion without a
bounded fallback. This measures a scaling opportunity, not audio-callback
reliability. A late or descheduled owner cannot safely be rendered concurrently
by the coordinator because it owns mutable delay, envelope and filter state.
The delay-injection checks verify output/state ownership, not a safe deadline
recovery policy. No part of this team is called by the normal engine, and no
multicore live build or updated USB package is provided by this experiment.

Local CTest passed all four checks, including `voice_ownership`. On the VIM4,
all 51 project source files were rebuilt with stage timing and experimental
spatial batches disabled, reusing only unchanged JUCE/resource objects. The
native ownership checks passed for every core count and both voice counts.

The two benchmark passes gave these uncapped render-block times. Ranges are the
two trial medians; p99 and worst columns take the larger value from either trial.

| Participants | First-second p50 (ms) | Later p50 (ms) | Later p99 (ms) | Worst, either window (ms) |
| --- | --- | --- | --- | --- |
| 1 | 14.300–14.410 | 8.807–8.919 | 10.927 | 15.405 |
| 2 | 7.254–7.306 | 4.483–4.516 | 6.383 | 8.264 |
| 3 | 5.073–5.094 | 3.166–3.168 | 3.998 | 5.710 |
| 4 | 3.809–3.814 | 2.403–2.406 | 3.047 | 4.454 |

Four participants improved first-second median time by about 3.77x and later
median time by about 3.69x. At one participant, 146 of 172 first-second blocks
exceeded the 11.61 ms reference budget; no later blocks did. At each of two,
three and four participants, none of the 684 measured blocks exceeded that
budget. These are offline threshold comparisons, not device deadline/xrun
measurements. There was no live listening test and no new sustainable voice-count
claim. The differing fixture also prevents comparing its absolute times directly
with the earlier live-registration captures.

Hardware samples throughout all eight timed intervals reported 2.208 GHz for
the performance cluster; the largest recorded thermal-zone temperature was
55.8 C. Affinity samples showed rendering participants on CPUs 0–3 and helper
threads on CPUs 4–7. Both governors remained `conservative`; the benchmark did
not change policy, limits or priorities. The 88 original organ/settings hashes
still matched after testing.

This is enough scaling to justify a separate live-integration design, including
per-owner control queues, predictable assignment as registrations change, shared
random-stream semantics, safe resource/tuning lifetimes and bounded handling of
late workers without lost samples, races or callback waits. The current offline
rendezvous is deliberately not connected to `Engine::processSubFrame`.
Evidence, source snapshot, commands, binary hashes, hardware/affinity samples and
analysis are retained under `build/profiles/vim4-voice-ownership-2026-10-17/`.
The normal ARM USB package remains unchanged.

### Nonblocking voice snapshots

A second, separate probe mode implements completion without waiting for a
worker. It is selected with `--check-nonblocking` or
`--benchmark-nonblocking FIRST_CPU`; the original waiting experiment remains
available for comparison. Neither mode is connected to the normal audio engine.

`VoiceSnapshot` prepares independent voice/delay storage outside processing.
For one 64-sample quantum it copies scalar DSP/envelope state and only the
circular history windows that the pipe, chiff and left/right spatial reads can
access. Nearest reads follow writes and need only `min(64, normalizedDelay)`
historical samples; zero-delay reads use this quantum's new writes entirely.
Chiff's fractional reads precede writes and need up to 65 historical samples,
including the interpolation neighbour. Quiet/inactive chiff history is skipped.
The scratch synthesis buffer is not transferred because processing clears it
before use. Adoption commits the 64 newly written
samples and final state; a quiet/inactive chiff commits zero resonator writes.
Full canonical delay history remains valid for subsequent quanta. Intrusive list
links and engine ownership never transfer to a worker. Changed pipe identity,
rebuild flags, empty buffers or changed buffer sizes reject offload rather than
allocate or resize during processing.

Each persistent worker receives at most one outstanding request. Its snapshots
are reused only after the whole previous request has completed. The coordinator
renders its own voices, then checks each result once. A finished result is
adopted; a queued job is cancelled when possible; a running/late job is discarded
and the canonical voice is rendered locally. Busy or failed lanes also render
locally. No completion polling, condition-variable wait, join or spinning occurs
in the processing method. Workers sleep on semaphores between requests; startup
and shutdown waits remain outside processing. Publication requires lock-free
integer/bool atomics on the target platform.

Correctness checks compare seeded serial output, per-voice output, ordered mixes,
random states and release completion. Direct snapshot checks alternate adoption
and discard, with elevated test-only chiff gain to exercise long held resonator
history through repeated wraps. Threaded tests cover 33/450 voices, one through
four participants, releases and reset/retrigger while previous snapshots can
still be running. Workers are held until the coordinator has completed 24 quanta,
then receive additional 2 ms injected delays. A completion wait would prevent the
coordinator from reaching the gate release. All canonical rendering continues
through these faults; no stale result or voice state is substituted.

All five local CTest checks passed. Per-thread C++ new/delete instrumentation,
including aligned operations, recorded zero allocations/frees in capture,
rendering, adoption and fallback. Targeted AddressSanitizer/UndefinedBehaviorSanitizer
checks passed with the snapshot implementation and probe instrumented; other
DSP/JUCE objects were not instrumented, and GUI leak detection was disabled.
This is not a ThreadSanitizer or complete application sanitizer validation.

The constructor's pipe resources must remain immutable and alive for the entire
snapshot/team lifetime. Current test fixtures supply that guarantee; normal
engine tuning/reload ownership does not yet do so. The tests still use explicit
per-voice random streams, not the application's shared cross-voice random order.
These contracts must be integrated before this helper can become a live organ
mode. The normal application and USB package remain unchanged.

The initial nonblocking implementation used conservative 66-sample windows and
also transferred the scratch synthesis buffer. Its ARM equivalence checks passed,
but two reverse-order benchmark passes showed a performance regression:

| Participants | First-second p50, two-trial mean (ms) | Later p50, two-trial mean (ms) |
| --- | --- | --- |
| 1 | 14.415 | 8.895 |
| 2 | 18.137 | 14.710 |
| 3 | 19.392 | 16.588 |
| 4 | 19.988 | 17.415 |

Every measured block with multiple participants exceeded the 11.61 ms reference
budget, despite over 99.9% of four-participant worker jobs being adopted. All eight
timed intervals reported 2.208 GHz, so this was not observed governor ramp-up or
throttling. A separate main-thread `cpu-clock:u` sampling diagnostic found about
29% in generic `memcpy`, 11% in capture and 7% in adoption, with additional copy
helper/PLT costs. That diagnostic includes setup/warm-up and multiple core counts;
its percentages are not isolated callback-stage timings or capacity evidence.

This motivated the reduced windows and skipped transfers described above.
Their local CTest, targeted ASan/UBSan and native ARM equivalence checks passed.
Two further reverse-order passes measured the reduced-copy version:

| Participants | First-second p50, two-trial mean (ms) | Later p50, two-trial mean (ms) | Larger later p99 (ms) | Worst block (ms) |
| --- | --- | --- | --- | --- |
| 1 | 14.422 | 8.895 | 10.962 | 15.361 |
| 2 | 16.842 | 12.533 | 14.136 | 17.612 |
| 3 | 17.737 | 13.827 | 15.403 | 18.614 |
| 4 | 18.092 | 14.409 | 15.888 | 18.629 |

Four-participant later-render time fell by about 17% from the first snapshot
version, but remained about 62% slower than serial. Two participants were still
about 41% slower than serial. Every multi-participant measured block exceeded
the 11.61 ms reference budget. Four-participant adoption remained above 99.9%,
with no rejected snapshots or busy lanes in those timed trials; lateness alone
does not explain the regression. All timed intervals reported 2.208 GHz. The
reduced-copy trials' maximum thermal-zone sample was 52.1 C.

The first native build rebuilt all 52 project source files. The reduced-copy
build recompiled the snapshot and probe translation units, verified hashes for
all other project source files and reused their unchanged objects. Commands and
source/binary hashes identify both versions. Both governors, profiling preferences
and all 88 original organ/settings files were unchanged after testing.

This design passes the correctness gate but fails the performance gate for a
live trial. It remains a research helper, with no live organ or sustainable
voice-count improvement claimed. Measurements include snapshot preparation,
adoption, local fallback and worker wake-up costs. The earlier 3.7x waiting
prototype measured a different completion strategy. The next candidate is to
amortize checkpoints over larger render jobs, while preserving MIDI event
boundaries; copying state for each voice every 64 samples is too expensive in
these measurements. That follow-up is described below.

Evidence for both snapshot variants and the diagnostic profile is retained under
`build/profiles/vim4-voice-nonblocking-2026-10-17/`. No live test was launched and
the normal ARM USB package remains unchanged.


### Larger nonblocking render jobs

The offline probe now supports jobs containing one to eight original 64-frame
DSP steps. `--check-large` checks limits of 128, 256 and 512 frames;
`--benchmark-large FIRST_CPU QUANTA` benchmarks 1, 2, 4 or 8 steps per job.
The snapshot copies the history needed for the entire job once and commits only
the actual writes. It counts chiff writes separately, including transitions into
quiet sustain or inactivity, and stops processing a voice after its tail ends.
All output storage is prepared before rendering. Late workers still use immediate
canonical fallback, with no completion wait.

The correctness corpus compares every voice sample, ordered mix, random seed and
finished-tail state with serial 64-frame processing. It includes 33 and 450 voices,
one through four participants, delayed/held workers, discarded results, repeated
releases and reset/retrigger boundaries. Elevated test-only chiff gain exercises
multiple resonator wraps. Jobs split at 512-frame block ends and deliberately
unaligned internal-quantum control boundaries, including quanta 33, 67, 513, 601,
3073 and 3151. These are fixed-pipe DSP tests, not a new MIDI scheduler: actual
processor MIDI events may split render ranges at arbitrary sample positions, and
live integration must preserve those ranges and the engine's buffered-quantum
semantics. No lookahead into another callback, added latency or onset staggering
is implemented. Immutable pipe lifetime and per-voice RNG restrictions from the
previous experiment still apply.

The comparison renders 450 voices at 44.1 kHz in 512-frame blocks, using the same
three-model fixture as the earlier ownership experiments. Each job-size run tests
one through four participants twice, in opposite orders, after a two-second warmup
per configuration. Only the active output prefix is cleared, transferred and
mixed, so shorter jobs do not process unused maximum-size voice output storage.
The first 86 blocks cover attack evolution; the next 256 cover later rendering.
Trigger/reset, MIDI/engine scheduling, reverb, resampling and audio-device work
remain outside these timings. The 11.61 ms threshold is a reference budget for the
probe, not a measured device-xrun count. A fresh single-participant baseline is
included for every job size.

Validation: all six local CTests passed under Xvfb, including the new
`voice_snapshot_large` test. The same large-job correctness corpus passed on the
VIM4. Scoped `new`/`delete` instrumentation detected no allocations or frees in
coordinator/worker rendering. Targeted AddressSanitizer/UndefinedBehaviorSanitizer
checks passed for the snapshot and probe translation units; the remaining DSP and
JUCE objects were not sanitizer-instrumented, GUI leak detection was disabled,
and this was not a ThreadSanitizer run.

VIM4 results (milliseconds per 512-frame block, mean of the two trial p50s):

| Job frames | One core attack / later | Four cores attack / later | Four-core worst | Four-core blocks over 11.61 ms |
| --- | ---: | ---: | ---: | ---: |
| 64 | 15.680 / 10.050 | 19.890 / 16.111 | 20.616 | 684 / 684 |
| 128 | 13.755 / 9.088 | 13.907 / 11.205 | 14.548 | 275 / 684 |
| 256 | 12.370 / 7.884 | 9.709 / 7.646 | 10.163 | 0 / 684 |
| 512 | 10.659 / 6.414 | 6.709 / 5.070 | 7.220 | 0 / 684 |

Larger jobs help even without worker offload. At the same 512-frame job size,
four participants improve attack p50 by about 37% and later p50 by about 21%
against one participant. The three-participant 512-frame result was
7.124 / 5.205 ms, worst 7.477 ms. Four participants had later p99 at or below
5.797 ms in both trials, and adopted every submitted snapshot: 115,254 per trial,
with zero fallback, busy lanes or rejected captures. Fault-injection correctness
tests separately exercised fallback; these timings do not establish a worst-case
bound under arbitrary OS delays.

Every timed interval sampled the performance cluster at 2.208 GHz; the maximum
thermal-zone sample across all four job-size runs was 52.0 C. Thread-affinity
samples confirmed rendering on cores 0–3 and other inherited threads on 4–7.
Both governors remained conservative. All 88 original files and the profiling
preferences were verified unchanged, with no profiling application left open.

These results use one binary with larger preallocated per-voice output storage,
including for the fresh 64-frame baseline. That baseline is slower than the
previous smaller-buffer probe's serial measurement; keep the historical results
separate. The same-job-size one/four-participant comparison above isolates the
additional worker benefit within this experiment. No production callback or
maximum sustainable live voice count was measured.

The 512-frame variant passes the fixed-fixture correctness and performance gates
for further integration work. The 64/128-frame paths still fail the attack budget;
a live design must avoid those regressions when MIDI/control boundaries shorten
jobs. Integrating engine modulation, original RNG behavior, voice recycling and
safe immutable-resource lifetime remains required before a listening trial.
The installed application and ARM USB package were not updated.

Evidence, frozen native source, binary/source hashes, benchmark/hardware/affinity
logs, analysis, native build commands and local test/sanitizer logs are retained
under `build/profiles/vim4-voice-large-jobs-2026-10-17/`.

### Experimental live complete-voice jobs

`AEOLUS_EXPERIMENTAL_VOICE_BATCHES=ON` adds an opt-in Linux stereo test renderer.
The option defaults OFF and rejects multibus/non-Linux builds. In a test-enabled
profiling build, select `AEOLUS_VOICE_WORKERS=3` for three workers plus the audio
coordinator, or `0` for the serial comparison. Unset the variable to retain the
ordinary serial rendering path. `AEOLUS_VOICE_JOB_QUANTA=1` forces the test
renderer to process one original quantum at a time; otherwise its maximum is
512 frames. `AEOLUS_PERFORMANCE_CPU_BASE=0` pins workers to cores 1–3 on the
known VIM4 layout. Audio-thread pinning to core 0 is handled by the test launcher;
the UI and other inherited threads use cores 4–7. These CPU numbers are specific
to the tested VIM4, not a general ARM affinity policy.

The engine batches only within the current MIDI render range and keeps its
original subframe buffer and interpolator behavior. At 44.1 kHz it calculates
how many original quanta that range would generate, including an already buffered
interpolator sample. It never renders into another MIDI range or callback beyond
the original partial-quantum behavior. Other device rates use single-quantum
serial processing in the experimental renderer. Reverb, output gain, limiting,
division mixing, swell filtering and tremulant updates keep their existing order.
Jobs shorter than 256 frames and registrations below 64 voices render locally.

Coupler aggregation is a sequential control calculation: coupled keys can change
across several internal quanta. The test renderer requires an unchanged aggregate
key state in every division before batching. It also requires every included voice
to remain in Attack (including sustained loops); release/over states can recycle
pool entries and change stop activation on following quanta. Those transitions
use original single-quantum processing until stable. The first version without
these guards failed the coupled-registration comparison and was rejected. The
safety fallback preserves those behaviors at the expense of reduced multicore
coverage during releases and coupled-state transitions. GUI stop/link changes
are sampled at job boundaries; MIDI control changes still split render ranges.
No note staggering, reduced-precision arithmetic, voice-limit increase, tail
shortening or extra callback of audio latency is introduced.

The experimental renderer gives each pool slot two private random streams,
using the same pipe-instability and chiff algorithms. Its serial reference uses
those same streams so output can be compared exactly regardless of worker order.
This changes random realization/draw ordering relative to the ordinary shared-RNG
path; it does not establish bit-identical output against the normal application.
The default build retains its original shared RNG behavior.

Experimental builds prepare immutable copies of playback pipes and their models
outside audio processing. Voice state and private snapshots retain the generation
they actually play, so a late worker does not borrow mutable/deleted source wave
or model data. A non-audio owner retains retired generations; the engine-global
UI timer collects them only when no source/voice/snapshot owns them. Reference
counts can change during processing, but final destruction occurs outside it.
This costs additional setup time and memory. Spatial-ring capacity is reserved
at device preparation using the current model set, while logical ring lengths
remain unchanged. Snapshots rebind only to retained generations and reject
unsupported buffer sizes. Editing models beyond the prepared spatial bound needs
fresh device preparation; live organ-editor/reload workflows have not received
end-to-end concurrency validation in this experiment.

Two existing MIDI allocations were also removed from the tested callback path.
The coupler destination decision is computed directly instead of allocating a
vector for every note-on/off, with no fixed division-count cap. A temporary no-op
keyboard listener primes JUCE's iterator storage during engine construction,
before processor/editor listeners attach; this generates no organ note events.

Validation on 2026-10-17: all six local CTests passed, and a separate build of all
project translation units with the experimental flag disabled passed the normal
audio/MIDI regressions (unchanged JUCE/resource objects were reused). The ARM
build also passed the full audio/MIDI regressions. Its integration corpus compares
serial single-quantum and adaptive multicore output bit for bit, with active
couplers, MIDI stop changes, repeated/short notes and nonzero event positions,
variable callback sizes, wet-zero then 42% convolution reverb, panic, queue
overflow, and releases that finish before panic. It verifies no prepared job
remains across a MIDI render-range boundary and matches voice counts after every
callback. At 48 kHz the test renderer remains serial. Delayed workers exercise
fallback at 44.1 kHz; native logs recorded both adoption and fallback.

Processor-only callback checks observed no scoped C++ allocations/frees. They do
not cover live editor callbacks or every C allocation API. Targeted ASan/UBSan
checks passed for the engine, division, voice, batch, snapshot, rankwave, delay,
chiff, spatial and integration-test translation units. JUCE and remaining DSP
objects were uninstrumented, GUI leak detection was disabled, and no TSan claim
is made. Separate immutable-generation tests retain/render/rebind pipes after
both the mutable source pipe and its model have been destroyed.

A silent VIM4 standalone startup/normal-close check passed at 44100/512. ALSA's
thread was unnamed on this image, so the launcher identified it with a four-second
setup-only userspace perf capture, then pinned it to core 0. Placement verified
voice workers on 1–3 and UI/background threads on 4–7. The setup profiler stops
before readiness; it is not a live performance result. The launcher stores CSV,
placement and app logs in its test folder and uses isolated organ data. The
initial launcher's XDG settings override was ineffective: JUCE's standalone
hardcodes `~/.config`. The startup comparison happened to leave preferences
unchanged and did not prove settings isolation. Linux test-enabled standalones
now require an absolute `AEOLUS_TEST_SETTINGS_DIRECTORY`, supplied by the launcher;
their generated entry point uses that directory without editing JUCE or changing
the production standalone. All 88 original files, the original profiling preferences, and both
conservative governors remained unchanged; the smoke-test application was closed
normally. No MIDI notes were sent during the startup check.

The first live single-quantum serial comparison reached 450 voices at 44100/512,
but the listener reported severe breakup in all three rounds (simultaneous,
500 ms note spacing, simultaneous). At 440 or more voices, median callback times
were 15.80/16.11/16.32 ms; every captured callback in that subset exceeded the
11.61 ms deadline. This is a regression relative to earlier clean spaced-note
tests, not evidence against the as-yet untested live multicore candidate.
Keep the single-quantum result separate from serial 512-frame batching and
multicore batching; the experimental serial path itself changes memory access
and RNG organization. The user manually enabled virtual MIDI after the settings
override failed; only `audioSetup` differed from the original preference backup.
The corrected standalone subsequently passed a silent startup/normal-close check
on the VIM4: its private settings file was saved, existing user preferences kept
the same SHA-256, and audio/worker/UI placement verified at 44100/512. This checks
settings isolation and startup, not live multicore sound or capacity.

The subsequent live candidate used three workers plus the coordinator at the
same 450 voices, 44100/512 and conservative governors. The listener reported
attack breakup in rounds 1 and 3, with round 2 (500 ms note spacing) clean.
For callbacks with at least 440 voices:

| Round | Median ms | p99 ms | Worst ms | Over 11.61 ms / captured |
|---|---:|---:|---:|---:|
| Simultaneous 1 | 7.663 | 12.454 | 46.676 | 9 / 692 |
| 500 ms spacing | 7.686 | 8.578 | 20.802 | 3 / 693 |
| Simultaneous 3 | 7.849 | 13.618 | 31.226 | 9 / 692 |

The clean listening report for round 2 does not mean zero callback deadline
misses. These figures include callbacks during release while the voice count
remains high, and exclude lower-count portions of each round. Final job counters
were adopted=767433, fallback=643, local=320702, busy=0, rejected=0. Compared with
the single-quantum experimental reference, this combines larger jobs and workers;
it does not isolate the benefit of multicore alone. A serial-large comparison
with the same maximum job size is needed for that attribution. Simultaneous
attack reliability and a higher sustainable voice limit remain unproven.

Native source/binary hashes match the checkout. All 3,095 checked JUCE-module and
resource files used by the cached dependency objects match local bytes. The first
native build compiled all 53 project source files; after the coupled-state guard
was added, all three translation units including `voicebatch.h` and the integration
test were rebuilt, with unchanged project objects reused after hash verification.
The ARM test binary is separate from the normal installation and USB release.
Full live breakup, xruns, sustained capacity and rapid GUI/model-reload behavior
remain to be measured before promotion.

Local evidence is retained under
`build/profiles/vim4-live-voice-jobs-2026-10-17/`. The test launcher is
[`tools/vim4_voice_batch_test.py`](../tools/vim4_voice_batch_test.py).

### Claude branch comparison and independent panic prototype

Claude commit `dc81f53a941c984fac4148c5922e8501b80070c3` was built separately
on the VIM4, with unchanged branch DSP source and a private-settings standalone
entry point. Its full audio/MIDI regressions passed. The branch deliberately
waits for in-flight worker jobs; its opt-in configuration uses FIFO 70 on audio
core 0 and three workers on cores 1–3. UI/background work inherits cores 4–7.
The governors remained conservative. These scheduling changes are part of this
comparison, so the result does not isolate the renderer from scheduling policy.

Six offline runs at 710 voices, 44100/512 and a 1024-voice pool compared serial,
batch-serial and three-worker modes in forward/reverse order. Sustained medians
were 20.05/19.63 ms, 13.72/13.56 ms and 4.00/4.00 ms respectively. Attack results
varied substantially between passes, and the unpaced FIFO benchmark can encounter
kernel RT throttling; it is not a live capacity measurement. No system RT limits,
RT bandwidth settings or governors were changed.

The subsequent matched 450-voice live rounds all sounded clean to the listener.
For callbacks with at least 440 voices:

| Round | Median ms | p99 ms | Worst ms | Over 11.61 ms / captured |
|---|---:|---:|---:|---:|
| Simultaneous 1 | 4.596 | 9.322 | 15.719 | 2 / 693 |
| 500 ms spacing | 4.586 | 7.089 | 7.472 | 0 / 693 |
| Simultaneous 3 | 4.779 | 8.766 | 16.905 | 2 / 692 |

Clean listening does not mean zero deadline misses. Later manual playing logged
a peak of 816 voices, which the listener described as clean above 800. That was
not a controlled sustained-capacity confirmation. Further overload made Aeolus
freeze or become difficult to close. Final team statistics reported 78.4% of jobs
on workers, 138.3 us mean wait and 872.8 us maximum wait for that session.

An isolated Claude-based panic/watchdog prototype now has a separate control
process on the efficiency cores. It requests silence, waits for worker completion
before reclaiming voices, and stops the exact application process if the panic
is not acknowledged. It never returns early into buffers still owned by workers.
The implementation, recovery thresholds, validation and limitations are recorded
in [the prototype instructions](../tools/claude_panic/README.md).

The prototype's live 450-voice panic test at 44100/512 received acknowledgement
in about 59.7 ms. The listener confirmed prompt silencing and successful explicit
Resume, with the application still running. This is one listening trial;
request-to-observed-acknowledgement timing is not acoustic latency or a worst-case
overload guarantee. A separate silent frozen-process test verified escalation
through SIGTERM and SIGKILL without automatic application restart.

Evidence is retained under `build/profiles/vim4-claude-multicore-2026-10-17/`
and `build/profiles/vim4-claude-panic-2026-10-18/`. The normal installation and
the earlier ARM USB package remain separate from these experiments.

### Production integration — October 18, 2026

The accepted scope is now the measured NEON/spatial kernels, bounded readable-ring
clears, the frozen Claude `dc81f53` renderer and HEALTH 2 Panic/watchdog recovery.
The integrated renderer is opt-in on Linux stereo builds; unset keeps the serial
reference and 512-entry pool. No fast-math, onset staggering, reduced precision,
new note-admission policy or release truncation is introduced. Later Claude
release/retrigger experiments and the rejected nonblocking/snapshot renderers are
not production targets.

Integration hardening adds worker-startup/scheduling fallback before publishing
jobs, exception-safe worker teardown, an explicit Panic/Resume epoch, and safe
supervisor detach when JUCE replaces a processor inside the same process. Existing
stop-model replacement waits for active notes/tails and a callback barrier;
creating a fresh model variant keeps existing voices sounding. Profiler file I/O
runs in a background writer and cannot hold the UI timer or audio callback.

The normal Linux standalone starts its adjacent Python companion automatically;
it requires no root privileges or test launcher. Recovery is independent of CSV
logging. Separate console Panic and Resume buttons remain available; an optional
GTK companion provides control outside the organ UI. Recovery reports are saved
after the owner exits/detaches. Automatic restart remains deferred.

The VIM4 USB IRQ result is mitigation evidence for a specific saturation/MIDI
starvation condition, not a newly measured clean voice limit. The temporary IRQ
change was restored. Persistent system changes, profile/core-selection menus,
whole-note admission limits, colored headroom meters and one-button boot remain
in [roadmap section 16](../AEOLUS_FEATURES.md#16-installer-setup-hardware-profiles-and-one-button-startup--agreed-2026-10-18).

### Integration verification

Current checks on October 18, 2026:

- Linux x86-64: 2/2 CTest tests passed under Xvfb (9.13 seconds); 33 Python
  tests passed with one FIFO-permission case skipped on this host.
- Native VIM4 ARM64 Release/IPO: 2/2 CTest tests passed (23.15 seconds), including
  `AEOLUS_TEST_POPUP_WORKFLOW=1` on the authenticated desktop. All 33 Python
  tests passed, including the actual FIFO-demotion case.
- An isolated test-enabled standalone automatically started its adjacent safety
  companion. Explicit Panic acknowledged, explicit Resume restored callbacks,
  JUCE processor replacement created a new guardian and detached the old one,
  and normal window closing exited with status 0 and reclaimed runtime files.
- A second isolated run deliberately stopped that exact process with SIGSTOP.
  The guardian detected stalled audio, requested correlated Panic, then escalated
  through SIGTERM/SIGKILL. Exit was -9; the private post-exit recovery report was
  saved and runtime files were reclaimed. No MIDI was sent in these checks.
- All 90 recorded normal organ/settings/application hashes remained unchanged.
  USB IRQ 37 remained allowed on 0–7/effective CPU 0; both CPU governors remained
  `conservative`. All 95 bundled recording/model hashes match the asset inventory.

These are regression and recovery checks, not a new listening/capacity benchmark.
The hardware runtime checks used isolated test-owned organ data/preferences;
normal production executables are built separately with test overrides disabled.
macOS/Windows compilation, new sustained-capacity limits and device-hotplug
reconnection are not established by this verification.

Evidence: `build/profiles/production-integration-20261018/` (local) and the owned
`production-integration-20261018` directory in the VIM4 lab. Private desktop
credentials and normal organ/settings files are not included in USB packages.
