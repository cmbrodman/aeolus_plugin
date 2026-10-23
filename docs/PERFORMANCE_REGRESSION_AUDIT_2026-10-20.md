# VIM4 performance regression audit — October 20, 2026

## Main finding

The failing VIM4 process is running the **serial renderer with the default
512-entry physical voice pool**, not the three-worker/1024-entry configuration
used for the earlier high-polyphony work. The multicore implementation is still
present. Its selection remains opt-in; these defaults did not change in the
correctness audit. This is a confirmed configuration mismatch, not proof that
every reported audio fault has the same cause.

The code audit also finds additional control/admission work and a reverb deadline
cost introduced since the integration baseline. Their existence is confirmed;
their net effect must be measured rather than inferred from the added loops.
The matched local and native runs below do not show a large overall sustain
regression. On the VIM4, current three-worker sustain took about 45.5% of the
callback budget at 1003 actual voices, while serial mode exceeded the budget.
Initial attacks still produced deadline misses in both compared versions.

## Scope and evidence

- Original accepted Claude renderer: `dc81f53`; integration baseline: `de405fc`,
  with independent Panic recovery. Both are compared with current source below.
- Current application source: `e33b22c`, also the source in merge `2370e9e`.
  This commit combines the earlier voice-capacity/meter increment and later
  audit repairs; the commit boundary alone does not identify which task authored
  each line. Later footer/workflow commits do not alter the synthesis kernels.
- Actual VIM4 executable inspected over SSH: the separate native Release/IPO
  build made during diagnosis. `/proc` showed no `AEOLUS_DSP_WORKERS` setting and
  no DSP-team threads. The original executable also lacked the worker setting
  and was built without IPO. Enabling IPO alone did not resolve the listening
  complaint.
- At audit start, both governors were `performance`, audio was pinned to CPU 0,
  and scheduling remained ordinary time sharing. The earlier diagnostic capture
  had observed `conservative`, frequencies falling to 500 MHz and audio migration
  onto efficiency cores. The user reported improvement after governor correction,
  with remaining breakup. No further system-policy changes are part of this audit.
- The saved device setup is direct FiiO USB ALSA, 44,100 Hz / 512 frames; the
  live timing trace confirmed that rate and block size. Reverb wet was zero.
- Saved MTS is disabled. The user organ has the same stop definitions as the
  default layout but additional Manual III octave-coupler definitions. Those
  links and bass coupling were disabled in the inspected saved registration.
  The benchmark's default organ/couplers-off workload does not cover every
  possible custom coupling graph or reproduce an actual MIDI performance.
- The remembered approximately 50% at 1024 voices is the user's historical
  observation. The retained documentation establishes three-worker tests above
  800 voices, but does not independently establish a controlled clean 1024-voice
  limit. Neither recollection nor an old smoothed meter substitutes for matched
  current measurements.

## Findings

### PERF-01 — High priority: multicore configuration was lost at launch

**Confirmed operational cause.** [RealtimeConfig](../Source/aeolus/rtconfig.h)
defaults to `dspWorkers = -1` and `maxVoices = 512`; those defaults are unchanged.
With no worker environment setting, `prepareBatchRenderer()` returns without
creating a DSP team. The running process and its thread list agree with this
path. A saved logical voice-cap preference does not enlarge the startup pool.

The [VIM4 launcher](../tools/vim4/aeolus-multicore.sh) still defaults to three
workers and a 1024-entry pool. Packaging also distinguishes **Start Aeolus.sh**
(serial) from **Start Aeolus multicore.sh**. Running the executable directly does
not inherit the latter's settings. The temporary **Start-Aeolus-LTO.sh** made in
the preceding diagnosis selected CPU 0 only; it did not restore multicore mode.
That test was insufficient to reproduce the earlier performance setup.

**Next action:** use the documented multicore launcher against the IPO-enabled
production executable, verify the actual worker configuration and measure/listen
again. Do not infer worker use merely from the physical pool size or UI voice cap.

### PERF-02 — Added admission/control work on every synthesis quantum

**Confirmed additional work; net regression not established.**
[Engine::prepareControlAdmission](../Source/aeolus/engine.cpp) runs on each
64-sample quantum, approximately 689 times per second at the internal rate.
It rebuilds the coupling graph, scans active voice headers in `beginAdmission`,
clears per-stop masks and runs collecting control steps. The subsequent control
phase releases and commits voices. These phases have different responsibilities;
simply deleting one would break admission or release semantics.

