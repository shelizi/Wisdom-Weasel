"""Benchmark LLM next-word prediction the way Wisdom-Weasel's llamacpp provider does it.

For each model in models.json a llama-server is started, and for every test case the
model is sampled N times (default 5 candidates x 4 new tokens, same sampler settings as
LlamaCppProvider). Candidates go through the same cleanup as WeaselServer and are scored
against the acceptable next words in testset.jsonl.

Usage:
  python bench.py                         # all models in models.json
  python bench.py --models qwen3-0.6b-base lfm2.5-230m-base
  python bench.py --temp 0.6 --samples 5 --tokens 4
"""
import argparse
import csv
import json
import statistics
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

import opencc
import requests

HERE = Path(__file__).resolve().parent
DEFAULT_SERVER = Path(r"C:\Users\zex55\src\llama-b11177\bin\llama-server.exe")

# Same prompt as LlamaCppProvider::PredictCandidates (Instruct branch); Base uses raw context.
INSTRUCT_SYSTEM = (
    "你是一个智能中文输入法，请根据以下上下文和当前输入，预测接下来最可能出现的{n}个候选词。\n\n"
    "要求：\n"
    "1. 只返回候选词，不要任何解释或标点\n"
    "2. 候选词之间用单个空格分隔\n"
    "3. 按可能性从高到低排列\n"
    "4. 如果上下文为空或无关，仅基于当前输入预测\n"
    "5. 确保候选词都是有效的中文词汇或常用短语\n"
    "6. 返回词数严格不超过{n}个\n\n"
)
INSTRUCT_USER = '上下文："{context}"\n当前输入："{current}"\n候选词：'

# Same trim set as RimeWithWeaselHandler (post-prediction cleanup).
TRIM_CHARS = (" \t\u3000\"'`\u201c\u201d\u2018\u2019\u300c\u300d\u300e\u300f"
              "()[]{}<>\uff08\uff09\u3010\u3011\u300a\u300b"
              ",.;:!?\uff0c\u3002\u3001\uff1b\uff1a\uff01\uff1f\u2026")

S2T = opencc.OpenCC("s2tw")  # 臺灣標準字形；s2t 會把「床、台、吃」等臺灣正常用字誤判為需轉換


def build_prompt(mode, context, n, prefix=""):
    if mode == "instruct":
        return INSTRUCT_SYSTEM.format(n=n) + INSTRUCT_USER.format(context=context, current="")
    # Base：可選的引導文字（例如要求繁體）接在前文之前
    return prefix + context


def clean(raw_list):
    """WeaselServer pipeline: remove spaces + trailing U+FFFD (provider), then first line,
    drop control chars, trim quotes/punctuation, dedupe (handler)."""
    out = []
    for raw in raw_list:
        s = raw.replace(" ", "").rstrip("\ufffd")
        s = s.replace("\r", "\n").split("\n", 1)[0]
        s = "".join(c for c in s if ord(c) >= 0x20 and c != "\x7f")
        s = s.strip(TRIM_CHARS)
        if s and s not in out:
            out.append(s)
    return out


def is_hit(cand, expected):
    """A candidate hits if it starts with an expected word, or is a >=2-char (or whole-word)
    prefix of one. Candidates are compared after s2t so simplified output still counts."""
    c = S2T.convert(cand)
    for e in expected:
        if c.startswith(e):
            return True
        if e.startswith(c) and len(c) >= min(2, len(e)):
            return True
    return False


def has_simplified(text):
    return S2T.convert(text) != text


def is_garbage(raw):
    """Raw output that the old WeaselServer would have shown verbatim: quotes, newlines,
    or nothing CJK at all."""
    stripped = raw.strip()
    if not stripped:
        return True
    if any(ch in raw for ch in "\n\r\"\u201c\u201d\u300c\u300d"):
        return True
    return not any("\u4e00" <= ch <= "\u9fff" for ch in raw)


class LlamaServer:
    def __init__(self, exe, model, port, ngl, ctx):
        self.url = f"http://127.0.0.1:{port}"
        self.log = open(HERE / "results" / f"server-{port}.log", "w", encoding="utf-8")
        self.proc = subprocess.Popen(
            [str(exe), "-m", model, "--port", str(port), "-ngl", str(ngl), "-c", str(ctx),
             "-np", "1", "--no-webui"],
            stdout=self.log, stderr=subprocess.STDOUT)

    def wait_ready(self, timeout=180):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.proc.poll() is not None:
                raise RuntimeError(f"llama-server exited with {self.proc.returncode}, see {self.log.name}")
            try:
                if requests.get(self.url + "/health", timeout=2).status_code == 200:
                    return
            except requests.RequestException:
                pass
            time.sleep(0.5)
        raise TimeoutError("llama-server did not become ready")

    def complete(self, prompt, n_predict, temp, seed):
        body = {
            "prompt": prompt, "n_predict": n_predict, "temperature": temp,
            "top_k": 40, "top_p": 0.95, "min_p": 0.05, "typical_p": 1.0,
            "repeat_penalty": 1.1, "seed": seed, "cache_prompt": True,
        }
        r = requests.post(self.url + "/completion", json=body, timeout=60)
        r.raise_for_status()
        j = r.json()
        t = j.get("timings", {})
        return j["content"], t.get("prompt_ms", 0.0) + t.get("predicted_ms", 0.0)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()


