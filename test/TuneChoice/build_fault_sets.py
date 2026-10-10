"""注音亂序、打錯鍵、同音錯字。三份題目都從可讀的列產生，不在評測當下才抽。

句子來自 select_set.txt（自寫＋舊公開題）。每句每種亂序、每種打錯各留一題，
位置用固定種子抽出。另外用一組單音節、雙音節把每一種形狀都列滿，不抽樣。

亂序的「聲調接在第一個符號後面」是目前快打方案有的規則。聲調打在最前、
接在第三碼前面、以及相鄰兩音節對調，方案沒有。

格式與 typo_probe --fault 相同：
  前文|看得出的鍵|初稿|正確|標籤|大千鍵|原鍵|錯字
原鍵空白表示這題的鍵是對的。
"""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SELECT = os.path.join(HERE, "select_set.txt")
CASES = os.path.join(HERE, "cases.txt")
OUT_ORDER = os.path.join(HERE, "disorder_set.txt")
OUT_TYPO = os.path.join(HERE, "typo_set.txt")
OUT_HANZI = os.path.join(HERE, "wrong_hanzi_set.txt")

SYMBOLS = "ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ"
KEYS = "1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/-"
TO_KEY = {s: k for s, k in zip(SYMBOLS, KEYS)}
TO_SYM = {k: s for s, k in zip(SYMBOLS, KEYS)}
TONE_MARK = {"6": "ˊ", "3": "ˇ", "4": "ˋ", "7": "˙"}
TONE_KEY = {"ˊ": "6", "ˇ": "3", "ˋ": "4", "˙": "7"}
INITIAL = set("1qaz2wsxedcrfv5tgbyhn")
MEDIAL = set("ujm")
FINAL = set("8ik,9ol.0p;/-")
TONE = set(" 6347")
NEI = {
    "1": "2qw", "2": "13qwe", "3": "24wer", "4": "35ert", "5": "46rty",
    "6": "57tyu", "7": "68yui", "8": "79uio", "9": "80iop", "0": "9-op[",
    "-": "0=p[]", "=": "-[]\\", "q": "w", "w": "qe", "e": "wr", "r": "et",
    "t": "ry", "y": "tu", "u": "yi", "i": "uo", "o": "ip", "p": "o[",
    "[": "p]", "]": "[\\", "\\": "]", "a": "s", "s": "ad", "d": "sf",
    "f": "dg", "g": "fh", "h": "gj", "j": "hk", "k": "jl", "l": "k;",
    ";": "l'", "'": ";", "z": "x", "x": "zc", "c": "xv", "v": "cb",
    "b": "vn", "n": "bm", "m": "n,", ",": "m.", ".": ",/", "/": ".",
}
COLUMNS = ["1qaz", "2wsx", "3edc", "4rfv", "5tgb", "6yhn", "7ujm", "8ik,", "9ol.", "0p;/"]

# 單音節、雙音節。注音手寫，用來把每種形狀列滿。
GRID = [
    ("爸", "ㄅㄚˋ"), ("家", "ㄐㄧㄚ"), ("光", "ㄍㄨㄤ"), ("見", "ㄐㄧㄢˋ"),
    ("選", "ㄒㄩㄢˇ"), ("對", "ㄉㄨㄟˋ"), ("月", "ㄩㄝˋ"), ("安", "ㄢ"),
    ("因", "ㄧㄣ"), ("完", "ㄨㄢˊ"), ("元", "ㄩㄢˊ"), ("表", "ㄅㄧㄠˇ"),
    ("快", "ㄎㄨㄞˋ"), ("全", "ㄑㄩㄢˊ"), ("花", "ㄏㄨㄚ"), ("六", "ㄌㄧㄡˋ"),
    ("吃", "ㄔ"), ("是", "ㄕˋ"), ("日", "ㄖˋ"), ("二", "ㄦˋ"),
    ("中", "ㄓㄨㄥ"), ("次", "ㄘˋ"), ("今天", "ㄐㄧㄣ ㄊㄧㄢ"),
    ("台灣", "ㄊㄞˊ ㄨㄢ"), ("我們", "ㄨㄛˇ ㄇㄣˊ"), ("咖啡", "ㄎㄚ ㄈㄟ"),
]