[Division::triggerVoicesOfEnabledStops](../Source/aeolus/division.cpp) now tests
missing keys separately for each enabled stop, including keys outside that stop's
playable zones. The old early-exit implementation was cheaper in some cases but
also hid missing held-key voices. In grouped rendering, admission remains serial
work before workers start, so excess preparation limits parallel scaling.

The first live userspace profile attributed about 1.6% of all sampled Aeolus
thread CPU time to `beginAdmission`, with most samples in pipe/spatial/delay DSP.
That is neither total admission cost nor a baseline/current comparison.

**Candidate optimization:** first skip empty missing-note masks before scanning
all 128 pitches. Profile an unchanged-registration/unchanged-key fast path or
incrementally maintained occupancy masks. Preserve newly enabled stops,
mixtures, coupler closure, blocked-note policy, voice completion and all tails.
Validate event offsets, repeated notes and serial/grouped output before adoption.

### PERF-03 — Additional note-on and release preflight cost

**Confirmed additional event work; magnitude not isolated.** Each note-on now
refreshes the key graph before taking its admission snapshot, scans active voices,
then refreshes the graph for the new roots. A simultaneous chord repeats that
work for each MIDI event. Note-off also refreshes the graph and checks releases
across divisions. Pipe triggers acquire immutable playback leases using atomic
counters. These protect correctness, but may contribute to attack-time peaks.

Lease-counter cache-line contention is **not established as a single-instance
cause**: the normal batch coordinator prepares and recycles states while its
workers are quiescent. Cross-instance sharing needs a separate measurement.
No new steady-state per-sample lease increment was found. Move-aware lease
transfers could reduce temporary reference-count operations during attacks; any
change must retain the existing reclamation guarantees.

### PERF-04 — Reverb correctness adds callback deadline risk

**Confirmed cost change; excluded from the captured wet-zero case.**
[Convolution partitions](../Source/aeolus/dsp/convolve.h) now copy their private
input and calculate locally when a worker result is not ready for its deadline.
This avoids reading shared mutable buffers or losing output/tails. A pending
worker can finish duplicate work whose stale result is discarded, and several
local calculations can coincide at a partition boundary.

The old late-result behavior is not a safe performance baseline to restore.
Long-IR, wet-on contention tests need fallback counts and boundary callback timing.
[ReverbSwitch](../Source/aeolus/dsp/reverbswitch.cpp) bypasses inaudible wet-zero
processing after transitions; this is not the explanation for sustained breakup
in the inspected reverb-off session.

### PERF-05 — Pool exhaustion and audible breakup must be distinguished

**Confirmed behavior change, not a CPU optimization.** Whole-note admission now
accepts or rejects the complete incoming note against the logical cap and free
physical pool. Release tails consume entries until finished. With a 512-entry
pool, a held registration near 400 voices plus release tails may reject another
whole note. The former behavior could admit an incomplete collection of pipes.

Record rejection counters as well as callback deadlines. Raising the saved logical
cap cannot exceed the physical pool; `AEOLUS_MAX_VOICES=1024` must be supplied at
process startup for the matched high-polyphony configuration. Do not shorten tails
or drop mixture ranks to make a performance figure look better.

### PERF-06 — Worker presence alone does not prove parallel execution

**Integration change, unchanged by the later audit repairs.**
[DspTeam::run](../Source/aeolus/dspteam.cpp) executes
grouped jobs locally before publication if workers have not started, policy or
affinity setup failed, or their priority is below the audio thread's priority.
That protects ownership and progress, but can conceal a scheduling/configuration
failure unless startup messages and actual worker-job statistics are inspected.

Do not automatically restore FIFO-70: the recorded VIM4 USB/IRQ starvation
problem remains relevant. This audit's offline comparisons use normal scheduling.

### PERF-07 — Meter and sound changes complicate historical comparison

**Confirmed, with limited explanatory scope.** The old callback meter used a
fixed smoothing coefficient and capped its publication at 100%; the new meter
uses time-based smoothing, uncapped diagnostics and a separate recent peak.
A previous displayed percentage alone does not quantify attack deadlines or all
four cores' utilization. This does not invalidate the user's listening report.

The footage-frequency correction in
[Pipewave::getPipeFrequency](../Source/aeolus/rankwave.cpp) also intentionally
changes non-8-foot chiff, delay and post-release timing. Baseline/current audio
is therefore not expected to null exactly, and release workloads can differ.

### PERF-08 — Integration added bounded Panic polling

**Confirmed integration cost; performance magnitude unmeasured in isolation.**
Relative to the original `dc81f53` renderer, production integration added Panic
checks in `Voice::process`, `Voice::renderBatch` and the job traversal. Each check
reads the mute flag and, when needed, request/acknowledgement atomics. Grouped
rendering therefore checks more than once per voice-quantum. The recovery fields
also share storage near heartbeat counters.

