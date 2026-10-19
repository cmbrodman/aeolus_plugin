# Aeolus development instructions

## Read before working

Read [README.md](README.md) and [AEOLUS_FEATURES.md](AEOLUS_FEATURES.md).
The README describes current functionality; the feature document records goals,
requirements, research and deferred decisions. It is not evidence of implementation
or benchmark results. Follow the current task; do not implement the roadmap
without a request. Documentation-only tasks must not change application behavior.

Before changing assets, dependencies, packaging or distribution claims, read
[NOTICE.md](NOTICE.md), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md),
[docs/ASSET_NOTICES.md](docs/ASSET_NOTICES.md) and
[docs/DISTRIBUTION_CHECKLIST.md](docs/DISTRIBUTION_CHECKLIST.md).

## Product and architecture

Aeolus is a C++17/JUCE pipe-organ synthesizer derived from Arthur Benilov's plugin
adaptation of Fons Adriaensen's Aeolus. This fork currently builds the standalone
application. Plugin-related classes and a CLAP extension submodule remain, but
current CMake does not enable VST3, AU or CLAP targets. Do not claim a platform or
format is validated without actually checking it.

- `Source/PluginProcessor.*`: JUCE audio/MIDI boundary, parameters and host state.
- `Source/PluginEditor.*` and `Source/ui/`: JUCE console, settings and organ editors.
- `Source/aeolus/engine.*`: organ configuration/state, MIDI dispatch, division
  rendering, resampling and output processing; `EngineGlobal` shares models,
  rankwaves, tuning and resources across instances.
- `Source/aeolus/division.*`, `stop.*`, `voice.*`: registrations, couplers,
  pooled voices and division mixing.
- `Source/aeolus/addsynth.*`, `rankwave.*`: additive models and pipe wavetables.
- `Source/aeolus/dsp/`: chiff, spatialization, filters, convolution and other DSP.
- `Source/aeolus/dspteam.*`, `rtconfig.*`: opt-in Linux stereo batch rendering
  and explicit scheduling configuration; non-Linux rendering remains serial.
- `Source/aeolus/paniccontrol.*` and `tools/aeolus_safety.py`: Linux independent
  HEALTH/Panic control and scoped process recovery. Preserve the receiver/Engine
  teardown order and guardian detachment during same-process processor replacement.
- `Source/aeolus/simd.*`, `worker.*`, `ringbuffer.h`: arithmetic dispatch,
  background work and bounded single-producer/single-consumer queues.
- `Resources/`: embedded organ configuration, stop models, IRs, fonts and icons.
- `Model/`: offline Python modelling experiments, not the application runtime.

The engine synthesizes at 44.1 kHz in 64-sample subframes and converts to the
external sample rate. The default voice pool has 512 entries; an explicit
environment setting can resize it. Stereo processing
includes spatialization and convolution; `WITH_MULTIBUS_OUTPUT` instead builds
fixed eight-mono-bus output without those stereo effects. This is not the planned
runtime routing to four configurable stereo pairs.

## Sound and real-time requirements

Preserve pipe timbre, chiff, envelopes, releases, tuning, spatial image, stop and
coupler semantics, and existing stereo/convolution behavior. Do not silently
reduce quality, drop notes or truncate tails to improve performance. Profile first
and compare repeatable MIDI renders before making optimization claims.

- Keep new allocation, file/network I/O, GUI calls and blocking synchronization
  out of audio callbacks. Prepare routing, filters and IR data outside processing
  and publish changes with safe ownership and bounded transitions.
- Respect MIDI event sample offsets and queue producer/consumer contracts. Handle
  overflow and device loss explicitly so note-offs cannot silently disappear.
- Keep voices preallocated and recycle only after audible tails finish. Do not
  duplicate pipe synthesis merely to route a signal to multiple destinations.
- Treat callback/UI/worker lifetimes and shared global resources explicitly;
  delayed callbacks must not outlive their owners.
- Smooth audible parameter/routing changes. Verify non-44.1 kHz operation and
  physical channel mapping when changing output or reverb processing.
- Existing code is not proof that these requirements are already satisfied.
  Report confirmed defects separately from suspected risks and proposed fixes.

## Feature boundaries

The roadmap targets a portable USB-MIDI organ and Linux appliance on x86-64 and
ARM64. Planned work includes deterministic benchmarks, Off/Algorithmic/Convolution
reverb, runtime stereo-pair routing and fallback, crossover/calibration controls,
touchscreen improvements and measured SIMD/multicore optimization.

Preserve saved multichannel profiles during stereo fallback. A single active pair
must retain original stereo behavior; multiple pairs must be independently
configurable. Keep room/reverb processing separate from speaker routing and EQ.
Qt/QML and alternative device/DSP libraries are research candidates, not approved
replacements. Pipe-onset staggering is deferred and requires measured evidence
and explicit user approval as stated in the feature document.

