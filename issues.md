# Source audit follow-up

Audit: 2026-10-19. This tracks the standalone worktree reviewed that day, including
pre-existing worktree changes. A checked entry means the code defect was addressed
and has regression coverage or the stated static verification. It does not mean
hardware, every platform, or every possible configuration was validated.

## October 20 performance investigation

The [VIM4 regression audit](docs/PERFORMANCE_REGRESSION_AUDIT_2026-10-20.md)
separates the confirmed serial/512-pool launch mismatch from added code costs,
matched timing results and the remaining live validation. The correctness fixes
below remain addressed; they do not establish a particular clean voice capacity.

- [x] **PERF-09 — Retired layouts can leave grouped rendering in serial fallback.**
  Fixed October 21: stably prune empty retired pointers at a quiescent batch
  boundary, preserving sounding tails, live divisions and setup-only owner
  destruction. A permanent 72-voice/18-edit regression checks automatic recovery,
  reference output and callback/worker heap safety at 44.1/48 kHz, with serial,
  zero-worker and three-worker rendering. Release, ASan/UBSan and TSan pass;
  see [stage 0 evidence and remaining checks](docs/STAGE_0_RELIABILITY_2026-10-21.md).
  This does not establish a fix for the fresh-launch VIM4 listening complaint.

## Addressed findings

- [x] **AUD-01/02 — Sequencer and MIDI stop-index memory corruption.** Resize
  sequencer state before restoring edited layouts; validate dimensions and reject
  invalid stop indices in Release builds.
- [x] **AUD-03/15/16 — Wavetable and voice lifetimes.** Immutable generations retain
  sounding pipes through releases. Old divisions render their release tails and
  recycle voices. Device preparation retains the current tuning. Unchanged pipes
  are shared; retired generations release unused pipes individually, so one held
  note does not retain whole ranks. Staged admission leases are cleared after
  commit/rejection. Rate changes detach shared pipes before rebuilding.
- [x] **AUD-04 — Convolution worker ownership and completion.** Private preallocated
  job input/results, generation checks and acquire/release publication prevent
  input reuse and stale output. Late/rejected jobs compute locally, preserving
  output and overlap tails. Setup joins and purges jobs before replacing storage;
  restart creates a fresh worker. Completed results transfer by pointer swap.
  Normal, stalled, full-queue and pending-setup tests exercise both computation
  paths, output equivalence, release tails and allocation checks. Worst-case
  boundary-callback work still needs target-hardware measurement below.
- [x] **AUD-05 — UI/audio registration races.** UI stop, link, tremulant, bass,
  preset, sequencer and MIDI-mask actions use a bounded message-to-audio queue.
  Overflow is reported; stale layout epochs cannot address replaced divisions.
  Processing applies at most eight queued actions per Engine render call; setup
  can drain the queue while suspended. General Cancel is one action across the
  whole organ, and division All OFF is one action, avoiding staggered per-stop
  releases behind the queue budget. With no active audio device, the owning
  processor's callback lock permits immediate message-thread application.
  UI/persistence readers use published atomic scalars and preallocated preset/
  sequencer storage. Save snapshots validate a mutation sequence; a busy snapshot
  is retried rather than replaced with old data. Host capture uses the validated
  snapshot with a short callback-lock fallback during continuous mutations;
  restoration holds the callback lock. Neither sets isSuspended(), so JUCE cannot
  discard incoming MIDI/note-offs during a state operation. JSON serialization
  happens after snapshot ownership is released. Ordinary timer saves do not
  suspend audio. Device prepare/release shares callback ownership with inactive
  UI control application; a parallel preparation/control stress test covers it. Concurrency tests check snapshots
  against the final applied control and exercise host save/restore alongside rendering.
  Presets whose stop/link dimensions no longer match are marked uncaptured;
  recalling them cannot unexpectedly silence the edited division.
- [x] **AUD-06 — Reciprocal couplers sustained released keys.** Compute graph state
  afresh from physical key owners. A bounded work list visits each forwarding
  division/pitch once; old aggregate state cannot feed back after release.
- [x] **AUD-07 — Bass disable left a pedal key held.** Derive each manual's lowest
  held note independently and union those sources with directly held pedal keys.
  Disabling one source cannot release another source's pedal note.