These checks are unchanged by the later correctness audit. Static estimates do
not establish their elapsed cost or justify removing them. The original/integrated
comparison measures their combined effect with the other integration changes;
any optimization must preserve Panic acknowledgement and worker teardown safety.

### PERF-09 — Confirmed persistent serial fallback after repeated layout replacement

**October 21 follow-up:** fixed and regression-tested; see
[stage 0 delivery and validation](STAGE_0_RELIABILITY_2026-10-21.md). The diagnosis
below describes the audited pre-fix snapshot.

**New reproducible defect; not the fresh-launch VIM4 cause.** The tail-preserving
layout implementation retains retired divisions until setup can safely destroy
them. Their pointers remain in `_renderDivisions` after their last voices finish.
`Engine::renderBatch()` selects serial fallback whenever that list exceeds the
64-division grouped capacity; it counts empty retired divisions too. Finishing
old tails therefore does not automatically restore parallel rendering. A later
structural edit cleans up the retired entries and restores batching.

An isolated probe kept voices alive through 18 layout replacements, retaining 72
old voices. After draining them and triggering four fresh voices, the grouped-job
counter stayed unchanged across 20 callbacks. One further setup edit restored
job dispatch. The corrected probe passed in both Release and ASan/UBSan, with the
usual parameter rebinding after each layout replacement (ASan leak detection
was disabled). This deliberately
extreme setup sequence establishes the defect, not its frequency in ordinary use.
The freshly launched four-division live process cannot have accumulated it.

**Deferred fix:** compact the non-owning render list at a safe batch boundary,
without allocation or destruction in the callback; retain setup-only destruction
of retired owners. Validate active tails, layout replacement and automatic return
to parallel rendering. Do not delete retired sounding divisions to regain speed.

## Changes ruled out by the diff

Between the selected baseline and current commit, these application files are
unchanged: `voice.cpp`, `dsp/spatial.cpp`, `dsp/spatial_neon.cpp`, `dsp/delay.cpp`,
the arithmetic SIMD implementations, `dspteam.cpp`, `rtconfig.cpp`, `rtconfig.h`
and `CMakeLists.txt`. The rejected research renderers remain excluded from the
production target. The accepted grouped renderer, bounded clears, NEON paths,
original 64-sample quantum and sample-offset splitting remain present.

No new steady-render heap allocation or blocking lock was established by this
audit. Host restore, setup and model publication still have explicit ownership
transitions; testing their deadline cost is separate from an unchanged held chord.

## Matched measurements

### Local x86-64 comparison

Fresh baseline source was frozen from Git, built with the same GCC 14 Release
flags and initialized dependency versions as current. IPO was off in both local
builds. Every run used a test-owned profile, 44,100 Hz / 512, 14 simultaneous notes,
all default stops, couplers off, wet zero, a 1024-entry pool and per-voice random
streams. Both builds reached **1003 voices**. Hold duration was ten synthesized
seconds. Two passes reversed baseline/current order within each renderer mode.

| Mode | Baseline sustain p50, two runs (ms) | Current sustain p50, two runs (ms) | Baseline highest p99 / worst (ms) | Current highest p99 / worst (ms) | Change in mean of run medians |
| --- | --- | --- | --- | --- | --- |
| Serial | 5.411 / 5.371 | 5.032 / 5.055 | 6.041 / 8.282 | 5.512 / 6.106 | -6.45% |
| Grouped, no workers | 5.083 / 5.094 | 4.758 / 4.755 | 5.665 / 6.250 | 5.300 / 6.437 | -6.52% |
| Grouped, three workers | 1.402 / 1.386 | 1.330 / 1.362 | 1.710 / 1.758 | 1.790 / 1.864 | -3.44% |

All local sustain callbacks stayed below the 11.61 ms processor budget. These
columns include the worse worker p99/worst values despite the lower median.
There were 775 sustain blocks per run. The results do not isolate individual fixes or establish an optimization gain on the
VIM4. Test wavetable randomization and the intentional footage correction differ
between source versions; this is a workload/timing comparison, not an audio-null
or hardware playback test. The attack window is the first second of one chord,
not a distribution of many independent onsets. The matched native comparison
below is more relevant to ARM costs.

### Native VIM4 comparison

