// 設定的狀態：從 Rime 設定讀進表單、存回時轉成 llm 設定。
// 邏輯與原本的設定視窗（SettingsDialog）相同，存出來的 weasel.custom.yaml 也相同。
// Rime 的純量沒有型別，讀進來都是字串；存回時布林與數字交給 Rime 轉成文字。

export const state = {
  init: null,         // 設定程式啟動時讀到的全部資料
  llm: {},            // 目前儲存的 llm 設定（存檔時以它為底，保留介面上沒有的項目）
  form: null,         // 各頁表單
  profiles: [],       // 模型設定
  use: { predict: -1, refine: -1, typo: -1, scorer: -1 },  // 各功能選用的模型設定（profiles 的索引）
  loaded: {},         // 目前已套用的值（判斷要不要改方案檔）
  schemas: [],        // [{id, name, selected}]，已選的在前
  style: null,        // {active, fonts}
  dirty: new Set(),   // schemas / style / llm / grammar
};

// ---------------------------------------------------------------------------
// 路徑與型別

export function get(obj, path) {
  let v = obj;
  for (const key of path.split('/')) {
    if (v == null || typeof v !== 'object') return undefined;
    v = v[key];
  }
  return v;
}

export function set(obj, path, value) {
  const keys = path.split('/');
  let v = obj;
  for (const key of keys.slice(0, -1)) {
    if (v[key] == null || typeof v[key] !== 'object' || Array.isArray(v[key])) v[key] = {};
    v = v[key];
  }
  v[keys[keys.length - 1]] = value;
}

export function del(obj, path) {
  const keys = path.split('/');
  const parent = keys.length > 1 ? get(obj, keys.slice(0, -1).join('/')) : obj;
  if (parent && typeof parent === 'object') delete parent[keys[keys.length - 1]];
}

export function str(v) {
  return v == null || typeof v === 'object' ? '' : String(v);
}

export function bool(v, fallback) {
  if (typeof v === 'boolean') return v;
  const s = str(v).toLowerCase();
  if (s === 'true') return true;
  if (s === 'false') return false;
  return fallback;
}

// 與 C 的 atoi／_wtoi 相同：取開頭的整數，沒有數字時為 fallback
export function int(v, fallback) {
  const n = parseInt(str(v).trim(), 10);
  return Number.isNaN(n) ? fallback : n;
}

// 去掉首尾空白（含全形空白）與換行
export function trim(s) {
  return str(s).replace(/^[\s　]+|[\s　]+$/g, '');
}

