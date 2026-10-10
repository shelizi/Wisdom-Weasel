"""自寫句子標上台灣注音，組成選字測試集。

讀音只用來標注，詞表不進題庫：McBopomofo（MIT）
https://github.com/openvanilla/McBopomofo 的 BPMFMappings.txt、phrase.occ、
BPMFBase.txt、heterophony1.list。

拉風公開驗證集另成一個檔（CC BY 4.0）：
https://github.com/llavon-ime/validation-set

用法：
  python build_select_set.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "..", "msbuild", "TuneChoice", "mcbpmf")
ITEMS = os.path.join(HERE, "select_items.tsv")
CASES = os.path.join(HERE, "cases.txt")
OUT = os.path.join(HERE, "select_set.txt")
LLAVON_OUT = os.path.join(HERE, "select_llavon.txt")

LIGHT = set("們了著的子麼嗎呢吧啊頭個")


def load_readings(data_dir):
    occ = {}
    with open(os.path.join(data_dir, "phrase.occ"), encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2 and parts[1].isdigit():
                occ[parts[0]] = int(parts[1])
    single = {}
    with open(os.path.join(data_dir, "heterophony1.list"), encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2:
                single[parts[0]] = parts[1]
    readings = {}
    with open(os.path.join(data_dir, "BPMFBase.txt"), encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2 and len(parts[0]) == 1:
                readings.setdefault(parts[0], []).append(parts[1])
    for ch, rs in readings.items():
        single.setdefault(ch, rs[0])
    polyphone = {ch for ch, rs in readings.items() if len(set(rs)) > 1}

    def score(word, syllables):
        s = 0
        for ch, syl in zip(word, syllables):
            if single.get(ch) == syl:
                s += 1
            if ch in LIGHT and syl.endswith("˙"):
                s += 1
        return s

    phrases = {}
    with open(os.path.join(data_dir, "BPMFMappings.txt"), encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            word, syllables = parts[0], parts[1:]
            if len(word) != len(syllables):
                continue
            if word not in phrases or score(word, syllables) > score(word, phrases[word]):
                phrases[word] = syllables
    max_len = max(len(w) for w in phrases)
    return phrases, single, polyphone, max_len


def annotate(text, phrases, single, polyphone, max_len):
    out, i, loose = [], 0, 0
    while i < len(text):
        best = None
        for n in range(min(max_len, len(text) - i), 1, -1):
            w = text[i : i + n]
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
    return " ".join(syllables)


def parse_items(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                raise SystemExit(f"{path}:{lineno} 欄位不足")
            tags, context, text = parts[0], parts[1], parts[2]
            override = parts[3] if len(parts) > 3 else ""
            rows.append((lineno, tags, context, text, override))
    return rows


def tone_mark(tone):
    return {1: "", 2: "ˊ", 3: "ˇ", 4: "ˋ", 5: "˙"}.get(tone, "")


def llavon_rows(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            syllables = []
            for pad in obj["padding"]:
                mark = tone_mark(int(pad["tone"]))
                syl = pad["syllable"] + mark
                syllables.append(syl)
            rows.append((obj.get("context") or "", zhuyin(syllables), obj["answer"], obj.get("difficulty")))
    return rows


def main():
    phrases, single, polyphone, max_len = load_readings(DATA)
    items = parse_items(ITEMS)
    lines = []
    loose_rows = []
    errors = []
    for lineno, tags, context, text, override in items:
        alts = [a for a in text.split("/") if a]
        readings = []
        loose = 0
        if override:
            if len(alts) != 1:
                errors.append(f"{lineno} 覆寫不能和斜線並用")
                continue
            syllables = override.split()
            if len(syllables) != len(alts[0]):
                errors.append(f"{lineno} 覆寫音節數 {len(syllables)} != 字數 {len(alts[0])} {alts[0]}")
                continue
            readings.append(syllables)
        else:
            for alt in alts:
                syl, loose_n = annotate(alt, phrases, single, polyphone, max_len)
                if not syl:
                    errors.append(f"{lineno} 標不了音 {alt}")
                    syl = None
                    break
                readings.append(syl)
                loose = max(loose, loose_n)
            if not readings or readings[-1] is None:
                continue
            joined = {zhuyin(s) for s in readings}
            if len(joined) != 1:
                errors.append(f"{lineno} 斜線兩邊注音不同 {text} {joined}")
                continue
        zy = zhuyin(readings[0])
        truth = "/".join(alts)
        lines.append((tags, context, zy, truth, loose, lineno))
        if loose and "poly" in tags.split(","):
            loose_rows.append((lineno, text, zy, loose))

    pairs = {}
    for tags, context, zy, truth, loose, lineno in lines:
        for tag in tags.split(","):
            if tag.startswith("pair"):
                pairs.setdefault(tag, []).append((lineno, zy, truth, context))
    for pid, group in sorted(pairs.items()):
        zys = {g[1] for g in group}
        if len(zys) != 1:
            errors.append(f"{pid} 注音不一致 " + " | ".join(f"{g[0]}:{g[2]}:{g[1]}" for g in group))
        if len(group) < 2:
            errors.append(f"{pid} 只有一題")

    legacy = []
    if os.path.exists(CASES):
        with open(CASES, encoding="utf-8") as f:
            for line in f:
                raw = line.rstrip("\n")
                if not raw or raw.startswith("#"):
                    continue
                parts = raw.split("|")
                if len(parts) < 4:
                    continue
                tags = "legacy"
                if len(parts) > 4 and parts[4]:
                    tags = parts[4] + ",legacy"
                legacy.append(f"{parts[0]}|{parts[1]}|{parts[2]}|{parts[3]}|{tags}")

    llavon = []
    val = os.path.join(DATA, "validation.jsonl")
    if os.path.exists(val):
        for context, zy, answer, diff in llavon_rows(val):
            llavon.append(f"{context}|{zy}||{answer}|llavon,d{diff}")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("# 選字測試集。格式：前文|注音|初稿|正確|標籤\n")
        f.write("# 主集合是自寫句子。注音由 McBopomofo 詞表標注（MIT，openvanilla/McBopomofo），詞表文字不在此檔。\n")
        f.write("# 斜線是同一鍵都能接受的寫法。undecidable 不計入主正確率。\n")
        f.write("# legacy 來自同目錄 cases.txt。\n")
        for tags, context, zy, truth, loose, lineno in lines:
            f.write(f"{context}|{zy}||{truth}|{tags}\n")
        for row in legacy:
            f.write(row + "\n")
    with open(LLAVON_OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("# 拉風輸入法公開驗證集，CC BY 4.0。\n")
        f.write("# 來源 https://github.com/llavon-ime/validation-set\n")
        f.write("# 只保留答案的注音。前文在第一欄，這次打鍵不會用到。\n")
        for row in llavon:
            f.write(row + "\n")

    from collections import Counter
    tags = Counter()
    for row in lines:
        for t in row[0].split(","):
            tags[t] += 1
    print(f"自寫 {len(lines)} 題，legacy {len(legacy)}，拉風 {len(llavon)}")
    print("標籤", dict(tags))
    if loose_rows:
        print("--- 破音未進詞（poly）---")
        for row in loose_rows:
            print(f"{row[0]}\tloose={row[3]}\t{row[1]}\t{row[2]}")
    if errors:
        print("--- 錯誤 ---", file=sys.stderr)
        for e in errors:
            print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