The native integration/current comparison used GCC 13 Release with IPO enabled
in both builds and identical dependency commits. These were isolated test-enabled
console binaries, not replacements for the production app. The current test
harness used three explicit `std::map` constructor annotations required by the native compiler; no application or benchmark logic was changed.
The benchmark rendered to memory; it did not open a physical audio device.
Workload, voice pool, random mode and reversed two-pass ordering matched the local comparison. The calling
thread was pinned to CPU 0, workers to CPUs 1–3, with normal scheduling. Both builds
reached **1003 voices** in every run. Each run contained 775 sustain callbacks.

| Mode | Integration sustain p50, two runs (ms) | Current sustain p50, two runs (ms) | Integration highest p99 / worst (ms) | Current highest p99 / worst (ms) | Sustain deadline misses, integration / current |
| --- | --- | --- | --- | --- | --- |
| Serial | 26.830 / 26.789 | 24.903 / 24.878 | 28.668 / 29.077 | 26.645 / 27.251 | 1550 / 1550 |
| Grouped, no workers | 18.062 / 18.079 | 16.580 / 16.553 | 19.698 / 20.167 | 18.143 / 18.578 | 1550 / 1550 |
| Grouped, three workers | 5.436 / 5.444 | 5.281 / 5.276 | 5.913 / 6.154 | 5.752 / 5.905 | 0 / 0 |

Current sustain medians were approximately 7.2%, 8.3% and 3.0% lower respectively.
Three-worker current sustain used about **45.5% of the 11.61 ms callback budget**,
consistent with the scale of the remembered performance. Approximately 75% of
grouped jobs actually ran on workers. The current serial workload consumed about
214% of the budget. This strongly supports the launch-mode explanation; it does
not establish a clean 1024-voice hardware playback limit.

Both three-worker versions still exceeded the budget on the initial attack:
one of 86 attack-window callbacks in each run. Current worst attack was 15.559 ms,
versus integration 15.800 ms; current highest attack p99 was 8.365 ms versus 7.656 ms.
Current worst release was 11.175 ms versus integration 10.111 ms, with no release
misses in three-worker mode. The serial and grouped-without-workers modes missed
all 86 attack callbacks and five release callbacks per run. Thus the measured
multicore sustain result does not imply glitch-free attacks. A single chord onset
per run is insufficient to characterize onset reliability.

One-second system samples during the integration/current build and matrix kept
both clusters at their reported maximum frequencies, 2208/2016 MHz, with
`performance` governors. Sampled SoC temperature ranged 43.3–62.8°C; this is
sampled evidence, not a guarantee against every thermal or scheduling transient.
The production process was no longer running at the later process check; its
timing log ended at 23:44:07 UTC with a dropped-record footer. This audit did not
close or restart it, and the exit cause was not established. Real USB/ALSA
playback, varying registrations,
live MIDI timing, GUI contention and sustained thermal operation remain separate
validation requirements.

A second native matrix compared the original accepted Claude renderer `dc81f53`
with current source, using the same settings and reversed two-pass order. Current
was re-run in this matrix rather than reusing the earlier timings. All 12 runs
completed successfully and again reached 1003 voices.

| Mode | Original sustain p50, two runs (ms) | Current sustain p50, two runs (ms) | Original highest p99 / worst (ms) | Current highest p99 / worst (ms) | Sustain deadline misses, original / current |
| --- | --- | --- | --- | --- | --- |
| Serial | 26.848 / 26.820 | 24.974 / 24.929 | 28.667 / 30.022 | 26.829 / 27.829 | 1550 / 1550 |
| Grouped, no workers | 17.982 / 17.996 | 16.576 / 16.561 | 19.886 / 20.409 | 18.149 / 18.701 | 1550 / 1550 |
| Grouped, three workers | 5.434 / 5.413 | 5.304 / 5.303 | 5.913 / 6.220 | 5.727 / 5.856 | 0 / 0 |

Current sustain medians were approximately 7.0%, 7.9% and 2.2% lower than original.
This workload exposes no large net sustain regression against either historical
source point. The combined changes and differing wavetable/tail details prevent
attribution of a small difference to any particular safety check.

In three-worker mode, both versions again missed one initial attack callback per
run. Original worst attack was 16.351 ms; current was 15.879 ms. Highest attack
p99 was 7.684 ms original versus 8.440 ms current. Worst release was 10.181 ms
original versus 11.413 ms current, with zero three-worker release misses. The
serial and grouped-without-workers modes again missed all 86 attack callbacks
and five release callbacks per run. These data retain the higher current attack
p99 and release peak despite its lower sustain times.

Before/after samples for every run in this second matrix showed performance
governors and 2208/2016 MHz; sampled SoC temperature was 47.8–54.0°C. This matrix
had before/after sampling, not continuous frequency tracing. Its first launch
attempt also failed on missing `xvfb-run`; the completed matrix used the existing
authenticated desktop. No failed launch is counted as a benchmark result.