# 正確句、同鍵錯字。讀音不一致的會在產生時丟掉。
HANZI = [
    ("我們必須今天交", "我們必需今天交"),
    ("聽到巨響以後他的反應很快", "聽到巨響以後他的反映很快"),
    ("即使下雨也要去上課", "既使下雨也要去上課"),
    ("請一併確認附件", "請一並確認附件"),
    ("這間教室的燈很亮", "這間教士的燈很亮"),
    ("小孩在公園裡騎腳踏車", "小孩在公元裡騎腳踏車"),
    ("消費者有退貨的權利", "消費者有退貨的權力"),
    ("主管機關行使權力", "主管機關行使權利"),
    ("中心思想寫在第一段", "忠心思想寫在第一段"),
    ("他的忠心沒人懷疑", "他的衷心沒人懷疑"),
    ("一番衷心的話", "一番中心的話"),
    ("黑板上寫著公式", "黑板上寫著公事"),
    ("他下午還有公事", "他下午還有攻勢"),
    ("對手發動攻勢", "對手發動公式"),
    ("旅遊的目的要先講", "旅遊的墓地要先講"),
    ("再接再厲把下一場打好", "再接再勵把下一場打好"),
    ("他走投無路才來借錢", "他走頭無路才來借錢"),
    ("迫不及待想知道結果", "迫不急待想知道結果"),
    ("這個現象值得再研究", "這個現像值得再研究"),
    ("影像已經上傳到雲端", "影象已經上傳到雲端"),
    ("因為下雨所以改期", "音位下雨所以改期"),
    ("他的意識已經恢復", "他的議事已經恢復"),
    ("利益要講清楚", "立意要講清楚"),
    ("時間不夠所以先做重點", "十間不夠所以先做重點"),
    ("世界那麼大", "視界那麼大"),
    ("家人都在南部", "加人都在南部"),
    ("危機處理小組到了", "微機處理小組到了"),
    ("做完作業再出去", "作完作業再出去"),
    ("好像快下雨了", "好象快下雨了"),
    ("今天天氣很好", "今天天汽很好"),
    ("我們明天見", "我門明天見"),
    ("謝謝你的幫忙", "謝謝你的邦忙"),
    ("這個問題很簡單", "這個問題很簡丹"),
    ("要拿去修理", "要拿去修里"),
    ("我想喝一杯咖啡", "我想喝一杯咖非"),
    ("他是我的好朋友", "他是我的好朋有"),
    ("昨天我去看電影", "昨天我去看電穎"),
    ("請大家準時出席會議", "請大家準時出席惠議"),
    ("電力公司來檢修", "電離公司來檢修"),
    ("基金會在徵人", "機金會在徵人"),
    ("記事本放在抽屜", "計事本放在抽屜"),
    ("政府公布新辦法", "正府公布新辦法"),
    ("科學實驗要重複做", "顆學實驗要重複做"),
    ("法律規定要先告知", "發律規定要先告知"),
    ("應該先問過負責人", "應改先問過負責人"),
    ("我記得把門關上", "我計得把門關上"),
    ("歷史老師今天請假", "理事老師今天請假"),
    ("這份禮儀手冊給新人", "這份利益手冊給新人"),
    ("希望你順利", "西望你順利"),
    ("大家早上九點到", "打家早上九點到"),
    ("比較以後再決定", "筆較以後再決定"),
    ("研究計畫已經送出", "嚴究計畫已經送出"),
    ("新帳號今天起可以登入", "新帳號今天起可以登入"),
]


def syllable_keys(zhuyin):
    out, cur = [], []

    def flush(tone):
        if cur:
            out.append("".join(cur) + tone)
            cur.clear()

    for ch in zhuyin:
        if ch in TO_KEY:
            cur.append(TO_KEY[ch])
        elif ch in TONE_KEY:
            flush(TONE_KEY[ch])
        elif ch == " ":
            flush(" ")
        elif ch == "/":
            return ""
        else:
            return ""
    flush(" ")
    return "".join(out)


