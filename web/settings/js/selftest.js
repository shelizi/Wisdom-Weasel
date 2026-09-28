// 自我測試（開發用）：WeaselDeployer.exe /websettings --selftest <結果檔> <階段>
// 請在沙盒使用者資料夾下執行（WEASEL_TEST_USER_DIR），會真的寫入設定並重新部署。
//   apply   修改各類設定並套用；另外測驗證、下載進度與取消、錯誤路徑
//   verify  重新啟動後讀回的狀態（由外部比對 apply 的 expected）
import { state, newProfile, validate } from './state.js';
import { addFont, setToken } from './fontface.js';

const wait = (ms) => new Promise((r) => setTimeout(r, ms));

function snapshot() {
  return {
    schemas: state.schemas.filter((s) => s.selected).map((s) => s.id),
    style: { active: state.style.active, fonts: state.style.fonts },
    form: state.form,
    profiles: state.profiles,
    use: state.use,
    grammar: state.init.grammar,
  };
}

async function expectError(ctx, method, params, results, name) {
  try {
    await ctx.call(method, params);
    results.checks.push({ name, ok: false, detail: '沒有丟出錯誤' });
  } catch (e) {
    results.checks.push({ name, ok: true, detail: e.message });
  }
}

// 語言模型下載：收到進度後取消，應該收到「已取消」
async function downloadAndCancel(ctx, results) {
  const events = [];
  ctx.on('grammar', (e) => events.push(e));
  await ctx.call('grammar.download');
  for (let i = 0; i < 100 && !events.some((e) => e.state === 'progress'); ++i) await wait(100);
  const sawProgress = events.some((e) => e.state === 'progress');
  const cancel = await ctx.call('grammar.download');  // 下載中再按一次 = 取消
  for (let i = 0; i < 100 && !events.some((e) => e.state !== 'progress'); ++i) await wait(100);
  const end = events.find((e) => e.state !== 'progress');
  results.checks.push({
    name: 'grammar download progress + cancel',
    ok: sawProgress && !!cancel.cancelled && !!end && (end.state === 'error' ? end.message === '已取消' : end.state === 'done'),
    detail: JSON.stringify({ progressEvents: events.filter((e) => e.state === 'progress').length, cancel, end }),
  });
}

async function applyPhase(ctx) {
  const results = { phase: 'apply', before: snapshot(), checks: [] };

  // 唯讀的方法都要能回應
  for (const [method, params] of [['stats.get', { span: 0 }], ['calibration.get', {}], ['choicelog.status', {}], ['grammar.status', {}],
    ['models.files', {}], ['models.scan', { current: '' }], ['dicts.list', {}], ['personal.status', {}],
    ['llm.probe', {}], ['fonts.list', {}], ['style.preview', { id: state.style.active }], ['schemas.details', { id: state.schemas[0].id }]]) {
    try {
      const r = await ctx.call(method, params);
      results.checks.push({ name: method, ok: true, detail: JSON.stringify(r).slice(0, 160) });
    } catch (e) {
      results.checks.push({ name: method, ok: false, detail: e.message });
    }
  }
  await expectError(ctx, 'models.download', { url: 'https://example.com/not-a-model.txt' }, results, 'models.download rejects non-gguf url');
  await expectError(ctx, 'models.add', { path: 'C:\\Windows\\win.ini' }, results, 'models.add rejects non-gguf file');
  await expectError(ctx, 'bench.start', { profiles: [newProfile({ name: 'x' })], predict: '只有一欄', correct: '' }, results, 'bench.start rejects malformed cases');
  await expectError(ctx, 'bench.start', { profiles: [], predict: '前文|詞', correct: '' }, results, 'bench.start needs a model');
  await downloadAndCancel(ctx, results);

  // 驗證：啟用預測卻沒有選模型 → 擋下
  const f = state.form;
  f.predict.enabled = true;
  const savedPredict = state.use.predict;
  state.use.predict = -1;
  const problem = validate();
  results.checks.push({ name: 'validate blocks missing predict model', ok: !!problem && problem.page === 'predict', detail: JSON.stringify(problem) });
  state.use.predict = savedPredict;

  // 各類設定都改一點
  const expected = {};
  const unselected = state.schemas.find((s) => !s.selected);
  if (unselected) unselected.selected = true;
  expected.schemas = state.schemas.filter((s) => s.selected).map((s) => s.id);
  ctx.markDirty('schemas');

  const schemes = state.init.style.schemes;
  const index = schemes.findIndex((s) => s.id === state.style.active);
  state.style.active = schemes[(index + 1) % schemes.length].id;
  const face = setToken(addFont(state.style.fonts.font_face, 'Segoe UI', { start: '0', end: '7f' }), 'weight', 'bold');
  state.style.fonts = { ...state.style.fonts, font_point: state.style.fonts.font_point + 1, font_face: face };
  expected.style = { active: state.style.active, fonts: state.style.fonts };
  ctx.markDirty('style');

  state.profiles.push(newProfile({ name: 'selftest API', remote: true, api_url: 'http://localhost:1/v1/chat/completions', model: 'selftest-model', no_think: true, think_tokens: 0 }));
  state.use.predict = state.profiles.length - 1;
  f.predict.enabled = true;
  f.predict.after_commit = !f.predict.after_commit;
  f.predict.prompt = '自我測試提示詞\n第二行';
  f.typo.rime = !f.typo.rime;
  f.typo.prompt = '自我測試校正指令';
  f.choice.rescore = !f.choice.rescore;
  f.choice.log = !f.choice.log;
  f.choice.min_confidence = f.choice.min_confidence === '0.25' ? '0.5' : '0.25';
  f.personal.max = 2;
  f.personal.half_life = '45';
  f.personal.interval = '3';
  f.personal.rime_boost = !f.personal.rime_boost;
  f.personal.filter = !f.personal.filter;
  f.personal.filter_min = '-8';
  f.local.n_ctx = f.local.n_ctx === 4096 ? 8192 : 4096;
  f.local.gpu_layers = f.local.gpu_layers === '-1' ? '0' : '-1';
  f.local.threads = '6';
  ctx.markDirty('llm');
  if (state.init.grammar.ready || f.choice.grammar) {
    f.choice.grammar = !f.choice.grammar;
    ctx.markDirty('grammar');
  }
  expected.form = JSON.parse(JSON.stringify(f));
  expected.profiles = JSON.parse(JSON.stringify(state.profiles));
  expected.use = { ...state.use };

  const started = Date.now();
  const ok = await ctx.apply();
  results.apply = { ok, ms: Date.now() - started, status: document.getElementById('status').textContent, dirty: [...state.dirty] };
  results.expected = expected;
  results.after = snapshot();
  return results;
}

export async function runSelftest(ctx, phase) {
  let results;
  try {
    results = phase === 'apply' ? await applyPhase(ctx) : { phase, state: snapshot() };
  } catch (e) {
    results = { phase, error: e.stack || e.message };
  }
  await ctx.call('app.selftestDone', results);
}
