# Aeolus Standalone — Feature Roadmap and Design Notes

**Status:** Working feature specification / planning document  
**Started:** 2026-10-15  
**Repository:** https://github.com/cmbrodman/aeolus_plugin  
**Scope:** Portable USB-MIDI organ, Linux appliance, performance optimization, and configurable multichannel audio.

## 1. Project goals

- Build a reliable, portable, standalone Aeolus pipe organ with a touchscreen-friendly interface.
- Support USB MIDI input, potentially up to six MIDI channels, including manuals, pedalboard, and registration controls.
- Preserve existing stereo spatialization and convolution-reverb behavior when operating with two audio channels.
- Scale to as many as eight physical audio channels, arranged as four configurable stereo pairs.
- Improve full-registration clarity using flexible stop/rank assignments and/or frequency-based speaker routing.
- Optimize for predictable real-time audio, fast startup, and minimal background activity.
- Keep changes portable across x86-64 and ARM64 Linux and avoid disrupting unrelated existing functionality.

## 2. Candidate hardware and allocation

| Device | CPU | Current consideration |
|---|---|---|
| Microsoft Surface Pro 4 | Intel Core i5-6300U, 2 cores / 4 threads | Debian 13 installed; integrated touchscreen and analog audio; strong initial test platform |
| Khadas VIM4 (early version) | Amlogic A311D2, 4 Cortex-A73 + 4 Cortex-A53 | Strong ARM candidate for optimized/multithreaded Aeolus; external audio interface likely |
| Raspberry Pi 4 | 4 Cortex-A72; 2 GHz overclock under consideration | Portable organ candidate, or solar monitoring |
| ROCK Pi 4C (RK3399-T) | 2 Cortex-A72 + 4 Cortex-A53 | Lower-priority Aeolus candidate; viable solar monitoring alternative |

**Allocation not finalized.** Solar monitoring requires comparatively modest processing but any hardware swap must preserve its display, touchscreen, serial interfaces, storage, power and enclosure compatibility. VIM4 currently has a separate solar-monitoring role, which can be reassigned if beneficial.

## 3. Standalone Linux appliance

