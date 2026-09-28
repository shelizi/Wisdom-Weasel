# 注音情境選字：依實測排序、以開源資源落地的後續規劃

本文件接續 `docs/zhuyin-contextual-reranker-optimization-plan.md`（下稱「原方案」）。原方案的架構與原則不變：
Rime 產生符合注音的候選，小模型依上下文重排，個人化只改排序，全部 fail-open，所有改動都要過 benchmark。

本文件補兩件事：

1. **用已經量到的數據重排優先順序**：先做最可能有收益、成本最低的事。
2. **每一階段先找現成的開源模型、資料、工具與前人做法**，只有找不到才自己做。

資料查核日期為 2026-09-29。標「未查證」的項目，實作前要再確認。

---

## 1. 已經量到的事實

來源是 `test/TuneChoice`：用本機模型跑使用者自己的選字紀錄，只輸出統計數字，並做 5-fold 交叉驗證。

| 項目 | 結果 |
|---|---|
| 可用題數（選字紀錄） | 663 |
| Rime 初稿（含 octagram `zh-hant-t-essay-bgw` 與選字記憶）第一候選整句正確率 | **96.1%** |
| 初稿有錯、而且換同音字改得對的題目 | 24 題（3.6%） |
| 同音字替換的上限（oracle） | 99.7% |
| 舊推薦（greedy，margin 0.5）全顯示 | 78～81%：gemma-4-E2B-it 與 MiniCPM5-2B-Base 都一樣，大多是把對的改錯 |
| 最佳參數加校準 | 96.1%，也就是等於不推薦：兩個 2B 模型都贏不了初稿 |
| 校準（ECE） | 0.01～0.02。10 筆樣本就能把錯誤推薦擋掉大半（93% 對 84%） |
| 公開題目（51 題：33 題同音錯字、18 題本來就對） | 初稿 37%，推薦後 82～84%（margin 4）。MiniCPM **base** 優於 gemma **instruct** |
| `ScoreText` 延遲 | 約 80 ms/次。原因：每次都清掉 KV、重算整段前文，候選一個一個評 |

由此得到的判斷：

- **瓶頸不是模型大小，而是「亂改」（wrong-change）與延遲。** 在真實打字上，Rime 加開源 n-gram 已經很強。神經模型必須先證明「會改的時候幾乎都改對」，才有資格介入。
- **原方案 §4 的 wrong-change rate 應該列為第一級 gate**，不只是一個「不得明顯增加」的附帶條件。
- **單一使用者的紀錄有偏差。** 使用者本來就會順著 Rime 打，錯誤率低。所以需要一份開放、可重現的 zh-TW 測試集，才能看出模型在一般文字上的真實能力（見 P0）。
- **原方案 §25.8「何時介入」的 contextual bandit，其實第一版已經有了**：commit `4836534` 的信心校準就是「顯示／不顯示」兩個 action 的 logistic policy。接下來是擴充特徵，不是從零開始。

---

## 2. 開源資源總表（依用途）

### 2.1 已在用、可以直接擴充

| 資源 | 授權 | 用法 |
|---|---|---|
| librime（Rime） | BSD-3 | 候選產生。**1.17.0（2026-06）新增 `translator/max_sentences`（1～100）與 `sentence_cutoff_threshold`**，可直接取得 Top-K 整句（beam search 產生）。目前綁的是 commit `33e7814`，要先確認版本。 |
| librime-octagram 與 lotem/rime-octagram-data `hant` | BSD-3 / LGPL-3.0 | 已用 `zh-hant-t-essay-bgw.gram`（詞級，41 MB）。同一分支還有字級的 `-bgc`（10 MB）。 |
| llama.cpp | MIT | 已用。可用的 API 見 §4 P2。 |

### 2.2 評分模型候選（base，不用 instruct）