- [x] **AUD-08 — Same-pitch channel ownership and incomplete held-note repair.**
  Retain physical key state per MIDI channel; one channel's release does not clear
  another channel's note. Repeated note-on on the same channel remains one held
  source. Repair every missing held pitch per stop, considering active voices and
  staged admission bitsets rather than stopping at the first matching voice.
- [x] **AUD-09 — Self-sub-coupler capacity over-reservation.** Self octave links add
  one shifted pitch without recursively feeding themselves. Dispatch, control and
  capacity forecasting use the same graph. Whole-note rejection preserves existing
  voices, including all old release tails during a release/repress transaction.
  Note transactions charge only newly reached graph keys; unrelated pending
  held-key registration additions have their own control-quantum admission.
  Refresh the pre-note graph after control changes and All Notes Off, so stale
  keys cannot be charged to an unrelated note or bypass re-press admission.
- [x] **AUD-10 — Pass-through ignored.** All links sound their destination; only an
  enabled pass-through permits the arrival to traverse further links. Tests cover
  both flags, transitive chains, cycles, releases and serial/grouped rendering.
- [x] **AUD-11 — Incomplete stop-file validation.** External JSON/binary imports are
  transactional. Legacy note-range conversion is included in bounds validation.
  Reject malformed syntax, unsupported versions, invalid harmonic
  counts, note ranges outside MIDI/LUT capacity, zero/invalid footage ratios,
  invalid masks/table dimensions, nonfinite or out-of-range values (including a
  +24 dB combined harmonic-gain ceiling) and truncated
  binary payloads before replacing a valid model. Empty internal names can use
  the filename; zero interpolation masks remain compatible. All 85 embedded stops
  are explicitly validated in the regression suite. Import short legacy harmonic
  arrays instead of silently dropping them. Authorship/comments are retained.
  All bundled source assets remain unchanged. Extreme tuning requests are bounded
  before continued-fraction wavetable calculations.
- [x] **AUD-12 — Footage applied twice to chiff frequency.** The pipe-frequency
  contract now returns the already footage-scaled fundamental. This intentionally
  corrects chiff frequency, onset delay and release timing for non-8-foot pipes;
  listening validation remains listed below.
- [x] **AUD-13 — Footage edits escaped test isolation.** Use the shared organ-data
  resolver and search its Stops folder on restart. Test overrides take precedence
  over executable-adjacent files and restrict stop searches to test-owned paths.
  Variants stay in memory until OK; only referenced variants are committed.
  Returning to original footage reselects its original model. Cancel writes none.
  Failed commits remove only files newly created by that commit, preserve existing
  files, report conflicts/errors and avoid publishing partially saved models.
  Deferred reloads discard removed variant files while keeping valid cached ranks;
  rollback cannot later raise a false missing-file import error. Existing
  same-named variants are resolved across supported search folders/formats and
  reused, preserving custom voicing instead of shadowing it with a generated JSON.
  Unchanged reused models keep their prepared pipe generation, avoiding unnecessary
  waveform randomization, rebuilds and processor suspension. A required changed
  model that is still deferred leaves Stop Editor open with a retry message;
  stale cached footage cannot be committed as a successful edit.
- [x] **AUD-14 — MIDI Settings lacked a device manager.** Standalone obtains its
  actual JUCE holder's manager; other wrapper types retain the null fallback.
  The component's lifetime remains contained by the editor/standalone owner.
  Division selectors resolve current indices/epochs and rebuild after layout
  replacement; delayed popup callbacks use component-safe pointers. An explicit
  editor lifetime guard stops panel polling and rejects callbacks if a modal
  callout survives destruction of its editor/processor. A regression keeps the
  panel alive through owner/processor destruction and pumps its timer.

## Other code defects addressed

- [x] Worker enqueue wake-up uses native semaphore signalling instead of the
  inherited mutex-based semaphore. Rejected jobs do not generate wake tokens;
  stop drains unused tokens, and start is idempotent. Linux is exercised locally;
  Windows/macOS builds and scheduling checks remain below.
- [x] Empty retired divisions skip serial rendering and batch mixing. Objects are
  still destroyed only during suspended setup or teardown, never in callbacks.
- [x] Initialize EngineGlobal's internal sample rate before early tuning rebuilds;
  publish rank note bounds atomically and use a generation's captured first note
  for trigger indexing. Setup-only registry locking serializes new-rank
  publication with device preparation; the shared prepared rate is atomic.
