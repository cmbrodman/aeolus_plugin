# Third-party notices

Audit date: 2026-10-15. Baseline: commit `23692623d2876bc1bf1025aefdf62942f56955c1`. This inventory records observed notices, not a blanket redistribution clearance. Original notices in individual files and dependencies remain authoritative.

## Original Aeolus and the plugin adaptation

See [NOTICE.md](NOTICE.md) and [LICENSE.txt](LICENSE.txt). Original synthesis code credits Fons Adriaensen; the plugin adaptation credits Arthur Benilov. Original source archive examined for historical authorship/license context: https://kokkinizita.linuxaudio.org/linuxaudio/downloads/aeolus-0.10.4.tar.bz2 (AUTHORS, COPYING and README). Do not assume this 2022 archive is the precise historical revision from which every file was adapted.

## JUCE 8.0.7

- Source: https://github.com/juce-framework/JUCE/tree/10a589619b452c261b2940767eb253171eb5a823
- Copyright (c) Raw Material Software Limited, as stated in the pinned module headers.
- Pinned submodule commit: `10a589619b452c261b2940767eb253171eb5a823`.
- Module license options: **AGPLv3 or commercial JUCE license**, not GPLv3-only.
- Readable copies: [JUCE's own license/dependency index](LICENSES/JUCE-8.0.7.md) and [GNU AGPLv3](LICENSES/AGPL-3.0.txt).
- Original dependency index: [JUCE/LICENSE.md](JUCE/LICENSE.md).

The open-source combination is governed by the inherited GPLv3-or-later application terms and the applicable AGPLv3 combination requirements, including network-interaction requirements when applicable. This notice does not relicense JUCE or claim a commercial license.

JUCE also contains third-party software with independent notices. Its pinned license index identifies AudioUnitSDK, Oboe, FLAC, GLEW/Mesa/Khronos, Ogg Vorbis, libjpeg, CHOC/QuickJS, LV2-related libraries, pslextensions, AAX/VST3 SDKs, Box2D, libpng, zlib, HarfBuzz and SheenBidi, along with build-tool/example dependencies. Preserve the license/copyright notices for code actually included by each target. A dependency's appearance in this list does not imply it is compiled into every Aeolus binary. Source distributions must include the required submodule contents rather than assuming GitHub's automatically generated source ZIP includes them.

## MTS-ESP client

- Files: `Source/mts/libMTSClient.h`, `Source/mts/libMTSClient.cpp`.
- Copyright (C) 2021 by ODDSound Ltd. info@oddsound.com
- Upstream: https://github.com/ODDSound/MTS-ESP
- License: the permissive grant and disclaimer reproduced verbatim in [LICENSES/MTS-ESP-client.txt](LICENSES/MTS-ESP-client.txt).

This grant covers the bundled client files. It does not grant redistribution rights to an independently installed MTS master, tuning product or runtime obtained elsewhere.

## JUCE Unofficial CLAP Plugin Support

- Submodule: `clap-juce-extensions`.
- Source: https://github.com/free-audio/clap-juce-extensions/tree/24e70f7f7cde2842528bb66ff50260b1dc0f4dae
- Copyright 2019-2020, Paul Walker.
- License: MIT, reproduced verbatim in [LICENSES/clap-juce-extensions-MIT.txt](LICENSES/clap-juce-extensions-MIT.txt).
- Original: [clap-juce-extensions/LICENSE.md](clap-juce-extensions/LICENSE.md).

The current top-level CMake selects Standalone. If CLAP support is enabled later, also retain the selected extension's nested dependency notices.

## Embedded fonts

The following notices were read directly from name-table records 0, 9, 13 and 14 of the exact TTF files tracked in this repository. The fonts already contain the OFL text in metadata; these copies make it readily available to recipients of embedded binaries.

| Font file | Copyright / reserved font name | License copy |
|---|---|---|
| `Resources/fonts/Balgruf-JRye7.ttf` | Copyright (c) 2020, Paul James Miller; Reserved Font Name **Balgruf** | [Balgruf-OFL.txt](LICENSES/Balgruf-OFL.txt) |
| `Resources/fonts/WignersFriendRoman-1GY8e.ttf` | Copyright (c) 2021, Paul James Miller; Reserved Font Name **Wigners Friend** | [Wigners-Friend-OFL.txt](LICENSES/Wigners-Friend-OFL.txt) |

Both fonts remain under **SIL Open Font License 1.1**, separate from the application's GPL. Retain their copyright and license when redistributing or embedding them. Do not sell the font files by themselves. Respect the reserved-name conditions if modifying them. Official OFL information: https://openfontlicense.org/

The full notices in `LICENSES/` are extracted from the bundled fonts, with line endings normalized to LF. No font outlines, names or metadata were modified.

## Impulse responses — attribution verification still required

The inherited README credits the **Open Acoustic Impulse Response Library (OpenAIR), Audiolab, University of York**, https://www.openair.hosted.york.ac.uk/ (historical site: http://www.openairlib.net/).

This credit is retained and made explicit here, but it is not a substitute for each recording's designated creator, dataset URL, license version and modification notice. The following bundled files have no accompanying dataset-specific license/attribution text in this repository:

- `Resources/irs/1st_baptist_nashville_balcony.wav`
- `Resources/irs/elveden_hall_suffolk_england.wav`
- `Resources/irs/lady_chapel_stalbans.wav`
- `Resources/irs/r1_nuclear_reactor_hall.wav`
- `Resources/irs/st_andrews_church.wav`
- `Resources/irs/st_georges_far.wav`
- `Resources/irs/st_laurentius_molenbeek.wav`
- `Resources/irs/york_council_chamber.wav`
- `Resources/irs/york_minster.wav`
- `Resources/irs/york_uni_sportscentre.wav`

**Unresolved:** Match each exact bundled recording to its primary dataset/archive, preserve the specified attribution and license, and record any resampling, trimming, channel conversion or other changes. Dataset names alone do not establish exact recording provenance. Do not assign a blanket CC license or copy another application's modification claims to these files.

The primary OpenAIR site could not be verified during this audit: direct HTTPS retrieval reported an expired certificate, and document retrieval timed out. No certificate checks were bypassed. This notice does not claim the recordings are unlicensed; it records the evidence still needed before clearing their redistribution. Resolve these entries, obtain appropriate permission, or use cleared alternatives before publishing a binary containing them.

## Stop definitions and other resources

The inherited README identifies `Resources/stops/*.ae0` as originating from Aeolus and notes JSON translations/additional models. JSON files retain their copyright fields, including `FA`, `mn`, `AB` and `ab`; their initials are not expanded here without evidence. Source readers preserve these fields.

**Unresolved:** Confirm the exact stop-package source/license and any separately contributed model terms, including converted models; preserve embedded author/copyright/comment fields. Root GPL application licensing alone is not evidence for every asset. The icon/image resources and headerless modelling scripts also need their provenance checked for any separately sourced material before a complete release inventory is asserted. No original resource or metadata was removed by this notice update.

## ASIO and other optional SDKs

Despite its name, `asiosdk_2.3.3_2019-06-14/` currently contains only `asio_sdk.txt`, an instruction to download the SDK. **The SDK implementation is not checked into this tree.** `WITH_ASIO` defaults to OFF, but the existing Windows CI configuration downloads the legacy SDK and enables ASIO.

If building that target, inspect the exact downloaded SDK's terms, retain required notices and confirm compatibility/permission for the distributed combination. This audit does not accept an SDK agreement, assert an exception or apply newer SDK terms retroactively. Check the actual AAX/AU/VST3/CLAP dependency versions and licenses if enabling other formats.

## Research-only candidates

Qt/QML, RtMidi, RtAudio, zita-rev1, Signalsmith, xsimd, Highway, Tracy, Google Benchmark and IEM recommendations in `AEOLUS_FEATURES.md` are not added to the application by these documents. Their future adoption requires a fresh version-specific inventory.
