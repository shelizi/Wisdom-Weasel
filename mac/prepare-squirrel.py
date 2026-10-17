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

    patch = ROOT / 'mac/squirrel/taiwan-lexicon.patch'
    def git(*args):
        return subprocess.run(['git', '-C', str(source), 'apply', *args, str(patch)],
                              capture_output=True, text=True)
    if git('--check').returncode == 0:
        result = git()
        if result.returncode:
            raise RuntimeError(result.stderr)
    elif git('--reverse', '--check').returncode != 0:
        raise RuntimeError('Squirrel deployment hook differs from the pinned version; '
                           'integration patch was not applied. Preserve and review local edits.')

    # package/add_data_files registers these in Copy Shared Support Files. This
    # happens before Xcode codesigns the app, so no signed bundle is modified.
    destination = source / 'data/plum'
    destination.mkdir(parents=True, exist_ok=True)
    for name in FILES:
        shutil.copyfile(resource / name, destination / name)
    print('Prepared Taiwan vocabulary and Squirrel deployment hook')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'squirrel')
    args = parser.parse_args()
    prepare(args.source)
