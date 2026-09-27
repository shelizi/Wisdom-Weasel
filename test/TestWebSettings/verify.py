"""比對網頁版設定整合測試的結果（run.ps1 呼叫）。

apply.json：自我測試的檢查項目、套用的結果與預期值；verify.json：重新開啟後讀回的狀態；
另外直接檢查沙盒裡寫出的 custom.yaml 與部署結果。
"""
import json
import os
import re
import sys

import yaml

work = sys.argv[1]
user = os.path.join(work, 'user')
failures = []


def check(ok, name, detail=''):
    # 失敗時印出的內容可能含模型設定：遮掉 API 金鑰
    detail = re.sub(r'("?api_key"?\s*[:=]\s*)"[^"]*"', r'\1"***"', detail)
    print(('PASS ' if ok else 'FAIL ') + name + (('  — ' + detail) if detail and not ok else ''))
    if not ok:
        failures.append(name)


def load_json(name):
    with open(os.path.join(work, name), encoding='utf-8') as f:
        return json.load(f)


def load_yaml(name):
    path = os.path.join(user, name)
    if not os.path.exists(path):
        return {}
    with open(path, encoding='utf-8') as f:
        return yaml.safe_load(f) or {}


def patch_get(doc, path):
    """custom.yaml 的 patch：鍵可能是 "a/b" 也可能是巢狀的"""
    patch = doc.get('patch') or {}
    if path in patch:
        return patch[path]
    value = patch
    for key in path.split('/'):
        if not isinstance(value, dict) or key not in value:
            return None
        value = value[key]
    return value


apply = load_json('apply.json')
verify = load_json('verify.json')
check('error' not in apply, 'apply phase ran', apply.get('error', ''))
check('error' not in verify, 'verify phase ran', verify.get('error', ''))

for c in apply.get('checks', []):
    check(c['ok'], 'selftest: ' + c['name'], c.get('detail', ''))

result = apply.get('apply', {})
check(result.get('ok') is True, 'apply succeeded', json.dumps(result, ensure_ascii=False))
check('已套用' in result.get('status', ''), 'apply status says applied', result.get('status', ''))
check(result.get('dirty') == [], 'nothing left dirty after apply', str(result.get('dirty')))

# 重新開啟後讀回的值等於套用時的預期
expected = apply.get('expected', {})
state = verify.get('state', {})
# 注音校正沒選模型時，讀回來預設與智慧預測相同（與原本設定視窗的規則一樣）
expected_use = dict(expected.get('use', {}))
if expected_use.get('typo', -1) < 0:
    expected_use['typo'] = expected_use.get('predict', -1)
expected['use'] = expected_use
for key in ('schemas', 'style', 'profiles', 'use'):
    check(state.get(key) == expected.get(key), 'reloaded ' + key,
          json.dumps({'expected': expected.get(key), 'got': state.get(key)}, ensure_ascii=False)[:600])
exp_form, got_form = expected.get('form', {}), state.get('form', {})
for section in ('predict', 'typo', 'choice', 'personal'):
    check(got_form.get(section) == exp_form.get(section), 'reloaded form.' + section,
          json.dumps({'expected': exp_form.get(section), 'got': got_form.get(section)}, ensure_ascii=False))
check(state.get('grammar', {}).get('enabled') == exp_form.get('choice', {}).get('grammar'),
      'reloaded grammar enabled', str(state.get('grammar')))

# 寫出的設定檔
weasel = load_yaml('weasel.custom.yaml')
llm = patch_get(weasel, 'llm') or {}
check(str(llm.get('enabled')).lower() == 'true', 'weasel.custom.yaml llm/enabled', str(llm.get('enabled')))
profiles = llm.get('profiles') or {}
check(any((p or {}).get('name') == 'selftest API' for p in profiles.values()), 'weasel.custom.yaml has new profile')
check(llm.get('provider_type') == 'openai' and (llm.get('openai') or {}).get('model') == 'selftest-model',
      'predict profile expanded to llm/openai', json.dumps({'provider_type': llm.get('provider_type'), 'openai': llm.get('openai')}, ensure_ascii=False))
# Rime 把多行文字存成 YAML 的 | 區塊，讀回來會多一個結尾換行
check((llm.get('prompt') or '').rstrip('\n') == '自我測試提示詞\n第二行', 'prompt saved with newline', repr(llm.get('prompt')))
check('prompt_prefix' not in (llm.get('llamacpp') or {}), 'legacy llamacpp/prompt_prefix removed')
check((llm.get('typo') or {}).get('prompt') == '自我測試校正指令', 'typo prompt saved')
personal = llm.get('personal') or {}
check(str(personal.get('max_candidates')) == '2' and str(personal.get('half_life_days')) == '45'
      and str((personal.get('refine') or {}).get('interval_days')) == '3', 'personal numbers saved',
      json.dumps(personal, ensure_ascii=False)[:300])
check(patch_get(weasel, 'style/color_scheme') == expected['style']['active'], 'style/color_scheme saved',
      str(patch_get(weasel, 'style/color_scheme')))
check(str(patch_get(weasel, 'style/font_point')) == str(expected['style']['fonts']['font_point']),
      'style/font_point saved', str(patch_get(weasel, 'style/font_point')))

default = load_yaml('default.custom.yaml')
schema_list = [item.get('schema') for item in (patch_get(default, 'schema_list') or [])]
check(schema_list == expected.get('schemas'), 'default.custom.yaml schema_list', str(schema_list))

# 注音方案的區塊（開關都有翻轉，檢查新狀態）
zhuyin = ['bopomofo', 'bopomofo_express', 'bopomofo_tw']
texts = {}
for s in zhuyin:
    path = os.path.join(user, s + '.custom.yaml')
    texts[s] = open(path, encoding='utf-8').read() if os.path.exists(path) else ''
typo_on = exp_form['typo']['rime']
check(all(('weasel-typo-correction' in texts[s]) == typo_on for s in zhuyin if texts[s] or typo_on),
      'typo correction block ' + ('added' if typo_on else 'removed'))
grammar_on = exp_form['choice'].get('grammar')
check(all(('weasel-grammar' in texts[s]) == bool(grammar_on) for s in zhuyin if texts[s] or grammar_on),
      'grammar block ' + ('added' if grammar_on else 'removed'))
boost_on = exp_form['personal']['enabled'] and exp_form['personal']['rime_boost']
selected_zhuyin = [s for s in zhuyin if s in expected['schemas']]
check(all(('weasel-personal-dict' in texts[s]) == boost_on for s in selected_zhuyin),
      'rime boost block ' + ('added' if boost_on else 'removed') + ' for selected zhuyin schemas')
check(os.path.exists(os.path.join(user, 'terra_pinyin.personal.dict.yaml')) == boost_on,
      'personal dict file ' + ('present' if boost_on else 'removed'))

# 部署：build 目錄裡的 weasel.yaml 反映新設定
build = os.path.join(user, 'build', 'weasel.yaml')
built = {}
if os.path.exists(build):
    with open(build, encoding='utf-8') as f:
        built = yaml.safe_load(f) or {}
check(str((built.get('llm') or {}).get('enabled')).lower() == 'true', 'deployed build/weasel.yaml has llm/enabled')
check((built.get('style') or {}).get('color_scheme') == expected['style']['active'], 'deployed color scheme')

print()
print(f'{len(failures)} failed' if failures else 'all passed')
sys.exit(1 if failures else 0)
