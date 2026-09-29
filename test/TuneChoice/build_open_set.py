"""開放 zh-TW 測試集：公開語料的句子 → 台灣用字 → 注音（McBopomofo 詞級讀音）。

每行輸出「前文|注音||正確的句子|標籤|來源」（初稿留空，由 TuneChoice 打進 Rime 產生），
格式與 cases.txt 相同，只多了標籤與來源兩欄。

產生的題目含有來源語料的文字（zhwiki 是 CC BY-SA 4.0），所以不放進 repo；
固定來源檔與 --seed 就能重現同一份。

用法：
  python build_open_set.py --mcbpmf <McBopomofo Source/Data 資料夾> \
      --wiki <wikimedia/wikipedia 20231101.zh 的 parquet> --cc100 <CC-100 zh-Hant 文字檔開頭> \
      --out open_set.txt [--n 3000] [--seed 20260929]

讀音資料：McBopomofo（MIT）的 BPMFMappings.txt（詞 → 每字注音）、phrase.occ（詞頻）、
BPMFBase.txt（單字讀音）、heterophony1.list（破音字的預設讀音）。
"""
import argparse
import collections
import os
import random
import re

import opencc

CJK = re.compile(r'^[一-鿿]+$')
SPLIT = re.compile(r'[，。、；：？！「」『』（）《》〈〉…—\n\r\t ,.;:?!()\[\]"\'／/·．]+')
# 常見同音字（原方案 §15 A）
COMMON = set('是市事式室試的得地在再做作已以')
# 常見的輕聲字
LIGHT = set('們了著的子麼嗎呢吧啊頭個')
# 台灣用語（原方案 §15 D，只取一些代表）
TAIWAN = ['捷運', '超商', '機車', '里長', '健保', '發票', '戶政', '台鐵', '臺鐵', '高鐵', '便當', '宵夜',
          '計程車', '垃圾車', '夜市', '悠遊卡', '統一發票', '立法院', '縣市', '鄉鎮']


def load_readings(data_dir):
    """詞 → 讀音（每字一個音節），詞頻高的優先；單字 → 預設讀音"""
    occ = {}
    with open(os.path.join(data_dir, 'phrase.occ'), encoding='utf-8') as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2 and parts[1].isdigit():
                occ[parts[0]] = int(parts[1])
    single = {}
    with open(os.path.join(data_dir, 'heterophony1.list'), encoding='utf-8') as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2:
                single[parts[0]] = parts[1]
    # 同一個詞有幾種讀音時：和破音字預設讀音相同的字越多越好，常見輕聲字讀輕聲加分（我們 → ㄇㄣ˙）
    def score(word, syllables):
        s = 0
        for ch, syl in zip(word, syllables):
            if single.get(ch) == syl:
                s += 1
            if ch in LIGHT and syl.endswith('˙'):
                s += 1
        return s
    phrases = {}
    with open(os.path.join(data_dir, 'BPMFMappings.txt'), encoding='utf-8') as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            word, syllables = parts[0], parts[1:]
            if len(word) != len(syllables):
                continue
            if word not in phrases or score(word, syllables) > score(word, phrases[word]):
                phrases[word] = syllables
    readings = collections.defaultdict(list)
    with open(os.path.join(data_dir, 'BPMFBase.txt'), encoding='utf-8') as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2 and len(parts[0]) == 1:
                readings[parts[0]].append(parts[1])
    for ch, rs in readings.items():
        single.setdefault(ch, rs[0])
    polyphone = {ch for ch, rs in readings.items() if len(set(rs)) > 1}
    max_len = max(len(w) for w in phrases)
    return phrases, occ, single, polyphone, max_len