| 模型 | 大小 | 授權 | llama.cpp | 備註 |
|---|---|---|---|---|
| CKIP gpt2-tiny-chinese | 4M | GPL-3.0 | `gpt2` 架構支援；沒有現成 GGUF，WordPiece 詞表可能要改轉換器（未查證） | 用繁中訓練（zhwiki 經 OpenCC、中央社 Gigaword），**一字一 token** |
| CKIP gpt2-base-chinese | 102M | GPL-3.0 | 同上 | PPL 8.36。專案本身是 GPL-3.0，授權相容；用獨立下載的方式散布 |
| Qwen3-0.6B-Base / 1.7B-Base | 0.6B / 1.7B | Apache-2.0 | 有社群 GGUF，一般注意力架構 | KV 相關技巧都能用；zh-TW 品質未查證 |
| Llama-3.2-Taiwan-1B（lianghsun） | 1B | Llama 3.2 | 要自己轉 GGUF | 繁中持續預訓練，是目前最貼近 zh-TW 的小 base |
| Gemma 3 270M / 1B pt | 270M / 1B | Gemma Terms | ggml-org 有 GGUF | 多語 |
| gemma-4-E2B（base） | 約 2B 有效參數 | Apache-2.0（未查證） | 要自己轉 | 已測過的是 -it 版，base 值得補測 |
| Qwen3.5-0.8B-Base | 0.8B | Apache-2.0 | `qwen35` | **混合 DeltaNet：遞迴狀態不能部分回退，`seq_rm` 會失敗，前綴重用受限**，排在後面 |

### 2.3 前人做法（照抄，不要重想）

| 做法 | 可以借用的地方 |
|---|---|
| **azooKey Zenzai / zenz**（日文假名轉漢字，CC-BY-SA-4.0） | 26M～310M 的字級 GPT-2，專為轉換任務訓練，Acc@1 可到 66～87%（Google 日文輸入是 54%），M2 Pro 每字 4～7 ms。重點有兩個：輸入格式是「左文 + 讀音 + 輸出」，而且用傳統轉換器的結果當 draft，讓 LM 只負責驗證或修正（speculative），呼叫次數有上限。**和原方案「Rime 產生、LM 重排」完全一致，是 P7 的藍本。** |
| **PinyinGPT**（ACL 2022） | 字級 GPT 加上拼音約束解碼。訓練時 softmax 只在同音字集合上正規化，正好就是重排任務。用 beam search，寬度 16。 |
| **McBopomofo 小麥注音**（MIT） | Gramambular2：在讀音格上用 unigram 走 DAG 最短路徑（Viterbi）。另有 user override 層、`phrase.occ` 次數轉 log 機率的流程，以及 heterophony 破音字優先表。 |
| **vChewing 威注音**（Megrez 是 LGPL-3.0，詞庫是 MulanPSL-2.0） | 同類的 DAG-DP 走法；詞頻來自國家教育研究院。 |
| **libchewing 新酷音**（LGPL-2.1，已移到 Codeberg） | 詞頻 DP。 |

### 2.4 資料

| 資料 | 授權 | 用途 |
|---|---|---|
| zhwiki dump（2026-09-01，3.4 GB） | CC BY-SA 4.0 | 經 OpenCC `s2twp` 轉換後當句子來源。可以訓練；衍生文字要相同方式分享 |
| CC-100 zh-Hant（5.3 GB） | 無 IP 主張 | 有明確的繁中分割，句子來源 |
| 立法院公報、g0v ly API | 可自由再利用（含改作），需標示 | 口語 zh-TW，句子來源 |
| FineWeb-2 `cmn_Hani` | ODC-By | 繁簡混合，要自己依字形過濾 |
| 教育部重編國語辭典、g0v 萌典 moedict-data | CC BY-ND 3.0 TW | **台灣讀音的標準答案**（注音） |
| McBopomofo `BPMFMappings`、heterophony | MIT | 詞級讀音與破音字優先順序 |
| 臺灣主權 AI 訓練語料庫（moda，超過 15 億 token） | CC BY-ND 加專用條款 | 要申請；**只能拿來訓練，不能散布修改後的文字** |
| SIGHAN 2013/14/15 CSC | 研究用（未查證） | 原生繁中錯字，當外部檢查 |
| Taiwan-Text-Excellence-2B、TaiwanChat 等 | **NC**（非商業） | 只做研究；不要進會散布的訓練集 |