- Prefer minimal Debian/Linux installation with only required audio, MIDI, display/touch, device management, and recovery services.
- Auto-start Aeolus after boot; aim for fast boot-to-play and no routine need for mouse/keyboard.
- Target one-button cold startup after initial setup, with persistent, validated hardware profiles and no routine password prompts; see [section 16](#16-installer-setup-hardware-profiles-and-one-button-startup--agreed-2026-10-18).
- Retain a lightweight graphical environment if using touchscreen for stop and preset control.
- Disable unnecessary services only after baseline measurement and dependency checks.
- Keep remote maintenance/recovery available during development.
- Test stock kernel and ALSA first; consider real-time kernel/scheduling tuning only if measurements warrant it.
- Avoid background updates or workloads interrupting performances.
- Do not strip the OS before collecting reproducible audio and boot benchmarks.

## 4. USB MIDI

- Support one multi-channel USB MIDI device or multiple USB MIDI controllers; exact physical arrangement TBD.
- Target up to six MIDI channels; division/control channel assignments configurable, not hard-coded.
- Auto-detect/reconnect supported MIDI devices when unplugged/replugged.
- Prevent or recover from stuck notes on disconnect or interrupted MIDI streams.
- Preserve stop/piston/program-change and other Aeolus MIDI control functions.
- Consider saved startup registration and safe all-notes-off recovery.

## 5. Audio baseline and interface choices

- Preserve the current Aeolus **full-range stereo spatial image** and existing reverb in two-channel mode.
- Test Surface onboard analog output first; compare with class-compliant USB audio if necessary.
- For VIM4, prefer class-compliant USB audio initially; HDMI LPCM or appropriately interfaced S/PDIF may be alternatives subject to Linux support.
- Do not assume a USB interface's onboard DSP can process USB playback streams; verify actual routing before purchase.
- Avoid purchasing a stereo-only interface as the permanent solution if eight-channel expansion is imminent.
- Evaluate 44.1/48 kHz, 256-sample then 128-sample buffers, with callback timing and xrun measurements.

## 6. Multichannel requirements — agreed behavior

### 6.1 Stereo pairs

- Support up to **four stereo output pairs**: 1–2, 3–4, 5–6, 7–8.
- **One active pair:** Pair 1 is the original, full-range stereo Aeolus output, with original spatialization and reverb.
- **Two or more active pairs:** **Pair 1 becomes fully configurable, exactly like every other active pair.** It must not remain permanently fixed to full-range.
- Every pair can be configured independently; stereo left/right spatial positioning is preserved within each pair.
- Pair count may be 1, 2, 3, or 4 according to device capabilities.
- Automatically fall back to original stereo on a two-channel device **without overwriting** the saved multichannel configuration. Restore multichannel settings when supported hardware returns.
- Do not blindly duplicate full-range signal to every newly enabled pair at full level; use explicit defaults and safe gain management.

### 6.2 Routing modes

- **Full-range:** Entire assigned stereo content without frequency splitting.
- **Stop/rank assignment:** Choose which organ stops feed a stereo pair, retaining stereo spatialization; a stop may feed multiple pairs with independent gains.
- **Frequency-based:** Configurable high-pass/low-pass filtering and crossover bands for each pair.
- **Hybrid (future):** Frequency filtering for selected stops/ranks, or combinations of stop and frequency rules.
- User currently favors **simpler frequency-based routing first**, while preserving stop-based routing as a planned option.
- Avoid complex per-pipe routing UI for the initial implementation. Mixture stops can contain multiple pitches/breaks; filtering the resulting audio avoids requiring special mixture-note rules at first.

### 6.3 Frequency splitting and crossover integrity

- Low-frequency energy may be routed to larger speakers while mid/high harmonics remain clear on smaller speakers.
- Preserve complementary bands with no unintended holes or peaks near crossover frequencies.
- Start by evaluating Linkwitz–Riley fourth-order (LR4) filters; provide configurable crossover frequency and slope where practical.
- Provide per-pair gain, delay, polarity inversion, mute/solo, and filter bypass.
- Correct **acoustic** summation depends on physical speaker response, distance, room, and receiver processing; an electrically flat crossover alone cannot guarantee a flat measured response.
- Add calibration/test tools such as pink noise, swept sine, and independent output mute/solo, with safe output levels.
- Prefer filters applied to a small number of mixed stereo buses, not separate crossovers on every active pipe, to control CPU cost.
- Avoid losing pipe spatial separation by collapsing all voices into stereo before stop-specific routing; keep suitable intermediate per-stop/per-group buses or precomputed voice destinations where needed.

### 6.4 Reverb and speaker processing

- Keep **pipe/stop routing**, **speaker EQ/crossover/delay/limiting**, and **room/reverb processing** conceptually separate.
- Preserve original stereo convolution in two-channel fallback.
- In multichannel mode, make dry and wet return assignments configurable, potentially with dedicated reverb speaker pair(s).
- Avoid applying identical full-strength wet signal to all channels by default.
- External algorithmic DSP reverb is an option but is not automatically equivalent to Aeolus convolution impulse-response reverb.
- Explore CPU savings from changing reverb only after profiling.

## 7. Existing Aeolus code considerations

- Repository already has `WITH_MULTIBUS_OUTPUT` option for **eight mono output buses**, with a fixed pipe-position spatial assignment.
- In existing multibus mode, convolution reverb and ordinary stereo spatialization are disabled. **This is a starting point, not the final requested stereo-pair routing behavior.**
- Proposed architecture should eventually replace rigid stereo-versus-multibus compile-time behavior with runtime-capable routing, subject to safe audio device reinitialization.
- Keep a single synthesized instance per pipe; route or mix it to one or more outputs without duplicating synthesis work.
- Build routing tables outside the real-time callback; no dynamic allocation or blocking locks on the audio thread.
- Crossfade or otherwise smoothly transition routing/filter changes to avoid clicks.
- Persist settings and provide user-selectable output profiles.

## 8. Processing and benchmarking

- Establish baseline on Surface Pro 4 (Debian 13), VIM4, Pi 4, and optionally Rock Pi.
- Test reproducible registrations and active pipe-voice counts (e.g. 50/100/200/300+), both reverb on and off.
- Measure mean/peak audio callback time, xruns, active voices, CPU core utilization/frequency, thermal throttling, and startup time.
- Compare Pi 4 at stock 1.8 GHz and intended 2.0 GHz only after confirming stable clock/cooling.
- Current engine's division/voice rendering is largely serial; explore fixed, persistent real-time DSP worker threads, division-level parallelism first, then voice chunking only if warranted.
- Avoid generic thread pools, thread creation, heap allocation, or mutex blocking inside audio callbacks.
- On VIM4, evaluate using A73 cores for heavy real-time work and A53 cores for background tasks; validate scheduling rather than assume affinity is automatically optimal.
- Inspect and benchmark existing x86 SSE/AVX/FMA code paths and potential ARM NEON optimizations.
- Do not claim hardware-specific performance without measured results.

## 9. Existing home theater receiver as a test platform

- User has a **7.1-channel home theater receiver/amplifier**, currently in storage; make/model and available inputs to be checked this weekend.
- Receiver supports Dolby Digital and DTS, and has S/PDIF input.
- Standard S/PDIF carries stereo PCM, or compressed multichannel formats such as Dolby Digital 5.1/DTS; it does **not** carry 6–8 channels of ordinary uncompressed PCM.
- Real-time AC-3 over S/PDIF is a possible experiment, but frame buffering, encoding/decoding, and receiver latency may be unacceptable for live organ performance.
- Prefer HDMI multichannel LPCM if the receiver has compatible HDMI inputs and Linux exposes them; alternatively look for multichannel analog inputs or use a USB multichannel interface.
- In 5.1 mode, center and LFE are not a normal third stereo pair; LFE may be filtered/level-managed. In 7.1 LPCM mode, verify actual physical channel map and receiver processing.
- Verify receiver's direct/pure modes, bass management, EQ, delays, and speaker mapping before trusting crossover measurements.

## 10. User interface concept

- An **Audio Routing** menu in Aeolus, touch-friendly.
- Select active pair count; select each pair (1–2, 3–4, 5–6, 7–8).
- Configure mode: full-range, frequency, stop/rank, eventually hybrid.
- Frequency mode: high-pass, low-pass, slope, level; show relationship to complementary bands.
- Stop mode: division → stop/rank assignment matrix, per-pair gains.
- Global stereo fallback and saved multichannel profiles.
- Speaker calibration page: gain, delay, polarity, mute/solo, test signals.
- Safe defaults, undo/revert, persistent configuration, and no clicks on changes.

## 11. Implementation phases

1. **Baseline:** Build/run current standalone on candidate Linux hardware, verify USB MIDI and two-channel audio, instrument performance.
2. **Audio abstraction:** Runtime channel-count detection, stereo fallback, persistent device/output profiles, pair-level routing framework.
3. **Frequency routing:** Add shared crossover/filter processing, safe complementary routing, gains, delay/polarity, and calibration tools.
4. **Stop routing:** Introduce stop/rank-aware stereo bus routing before irreversible mixing; UI assignments and saved presets.
5. **Reverb routing:** Preserve stereo convolution, then add flexible multichannel dry/wet buses and optional external processing.
6. **Performance:** Implement measured multicore/NEON optimizations as needed; retest on all platforms.
7. **Appliance polish:** Minimal Linux services, auto-start, MIDI hotplug/recovery, touchscreen usability, boot optimization. Include the in-app installer, persistent system profiles, limit meters and cold-start acceptance gates in [section 16](#16-installer-setup-hardware-profiles-and-one-button-startup--agreed-2026-10-18); performance-phase scheduling controls must integrate with that setup model.

## 12. Open questions / decisions deferred

- Receiver make/model, HDMI LPCM support, analog multichannel inputs, and real speaker configuration.
- Which hardware becomes permanent Aeolus host versus solar monitor.
- USB MIDI physical topology (one multichannel interface versus multiple controllers).
- Final USB interface and amplifier/speaker topology.
- Exact crossover/filter defaults and whether to add acoustic measurement support.
- Specific stop/rank routing rules, especially mixtures and compound stops.
- Reverb strategy for >2 channels, including dedicated wet outputs or multichannel convolution.
- Which settings should be global device profiles versus saved per-organ registrations.

## 13. Working agreements

- Preserve existing stereo functionality and unrelated changes.
- Make targeted, testable modifications with clear validation and rollback paths.
- Distinguish confirmed code behavior, planned requirements, and untested ideas.
- Do not treat this feature file as implementation or a benchmark result.

## 14. Core performance objective — agreed 2026-10-15

**Primary goal:** Produce the maximum practical number of simultaneously sounding, independently modeled organ pipes with the minimum compute cost **without perceptible loss of sound quality**. Optimize for musical reliability, not synthetic peak throughput. Maintain the existing sonic character and offer measurable, reversible changes.

- Measure active pipe voices, throughput, p50/p99/worst processing times, missed deadlines, output latency, and audio quality versus a reference render.
- Preserve chiff, attack/release behavior, harmonic content, tuning, spatial image, and registration response. No silent dropping of pipes or unapproved quality reductions.
- Profile before optimizing; compare the same deterministic MIDI/registration tests and reference recordings across platforms.

### 14.1 Algorithmic reverb — committed feature

- Add selectable **Off / Algorithmic / Convolution** reverb modes. Retain current convolution reverb and impulse-response compatibility for users with adequate compute.
- Implement a low-cost, high-quality algorithmic reverb (evaluate feedback delay networks and appropriate diffusion/early-reflection designs), with adjustable RT60, pre-delay, room size, early-reflection level, diffusion, damping, frequency-dependent decay, wet/dry balance, and stereo width; expose modulation controls only if justified by the algorithm.
- Keep each engine's settings and presets separate; avoid clicks when switching modes. Preserve the original stereo spatialization and pipe sound.
- Support future per-division sends and multichannel wet routing without tying room simulation to speaker crossover/routing.
- Benchmark algorithmic vs convolution CPU cost, tail quality, latency, and clarity under dense registrations. Do not claim that algorithmic reverb always outperforms convolution without measurement.

### 14.2 Performance optimization workstream

1. Add low-overhead instrumentation and offline deterministic render/quality comparisons before changing the DSP.
2. Verify Release compiler flags, scalar fallback and existing x86 SIMD dispatch. Evaluate compiler auto-vectorization and implement/test ARM64 NEON paths for mixing, vector arithmetic, complex multiply, FFT and other demonstrated hotspots.
3. Profile `Voice::process`, chiff, delay lines, spatialization, wavetable playback and envelope/release handling; bypass genuinely inactive stages and precompute stable coefficients where mathematically safe.
4. Profile repeated buffer clears, copies and division mixing; reduce memory traffic and improve voice traversal/cache locality without introducing audio-thread allocations.
5. Keep voice pools preallocated, recycle only after audible tails finish, and preserve exact MIDI/stop/coupler semantics.
6. Add persistent multicore workers with private accumulation buffers and deterministic summing. Start with division jobs, then test bounded voice chunks for load balancing; retain a low-overhead single-thread fast path when appropriate.
7. Consider optional quality/performance profiles only with explicit user control and documented sonic differences; no automatic degradation by default.
8. Tune Linux audio scheduling, CPU governor, affinity and thermals after baseline measurements; test actual xruns and end-to-end key-to-sound latency.

### 14.3 Late-stage optional experiment — pipe-onset staggering

**Status: deferred / not required for initial implementation.** Investigate only near the end of development, and only if profiling demonstrates that simultaneous activation of large chords/full registrations causes meaningful callback-time spikes after the primary sound-preserving optimizations are complete. It may never be necessary.

- Objective: determine whether a small, bounded, deterministic onset spread can reduce *activation-time* CPU spikes without perceptible loss of musical responsiveness or sound quality.
- Do not assume that staggering reduces steady-state DSP cost or increases sustainable polyphony; all voices still need processing once sounding.
- If justified, compare baseline simultaneous attacks with optional 5 ms and 10 ms maximum spreads, including dense mixtures, couplers, repeated notes and releases. Start with zero additional spread as the default.
- Keep intentional musical onset timing separate from thread scheduling. Workers must still meet hard audio deadlines; do not allow late threads to cause glitches, unbounded waits or incomplete output blocks.
- Evaluate p99 and worst-case callback time, missed deadlines, note-to-sound latency and blind listening results. Reject the feature if benefits are negligible or timing/quality suffers.
- Do not implement unless measured evidence supports it and the user approves proceeding.

### 14.4 Acceptance criteria (to refine after baselines)

- Original stereo/convolution configuration remains available and regression-tested.
- Algorithmic reverb sounds credible on sustained organ tones and dense chords, with no audible zipper noise during parameter changes.
- Optimized renders match reference output when operating in sound-preserving modes, within appropriate numerical tolerance; no lost notes or truncated tails.
- Measured increases in sustainable pipe count at a defined sample rate, buffer size and maximum deadline-miss rate, reported for Surface i5, VIM4, Pi 4 and Rock Pi where available.
- If the deferred onset-staggering experiment is ever implemented, it must be bounded, repeatable, disableable, and shown to improve measured attack-time spikes without audible degradation; worker scheduling delays never masquerade as acoustic realism.

## Touchscreen UI redesign — proposed phase (2026-10-15)

**Goal:** A compact, responsive, user-friendly organ console on 10–14 inch touchscreens, retaining mouse/keyboard access for configuration. Requirements below are planned, not implemented.

- **Large, button-oriented menus:** Enlarge touch targets, menu items and essential labels; provide clear selected, active and disabled states. Advanced setup remains practical with mouse and keyboard.
- **Optional touch-and-drag stop selection:** In performance mode, allow one continuous touch gesture to set several stops. Determine the intended state from the first stop (activate if off, deactivate if on), apply that same state to each newly encountered stop, and never repeatedly toggle the same stop within one gesture. Provide an explicit on/off preference; keep normal tap operation.
- **Updated color scheme:** Improve contrast, legibility and state recognition; offer theme options and do not rely on color alone to communicate stop state.
- **Space-efficient 10–14-inch layouts:** Reduce unused padding/empty areas; adapt to resolution and orientation; support fullscreen/borderless performance mode where supported by the platform. Do not make touch targets too small while reducing whitespace.
- **Simplified performance UI:** Focus on stops, divisions, registrations and essential status. Keep audio/MIDI/reverb/routing and other advanced configuration in a separate setup view.
- **Reduced-feature edition or profile:** Prefer configurable feature visibility first, with an optional lean build later if it provides measurable resource or maintenance benefits. Preserve full-feature capability and avoid removing functionality prematurely.
- **Quality and reliability:** No GUI work or touch gestures may block real-time audio; verify gesture behavior against accidental registration changes, touchscreen sizes, mouse use and keyboard access.

**Status:** Initial requirements recorded; additional UI requests expected. Mockups, final themes, navigation structure and implementation sequence remain open.

---

## 15. Open-source module research and prioritized adoption plan — 2026-10-15

**Status:** Research recommendations, not implemented features or measured performance results. This addendum preserves all earlier requirements and phases. Priorities below are suggested additions to section 11, not authorization to replace working behavior.

### 15.1 Verified repository baseline and architecture implications

Reviewed `master` at [2fad8f123758d2d4f1ce4029599bf202f5dc6f34](https://github.com/cmbrodman/aeolus_plugin/commit/2fad8f123758d2d4f1ce4029599bf202f5dc6f34), dated 2026-09-05.

- [CMakeLists.txt](https://github.com/cmbrodman/aeolus_plugin/blob/2fad8f123758d2d4f1ce4029599bf202f5dc6f34/CMakeLists.txt) uses C++17, JUCE, and currently selects **Standalone** in `plugin_formats`. The README describes additional plugin formats; do not assume the current build produces them. Repeated project/options declarations are a build-cleanup verification item.
- [PluginProcessor.cpp](https://github.com/cmbrodman/aeolus_plugin/blob/2fad8f123758d2d4f1ce4029599bf202f5dc6f34/Source/PluginProcessor.cpp) already handles MIDI and callback-load measurement, with separate stereo and multibus processing branches. Its load meter is smoothed and capped at 1.0; add uncapped timing distributions and deadline-miss counts before optimization. It iterates MIDI events without using their sample offsets; verify timing semantics in a deterministic render test.
- [simd.h](https://github.com/cmbrodman/aeolus_plugin/blob/2fad8f123758d2d4f1ce4029599bf202f5dc6f34/Source/aeolus/simd.h) already provides runtime x86 dispatch and documents buffer-alignment/size requirements. Extend or replace proven hotspots, rather than adding a second vector layer everywhere.
- [DivisionView.cpp](https://github.com/cmbrodman/aeolus_plugin/blob/2fad8f123758d2d4f1ce4029599bf202f5dc6f34/Source/ui/DivisionView.cpp) already contains customized stop/coupler/preset controls. Audit these changes before porting UI behavior. Engine and UI currently reference JUCE types; changing the display toolkit alone will not remove JUCE.

### 15.2 Priority and integration shortlist

**P0 = establish before implementation choices; P1 = first prototypes/features; P2 = adopt only after measurement; P3 = optional later research.** License labels describe the reviewed upstream materials; lock exact versions and audit included files before release.

| Priority / roadmap link | Module and sources | License / compatibility | Integration recommendation and verification |
|---|---|---|---|
| **P0 — phase 1; section 14.2** | [Tracy](https://github.com/wolfpld/tracy), [license](https://github.com/wolfpld/tracy/blob/master/LICENSE) | BSD-3-Clause; compatible with GPLv3 with notices retained. | Optional development build only. Profile engine, divisions, reverb and worker synchronization. Keep instrumentation bounded; compare enabled/disabled overhead and validate uninstrumented xruns. Never stream logs or wait on a profiler from the callback. |
| **P0 — phase 1; section 14.2** | [Google Benchmark](https://github.com/google/benchmark), [license](https://github.com/google/benchmark/blob/main/LICENSE) | Apache-2.0; GPLv3-compatible, retain applicable notices. | Separate executable for SIMD, filters and mixing kernels. Pair with deterministic whole-engine MIDI renders and actual device tests: microbenchmarks cannot establish live reliability or end-to-end latency. |
| **P1 — UI prototype alongside phase 1; appliance polish in phase 7** | [Qt 6 Quick/QML](https://doc.qt.io/qt-6/qtquick-index.html), [Quick Controls](https://doc.qt.io/qt-6/qtquickcontrols-index.html), [Qt licensing](https://doc.qt.io/qt-6/licensing.html) | Essential modules offer LGPLv3/GPLv3/commercial options; confirm each chosen module and bundled third-party component. Compatible with the intended GPLv3 application when obligations are met. | Preferred candidate to prototype a standalone touchscreen frontend: Qt Core, Gui, Qml, Quick, QuickControls2 and layouts. Start with one division, setup navigation, fullscreen and responsive sizing. Put a narrow C++ state/command adapter between QML and engine; no QObject, JavaScript, rendering or GUI locks in audio processing. Keep the existing UI until hardware tests justify migration. Measure startup, RAM, frame pacing and xruns on Surface and ARM64; test actual GPU drivers and display stack. |
| **P1 — same UI prototype** | [Qt Pointer Handlers](https://doc.qt.io/qt-6/qml-qtquick-pointerhandler.html), [DragHandler](https://doc.qt.io/qt-6/qml-qtquick-draghandler.html) | Part of Qt Quick; same selected module license. No extra gesture framework required. | Use board-level pointer tracking and hit-testing; a handler does not automatically implement painting across stops. Capture the initial desired stop state and track visited stop IDs per gesture; use explicit set-state commands. Resolve gesture grabs versus scrolling, tap-to-click duplication, cancellation and simultaneous fingers. Verify fast drags across gaps, reverse motion, touch release outside the board, popup blocking, mouse/keyboard operation and MIDI state feedback. |
| **P1 — phases 1–2; USB MIDI** | Existing [JUCE audio/MIDI devices module at pinned commit](https://github.com/juce-framework/JUCE/blob/10a589619b452c261b2940767eb253171eb5a823/modules/juce_audio_devices/juce_audio_devices.h) | Pinned JUCE 8.0.7: AGPLv3/commercial; see section 15.5. | Lowest-change baseline for MIDI, device enumeration and ALSA audio. Retain initially even during a Qt prototype. Verify multi-device identity, six logical MIDI channels, disconnect panic, reconnection, CC98/program changes, sustain/couplers, duplicate device names and timestamp handling. Do not open independent device streams from both UI and engine. |
| **P2 — phase 2 if removing JUCE device dependency** | [RtMidi](https://github.com/thestk/rtmidi), [license](https://github.com/thestk/rtmidi/blob/master/LICENSE) | MIT-like permissive RtMidi license. Sending modifications upstream is requested but explicitly nonbinding. GPLv3-compatible with notices. | Good standalone MIDI I/O adapter for ALSA/JACK and cross-platform builds. Not a registration engine or reconnect policy. Convert incoming timestamps into engine sample scheduling; use bounded queues and explicit overflow recovery. Verify port rescanning/reconnect and any dynamic allocations in callback adapters. Adds little value while JUCE already meets these needs. |
| **P1 audit / P2 replacement — phases 1–2** | [ALSA lib](https://github.com/alsa-project/alsa-lib), [COPYING](https://github.com/alsa-project/alsa-lib/blob/master/COPYING), [PCM API](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html) | LGPL-2.1 in upstream COPYING; check per-file notices and any later-version grants. Distinguish userspace library from kernel licensing. | Use existing JUCE ALSA first. Direct alsa-lib is the lean Linux-only alternative if device control requires it. Implement format/channel negotiation, short writes, xrun/suspend recovery, safe device-loss handling and channel-map validation. Test physical latency and callback headroom at 44.1/48 kHz and 256/128 frames; period size alone is not total latency. |
| **P2 — alternative to direct ALSA in phase 2** | [RtAudio](https://github.com/thestk/rtaudio), [license](https://github.com/thestk/rtaudio/blob/master/LICENSE) | MIT-like permissive RtAudio license; upstream-modification request is nonbinding. Backends/SDKs have separate terms. | Cross-platform audio abstraction if JUCE is removed. Select ALSA explicitly on Linux and validate actual output count/channel order, device reconnection and buffer negotiation. Choose this OR a direct ALSA backend for the first replacement; avoid maintaining both without a demonstrated need. |
| **P1 — section 14.1; phase 5** | [zita-rev1](https://kokkinizita.linuxaudio.org/linuxaudio/), [official behavior guide](https://kokkinizita.linuxaudio.org/linuxaudio/zita-rev1-doc/quickguide.html), [source downloads](https://kokkinizita.linuxaudio.org/linuxaudio/downloads/) | Official download listing identifies GPL; inspect COPYING and source headers in the selected archive for exact version and “or later” grant before copying DSP code. Do not treat unspecified GPL as GPLv3 verification. | Strong first audition for organ hall reverb: derived from the original Aeolus reverb, stereo/ambisonic operation. Extract DSP behind a small prepare/process/reset/tail interface; do not import its JACK/GUI application architecture. Controls do not cover every requested parameter, and most parameters are not smoothed; add smoothing or controlled transitions. Preserve convolution and per-mode presets. |
| **P1 comparative prototype / P2 production — section 14.1** | [Signalsmith Basics](https://github.com/Signalsmith-Audio/basics), [reverb source](https://github.com/Signalsmith-Audio/basics/blob/main/include/signalsmith-basics/reverb.h), [DSP primitives](https://github.com/Signalsmith-Audio/dsp) | MIT for Basics and DSP; include license notices and audit helper dependencies. | Useful FDN alternative and reusable delays/filters/mixers. Reviewed reverb is **stereo only**, despite eight internal delay channels. Its decay parameter is RT20, not RT60: account for the nominal factor of three in UI mapping. Configure/preallocate off the callback. Test changing room size and other parameters under load. FluidSynth's [integration notes](https://github.com/FluidSynth/fluidsynth/blob/master/doc/wiki/reverbators.md) report substantial runtime variation; treat low CPU cost as unproven until target-hardware measurement. |
| **P1 — phase 3** | [JUCE DSP LinkwitzRileyFilter](https://docs.juce.com/master/classjuce_1_1dsp_1_1LinkwitzRileyFilter.html), [pinned source](https://github.com/juce-framework/JUCE/blob/10a589619b452c261b2940767eb253171eb5a823/modules/juce_dsp/processors/juce_LinkwitzRileyFilter.h) | Pinned JUCE module AGPLv3/commercial. | Smallest integration step while JUCE remains. Add `juce_dsp` to the build if required; prepare channel state outside processing. LR4 provides complementary low/high outputs, but a three/four-way cascade also needs phase alignment of earlier branches. Verify electrical summed magnitude and phase, stereo independence, transitions, delay/polarity and physical output mapping. |
| **P2 — phase 6** | [xsimd](https://github.com/xtensor-stack/xsimd), [license](https://github.com/xtensor-stack/xsimd/blob/master/LICENSE) | BSD-3-Clause; GPLv3-compatible with attribution. | Preferred first portable-SIMD experiment for existing arithmetic/mixing hotspots across x86 and ARM NEON. Wrap current scalar interface; compare with existing intrinsics and compiler vectorization. Verify unaligned buffers, remainder lengths, CPU/OS dispatch support, denormals and tolerances; do not compile the entire distributed binary for the build machine's instruction set. |
| **P2 alternative — phase 6** | [Google Highway](https://github.com/google/highway), [license](https://github.com/google/highway/blob/master/LICENSE) | Choice of Apache-2.0 or BSD-3-Clause in current upstream license; record chosen route and version. | Alternative if multi-target dispatch is easier for the measured kernels. Evaluate one small kernel against xsimd and existing code, then choose one abstraction. Do not assume vectorization improves scattered wavetable accesses or tiny buffers. |
| **P3 — after phase 3/5 routing works** | [IEM Plug-in Suite](https://github.com/tu-studio/IEMPluginSuite), [canonical upstream](https://git.iem.at/audioplugins/IEMPluginSuite/) | Repository identifies GPL-3.0; audit copied classes, exact grants and dependencies. JUCE remains a separate license consideration. | Reference implementation/tooling for matrices, loudspeaker decoding and more advanced spatial processing. Not necessary for four ordinary stereo pairs. Treat Ambisonic normalization/order and decoding as a separate future feature; do not substitute it for the agreed pair-routing architecture. |

### 15.3 Proposed additions to implementation phases

1. **Phase 1 / P0:** Lock repository/submodule revisions, resolve license inventory, build a deterministic MIDI render harness and record uncapped callback distributions/xruns. Establish the existing JUCE ALSA/MIDI baseline before adding a replacement backend.
2. **Parallel UI workstream / P1:** Prototype Qt/QML with one division and the exact optional drag-to-set behavior. Compare against enlarging the existing JUCE controls. Select the UI after real 10–14-inch touchscreen and ARM graphics tests; Qt is a candidate, not an approved full rewrite.
3. **Phase 2 / P1:** Keep backend interfaces independent of the GUI. Add runtime device capabilities/profiles and safe stereo fallback. If JUCE removal becomes a goal, stage RtMidi plus either RtAudio or direct ALSA behind those interfaces.
4. **Phase 3 / P1:** Prototype pair-level LR4 routing, gain/delay/polarity and safe test signals. Validate two-, three- and four-way complementary sums, not only separate filter responses. Preserve the original stereo path for one active pair.
5. **Phase 5 / P1:** Audition zita-rev1 and Signalsmith against current convolution with sustained bass, mixtures, full registrations and parameter changes. Choose on sound and measured worst-case processing cost; retain Off/Algorithmic/Convolution modes and separate settings. Add wet routing independently from speaker filters.
6. **Phase 6 / P2:** Use profiles to choose xsimd or Highway kernels. Retest scalar/x86/ARM64 quality and realistic pipe counts. Persistent multicore workers remain a separate measured experiment; no generic task framework is recommended for the callback.
7. **Phase 7:** Recheck hotplug, cold boot, fullscreen, setup/recovery and minimal installation only after audio/UI choices stabilize. Hide unused features before considering lean build variants.

### 15.4 Verification gates before adoption

- [ ] Record dependency version/commit, license text, source URL, build flags and transitive dependencies. Prefer packaged/pinned stable versions supported by the target OS; current online Qt documentation may describe APIs newer than Debian's packages.
- [ ] Build Release for Surface x86-64 and at least one ARM64 target. Confirm scalar fallback and that optional profiler/benchmark targets are absent from production builds.
- [ ] Run a fixed MIDI corpus with notes/releases at nonzero block offsets, dense registration changes, couplers, repeated notes, disconnect/reconnect and queue-overflow scenarios. No lost/stuck notes or truncated release tails.
- [ ] Compare original stereo/convolution output to reference renders. Use numerical tolerances for equivalent DSP and blind listening for intentional algorithm changes; report differences rather than hiding them.
- [ ] Measure p50/p99/worst callback time, deadline misses, xruns, RAM, thermals and sustainable pipe count at 44.1/48 kHz and 256/128 frames. At 48 kHz, 128 frames is about 2.67 ms of processing time; reserve headroom instead of treating that entire interval as available DSP budget.
- [ ] Test 2/4/6/8 physical channels, real channel order, absent/returning hardware and restoration of saved multichannel profiles. Do not mistake eight mono pipe-position buses for four configurable stereo pairs.
- [ ] Verify no new allocation, blocking synchronization, disk/network operations or GUI calls in realtime processing. Publish immutable routing/control snapshots with a safe lifetime mechanism; a “lock-free” label alone does not prove correctness.
- [ ] Test reverb stability/tails, switching crossfades, silent input/denormals, bass clarity and worst-case parameter changes. Map unsupported requested controls explicitly rather than inventing equivalent knobs.
- [ ] Test touch on actual panels: fast and reversed drags, simultaneous fingers, cancel/focus loss, scrolling conflicts, mouse input, keyboard focus, accessible state indicators and settings persistence.

### 15.5 Licensing compatibility and release checklist

**Project baseline:** Top-level [LICENSE.txt](https://github.com/cmbrodman/aeolus_plugin/blob/master/LICENSE.txt) contains GPLv3. Reviewed project source headers expressly permit GPL version 3 or later. Preserve original notices; audit inherited Aeolus files and assets separately before assigning SPDX identifiers across the tree.

**Important pinned dependency:** JUCE submodule `10a589619b452c261b2940767eb253171eb5a823` declares version **8.0.7** and **AGPLv3/commercial**, confirmed in its [module header](https://github.com/juce-framework/JUCE/blob/10a589619b452c261b2940767eb253171eb5a823/modules/juce_audio_devices/juce_audio_devices.h) and [license](https://github.com/juce-framework/JUCE/blob/10a589619b452c261b2940767eb253171eb5a823/LICENSE.md). Do not describe this dependency as GPLv3-only or assume the repository's license file covers every dependency.

GPLv3 section 13 permits combination with AGPLv3, with AGPL network-interaction requirements applying to the combination as specified there. This does not mean relicensing all inherited project code, nor does “open source” alone satisfy distribution duties. Review the actual binary/source delivery path, preserve JUCE's notices and provide corresponding source/build materials. See [GNU license compatibility guidance](https://www.gnu.org/licenses/license-list.en.html) and [GPLv3 section 13](https://www.gnu.org/licenses/gpl-3.0.html#section13). Replacing only the editor with Qt leaves JUCE obligations in place if JUCE audio/core classes remain linked.

Qt's essential LGPLv3 option can fit this project; choose modules deliberately. Prefer dynamic system libraries for the initial Linux appliance. Retain notices and source for modifications, and satisfy replacement/relinking and installation-information requirements where applicable. Some Qt additions are GPL-only, including Virtual Keyboard in the current licensing list; audit module/plugin selection and third-party components instead of assuming all Qt packages are LGPL. Qt guidance: [module licensing](https://doc.qt.io/qt-6/licensing.html), [LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations).

MIT-like, MIT, BSD-3-Clause and Apache-2.0 candidates above are generally compatible with GPLv3 when their notices/conditions are preserved; they do not change the inherited Aeolus license into a permissive one. LGPL library obligations and AGPL combination requirements still need to be addressed. GPLv2-only code without an applicable exception or later-version permission is a compatibility blocker for this GPLv3 combination; verify exact zita archive grants before adoption.

Before distributing any binary:

- [ ] Produce a dependency/asset inventory and bundled notices, including pinned JUCE and any copied DSP/helper code, Qt plugins, fonts/icons, organ definitions and impulse responses.
- [x] Audit each bundled OpenAIR impulse response's actual license/attribution and redistribution terms. Completed 2026-10-15 for all ten recordings and all 85 stop-model files: see [asset notices](docs/ASSET_NOTICES.md) and [per-file source/license inventory](docs/asset-provenance.json). Ship those notices/license texts with releases and re-audit changed assets.
- [ ] The checked-in `asiosdk_2.3.3_2019-06-14` directory contains only download instructions, not a vendored SDK. `WITH_ASIO` is OFF by default, but Windows CI downloads/enables the legacy SDK. Audit the exact downloaded SDK and its redistribution terms for ASIO builds; newer SDK terms do not automatically apply. See `THIRD_PARTY_NOTICES.md` and `docs/DISTRIBUTION_CHECKLIST.md` (inventory corrected 2026-10-15).
- [ ] If plugin formats return later, inspect the exact VST3/AU/CLAP wrapper/SDK versions and their licenses. Current CMake's standalone scope does not establish permission for every future format.
- [ ] Supply corresponding source, patches, build/configuration scripts and required installation/relinking information for the selected distribution method. Preserve license texts and copyright notices and verify the recipient can rebuild the delivered version.

**Decision boundary:** Research supports the prototypes above. No UI rewrite, backend removal, DSP replacement, performance improvement or legal-compliance certification is claimed by this addendum. All original sound-preservation, stereo fallback and explicit-approval requirements remain in force.


## Reliability implementation update — 2026-10-17

A focused maintenance increment addresses issues in the existing engine before
larger roadmap features: host MIDI event-offset rendering (retaining the existing
64-sample synthesis quantum), note-queue overflow panic and short-note voice
reclamation, engine-owned save timers, message-thread IR preparation with atomic
ownership transfer and a 50 ms crossfade, device-rate IR resampling, Linux SSE3
kernel dispatch with scalar fallback, corrected CPU checks, and duplicate CMake
setup removal. The optional `AEOLUS_BUILD_TESTS` target covers these paths with an
isolated profile. See README for build/test commands and transition tradeoffs.

This increment does not implement algorithmic reverb, configurable stereo pairs,
MIDI device reconnect policy, ARM NEON, multicore voice rendering or the proposed
touchscreen redesign. Actual-device xrun, listening and platform checks remain
necessary; test results are reported with the change rather than inferred here.


## 16. Installer setup, hardware profiles and one-button startup — agreed 2026-10-18

**Status: installer/profile UI, admission controls, meters and persistent startup
remain planned.** The separate renderer/Panic/watchdog integration is described in
[README](README.md#integrated-multicore-rendering-and-emergency-recovery); it does not
implement the setup features below. The isolated VIM4 IRQ-placement
experiment informs this design; it is not a persistent installation. See the
[hang investigation](docs/HANG_INVESTIGATION_2026-10-18.md) for observations and
validation limits. This section extends phases 6 and 7 without authorizing an
immediate implementation or changing existing sound-preservation requirements.

### 16.1 In-app installation and administrator access

- Provide a touchscreen-friendly first-run wizard and password-protected
  **Installer / Advanced Setup** menu. Keep ordinary performance controls separate.
- Complete supported setup inside Aeolus: detect hardware, choose a profile,
  review changes, authenticate, apply, test, save and restore previous settings.
- Use system administrator authentication (polkit is a candidate) and a small,
  installed privileged helper for narrowly defined system operations. Keep Aeolus
  unprivileged; do not store passwords, run the whole UI as root or grant a general
  passwordless shell. Enforce authorization in the helper as well as the UI.
- Install protected, validated configuration and startup services with initial
  administrator approval. Normal startup may reapply the approved profile without
  requesting a password; changing privileged policy requires authorization again.
- Include authentication-agent support in the appliance installation where needed.
  For unsupported operating systems, controllers or permissions, explain the
  limitation in the app and provide exact manual setup/rollback instructions.
- Label settings by when they apply: immediately, after an audio restart, after
  an application/service restart, after login, or after a system reboot. Apply
  changes outside callbacks, with bounded transitions and recoverable failure.

### 16.2 Portable CPU, device and safety profiles

- Detect available/online CPUs and their topology/capabilities on x86-64 and ARM64;
  do not assume VIM4 core numbers, a fixed core count, or performance/efficiency
  clusters on every SBC. Include Automatic, Custom and Restore Defaults modes.
- Select audio-thread cores, DSP worker count/cores, UI/background cores and
  independent safety-control resources. Permit limiting Aeolus's CPU footprint
  so other applications can run. Distinguish affinity from exclusive reservation;
  any system-wide isolation must be explicit and separately tested.
- Discover the USB controller serving selected MIDI/audio devices and configure
  its IRQ placement where supported. Verify actual effective affinity and
  interrupt delivery, not merely a successful configuration write. Account for
  shared controllers and existing IRQ-management services; avoid competing policy.
- Expose measured scheduling/priority profiles as expert settings. Keep USB/MIDI,
  OS and recovery work able to progress during audio saturation. Do not disable
  kernel RT bandwidth protection or unrelated services as a default setup action.
- Save audio device, sample rate/buffer, MIDI assignments/reconnection policy,
  startup registration, reverb/output profile, voice limit and watchdog/Panic
  settings. Preserve custom organs and saved multichannel profiles during fallback.
- Include temperature/throttling status and a setup test covering dense sustained
  registrations, simultaneous attacks, releases, MIDI continuity and Panic. Store
  results with hardware/software identity; a voice cap alone is not a clean-audio
  guarantee. Keep diagnostic logging away from real-time processing.
- Retain independent Panic: attempt safe silence first, then terminate the exact
  unresponsive app if acknowledgement fails. Automatic application restart remains
  a later final-release feature requiring separate validation, bounded retries and
  a notes-off startup; it is not enabled by this roadmap entry.

### 16.3 Selectable voice capacity and performance meters

- Provide a configurable maximum active **pipe-voice** count with explicit
  rejection of new notes at capacity. Account for stops, mixtures and couplers;
  prefer accepting/rejecting the complete incoming note rather than producing an
  incomplete registration. Validate admission behavior for registration changes.
- Always handle releases, note-offs and Panic. Preserve existing voices and audible
  tails; do not steal voices, shorten tails or silently reduce timbre. Prepare pool
  changes outside processing; lowering the cap below current use blocks new
  admissions while existing voices finish. Show when notes are rejected.
- Add two meter bars, visually consistent with audio level indicators: **audio
  processing headroom/load** and **voice-capacity usage**. Green means comfortable
  margin, orange approaching the limit, and red very near/at capacity or missing
  audio deadlines. Choose thresholds from measured behavior; do not present
  provisional percentages as established safety limits.
- Show numeric values and text/icons alongside colors, recent callback peaks,
  deadline misses/xruns where available, and a brief retained overload/rejection
  indication. Base audio status on uncapped timing relative to the callback
  deadline; the existing smoothed, capped CPU meter is insufficient. Voice count
  is a separate capacity measure, not a substitute for processing cost.

### 16.4 Persistence, device changes and cold startup

- **Normal-use goal:** where the hardware supports it, press the power button
  from a powered-off state, boot, apply the validated profile, start Aeolus and
  reach a clearly indicated playable console without a terminal, routine password
  prompt, or manual device selection. Configure an appropriate appliance graphical
  session/startup path during installation; measure actual boot-to-play time.
- Persist the approved profile across reboot and power cycles. Reapply volatile
  IRQ/thread assignments through installed startup/device-management integration
  after the relevant hardware appears and before enabling the performance profile.
  Discover devices/controllers and current IRQs each time; never persist a raw IRQ
  number or ALSA client number as the sole device identity.
- Revalidate at every startup and relevant hotplug event, even if the user changed
  nothing. Use available stable device identities (serial/vendor/product and
  topology as appropriate), account for identical devices without unique serials,
  and detect CPU/kernel/device-capability changes. Check desired versus effective
  settings and handle devices that appear late with bounded waits/retries.
- A new/replacement keyboard should not automatically invalidate an otherwise
  working system profile. Reuse compatible mappings only when identity/capabilities
  are unambiguous; offer focused reassignment when needed. Recheck controller/IRQ
  placement if USB topology changed. Do not silently guess manual/pedal mappings.
- Preserve saved profiles on missing/changed hardware. Offer a clearly indicated,
  validated fallback or focused setup guidance when required resources are absent;
  do not silently start the previously unsafe scheduling arrangement. Reconnection
  must not restore held notes or create stuck notes. No system probing, privileged
  operations or blocking device setup belongs in the audio callback.
- Provide configuration backup, last-known-working rollback and an uninstall/reset
  path for system changes. Test service failure, unavailable permissions, changed
  devices and unsupported affinity without leaving partial configuration behind.

### 16.5 Acceptance gates

- [ ] Repeated cold boots with unchanged hardware reach playable state without
  operator authentication, terminal commands or manual setup; verify actual
  affinity, MIDI input, physical audio outputs and startup registration.
- [ ] Reboot/renumber IRQs and device enumeration; reconnect, replace and add MIDI
  devices, including identical models and changed USB ports/hubs. Preserve organs,
  mappings and multichannel profiles; request intervention only where needed.
- [ ] Validate core selections, worker limits and recovery resources on each tested
  SBC/CPU layout; repeat saturation/release tests and record callback deadlines,
  MIDI continuity, thermals and user-audible results. No blanket SBC support claim.
- [ ] Exercise voice admission, short/repeated notes, couplers, full registration,
  tails, capacity reduction, rejection indication and Panic at the configured cap.
- [ ] Verify meter colors/labels/peak retention against measured deadline misses
  and voice rejection, including short spikes hidden by smoothed averages.
- [ ] Verify administrator authentication/cancellation, helper validation,
  persistence, apply/restart messages, failed-apply rollback and restoration of
  system defaults. Normal performance remains usable without administrator rights.

Implementation references: [Linux IRQ affinity documentation](https://www.kernel.org/doc/html/latest/core-api/irq/irq-affinity.html),
[systemd service startup](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html),
[systemd execution settings](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html),
and [polkit authentication architecture](https://www.freedesktop.org/software/polkit/docs/latest/polkit.8.html).
Exact mechanisms and available options must be checked against the installed OS;
these references do not establish implementation or validation on a target board.
