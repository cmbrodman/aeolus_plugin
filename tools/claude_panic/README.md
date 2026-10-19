# Independent panic and watchdog test

This is an isolated prototype based on Claude commit
`dc81f53a941c984fac4148c5922e8501b80070c3`. It does not replace the normal ARM
installation or merge the two development branches.

`engine.patch` applies to that exact commit. Copy `paniccontrol.h` and
`paniccontrol.cpp` into its `Source/aeolus/` directory before building. The
patch contains the engine, UI and regression-test changes. Use the same Linux
test-enabled profiling build as the multicore experiment, with the separate
preferences entry point documented in `docs/ARM_OPTIMIZATION.md`.

The test bundle contains `run-test.py` (from `run_test.py` here), the three
Python tools from the parent directory, and private `data/` and `config/`
directories. The launcher creates a private Unix datagram endpoint and starts
the independent safety processes. Use the launcher to get the protected test;
running the binary directly does not start the independent panel or watchdog.
Python 3 with GTK 3/PyGObject is required for the panel; it is available on the
tested VIM4. No host scheduling limits or governors are changed.

## Behavior

- Audio and three workers retain FIFO 70 on performance cores 0–3.
- A receiver thread uses FIFO 1 on efficiency core 4. The separate panic panel
  uses FIFO 1 on core 5; the headless watchdog uses cores 4, 6 and 7 at FIFO 1.
- Pressing PANIC sends an atomic request independently of the main window.
  Existing in-app panic buttons use that request too. Workers check between
  original 64-sample steps and skip the remaining voice work. The audio owner
  waits for every job to finish before clearing voice lists and acknowledging.
- Panic latches digital silence, including old reverb output. Notes entered
  while muted are discarded. **Resume audio** explicitly stops/reprepares the
  audio device outside processing, clearing old effect history before playback.
  It retains the application, registration, selected IR and audio setup.
- The acknowledgement window is one second. If the nonce-correlated reply is
  missing, the controller retries the same request ID and allows one additional
  window. The receiver deduplicates its last 32 IDs. A confirmed acknowledgement
  remains valid if another controller already resumed audio. Unacknowledged
  panic escalates to SIGTERM, then SIGKILL three seconds later, using PID
  descriptors. There is **no automatic application restart**.
- The protected launcher requires `HEALTH 2` from the independent control
  receiver. The audio callback increments lock-free started/completed counters;
  the receiver reports these with device state (0 closed, 1 active, 2 preparing)
  and a separate UI counter. Five seconds without callback completion initiates
  recovery. UI-only staleness is diagnostic. The safety loop never opens the CSV;
  incomplete logging cannot masquerade as failed audio. Deliberate closure is
  exempt only with a fresh response and no in-flight callback. Preparation gets
  a 30-second allowance, including changes outside explicit Resume. Successful
  preparation starts a fresh callback window. A stale prepare flag never
  overrides observed, continuing callback completion.
- Control logs, metadata and helper stdout/stderr live in a private `/dev/shm`
  directory. The launcher publishes symlinks under `logs/`, then persists those
  files after the app exits. App stdout/stderr also use RAM. Safety helpers have
  separate sessions and keep running independently of the launching terminal.
  Arming does not wait for the CSV; its first settings row is optional startup
  metadata. Standalone generic CSV supervision remains for older experiments;
  panic-socket supervision always requires the independent protocol.
- Profiling formatting, file creation, writes and close now run on `aeolus-log`,
  draining the same bounded SPSC queue about every 200 ms. The writer uses
  exclusive creation and ordinary OS writes, without the old periodic `fsync`.
  Audio publication never waits for log writes. Teardown gives the writer a
  100 ms completion window and otherwise leaves it to finish with independently
  owned state. Stop notification uses the writer's mutex to prevent a lost wake.
  A blocked disk or process exit can leave an incomplete capture/footer; this
  affects telemetry, not notes. No crash/power-loss durability is promised.

This handles audio stalls while the OS can schedule the independent control.
It cannot recover a kernel freeze. UI-only stalls are reported separately;
the independent panel remains the manual recovery path. This prototype is for
controlled audio testing, with organ/model editing outside the test.
No ordinary deadline miss drops notes or
reduces sound quality. Truncation/muting happens only on explicit or watchdog
panic recovery.

## Checks