- [x] Checked temporary writes verify output and flush before replacing a file.
  JUCE's flush calls fsync on POSIX/FlushFileBuffers on Windows. Verify the
  resulting byte count as well, since JUCE can omit an error after a positive
  short buffered write. A Linux child-process file-size limit exercises that
  failure while preserving the previous file. Adjacent unique save temporaries
  use a .tmp extension, so crash leftovers cannot become stop-model candidates.
  Report failed
  configuration/state/variant/export writes and preserve existing targets.
  Malformed saved state is retained with an explicit repair/remove message and
  persistence remains disabled for that instance; it is never silently replaced.
  Zero-byte state files are treated as absent and allow subsequent saving.
  Unwritable initialization uses embedded models/configuration with a reported
  error, rather than throwing from processor construction. Multi-division stop
  edits stage one configuration and write once. Invalid edits are validated before
  retiring divisions and reload the exact validated in-memory configuration.
  Successful structural edits save migrated state synchronously during setup,
  rather than leaving the old division count until a debounce timer fires.
  This does not make the two files a crash-atomic transaction.
- [x] Bounds-check legacy MIDI channels before shifting and constrain masks to
  sixteen channels. Removing a channel clears only that channel's held keys;
  subsequent note-offs cannot leave ownership stuck. Correct swell-mask matching.
- [x] Reject unsupported or malformed organ layouts before loading them. Startup
  can use the embedded organ while preserving rejected custom configuration/state;
  failed structural edits retain active divisions and valid batch-render pointers.
  Embedded fallback sessions refuse structural edits until the rejected custom
  configuration is repaired and the app restarted; Division Editor cannot truncate
  an unsupported custom organ to the visible fallback count.
- [x] Correct N_func::clearValue's inverted index guard and test interpolation.
- [x] Avoid redundant JUCE message-manager locks when the current thread already
  owns the message manager. Full TSan exposed this inherited locked-mutex
  destruction path; background-thread setup/teardown keeps lock protection.
- [x] Log preferences-save failures during EngineGlobal shutdown instead of
  launching asynchronous AlertWindows after JUCE teardown has begun.
- [x] Existing-model reload owns processor callback locks without setting
  isSuspended(), retaining incoming MIDI even for held keys with no sounding
  voices. File parsing, validation, model allocation and full waveform synthesis
  happen in a private prepared rank before callback ownership. Publication copies
  prepared metadata and moves the ready generation; retirement/destruction follows
  outside ownership. Pipe-owned shared model snapshots retain private metadata
  through tuning/rate copies and old leases. Reload honors active MTS per-note
  frequencies/footage; no-master fallback honors configured local tuning.
  Synthetic per-note tuning and retained model metadata are regression-tested. Updates still wait for audible
  voices/tails to finish, including a second check after preparation. Setup
  transition deadlines still require the measurements below.

## Deliberate failure-handling policies

- An externally changed organ with a different division count cannot safely use
  its old registration by index. Its state file is retained and saving is disabled
  with a repair/remove warning. Prefix migration is reserved for successful
  in-application layout edits, where the previous layout is known. This preserves
  custom state instead of replacing it after a failed load.
- If applying committed footage variants fails or the editor closes first, newly
  created files are removed. Valid prepared models may remain in the process cache
  until shutdown; repeating the same name updates that entry. Eagerly deleting
  globally published ranks could invalidate another instance or sounding pipes.
  The stop picker lists backing files, so cached-only entries are not selectable.
- Layout replacement invalidates pending channel popups; their selection cannot
  address the replacement division. Import errors in configured external-stop
  search folders remain visible, including unrelated JSON placed in those folders.

## Registration-edit semantics

Sequencer steps store positional stop/link masks. After changing stop/coupler
ordering, reprogram the affected steps; migration by stop identity is not
implemented. Dimension changes are bounds-safe, and mismatched divisional
presets are explicitly invalidated.

## Remaining validation requiring other hardware or environments

- [ ] Measure callback p50/p99/worst, deadline misses and actual device xruns on
  VIM4/ARM64 and representative x86 hardware at 128/256/512/1024 frames. Include
  full registrations, coupler graphs, control bursts, long IRs, worker contention
  and boundary callbacks where every convolution partition falls back locally.
  Include host state restoration, model reload and device-setup/control wait time;
  callback ownership prevents lost MIDI but does not prove deadline compliance.
  No optimization or reliable sustainable voice-count claim follows from static
  inspection, a CPU meter or an uncapped local render alone.
