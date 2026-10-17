"""Build/install an independent Taiwan vocabulary pack for the bundled Rime schemas.

Build: python tools/taiwan_lexicon.py build --source-dir <McBopomofo Source/Data> --chewing-source-dir <libchewing-data>
Install: python tools/taiwan_lexicon.py install --user-dir <Rime user folder>
Remove: python tools/taiwan_lexicon.py remove --user-dir <Rime user folder>
Installation preserves personal dictionaries and custom-phrase files. Redeploy Rime
after installing/removing. Python 3.9+ and PyYAML are required for config validation.
"""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import tempfile
import time
import zipfile

import yaml

ROOT = Path(__file__).resolve().parents[1]
PACK = "wisdom_taiwan"
RESOURCE = ROOT / "resource" / "lexicons" / "taiwan"
REVISION = "be6564acad6c4d3265c34a2e1a872d80f9db6068"
CHEWING_REVISION = "ed9758d24302592e560ab680b3e10c4bd05f9c03"
CHEWING_HASH = "c889a1ac3ae1901b3f8f62748bc41b958f010bf995f7f88dbaf9e3494f341428"
FILES = (PACK + '.dict.yaml', PACK + '.manifest.json', PACK + '.NOTICE.txt',
         PACK + '.sources.zip')
SOURCE_HASHES = {
    "BPMFBase.txt": "46e64a53dc9c6baa2cbe6e0b1af572f8ef6aafb50597178b5fd53df804316ce7",
    "BPMFMappings.txt": "394f3946754ebdd9d5e503f0f94c6977e5e19bd7cab21b4b18a6ea0167e00171",
    "phrase.occ": "dcd11597090b2bef88fb385bdd567e235ccd916728c84b8d0d3f8069ccfc7b8d",
}
SCHEMAS = ("bopomofo", "bopomofo_express", "bopomofo_tw")
BEGIN = "  # >>> wisdom-taiwan-lexicon"
END = "  # <<< wisdom-taiwan-lexicon"
HAN = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\U00020000-\U000323af]{2,7}\Z")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def normalize_zhuyin(value):
    # Chewing also accepts a leading neutral tone; McBopomofo writes it last.
    tone = ''.join(c for c in value if c in 'ˊˇˋ˙')
    base = ''.join(c for c in value if c not in 'ˉˊˇˋ˙')
    return base + tone


def merge_chewing(entries, readings, syllables, path):
    rows, skipped = {}, 0
    for fields in csv.reader(path.read_text(encoding='utf-8-sig').splitlines()):
        if len(fields) != 3 or fields[0].startswith('#') or not HAN.fullmatch(fields[0]):
            continue
        word, count, pronunciation = fields
        zh = pronunciation.split()
        if len(zh) != len(word) or any(normalize_zhuyin(s) not in readings for s in zh):
            skipped += 1
            continue
        frequency = int(count)
        if frequency < 0:
            raise ValueError('Negative Chewing frequency: ' + word)
        code = ' '.join(readings[normalize_zhuyin(s)] for s in zh)
        if not set(code.split()) <= syllables:
            raise ValueError('Unsupported Chewing code: ' + word)
        key = word, code
        rows[key] = max(rows.get(key, 0), frequency)
    maximum = max(rows.values(), default=0)
    added = updated = overlaps = 0
    for key, frequency in rows.items():
        # Compress this source's own scale. Conservative 100..1000 weights keep
        # its corpus counts from overwhelming existing or personal preferences.
        weight = 100 + round(900 * math.log1p(frequency) / math.log1p(maximum)) if maximum else 100
        if key in entries:
            overlaps += 1
            updated += weight > entries[key]
        else:
            added += 1
        entries[key] = max(entries.get(key, 0), weight)  # Never sum duplicates.
    return dict(entries=len(rows), added=added, overlaps=overlaps,
                weights_updated=updated, skipped_readings=skipped, max_frequency=maximum)