Across both native matrices, **24 benchmark runs completed**. Final hash checks
confirmed all **91 checked organ/preference files unchanged**, with no new files
in those normal Aeolus data/settings locations. All fixtures stayed in test-owned
build directories.

The native current-source CTest run passed **2/2 tests in 31.11 seconds** on the
authenticated desktop. An earlier launch attempt failed because `xvfb-run` is not
installed; the successful run used the existing desktop, without installing
packages or changing desktop settings. These tests include deterministic audio
regressions, but are not an acoustic listening test.

## October 21 live follow-up

The [stage 0 investigation](STAGE_0_RELIABILITY_2026-10-21.md) verified three
workers and direct FiiO output after a user reboot restored the missing DAC.
The user heard attack breakup around 1000 voices under conservative governors,
then reported no breakup after applying performance governors. The latter capture
peaked at 1024 voices with median active processing around 5.8 ms; some blocks
still exceeded the processor budget. This short playing result supports the
configuration diagnosis and does not certify clean maximum polyphony or cold-start
reliability. The corrected VIM4 test launcher restores the explicit worker/pool
settings; governor persistence remains separate planned profile work.

## Recommended next session

Close any existing app normally, then run the existing optimized production build
through the multicore launcher from the VIM4 checkout:

```sh
cd /home/pwrmon/Downloads/aeolus_plugin-master/aeolus_plugin
AEOLUS_BIN="$PWD/build/vim4-lto/Aeolus_artefacts/Release/Standalone/Aeolus" \
AEOLUS_DSP_WORKERS=3 AEOLUS_MAX_VOICES=1024 \
AEOLUS_AUDIO_RT_PRIORITY=0 AEOLUS_AUDIO_CPUS=0 AEOLUS_DSP_WORKER_CPUS=1-3 \
AEOLUS_DSP_GROUP_SIZE=8 AEOLUS_DSP_BATCH_QUANTA=32 \
tools/vim4/aeolus-multicore.sh
```

This command was **not applied to the running app overnight**. It restores the
renderer/pool configuration and explicit core placement without requesting FIFO.
Verify startup policy success and worker job execution, then replay the same
registration through the FiiO device at 44.1 kHz / 512. Capture uncapped p50/p99/worst,
misses/xruns, rejected notes, actual voice count and releases. First compare the
original troublesome chord before testing maximum capacity or changing reverb.

If attacks still break up, profile many repeated onsets and attribute graph/admission,
ring initialization, convolution boundaries and worker wait separately. Retain the
memory/lifetime fixes. Implement PERF-09 independently with a permanent regression
test; it is a confirmed defect, but not evidence for changing the live machine's
settings or weakening voice-tail ownership tonight.

## Review and retained evidence

Independent Claude review **20261020T234443Z-5ce04088b0** completed on the current
source snapshot with nine observations. Codex verified their code paths, separated
unchanged configuration risks from new work, and excluded the wet-zero reverb and
single-instance lease-contention explanations where unsupported. Claude did not
build, benchmark or listen. Its suggestion of a 10–20% serial loss was an estimate,
not a measured result; the timing tables take precedence.

A second source-snapshot review, **20261021T001302Z-19d86ab0dd**, compared against
the original accepted renderer and identified four integration/control overhead
risks. Its interpretation of lower current callback times as a slowdown was
incorrect; the recorded measurements have the opposite sign. Neither review
is a substitute for the native timings.

Focused review **20261021T003530Z-ff1a3d965e** independently confirmed PERF-09
and the need to preserve batch-index stability, pointer order and setup-only
destruction in a future compaction fix. Codex additionally reproduced that
behavior in the isolated Release and ASan/UBSan probes; Claude performed static
inspection only.

Retained evidence includes `local-*.log`, native `[01]-*.log`,
`original-comparison-*.log` with before/after conditions, `native-thermal.csv`,
`final-preservation-check.json`, and `retired-layout-probe-fixed-{release,asan}.log`.
Both native old snapshots emitted a GCC LTO warning at their old atomic
pipe-trigger path; current emitted none. Old snapshots were used only for isolated valid-input
benchmarks and were never installed as production applications.

Local source snapshots, logs and the validated reviews are under ignored
`build/vim4-regression/` and `build/claude-reviews/`. Native source snapshots,
builds and profiles are under the VIM4 checkout's ignored
`build/vim4-code-audit/`. Test-enabled binaries are for isolated regression work,
not production distribution. No production app was stopped, restarted or
reconfigured by this audit; the previously running process had exited by the final process check. No notes were
injected into physical MIDI/audio devices.
