# Stage 0 reliability work — October 21, 2026

Stage 0 is **in progress**. This first increment fixes PERF-09; it does not close
the live-device, attack-time or other acceptance tasks in
[the roadmap](../AEOLUS_FEATURES.md#11-implementation-phases).

## Delivered: automatic recovery from retired-layout serial fallback

Repeated organ edits can leave more than 64 division pointers in the grouped
render list. Audible retired divisions must keep rendering, but empty retired
entries previously kept the serial fallback active even after every tail ended.

`Engine::renderBatch()` now stably removes empty retired pointers before checking
the grouped capacity and building division/job indices. This runs only between
batches, after earlier jobs/output consumption finish. It keeps empty live
entries, all sounding retired entries, pointer order and vector capacity.
Ownership stays with `_retiredDivisions`; object destruction remains in suspended
setup. No new callback allocation, deletion, locks or sound-quality changes are
introduced. Layouts genuinely exceeding the grouped capacity still render serially
until enough tails finish, then recover automatically without another edit.

The fix is independent of board names, core numbers and worker counts. It does
not change the 512-entry startup default or implement stage 1A's proposed
128-voice fresh-install profile.

## Regression and verification

The permanent regression retains 72 voices through 18 layout replacements, with
the first generation already audible. It checks automatic grouped-job recovery
while tails drain and on fresh notes without another setup edit. It compares
serial/grouped output within 1e-5 and zero-worker/three-worker output exactly at
44.1 and 48 kHz, under the callback/worker allocation-and-free guard.

| Check | Result |
| --- | --- |
| New regression against pre-fix engine | Failed as expected: grouped rendering did not recover during tail drain |
| Linux x86-64 Release CTest | 2/2 passed, 17.81 s |
| Production Linux x86-64 standalone | Built successfully with tests disabled; not deployed |
| ASan + UBSan CTest | 2/2 passed, 44.38 s; leak detection disabled |
| ThreadSanitizer CTest | 2/2 passed, 135.49 s; halt-on-error, no suppressions |
| Ordinary dry reference exports | Four files byte-identical before/after: 44.1/48 kHz, serial/three-worker |
| Claude review | `20261021T222912Z-6ee4b586b1`: completed, current source snapshot, zero findings |

Claude inspected the supplied source/test snapshot; it did not execute tests.
Codex checked the review's setup-ownership limitation against processor preparation,
release and editor suspension: normal device teardown/preparation share JUCE's
callback lock, and editor edits suspend processing before mutating layout owners.
The existing caller ownership contract is retained. The new stress test executes
on Linux stereo builds; no new multibus/non-Linux or physical-device result is
claimed by these checks.

Local logs, before/after exports and profiling artifacts are under ignored
`build/stage0/`. The three pre-existing explicit-map constructor annotations in
other test files and earlier roadmap/audit edits are retained.

## VIM4 and attack investigation

At the read-only machine check, no production Aeolus process was running and both
CPU governors were `conservative`. That initial check could not verify a live renderer, pool or device. The later
launches below used the retained production executable; no application binary was
installed/replaced and no organ/model or preference file was directly rewritten.

An isolated offline attack probe used the retained audit snapshot to separate
whole-chord note dispatch from the first render block. That snapshot precedes
PERF-09's fix; fresh-layout attack behavior is unaffected by the fix. Twelve onsets
per registration used 44.1 kHz / 512, all default stops, couplers off, wet zero,
1024 pool entries and three workers on the measured VIM4 core arrangement. Each
onset followed complete release-tail drainage; no pipes were stolen or shortened.

| Actual voices | First onset: dispatch + first block (ms) | Later 11: median dispatch (ms) | Later 11: median first block (ms) | Later 11: median combined (ms) |
| --- | --- | --- | --- | --- |
| 432 (six keys) | 8.620 | 1.588 | 3.695 | 5.282 |
| 1003 (14 keys) | 17.558 | 3.528 | 8.128 | 11.698 |

The combined values are measured per-onset sums, not sums of independent medians.
Dispatch was called directly before `processBlock`, so this separates engine work
rather than reproducing host MIDI scheduling or proving a full callback budget.
The initial dense onset and later dense totals show little headroom against
11.61 ms; they do not identify a safe optimization by themselves. Twelve onsets
are not a reliable p99 population. Both governors remained conservative; the
large cluster was 2016 MHz before the dense run and 2208 MHz after it. These runs
are not directly comparable with the earlier performance-governor audit.

The `/usr/bin/perf` wrapper failed (exit 2) because it has no tool for kernel
5.15.137. An already installed executable at
`/usr/lib/linux-tools/6.8.0-147-generic/perf` subsequently recorded a separate
20-second live userspace profile at 199 Hz: 5,774 samples, zero lost samples.
No installation, kernel/sysctl/authentication change or elevated access was needed.
Its timings are separate from the uninstrumented captures.

Across all profiled threads, about 53.5% of samples were attributed to
`Voice::process` and 16.1% to `Pipewave::play`; `beginAdmission` accounted for 1.18%.
Inlining and the mixed live workload limit attribution. Only ten call chains
included `Engine::noteOn` or `Voice::trigger`, spanning clears, filter/spatial
setup, admission and atomics. That is insufficient to rank attack sub-stages or
justify a new DSP optimization. Full report/data remain in the VIM4's ignored
`build/vim4-code-audit/stage0-live-perf*`; the report is also retained locally.

## Live MIDI capture before the user's restart

With the user available to play, the retained production IPO binary was launched
with three workers, 1024 pool entries, audio CPU 0 / workers 1–3, normal scheduling
and a fresh timing file. Startup messages and `/proc` thread affinity confirmed
those requested renderer settings. Worker CPU ticks increased during capture;
governors were not modified. This binary does not yet contain the local PERF-09
fix, so the capture is not ARM validation of that change.

The saved preference requested the FiiO direct ALSA device, but ALSA enumeration
showed only the keyboard and onboard sound. The actual process was a PipeWire
ALSA client routed to the onboard stereo sink, with no direct PCM device open.
The user confirmed there was **no audio output available to listen to** and
restarted the VIM4. This is MIDI/processor telemetry, not a passed listening test
or validation of direct-FiiO reliability.

The marked 55.15-second pass contained 4,750 records, of which 1,543 had active
voices, peaking at 756 voices including tails. Active-block p50/p99/worst were
5.617 / 12.220 / 24.990 ms, with 21 processing-budget misses at 44.1 kHz / 512.
The reported backend xrun counter remained zero; it does not establish an
error-free physical output in this fallback configuration. CPU frequency samples
ranged 500–2208 MHz under the conservative governor. Most misses clustered at
voice-count increases while nearby 250 ms frequency samples were low. That
association is not an isolated causal measurement or proof of a DSP regression.

Logs and the marked summary are retained under `build/stage0/live-*`. After
restart, first verify DAC enumeration and actual output selection, then repeat
with audible output and a recorded effective hardware profile. Preserve the
original desired device preference when diagnosing fallback; do not treat the
saved preference alone as proof of the active audio path.

## Direct-DAC follow-up after restart

After reboot, ALSA enumerated the FiiO as card 2 and the saved preference still
requested its direct device. The retained production build was relaunched with
the same explicit test renderer settings. Its open `/dev/snd/pcmC2D0p` descriptor
confirmed direct DAC access, and startup logs/thread masks again confirmed three
workers on CPUs 1–3 with audio on CPU 0. This is a measured VIM4 test configuration,
not a new application default or a portable core-number policy.

The marked direct-DAC pass covered 55.20 seconds and 4,755 records, including
1,259 active blocks and a peak of 1001 voices. Active p50/p99/worst were
4.618 / 18.332 / 35.484 ms, with 67 processing-budget misses and a reported native
xrun counter of zero. Frequency samples again ranged 500–2208 MHz with conservative
governors. This varied registration/attack pass is not a fixed-voice sustain
benchmark and is not directly comparable with the dry offline corpus.

The user subsequently reported loss of organ sound with the device test tone
still working. HEALTH/STATUS showed an active, responding engine with Panic mute
off, and the keyboard retained its ALSA subscription to Aeolus. The user found
master volume at zero and confirmed moving the physical keyboard knob. A passive
ALSA MIDI capture independently recorded channel 1 CC7 values descending to zero.
CC7 master-volume handling is intended behavior; no volume-control code was changed.

After restoring volume, the user reported breakup on attacks at about 1000 voices,
settling within less than a second. Remote `sudo -n` required a password, so the
user applied the supplied temporary performance-governor command in their terminal.
The sampler verified both governors and fixed 2208/2016 MHz before measuring the
comparison. With the same app and registration, the user then reported **no breakup**.

The 55.02-second performance-governor window had 4,739 records, 3,062 active blocks
and a peak of 1024 voices. Active p50/p99/worst were 5.834 / 13.393 / 16.222 ms,
with 112 processing-budget misses and native xrun values remaining zero. Median
processing occupied about 50.2% of the 11.61 ms budget. The previous conservative
window and this one have different active-block counts and voice trajectories;
miss counts or medians must not be compared as if they were a matched MIDI corpus.
The lower observed peak and the user's repeated-chord listening result support
clock ramping as a contributor, not proof that all attack cost has been removed.

This is a successful short audible comparison, **not a clean 1024-voice capacity
certification**: processing headroom, independent repeated/cold onsets and longer
runs remain open. Live rejection counters were not exported in this timing trace;
a full-capacity test must account for whole-note rejections as well as sound/xruns.

The earlier task-owned `build/vim4-lto/Start-Aeolus-LTO.sh` selected CPU 0 but
omitted workers/pool settings. It now delegates through `sh` to the existing
multicore launcher, defaulting this explicitly named VIM4 test profile to three
workers, pool 1024, audio CPU 0 / workers 1–3, normal scheduling. Explicit environment
overrides remain supported. Shell syntax, resolved defaults and overrides were
checked with `/usr/bin/env` rather than opening another audio instance. The old
script is retained beside it as `.stage0-before`. This changes no application
or fresh-install default and performs no privileged governor writes.

The app remains open on the direct DAC. Performance governors were applied by the
user for the test; the earlier reboot demonstrated that the setting is volatile.
Persistent, portable profile application remains stage 1A/7C work. Of the original
91 checked data/preference files, the later check found only `organ_state.json`
changed during the user's live registration interaction; its contents were left
in place. No custom organ/model or preference file was programmatically rewritten.

## Repeating the current VIM4 test profile

After a reboot, with no other Aeolus instance running, the manually verified
commands are:

```sh
sudo sh -c 'for p in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do echo performance > "$p"; done'
sh /home/pwrmon/Downloads/aeolus_plugin-master/aeolus_plugin/build/vim4-lto/Start-Aeolus-LTO.sh
```

This is the current VIM4 comparison procedure, not an installer or a universal
hardware default. The first command needs local administrator authentication;
the launcher itself remains unprivileged and prints the actual governor state.
New-install 128-voice defaults and portable saved configuration remain stage 1A.

## Still required before stage 0 can close

- Extend the verified VIM4 renderer/device/policy case to cold startup, durable
  profile application and other hardware. The current app/registration remains
  a measured test case, not a universal machine profile.
- Reproduce and attribute dense attack misses with repeated onsets and real MIDI;
  retain timbre, whole-note admission, spatial sound and complete release tails.
- Validate live attacks/sustain/releases/Panic and record callback timing, xruns,
  rejection counts, thermal behavior and listening results.
- Complete the remaining real-device/MTS/editor/save-restart/timbre/durability
  checks in [issues.md](../issues.md). Offline timing does not close those gates.

## Follow-up: requested temporary CPU-performance control

A separate VIM4 production build now includes PERF-09's source fix and the
requested temporary footer button. It was opened with the existing explicit
three-worker/1024-entry test profile and direct FiiO output. The user confirmed
the button is visible and normal playback works. No new high-voice benchmark or
administrator-authentication result is claimed; both governors were already in
performance mode. See [the temporary-control record](TEMPORARY_PERFORMANCE_BUTTON.md)
for current validation, limitations and the opt-in desktop launcher. The earlier
statements above that no executable was deployed describe the earlier captures;
the original executable remains preserved. This follow-up does not close stage 0.