SEPARATORS = set(" \t\n\r，。、；：？！…—–（）【】《》,.;:?!-_")


def build_context(cases, idx, history, fmt, max_chars):
    """模擬輸入法組給模型的前文。

    history>0 時在題目前面塞入其他題目的完整句子（不同主題），模擬所有程式共用一份
    上下文造成的汙染。fmt="spaced" 模擬原本 ContextHistory 的格式（依標點切詞、去標點、
    以空格連接）；"natural" 保留原文。max_chars>0 時只保留最後 N 個字。"""
    n = len(cases)
    parts = []
    for j in range(1, history + 1):
        other = cases[(idx + 7 * j) % n]
        parts.append(other["context"] + other["expected"][0] + "。")
    text = "".join(parts) + cases[idx]["context"]
    if fmt == "spaced":
        words, cur = [], ""
        for ch in text:
            if ch in SEPARATORS:
                if cur:
                    words.append(cur)
                cur = ""
            else:
                cur += ch
        if cur:
            words.append(cur)
        text = " ".join(words)
    if max_chars > 0:
        text = text[-max_chars:]
    return text


def run_model(m, cases, args, out_dir):
    srv = LlamaServer(args.server, m["path"], args.port, args.ngl, args.ctx)
    rows = []
    try:
        srv.wait_ready()
        for idx, case in enumerate(cases):
            context = build_context(cases, idx, args.history, args.format, args.max_chars)
            prompt = build_prompt(m["mode"], context, args.samples, args.prefix)
            raws, ms = [], []
            for i in range(args.samples):
                text, t = srv.complete(prompt, args.tokens, args.temp, args.seed + i)
                raws.append(text)
                ms.append(t)
            cands = clean(raws)
            rows.append({
                "id": case["id"], "category": case["category"], "context": case["context"],
                "model_context": context,
                "expected": case["expected"], "raw": raws, "candidates": cands,
                "hit1": bool(cands) and is_hit(cands[0], case["expected"]),
                "hit5": any(is_hit(c, case["expected"]) for c in cands),
                "unique": len(cands),
                "garbage": sum(is_garbage(r) for r in raws) / len(raws),
                "simplified": (sum(has_simplified(c) for c in cands) / len(cands)) if cands else 0.0,
                "ms": sum(ms) / len(ms),
            })
            mark = "O" if rows[-1]["hit5"] else "x"
            print(f"  [{mark}] {case['context']} -> {' / '.join(cands) or '(空)'}", flush=True)
    finally:
        srv.stop()
    with open(out_dir / f"{m['name']}.jsonl", "w", encoding="utf-8") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    return rows


def summarize(name, mode, rows):
    n = len(rows)
    return {
        "model": name, "mode": mode, "cases": n,
        "hit@1": sum(r["hit1"] for r in rows) / n,
        "hit@5": sum(r["hit5"] for r in rows) / n,
        "unique_cands": statistics.mean(r["unique"] for r in rows),
        "garbage_raw": statistics.mean(r["garbage"] for r in rows),
        "simplified": statistics.mean(r["simplified"] for r in rows),
        "ms_per_sample": statistics.mean(r["ms"] for r in rows),
        "by_category": {
            cat: sum(r["hit5"] for r in rows if r["category"] == cat) / sum(1 for r in rows if r["category"] == cat)
            for cat in dict.fromkeys(r["category"] for r in rows)
        },
    }


