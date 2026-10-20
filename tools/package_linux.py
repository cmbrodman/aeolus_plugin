#!/usr/bin/env python3
"""Package a CI Linux artifact with its matching source, including submodules."""
import argparse
import hashlib
import io
import json
import re
from urllib.parse import unquote
from pathlib import Path
import shutil
import subprocess
import tarfile


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args])


def source_archive(repo, revision, archive, prefix='source/'):
    # Archive committed content only, never local organ files or build products.
    data = git(repo, 'archive', '--format=tar', revision)
    with tarfile.open(fileobj=io.BytesIO(data)) as tree:
        for member in tree:
            member.name = prefix + member.name
            archive.addfile(member, tree.extractfile(member) if member.isfile() else None)
    for record in git(repo, 'ls-tree', '-r', '-z', revision).split(b'\0'):
        if not record:
            continue
        meta, name = record.split(b'\t', 1)
        mode, kind, sha = meta.split()
        if mode == b'160000':
            relative = name.decode()
            source_archive(repo / relative, sha.decode(), archive, prefix + relative + '/')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--commit', required=True, help='Exact commit used by the CI artifact')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    revision = git(repo, 'rev-parse', args.commit + '^{commit}').decode().strip()
    binary = args.artifact / 'Standalone' / 'Aeolus'
    executable = binary.read_bytes()
    if b'AEOLUS_TEST_SETTINGS_DIRECTORY' in executable or b'RESET_TEST' in executable:
        parser.error('Refusing a test-enabled standalone; rebuild with AEOLUS_BUILD_TESTS=OFF')
    header = executable[:20]
    if header[:6] != b'\x7fELF\x02\x01':
        parser.error('Expected a 64-bit little-endian Linux ELF executable')
    machine = int.from_bytes(header[18:20], 'little')
    architecture = {183: 'arm64', 62: 'x86_64'}.get(machine)
    if architecture is None:
        parser.error(f'Unsupported ELF architecture: {machine}')
    info = args.artifact / 'BuildInfo.json'
    build_info = json.loads(info.read_text()) if info.exists() else {}
    if build_info.get('commit') != revision:
        parser.error('BuildInfo.json must identify the exact artifact commit')
    args.destination.mkdir(parents=True, exist_ok=False)
    destination = args.destination
    shutil.copytree(args.artifact, destination, dirs_exist_ok=True)
    # Preserve the pinned framework's index and its referenced dependency notices
    # at their relative paths. Full dependency sources are also archived below.
    juce_notices = destination / 'Notices' / 'JUCE'
    juce_notices.mkdir(parents=True, exist_ok=True)
    juce_revision = git(repo, 'ls-tree', revision, 'JUCE').split()[2].decode()
    index = git(repo / 'JUCE', 'show', juce_revision + ':LICENSE.md')
    (juce_notices / 'LICENSE.md').write_bytes(index)
    for link in re.findall(r'\]\(([^)]+)\)', index.decode()):
        if ':' in link or link.endswith('/'):
            continue
        relative = Path(unquote(link))
        target = juce_notices / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(git(repo / 'JUCE', 'show', juce_revision + ':' + relative.as_posix()))
    (destination / 'Standalone' / 'Aeolus').chmod(0o755)
    (destination / 'Start Aeolus.sh').write_text('#!/bin/sh\ncd "$(dirname "$0")" || exit 1\nexec ./Standalone/Aeolus "$@"\n')
    (destination / 'Start Aeolus.sh').chmod(0o755)
    (destination / 'Start Aeolus multicore.sh').write_text(
        '#!/bin/sh\ncd "$(dirname "$0")" || exit 1\n'
        ': "${AEOLUS_DSP_WORKERS:=3}"\n: "${AEOLUS_MAX_VOICES:=1024}"\n'
        'export AEOLUS_DSP_WORKERS AEOLUS_MAX_VOICES\n'
        'exec ./Standalone/Aeolus "$@"\n')
    (destination / 'Start Aeolus multicore.sh').chmod(0o755)
    (destination / 'READ-ME.txt').write_text(
        f'Aeolus Linux {architecture}\nSource commit: {revision}\n'
        f'Build environment: {build_info.get("build_os", build_info.get("runner", "see BuildInfo.json"))}\n\n'
        'Copy this entire folder to the target computer. Run Start Aeolus.sh\n'
        'from a graphical desktop, or execute ./Standalone/Aeolus in a terminal.\n'
        'Start Aeolus multicore.sh opts into 3 workers and a 1024-entry voice pool.\n'
        'It requests no core affinity, FIFO scheduling, IRQ or system changes.\n'
        'This pool size is not a claim of clean 1024-voice playback.\n'
        'Use a writable Linux directory if the USB drive is mounted noexec.\n'
        'On FAT/exFAT, run chmod +x after copying to the Linux filesystem.\n'
        'Keep Notices, Standalone/helpers and corresponding-source.tar.gz with the application.\n'
        'Linux independent safety requires Python 3.9+ with pidfds and writable /dev/shm.\n'
        'PyGObject/GTK 3 is optional for the separate Panic/Resume window.\n'
        'The console footer reports whether the independent watchdog is active.\n'
        'Your existing organ configuration is not included or overwritten.\n'
        'Report startup errors from the terminal; use ldd Standalone/Aeolus\n'
        'to check for missing runtime libraries. See source/README.md in the\n'
        'source archive for build options and platform limitations.\n')
    with tarfile.open(destination / 'corresponding-source.tar.gz', 'w:gz') as archive:
        source_archive(repo, revision, archive)
    sums = []
    for path in sorted(destination.rglob('*')):
        if path.is_file():
            sums.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(destination)}\n')
    (destination / 'SHA256SUMS').write_text(''.join(sums))
    print(destination)


if __name__ == '__main__':
    main()
