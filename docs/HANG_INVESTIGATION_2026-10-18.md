# VIM4 hang investigation — October 18, 2026

## Confirmed finding

**The profiling build can freeze its UI while synchronizing the callback CSV to
storage. Audio can continue processing throughout that UI stall.** This is a
confirmed contributor, not proof that all previously reported lockups share one
cause. No permanent DSP worker deadlock was established in this investigation.

The isolated test uses Claude commit
`dc81f53a941c984fac4148c5922e8501b80070c3` with the previously validated panic
overlay, at 44100 Hz / 512 samples. No organ timbre, rendering math, voice limit,
system scheduling limits or governor was changed for these experiments.

## Evidence

- `CallbackProfiler::timerCallback()` drains callback samples on JUCE's message
  thread every 200 ms and calls `FileOutputStream::flush()`.
  JUCE's POSIX `flushInternal()` calls `fsync()`.
- During the disk-control session, five direct `/proc/<pid>/syscall` observations
  showed main-thread syscall **82**, verified as `__NR_fsync` in the VIM4's
  installed ARM64 headers. The descriptor was **8**, resolving to that session's
  callback timing CSV. The wait channel was `jbd2_log_wait_commit`.
- In that run, the UI was repeatedly observed in journal/page-writeback waits
  across a **4,975 ms sampled span**, while **335 audio callbacks completed**.
  Those callbacks reached 1024 voices with median duration about **16.02 ms**.
  This sampling does not prove one uninterrupted five-second syscall, but it
  establishes UI storage waits with continuing audio progress.
- Shorter sampled I/O-wait spans also included 58 completed idle callbacks over
  675 ms and 77 completed callbacks over 900 ms with a peak of 385 voices.
  The logging problem therefore is not restricted to full voice-pool load.
- With logs in `/dev/shm`, a four-round MIDI replay reached the 1024-voice pool
  limit, completed its note-offs and returned to zero voices without watchdog
  recovery. It still incurred deadline overruns. This does not demonstrate clean
  audio at 1024 voices.
- Replaying the same MIDI with disk logs also completed without permanent hang
  or watchdog escalation, while reproducing UI storage waits. Voice traces
  differed between sessions, so these runs are **not** a matched performance or
  causal A/B benchmark. Original private registration/input conditions need
  tighter control for further comparisons.

## Watchdog implication

The current prototype watches a CSV flushed by the GUI timer. A stalled UI,
logger or disk can make that telemetry stale even when audio is progressing.
Conversely, partially drained samples can make the file appear newer than the
latest callback evidence warrants. CSV age is not an independent audio heartbeat.

The release design needs callback/worker progress published through bounded,
real-time-safe state that the independent controller can read without requiring
the GUI or disk. Keep UI health, audio progress and logging health distinguishable.
The rule that buffers/voices cannot be reclaimed before worker completion still
applies. The initial investigation did not implement a replacement heartbeat;
the follow-up below does.

## Diagnostic tooling and limitations

Added [capture_hang.py](../tools/claude_panic/capture_hang.py) to record owned-process
thread states, affinities, scheduling/accounting, wait channels and main-thread
sync syscall/file-descriptor evidence. It can optionally attach GDB once, with a
bounded timeout; `--no-debugger` avoids stopping the target. Output belongs in RAM
while testing and must be archived to persistent storage afterward.

A healthy baseline GDB attachment succeeded. The first overloaded run reached
973 voices and showed the UI in a journal wait. Automatic attachment timed out,
and the initial diagnostic tool left the process group-stopped. The watchdog then
escalated through SIGTERM/SIGKILL. **That forced shutdown is confounded by the
debugger and is not evidence of a natural renderer deadlock.**

The tool now resumes its checked target via pidfd SIGCONT after debugger timeout,
skips automatic attachment during uninterruptible kernel I/O, and avoids attaching
after the watchdog time budget is consumed. A disposable-child test actually
SIGSTOPs the target and verifies timeout cleanup leaves it alive and unstopped.
The disk-control run used passive observation without debugger attachment.

Three observer subprocess tests passed locally: wrong-executable rejection,
single capture after stalled telemetry, and bounded timeout with stopped-target
cleanup. The healthy GDB baseline was verified on the VIM4. No new C++ renderer
changes or C++ build were required for this investigation.

## Claude review: leads, not established overload causes

A read-only Claude review found no definite generation/claim/completion bug in
`DspTeam`. Its output is retained with the local evidence. Findings were checked
against source rather than accepted as runtime proof:

- The coordinator waits until every claimed job finishes. FIFO `yield()` does
  not guarantee CPU time to ordinary work on its core. Whether this contributes
  to kernel/IRQ starvation here remains unproven; journal-thread affinity spans
  all eight CPUs, so it must not simply be described as pinned to core 0.
- Failed triggers from a full voice pool can repeat list scans during control
  steps. Measure this potential cost amplifier before changing retry behavior.
- Release-time batch shortening can increase coordination overhead. It has not
  been shown to cause a hang.
- `EngineGlobal::updateStops()` declares its thread pool before completion locals;
  the final job decrements the counter before signalling the event. There is a
  possible event-lifetime race on return that deserves a focused test. It was
  not linked to the overload-only symptoms by captured evidence.
- Organ/model editing and reload ownership need separate review. Editor callers
  do use `suspendProcessing`; the review's limited pasted files omitted that
  caller context. Do not label every edit path an unprotected race based on that
  review alone.
- The review's conditional 64-bit alive-mask shift concern is not applicable to
  the current `MaxBatchQuanta = 32` configuration.

## Next work

1. Remove synchronous disk logging from the UI path; keep callback recording
   bounded and preserve diagnostics without introducing callback I/O.
2. Add an independent audio/worker heartbeat to the safety protocol so logger
   delays cannot masquerade as a DSP stall.
3. Reproduce with fixed registration, isolated MIDI inputs and deterministic
   attacks/holds/releases. Record sound separately from callback deadlines.
4. If a stall remains after the logging fault is addressed, capture coordinator
   phase, generation/completion counts and worker job progress. Investigate
   pool-exhaustion scans and scheduling based on that evidence.
5. Keep automatic application restart deferred during these tests; it remains
   part of the intended final-release fallback, not a substitute for reliability.

## Evidence locations

- Local: `build/profiles/vim4-hang-investigation-2026-10-18/`.
- VIM4: `/home/pwrmon/Projects/aeolus-balanced-live-20261017/claude-hang-investigation-20261018/`.
- `disk-logs-first-run/`: first run, including watchdog escalation after the
  diagnostic attachment failure.
- `baseline-stack/`: successful healthy-process GDB stack capture.
- `ram-first-run-evidence/`: first RAM session's snapshots, timing logs, MIDI
  replay plan and summary.
- `disk-control-logs/` and the archived disk-control observer evidence include
  `ui-sync-syscalls.json` and `ui-io-callback-progress.json`.

Normal installation and demo bundle were preserved. Diagnostic copies use their
own organ data/settings. No commit, push or merge is part of this investigation.

The historical preservation manifest matched 87 of 88 original files at the end
of this investigation. The differing normal `Aeolus.settings` file had a modified
timestamp of 14:50 UTC, and the profiling preferences differed from their older
stored hash with a timestamp of 14:40 UTC. Both timestamps precede the diagnostic
session, whose healthy baseline capture was at 20:14 UTC. These existing preference
changes were left in place; they were not restored from historical test fixtures.

## Implemented follow-up: background logging and HEALTH 2

The shared callback profiler now has a dedicated writer. Audio publishes bounded
records; the writer formats, exclusively opens and writes its CSV about every
200 ms without periodic `fsync`. Shutdown signals under its condition-variable
mutex, waits up to 100 ms for completion and otherwise allows independently owned
state to finish. A missing footer/incomplete file remains possible after blocked
storage or process exit. No synchronous profiling writes remain on the GUI timer.

The isolated Claude overlay publishes callback-started/completed atomics even
without profiling. Its independent receiver reports `HEALTH 2` with those counts,
GUI ticks and device state: deliberately closed, active or preparing. The watchdog
does not open the CSV. It reports UI staleness separately and does not stop
progressing audio because the UI/logger is slow. A closed-device exemption
requires a fresh reply and no in-flight callback. Preparation gets a bounded
allowance and a fresh callback window on completion.

The launcher keeps control logs, metadata and helper stdout in private RAM storage,
then persists them after audio stops. It arms safety before inspecting optional
CSV metadata. Helpers run in separate sessions. Panic retries use the same nonce;
a bounded receiver cache prevents duplicate requests from creating new epochs.
One extra acknowledgement window is allowed when correlation is missing. A
correlated acknowledgement remains valid after another client's explicit Resume.

Validation of this follow-up:

