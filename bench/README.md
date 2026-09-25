# LLM 預測模型測試

用和 Wisdom-Weasel `llamacpp` provider 相同的方式跑模型，比較不同模型當輸入法「下一個詞預測」的效果。

## 怎麼跑

```bat
run_bench.bat                                   :: 跑 models.json 裡所有模型
run_bench.bat --models qwen3-0.6b-base          :: 只跑指定模型
run_bench.bat --temp 0.6 --samples 5 --tokens 4 :: 調整取樣參數
```

結果放在 `results\<時間>\`：
- `summary.md`：總表與各類別命中率
- `summary.csv`：同上，方便用 Excel 比較
- `<模型>.jsonl`：每一題的原始輸出、清洗後候選、是否命中

## 加新模型

在 `models.json` 加一行：

```json
{"name": "自訂名稱", "path": "C:/Users/zex55/models/xxx.gguf", "mode": "base"}
```

`mode` 是 `base`（直接續寫上下文，建議）或 `instruct`（用 Wisdom-Weasel 的指令 prompt）。

## 加測試題

在 `testset.jsonl` 加一行：

```json
{"id": "daily-09", "category": "日常", "context": "上下文", "expected": ["可接受的下一個詞", "..."]}
```

`expected` 盡量多列幾個合理答案。

## 怎麼模擬輸入法

- 每題取樣 5 次、每次 4 個 token（與 `LlamaCppProvider::GenerateCandidatesBatch(..., 5, 4)` 相同）。
- sampler：top_k 40、top_p 0.95、min_p 0.05、repeat_penalty 1.1，temperature 預設 0.8（與目前 `weasel.custom.yaml` 相同）。
- 候選清洗與 WeaselServer 相同：去空白、只取第一行、去控制字元、去首尾引號與標點、去重。
- 目前只測「選字送出後預測下一個詞」（沒有正在輸入的注音），這是最常觸發的情境。

## 指標

| 指標 | 意思 |
|---|---|
| 命中@1 | 第一個候選符合預期（簡體會先轉臺灣繁體再比對） |
| 命中@5 | 任一個候選符合預期 |
| 平均不重複候選 | 清洗、去重後剩幾個候選（滿分 5） |
| 原始輸出雜訊 | 原始輸出含引號、換行或完全沒有中文的比例 |
| 簡體比例 | 候選中含簡體字的比例 |
| 每次取樣 ms | 單次取樣推論時間（輸入法實際是 5 路平行） |

「符合」的定義：候選以某個預期詞開頭，或候選是預期詞的前綴且至少 2 個字（預期詞只有 1 個字時 1 個字即可）。