def build(source, terra, chewing_source, resource=RESOURCE):
    """Keep explicit readings from upstream; never infer proper-name pronunciation."""
    for name, expected in SOURCE_HASHES.items():
        if digest(source / name) != expected:
            raise ValueError("Source does not match the pinned upstream revision: " + name)
    chewing_file = chewing_source / 'dict/chewing/tsi.csv'
    if digest(chewing_file) != CHEWING_HASH:
        raise ValueError('Chewing source does not match the pinned upstream revision')
    syllables = set()
    in_table = False
    for line in terra.read_text(encoding="utf-8-sig").splitlines():
        if line.strip() == "...":
            in_table = True
        elif in_table and "\t" in line and not line.startswith("#"):
            syllables.update(line.split("\t")[1].split())
    if terra.name == 'syllables.txt':
        syllables = set(terra.read_text(encoding='utf-8').split())
    if len(syllables) < 100:
        raise ValueError("Cannot read the terra_pinyin syllable table")
    readings = {}
    for line in (source / "BPMFBase.txt").read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        zh, py = fields[1:3]
        if py[-1:] not in "12345":
            py += "1"
        # Prefer the tone written in the Zhuyin, including unmarked first tone.
        tone = next((str(i) for i, mark in enumerate("ˉˊˇˋ˙", 1) if mark in zh), "1")
        if py[-1:] == tone and py in syllables:
            readings[normalize_zhuyin(zh)] = py
    frequencies = {}
    for line in (source / "phrase.occ").read_text(encoding="utf-8").splitlines():
        fields = line.rsplit(" ", 1)
        if len(fields) == 2:
            frequencies[fields[0]] = int(fields[1])
    entries = {}
    skipped = 0
    for line in (source / "BPMFMappings.txt").read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if not fields or not HAN.fullmatch(fields[0]):
            continue
        word, zh = fields[0], fields[1:]
        if len(zh) != len(word) or any(normalize_zhuyin(s) not in readings for s in zh):
            skipped += 1
            continue
        code = " ".join(readings[normalize_zhuyin(s)] for s in zh)
        # Public vocabulary gets ordinary, bounded weights, not personal boost scores.
        weight = max(100, min(10000, frequencies.get(word, 0) * 10))
        entries[word, code] = weight
    chewing_stats = merge_chewing(entries, readings, syllables, chewing_file)
    extra = resource / "supplement.tsv"
    supplement_count = 0
    for line in extra.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        word, code = line.split("\t")
        if not HAN.fullmatch(word) or len(code.split()) != len(word):
            raise ValueError("Invalid supplemental phrase: " + word)
        if not set(code.split()) <= syllables:
            raise ValueError("Unsupported supplemental reading: " + word)
        entries[word, code] = max(1000, entries.get((word, code), 0))
        supplement_count += 1
    header = ("# Taiwan vocabulary: McBopomofo + libchewing-data + Wisdom-Weasel supplement.\n"
              "# See wisdom_taiwan.NOTICE.txt and wisdom_taiwan.manifest.json.\n"
              "---\nname: wisdom_taiwan\nversion: '2026.10.17.2'\nsort: by_weight\n"
              "use_preset_vocabulary: false\ncolumns: [text, code, weight]\n...\n")
    text = header + "".join(f"{word}\t{code}\t{weight}\n"
                            for (word, code), weight in sorted(entries.items()))
    output = resource / (PACK + ".dict.yaml")
    atomic_write(output, text.encode("utf-8"))
    # Ship the complete conversion inputs and code with the LGPL-derived data.
    # Fixed ZIP timestamps make source bundles reproducible across builds.
    archive = resource / (PACK + '.sources.zip')
    inputs = {f'McBopomofo/{name}': (source / name).read_bytes()
              for name in (*SOURCE_HASHES, 'LICENSE.txt')}
    inputs.update({
        'libchewing-data/dict/chewing/tsi.csv': chewing_file.read_bytes(),
        'tools/taiwan_lexicon.py': Path(__file__).read_bytes().replace(b'\r\n', b'\n'),
        'resource/lexicons/taiwan/supplement.tsv': extra.read_bytes().replace(b'\r\n', b'\n'),
        'resource/lexicons/taiwan/wisdom_taiwan.NOTICE.txt': (resource / (PACK + '.NOTICE.txt')).read_bytes().replace(b'\r\n', b'\n'),
        'syllables.txt': ('\n'.join(sorted(syllables)) + '\n').encode(),
        'REBUILD.txt': b'Python 3.9+ and PyYAML required. From the extracted root run:\npython tools/taiwan_lexicon.py build --source-dir McBopomofo --chewing-source-dir libchewing-data --terra syllables.txt\n',
    })
    for license_name in ('LGPL-2.1-or-later', 'GPL-2.0-or-later'):
        inputs['libchewing-data/LICENSES/' + license_name + '.txt'] = (
            chewing_source / 'LICENSES' / (license_name + '.txt')).read_bytes().replace(b'\r\n', b'\n')
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for name, data in sorted(inputs.items()):
            bundle.writestr(zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0)),
                            data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    manifest = dict(source="https://github.com/openvanilla/McBopomofo",
                    revision=REVISION, entries=len(entries),
                    words=len({w for w, _ in entries}), supplement_entries=supplement_count,
                    skipped_readings=skipped, sha256=digest(output),
                    chewing=dict(source='https://codeberg.org/chewing/libchewing-data',
                                 revision=CHEWING_REVISION, sha256=CHEWING_HASH,
                                 license='LGPL-2.1-or-later', **chewing_stats),
                    frequency_policy='McBopomofo: count*10 clipped 100..10000; Chewing: log1p normalized 100..1000; duplicate word+reading: max, never sum; supplement: minimum 1000',
                    sources_sha256=digest(archive),
                    source_sha256={name: digest(source / name) for name in
                                   ("BPMFBase.txt", "BPMFMappings.txt", "phrase.occ")})
    atomic_write(resource / (PACK + ".manifest.json"),
                 (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    return manifest


def patch_custom(content, enable):
    """Change only our marked block, retaining comments and line endings elsewhere."""
    bom = b"\xef\xbb\xbf" if content.startswith(b"\xef\xbb\xbf") else b""
    text = content.decode("utf-8-sig")
    newline = "\r\n" if "\r\n" in text else "\n"
    lines, inside = [], False
    for line in text.splitlines(keepends=True):
        if line.rstrip("\r\n") == BEGIN:
            if inside:
                raise ValueError("Repeated Taiwan vocabulary marker")
            inside = True
        elif line.rstrip("\r\n") == END:
            if not inside:
                raise ValueError("Unmatched Taiwan vocabulary marker")
            inside = False
        elif not inside:
            lines.append(line)
    if inside:
        raise ValueError("Unclosed Taiwan vocabulary marker")
    cleaned = "".join(lines)
    data = yaml.safe_load(cleaned) or {}
    if not isinstance(data, dict):
        raise ValueError("The schema custom file is not a YAML map")
    patch = data.get("patch")
    if patch is not None and not isinstance(patch, dict):
        raise ValueError("The custom patch must be a YAML map")
    if enable:
        if patch and "translator/packs/+" in patch:
            raise ValueError("A custom translator/packs/+ already exists; merge it manually")
        block = newline.join((BEGIN, "  translator/packs/+:", "    - " + PACK, END)) + newline
        patch_line = next((i for i, line in enumerate(lines)
                           if re.fullmatch(r"patch:[ \t]*(?:#[^\r\n]*)?[\r\n]*", line)), None)
        if patch_line is None:
            if "patch" in data:
                raise ValueError("Inline patch maps cannot be edited automatically")
            cleaned += (newline if cleaned and not cleaned.endswith(("\r", "\n")) else "")
            cleaned += "patch:" + newline + block
        else:
            if not lines[patch_line].endswith(("\r", "\n")):
                lines[patch_line] += newline
            lines.insert(patch_line + 1, block)
            cleaned = "".join(lines)
    return bom + cleaned.encode("utf-8")


def install(user, enable=True, resource=RESOURCE):
    user = user.resolve()
    user.mkdir(parents=True, exist_ok=True)
    # Persist explicit opt-out so the native deployer honors it on future upgrades.
    changes = {user / "wisdom_taiwan.user.yaml":
               ("enabled: " + ("true" if enable else "false") + "\n").encode("utf-8")}
    for schema in SCHEMAS:
        path = user / (schema + ".custom.yaml")
        if not enable and not path.exists():
            continue
        original = path.read_bytes() if path.exists() else b""
        data = yaml.safe_load(original.decode("utf-8-sig")) or {}
        patch = data.get("patch") if isinstance(data, dict) else None
        # Express/TW inherit bopomofo.schema, including its custom patches. Appending
        # in all three would load the same pack twice. Only override that inheritance
        # when the child explicitly replaces the packs list itself.
        replaces_packs = isinstance(patch, dict) and (
            "translator/packs" in patch or
            isinstance(patch.get("translator"), dict) and "packs" in patch["translator"])
        own_pack = enable and (schema == "bopomofo" or replaces_packs)
        if not own_pack and not path.exists():
            continue
        changes[path] = patch_custom(original, own_pack)
    if enable:
        manifest = json.loads((resource / (PACK + ".manifest.json")).read_text(encoding="utf-8"))
        dictionary = resource / (PACK + ".dict.yaml")
        if digest(dictionary) != manifest["sha256"]:
            raise ValueError("Pack checksum mismatch")
        if digest(resource / FILES[3]) != manifest['sources_sha256']:
            raise ValueError('Source bundle checksum mismatch')
        for name in FILES:
            changes[user / name] = (resource / name).read_bytes()
    # Validate everything before changing the user folder; keep exact pre-install copies.
    changed = {p: value for p, value in changes.items()
               if not p.exists() or p.read_bytes() != value}
    if not changed:
        return dict(changed=0, backup=None)
    backup = user / ("backup-taiwan-" + time.strftime("%Y%m%d-%H%M%S") + "-" + str(time.time_ns()))
    backup.mkdir()
    originals = {p: p.read_bytes() if p.exists() else None for p in changed}
    for path, original in originals.items():
        if original is not None:
            (backup / path.name).write_bytes(original)
    try:
        for path, value in changed.items():
            atomic_write(path, value)
    except Exception:
        for path, original in originals.items():
            if original is None:
                path.unlink(missing_ok=True)
            else:
                atomic_write(path, original)
        raise
    # Removal only detaches our pack. Keep source/attribution for safe re-enable and diagnosis.
    return dict(changed=len(changed), backup=str(backup))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    generate = sub.add_parser("build")
    generate.add_argument("--source-dir", type=Path, required=True)
    generate.add_argument("--chewing-source-dir", type=Path, required=True)
    generate.add_argument("--terra", type=Path,
                          default=Path(r"C:\Program Files\Rime\wisdom-weasel\data\terra_pinyin.dict.yaml"))
    for action in ("install", "remove"):
        command = sub.add_parser(action)
        command.add_argument("--user-dir", type=Path,
                             default=Path(os.environ.get("APPDATA", "~")) / "Rime")
    args = parser.parse_args()
    result = build(args.source_dir, args.terra, args.chewing_source_dir) if args.action == "build" else install(
        args.user_dir, args.action == "install")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