- [ ] Measure real MTS-master update workloads, peak memory and rebuild latency on
  memory-limited hardware. Unit tests cover repeated tuning publication, held
  pipes, shared unchanged pipes, per-pipe retirement and concurrent triggering.
- [ ] Exercise real USB-MIDI input enablement/device loss/reconnection and physical
  audio channel mapping. Authenticated-desktop pending-menu teardown passes
  locally; full Stop/MIDI editor interaction, touch controls and save/restart
  workflows still need actual-device verification.
- [ ] Listen to and compare 16-foot/8-foot/4-foot chiff/onset/release renders. The
  frequency correction is intentional; numerical tests do not establish timbral
  preference or real-device audio reliability.
- [ ] Build and test native worker signalling on Windows/macOS and ARM64. No claim
  of those platforms' current validation is made by the Linux worktree results.
- [ ] Validate appliance power-loss durability on its filesystem/storage. Checked
  file writes flush file contents; directory-entry durability across abrupt power
  loss is not established by ordinary write-failure tests.

## Evidence and qualifications

Final Linux x86-64 worktree verification (2026-10-19 local / 2026-10-20 UTC):

| Check | Result |
| --- | --- |
| Release CTest | 2/2 passed, 15.28 s |
| Standalone target | Aeolus_Standalone linked in the test-enabled Release build |
| ASan + UBSan CTest | 2/2 passed, 39.38 s; leak detection disabled |
| Full application/JUCE TSan CTest | 2/2 passed, 125.94 s; no suppressions |
| WITH_MULTIBUS_OUTPUT CTest | 2/2 passed, 9.15 s |
| Authenticated desktop, pending-popup workflow enabled | 2/2 passed, 15.29 s |
| Documentation | Local links and git diff --check passed |

Expected error paths log warnings in test builds; production warning rendering,
USB-MIDI/audio hardware, touch controls and the other environments above remain
separate checks. All profiles/files used here were test-owned. The test-enabled
standalone is not a production distribution artifact.

The final corrective Claude review **20261020T031301Z-d191d60bad** completed with
zero findings and a valid current snapshot. This was static review, not independent
execution of the tests. Earlier broader reviews drove the ownership, MIDI,
validation and preservation fixes; stale snapshots were not substituted for the
final review. Artifacts remain under ignored build/claude-reviews/.

Local final render timings used an AMD Ryzen 9 7900X, Linux
6.12.111+deb13-amd64, 44.1 kHz / 512 frames (11.61 ms budget), all stops, couplers
off, reverb wet zero, ten requested notes held eight seconds, deterministic
per-voice randomization and a 1024-entry pool. Both runs reached 710 voices.
Scheduling and affinity were unchanged. This is an uncapped offline loop with
no hardware output or native xrun measurement, not a sustainable-voice or
optimization claim.

| Renderer / phase | Blocks | p50 ms | p99 ms | Worst ms | Over budget |
| --- | ---: | ---: | ---: | ---: | ---: |
| Serial attack | 86 | 4.320 | 6.185 | 7.809 | 0 |
| Serial sustain | 603 | 3.597 | 4.010 | 4.314 | 0 |
| Serial release | 18 | 0.316 | 6.743 | 6.743 | 0 |
| Three workers, group 8: attack | 86 | 1.323 | 2.048 | 3.196 | 0 |
| Three workers, group 8: sustain | 603 | 1.034 | 1.316 | 1.470 | 0 |
| Three workers, group 8: release | 18 | 0.158 | 1.755 | 1.755 | 0 |

The local logs are build/rest-ctest.log, build/rest-sanitize-ctest.log,
build/rest-tsan-ctest.log, build/rest-multibus-ctest.log,
build/rest-desktop-ctest.log and build/rest-benchmark-{serial,workers}.log.
They are ignored workspace artifacts. Original audit evidence remains in
build/source-audit/. Original Claude reviews were 20261020T001309Z-87888c222c
and 20261020T001755Z-b8fdd5434a; AUD-04's completed review was
20261020T010014Z-37c77c1871. Historical results do not establish current hardware
reliability or distribution clearance.
