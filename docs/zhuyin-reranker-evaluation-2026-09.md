# 注音選字評估結果（2026-09）

依 `docs/zhuyin-reranker-roadmap-open-resources.md` §5 的近期工作清單，先做評估類項目，
結果用來決定接下來要實作什麼。這份文件只放統計數字，不含選字紀錄的內容。

工具全部在 `test/TuneChoice/`：

| 檔案 | 用途 |
|---|---|
| `TuneChoice.cpp` | 推薦的網格搜尋與校準（原有）；新增 `--rime-only`（P0.5）、`--topk-rerank`（LM 重排 Rime Top-K）、`--grammar`、`--max-sentences`、`--latency`、`--dump-topk`；指標新增 wrong-change、Top-K、MRR、延遲百分位 |
| `build_open_set.py` | 開放 zh-TW 測試集：zhwiki / CC-100 zh-Hant 的句子 → OpenCC `s2twp` → McBopomofo 詞級讀音。以固定種子重現，產物不進 repo |
| `teacher_pilot.py` | 用雲端模型（DeepSeek）排序 Rime 候選的試跑 |
| `convert_ckip_gpt2.py` | 把 CKIP 繁中 GPT-2（WordPiece 詞表）轉成 llama.cpp GGUF |

測試環境：GTX 1080 Ti（11 GB，其中約 7 GB 被執行中的輸入法模型占用），llama.cpp（安裝版的 WisdomLLMHost），librime 1.17.0。

---

## 1. librime 版本

安裝版與建置輸出的 `rime.dll` 都是 **librime 1.17.0**，已經有 `translator/max_sentences` 與 `sentence_cutoff_threshold`，
也內建 octagram 外掛。**不需要升級**，就能直接拿到 Top-K 整句。

## 2. 測試集

| 測試集 | 題數 | 說明 |
|---|---|---|
| 開放測試集 | 3,000（wiki 1,500、CC-100 1,500） | 4～12 字的子句，前文最多 30 字；分類 A（常見同音字）1,189 題、F0/F10/F30（前文長度）。台灣用語（D）只有 1 題，要另外補來源（例如立法院公報） |
| 選字紀錄 | 約 845 | 只在本機解密，只輸出統計 |
| 公開題目 `cases.txt` | 51 | 回歸測試用 |

讀音標註的限制：開放測試集約 14% 的題目有字不在該音節前 6 個同音字裡（表中的「讀音存疑」）。
原因可能是讀音標錯，也可能只是罕用字。絕對數字因此偏保守，但各設定之間的比較不受影響。

## 3. P0.5：Rime 自己能做到多少

開放測試集，Top-1 與 oracle@K（正確句子在 Rime 前 K 個整句裡的比例），格式為 wiki / CC-100：

| Rime 設定 | Top-1 | @3 | @5 | @10 |
|---|---|---|---|---|
| 無語言模型 | 52.9 / 59.2 | ＝Top-1 | | |
| **octagram `bgw`（詞級，目前使用）** | **56.9 / 63.6** | ＝Top-1 | | |
| octagram `bgc`（字級） | 51.5 / 59.1 | ＝Top-1 | | |
| **`bgw` 加 `max_sentences: 10`** | 56.9 / 63.6 | 67.0 / 74.1 | 69.0 / 76.1 | **71.7 / 77.8** |

- 詞級 `bgw` 比沒有語言模型好 4 個百分點；字級 `bgc` 反而較差。**維持 `bgw`。**
- 打開 `max_sentences` 後，**正確句子常常在前 10 名**，完美的重排可以把 Top-1 拉高約 14 個百分點。
- 選字紀錄：沙盒 Rime（沒有選字記憶）Top-1 是 83%，@10 是 90%；使用者實際的 Rime（有記憶）初稿是 94.6%。
- Rime 自己的延遲：P50 0.3～0.8 ms，P95 13～35 ms（含 octagram 與 Top-10）。

## 4. 評分模型比較：LM 重排 Rime Top-10

作法：用 LM 替 Rime 的每個整句打分數 `log P(句子 | 前文)`。最好的句子要比 Rime 第一句通順超過 margin 才換。
margin 用 2-fold 選（一半選、另一半報）。開放測試集取 1,500 題。

開放測試集（wiki / CC-100），Rime 初稿約 57.1～57.2 / 64.1～64.3（各次執行的可用題數略有不同）：

| 模型 | 授權 | 重排後 Top-1 | wrong-change | P95 延遲（逐句評分，未批次） |
|---|---|---|---|---|
| **Qwen3-0.6B-Base** Q8_0 | Apache-2.0 | **70.4 / 73.9** | 0.5% / 2.1% | 518 ms |
| gemma-3-1b-pt Q8_0 | Gemma Terms | 69.7 / 74.2 | **0.2% / 0.4%** | 1,370 ms |
| MiniCPM5-1B-Base Q8_0 | — | 69.2 / 71.7 | 0.2% / 1.3% | （同時跑了別的工作，不採計） |
| Qwen3-1.7B-Base Q8_0 | Apache-2.0 | 70.3 / 73.3 | 0.2% / 0.8% | 534 ms |
| MiniCPM5-2B-Base Q5_K_M | — | 70.0 / 72.5 | 1.0% / 2.3% | 440 ms |
| gemma-4-E2B-it Q4_K_M（instruct） | Apache-2.0 | 68.2 / 72.7 | 1.9% / 2.3% | 463 ms |
| CKIP gpt2-base-chinese（102M）f16 | GPL-3.0 | 64.4 / 68.7 | 0.7% / 1.9% | **115 ms** |
| CKIP gpt2-tiny-chinese（4M）f16 | GPL-3.0 | 59.4 / 65.3 | 0.2% / 0.6% | **38 ms** |

