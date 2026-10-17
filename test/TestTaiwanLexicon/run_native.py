"""Run real-librime vocabulary tests without touching the user's Rime data."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
binary, shared = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix='wisdom-taiwan-') as directory:
    sandbox = Path(directory)
    staged = sandbox / 'shared'
    shutil.copytree(shared, staged)
    for name in ('wisdom_taiwan.dict.yaml', 'wisdom_taiwan.manifest.json', 'wisdom_taiwan.NOTICE.txt'):
        shutil.copyfile(root / 'resource/lexicons/taiwan' / name, staged / name)
    user = sandbox / 'user'
    user.mkdir()
    (user / 'default.custom.yaml').write_text(
        'patch:\n  schema_list:\n    - schema: bopomofo\n    - schema: bopomofo_express\n'
        '    - schema: bopomofo_tw\n', encoding='utf-8')
    (user / 'bopomofo_express.custom.yaml').write_text(
        'patch:\n  translator/dictionary: terra_pinyin.personal\n'
        '  translator/user_dict: terra_pinyin\n', encoding='utf-8')
    (user / 'terra_pinyin.personal.dict.yaml').write_text(
        "---\nname: terra_pinyin.personal\nversion: '1'\nsort: by_weight\n"
        'use_preset_vocabulary: true\nimport_tables: [terra_pinyin]\n...\n', encoding='utf-8')
    subprocess.run([str(binary), str(staged), str(user)], check=True)
