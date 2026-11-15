# Third-Party Source and License Intake Policy

**Applies to:** all LionOS branches, kernel code, userland, build tools, boot images, documentation, and release artifacts.  
**Policy status:** active intake gate; this is an engineering process, not legal advice.

## 1. Non-negotiable rule

No external source file, generated code, binary library, firmware blob, icon, font, wallpaper, or other third-party asset may be added to LionOS until its provenance and license are recorded and reviewed. A successful build does not approve a license.

The repository's top-level MIT license applies to LionOS-authored material only. It must not be represented as relicensing third-party components.

## 2. Required intake record

Create one row in `docs/THIRD_PARTY_COMPONENTS.csv` for each separately licensed component (and separate rows for mixed-license subcomponents where necessary). Before approval, record:

- stable component ID and exact component name;
- canonical upstream project and source-package URLs;
- exact upstream tag/commit and package version, not merely a moving branch or distro codename;
- SPDX license expression where accurately established, plus all exceptions and per-file deviations;
- copyright holders and preserved original notices;
- exact imported source paths or binary/artifact locations;
- all local patches, generated files, and build transformations;
- whether it is included in ISO, USB image, or other distributed artifacts;
- source-code availability, archive/retrieval method, and the applicable redistribution obligations;
- reviewer, review date, and approval status (record reviewer/date in notes until dedicated columns are added).

Unknown facts must be written as `UNKNOWN` or `UNRECORDED`; never infer or silently fill them. An unknown value blocks approval.

## 3. Status meanings

- `PENDING_AUDIT`: provenance, exact version, license, notices, or redistribution obligations are incomplete. Not approved for import/release.
- `BUILD_ONLY_NOT_SHIPPED`: used only by developers/CI and not included in LionOS runtime artifacts. Reassess if binaries or libraries become bundled.
- `APPROVED`: all required source, license, notice, compatibility, and redistribution checks are complete and documented.
- `REJECTED`: incompatible, untraceable, or unacceptable for the project.

The manifest checker validates record structure and prevents incomplete rows from being marked `APPROVED`. It does not make a legal determination and does not replace human review.

## 4. License compatibility and source separation

- Preserve every original copyright and license header.
- Never replace a third-party license with the LionOS MIT license.
- Keep imported code in a clearly separated directory, for example `third_party/<component>/`, with its original license files.
- Do not copy GPL/LGPL/MPL or other copyleft code into the MIT-licensed kernel without an explicit compatibility and distribution review.
- Treat mixed-license repositories file-by-file. A project-level badge or Ubuntu repository component is not enough.
- Record linking mode, static/dynamic inclusion, generated-code provenance, and any applicable exception (for example, a runtime-library exception) when relevant.
- Do not remove attribution, notices, or source-offer information from release media.

## 5. Binary and release artifact gate

The ISO and USB image are distributions, not merely local build outputs. Before publishing them:

1. inventory every embedded bootloader module, library, firmware file, font, icon, and other non-LionOS artifact;
2. capture exact package versions and source revisions from the build environment;
3. generate a release-specific third-party notices file;
4. provide corresponding source or another valid source-access mechanism where required;
5. archive the manifest and notices alongside the exact release;
6. block publication if any shipped component remains `PENDING_AUDIT`.

GRUB is currently invoked by `grub-mkrescue` and its components are included in generated boot media. The current workflow does not pin its package version or record the embedded module inventory. Therefore GRUB distribution compliance is an open release blocker, not an approved entry.

The repository's PNG branding, icons, and wallpapers are compiled into the kernel. Their creator, source, and redistribution terms must be recorded before public binary releases are treated as fully audited.

## 6. Kernel and driver source intake

Before adapting code from Linux, Ubuntu, or another driver project:

- identify the exact file and upstream revision;
- inspect that file's own SPDX tag and copyright header;
- inspect relevant directory-level and project-level license rules;
- determine whether the code is independent or relies on Linux-only APIs;
- record copied, modified, and independently reimplemented portions separately;
- preserve provenance in source comments and the component register;
- perform architecture, ABI, memory-safety, concurrency, and hardware-contract review;
- do not treat a register sequence as safe without generation-specific hardware documentation and validation.

Linux kernel code is not automatically compatible with the MIT-licensed LionOS kernel. Technical inspiration and copied source are different categories and must be recorded honestly.

## 7. Contribution requirements

Contributors must disclose the origin and license of submitted code/assets and confirm they have the right to submit them under the proposed terms. Do not submit code copied from websites, AI output, forums, or repositories without checking the underlying source and license. If provenance is uncertain, leave the material out and request review.

## 8. Enforcement

Run:

```sh
make third-party-check
```

The target checks CSV structure, required metadata, unique IDs, known status values, and evidence completeness for `APPROVED` rows. It deliberately permits pending audit rows while reporting them; this is not a release approval.

Every source-import pull request must include:
- manifest changes;
- original license and notice files;
- source/revision provenance;
- a summary of compatibility and local modifications;
- test evidence;
- reviewer approval.

## 9. Current known intake items

See [Third-party component register](THIRD_PARTY_COMPONENTS.csv). At the time this policy was added:
- GRUB is a runtime component in generated ISO/USB media and remains pending exact-version/module/source-offer review.
- Build and CI tools are tracked separately as non-shipped dependencies.
- Existing image assets and a repository-wide file-level provenance scan remain pending.
- This stage imports no third-party source code.