選字紀錄（沙盒 Rime Top-1 是 84.2%）：各模型重排後都在 80～84%，wrong-change 4～9%。
**在這份資料上，重排沒有超過沙盒 Rime，更遠低於使用者實際 Rime 的 94.6%。**

結論：

1. **在一般文字上，LM 重排 Rime Top-K 很有效：Top-1 +10～13 個百分點，而且 wrong-change 很低。**
   舊的「同音字逐位置替換」在同一個使用者的紀錄上一直贏不了 Rime。這次改成重排 Top-K，第一次在開放文字上看到明顯進步。
2. **模型大小不是關鍵**：0.6B 和 1.7B、2B 差不多。instruct 模型最差。base 模型裡，Qwen3-0.6B-Base 的正確率、授權與速度最平衡；
   gemma-3-1b-pt 最保守（wrong-change 最低），但詞表很大（約 262k），每個 token 都要做一次全詞表 softmax，所以特別慢。
3. **延遲的瓶頸是評分方式，不是模型**：小模型並沒有變快，P95 還是約 0.5 秒，因為每一句都重算前文、各自一次 IPC。
   **一定要先做 P2（ScoreBatch、前文 KV 重用、單次 batch decode），才能上線。**
4. **選字紀錄上看不到收益**：使用者的 Rime 有選字記憶，已經 94.6%。離線沙盒沒有記憶，無法模擬「有記憶的 Top-K 再重排」。
   上線前要用 shadow 模式驗證（原方案 §28.6）：記錄 Rime 第一句、重排後的第一句，以及使用者實際送出的句子，但不改使用者看到的排序。

### 4.1 CKIP

`convert_ckip_gpt2.py` 用 GPT-2 權重加 BERT 的 WordPiece（WPM）詞表，並關掉句尾的 `[SEP]`，轉換成功；llama.cpp 可以直接評分。

- 繁中原生、一字一 token、詞表約 21k，**逐句評分也快**：base 版的 P95 是 115 ms，大約是 0.6B 模型的四分之一。這也說明大模型慢，主要是全詞表 softmax 與模型本身，而不是 IPC。
- 正確率比 Qwen3-0.6B-Base 低（wiki +7、CC-100 +5 個百分點，對比 +13 / +10）。訓練語料只有維基百科與中央社，口語與網路文字較弱。
- 適合當 P7 專用 scorer 的起點（原方案 §20、roadmap P7：從繁中原生的小 GPT-2 繼續訓練，照 zenz 的做法）；tiny 版幾乎沒有收益，不建議。
- 授權是 GPL-3.0，和本專案相同；權重仍建議讓使用者另外下載。

## 5. Teacher 試跑：DeepSeek（`deepseek-v4-flash`，關閉思考）

開放測試集 500 題，只送公開語料。正確句子一定放進候選，每題排兩次、候選順序各自打亂：

| 指標 | 結果 |
|---|---|
| 第一名 = 正確句子 | **89.4%**（MRR 0.934） |
| Rime 第一句本來就對 / 不對 | 95.8% / 83.6% |
| 兩次的第一名相同 | 90.2% |
| 只留兩次一致的題目 | 正確率 **94.2%**，保留 90.2% |
| token | 1,000 次請求：輸入 26.8 萬、輸出 2.6 萬 |

- 關閉思考很重要：一開始沒關，每次請求輸出約 4,000 個 token；送 `"thinking": {"type": "disabled"}` 後降到約 23 個，正確率沒有下降。
- 「同一模型、打亂順序判斷兩次，一致才採用」可以取代多 teacher 投票：保留九成資料，正確率到 94%。
- 不一致的題目裡，有些本來就有兩種合理的答案（他/她、的/得），有些可能是語料的讀音標錯，正好可以拿來清理資料。
- 成本低，擴大到數萬題可行（P7 的資料管線）。

## 6. 對實作的建議（待決定）

依數據排序：

1. **P2：ScoreBatch 與前文 KV 重用**：必做。沒有它，重排 Top-10 的 P95 約 0.5 秒，無法上線。
2. **P3：Rime `max_sentences` 加 LM 重排**：開放文字上 +10～13 個百分點。先以 shadow 模式在使用者實機收集，確認在「有選字記憶的 Rime」上仍有收益，再決定是否顯示。
   預設 scorer 建議 **Qwen3-0.6B-Base**（Apache-2.0，約 610 MB）；需要更快或記憶體不夠時，用 **CKIP gpt2-base**（約 240 MB，P95 約四分之一）。
