import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("taiwan_lexicon", ROOT / "tools/taiwan_lexicon.py")
lexicon = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lexicon)


class TaiwanLexiconTest(unittest.TestCase):
    def test_chewing_frequencies_are_normalized_and_duplicates_are_not_summed(self):
        with tempfile.TemporaryDirectory() as tmp:
            csv_file = Path(tmp) / 'tsi.csv'
            csv_file.write_text(
                '# dc:license,LGPL-2.1-or-later,\n'
                '高鐵,100000,ㄍㄠ ㄊㄧㄝˇ\n'
                '高鐵,2,ㄍㄠ ㄊㄧㄝˇ\n'
                '發票,10,ㄈㄚ ㄆㄧㄠˋ\n'
                '你的,0,ㄋㄧˇ ˙ㄉㄜ\n'
                '錯音,30,ㄘㄨㄛˋ ㄧㄣ\n', encoding='utf-8')
            entries = {('高鐵', 'gao1 tie3'): 5000}
            readings = {'ㄍㄠ': 'gao1', 'ㄊㄧㄝˇ': 'tie3', 'ㄈㄚ': 'fa1',
                        'ㄆㄧㄠˋ': 'piao4', 'ㄋㄧˇ': 'ni3', 'ㄉㄜ˙': 'de5'}
            stats = lexicon.merge_chewing(entries, readings, set(readings.values()), csv_file)
            self.assertEqual(entries['高鐵', 'gao1 tie3'], 5000)
            self.assertTrue(100 < entries['發票', 'fa1 piao4'] <= 1000)
            self.assertEqual(entries['你的', 'ni3 de5'], 100)
            self.assertEqual(stats['added'], 2)
            self.assertEqual(stats['overlaps'], 1)
            self.assertEqual(stats['skipped_readings'], 1)
            self.assertEqual(stats['weights_updated'], 0)

    def test_source_bundle_contains_pinned_sources_licenses_and_rebuild_code(self):
        manifest = json.loads((lexicon.RESOURCE / 'wisdom_taiwan.manifest.json').read_text())
        archive = lexicon.RESOURCE / 'wisdom_taiwan.sources.zip'
        self.assertEqual(lexicon.digest(archive), manifest['sources_sha256'])
        self.assertEqual(manifest['chewing']['revision'], lexicon.CHEWING_REVISION)
        with zipfile.ZipFile(archive) as bundle:
            import hashlib
            self.assertEqual(hashlib.sha256(bundle.read('libchewing-data/dict/chewing/tsi.csv')).hexdigest(),
                             lexicon.CHEWING_HASH)
            for name in ('libchewing-data/LICENSES/LGPL-2.1-or-later.txt',
                         'libchewing-data/LICENSES/GPL-2.0-or-later.txt',
                         'McBopomofo/LICENSE.txt', 'REBUILD.txt', 'syllables.txt'):
                self.assertGreater(len(bundle.read(name)), 100)
            self.assertEqual(bundle.read('tools/taiwan_lexicon.py'),
                             (ROOT / 'tools/taiwan_lexicon.py').read_bytes().replace(b'\r\n', b'\n'))

    def test_patch_retains_existing_settings_and_line_endings(self):
        original = (b"\xef\xbb\xbf# my configuration\r\npatch:\r\n"
                    b"  translator/dictionary: terra_pinyin.personal\r\n"
                    b"  translator/packs: [existing_pack]\r\n"
                    b"  translator/user_dict: terra_pinyin\r\n"
                    b"  speller/algebra: ['derive/ab/ba/']\r\n")
        patched = lexicon.patch_custom(original, True)
        self.assertIn(b"translator/packs/+:\r\n    - wisdom_taiwan", patched)
        self.assertEqual(lexicon.patch_custom(patched, True), patched)
        self.assertEqual(lexicon.patch_custom(patched, False), original)

    def test_invalid_config_does_not_change_any_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            user = Path(tmp)
            (user / "bopomofo.custom.yaml").write_text("patch:\n  menu/page_size: 7\n")
            (user / "bopomofo_express.custom.yaml").write_text("patch: [bad]\n")
            before = {p.name: p.read_bytes() for p in user.iterdir()}
            with self.assertRaises(ValueError):
                lexicon.install(user)
            self.assertEqual({p.name: p.read_bytes() for p in user.iterdir()}, before)
        for text in (b"patch: {menu/page_size: 7}\n", b"patch:\n" + lexicon.BEGIN.encode() + b"\n"):
            with self.assertRaises(ValueError):
                lexicon.patch_custom(text, True)

    def test_public_install_never_changes_personal_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            user = Path(tmp)
            original = b"patch:\n  translator/dictionary: terra_pinyin.personal\n"
            (user / "bopomofo.custom.yaml").write_bytes(original)
            (user / "bopomofo_express.custom.yaml").write_bytes(original)
            protected = ("lexicon.dat", "refine.dat", "terra_pinyin.userdb", "custom_phrase.txt",
                         "terra_pinyin.personal.dict.yaml")
            for name in protected:
                (user / name).write_bytes(("untouched-" + name).encode())
            snapshots = {name: (user / name).read_bytes() for name in protected}
            report = lexicon.install(user)
            self.assertGreater(report["changed"], 0)
            self.assertEqual((Path(report["backup"]) / "bopomofo.custom.yaml").read_bytes(), original)
            self.assertNotIn(b"translator/packs/+", (user / "bopomofo_express.custom.yaml").read_bytes())
            self.assertEqual(lexicon.install(user)["changed"], 0)
            lexicon.install(user, False)
            self.assertFalse(lexicon.yaml.safe_load((user / "wisdom_taiwan.user.yaml").read_text())["enabled"])
            self.assertEqual((user / "bopomofo_express.custom.yaml").read_bytes(), original)
            self.assertEqual({name: (user / name).read_bytes() for name in protected}, snapshots)

    def test_pack_contains_readings_and_provenance(self):
        manifest = json.loads((lexicon.RESOURCE / "wisdom_taiwan.manifest.json").read_text(encoding="utf-8"))
        dictionary = lexicon.RESOURCE / "wisdom_taiwan.dict.yaml"
        self.assertEqual(lexicon.digest(dictionary), manifest["sha256"])
        self.assertEqual(manifest["revision"], lexicon.REVISION)
        rows = {}
        for line in dictionary.read_text(encoding="utf-8").split("...\n", 1)[1].splitlines():
            word, code, weight = line.split("\t")
            self.assertEqual(len(word), len(code.split()))
            self.assertTrue(100 <= int(weight) <= 10000)
            rows[word, code] = weight
        self.assertEqual(len(rows), manifest["entries"])
        for word, code in (("台積電", "tai2 ji1 dian4"), ("悠遊卡", "you1 you2 ka3"),
                           ("台灣高鐵", "tai2 wan1 gao1 tie3"), ("電子發票", "dian4 zi3 fa1 piao4"),
                           ("行動支付", "xing2 dong4 zhi1 fu4")):
            self.assertIn((word, code), rows)


if __name__ == "__main__":
    unittest.main()