- Full patched audio/MIDI regressions passed locally under Xvfb and on the VIM4's
  desktop, including 44.1/48 kHz and dense worker/cancellation cases. The working
  tree's attack-counter profiler variant also compiles; the full older renderer
  experiment in this worktree was not rebuilt as part of the overlay test.
- All 25 Python safety/diagnostic tests passed on the VIM4, including real FIFO
  scheduling, a blocked FIFO path that safety must not read, dropped reply
  retries, inactive/in-flight cases and long/stuck prepare transitions.
- The real writer was delayed for 30 seconds without sending MIDI. Safety armed
  while the CSV was empty. Across 6.55 seconds, completed callbacks increased
  481→1046 and GUI ticks 21→151 without automatic panic. Writer affinity was
  CPUs 4–7 with ordinary scheduling; capture finished with zero telemetry drops.
- Repeated request nonces were idempotent. Healthy panic/resume/normal close and
  deliberately frozen-process SIGTERM/SIGKILL recovery passed at 44100/512.

Read-only Claude review found the stop-notification and long-prepare edge cases;
these were fixed and regression-tested. An initial ARM link recipe referenced
older project objects and was discarded before hardware use. Final project
objects were rebuilt and linked with ODR/type-mismatch warnings treated as
errors. An unavailable Xvfb wrapper on the VIM4 required running its suite on the
existing desktop instead; that successful run is the functional validation.
Recovery-check reporting was also updated for optional early CSV metadata.
Superseded failures are retained separately from successful test evidence.

This removes the confirmed profiling-related freeze path. It does not establish
that every overload lockup is fixed. DSP ownership/waits and sound-generation
math remain unchanged. Model/organ editing during protected tests remains outside
the prototype's validation; automatic application restart remains deferred for
the final release.

Final build and evidence: `build/profiles/vim4-claude-health-2026-10-18/` locally;
`claude-health-final-test-20261018`, `claude-health-final-source-20261018` and
`native-claude-health-final-20261018` under the VIM4 lab. The normal installation
and earlier demonstration bundles remain separate.

### Manual overload capture after HEALTH 2 deployment

The user reported two release/replay hangs: reaching 1024 voices, releasing and
replaying the registration, then another hang with the UI counter at 680. The
passive observer used `--no-debugger`; it never stopped the application. The same
application PID 11402 remained running afterward.

Three Safety Control panic requests were logged in this session, acknowledged
in approximately 72, 77 and 101 ms respectively. No automatic watchdog panic or
termination occurred. The last two requests correspond to capture windows with
1024-voice peaks; the latter includes a 680-voice plateau. In the 25 seconds before
those requests, recorded callback maxima were 21.27 and 22.29 ms respectively,
against an 11.61 ms deadline at 44100 Hz / 512 frames. Of the recorded callbacks
in those windows, 938/1872 and 768/1979 exceeded that deadline. These windows
include changing loads and are not sustainable-capacity benchmarks.

Callbacks continued completing through the reported episodes. The independent
HEALTH sampler (which started after the first panic) also recorded continuing
GUI timer progress, with no prolonged callback-counter freeze outside explicit
device resume transitions. This does not establish that controls remained usable
or that MIDI note-offs were delivered correctly. Clarification of the perceived
hang and examination of release/replay MIDI state remain necessary. A permanently
stalled DSP worker is not established by this capture.

Evidence was copied locally to
`build/profiles/vim4-claude-health-2026-10-18/overload-evidence-1792360573701483056.tar.gz`.
It contains the passive thread snapshots, HEALTH samples, callback summary and
session logs captured while the application was still running; it is not a final
closed-session archive. No audio, scheduler, organ or application behavior was
changed for this experiment.

### Release/replay follow-up reported by the player

The user subsequently confirmed that the main settings UI remained usable during
an episode, including applying device sample-rate and buffer changes. In four
follow-up tests, the user reported:

- Releasing and replaying the affected keys did not stop the sound during the
  episode, including a five-minute wait. Switching 44100 to 48000 Hz restored
  responsiveness, but affected keys needed another press/release to stop notes.
- An immediate 44100-to-48000 switch likewise restored responsiveness, followed
  by the same need to replay keys to release them.
- Buffer changes could also restore operation, sometimes requiring several tries.
- Different starting rates/buffers all reproduced the symptom; larger buffers
  subjectively made recovery by device changes less reliable. Initial tests
  started at 44100 Hz / 512 frames.

