# Multicore voice rendering and real-time scheduling

Updated 2026-10-17. Branch `claude/multicore-voice-rendering`.

Goal: raise the sustainable pipe-voice count on the Khadas VIM4 (approximately
450 voices clean at 44,100 Hz / 512 frames, reverb off, single-threaded) toward
roughly 900, without changing the sound.

**Status:** implemented and tested on an x86-64 development host only.
**Nothing here has been measured on the VIM4 yet.** Every new behavior is
opt-in: with none of the environment variables below set, the application
renders exactly as before, using the original serial renderer, scheduling and
512-voice pool. Two always-on changes (bounded delay-ring clears and the
allocation fixes) are proven output-identical by tests; see
[Always-on changes](#always-on-changes).

This work has three parts:

1. **Real-time foundation:** optional `SCHED_FIFO` priority and CPU affinity
   for the audio thread and DSP workers.
2. **Attack/release cost and audio-thread allocations:** voices no longer clear
   2 × 44,100 floats when triggered and recycled. Two audio-thread allocations
   are removed, and the voice pool size is configurable.
3. **Batch renderer:** voices are rendered whole for several 64-sample quanta at
   once, in groups, by the audio thread plus persistent worker threads, with
   deterministic summation.

## Quick start on the VIM4

Build natively on the board as usual (Release, IPO recommended), then launch
with the helper script, which sets the variables below, checks the system and
then runs Aeolus. It does not change any system setting:

```sh
tools/vim4/aeolus-multicore.sh                 # uses build/…/Standalone/Aeolus
AEOLUS_BIN=/path/to/Standalone/Aeolus tools/vim4/aeolus-multicore.sh
```

Defaults: audio thread on CPU 0 at `SCHED_FIFO` 70, three workers on CPUs 1–3
at the same priority, 1,024-voice pool. Override any variable by exporting it
before running the script.

On startup Aeolus writes lines like these to standard error:

```text
Aeolus batch renderer: 3 workers, group size 8, voice pool 1024; worker 1: SCHED_FIFO 70, CPUs 1; …
Aeolus audio thread: SCHED_FIFO 70, CPUs 0
```

The audio-thread line appears once audio starts. If it says `refused`, the
priority was not granted (check `ulimit -r`, which must be at least the
requested priority). In that case the renderer still works, but timing is not
protected; do not use such a run for capacity measurements. Confirm with:

```sh
ps -L -o tid,cls,rtprio,psr,comm -p "$(pidof Aeolus)"
```

`aeolus-dsp-N` threads and the audio thread should show `FF` / `70` on the
expected CPUs. On exit, a summary line reports batches, the share of jobs done by
workers and how long the audio thread waited for workers.

## Configuration reference

All variables are read once when the engine is created. Unset means original
behavior.

| Variable | Values | Effect |
| --- | --- | --- |
| `AEOLUS_AUDIO_RT_PRIORITY` | 1–99 | `SCHED_FIFO` priority for the audio callback thread (applied once on its first callback) and for DSP workers. Linux only. |
| `AEOLUS_AUDIO_CPUS` | CPU list, e.g. `0` or `0-1` | Affinity of the audio callback thread. |
| `AEOLUS_DSP_WORKERS` | 0–15 | Selects the batch renderer with this many workers in addition to the audio thread. `0` = batch renderer on the audio thread only (useful A/B control). Unset = original serial renderer. Ignored in `WITH_MULTIBUS_OUTPUT` builds. |
| `AEOLUS_DSP_WORKER_CPUS` | CPU list, e.g. `1-3` | Worker *i* is pinned to the *i*-th listed CPU (wrapping). |
| `AEOLUS_MAX_VOICES` | 16–4096 | Voice pool size (default 512). Each voice preallocates about 370 KB of delay history, so 1,024 voices need about 380 MB. |
| `AEOLUS_DSP_GROUP_SIZE` | 1–64 | Voices per batch job (default 8). Smaller groups balance better; larger groups reduce mixing passes. Changes summation grouping (rounding-level differences only). |
| `AEOLUS_DSP_BATCH_QUANTA` | 1–32 | Maximum 64-sample quanta per batch (default 32). For experiments; batches never extend past a MIDI event or the end of the current callback. |
| `AEOLUS_PER_VOICE_RANDOM` | 0/1 | Per-voice noise streams in the serial renderer (always on in the batch renderer). Used by the tests as the serial reference. |

CPU numbers are board-specific. On the tested VIM4 kernel, CPUs 0–3 are the
Cortex-A73 performance cluster (2.208 GHz) and CPUs 4–7 the A53 efficiency
cluster; verify with `cat /sys/devices/system/cpu/cpu*/cpu_capacity` on other
systems.

## System preparation (not done by the application)

These are system decisions for the board owner. The script reports their
current state but does not change them.

- **Real-time limit.** The VIM4 already has `pwrmon - rtprio 70` in
  `/etc/security/limits.d/90-aeolus.conf`, according to the handoff notes. Without it,
  `SCHED_FIFO` is refused. Earlier VIM4 checks found the direct ALSA audio
  thread running as `TS` (ordinary time-sharing). JUCE does not request real-time
  scheduling for it, which is why this option exists.
- **Cooling.** Four A73 cores at sustained full load will throttle with a poor
  thermal interface, and the voice capacity falls with the clock. Fix the heatsink
  contact or add a fan before capacity runs, and log
  `/sys/class/thermal/thermal_zone*/temp` and
  `/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq` during tests.
- **Governor.** The board boots with `conservative`, which lets the first
  callbacks of a chord run at low clock. Measurements so far used `performance`
  or confirmed 2.208 GHz. Decide on a persistent policy for the appliance.
- **Optional core isolation.** Pinned `SCHED_FIFO` threads already pre-empt
  ordinary threads on CPUs 0–3. Interrupts and other real-time threads can
  still run there. Booting with `isolcpus=1-3 nohz_full=1-3` (or a cpuset) is a
  further, separate experiment.
- **Reverb worker.** The convolution partition worker is still an ordinary
  thread without affinity. With the batch renderer occupying CPUs 0–3, it will
  mostly run on the A53 cores. Its late-result behavior is a known risk (see
  [Known issues](#known-issues-found-during-this-work)); measure reverb-on
  separately.

## Always-on changes

These apply in every build and mode, and the tests show they leave the output
identical.

### Bounded delay-ring clears (attack and release cost)

Each voice owns a 44,100-float pipe delay ring and a 44,100-float chiff
resonator ring. Previously, every note-on cleared the whole chiff ring, and every
voice returned to the pool cleared both rings: about 530 KB of memory writes per
note. A 450-voice chord therefore cleared about 80 MB at note-on and about 160 MB
as its tails ended, inside one or two callbacks. That is cost the sustained-chord
captures did not measure.

Now `trigger()` resets the write position and clears only the slots that can be
read before they are overwritten (`DelayLine::resetForReadIndex`):

- pipe delay: slots `0 … delay` (nearest reads after each write);
- chiff resonator: slots `0 … floor(delay) + 1` (interpolated reads before each
  write, with feedback).

Writes move backwards from slot 0, so every other slot is written before a read
can reach it. `Voice::reset()` no longer clears these rings. Delays that do not
fit fall back to the full clear. The spatial delay and filter states are reset as
before.

Evidence:
- `delayBoundedResetTests` compares full and bounded resets for both access
  patterns, from histories containing NaNs, over several ring revolutions and a
  range of ring sizes and delays.
- An engine-level test renders a script with a 128-voice pool (so voices are
  recycled with old audio in their rings) using full and bounded clears, and
  requires identical output.

`-DAEOLUS_BOUNDED_VOICE_CLEAR=OFF` restores the full clears for device A/B
runs. In test builds, `AEOLUS_BENCHMARK_FULL_VOICE_CLEAR=1` does the same for
`--benchmark-engine` without rebuilding.

On the x86 host at 710 voices (10 notes, all stops), the worst note-on block fell
from 11.9–13.9 ms to 8.0–8.3 ms across two A/B pairs. That is roughly the cost of
clearing about 125 MB. The VIM4's memory is slower, so the effect there is
expected to be larger, but it has not been measured.

The Codex worktree (`codex/live-balanced-spatial`, uncommitted at the time of
writing) contains an equivalent bounded chiff clear for note-on. This branch
also removes the recycle-time clears, which that work does not.

### Audio-thread allocations removed

Both were found by counting heap operations on every thread during
`processBlock` in the new tests:

- **Spatial delay growth.** `SpatialSource::recalculate()` resizes its delay
  line when a voice is triggered. The pool builds its voices by copying one
  prototype, and copying a `std::vector` does not keep reserved capacity. So
  every voice started with about 20 samples of capacity and reallocated, on the
  audio thread, the first time it played a distant source. The default organ's
  16′ and 32′ pedal pipes need up to about 2,440 samples. The pool now reserves
  4,096 samples on every voice after construction. Larger geometries, which
  would need extreme custom `fn`/`fd` values, still resize as before.
- **Note events.** `Engine::noteOn`/`noteOff` allocated a `std::vector<char>`
  per event. They now use fixed storage for up to 64 divisions (layouts allow 10).

A third source is in JUCE: `MidiKeyboardState` grows its listener-iterator
storage on first use per processor. It is a one-time warm-up and is not changed
here; the tests warm it up before counting.

With these changes, every render mode in the tests performs **zero** heap
operations, on any thread, during `processBlock`.

### Engine interfaces added

- Optional per-voice noise streams. Pipe instability and chiff noise
  previously drew from two process-wide generators.
- `Division::controlStep()`, factored out of `Division::process()` with
  unchanged serial behavior.
- `Interpolator::getFraction()`, `Pipewave::getReleaseQuanta()`.

## Batch renderer design

Selected by `AEOLUS_DSP_WORKERS`. Stereo engine only.

When `Engine::process()` needs a new 64-sample quantum and none is buffered, it
renders a **batch** of quanta. The batch length is the exact number of quanta
the current `process()` call will consume. It is computed by replaying the
interpolator's arithmetic (`countSubFramesNeeded`), so a batch never runs ahead
of a MIDI event: the processor still renders up to each event's sample offset
and applies events between `process()` calls. The batch is capped at 32 quanta
and shortened as described below. Each batch then runs these phases:

1. **Control, serial.** For each quantum *q*, in the original division order,
   each division runs its control step: key aggregation, releases for released
   keys or stops, and triggers. Releases are *scheduled* for quantum *q* rather
   than applied, and new voices start at *q*. Each voice's release is applied
   immediately before rendering that quantum, exactly where the serial renderer
   applies it.
2. **Finish prediction.** In the serial renderer, a finished voice leaves its
   division immediately, and later control steps observe this: a stop with no
   remaining voice for a held key is retriggered (see the coupler issue below).
   `Voice::predictFinishQuantum()` derives, from the envelope state, release
   length and post-release counter, the quantum in which each voice will finish.
   It mirrors `Pipewave::play()` and `Voice::process()`. The batch ends at that
   quantum, so finished voices are reclaimed at the same point as in the serial
   renderer. Releases scheduled during phase 1 can shorten the batch further.
3. **Render, parallel.** Each division's voice list is cut into jobs of up to
   `AEOLUS_DSP_GROUP_SIZE` consecutive voices. Each job renders its voices for
   the whole batch into a private stereo buffer, adding them in list order, and
   records the quanta in which any of them sounded. Rendering one voice for many
   quanta keeps its state in cache. Jobs only touch their own voices and
   buffers. Pipe wavetables are shared and read-only.
4. **Mix, serial.** For each quantum, job buffers are added into the division
   buffer in job order. Tremulant, gain and swell modulation, division meters
   and the master sum then run exactly as in `processSubFrame()`.
5. **Reclaim, serial.** Finished voices return to the pool. The rest clear their
   batch scheduling.

### Worker team (`DspTeam`)

- Workers are persistent threads named `aeolus-dsp-N`. They are created when the
  engine is constructed, take the configured priority and affinity, and set the
  same flush-to-zero mode as the audio thread (`ScopedNoDenormals`), so results
  do not depend on which thread renders a job.
- `run()` publishes the batch in one atomic word: generation, job count and
  next job. Workers and the audio thread claim jobs with compare-and-swap, so a
  late worker can never claim a job from a batch it has not seen.
- Sleeping workers are woken with POSIX semaphores (no locks on the audio
  thread). Elsewhere, the existing mutex-based semaphore is used.
- The audio thread renders jobs too. Once every job is claimed, it waits only
  for jobs **already running** on workers, never for unstarted work, so a
  descheduled worker cannot hold up jobs it has not claimed. The wait spins for
  50 µs, then yields the CPU, so a worker preempted on the audio thread's CPU can
  still run.
- No allocation happens per batch. Job descriptors and buffers are preallocated
  at construction (about 3 MB for 1,024 voices).

**Waiting policy.** This design deliberately lets the callback wait for
in-flight jobs. The wait is bounded by one job's duration (about 10–20 µs per
voice for a full batch) **only if workers are not preempted**. That requires
`SCHED_FIFO` workers on CPUs not shared with other real-time load. Earlier
offline probes, which never let the callback wait, lost more to copying voice
state than they gained from parallelism (see
[ARM_OPTIMIZATION.md](ARM_OPTIMIZATION.md#multicore-scaling-experiment)). This
conflicts with the feature document's guidance against unbounded waits, so it
stays opt-in. On every run, check the wait statistics and the timing captures
before relying on it.

## Differences from the serial renderer

The tests establish these properties:

- **Exact equivalence.** With group size 1, the batch renderer is
  **sample-for-sample identical** to the serial renderer with per-voice noise
  streams, with 0 and 3 workers, at 44.1 and 48 kHz. The fixture covers MIDI at
  odd offsets, blocks of 1–1,024 frames, couplers from later to earlier
  divisions, tremulant, swell, a short note, a retrigger and stop changes while
  keys are held.
- **Thread-count independence.** With the default group size of 8, output is
  identical for 0, 1 and 3 workers.

The remaining differences are intentional or edge cases:

1. **Noise streams.** Each voice draws pipe-instability and chiff noise from
   its own generators, seeded from the trigger sequence (SplitMix64). The
   statistics are unchanged, but the samples differ from the original shared
   generators. The original shared generators are also seeded from the clock, so
   the original output was never repeatable anyway. With per-voice streams, a
   render is repeatable for a given MIDI and registration sequence.
2. **Summation grouping.** Voices are summed within groups first, then the
   groups are summed. This is a floating-point rounding difference only: the
   peak difference is 3.6 × 10⁻⁷ at a signal peak of about 1.5 in the test
   fixture. Use group size 1 to remove it, at some cost.
3. **Pool exhaustion.** When the pool is full, a voice finishing in quantum *q*
   becomes available at the end of the batch rather than to divisions later in
   the same quantum. This only matters when notes are already being dropped. Set
   `AEOLUS_MAX_VOICES` above the target load.
4. **Stop and parameter changes from the UI** take effect at a batch boundary,
   at most one callback later. Their arrival time relative to the audio stream
   was already arbitrary.
5. **Retuning while notes sound**, which already stops the affected voices
   immediately, may retrigger coupled voices one batch later.

## Verification performed

On an AMD Ryzen 9 7900X (x86-64), Debian GCC 14.2, Release build with tests,
headless (`xvfb-run`):

- The full `aeolus_tests` suite passes. That includes the existing regressions
  plus `delayBoundedResetTests` and `batchRendererTests`, covering the exact
  equivalences above, bounded vs full clears with voice reuse, no leftover
  rendered quanta, no stuck voices, zero heap operations during processing, and
  pool exhaustion with 3 workers.
- ThreadSanitizer: a whole-program test build (`-fsanitize=thread -O1`, JUCE
  included) passes the same suite, including the 3-worker renders. It reported
  48 warnings, all in pre-existing code and none in the worker team or batch
  renderer:
  - 37 JUCE `MessageManager::Lock` "destroy of a locked mutex" reports in the
    engine destructor;
  - 11 races in `EngineGlobal::updateStops`, whose `WaitableEvent` is
    destroyed while a pool thread may still be signalling it.

  ThreadSanitizer only checks the interleavings that actually occurred.
- Host benchmark: `aeolus_tests --benchmark-engine 20 3` with
  `AEOLUS_MAX_VOICES=2048`, 20 notes, all stops, 1,435 voices, 44.1 kHz / 512
  frames, offline (no audio device), no affinity or priority. This shows
  scaling on x86 only; it is **not** VIM4 evidence:

| Renderer | Attack p50 / p99 / max (ms) | Sustain p50 / p99 / max (ms) |
| --- | --- | --- |
| Serial (original) | 9.09 / 14.42 / 17.11 | 8.12 / 8.74 / 8.98 |
| Batch, 0 workers | 8.69 / 14.21 / 16.36 | 7.75 / 8.31 / 8.43 |
| Batch, 1 worker | 4.42 / 7.41 / 9.66 | 3.83 / 4.23 / 4.34 |
| Batch, 2 workers | 3.03 / 4.79 / 7.63 | 2.68 / 3.34 / 4.08 |
| Batch, 3 workers | 2.60 / 3.79 / 6.38 | 2.09 / 2.53 / 2.67 |

With 3 workers, sustain is 3.9× faster than serial. The longest wait for
workers was 75 µs. Not verified: VIM4 timing, live audio-device behavior,
listening tests, thermal behavior, reverb-on load and the A53 cores.

## VIM4 measurement procedure

Use the existing profile build and telemetry (see
[processor telemetry](ARM_OPTIMIZATION.md#processor-telemetry)) with test-owned
copies of the organ data, as before.

1. **Offline engine benchmark** (no audio device; quick to compare). Build with
   `-DAEOLUS_BUILD_TESTS=ON -DAEOLUS_ENABLE_IPO=ON`, then:

   ```sh
   export AEOLUS_TEST_DATA_DIRECTORY=$PWD/build/test-data   # test-owned
   export AEOLUS_MAX_VOICES=1024 AEOLUS_AUDIO_RT_PRIORITY=70 AEOLUS_AUDIO_CPUS=0
   for w in serial 0 1 2 3; do
     if [ $w = serial ]; then unset AEOLUS_DSP_WORKERS; else export AEOLUS_DSP_WORKERS=$w; fi
     AEOLUS_DSP_WORKER_CPUS=1-3 xvfb-run -a build/aeolus_tests --benchmark-engine 10 8
   done
   ```

   Vary the note count (first argument) to reach about 450, 700 and 900
   voices. Repeat in reverse order and log the clock and temperature. Also
   compare `AEOLUS_BENCHMARK_FULL_VOICE_CLEAR=1` against unset for the
   attack/release effect.
2. **Live A/B/A** with the profile build, direct FiiO ALSA output at
   44,100 / 512: serial reference, batch with 3 workers, then serial reference
   again, at the same registrations. Include simultaneous-attack chords and
   releases, not only sustained chords. Record p50/p99/worst processor time,
   start intervals, budget exceedances, the renderer's exit summary and
   listening results. The native xrun counter alone is not a dropout detector
   on this device.
3. **Capacity search.** Raise the voice count until breakup, and confirm the
   highest clean count with a two-minute run, as was done for the 450-voice
   result.

Acceptance: clean listening, and processor p99 below the 11.61 ms period with
margin, at the target voice count. Repeat after 10 minutes of sustained load to
expose throttling.

## Known issues found during this work

- **Coupler onset delay (pre-existing, confirmed; not fixed).** A division
  coupled from a division that comes *later* in the organ order (Pedal →
  manuals, II → III, I → II/III in the default organ) sees the source's key
  state one quantum late. Its coupled voices are therefore released silently at
  once and retriggered only when those dead voices are reclaimed. Measured: a
  Manual I stop played through the pedal coupler starts about **54 ms** later
  than the same note played directly (onset at sample 4,023 vs 1,656). Fixing
  it changes musical timing, so it needs a decision. Both renderers currently
  reproduce it exactly.
- **Wavetable sets alternate per `prepareToPlay()` (pre-existing).** Each rank
  switches to its other pipe set, and the two sets are generated with
  independent random detuning. A device restart therefore changes the
  wavetables. The tests prepare twice so that every render uses the same set.
  Keep this in mind when comparing renders across processor instances or
  restarts.
- **Reverb partition worker (pre-existing risk, unchanged).** A late
  convolution partition result is silently skipped. See the risk notes from the
  earlier analysis.

## Files

- `Source/aeolus/rtconfig.*` – configuration, thread policy and status reporting.
- `Source/aeolus/dspteam.*` – worker team.
- `Source/aeolus/engine.*` – batch renderer (`renderBatch`, `runBatchJob`,
  `countSubFramesNeeded`), allocation-free note events, status logging.
- `Source/aeolus/voice.*`, `division.*` – deferred releases, batch rendering,
  finish prediction, per-voice noise streams, bounded clears.
- `Source/aeolus/dsp/delay.*`, `chiff.*`, `spatial.*`, `rankwave.*`,
  `interpolator.h` – supporting changes.
- `tests/AudioTests.cpp` – equivalence, allocation and reuse tests;
  `--benchmark-engine`.
- `tools/vim4/aeolus-multicore.sh` – launcher.
