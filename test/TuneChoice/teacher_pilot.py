"""Teacher 試跑：請雲端模型（OpenAI 相容 API，例如 DeepSeek）排序 Rime 的整句候選。

只送開放測試集的題目（TuneChoice --dump-topk 的輸出，本來就不含選字紀錄）。
正確的句子一定放進候選（Rime Top-K 沒有時插進去），每題排兩次、候選順序各自打亂，量：
  - 第一名是正確句子的比例（teacher 判斷和語料一致的程度）與 MRR
  - 兩次的第一名相同的比例（一致率）；只留兩次一致的題目時的正確率與保留比例
  - token 用量（估成本用）

API 金鑰：--key-env 指定的環境變數，或 --rime-config 讀小狼毫設定裡 llm/personal/refine 的
api_url / api_key / model（不會印出金鑰）。

用法：
  python teacher_pilot.py --topk open_topk.tsv --rime-config %APPDATA%/Rime/weasel.custom.yaml --n 500
"""
import argparse
import concurrent.futures
import json
import random
import re
import time
import urllib.request

SYSTEM = '你是繁體中文（台灣）注音輸入法的選字評審，只根據語意與用字自然度判斷。'
PROMPT = ('前文：{context}\n注音：{zhuyin}\n'
          '以下候選都是這串注音可能的轉換：\n{options}\n'
          '請把候選依「接在前文後面最自然、最可能是使用者要打的」由好到差排序。'
          '只輸出 JSON，例如 {{"ranking": [2, 0, 1]}}，陣列放所有候選的編號。')


def rime_llm_config(path):
    text = open(path, encoding='utf-8').read()
    m = re.search(r'refine:\s*\{(.*?)\}', text, re.S)
    if not m:
        raise SystemExit('找不到 llm/personal/refine 設定')
    block = m.group(1)

    def field(name):
        f = re.search(name + r':\s*"?([^",}]+)"?', block)
        return f.group(1).strip() if f else ''
    return field('api_url'), field('api_key'), field('model')


def ask(url, key, model, context, zhuyin, options, thinking=False, retries=3):
    body = {
        'model': model,
        'messages': [{'role': 'system', 'content': SYSTEM},
                     {'role': 'user', 'content': PROMPT.format(
                         context=context or '（無）', zhuyin=zhuyin,
                         options='\n'.join(f'{i}. {o}' for i, o in enumerate(options)))}],
        'temperature': 0,
        'response_format': {'type': 'json_object'},
    }
    # DeepSeek 官方 API 用 thinking.type 關掉思考（同 core/llm/LLMProvider.cpp）
    if 'deepseek.com' in url and not thinking:
        body['thinking'] = {'type': 'disabled'}
    data = json.dumps(body).encode('utf-8')
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, data=data, headers={
                'Content-Type': 'application/json', 'Authorization': 'Bearer ' + key})
            with urllib.request.urlopen(req, timeout=60) as r:
                resp = json.load(r)
            content = resp['choices'][0]['message']['content']
            ranking = json.loads(content)['ranking']
            ranking = [int(i) for i in ranking if 0 <= int(i) < len(options)]
            usage = resp.get('usage', {})
            return ranking, usage.get('prompt_tokens', 0), usage.get('completion_tokens', 0)
        except Exception as e:  # noqa: BLE001
            if attempt + 1 == retries:
                return None, 0, 0
            time.sleep(2 * (attempt + 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--topk', required=True)
    ap.add_argument('--rime-config')
    ap.add_argument('--key-env')
    ap.add_argument('--api-url')
    ap.add_argument('--model')
    ap.add_argument('--n', type=int, default=500)
    ap.add_argument('--seed', type=int, default=20260929)
    ap.add_argument('--workers', type=int, default=8)
    ap.add_argument('--thinking', action='store_true', help='保留模型的思考（比較貴）')
    args = ap.parse_args()

    url = key = model = ''
    if args.rime_config:
        url, key, model = rime_llm_config(args.rime_config)
    if args.key_env:
        import os
        key = os.environ.get(args.key_env, '')
    url = args.api_url or url
    model = args.model or model
    if not (url and key and model):
        raise SystemExit('缺少 api_url / api_key / model')
    print(f'teacher：{model} @ {url}（思考{"開" if args.thinking else "關"}）')

    rows = []
    with open(args.topk, encoding='utf-8') as f:
        for line in f:
            parts = line.rstrip('\n').split('\t')
            if len(parts) != 6:
                continue
            src, context, zhuyin, truth, draft, joined = parts
            cands = [c for c in joined.split('|') if c]
            if len(cands) < 2:
                continue
            rows.append((src, context, zhuyin, truth, draft, cands))
    rng = random.Random(args.seed)
    rng.shuffle(rows)
    rows = rows[:args.n]

    jobs = []
    for idx, (src, context, zhuyin, truth, draft, cands) in enumerate(rows):
        options = list(dict.fromkeys(cands[:10] + ([] if truth in cands[:10] else [truth])))
        for run in range(2):
            order = options[:]
            random.Random(args.seed * 31 + idx * 2 + run).shuffle(order)
            jobs.append((idx, run, order))

    results = {}
    tokens_in = tokens_out = failed = 0
    with concurrent.futures.ThreadPoolExecutor(args.workers) as ex:
        futs = {ex.submit(ask, url, key, model, rows[i][1], rows[i][2], order, args.thinking): (i, run, order)
                for i, run, order in jobs}
        for n, fut in enumerate(concurrent.futures.as_completed(futs), 1):
            i, run, order = futs[fut]
            ranking, ti, to = fut.result()
            tokens_in += ti
            tokens_out += to
            if not ranking:
                failed += 1
                continue
            results[(i, run)] = [order[k] for k in ranking]
            if n % 200 == 0:
                print(f'  {n}/{len(jobs)}')

    both = right1 = agree = agree_right = 0
    mrr = 0.0
    right_by_rime = {True: [0, 0], False: [0, 0]}
    for i, (src, context, zhuyin, truth, draft, cands) in enumerate(rows):
        r0, r1 = results.get((i, 0)), results.get((i, 1))
        if not r0 or not r1:
            continue
        both += 1
        ok = r0[0] == truth
        right1 += ok
        mrr += 1.0 / (r0.index(truth) + 1) if truth in r0 else 0
        right_by_rime[draft == truth][0] += ok
        right_by_rime[draft == truth][1] += 1
        if r0[0] == r1[0]:
            agree += 1
            agree_right += ok
    print(f'題數 {len(rows)}（兩次都有回應 {both}，失敗 {failed} 次）')
    if both:
        print(f'第一名 = 正確句子：{right1 / both:.4f}，MRR {mrr / both:.3f}')
        for k, name in ((True, 'Rime 第一句本來就對'), (False, 'Rime 第一句不對')):
            a, b = right_by_rime[k]
            if b:
                print(f'  {name}：{a / b:.4f}（{b} 題）')
        print(f'兩次第一名相同：{agree / both:.4f}；只留一致的題目：正確率 {agree_right / max(1, agree):.4f}、'
              f'保留 {agree / both:.4f}')
    print(f'token：輸入 {tokens_in}、輸出 {tokens_out}（{len(jobs)} 次請求）')


if __name__ == '__main__':
    main()