These are user observations, not proof of the layer at which a note-off was lost.
The existing profiler records no MIDI receipt/dispatch counters. The passive RAM
observers were time limited and do not cover the entire later testing period.
The continuously running callback CSV and watchdog logs were saved again in
`build/profiles/vim4-claude-health-2026-10-18/overload-followup-evidence.tar.gz`.
The app was still alive when this archive was taken.

Static inspection identifies several boundaries requiring measurement: ALSA MIDI
receipt, JUCE MidiMessageCollector enqueue/dequeue, processor MIDI dispatch, and
engine/division note-off handling. JUCE's collector resets and clears queued
messages on device reinitialization, and has age/window pruning paths; neither
path is established as the cause here. A short callback overrun alone does not
prove that those pruning paths ran. Device restart can change both MIDI collector
state and DSP/device state, so recovery after restart cannot isolate one cause.
Next instrumentation should correlate bounded note-on/off counters across those
boundaries and pool allocation failures, without logging from the audio callback
or altering note, release or overflow semantics.

### MIDI diagnostic implementation

An isolated overlay now counts note-on/off progression at JUCE's device callback,
collector enqueue/dequeue, processor dispatch and engine MIDI dispatch, with
per-key counters. It also counts accepted release handling, matching voice visits,
voice-release calls, pool allocation failures/returns and actual queue-pruning
note-off discards. Read-only DIAG/NOTE queries use the existing independent
receiver; HEALTH 2 and emergency recovery remain unchanged. No note, voice, tail,
queue-pruning or DSP scheduling policy is changed.

`tools/claude_panic/instrument_midi.py` provides the reviewable overlay, including
private JUCE wrappers rather than submodule edits. The source snapshot and local
build are `build/profiles/vim4-claude-health-2026-10-18/midi-final-source` and
`midi-final-local-build`. The local full audio/MIDI suite passed, including added
collector discard tests. The read-only observer's Unix-socket integration test
passed. ARM validation and live MIDI overload attribution are recorded separately
when completed; local success alone is not target validation.

The final diagnostic overlay also passed the native ARM audio/MIDI regressions,
including the added collector discard tests. The silent-device Panic/Resume check
passed at 44100/512; a separate intentionally SIGSTOP-frozen owned process was
terminated through the expected watchdog escalation. No MIDI was sent in those
checks. An initial superseded diagnostic build was stopped before completion
when raw-message parsing and explicit note-off discard counters were finalized;
only the final build is counted as ARM validation.

The live diagnostic copy is
`/home/pwrmon/Projects/aeolus-balanced-live-20261017/midi-final-test-20261018`,
binary SHA-256 `04ab642e96740f628e850cd20a6df47370be207dbfeccdc0a75e11afa6f1c941`.
It was launched as PID 15363 with verified safety control, 44100/512 and GX61
connection. MIDI capture runs as PID 15406 in
`/dev/shm/aeolus-midi-live-e9j8wgpy/capture/midi.jsonl`; metadata is in the bundle's
`active-midi-capture.json`. These identifiers describe this session only.
The observer was verified to return DIAG 1 with all 24 counters, record HEALTH
progress, and report zero MIDI events before player input. The original 90-file
preservation baseline still matched. Live overload attribution remains pending.

Validation binaries/commands and safety logs were saved locally in
`build/profiles/vim4-claude-health-2026-10-18/midi-diagnostic-validation-20261018.tar.gz`;
private desktop authentication data is excluded. This is internal test evidence,
not an updated USB release package. The existing normal application is unchanged.

### Live stuck-state capture: MIDI interruption precedes Aeolus dispatch

The player reproduced the fault in the MIDI diagnostic build and reported
“stuck,” leaving audio settings unchanged. The captured callback CSV showed a
stable 586 voices at 44100/512; recent callbacks reached 14.61 ms against the
11.61 ms deadline. Audio completion and GUI progress counters continued advancing.
The pool recorded 46003 successful triggers, 45417 returns and 4707 allocation
failures. The failures describe this run, not a confirmed causal mechanism.

Every instrumented MIDI stage agreed on 734 note-ons and 728 note-offs, including
identical per-key counts from device callback through engine MIDI dispatch. All
collector age/window/reset note-off discard counters and GUI queue overflows were
zero. Engine noteOff calls were 726; duplicate note-offs can be suppressed by the
keyboard state layer, so this difference alone is not evidence of a defect.

