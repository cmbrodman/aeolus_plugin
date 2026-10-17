![build](https://github.com/cmbrodman/aeolus_plugin/actions/workflows/build.yml/badge.svg)

# ![aeolus](Resources/icons/icon64.png) Aeolus
> **Modified fork:** This repository is maintained as `cmbrodman/aeolus_plugin` and includes changes to Arthur Benilov's plugin adaptation of Fons Adriaensen's Aeolus. See [NOTICE.md](NOTICE.md) for original credits, licensing, warranty and dated modification notices; [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for dependency/font notices and recording/model license attribution; and [the distribution checklist](docs/DISTRIBUTION_CHECKLIST.md) before publishing packages.
>
> The inherited application code is GPLv3-or-later. The pinned JUCE 8.0.7 modules use AGPLv3/commercial terms; fonts and other third-party assets retain their own terms. The root GPL text does not license every dependency or recording. See [recording and stop-model credits](docs/ASSET_NOTICES.md) and [the per-file inventory](docs/asset-provenance.json). Notice documentation updated **2026-10-15**.

Pipe organ emulator using additive synthesis, currently built as a **standalone application** with JUCE and C++17. This fork focuses on an editable organ console and a future portable USB-MIDI/Linux appliance.

Aeolus was originally developed by Fons Adriaensen and presented in 2004; see [the original project](https://kokkinizita.linuxaudio.org/linuxaudio/aeolus/). Arthur Benilov adapted its wavetable synthesis into a [JUCE-based plugin](https://github.com/Archie3d/aeolus_plugin), adding pipe-attack chiff, new pipe models, spatialization and convolution reverb. Organ configuration and some pipe models use JSON alongside the original binary `.ae0` models.

The upstream project supports plugin formats, but **this checkout's CMake configuration currently selects Standalone only**. VST3, AU and CLAP output would require build integration and validation. CI is configured for Linux, macOS and Windows; hardware audio reliability must be tested separately.

## Work in this fork

Chris's (`cmbrodman`) work recorded from August through September 2026 extends the organ console and editing workflow:

- **Division Editor:** Configure one to ten divisions and edit their names from the settings menu.
- **Stop Editor:** Select and arrange division stops, with a footage selector that creates a separate JSON model variant under `Documents/Aeolus/Stops`.
- **Coupler Editor:** Configure unison, sub-octave and super-octave links, with pass-through options and bass couplers for manuals. The fork includes subsequent octave-coupler and note-handling fixes.
- **Divisional presets:** Ten preset buttons per division, with stored-registration and current-match indicators. Click a preset in programming mode to store it; click in normal mode to recall it.
- **Persistent organ state:** Automatic saving to `organ_state.json`, including division/sequencer state, MIDI channel masks, selected reverb IR and window size.
- **Console preferences and layout:** Remembered window dimensions, an option to hide the on-screen keyboard, and revised placement of MIDI settings, coupler controls and panic controls.
- **Editable stop workflow:** Export of embedded stop models and polling for modifications to external `.ae0`/JSON stop files, followed by model/rankwave reload. First discovery by the poller records a timestamp; it does not itself reload a newly discovered file.
- **Audio startup work:** Convolver priming and low-level keep-alive noise intended to address first-note pops and audio-device wake behavior. These changes still require platform-specific audio regression checks.
- **Editor stability:** Follow-up fixes for menu closing and organ-editor updates.

October 2026 documentation work added the feature roadmap, preserved upstream/dependency notices, and documented provenance for all ten bundled impulse responses and 85 stop models. See [NOTICE.md](NOTICE.md) for the dated modification record and [asset notices](docs/ASSET_NOTICES.md) for attribution details. These entries describe work present in source/history, not a claim that every workflow has passed current runtime testing.

## Planned development

[AEOLUS_FEATURES.md](AEOLUS_FEATURES.md) records the agreed requirements and research. Planned work includes deterministic rendering and performance measurements, selectable Off/Algorithmic/Convolution reverb, runtime routing to as many as four configurable stereo pairs with safe stereo fallback, crossover/calibration tools, USB-MIDI recovery, touchscreen improvements and measured x86-64/ARM64 optimization.

These features are not implemented by the roadmap. The existing eight-mono-bus build is not the proposed stereo-pair routing system. Qt/QML and alternative audio/DSP libraries remain candidates for evaluation; no complete frontend or backend replacement is implied.

## Implementation notes

This project takes only `addsynth` and `rankwave` modules from the original implementation (the source's been modified though). These modules describe the 64-harmonics additive synth and organ pipes wavetables generation. Plus this repo includes the original `ae0` files that contain the harmonics tables for various pipes in binary format (these are embedded into the plugin's resources). All the new pipe models are encoded in equivalent JSON format.

The plugin adaptation introduced its own voicing, spatialization and reverb rather than carrying over the original Aeolus application architecture, so its sound differs from the original. This fork builds on that adaptation.

The convolution reverb uses impulse responses from the [Open AIR](https://www.openair.hosted.york.ac.uk/) project database.

## MIDI Control
Sequencer steps are controlled via the program change messages sent on the control MIDI channel (program `0 `corresponds to the first step of the sequencer, `1` - to the second and so on).
> The processor also exposes a `Program` parameter for host integration if plugin targets are enabled in a future build.
> Another way to control the sequencer is via key switches (MIDI notes). The default configuration uses keys `22` and `23` to control the sequencer. These can be changed via the organ configuration file (multiple keys can be assigned for forward and backward actions).

### CCs:
- `CC 1` modulation wheel enables/disables tremulant
- `CC 7` controls the global volume (on the control MIDI channel) and the volume of each division which has `swell` config flag enabled (on the swell MIDI channel)
- `CC 91` controls the reverb output level
- `CC 98` Stops control
- `CC 123` All notes off (on control MIDI channel or inidividual divisions).

> :point_right: The stops control follows the original Aeolus convention. The control mode is set by the message `01mm0ggg`, where `mm` is the control mode, and `ggg` is the control group (division number, counter from the top starting from `0`).
> Control modes are:
> - `00` Disable the division (all stops are off)
> - `01` Set stop off
> - `10` Set stop on
> - `11` Toggle
>
> Once the control message is received the value of the format `000bbbbb` will execute the selected control mode action on specified stop buton `bbbbb`, counted from `0`.

## Custom organ configuration
Organ configuration and state normally use `Documents/Aeolus/organ_config.json` and `organ_state.json`. If either file exists beside the executable when the data directory is first selected, the application uses that directory for both files instead. Missing files are initialized from the embedded configuration and an empty state.
> :point_right: The `Documents` folder's exact location depends on the operating system and the user profile.

To create the `organ_config.json` start with [default one embedded into the plugin](Resources/configs/default_organ.json) by copying it to `Documents/Aeolus` folder and renaming to `organ_config.json`.

It is also possible to use custom pipe configs in `.ae0` or `.json` format. These have to be placed to the `Documents/Aeolus` folder so they can be referenced from the `organ_config.json` (use pipe file name without the extension in the `"pipe"` attribute of the `organ_config.json`).

## MTS support
When enabled, use a MIDI Tuning Standard (MTS) master (like [MTS-ESP Mini](https://oddsound.com/mtsespmini.php)) to change the tuning.
> :point_right: Tuning change will affect newly played notes.
> All plugin instances are affected by the tuning change.

## Multibus output
When compiled with the `WITH_MULTIBUS_OUTPUT` CMake option enabled, the generated application will output to `8` separate mono buses. Each bus corresponds to the pipes groups placement in space according to the internal horizontal arrangement of the pipes.

In multibus configuration there is no reverb applied, and there is no spatialization performed.

> :point_right: The multibus mode is intended for the object based mixing, where you could place individual pipe groups in space yourself and apply a reverb of your preference.

Pipes are arranged starting from the lowest key from the sides (buses 0 and 7) to the center in the middle of the range (buses 3, 4), and then going back from the centre towards the sides. For the pedal pipes, they go from the outside towards the centre only.

Corresponding pipe position jumps between left and right following the keys (C will be on the left, C# on the right, D of the left, D# on the right and so on).

> :point_right: This very same pipes spatial arrangement is used in the stereo version of the plugin to perform spatialized rendering followed by a stereo convolutional reverb.

## Building

Initialize the pinned submodules before configuring. The build requires CMake 3.20 or later, a C++17 compiler and the platform dependencies required by JUCE. On Debian/Ubuntu, the current CI installs `ninja-build`, `libx11-dev`, `libfreetype-dev`, `libfontconfig-dev`, `libasound2-dev`, `libxrandr-dev`, `libxinerama-dev` and `libxcursor-dev`; see [the workflow](.github/workflows/build.yml) for the current platform setup.

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
```

The standalone executable is generated under `build/Aeolus_artefacts/Release/Standalone` (platform-specific executable or application bundle). `WITH_ASIO` and `WITH_MULTIBUS_OUTPUT` default to `OFF`. Enabling multibus changes the target/product name to `Aeolus-multibus` and its corresponding artifact directory.

The checked-in ASIO directory contains download instructions only. An ASIO-enabled Windows build requires the SDK separately; Windows CI currently downloads the legacy SDK. Read [the third-party notices](THIRD_PARTY_NOTICES.md) before distributing such a build.

The [JUCE Unofficial CLAP Plugin Support](https://github.com/free-audio/clap-juce-extensions) submodule is retained, but current CMake does not integrate or produce a CLAP target.

## Development and validation

Read [AGENTS.md](AGENTS.md) for source layout, real-time audio requirements, configuration handling and verification guidance; [CLAUDE.md](CLAUDE.md) adds Claude-specific guidance. Preserve custom organs and use isolated user data for tests.

Current CMake has no dedicated engine regression test target. The scripts in `Model/` are offline modelling experiments. Changes to MIDI timing, reverb, routing or performance need deterministic audio comparisons and actual device checks; successful compilation alone does not establish sound quality, latency or xrun behavior. Follow [the distribution checklist](docs/DISTRIBUTION_CHECKLIST.md) when packaging releases, since CI artifacts do not automatically include all notices and corresponding source materials.
