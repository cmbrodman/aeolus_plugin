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

## Impulse responses — resolved 2026-10-15

All ten bundled WAVs are identified against the University of York's original OpenAIR recordings. Their dataset-specific publisher attribution sections archived on 2021-10-26 explicitly offer **CC BY 4.0**. Earlier captures had different licenses; the evidence and selected later grant are documented rather than treating OpenAIR as uniformly licensed.

See [the complete recording and stop-model notices](docs/ASSET_NOTICES.md) for all ten titles, designated creator/library credits, original recording URLs, archived publisher license evidence and measured modification notices. The [per-file inventory](docs/asset-provenance.json) includes bundled/source SHA-256 hashes, formats and waveform comparisons. Retain [CC BY 4.0](LICENSES/CC-BY-4.0.txt) and those credits/modification notices with the recordings, including when embedded in binaries. The recordings retain their separate CC terms; the root GPL does not replace them.

## Stop definitions — resolved 2026-10-15

All **59 binary and 26 JSON models** have source/license evidence in [ASSET_NOTICES.md](docs/ASSET_NOTICES.md) and [the inventory](docs/asset-provenance.json):

- **49 binary models:** byte-identical to `stops-0.3.0`; GPL-2.0-or-later as recorded in the matching Debian source package's copyright record. Retain [the original grant and copyright record](LICENSES/Aeolus-stops-notice.txt), [GPLv2](LICENSES/GPL-2.0.txt), individual author/comment fields, and the root GPLv3 used through the later-version option.
- **10 binary models:** exact Git blob matches to Simul Piscator's 2021-02-16 multi-rank cornet/mixture contribution, published in that contributor's GPLv3 Aeolus tree. Preserve embedded `jm` credit and underlying Aeolus notices.
- **26 JSON models:** traced to Arthur Benilov's upstream plugin commits; nine retain original binary-model parameter arrays and author credits, and 17 retain AB/ab credits. JSON conversion and other additions are documented separately from inherited model rights.

Individual model credits include Fons Adriaensen, Matthias Nagorni, Nathan Hurst and the Reiner Janke sample-source comment. Do not discard fields or replace them with a package-level credit. Model files are editable synthesis parameters; keep them with corresponding source. This audit changed no asset bytes.

## Other resource checks

The icon/image resources and headerless modelling scripts still need their provenance checked for any separately sourced material before a complete release inventory is asserted. This is separate from the now-resolved recording/stop-model inventory. No original resource or metadata was removed by this notice update.

## ASIO and other optional SDKs

Despite its name, `asiosdk_2.3.3_2019-06-14/` currently contains only `asio_sdk.txt`, an instruction to download the SDK. **The SDK implementation is not checked into this tree.** `WITH_ASIO` defaults to OFF, but the existing Windows CI configuration downloads the legacy SDK and enables ASIO.

If building that target, inspect the exact downloaded SDK's terms, retain required notices and confirm compatibility/permission for the distributed combination. This audit does not accept an SDK agreement, assert an exception or apply newer SDK terms retroactively. Check the actual AAX/AU/VST3/CLAP dependency versions and licenses if enabling other formats.

## Research-only candidates

Qt/QML, RtMidi, RtAudio, zita-rev1, Signalsmith, xsimd, Highway, Tracy, Google Benchmark and IEM recommendations in `AEOLUS_FEATURES.md` are not added to the application by these documents. Their future adoption requires a fresh version-specific inventory.
