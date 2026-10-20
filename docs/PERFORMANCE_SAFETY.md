# Voice admission and performance meters

Implemented October 19, 2026. This increment covers the logical voice cap,
processing/voice meters and their settings menus. It does not configure system
privileges, CPU affinity, IRQs, boot startup or automatic restart.

## Operation

Open **Settings → Performance & Safety → Voice capacity** to set a logical cap
or follow the startup pool size. Apply saves the preference; Cancel leaves it
unchanged. The physical pool remains preallocated at startup (512 by default,
`AEOLUS_MAX_VOICES=16..4096`). An explicit preference is clamped to the current
pool without overwriting a larger saved preference until the user applies a new
choice. Lowering the cap never releases existing notes or truncates tails.

The incoming note transaction collects actual pipe states using the existing
note dispatch, including mixtures and bass-coupler transitions. Control key,
bass and trigger state are snapshotted; bass releases are deferred. If the
complete demand cannot fit, the states are restored and nothing is triggered
or released. A note-on rejected at capacity does not become a delayed note.
Note-offs, releases and Panic continue independently of admission.

The bounded coupling graph accounts for enabled octave and chained couplers,
including cycles. Incoming note admission counts newly reached keys; pending
registration additions for existing held keys are admitted separately at the
control step. Release tails and voices not yet recycled remain occupied capacity.

Control-step registration additions are one transaction across divisions: all
new additions fit or none are admitted. Rejected additions are blocked for the
corresponding held pitches until release/replay, even if the cap is increased.
The rejection indicator counts the rejected registration transaction separately
from incoming MIDI notes. A related defect was fixed: each stop now has its own
missing-note set, so an already voiced stop cannot hide held keys from a newly
enabled stop. This intentionally restores formerly missing registration voices.

Admission storage and per-stop key lookup masks are sized outside processing.
The masks are rebuilt once per transaction, avoiding a full voice-list scan for
every requested pipe. The callback does not allocate,
resize the pool, take a new lock, steal voices or shorten audible tails. The
ten-division admission bound matches the supported organ layout maximum. Larger
hand-edited configurations are rejected and preserved for repair.

## Meters and preferences

The audio bar uses elapsed callback time / current buffer deadline, with
approximately 300 ms time-based smoothing. Latest and recent peak values remain
uncapped in diagnostics; the console displays the uncapped recent peak. The bar
fill itself stops at its right edge. Offline rendering does not increment
real-time processing-deadline misses. Zero-length blocks do not divide by zero.

White markers retain recent peaks for two seconds. Default warning/critical
thresholds are 75% and 90%, configurable under **Load & voice meters**. Both bars
show numeric and text status as well as color. A missed processing deadline or
observed native xrun turns the audio bar red for two seconds. A rejected note or
registration request does the same for the capacity bar, with retained blocked
counts. Voice peaks are observed by the UI timer; callback peaks are captured by
the audio owner. Neither is a calibrated hardware capacity measurement.

**Diagnostics → Live readings & reset counters** distinguishes processing misses
from native device xruns (shown as unavailable when the backend does not expose
them). Native-counter resets/device changes do not erase already observed xruns;
unavailable intervals cannot be measured. Reset clears diagnostic counts and
callback peak history on the next callback without altering voices or MIDI.
Counters are session-only. The cap and color thresholds use application
preferences, separate from custom organ files and host organ state.

The settings popup guards its editor lifetime and stops its diagnostics timer
when the editor is destroyed. Panic/Resume retain the separate reserved footer,
including during preset programming. The settings button and both meter bars
fit the existing 640×480 minimum window size.

## Verification and remaining checks

- Release standalone/shared code and regression executable built on Linux x86-64.
- Regression coverage includes exact-cap and over-cap mixtures, repeated notes,
  cap reduction below active use, full release tails, no replay after rejection,
  registration admission/rejection, same-block release/repress through chained
  couplers, octave couplers, bass rollback,
  Panic, and callback/worker heap checks at 44.1/48 kHz with serial and three-worker
  rendering. Existing tests cover nonzero MIDI offsets and queue overflow.
- Meter tests use synthetic callback times above 100%, peak expiry, offline and
  zero-frame callbacks, counter reset, native-xrun changes and device-counter
  reset. UI tests cover Apply/Cancel, saved preference reload, compact layout,
  editor-owner deletion and a reset attempt after processor destruction.
- Four repeatable dry stereo render comparisons at 44.1/48 kHz, serial/three-worker,
  are bit-identical against commit `5df27bf` with the same held-stop defect fix
  applied independently. Test builds seed each pipe's wavetable separately so
  setup thread-pool job order cannot change this fixture; production wavetable
  randomization is unchanged. The original unfixed reference had 94 peak voices
  in this registration; both corrected versions have 101. This is a correctness
  change, not a throughput improvement.
- Evidence is kept locally under `build/profiles/voice-safety/`. Final render
  comparisons ran directly and exited 0 (`render-final.json`); the four sample
  files matched byte for byte. Earlier `xvfb-run` wrappers returned 1 after
  failing to create their server and cleaning up an already exited process;
  these are not successful isolated-Xvfb launches. Offscreen snapshots were
  visually reviewed.
- Final headless/offscreen CTest passed 2/2 in 10.88 seconds, exit 0. The normal
  non-test Release standalone also built successfully, exit 0. The optional
  desktop-popup test crashed on the
  provided `DISPLAY=:1`; a direct X11 connection check also failed. Actual menu
  interaction still requires an accessible authenticated desktop with a window
  manager. The popup failure is not counted as UI validation.
- No new VIM4/native ARM audio, physical touch, USB/MIDI saturation or hardware
  capacity result is claimed. Recheck callback tails/deadlines and dense attacks
  on the board before treating a selected cap as a reliable playing profile.

### Desktop timing check

A short offline x86-64 A/B/B/A check used ten simultaneous notes, all stops,
710 allocated voices, three workers, a 1024-voice pool, 44.1 kHz / 512 frames,
zero wet reverb and no requested affinity/FIFO changes. Both reference and
candidate completed without measured processing blocks exceeding the 11.61 ms
budget. These are wall-clock render timings, not live audio/xrun or VIM4 limits.

| Build | Sustain p50 (two runs) | Sustain p99 | Worst attack block |
| --- | --- | --- | --- |
| Prior renderer + held-stop fix | 1.040 / 1.093 ms | 1.332 / 1.340 ms | 3.152 / 3.090 ms |
| New admission + meters | 1.058 / 1.062 ms | 1.264 / 1.225 ms | 2.966 / 3.316 ms |

The short run and scheduling variation do not establish a throughput gain or a
precise overhead percentage. Raw output and exit codes are in
`benchmark-final.json` and the adjacent stdout/stderr files.

The requested independent Claude review did not complete in the restricted
execution environment. Its exact command, timeout/exit result, stdout/stderr and
non-secret environment diagnostics are retained in the local evidence directory.
This is not counted as a completed independent review.