The Python subprocess tests exercise process identity, stale acknowledgements,
successful acknowledgement without application exit, monitor-only behavior,
stale telemetry and the SIGTERM/SIGKILL sequence against disposable children.
The VIM4 also runs the real FIFO-priority-demotion test using an owned child.
The heartbeat tests cover progressing audio with frozen UI/CSV, deliberately
inactive audio, responding control with stalled audio, missing protocol support
and a stuck reprepare transition. Unsupported endpoints never arm this mode.

The C++ regression patch requests panic during a deliberately delayed batch
job at 44.1/48 kHz, with 0/3 workers and dense registrations. It checks completion,
voice reclamation, zero wet/dry output, queued-note suppression, explicit resume,
and scoped C++ allocations on the audio coordinator. These checks do not cover
every allocator API or every GUI/model-edit interleaving. Silent device startup,
real process-freeze recovery and live listening are separate hardware checks.
The background-writer tests stall a test-owned writer, verify GUI timer progress,
bounded publication/overflow and teardown, and allow it to finish after its owner
is destroyed. They also check existing-file preservation and opening errors.

On 2026-10-18, the full patched audio/MIDI regression suite passed locally and
on the VIM4. All nine Python subprocess tests passed on the VIM4, including
actual FIFO demotion. Silent device checks passed at 44100/512: healthy panic
acknowledgement, explicit audio Resume and normal close; then SIGSTOP of the
isolated application, watchdog panic timeout, SIGTERM/SIGKILL and helper exit.
No MIDI was sent in those device checks. The original 88 organ/model files,
profiling preferences and conservative governors were preserved.

In the subsequent live 450-voice chord test at 44100/512, the independent
panel logged panic acknowledgement about 59.7 ms after its request. This is
control request-to-observed-acknowledgement time, not a measured acoustic
silencing latency. The listener confirmed that the sound stopped promptly and
explicit Resume restored playing. The application remained running. This
single successful live test does not establish a worst-case recovery bound
under every overload; the separate frozen-process check exercises escalation.

```sh
python3 -m unittest discover -s tests -p 'test_*panic*.py' -v
python3 tests/test_vim4_test_watchdog.py -v
python3 tests/test_audio_health_watchdog.py -v
```

The later logger/HEALTH 2 changes passed the full patched audio/MIDI suite locally
and on the VIM4. All 25 Python safety/diagnostic tests passed on the VIM4, including
an actual FIFO test, lost acknowledgement retries, restart transitions and a FIFO
used as a blocked CSV path that the safety process must never open.

The final silent hardware check delayed the real logger's first write by 30 s.
Safety armed with the CSV empty. Across a 6.55 s observation, completed callbacks
advanced from 481 to 1046 and GUI ticks from 21 to 151, with no automatic panic.
The writer ran at ordinary priority on CPUs 4–7; its capture ended with zero drops.
Repeated panic IDs did not increment the request epoch. Panic, explicit Resume
and normal close passed at 44100/512. Real SIGSTOP recovery again exercised
panic timeout, SIGTERM/SIGKILL and helper cleanup. These are controlled recovery
checks, not a new sustainable-polyphony or worst-case acoustic-latency result.

The VIM4 bundle is `claude-health-final-test-20261018` under the existing test lab.
Its native output is `native-claude-health-final-20261018`; local build/evidence
is under `build/profiles/vim4-claude-health-2026-10-18/`.

## Hang investigation

`capture_hang.py` observes only the specified owned executable and records
thread scheduling, CPU accounting and wait channels. It can capture bounded GDB
stacks after telemetry stops. Use RAM-backed output and timing files on the VIM4;
debugger attachment pauses audio and invalidates performance measurements for
that interval. The tool skips automatic attachment when a thread is in
uninterruptible kernel I/O or insufficient time remains before the watchdog.
On debugger timeout it sends SIGCONT through the checked target's PID descriptor
so an interrupted attach cannot leave the application group-stopped.
`--no-debugger` collects thread state without attaching or pausing the target.

The first overload diagnostic run reached 973 voices and recorded the UI thread
waiting in `jbd2_log_wait_commit`, with audio/worker threads still runnable.
Stack attachment timed out, left the target stopped, and watchdog recovery
terminated it. This run is confounded by the initial diagnostic tool's missing
resume cleanup; it does not establish a renderer deadlock. That cleanup is now
covered by a disposable-child test that actually stops the target.