Update documentation when behavior changes. Distinguish implemented behavior,
planned requirements and actual validation; never mark a feature complete based
only on a UI control, prototype or proposed design.

## Configuration and assets

Organ configuration/state normally use the platform's Documents/Aeolus directory.
If either organ file exists beside the executable, that directory takes precedence
when the data directory is first selected. Preserve custom
`organ_config.json`, `organ_state.json`, stop models and user preferences.
Editor-created footage variants use the `Stops` subdirectory. Validate input and
preserve existing data on failed edits or loads. Do not overwrite a user's organ
or settings while testing; use isolated test data.

Keep original code notices and embedded stop author/comment fields. Application
code, JUCE, fonts, recordings and contributed models have distinct license terms;
the root GPL text does not cover them all. Preserve the asset provenance inventory
and recheck hashes when assets change. Never describe the whole dependency set as
GPL-only or claim complete release clearance from the notice documents alone.

## Build and verification

The build requires CMake 3.20+, a C++17 compiler, initialized submodules and the
platform dependencies used by JUCE. Inspect `CMakeLists.txt` and
`.github/workflows/build.yml` for the current target/options and CI setup.

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
```

`WITH_ASIO` and `WITH_MULTIBUS_OUTPUT` default to OFF. The ASIO directory contains
download instructions, not an SDK implementation; Windows CI downloads the legacy
SDK separately. Check its exact terms before distributing an ASIO build. Current
CI builds on Linux, macOS and Windows but does not establish audio reliability or
hardware support. Do not initialize dependencies or launch a build solely for a
documentation edit.

`AEOLUS_BUILD_TESTS=ON` adds the `aeolus_tests` executable and the CTest
`audio_regressions` test, using a build-directory profile. Run CTest under
`xvfb-run -a` on headless Linux. Keep `AEOLUS_TEST_DATA_DIRECTORY` pointed only at
test-owned files; the override exists only in test-enabled builds.
`Model/test.py` is separate offline modelling work. Use checks appropriate to
each change:

- DSP/MIDI: deterministic renders with nonzero event offsets, repeated notes,
  releases, dense registrations, couplers, panic and queue overflow; compare
  reference output and tails, then test real devices when available.
- Performance: uncapped p50/p99/worst callback times, deadline misses, xruns,
  sustainable voice counts, sample rate/buffer size and target hardware. The
  existing smoothed/capped CPU meter is insufficient evidence.
- UI/state: on an authenticated Linux desktop, `AEOLUS_TEST_POPUP_WORKFLOW=1`
  additionally exercises closing the editor with its settings menu pending. Bare
  Xvfb lacks the window-manager atoms required by JUCE popups. Exercise the actual
  editor workflow, cancellation, closing pending
  menus, save/restart round trips and preservation of custom organs. Use Xvfb
  where appropriate; actual touch and audio hardware need their own checks.
- Documentation: verify local links, consistency with code/history and
  `git diff --check`. Do not rebuild audio for prose-only changes.

Linux production standalone builds ship adjacent Python safety helpers; keep them
with the executable. Independent supervision requires Python 3.9+ with pidfds and
Linux 5.3+; test-enabled builds opt in explicitly and must use isolated preferences.
Do not distribute a test-enabled executable as the normal application.

Report checks actually run and their limits. Do not present historical CI results,
static inspection or failed test launches as current functional validation.

## Workspace, permissions and collaboration

Inspect the branch, status and unrelated edits before modifying files. Search with
`rg` and quote paths. Preserve user edits. This checkout is an Orca worktree; use
Orca for additional worktrees only when requested. Never build or edit in another
agent's checkout, use bare `git stash`, overwrite concurrent work or force-push.

Never commit directly to `main` or `master`. For a new task branch use
`codex/<topic>` or `claude/<topic>` unless the user supplies a branch; an existing
user-assigned working branch may be retained. Keep the user's Git author identity.

Read-only inspection, scoped local edits and proportionate checks are authorized
by the task. Ask before pushes, PR creation/merges, deployment, publishing,
purchases, destructive operations outside the task or external messages, unless
already explicitly authorized. Do not expose secrets or weaken host controls.

Work solo unless the user explicitly requests delegation or assigns a coordinating
role. When using Orca, first read the installed orchestration skill and follow the
version-matched guide; never invoke bare `orca` outside managed terminals. A
worker follows its dispatch scope, does not delegate further and reports its
checks and remaining issues. Parallel workers need disjoint write ownership and
separate build directories. Avoid concurrent builds in one directory and keep
machine-wide resource use modest.

Before delivery, review the diff for scope and unintended behavior changes.
Summarize changes, validation and material limitations concisely. Complete the
requested work without expanding it into unrelated cleanup or backlog features.