G2P 工具：g2pW（Apache-2.0，BERT 破音字消歧，輸出注音）用 CPP 資料集訓練，讀音偏大陸；pypinyin 的 BOPOMOFO 也是大陸讀音。**zh-TW 讀音以萌典或 McBopomofo 詞級讀音為準，g2pW 只處理剩下查不到的破音字。**

### 2.5 訓練與部署工具

- **HF TRL**（Apache-2.0）：`RewardTrainer`（pairwise）與 `DPOTrainer`。
- **PEFT**（Apache-2.0）：LoRA。
- **sentence-transformers**（Apache-2.0）：`CrossEncoder` 與 pairwise/listwise loss。
- **llama.cpp**：`convert_lora_to_gguf.py`。
- **llm.c**：zenz 用它訓練 GPT-2。
- **KenLM**（LGPL-2.1）：每次查詢不到 1 µs，拿來做 n-gram 對照組。

---

## 3. 授權注意

- 專案是 **GPL-3.0**。GPL、LGPL、Apache、MIT、BSD 的模型和資料都可以用；模型權重建議讓使用者另外下載，不要打包進安裝檔。
- **Teacher 用 DeepSeek API**（使用者已經在用，設定裡有 `deepseek-v4-flash`）。[DeepSeek Open Platform Terms of Service](https://cdn.deepseek.com/policies/en-US/deepseek-open-platform-terms-of-service.html)（2026-04-29 版）把輸出的權利讓渡給使用者，並明文允許「training other models (such as model distillation)」；限制是不得在品牌或行銷上暗示和 DeepSeek 有關係。條款沒寫明會不會拿 API 輸入去訓練，所以**只送公開語料與 Rime 對它產生的候選**，選字紀錄與打字內容一律不送。
- 其他閉源 API（OpenAI、Anthropic、Google 等）的條款大多禁止用輸出開發競爭模型：只拿來抽查或評估，不產生訓練資料。
- **Llama 3.2 條款**：用它的輸出訓練並散布模型時，模型名稱必須以「Llama」開頭，而且要標示「Built with Llama」。所以 Llama 系列不當 teacher。要本機或開放權重的 teacher 時，用 Qwen3.5（Apache-2.0）。
- **ND 授權**（萌典、moda）：讀音可以拿來當標註，但不要把修改後的辭典文字放進 repo。
- **NC 授權**的資料集不要混進要散布的模型。
- **使用者的選字紀錄永遠只在本機使用。** 公開題目與開放測試集只能用上表的來源。

---

## 4. 修訂後的路線

每一階段寫明：目標、採用的開源資源、要做的事，以及 gate（未通過就不往下）。
階段編號沿用原方案，順序依 §1 的數據調整。

### P0 開放 benchmark（原方案 §4、§15、§16）

現況：`test/TuneChoice` 已經能用選字紀錄與公開題目評估推薦與校正，也支援網格搜尋與交叉驗證。

要做：

1. **開放 zh-TW 測試集產生器**（`test/TuneChoice/build_open_set.py`）：
   - 句子來源：zhwiki（OpenCC `s2twp`）、立法院公報、CC-100 zh-Hant，依原方案 §15 的類別 A～I 抽樣並分層。
   - 讀音：萌典或 McBopomofo 詞級讀音，查不到再用 g2pW。
   - 把注音換成大千按鍵，**真的打進 Rime**，取得真實的初稿與 Top-K。不要用 LLM 合成初稿（原方案 §25.1：ground truth、注音、候選、teacher 要分工）。
   - 產生的測試集不放進 repo（CC BY-SA 會連帶要求相同方式分享），只放產生腳本與固定的隨機種子，任何人都能重現。
2. **TuneChoice 補指標**：wrong-change rate、no-change accuracy、Top-3/Top-5、MRR、P50/P95/P99 延遲（原方案 §4.2）。
3. **SIGHAN 13～15 當外部檢查**：把錯字句轉成注音，看模型能不能改回正確的句子。
4. **選字紀錄補記「顯示了哪些候選」**：Rime 前幾名與推薦、校正的內容，讓之後能組 strong preference pair（原方案 §25.7、§29）。只在本機加密存放，沿用現在的 `choice_log`。

Gate：開放測試集至少 2,000 題，各類別至少 100 題；TuneChoice 能一次產出原方案 §16 的比較矩陣。

### P0.5 先量 Rime 自己能做到多少（新增，成本最低）

1. 確認綁的 librime 版本。若低於 1.17，就升級並打開 `translator/max_sentences`。
2. 在 P0 測試集上量：
   - 只有 Rime、Rime 加 octagram `bgw`（詞級）、加 `bgc`（字級）、加 RIME-LMDG zh-hant（CC-BY-4.0，以簡體語料為主，當對照）。
   - **oracle@K**：正確句子出現在 Rime Top-K 整句裡的比例。
3. 判斷：oracle@K 比 Top-1 高出很多，才值得做 LM 重排（P1、P3）；差距很小，重點就應該放在個人化（P4）與「不亂改」。

Gate：拿到 `Top-1`、`oracle@{3,5,10}` 與延遲；決定 P1、P3 的投入。

### P2（提前）ScoreBatch 與 KV 前綴重用（原方案 §6）

放在 beam 前面做，因為 beam、Top-K、模型比較都要先有便宜的評分，而且 §1 已確認 80 ms 主要來自重算。

用 llama.cpp 現有的 API，不需要自己寫 kernel：

1. 前文只 decode 一次（seq 0）。
2. 用 `llama_memory_seq_cp` 把前文複製到 N 條序列。
3. 所有候選的 token 放進**同一個** `llama_decode` batch（用 `llama_batch.seq_id` 區分，只對候選的位置開 `logits`），再用 `llama_get_logits_ith` 取出每個位置的 logits。
4. 前文沒變時保留 seq 0（request-level cache），只清候選的序列。

程式已經設好 `n_seq_max = 64` 與 `kv_unified = true`，可以直接用。`LLMProvider::ScoreBatch` 先把 API 固定下來（原方案 §6.1），IPC 一次傳整批。

排除遞迴架構（Qwen3.5 的 `seq_rm` 會失敗），或讓它退回逐筆評分。

Gate：同一批 8 個候選的評分延遲，P95 低於 30 ms（從約 80 ms × N 降下來）；TuneChoice 分數不變。

### P2.5 評分模型比較（原方案 §14）

P2 完成後，在 P0 測試集與選字紀錄上比較 §2.2 的候選（重點是 base 模型與繁中原生的小模型）：

- 依序測：CKIP gpt2-tiny、CKIP gpt2-base、Qwen3-0.6B-Base、Llama-3.2-Taiwan-1B、Gemma-3-270M/1B pt、gemma-4-E2B base，並以現有的 MiniCPM5-2B-Base 當基準。
- 另外記錄：繁中每 token 幾個字（用 `llama-tokenize` 跑選字紀錄）、RAM/VRAM、載入時間。
- CKIP 要先驗證 GGUF 轉換。若轉換器不支援 WordPiece，改由 host 自己切 token 後餵 token id。

Gate：選出一個在「wrong-change 不增加」的前提下 Top-1 最高、而且 P95 最低的模型，當預設的 scorer（原方案 §14 的 Model A）。

### P1 greedy → beam（原方案 §5）

保留，但依 P0.5 的結果決定投入：

- 在 `core/ime/candidate_reranker.*` 實作 beam，保留 greedy，用 TuneChoice 做 A/B。
- 依 PinyinGPT，把 beam 寬度 8 與 16 都列入網格。
- 評分全部走 P2 的 `ScoreBatch`。

Gate：沿用原方案 §23（beam Top-1 高於 greedy，wrong-change 不增加，P95 可接受），並以開放測試集和選字紀錄兩邊都通過為準。

### P3 Top-K 整句，不自己做 lattice（原方案 §7）

- **先用 librime 1.17 的 `max_sentences` 拿 Top-K 整句**，從候選清單取出（`candidate_list_begin` / `from_index`），交給 `ScoreBatch` 重排。
- 需要分段候選時，用 librime-lua（BSD-3）的 `Component.Translator(...):query()` 與 composition segments，**不 fork librime**。只有需要原始 WordGraph 時才寫 C++ plugin。
- 長度正規化照原方案 §11 做網格（`alpha = 0 / 0.3 / 0.5 / 1`）。

Gate：多字詞與多位置歧義的類別（原方案 §15 B、C）明顯勝過字級。

### P4 個人化進排序（原方案 §8）

- `PersonalLexicon` 的特徵照原方案的公式；user override 的行為參考 McBopomofo 的 user-override 層（MIT），不自己設計新語意。
- 權重由 TuneChoice 在選字紀錄上用交叉驗證調整（和這次調 margin 與先驗的方式相同）。

### P5、P6 前文與右側前文（原方案 §9、§10）

- 前文長度在 TuneChoice 裡跑 `0 / 10 / 20 / 30 / 50 / 100`。
- 右側前文照 zenz v3.2 的做法：直接在輸入格式裡放右文，排在後期。

### P7 專用 scorer：照 zenz 與 PinyinGPT 的做法（原方案 §20、§25）

| 項目 | 做法 |
|---|---|
| 模型 | 字級 GPT-2，20～100M。可以從 CKIP gpt2-tiny/base（繁中原生，一字一 token）繼續訓練，或照 zenz-xsmall 從頭訓練 |
| 輸入格式 | `〈左文〉〈注音〉〈輸出〉`（zenz v3）。注音 token 對齊輸出位置（PinyinGPT 的 Concat） |
| Loss | 只在 Rime 候選或同音字集合上正規化的 softmax，或 pairwise `-log σ(s⁺ − s⁻)`（原方案 §25.5） |
| 資料 | §2.4 的開放句子 → 萌典讀音 → 打進 Rime → Rime Top-K 當 hard negative（原方案 §25.3）。選字紀錄只在本機、只用來驗證或做個人層 |
| Teacher | **DeepSeek API**（沿用 `llm/personal/refine` 已設定的 `deepseek-v4-flash`，需要更準時才改用推理模式）負責三件事：排序 Rime 候選、依把握度過濾、產生對抗性 negative（原方案 §25.10）。原方案 §25.11 的「多 teacher 一致」改成同一模型在不同提示與候選順序下多次判斷，一致才給高權重；本機的 Qwen3.5 可以當第二意見。只送公開語料 |
| 工具 | llm.c 或 HF `transformers`；pairwise 用 sentence-transformers CrossEncoder 或 TRL `RewardTrainer` |
| 部署 | 轉成 GGUF（`gpt2` 架構），在 WisdomLLMHost 裡常駐；走 zenz 式 speculative：Rime 初稿當 draft，LM 只驗證、呼叫次數設上限 |

Gate：在開放測試集與選字紀錄上都贏過 P2.5 選出的通用模型，而且 P95 更低。

### P8、P9 LoRA 個人化（原方案 §26～§33）

放在 P7 之後。工具都有現成的：

- PEFT 訓練 LoRA（rank 2/4/8），再用 `convert_lora_to_gguf.py` 轉成 GGUF。
- 執行時用 `llama_adapter_lora_init` 載入，用 `llama_set_adapters_lora(ctx, adapters, n, scales)` 設定每個 adapter 的比例（User LoRA 0.1～0.5，出問題就調到 0）。
- shadow、rollback、anchor 照原方案，不另外設計。

### P10 何時介入：擴充現有的校準（原方案 §25.8）

- 現有的校準：只用一個特徵 `gain`，用 logistic 決定 show/hide，並依使用者的採用來學習。
- 下一步加特徵：Rime 前兩名的分數差（P0.5 之後才拿得到）、octagram 分數、個人詞頻、打字速度、最近的修正率。改成多特徵 logistic，仍然用 log loss 擬合、用 TuneChoice 的學習曲線決定樣本門檻。
- reward 照原方案 §25.8 的定義，從選字紀錄的 strong/medium signal 取得（P0 第 4 項）。

---

## 5. 近期工作清單（依序）

| # | 工作 | 產出 | 預估 |
|---|---|---|---|
| 1 | 確認 librime 版本，在 TuneChoice 印出 `get_version()` | 版本號；決定要不要升級到 1.17 | 0.5 天 |
| 2 | TuneChoice 補 wrong-change、Top-K、MRR、延遲 | 原方案 §16 的矩陣 | 1 天 |
| 3 | `build_open_set.py`：zhwiki、立法院 → 萌典讀音 → 打進 Rime | 可重現的開放測試集（腳本加種子） | 2～3 天 |
| 4 | P0.5：Rime、octagram bgw/bgc、`max_sentences` 的 oracle@K | 判斷 LM 重排還有多少空間 | 1 天 |
| 5 | P2：`ScoreBatch`、前文 KV 重用、單次 batch decode | 評分延遲降一個數量級 | 2～3 天 |
| 6 | P2.5：CKIP、Qwen3-0.6B-Base、Llama-3.2-Taiwan-1B、Gemma-3 的比較 | 預設 scorer 的選擇 | 2 天 |
| 6b | 用 DeepSeek 對開放測試集的一小批（例如 500 題）做排序：與萌典讀音、Rime 候選對照，量它的正確率與重複判斷的一致率 | 確認 DeepSeek 能不能當 teacher、預估 P7 的成本 | 0.5 天 |
| 7 | 依 4、6 的結果決定：先做 P1（beam）還是 P3（Top-K 整句） | — | — |

第 1～4 項不需要任何新模型，完全靠開源的 Rime 與資料，就能回答「LM 重排到底還有多少空間」。

---

## 6. 暫時不做

沿用原方案 §22，另外加上：

- 不自己設計讀音標註：用萌典與 McBopomofo，不要自己寫破音字規則。
- 不自己寫 lattice 或 decoder：先用 librime 1.17 的 Top-K 與 librime-lua。
- 不用 instruct 模型當 scorer（§1 已經量過：base 較好）。
- 不把 NC 或 ND 授權的文字、使用者的選字紀錄放進 repo 或會散布的模型。

---

## 7. 參考來源

- librime `rime_api.h`：https://github.com/rime/librime/blob/master/src/rime_api.h
- librime-lua：https://github.com/hchunhui/librime-lua
- librime-octagram：https://github.com/lotem/librime-octagram
- rime-octagram-data：https://github.com/lotem/rime-octagram-data
- RIME-LMDG：https://github.com/amzxyz/RIME-LMDG
- llama.cpp：https://github.com/ggml-org/llama.cpp
- azooKey Zenzai：https://github.com/azooKey/AzooKeyKanaKanjiConverter/blob/main/Docs/zenzai.md；論文：https://www.anlp.jp/proceedings/annual_meeting/2025/pdf_dir/P1-19.pdf；模型：https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf
- PinyinGPT：https://arxiv.org/abs/2203.00249
- CKIP transformers：https://github.com/ckiplab/ckip-transformers；https://huggingface.co/ckiplab/gpt2-tiny-chinese
- Qwen3.5-0.8B-Base：https://huggingface.co/Qwen/Qwen3.5-0.8B-Base
- Llama-3.2-Taiwan-1B：https://huggingface.co/lianghsun/Llama-3.2-Taiwan-1B
- Gemma 3 1B pt：https://huggingface.co/google/gemma-3-1b-pt
- McBopomofo：https://github.com/openvanilla/McBopomofo
- vChewing 詞庫：https://github.com/vChewing/vChewing-VanguardLexicon
- libchewing 資料：https://codeberg.org/chewing/libchewing-data
- g0v 萌典：https://github.com/g0v/moedict-data
- 教育部重編國語辭典：https://language.moe.gov.tw/001/Upload/Files/site_content/M0001/respub/dict_reviseddict_download.html
- g2pW：https://github.com/GitYCC/g2pW
- zhwiki dump：https://dumps.wikimedia.org/zhwiki/latest/
- CC-100：https://data.statmt.org/cc-100/
- 立法院開放資料：https://data.ly.gov.tw
- FineWeb-2：https://huggingface.co/datasets/HuggingFaceFW/fineweb-2
- 臺灣主權 AI 訓練語料庫：https://taic.moda.gov.tw
- SIGHAN CSC：https://github.com/NYCU-NLP/SIGHAN-CSC
- KenLM：https://github.com/kpu/kenlm
- DeepSeek Open Platform Terms of Service：https://cdn.deepseek.com/policies/en-US/deepseek-open-platform-terms-of-service.html
- TRL：https://github.com/huggingface/trl；PEFT：https://github.com/huggingface/peft；sentence-transformers：https://github.com/huggingface/sentence-transformers