def annotate(text, phrases, occ, single, polyphone, max_len):
    """最長詞匹配（同長度時詞頻高的優先）；回傳每字的注音與沒被詞覆蓋的破音字數"""
    out, i, loose = [], 0, 0
    while i < len(text):
        best = None
        for n in range(min(max_len, len(text) - i), 1, -1):
            w = text[i:i + n]
            if w in phrases:
                best = w
                break
        if best:
            out.extend(phrases[best])
            i += len(best)
            continue
        ch = text[i]
        if ch not in single:
            return None, 0
        loose += 1 if ch in polyphone else 0
        out.append(single[ch])
        i += 1
    return out, loose


def zhuyin(syllables):
    # 輸入法記錄的格式：聲調符號之後斷開，一聲用空白
    return ' '.join(syllables)


def clauses(paragraph):
    """(前文, 子句)：子句 4～12 個漢字，前文是同一段裡它前面最多 30 個字"""
    pos = 0
    for m in SPLIT.finditer(paragraph + '\n'):
        piece = paragraph[pos:m.start()]
        if 4 <= len(piece) <= 12 and CJK.match(piece):
            yield paragraph[max(0, pos - 30):pos], piece
        pos = m.end()


def wiki_paragraphs(path, limit):
    import pyarrow.parquet as pq
    table = pq.read_table(path, columns=['text'])
    n = 0
    for text in table.column('text').to_pylist():
        for p in text.split('\n'):
            if len(p) >= 20:
                yield p
                n += 1
                if n >= limit:
                    return


def cc100_paragraphs(path, limit):
    n = 0
    with open(path, encoding='utf-8', errors='ignore') as f:
        for line in f:
            line = line.strip()
            if len(line) >= 20:
                yield line
                n += 1
                if n >= limit:
                    return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mcbpmf', required=True)
    ap.add_argument('--wiki')
    ap.add_argument('--cc100')
    ap.add_argument('--out', required=True)
    ap.add_argument('--n', type=int, default=3000)
    ap.add_argument('--seed', type=int, default=20260929)
    ap.add_argument('--paragraphs', type=int, default=200000, help='每個來源最多讀幾段')
    ap.add_argument('--max-loose', type=int, default=1, help='沒被詞覆蓋的破音字最多幾個（讀音可能錯）')
    args = ap.parse_args()

    phrases, occ, single, polyphone, max_len = load_readings(args.mcbpmf)
    cc = opencc.OpenCC('s2twp')
    rng = random.Random(args.seed)
    pools = {}
    for name, path, reader in (('wiki', args.wiki, wiki_paragraphs), ('cc100', args.cc100, cc100_paragraphs)):
        if not path:
            continue
        seen, pool = set(), []
        for p in reader(path, args.paragraphs):
            p = cc.convert(p)
            for context, target in clauses(p):
                if target in seen:
                    continue
                seen.add(target)
                syl, loose = annotate(target, phrases, occ, single, polyphone, max_len)
                if not syl or loose > args.max_loose:
                    continue
                tags = []
                if COMMON & set(target):
                    tags.append('A')
                if any(t in target for t in TAIWAN):
                    tags.append('D')
                tags.append('F0' if not context else 'F10' if len(context) < 10 else 'F30')
                pool.append((context.replace('|', ' '), zhuyin(syl), target, ','.join(tags), name))
        rng.shuffle(pool)
        pools[name] = pool
        print(f'{name}: {len(pool)} 個可用子句')
    # 各來源平均抽
    per = args.n // max(1, len(pools))
    picked = []
    for name, pool in pools.items():
        picked.extend(pool[:per])
    rng.shuffle(picked)
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# 開放 zh-TW 測試集（build_open_set.py 產生，seed %d）：前文|注音||正確|標籤|來源\n' % args.seed)
        for row in picked:
            context, zy, target, tags, src = row
            f.write(f'{context}|{zy}||{target}|{tags}|{src}\n')
    counts = collections.Counter(t for row in picked for t in row[3].split(','))
    print(f'寫出 {len(picked)} 題 → {args.out}；標籤 {dict(counts)}')


if __name__ == '__main__':
    main()