const lower = (s) => str(s).toLowerCase();
const toWindowsPath = (p) => str(p).replace(/\//g, '\\');
const toYamlPath = (p) => str(p).replace(/\\/g, '/');
const clone = (v) => JSON.parse(JSON.stringify(v ?? {}));

// ---------------------------------------------------------------------------
// 模型設定（llm/profiles/pNN）

export function newProfile(fields = {}) {
  return {
    name: '', remote: false, model_path: '', model_type: 'Base', api_url: '', api_key: '', model: '',
    no_think: false, think_tokens: 2048, ...fields,
  };
}

export function profileLabel(p) {
  return (p.name || '（未命名）') + (p.remote ? '　［API］' : '　［本機］');
}

function loadProfiles(llm, defaultApiUrl) {
  const profiles = [];
  const indexOf = {};
  const map = llm.profiles && typeof llm.profiles === 'object' ? llm.profiles : {};
  for (const key of Object.keys(map).sort()) {
    const p = map[key] || {};
    const think = str(p.think_tokens);
    indexOf[key] = profiles.length;
    profiles.push(newProfile({
      name: str(p.name),
      remote: lower(p.type) === 'openai',
      model_path: toWindowsPath(p.model_path),
      model_type: lower(p.model_type) === 'instruct' ? 'Instruct' : 'Base',
      api_url: str(p.api_url),
      api_key: str(p.api_key),
      model: str(p.model),
      no_think: str(p.disable_thinking) === 'true',
      think_tokens: think === '' ? 2048 : Math.max(0, int(think, 0)),
    }));
  }
  let predict = -1, refine = -1, typo = -1, scorer = -1;
  if (!profiles.length) {
    // 舊設定轉換：本機模型、預測用的 API、精煉用的 API 各一組
    const provider = lower(get(llm, 'provider_type'));
    const path = toWindowsPath(get(llm, 'llamacpp/model_path'));
    if (path) {
      const stem = path.split('\\').pop().replace(/\.[^.]*$/, '');
      // 未設定時 LlamaCppProvider 預設 Instruct
      const type = lower(get(llm, 'llamacpp/model_type')) === 'base' ? 'Base' : 'Instruct';
      if (provider !== 'openai') predict = profiles.length;
      profiles.push(newProfile({ name: stem, model_path: path, model_type: type }));
    }
    const url = str(get(llm, 'openai/api_url'));
    if (url || provider === 'openai') {
      const model = str(get(llm, 'openai/model'));
      if (provider === 'openai') predict = profiles.length;
      profiles.push(newProfile({
        remote: true, api_url: url || defaultApiUrl, api_key: str(get(llm, 'openai/api_key')), model,
        name: model || 'OpenAI 相容 API',
      }));
    }
    const refineUrl = str(get(llm, 'personal/refine/api_url'));
    if (refineUrl) {
      const p = newProfile({
        remote: true, api_url: refineUrl, api_key: str(get(llm, 'personal/refine/api_key')),
        model: str(get(llm, 'personal/refine/model')),
      });
      p.name = '精煉用 ' + (p.model || 'API');
      profiles.forEach((q, i) => {
        if (q.remote && q.api_url === p.api_url && q.api_key === p.api_key && q.model === p.model) refine = i;
      });
      if (refine < 0) {
        refine = profiles.length;
        profiles.push(p);
      }
    }
  } else {
    const find = (key) => (str(key) in indexOf ? indexOf[str(key)] : -1);
    predict = find(get(llm, 'predict_profile'));
    refine = find(get(llm, 'personal/refine/profile'));
    typo = find(get(llm, 'typo/profile'));
    scorer = find(get(llm, 'choice/scorer/profile'));
  }
  if (typo < 0) typo = predict;  // 還沒選過：預設和智慧預測用同一個模型
  return { profiles, use: { predict, refine, typo, scorer } };
}

function saveProfiles(llm, profiles, use) {
  const keyOf = (i) => 'p' + String(i + 1).padStart(2, '0');
  delete llm.profiles;
  profiles.forEach((p, i) => {
    const base = 'profiles/' + keyOf(i) + '/';
    set(llm, base + 'name', p.name);
    set(llm, base + 'type', p.remote ? 'openai' : 'llamacpp');
    set(llm, base + 'model_path', toYamlPath(p.model_path));
    set(llm, base + 'model_type', p.model_type);
    set(llm, base + 'api_url', p.api_url);
    set(llm, base + 'api_key', p.api_key);
    set(llm, base + 'model', p.model);
    set(llm, base + 'disable_thinking', p.no_think);
    set(llm, base + 'think_tokens', p.think_tokens);
  });
  const valid = (i) => (i >= 0 && i < profiles.length ? i : -1);
  // 預測：展開到輸入法讀取的欄位
  const predict = valid(use.predict);
  set(llm, 'predict_profile', predict >= 0 ? keyOf(predict) : '');
  if (predict >= 0) {
    const p = profiles[predict];
    set(llm, 'provider_type', p.remote ? 'openai' : 'llamacpp');
    if (p.remote) {
      set(llm, 'openai/api_url', p.api_url);
      set(llm, 'openai/api_key', p.api_key);
      set(llm, 'openai/model', p.model);
      set(llm, 'openai/disable_thinking', p.no_think);
      set(llm, 'openai/think_tokens', p.think_tokens);
    } else {
      set(llm, 'llamacpp/model_path', toYamlPath(p.model_path));
      set(llm, 'llamacpp/model_type', p.model_type);
      set(llm, 'llamacpp/disable_thinking', p.no_think);
      set(llm, 'llamacpp/think_tokens', p.think_tokens);
    }
  }
  const none = newProfile();
  // 精煉
  const refine = valid(use.refine);
  const r = refine >= 0 ? profiles[refine] : none;
  set(llm, 'personal/refine/profile', refine >= 0 ? keyOf(refine) : '');
  set(llm, 'personal/refine/type', refine < 0 ? '' : r.remote ? 'openai' : 'llamacpp');
  set(llm, 'personal/refine/name', refine >= 0 ? r.name : '');
  set(llm, 'personal/refine/model_path', refine >= 0 && !r.remote ? toYamlPath(r.model_path) : '');
  set(llm, 'personal/refine/model_type', refine >= 0 && !r.remote ? r.model_type : '');
  set(llm, 'personal/refine/api_url', refine >= 0 && r.remote ? r.api_url : '');
  set(llm, 'personal/refine/api_key', refine >= 0 && r.remote ? r.api_key : '');
  set(llm, 'personal/refine/model', refine >= 0 && r.remote ? r.model : '');
  set(llm, 'personal/refine/disable_thinking', refine >= 0 && r.no_think);
  set(llm, 'personal/refine/think_tokens', r.think_tokens);
  // 注音校正
  const typo = valid(use.typo);
  const c = typo >= 0 ? profiles[typo] : none;
  set(llm, 'typo/profile', typo >= 0 ? keyOf(typo) : '');
  set(llm, 'typo/type', typo < 0 ? '' : c.remote ? 'openai' : 'llamacpp');
  set(llm, 'typo/model_path', typo >= 0 && !c.remote ? toYamlPath(c.model_path) : '');
  set(llm, 'typo/model_type', typo >= 0 && !c.remote ? c.model_type : '');
  set(llm, 'typo/api_url', typo >= 0 && c.remote ? c.api_url : '');
  set(llm, 'typo/api_key', typo >= 0 && c.remote ? c.api_key : '');
  set(llm, 'typo/model', typo >= 0 && c.remote ? c.model : '');
  set(llm, 'typo/disable_thinking', typo >= 0 && c.no_think);
  set(llm, 'typo/think_tokens', c.think_tokens);
  // 評分（推薦、整句重排）：只能用本機模型；沒選時借預測或校正已載入的本機模型
  const scorerIndex = valid(use.scorer);
  const sc = scorerIndex >= 0 && !profiles[scorerIndex].remote ? profiles[scorerIndex] : null;
  set(llm, 'choice/scorer/profile', sc ? keyOf(scorerIndex) : '');
  set(llm, 'choice/scorer/model_path', sc ? toYamlPath(sc.model_path) : '');
  set(llm, 'choice/scorer/model_type', sc ? sc.model_type : '');
}

// ---------------------------------------------------------------------------
// 表單 ↔ llm 設定

function loadForm(llm, defaults) {
  let prompt = str(get(llm, 'prompt')) || str(get(llm, 'llamacpp/prompt_prefix'));
  prompt = prompt.replace(/[\r\n]+$/, '');
  const legacy = str(get(llm, 'typo_correction'));
  const typoPrompt = str(get(llm, 'typo/prompt'));
  const max = int(get(llm, 'personal/max_candidates'), 3);
  const halfLife = int(get(llm, 'personal/half_life_days'), 30);
  return {
    predict: {
      enabled: bool(get(llm, 'enabled'), false),
      after_commit: bool(get(llm, 'predict_after_commit'), true),
      while_typing: bool(get(llm, 'predict_while_typing'), true),
      prompt,
    },
    choice: {
      log: bool(get(llm, 'choice/log'), false),
      rescore: bool(get(llm, 'choice/rescore'), false),
      rerank: ['off', 'shadow', 'on'].includes(str(get(llm, 'choice/rerank'))) ? str(get(llm, 'choice/rerank')) : 'off',
      rerank_margin: str(get(llm, 'choice/rerank_margin')) || '2',
      min_confidence: str(get(llm, 'choice/min_confidence')) || '0.5',
    },
    typo: {
      // 兩個開關各自獨立；舊設定 llm/typo_correction（off / rime / llm，llm 含 Rime 容錯）
      rime: bool(get(llm, 'typo/rime'), legacy === 'rime' || legacy === 'llm'),
      llm: bool(get(llm, 'typo/llm'), legacy === 'llm'),
      // 沒有自訂時顯示預設指令，方便在上面修改
      prompt: typoPrompt || defaults.correct_instruction,
    },
    personal: {
      enabled: bool(get(llm, 'personal/enabled'), true),
      keep_log: bool(get(llm, 'personal/keep_raw_log'), true),
      filter: bool(get(llm, 'personal/filter/enabled'), true),
      filter_min: str(get(llm, 'personal/filter/min_logprob')) || '-7.5',
      rime_boost: bool(get(llm, 'personal/rime_boost'), false),
      max: Math.max(0, Math.min(max, 5)),
      half_life: String(halfLife > 0 ? halfLife : 30),
      interval: str(get(llm, 'personal/refine/interval_days')) || '1',
    },
    // 本機模型（llama.cpp）的執行設定：預測的模型用；GPU 層數與執行緒整句校正與精煉也用
    local: {
      n_ctx: int(get(llm, 'llamacpp/n_ctx'), 2048),
      gpu_layers: String(int(get(llm, 'llamacpp/n_gpu_layers'), 0)),
      threads: String(int(get(llm, 'llamacpp/n_threads'), 4)),
    },
  };
}

// 目前的表單存成 llm 設定（以目前儲存的為底）
export function buildLlm() {
  const { form, profiles, use, init } = state;
  const llm = clone(state.llm);
  set(llm, 'enabled', form.predict.enabled);
  set(llm, 'predict_after_commit', form.predict.after_commit);
  set(llm, 'predict_while_typing', form.predict.while_typing);
  set(llm, 'choice/log', form.choice.log);
  set(llm, 'choice/rescore', form.choice.rescore);
  set(llm, 'choice/rerank', form.choice.rerank);
  const rerankMargin = parseFloat(trim(form.choice.rerank_margin));
  set(llm, 'choice/rerank_margin', Number.isFinite(rerankMargin) && rerankMargin >= 0 ? rerankMargin : 2);
  // 0～0.9；空白或不合理時用預設
  const minConfidence = parseFloat(trim(form.choice.min_confidence));
  set(llm, 'choice/min_confidence',
    Number.isFinite(minConfidence) && minConfidence >= 0 && minConfidence <= 0.9 ? minConfidence : 0.5);
  saveProfiles(llm, profiles, use);
  // 提示詞（兩種模式共用）：統一換行為 \n；改存 llm/prompt，移除舊的 llamacpp/prompt_prefix
  set(llm, 'prompt', trim(form.predict.prompt.replace(/\r/g, '')));
  del(llm, 'llamacpp/prompt_prefix');
  set(llm, 'typo/rime', form.typo.rime);
  set(llm, 'typo/llm', form.typo.llm);
  // 和預設相同（或清空）就不存，之後改了預設指令也會跟著更新
  const typoPrompt = trim(form.typo.prompt.replace(/\r/g, ''));
  set(llm, 'typo/prompt', typoPrompt === trim(init.defaults.correct_instruction) ? '' : typoPrompt);
  del(llm, 'typo_correction');
  const p = form.personal;
  const number = (text, fallback) => (trim(text) === '' ? fallback : int(text, 0));
  set(llm, 'personal/enabled', p.enabled);
  set(llm, 'personal/keep_raw_log', p.keep_log);
  set(llm, 'personal/filter/enabled', p.filter);
  // 門檻要是負數（每字的 log 機率）；空白或不合理時用預設
  const filterMin = parseFloat(trim(p.filter_min));
  set(llm, 'personal/filter/min_logprob', Number.isFinite(filterMin) && filterMin < 0 ? filterMin : -7.5);
  set(llm, 'personal/rime_boost', p.enabled && p.rime_boost);
  set(llm, 'personal/max_candidates', p.max < 0 ? 3 : p.max);
  const halfLife = number(p.half_life, 30);
  set(llm, 'personal/half_life_days', halfLife > 0 ? halfLife : 30);
  set(llm, 'personal/refine/interval_days', Math.max(0, number(p.interval, 1)));
  const local = form.local;
  set(llm, 'llamacpp/n_ctx', local.n_ctx > 0 ? local.n_ctx : 2048);
  set(llm, 'llamacpp/n_gpu_layers', Math.max(-1, number(local.gpu_layers, 0)));
  set(llm, 'llamacpp/n_threads', Math.max(1, number(local.threads, 4)));
  return llm;
}

// 套用前檢查：選用的模型設定要填完整。失敗時回傳 {page, message, profile}
export function validate() {
  const { form, profiles, use } = state;
  const valid = (i) => (i >= 0 && i < profiles.length ? i : -1);
  const llmOn = form.predict.enabled;
  const predict = valid(use.predict);
  if (llmOn && predict < 0) return { page: 'predict', message: '請選擇預測使用的模型（可在「語言模型」頁新增）。' };
  const typoLlm = form.typo.llm;
  const typo = valid(use.typo);
  if (typoLlm && typo < 0) return { page: 'typo', message: '請選擇注音校正使用的模型（可在「語言模型」頁新增）。' };
  for (const index of [llmOn ? predict : -1, valid(use.refine), typoLlm ? typo : -1]) {
    if (index < 0) continue;
    const p = profiles[index];
    if (p.remote && !p.api_url) return { page: 'models', profile: index, message: '請填寫 OpenAI 相容 API 的網址。' };
    if (!p.remote && !p.model_path) return { page: 'models', profile: index, message: '請選擇模型檔。' };
  }
  return null;
}

// 啟動或套用後：以儲存的設定重設表單
export function loadLlm(llm) {
  state.llm = llm || {};
  state.form = loadForm(state.llm, state.init.defaults);
  const { profiles, use } = loadProfiles(state.llm, state.init.defaults.api_url);
  state.profiles = profiles;
  state.use = use;
  state.loaded.rime_boost = bool(get(state.llm, 'personal/rime_boost'), false);
  state.loaded.typo_rime = state.form.typo.rime;
  state.loaded.rerank = state.form.choice.rerank;
}

// 套用：收集變更的部分
export function collectChanges() {
  const changes = {};
  const { dirty, form } = state;
  if (dirty.has('schemas')) changes.schemas = state.schemas.filter((s) => s.selected).map((s) => s.id);
  if (dirty.has('style')) changes.style = { color_scheme: state.style.active, fonts: state.style.fonts };
  if (dirty.has('llm')) {
    changes.llm = buildLlm();
    // 注音排序、Rime 容錯：有變才改方案
    const boost = form.personal.enabled && form.personal.rime_boost;
    if (boost !== state.loaded.rime_boost) changes.rime_boost = boost;
    if (form.typo.rime !== state.loaded.typo_rime) changes.typo_rime = form.typo.rime;
    // 整句重排要 Rime 給多個整句：開關有變才改方案
    const sentences = form.choice.rerank !== 'off';
    if (sentences !== (state.loaded.rerank !== 'off')) changes.rerank_sentences = sentences;
  }
  if (dirty.has('grammar') && form.choice.grammar !== state.loaded.grammar) changes.grammar = form.choice.grammar;
  return changes;
}