def render(keys):
    cur, out = [], []

    def flush(tone):
        if not cur:
            if tone != " ":
                out.append(TONE_MARK[tone])
            return
        out.append("".join(cur))
        out.append(TONE_MARK.get(tone, " "))
        cur.clear()

    for ch in keys:
        if ch in TO_SYM:
            cur.append(TO_SYM[ch])
        elif ch in TONE_MARK or ch == " ":
            flush(ch)
        else:
            # 鄰鍵有時會跑到注音用不到的鍵，例如分號右邊的引號。
            flush(" ")
            out.append(ch)
    if cur:
        flush(" ")
    text = "".join(out)
    return " ".join(text.split())


def split_syllables(keys):
    out, i = [], 0
    while i < len(keys):
        begin = i
        while i < len(keys) and keys[i] not in TONE:
            i += 1
        if i >= len(keys):
            break
        out.append((keys[begin:i], keys[i], begin, i + 1))
        i += 1
    return out


def shape(body):
    if len(body) == 2 and body[0] in INITIAL and body[1] in MEDIAL:
        return 1
    if len(body) == 2 and body[0] in INITIAL and body[1] in FINAL:
        return 2
    if len(body) == 2 and body[0] in MEDIAL and body[1] in FINAL:
        return 3
    if len(body) == 3 and body[0] in INITIAL and body[1] in MEDIAL and body[2] in FINAL:
        return 4
    return 0


def syllable_rep(body, tone, kind):
    """回傳這個音節換成這種亂序後的鍵。不適用就 None。和原來一樣也 None。"""
    sh = shape(body)
    rep = None
    if kind == "o-swap-im" and sh in (1, 4):
        chars = list(body)
        chars[0], chars[1] = chars[1], chars[0]
        rep = "".join(chars) + tone
    elif kind == "o-swap-mf" and sh == 3:
        rep = body[1] + body[0] + tone
    elif kind == "o-swap-mf" and sh == 4:
        rep = body[0] + body[2] + body[1] + tone
    elif kind == "o-swap-if" and sh == 4:
        rep = body[2] + body[0] + body[1] + tone
    elif kind == "o-swap-if" and sh == 2:
        rep = body[1] + body[0] + tone
    elif kind == "o-reverse" and sh == 4:
        rep = body[2] + body[1] + body[0] + tone
    elif kind == "o-medial-first" and sh == 4:
        rep = body[1] + body[2] + body[0] + tone
    elif kind == "o-tone-front" and body:
        rep = tone + body
    elif kind == "o-tone-after1" and len(body) >= 2:
        rep = body[0] + tone + body[1:]
    elif kind == "o-tone-after2" and len(body) >= 3:
        rep = body[:2] + tone + body[2:]
    elif kind == "o-syl-swap":
        return None
    if rep is None or rep == body + tone:
        return None
    return rep


ORDER_KINDS = [
    "o-swap-im", "o-swap-mf", "o-swap-if", "o-reverse", "o-medial-first",
    "o-tone-front", "o-tone-after1", "o-tone-after2", "o-syl-swap",
]


def splice(keys, begin, end, rep):
    return keys[:begin] + rep + keys[end:]


def order_one(keys, rng):
    syls = split_syllables(keys)
    buckets = {k: [] for k in ORDER_KINDS}
    for body, tone, begin, end in syls:
        raw = body + tone
        for kind in ORDER_KINDS:
            if kind == "o-syl-swap":
                continue
            rep = syllable_rep(body, tone, kind)
            if rep:
                buckets[kind].append(splice(keys, begin, end, rep))
    for i in range(len(syls) - 1):
        a, b = syls[i], syls[i + 1]
        rep = keys[b[2]:b[3]] + keys[a[2]:a[3]]
        if rep != keys[a[2]:b[3]]:
            buckets["o-syl-swap"].append(splice(keys, a[2], b[3], rep))
    out = []
    for kind, alts in buckets.items():
        if alts:
            out.append((kind + ",o-one", alts[rng.randrange(len(alts))]))
    return out