Source inspection also found profiler CSV flushes on the GUI timer every 200 ms;
JUCE's POSIX `FileOutputStream::flushInternal()` calls `fsync()`. That is a
source of UI stalls. A subsequent disk-logging run captured main-thread syscall
82 (`fsync` on this ARM64 system) with its file descriptor pointing to the
profiling CSV, confirming that specific operation was blocking the UI. Audio
callbacks continued during the observed UI I/O waits. A RAM-logging replay also
reached the 1024-voice pool limit and returned to zero without recovery, but the
two replays had different voice traces and are not a matched causal benchmark.
The stale-CSV watchdog must not be treated as an independent audio callback
heartbeat. See [the investigation record](../../docs/HANG_INVESTIGATION_2026-10-18.md)
for evidence and remaining uncertainties.

### MIDI loss diagnostic overlay

`instrument_midi.py SOURCE JUCE` instruments a **fresh copy** of the validated
HEALTH 2 source; it must not be run in the normal checkout or twice in one copy.
It adds process-lifetime lock-free counters and generates private JUCE module
wrappers. The JUCE submodule is not modified. Original notices are retained.
This is an investigative build, not a performance benchmark or a MIDI fix.

The independent receiver retains HEALTH 2/PANIC behavior and adds read-only queries:

- `DIAG` returns `DIAG 1` followed by on/off counts for five stages, in order:
  AudioProcessorPlayer device callback, collector enqueue, collector dequeue,
  processor dispatch, engine MIDI dispatch. Device callback means delivery from
  JUCE's MIDI backend, not proof of electrical/USB/ALSA hardware receipt.
- Remaining values: collector resets, age-pruned note events, window-pruned note
  events, engine noteOff calls, accepted division noteOff calls, matching voices
  visited by note release, Voice::release calls, successful pool triggers, pool
  allocation failures, pool returns, GUI note-queue overflow, age-pruned note-offs,
  window-pruned note-offs, reset-discarded note-offs.
- `NOTE CHANNEL NOTE` returns `NOTE 1 CHANNEL NOTE` followed by those five pairs of
  per-key stage counters. CHANNEL is 1–16 and NOTE is 0–127. Zero-velocity note-ons
  count as note-offs. Engine release calls include non-device paths; division and
  voice counts can fan out through stops/couplers, so they are not event balances.

Counters are never cleared, including during audio resets and Panic. Snapshots
sample fields independently and may show transient disagreements. Pool failures
report existing behavior; the diagnostic does not change allocation/retry policy.
Pruning counters observe existing JUCE behavior without changing its policy.
Hooks use fixed atomics and raw short-message parsing; they add no file I/O,
locks, allocations or event signaling to the audio callback. Their overhead makes
this unsuitable for measuring the final sustainable voice limit.

`build_midi.py --plan PLAN --source SOURCE --output OUTPUT` recompiles the five
instrumented project units, regression tests and two private JUCE modules. It
links them ahead of the validated HEALTH 2 archive/unchanged objects. No class
layout changes are introduced; commands are saved in OUTPUT. Plans are specific
to the owned local/VIM4 build directories, not a portable release build recipe.

`capture_midi.py` requests only DIAG, NOTE, HEALTH and STATUS. It writes to a new
private directory below `/dev/shm`, polls aggregate counters about four times per
second and snapshots channel 1 plus a rotating channel every five seconds.
Per-key coverage across all other channels takes up to 80 seconds. It runs until
the owned app exits by default; `--duration` can bound a scripted check. With
`--archive DIRECTORY`, it copies RAM logs there after observing target exit.
It never sends Panic, Resume, signals or MIDI, and never attaches a debugger.

Validation includes audio/MIDI regression renders at 44.1/48 kHz, zero-velocity
note-off handling, real collector enqueue/dequeue and deliberate age/window/reset
note-off discards. `tests/test_capture_midi.py` exercises an actual Unix datagram
endpoint, per-key correlation in the presence of unrelated replies, and read-only
commands. Passing these checks does not establish the cause of the live overload.

`test_usb_irq.py` is a Linux/VIM4-only, manually sudo-launched host experiment.
Its read-only `--check` validates the exact test PID/executable and xHCI IRQ. The
active mode moves only that IRQ to one efficiency CPU (default 4), verifies its
effective affinity, and restores the original allowed mask on target exit,
handled SIGINT/SIGTERM/SIGHUP, or timeout (default 900 seconds). Keep its terminal
open. SIGKILL, power loss or kernel failure cannot execute its cleanup; verify
actual affinity afterward. It makes no persistent boot, priority, governor or
RT-bandwidth changes. The standalone application does not automatically apply it.
