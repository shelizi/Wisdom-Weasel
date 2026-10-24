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


def preimage(patch):
    """Rebuild the hunks' original text from the patch itself.

    The macOS core CI job checks out without submodules; the squirrel job applies
    the patch to the real pinned file when it builds the app.
    """
    lines, in_hunk = [], False
    for line in patch.read_text(encoding='utf-8').splitlines():
        if line.startswith('@@'):
            in_hunk = True
        elif in_hunk and line[:1] in (' ', '-'):
            lines.append(line[1:])
        elif in_hunk and not line.startswith('+'):
            in_hunk = False
    return '\n'.join(lines) + '\n'


INPUT_CONTROLLER = preimage(ROOT / 'mac/squirrel/preedit-selection.patch')
STYLE = '''style:
  inline_preedit: true
  # Whether to embed selected candidate as text in input field
  inline_candidate: false
  memorize_size: true
'''


class MacPackagingTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.source = Path(self.tmp.name) / 'squirrel'
        self.delegate = self.source / 'sources/SquirrelApplicationDelegate.swift'
        self.delegate.parent.mkdir(parents=True)
        self.delegate.write_text(ORIGINAL, encoding='utf-8')
        self.controller = self.source / 'sources/SquirrelInputController.swift'
        self.controller.write_text(INPUT_CONTROLLER, encoding='utf-8')
        self.style = self.source / 'data/squirrel.yaml'
        self.style.parent.mkdir()
        self.style.write_text(STYLE, encoding='utf-8')
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
        controller = self.controller.read_bytes()
        self.assertIn(b'exactSelection: true', controller)
        packaging.prepare(self.source)
        self.assertEqual(first, self.delegate.read_bytes())
        self.assertEqual(controller, self.controller.read_bytes())
        for name in packaging.FILES:
            self.assertEqual((ROOT / 'resource/lexicons/taiwan' / name).read_bytes(),
                             (self.source / 'data/plum' / name).read_bytes())

    def test_inline_candidate_is_on_by_default(self):
        packaging.prepare(self.source)
        expected = STYLE.replace('inline_candidate: false', 'inline_candidate: true')
        self.assertEqual(expected, self.style.read_text(encoding='utf-8'))
        packaging.prepare(self.source)
        self.assertEqual(expected, self.style.read_text(encoding='utf-8'))

    def test_missing_inline_candidate_does_not_patch_or_stage(self):
        style = STYLE.replace('  inline_candidate: false\n', '')
        self.style.write_text(style, encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError, 'inline_candidate'):
            packaging.prepare(self.source)
        self.assertEqual(ORIGINAL, self.delegate.read_text(encoding='utf-8'))
        self.assertEqual(style, self.style.read_text(encoding='utf-8'))
        self.assertFalse((self.source / 'data/plum').exists())

    def test_incompatible_deployment_hook_is_preserved(self):
        original = ORIGINAL.replace('rimeAPI.initialize(nil)', 'customInitialize()')
        self.delegate.write_text(original, encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError, 'differs from the pinned version'):
            packaging.prepare(self.source)
        self.assertEqual(original, self.delegate.read_text(encoding='utf-8'))
        self.assertEqual(INPUT_CONTROLLER, self.controller.read_text(encoding='utf-8'))
        self.assertFalse((self.source / 'data/plum').exists())
        self.assertEqual(STYLE, self.style.read_text(encoding='utf-8'))

    def test_incompatible_input_controller_leaves_hook_unpatched(self):
        changed = INPUT_CONTROLLER.replace('func show(preedit: String', 'func showMarked(preedit: String')
        self.controller.write_text(changed, encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError, 'preedit-selection.patch'):
            packaging.prepare(self.source)
        self.assertEqual(ORIGINAL, self.delegate.read_text(encoding='utf-8'))
        self.assertEqual(changed, self.controller.read_text(encoding='utf-8'))
        self.assertFalse((self.source / 'data/plum').exists())

    def test_bad_checksum_does_not_patch_or_stage(self):
        resource = Path(self.tmp.name) / 'resource'
        shutil.copytree(ROOT / 'resource/lexicons/taiwan', resource)
        (resource / packaging.FILES[0]).write_text('corrupt', encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
            packaging.prepare(self.source, resource)
        self.assertEqual(ORIGINAL, self.delegate.read_text(encoding='utf-8'))
        self.assertFalse((self.source / 'data/plum').exists())
        self.assertEqual(STYLE, self.style.read_text(encoding='utf-8'))

    def test_missing_license_does_not_patch_or_stage(self):
        resource = Path(self.tmp.name) / 'resource'
        shutil.copytree(ROOT / 'resource/lexicons/taiwan', resource)
        (resource / packaging.FILES[1]).unlink()
        with self.assertRaisesRegex(RuntimeError, 'Missing bundled vocabulary resource'):
            packaging.prepare(self.source, resource)
        self.assertEqual(ORIGINAL, self.delegate.read_text(encoding='utf-8'))
        self.assertFalse((self.source / 'data/plum').exists())
        self.assertEqual(STYLE, self.style.read_text(encoding='utf-8'))

    def test_bad_source_archive_does_not_patch_or_stage(self):
        resource = Path(self.tmp.name) / 'resource'
        shutil.copytree(ROOT / 'resource/lexicons/taiwan', resource)
        (resource / packaging.FILES[3]).write_bytes(b'corrupt source archive')
        with self.assertRaisesRegex(RuntimeError, 'source checksum mismatch'):
            packaging.prepare(self.source, resource)
        self.assertEqual(ORIGINAL, self.delegate.read_text(encoding='utf-8'))
        self.assertFalse((self.source / 'data/plum').exists())
        self.assertEqual(STYLE, self.style.read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
