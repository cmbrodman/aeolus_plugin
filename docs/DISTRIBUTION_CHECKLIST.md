# Distribution checklist

Updated 2026-10-15. Use this alongside the complete applicable licenses; completing notices alone does not fulfill all source-delivery or asset-permission requirements.

## Source releases

- Preserve the existing Fons Adriaensen, Arthur Benilov and third-party file notices, license grants and warranty disclaimers.
- Retain [LICENSE.txt](../LICENSE.txt), [NOTICE.md](../NOTICE.md), [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) and all files in [LICENSES/](../LICENSES/).
- Mark modified versions prominently and record accurate descriptions/dates. Keep existing author names and separate fork modifications from upstream work.
- Include the exact required JUCE and CLAP submodule contents and their nested dependency notices. A repository archive without submodules is not automatically complete corresponding source.
- Retain [the resolved recording/stop-model notices](ASSET_NOTICES.md) and [per-file inventory](asset-provenance.json). Verify hashes for the release; re-audit changed assets. Complete the separate icon/image/modelling-script provenance checks in THIRD_PARTY_NOTICES before representing the whole package as cleared.

## Binary releases, including appliance images

- Ship readable copies of the above license/notice files next to the application or in an accessible documentation/licenses location. Embedded font metadata alone is not the intended way for application recipients to find the notices.
- Include all applicable third-party notices for code actually linked or embedded, including JUCE's nested dependencies and downloaded optional SDKs. Read the exact license texts.
- Deliver complete, matching corresponding source using a method permitted by the applicable GPL/AGPL terms: application, modifications, dependency sources, build/configuration scripts and required installation information. Provide the specific release/commit and pinned dependency revisions with the binary.
- When offering object code from a download location under GPLv3 section 6(d), offer equivalent access to corresponding source as specified there. A documentation checklist or generic GitHub homepage is not itself corresponding source.
- If the distributed system qualifies as a User Product and installation-information requirements apply, provide the required information for installing/running modified covered software.
- With the open-source JUCE route, comply with the AGPLv3 terms applicable to the combination, including the remote-network source offer if that provision applies. Do not call the entire combination GPL-only.
- Ensure recipients can find the copyright, license and no-warranty notices. Preserve existing interactive legal notices when present; assess GPLv3 section 5(d)'s original-interface exception rather than assuming a new About screen is always mandatory.
- Package all ten recordings' designated credits, CC BY 4.0 license/link and conversion notices from ASSET_NOTICES.md. Preserve stop-model GPL grants, embedded author/comment fields and editable model sources. Fonts stay under OFL with reserved-name conditions; recordings stay under CC BY 4.0. Do not relabel them under the application GPL.
- For ASIO-enabled Windows builds, verify the downloaded legacy SDK's exact license and required notices. It is not cleared merely because the Linux target defaults to ASIO OFF.
- Check that the final archive/image actually contains these materials; the existing CI artifact uploader does not automatically collect root-level documentation/license files.

No binaries are built or released by the notice-only update of 2026-10-15. Audio behavior, original source headers, font bytes and impulse-response bytes are unchanged.