def write_report(summaries, out_dir, args):
    cols = ["model", "mode", "hit@1", "hit@5", "unique_cands", "garbage_raw", "simplified", "ms_per_sample"]
    with open(out_dir / "summary.csv", "w", encoding="utf-8-sig", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for s in summaries:
            w.writerow({k: s[k] for k in cols})

    pct = lambda v: f"{v * 100:.0f}%"
    cats = list(summaries[0]["by_category"]) if summaries else []
    lines = [
        f"# LLM 預測測試報告 ({datetime.now():%Y-%m-%d %H:%M})",
        "",
        f"設定：每題取樣 {args.samples} 次 × {args.tokens} tokens，temperature {args.temp}，"
        f"seed {args.seed}，與 Wisdom-Weasel llamacpp 相同的 sampler 與候選清洗流程。",
        "",
        f"Base 引導文字：{args.prefix!r}" if args.prefix else "Base 引導文字：（無）",
        "",
        f"前文：汙染句數 {args.history}、格式 {args.format}、"
        f"截斷 {args.max_chars if args.max_chars else '無'}",
        "",
        "| 模型 | 模式 | 命中@1 | 命中@5 | 平均不重複候選 | 原始輸出雜訊 | 簡體比例 | 每次取樣 ms |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for s in sorted(summaries, key=lambda s: -s["hit@5"]):
        lines.append(f"| {s['model']} | {s['mode']} | {pct(s['hit@1'])} | {pct(s['hit@5'])} | "
                     f"{s['unique_cands']:.1f} | {pct(s['garbage_raw'])} | {pct(s['simplified'])} | "
                     f"{s['ms_per_sample']:.0f} |")
    lines += ["", "## 各類別命中@5", "", "| 模型 | " + " | ".join(cats) + " |",
              "|---|" + "---|" * len(cats)]
    for s in sorted(summaries, key=lambda s: -s["hit@5"]):
        lines.append(f"| {s['model']} | " + " | ".join(pct(s["by_category"][c]) for c in cats) + " |")
    lines += ["", "## 指標說明", "",
              "- **命中@1 / 命中@5**：第一個 / 任一個候選符合預期的下一個詞（簡體會先轉臺灣繁體再比對）。",
              "- **平均不重複候選**：清洗、去重後剩幾個候選（滿分 = 取樣次數）。",
              "- **原始輸出雜訊**：模型原始輸出含引號、換行或完全沒有中文的比例（越低越好）。",
              "- **簡體比例**：候選中含簡體字的比例（臺灣使用者越低越好）。",
              "- **每次取樣 ms**：單次取樣的推論時間；輸入法實際是批次平行取樣。",
              "", "逐題結果見同資料夾的 `<模型>.jsonl`。"]
    (out_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--models", nargs="*", help="model names from models.json (default: all)")
    ap.add_argument("--models-file", default=HERE / "models.json", type=Path)
    ap.add_argument("--testset", default=HERE / "testset.jsonl", type=Path)
    ap.add_argument("--server", default=DEFAULT_SERVER, type=Path)
    ap.add_argument("--samples", type=int, default=5, help="candidates per case (WW uses 5)")
    ap.add_argument("--tokens", type=int, default=4, help="new tokens per candidate (WW uses 4)")
    ap.add_argument("--temp", type=float, default=0.8)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--ngl", type=int, default=99)
    ap.add_argument("--ctx", type=int, default=2048)
    ap.add_argument("--port", type=int, default=18080)
    ap.add_argument("--prefix", default="", help="Base 模式接在前文前的引導文字（\\n 代表換行）")
    ap.add_argument("--tag", default="", help="這次測試的名稱，會加在結果資料夾名稱後面")
    ap.add_argument("--history", type=int, default=0,
                    help="在題目前塞入幾句其他主題的句子，模擬上下文汙染（預設 0 = 乾淨）")
    ap.add_argument("--format", choices=["natural", "spaced"], default="natural",
                    help="前文格式：natural 保留標點；spaced 模擬原本的空格拼接")
    ap.add_argument("--max-chars", type=int, default=0, help="前文只保留最後 N 個字（0 = 不截斷）")
    args = ap.parse_args()
    args.prefix = args.prefix.replace("\\n", "\n")

    models = json.loads(args.models_file.read_text(encoding="utf-8"))
    if args.models:
        models = [m for m in models if m["name"] in args.models]
    models = [m for m in models if Path(m["path"]).exists() or print(f"skip {m['name']}: file not found")]
    cases = [json.loads(l) for l in args.testset.read_text(encoding="utf-8").splitlines() if l.strip()]

    out_dir = HERE / "results" / (datetime.now().strftime("%Y%m%d-%H%M%S") + (f"-{args.tag}" if args.tag else ""))
    out_dir.mkdir(parents=True, exist_ok=True)
    summaries = []
    for m in models:
        print(f"\n=== {m['name']} ({m['mode']}) ===", flush=True)
        rows = run_model(m, cases, args, out_dir)
        summaries.append(summarize(m["name"], m["mode"], rows))
    write_report(summaries, out_dir, args)
    print("\n" + (out_dir / "summary.md").read_text(encoding="utf-8"))
    print(f"報告：{out_dir / 'summary.md'}")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