def order_all(keys):
    syls = split_syllables(keys)
    out = []
    for kind in ORDER_KINDS:
        if kind == "o-syl-swap":
            continue
        parts, changed = [], False
        for body, tone, begin, end in syls:
            rep = syllable_rep(body, tone, kind)
            if rep:
                parts.append(rep)
                changed = True
            else:
                parts.append(body + tone)
        if changed:
            out.append((kind + ",o-all", "".join(parts)))
    return out


def rime_can_restore(typed, original):
    return original in NEI.get(typed, "")


def reverse_neighbors(original):
    return [typed for typed, neigh in NEI.items() if typed != original and original in neigh]


def vertical_mistypes(original):
    out = []
    for col in COLUMNS:
        if original not in col:
            continue
        i = col.index(original)
        if i:
            out.append(col[i - 1])
        if i + 1 < len(col):
            out.append(col[i + 1])
        break
    return [typed for typed in out if not rime_can_restore(typed, original)]


def role(ch):
    if ch in TONE:
        return "k-tone"
    if ch in MEDIAL:
        return "k-medial"
    if ch in FINAL:
        return "k-final"
    if ch in INITIAL:
        return "k-initial"
    return "k-other"


def typo_all(keys):
    found = {"k-neighbor": [], "k-vertical": [], "k-extra": [], "k-missing": []}
    for i, original in enumerate(keys):
        for typed in reverse_neighbors(original):
            found["k-neighbor"].append((keys[:i] + typed + keys[i + 1:], role(original)))
        for typed in vertical_mistypes(original):
            found["k-vertical"].append((keys[:i] + typed + keys[i + 1:], role(original)))
        if original != " ":
            found["k-extra"].append((keys[:i] + original + keys[i:], role(original)))
        if len(keys) > 2:
            found["k-missing"].append((keys[:i] + keys[i + 1:], role(original)))
    return found


def typo_one(keys, rng):
    out = []
    for kind, alts in typo_all(keys).items():
        if not alts:
            continue
        keys_m, which = alts[rng.randrange(len(alts))]
        out.append((kind + "," + which + ",k-one", keys_m))
    return out


def row(truth, tags, keys, orig, trap=""):
    shown = render(keys)
    return f"|{shown}||{truth}|{tags}|{keys}|{orig}|{trap}"


