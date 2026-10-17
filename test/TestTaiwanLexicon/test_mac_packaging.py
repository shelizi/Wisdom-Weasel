import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('prepare_squirrel', ROOT / 'mac/prepare-squirrel.py')
packaging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packaging)

ORIGINAL = '''// local application customization
  func startRime(fullCheck: Bool) -> Bool {
    print("Initializing la rime...")
    rimeAPI.initialize(nil)
    if rimeAPI.start_maintenance(fullCheck) {
      _ = rimeAPI.deploy_config_file("squirrel.yaml", "config_version")
      return true
    }
    return false
  }
'''


class MacPackagingTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.source = Path(self.tmp.name) / 'squirrel'
        self.delegate = self.source / 'sources/SquirrelApplicationDelegate.swift'
        self.delegate.parent.mkdir(parents=True)
        self.delegate.write_text(ORIGINAL, encoding='utf-8')
        subprocess.run(['git', 'init', '-q', str(self.source)], check=True)
        # git apply does not require a commit, and these tests do not use user identity.
        subprocess.run(['git', '-C', str(self.source), 'add', '.'], check=True,
                       capture_output=True)

    def test_first_and_repeated_build_keep_customization_and_bundle_attribution(self):
        packaging.prepare(self.source)
        first = self.delegate.read_bytes()
        self.assertIn(b'// local application customization', first)
        self.assertLess(first.index(b'prepareTaiwanLexicon('), first.index(b'start_maintenance('))
        self.assertIn(b'fullCheck || vocabularyChanged', first)
        packaging.prepare(self.source)
        self.assertEqual(first, self.delegate.read_bytes())
        for name in packaging.FILES:
            self.assertEqual((ROOT / 'resource/lexicons/taiwan' / name).read_bytes(),
                             (self.source / 'data/plum' / name).read_bytes())

    def test_incompatible_deployment_hook_is_preserved(self):
        original = ORIGINAL.replace('rimeAPI.initialize(nil)', 'customInitialize()')
        self.delegate.write_text(original, encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError, 'differs from the pinned version'):
            packaging.prepare(self.source)
        self.assertEqual(original, self.delegate.read_text(encoding='utf-8'))
        self.assertFalse((self.source / 'data').exists())

    def test_bad_checksum_does_not_patch_or_stage(self):
        resource = Path(self.tmp.name) / 'resource'
        shutil.copytree(ROOT / 'resource/lexicons/taiwan', resource)
        (resource / packaging.FILES[0]).write_text('corrupt', encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
            packaging.prepare(self.source, resource)
        self.assertEqual(ORIGINAL, self.delegate.read_text(encoding='utf-8'))
        self.assertFalse((self.source / 'data').exists())

    def test_missing_license_does_not_patch_or_stage(self):
        resource = Path(self.tmp.name) / 'resource'
        shutil.copytree(ROOT / 'resource/lexicons/taiwan', resource)
        (resource / packaging.FILES[1]).unlink()
        with self.assertRaisesRegex(RuntimeError, 'Missing bundled vocabulary resource'):
            packaging.prepare(self.source, resource)
        self.assertEqual(ORIGINAL, self.delegate.read_text(encoding='utf-8'))
        self.assertFalse((self.source / 'data').exists())


if __name__ == '__main__':
    unittest.main()
