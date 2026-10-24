#!/usr/bin/env python3
"""Prepare the pinned Squirrel dependency before its Xcode resource/signing phases.

Keep the integration patch in the parent repository so a clean submodule checkout
gets the same changes without depending on an unpublished submodule commit.
Only Python's standard library is used, at build time.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
FILES = ('wisdom_taiwan.dict.yaml', 'wisdom_taiwan.NOTICE.txt',
         'wisdom_taiwan.manifest.json', 'wisdom_taiwan.sources.zip')
INLINE_CANDIDATE_OFF = '\n  inline_candidate: false\n'
INLINE_CANDIDATE_ON = '\n  inline_candidate: true\n'
# Integration patches for the pinned Squirrel, applied in order.
PATCHES = ('taiwan-lexicon.patch', 'preedit-selection.patch')


def prepare(source, resource=ROOT / 'resource/lexicons/taiwan'):
    source, resource = Path(source), Path(resource)
    if not (source / 'sources/SquirrelApplicationDelegate.swift').is_file():
        raise RuntimeError('Initialize the squirrel submodule before building')
    for name in FILES:
        if not (resource / name).is_file():
            raise RuntimeError('Missing bundled vocabulary resource: ' + name)
    manifest = json.loads((resource / FILES[2]).read_text(encoding='utf-8'))
    digest = hashlib.sha256((resource / FILES[0]).read_bytes()).hexdigest()
    if digest != manifest['sha256'].lower():
        raise RuntimeError('Bundled vocabulary checksum mismatch')
    if hashlib.sha256((resource / FILES[3]).read_bytes()).hexdigest() != manifest['sources_sha256']:
        raise RuntimeError('Bundled vocabulary source checksum mismatch')
    # Show the converted characters while typing (like 新注音), not the zhuyin keys.
    # Users can still turn it off with style/inline_candidate in squirrel.custom.yaml.
    style = source / 'data/squirrel.yaml'
    style_text = style.read_text(encoding='utf-8') if style.is_file() else ''
    if style_text.count(INLINE_CANDIDATE_OFF) + style_text.count(INLINE_CANDIDATE_ON) != 1:
        raise RuntimeError("data/squirrel.yaml has no single style/inline_candidate line; "
                           'default was not changed. Review the pinned Squirrel version.')

    def git(patch, *args):
        return subprocess.run(['git', '-C', str(source), 'apply', *args, str(patch)],
                              capture_output=True, text=True)
    # Check every patch before applying any, so a mismatch leaves the source untouched.
    pending = []
    for name in PATCHES:
        patch = ROOT / 'mac/squirrel' / name
        if git(patch, '--check').returncode == 0:
            pending.append(patch)
        elif git(patch, '--reverse', '--check').returncode != 0:
            raise RuntimeError('Squirrel source differs from the pinned version (' + name + '); '
                               'integration patches were not applied. Preserve and review local edits.')
    for patch in pending:
        result = git(patch)
        if result.returncode:
            raise RuntimeError(result.stderr)
    if INLINE_CANDIDATE_OFF in style_text:
        style.write_text(style_text.replace(INLINE_CANDIDATE_OFF, INLINE_CANDIDATE_ON),
                         encoding='utf-8')

    # package/add_data_files registers these in Copy Shared Support Files. This
    # happens before Xcode codesigns the app, so no signed bundle is modified.
    destination = source / 'data/plum'
    destination.mkdir(parents=True, exist_ok=True)
    for name in FILES:
        shutil.copyfile(resource / name, destination / name)
    print('Prepared Taiwan vocabulary, Squirrel integration patches and inline candidate default')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'squirrel')
    args = parser.parse_args()
    prepare(args.source)