def load_select():
    items = []
    with open(SELECT, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("|")
            if len(parts) < 5:
                continue
            tags = parts[4]
            if "undecidable" in tags.split(",") or "llavon" in tags.split(","):
                continue
            zy = parts[1]
            keys = syllable_keys(zy)
            truth = parts[3]
            if len(keys) < 2 or not truth or "/" in zy:
                continue
            items.append((lineno, truth, tags, keys))
    return items


def hanzi_rows():
    sys.path.insert(0, HERE)
    import build_select_set as b

    phrases, single, poly, max_len = b.load_readings(b.DATA)
    rows, skipped = [], []
    seen = set()
    for gold, trap in HANZI:
        if gold == trap:
            skipped.append(f"相同 {gold}")
            continue
        g, _ = b.annotate(gold, phrases, single, poly, max_len)
        t, _ = b.annotate(trap, phrases, single, poly, max_len)
        if not g or not t:
            skipped.append(f"標不了 {gold} / {trap}")
            continue
        if g != t:
            skipped.append(f"讀音不同 {gold} {b.zhuyin(g)} / {trap} {b.zhuyin(t)}")
            continue
        keys = syllable_keys(b.zhuyin(g))
        if len(keys) < 2:
            skipped.append(f"鍵太短 {gold}")
            continue
        seen.add(gold)
        rows.append(row(gold, "h-homo,g-hanzi", keys, "", trap))
    if os.path.exists(CASES):
        with open(CASES, encoding="utf-8") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line or line.startswith("#"):
                    continue
                parts = line.split("|")
                if len(parts) < 4:
                    continue
                draft, truth, zy = parts[2], parts[3], parts[1]
                if not draft or draft == truth:
                    continue
                keys = syllable_keys(zy)
                if len(keys) < 2:
                    continue
                rows.append(row(truth, "h-legacy,g-hanzi", keys, "", draft))
    return rows, skipped


def main():
    assert syllable_keys("ㄏㄠˇ") == "cl3", syllable_keys("ㄏㄠˇ")
    assert syllable_keys("ㄒㄧㄝˋ ㄒㄧㄝˋ") == "vu,4vu,4"
    items = load_select()
    order_rows, typo_rows = [], []
    kind_sent = {}
    for index, (lineno, truth, tags, keys) in enumerate(items):
        rng = random.Random(20261010 + lineno)
        domain = ""
        for tag in tags.split(","):
            if tag and not tag.startswith("pair"):
                domain = "d-" + tag
                break
        base = ",".join(p for p in ("g-sent", domain) if p)
        for kind, mut in order_one(keys, rng) + order_all(keys):
            order_rows.append(row(truth, kind + "," + base, mut, keys))
            kind_sent[kind] = kind_sent.get(kind, 0) + 1
        for kind, mut in typo_one(keys, rng):
            typo_rows.append(row(truth, kind + "," + base, mut, keys))
            kind_sent[kind] = kind_sent.get(kind, 0) + 1
    grid_order = grid_typo = 0
    for text, zy in GRID:
        keys = syllable_keys(zy)
        if len(keys) < 2:
            raise SystemExit(f"格子標不了 {text} {zy}")
        syls = split_syllables(keys)
        seen_rep = set()
        for body, tone, begin, end in syls:
            for kind in ORDER_KINDS:
                if kind == "o-syl-swap":
                    continue
                rep = syllable_rep(body, tone, kind)
                if not rep:
                    continue
                mut = splice(keys, begin, end, rep)
                mark = (kind, mut)
                if mark in seen_rep:
                    continue
                seen_rep.add(mark)
                order_rows.append(row(text, kind + ",o-one,g-grid", mut, keys))
                grid_order += 1
        for i in range(len(syls) - 1):
            a, b = syls[i], syls[i + 1]
            rep = keys[b[2]:b[3]] + keys[a[2]:a[3]]
            mut = splice(keys, a[2], b[3], rep)
            order_rows.append(row(text, "o-syl-swap,o-one,g-grid", mut, keys))
            grid_order += 1
        for kind, alts in typo_all(keys).items():
            seen = set()
            for mut, which in alts:
                if mut in seen:
                    continue
                seen.add(mut)
                typo_rows.append(row(text, kind + "," + which + ",k-all,g-grid", mut, keys))
                grid_typo += 1
    hanzi, skipped = hanzi_rows()
    header_order = (
        "# 注音亂序。前文|亂序後的注音|初稿|正確|標籤|大千鍵|原鍵|\n"
        "# o-one 每句每種抽一處；o-all 是該句每個適用的音節都亂。g-grid 是形狀格，不抽樣。\n"
        "# o-tone-after1 是目前方案有的規則。o-tone-front、o-tone-after2、o-syl-swap 沒有。\n"
    )
    header_typo = (
        "# 打錯鍵。前文|打錯後的注音|初稿|正確|標籤|大千鍵|原鍵|\n"
        "# k-one 每句每種抽一處。g-grid 的 k-all 是該詞每一種打錯都列。\n"
        "# k-neighbor 在 Rime 的左右鄰鍵表裡。k-vertical 是直向，大多不在表裡。\n"
    )
    header_hanzi = (
        "# 同音錯字。鍵是正確句的鍵。最後一欄是寫明的錯字。\n"
        "# 讀音用 McBopomofo 核過，兩邊不一致的沒有收。\n"
    )
    for path, header, rows in (
        (OUT_ORDER, header_order, order_rows),
        (OUT_TYPO, header_typo, typo_rows),
        (OUT_HANZI, header_hanzi, hanzi),
    ):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(header)
            for line in rows:
                f.write(line + "\n")
    print(f"句子 {len(items)}")
    print(f"亂序 {len(order_rows)}（格子 {grid_order}）→ {OUT_ORDER}")
    print(f"打錯 {len(typo_rows)}（格子 {grid_typo}）→ {OUT_TYPO}")
    print(f"同音錯字 {len(hanzi)} → {OUT_HANZI}")
    print("句子覆蓋", dict(sorted(kind_sent.items())))
    if skipped:
        print("同音錯字未收")
        for line in skipped:
            print(" ", line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
