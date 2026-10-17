# Aeolus — copyright, license and modification notice

This repository is a modified version of [Arthur Benilov's Aeolus plugin](https://github.com/Archie3d/aeolus_plugin), derived in part from [Fons Adriaensen's original Aeolus](https://kokkinizita.linuxaudio.org/linuxaudio/aeolus/). It is maintained as [cmbrodman/aeolus_plugin](https://github.com/cmbrodman/aeolus_plugin). It is not an unmodified upstream release.

## Original authors and retained notices

- The inherited `Source/aeolus/addsynth.cpp`, `addsynth.h`, `rankwave.cpp` and `rankwave.h` carry:
  - Copyright (C) 2003-2013 Fons Adriaensen <fons@linuxaudio.org>
  - Copyright (C) 2021 Arthur Benilov <arthur.benilov@gmail.com>
- Other inherited application source files retain Arthur Benilov's copyright notices and dates (including 2021, 2024 and 2025 as stated in individual files).
- Aeolus 0.10.4's original AUTHORS file credits Fons Adriaensen and Hans Fugal. This historical credit does not assert that every original component is included in this plugin.
- Stop definitions retain their embedded copyright/author fields. These are separate from the C++ file headers; do not remove them when converting or editing a stop.

The inherited application source headers license that code under the **GNU General Public License, version 3 or, at your option, any later version**. See [LICENSE.txt](LICENSE.txt) for the unmodified GPLv3 text. This notice does not replace or remove any original copyright, license or warranty notice.

Aeolus is free software: you may redistribute and modify its GPL-covered code under those terms. It is supplied WITHOUT ANY WARRANTY, including the implied warranties of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE, to the extent permitted by law. The complete license controls.

## Dated modification record

- **2021:** The inherited plugin adaptation by Arthur Benilov modifies the original additive-synthesis and rank-wave modules and adds a JUCE-based application/plugin, voicing, spatialization and convolution reverb. See the original file headers and upstream history.
- **2026-08-23 through 2026-09-05:** This fork's Git history records standalone, organ editing, MIDI/UI and related changes relative to upstream. The history is the authoritative per-file change record; these changes are not attributed to the original Aeolus authors.
- **2026-10-15:** Added `AEOLUS_FEATURES.md`, recording plans and open-source module research. Those plans do not describe completed DSP/UI changes.
- **2026-10-15:** Added this notice, readable third-party/font license copies, a distribution checklist and README links. Corrected the roadmap's ASIO inventory: the checked-in directory is a download placeholder, not a vendored SDK. This notice update makes no changes to audio processing or original author headers.
- **2026-10-15:** Resolved the ten impulse-response and 85 stop-model provenance entries; added designated recording credits, source/license evidence, measured conversion notices, a per-file hash inventory and complete CC BY 4.0/GPLv2 texts. Audio/model bytes and source headers are unchanged.

- **2026-10-17:** Corrected project agent guidance and documented this fork's implemented organ-console work. Added MIDI event-offset rendering, bounded note-queue overflow recovery, timer-owned state saves, off-callback IR preparation with crossfading and device-rate resampling, Linux SSE3 dispatch and CPU-feature checks, and regression tests. Removed duplicate CMake setup, corrected a C++ declaration blocking GCC builds and padded short IRs for safe convolution. Guarded zero-sized convolution initialization against null-buffer operations. CI now runs the regression target and collects root/asset notices with application artifacts. Original code notices and bundled audio/model bytes remain intact.

- **2026-10-17:** Added separate native Linux ARM64 and x86-64 CI builds and regression runs, architecture-specific artifact names and Linux deployment guidance. Both Linux builds use an Ubuntu 22.04 baseline; target-board audio validation remains separate.

Future modified releases must retain these notices and add accurate change descriptions and dates. Do not backdate changes or replace an original author's notice with a fork contributor's name.

## Dependencies and assets have their own terms

The repository's GPL license is not a blanket license for all bundled material.

- The pinned **JUCE 8.0.7** dependency is offered under AGPLv3 or a commercial license. This fork documents the open-source route; no commercial license is asserted. Combining GPLv3 application code with AGPLv3 code is permitted by GPLv3 section 13, with the section's requirements for the combination. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- Fonts remain under SIL OFL 1.1. MTS-ESP client code and the optional CLAP extension retain their separate permissive notices.
- The ten impulse responses retain CC BY 4.0 terms and their designated OpenAIR creator credits. The 85 stop-model files retain original GPL grants and embedded contributor fields. See [the complete asset notices](docs/ASSET_NOTICES.md) and [per-file provenance](docs/asset-provenance.json), including recording modifications and source/license evidence.
- Libraries suggested only in the roadmap are not installed dependencies and receive no implied permission or attribution claim here.

## Source and redistribution

Source for this fork: https://github.com/cmbrodman/aeolus_plugin

When distributing binaries, provide the exact corresponding source, modifications and build scripts as required by the applicable licenses, including pinned submodule sources. A link to an evolving default branch alone is not a release-specific source delivery plan. Include `LICENSE.txt`, this notice, `THIRD_PARTY_NOTICES.md`, `LICENSES/`, `docs/ASSET_NOTICES.md`, `docs/asset-provenance.json`, and the applicable transitive dependency notices.

See [the distribution checklist](docs/DISTRIBUTION_CHECKLIST.md) before publishing binary packages. The recording/stop-model entries are resolved for the audited baseline; other resource and release-delivery checks remain in the third-party notices and checklist. No endorsement by the original authors or third-party contributors is implied.