3. **舊的同音字替換推薦（`RescoreSentence`）**：維持現況（預設關閉加上校準擋掉），不再投入；由 P3 取代。
4. **P7 的資料管線可行**：開放語料、萌典或 McBopomofo 讀音、Rime Top-K 當 hard negative，再加 DeepSeek 排序與一致性過濾，工具都已驗證過。
5. **開放測試集要補台灣用語**（例如立法院公報），並處理約 14% 的讀音存疑題目（例如加上萌典讀音或 g2pW）。

## 7. 已實作：整句重排（可以開關）

依 §6 第 1、2 項實作，預設關閉：

| 設定（`weasel.yaml` 的 `llm/choice/*`；設定程式「選字策略」頁） | 說明 |
|---|---|
| `rerank: off / shadow / on` | **off** 關閉（預設）。**shadow**：只算不顯示，送出時統計「重排會改的次數、其中改對與改錯的次數」（選字統計的「整句重排」欄），並替校準累積樣本。**on**：比 Rime 第一句通順超過門檻的整句顯示為「推薦」（Tab 套用），取代同音字推薦 |
| `rerank_margin`（預設 2） | 整句 log 機率要好多少才推薦；之後再經信心校準（`rerank` 種類）決定顯示與否 |
| `rerank_sentences`（預設 10） | 最多評幾句 |
| `scorer/*`（選字策略頁的「評分模型」） | 評分專用的本機模型，建議 Qwen3-0.6B-Base；沒選時借用智慧預測或注音校正已載入的本機模型 |

- 設定程式會替三個注音方案寫入 `translator/max_sentences`，然後重新部署。整句數量是「長句的整句候選」選的數目；開啟整句重排時至少 10 句。
  另外會把 `translator/sentence_cutoff_threshold` 放寬到 100。Rime 預設會濾掉比最好的一句低很多的整句，
  在 `bopomofo_express` 上常常只剩一句，幾乎沒有東西可以重排。
- 「長句的整句候選」也可以單獨開（3 / 5 / 10 句）。Rime 預設只有第一個候選是整句，第二名起是句首的詞，所以長句的後面幾個候選看起來很短。
  開啟後，前幾名是完整的整句，句首的詞排在後面。
- 整句候選來自使用者自己的 Rime（背景 session，同一份選字記憶），第一句一定是使用者看到的轉換。
  這補上了 §4 第 4 點離線沙盒做不到的部分，所以 shadow 模式的統計才是有沒有收益的真正答案。
- **fail-open**：評分失敗、模型不支援批次評分、推理行程是舊版（不認得批次評分的請求），都退回逐句評分或不推薦。
  Rime 的候選一律立即顯示，重排的結果晚到才刷新；繼續打字時，舊的推理會被取消。

批次評分（`ScoreBatch`）：前文只解碼一次，用 `llama_memory_seq_cp` 分給每個候選，所有候選一次 `llama_decode`。
Qwen3-0.6B-Base、開放測試集 500 題、一題 10 句的實測：

| 評分方式 | P50 | P95 | Top-1 |
|---|---|---|---|
| 逐句 | 54～61 ms | 344～385 ms | 69.5% |
| 批次 | 32～53 ms | 105～267 ms | 69.1% |

- 範圍取自不同時間的多次測量。這台機器的 GPU 同時跑著輸入法自己的模型，負載不同，延遲就差很多。
- 批次在較空閒時快 3.3 倍，負載高時快 1.4 倍，還沒達到原方案 P95 80 ms 的目標。
- 詞表小的 CKIP gpt2-base 也要約 140 ms，可見剩下的主要是每次請求的固定成本（IPC、GPU 啟動），以及和輸入法模型搶 GPU。
- 後續可以做：常駐的前文 KV 快取（打字時前文不變）、只對 Top-5 評分、評分模型和預測模型共用同一個推理行程。

## 8. 重現

```bat
rem 開放測試集（需要 opencc、pyarrow；資料見 build_open_set.py 的說明）
python test\TuneChoice\build_open_set.py --mcbpmf <McBopomofo Source\Data> --wiki <zhwiki parquet> --cc100 <CC-100 zh-Hant 開頭> --out open_set.txt

rem P0.5：Rime 各種設定
test\TuneChoice\run.bat - --rime-only --cases open_set.txt --grammar %APPDATA%\Rime\zh-hant-t-essay-bgw.gram --max-sentences 10

rem 模型比較：LM 重排 Top-10
test\TuneChoice\run.bat <model.gguf> --base --topk-rerank --grammar %APPDATA%\Rime\zh-hant-t-essay-bgw.gram --max-sentences 10 --cases open_set.txt

rem Teacher 試跑
test\TuneChoice\run.bat - --rime-only --cases open_set.txt --grammar ... --max-sentences 10 --dump-topk open_topk.tsv
python test\TuneChoice\teacher_pilot.py --topk open_topk.tsv --rime-config %APPDATA%\Rime\weasel.custom.yaml --n 500
```