While the fault remained active, the player pressed/released middle C once, then
repeated it three times after an independent `aseqdump -p 24:0` subscription was
confirmed active. Neither Aeolus's device counters nor the independent ALSA
subscriber received the three requested presses. The initial single press may
have preceded subscriber startup and is not used to validate that subscriber.
This places the observed interruption upstream of Aeolus's MIDI dispatch; it does
not isolate USB hardware, USB completion processing and ALSA backend handling.

The player then pressed **Panic without Resume**, made no audio-device changes,
and pressed/released middle C again. Independent ALSA monitoring and every Aeolus
MIDI stage resumed receiving matching note-on/off events. The collector reset
counter remained **1**, the device stayed active, and Panic stayed acknowledged
and muted (STATUS 1 1 1). Pool returns reached 46003, clearing all remaining voices.
The ALSA trace includes four on/off pairs at the first post-Panic observation;
this count is not used to distinguish delayed from newly played input.

The USB xHCI interrupt (IRQ 37) was delivered exclusively on CPU 0. Its configured
allowed mask was 0–7 but **effective affinity was 0**. The audio callback also used
CPU 0 at SCHED_FIFO priority 70. `ksoftirqd/0`, which runs at SCHED_OTHER, was
runnable during the fault and sleeping after recovery; its scheduler statistics
show substantial accumulated run-queue waiting. This is strong evidence for USB/
MIDI servicing starvation under sustained audio saturation, but a controlled
scheduling/IRQ-affinity comparison is still needed for causal attribution.

The next proposed test is to move USB interrupt servicing temporarily to an
otherwise available efficiency core while preserving the four performance-core
DSP assignments and sound settings, recording and restoring the original affinity.
No IRQ affinity, kernel thread policy, RT bandwidth or governor was changed during
this capture. An alternative experiment can reserve CPU 0 by reducing/reassigning
DSP work, with the resulting capacity change explicitly accounted for.

The independent ALSA observer was stopped after capture; Aeolus remains open in
Panic-muted state, and its read-only MIDI observer continues running. Evidence is
saved locally in
`build/profiles/vim4-claude-health-2026-10-18/midi-stuck-recovery-evidence-20261018.tar.gz`,
including kernel snapshots, per-key/counter snapshots, MIDI trace and callback
CSV. No debugger was attached, and the normal application/files were unchanged.

### Temporary USB IRQ placement comparison

With explicit user authorization, the player ran the scoped root helper
`tools/claude_panic/test_usb_irq.py` locally because noninteractive SSH sudo was
unavailable. It recorded original allowed affinity 0–7/effective CPU 0, moved IRQ
37 to CPU 4, and verified allowed/effective affinity 4. Audio remained FIFO 70 on
CPU 0; DSP workers remained FIFO 70 on CPUs 1–3. The application, registration,
voice cap and DSP policies were unchanged. The helper restores the original
allowed affinity when the app exits, on handled termination, or after 900 seconds.
This was a temporary host experiment, not a persistent appliance configuration.

After Resume, the player could provoke breakup while holding many keys but could
no longer reproduce the sustained stuck condition: releasing keys cleared the
load. The captured phase reached **1024 voices** and subsequently returned to
**zero voices** at 44100/512. Device/collector/processor/engine stages each advanced
by **1954 note-ons and 1954 note-offs** relative to the pre-Resume snapshot. No
collector note-off discards or GUI queue overflows were recorded. Pool successes
and returns both reached 171920, with 15508 accumulated allocation failures. The
panic epoch remained 1: no additional Panic was used in this phase. The collector
reset count changed from 1 to 2 during the explicit Resume, as expected.

This intervention strongly supports USB/MIDI servicing starvation on saturated
CPU 0 as the mechanism sustaining the stuck notes. It does not establish that
1024 voices can sound cleanly, that every reported historical hang had this cause,
or that no DSP/state defects remain. Manual playing was not an identical MIDI
replay, and no restoration-to-CPU-0 reversal trial was performed in this phase.
No global RT bandwidth, kernel-thread priority or governor changes were made.

The comparison evidence is saved locally as
`build/profiles/vim4-claude-health-2026-10-18/midi-irq-comparison-evidence-20261018.tar.gz`.
The player was asked to close the app normally to trigger affinity restoration;
restoration is recorded separately after verification.

After the player closed Aeolus normally, verification confirmed app exit and IRQ
37 restored to allowed **0–7**, effective **CPU 0**. All 90 original files still
matched their preservation hashes. The independent ALSA observer was stopped.
Final closed-session evidence and restoration proof are saved locally in
`build/profiles/vim4-claude-health-2026-10-18/midi-irq-comparison-final-20261018.tar.gz`.
