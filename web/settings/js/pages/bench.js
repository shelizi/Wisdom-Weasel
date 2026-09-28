import { h, section, card, checkbox, button, buttons, textarea, note } from '../ui.js';
import { get, int, profileLabel } from '../state.js';

// 題目：預測「前文|期望的詞」；校正「前文|注音|初稿|正確的句子」（注音每個音節以空格或聲調斷開）
const kDefaultPredict = [
  '謝謝你的|幫忙',
  '不好意思，打擾|一下',
  '祝你生日|快樂',
  '新年|快樂',
  '請問這個多少|錢',
  '天氣預報說明天會下|雨',
  '路上小|心',
  '早安，今天也要加|油',
  '我先去吃|飯',
  '一石二|鳥',
  '明天見，晚|安',
  '有空的話回我一下，謝|謝',
].join('\n');
const kDefaultCorrect = [
  '|ㄐㄧㄣ ㄊㄧㄢ ㄊㄧㄢ ㄑㄧˋ ㄏㄣˇ ㄏㄠˇ|今天天汽很好|今天天氣很好',
  '|ㄨㄛˇ ㄇㄣ˙ ㄇㄧㄥˊ ㄊㄧㄢ ㄐㄧㄢˋ|我門明天見|我們明天見',
  '|ㄒㄧㄝˋ ㄒㄧㄝ˙ ㄋㄧˇ ㄉㄜ˙ ㄅㄤ ㄇㄤˊ|謝謝你的邦忙|謝謝你的幫忙',
  '|ㄨㄛˇ ㄇㄣ˙ ㄒㄧㄚˋ ㄘˋ ㄗㄞˋ ㄐㄧㄢˋ|我們下次在見|我們下次再見',
  '|ㄓㄜˋ ㄍㄜ˙ ㄨㄣˋ ㄊㄧˊ ㄏㄣˇ ㄐㄧㄢˇ ㄉㄢ|這個問題很簡丹|這個問題很簡單',
  '|ㄑㄧㄥˇ ㄉㄚˋ ㄐㄧㄚ ㄓㄨㄣˇ ㄕˊ ㄔㄨ ㄒㄧˊ ㄏㄨㄟˋ ㄧˋ|請大家準時出席惠議|請大家準時出席會議',
  '他的電腦壞了，|ㄧㄠˋ ㄋㄚˊ ㄑㄩˋ ㄒㄧㄡ ㄌㄧˇ|要拿去修里|要拿去修理',
  '# 初稿本來就對：不該亂改',
  '|ㄨㄛˇ ㄇㄧㄥˊ ㄊㄧㄢ ㄧㄠˋ ㄑㄩˋ ㄊㄞˊ ㄅㄟˇ ㄎㄞ ㄏㄨㄟˋ|我明天要去台北開會|我明天要去台北開會',
].join('\n');

// 題目存在這台電腦的瀏覽器儲存空間（改過的題目下次還在）；讀不到時用預設
function loadText(key, fallback) {
  try {
    const v = localStorage.getItem(key);
    return v === null ? fallback : v;
  } catch {
    return fallback;
  }
}
function saveText(key, value) {
  try {
    localStorage.setItem(key, value);
  } catch {
    // 存不了就算了，只影響下次開啟時的預設
  }
}

let predictText = loadText('bench.predict', kDefaultPredict);
let correctText = loadText('bench.correct', kDefaultCorrect);
let doPredict = true;
let doCorrect = true;
let chosen = null;     // 勾選的模型設定（索引）
let running = false;
let statusText = '';
let run = null;        // 這次測試：{profiles, predict, correct, load[], predictResults[][], correctResults[][]}
let subscribed = false;

// 題目的一行 → 欄位（與輸入法端的解析相同：| 或全形｜分隔，# 開頭與空行略過）
function parseLines(text) {
  return text.split('\n').map((l) => l.trim()).filter((l) => l && !l.startsWith('#'))
    .map((l) => l.split(/[|｜]/).map((f) => f.trim()));
}

const avg = (list) => {
  const ms = list.filter(Boolean).map((r) => r.ms);
  return ms.length ? Math.round(ms.reduce((a, b) => a + b, 0) / ms.length) + ' ms' : '—';
};
const rate = (list, total, key = 'ok') => {
  const done = list.filter(Boolean);
  if (!total) return '—';
  const hit = done.filter((r) => r[key]).length;
  return hit + ' / ' + total + (done.length < total ? '（進行中）' : '') + '　' + Math.round((hit * 100) / total) + '%';
};

export default {
  id: 'bench',
  nav: '模型測試',
  icon: '',
  title: '模型測試',
  desc: '用一組題目測試各個模型的預測與整句校正：命中率與反應時間。',

  onShow(ctx) {
    if (!subscribed) {
      subscribed = true;
      ctx.on('bench', (e) => this.onEvent(ctx, e));
    }
  },

  onEvent(ctx, e) {
    if (!run) return;
    if (e.state === 'load') {
      run.load[e.profile] = e;
      const name = run.profiles[e.profile].name;
      statusText = e.ok ? '測試中：' + name + '（載入 ' + e.ms + ' ms）' : name + '：' + e.error;
    } else if (e.state === 'predict') {
      run.predictResults[e.profile][e.index] = e;
    } else if (e.state === 'correct') {
      run.correctResults[e.profile][e.index] = e;
    } else {
      running = false;
      statusText = e.state === 'done' ? '測試完成。' : e.state === 'cancelled' ? '已取消。' : '測試失敗：' + e.message;
      if (ctx.isCurrent(this)) ctx.rerender();
      return;
    }
    if (ctx.isCurrent(this) && this.update) this.update();
  },

  async start(ctx) {
    const { state } = ctx;
    const profiles = [...chosen].sort((a, b) => a - b).map((i) => state.profiles[i]).filter(Boolean);
    const predict = doPredict ? parseLines(predictText) : [];
    const correct = doCorrect ? parseLines(correctText) : [];
    const f = state.form;
    const options = {
      prompt: f.predict.prompt,
      typo_prompt: f.typo.prompt,
      n_ctx: f.local.n_ctx,
      n_gpu_layers: int(f.local.gpu_layers, 0),
      n_threads: int(f.local.threads, 4),
      temperature: parseFloat(get(state.llm, 'llamacpp/temperature')) || 0.8,
    };
    try {
      await ctx.call('bench.start', {
        profiles, options,
        predict: doPredict ? predictText : '', correct: doCorrect ? correctText : '',
      });
    } catch (e) {
      ctx.status(e.message, 'error');
      return;
    }
    run = {
      profiles, predict, correct, load: [],
      predictResults: profiles.map(() => []), correctResults: profiles.map(() => []),
    };
    running = true;
    statusText = '載入模型中…';
    ctx.rerender();
  },

  // 結果：總表與每一題的輸出
  renderResults() {
    if (!run) return [];
    const { profiles, predict, correct } = run;
    const summary = h('div', { class: 'table-wrap' }, h('table', { class: 'table' },
      h('thead', {}, h('tr', {}, ...['模型', '載入', '預測 第一個對', '預測 前五個有', '預測 平均', '校正 正確', '校正 平均'].map((t) => h('th', {}, t)))),
      h('tbody', {}, ...profiles.map((p, i) => {
        const load = run.load[i];
        const failed = load && !load.ok;  // 沒載入：沒有結果
        const or = (text) => (failed ? '—' : text);
        return h('tr', {},
          h('td', {}, profileLabel(p)),
          h('td', { class: load && !load.ok ? 'wrap' : '' }, !load ? '…' : load.ok ? load.ms + ' ms' : '✗ ' + load.error),
          h('td', {}, or(rate(run.predictResults[i], predict.length, 'top1'))),
          h('td', {}, or(rate(run.predictResults[i], predict.length))),
          h('td', {}, or(avg(run.predictResults[i]))),
          h('td', {}, or(rate(run.correctResults[i], correct.length))),
          h('td', {}, or(avg(run.correctResults[i]))));
      }))));
    const failed = (i) => run.load[i] && !run.load[i].ok;
    const mark = (r) => (!r ? '…' : r.ok ? '✓ ' : '✗ ');
    const detail = (title, headers, rows) => rows.length ? [
      h('div', { class: 'spacer' }),
      h('div', { class: 'field-label' }, title),
      h('div', { class: 'table-wrap' }, h('table', { class: 'table' },
        h('thead', {}, h('tr', {}, ...headers.map((t) => h('th', {}, t)), ...profiles.map((p) => h('th', {}, p.name)))),
        h('tbody', {}, ...rows))),
    ] : [];
    const predictRows = predict.map(([context, expected], j) => h('tr', {},
      h('td', { class: 'wrap' }, context), h('td', {}, expected),
      ...profiles.map((_, i) => {
        const r = run.predictResults[i][j];
        if (failed(i)) return h('td', {}, '—');
        return h('td', { class: 'wrap' }, mark(r) + (r ? r.candidates.join('、') || '（沒有候選）' : '') + (r ? '（' + r.ms + ' ms）' : ''));
      })));
    const correctRows = correct.map(([, , draft, expected], j) => h('tr', {},
      h('td', {}, draft), h('td', {}, expected),
      ...profiles.map((_, i) => {
        const r = run.correctResults[i][j];
        if (failed(i)) return h('td', {}, '—');
        return h('td', {}, mark(r) + (r ? (r.changed ? r.output : '（不改）') + '（' + r.ms + ' ms）' : ''));
      })));
    return [
      summary,
      ...detail('預測（✓ = 前五個候選有期望的詞）', ['前文', '期望'], predictRows),
      ...detail('整句校正', ['初稿', '正確'], correctRows),
    ];
  },

  render(ctx) {
    const { profiles, use } = ctx.state;
    // 預設勾選智慧預測與注音校正正在用的模型（render 比 onShow 先呼叫）
    if (chosen === null)
      chosen = new Set([use.predict, use.typo].filter((i) => i >= 0 && i < profiles.length));
    const statusEl = note(statusText);
    const results = h('div', {}, ...this.renderResults());
    this.update = () => {
      statusEl.set(statusText);
      results.replaceChildren(...this.renderResults());
    };
    const startButton = button(running ? '測試中…' : '開始測試', () => this.start(ctx), { primary: true, disabled: running });
    const cancelButton = button('取消', () => ctx.call('bench.cancel'), { disabled: !running });

    return [
      section('要測試的模型',
        card({
          desc: '用「語言模型」頁的設定與目前表單的提示詞、本機模型執行設定（不必先套用）。設定程式會自己載入模型，測完就釋放；本機模型會暫時多佔一份記憶體。API 模型每一題都會送出請求。',
          below: profiles.length
            ? h('div', {}, ...profiles.map((p, i) => h('div', {}, checkbox(chosen.has(i), profileLabel(p), (v) => {
              if (v) chosen.add(i);
              else chosen.delete(i);
            }, { disabled: running }))))
            : h('div', { class: 'muted' }, '還沒有模型設定，請先到「語言模型」頁新增。'),
        })),
      section('題目',
        card({
          title: checkbox(doPredict, '預測：前文|期望的詞', (v) => { doPredict = v; }, { disabled: running }),
          desc: '期望的詞出現在前五個候選裡算對（候選多接了字也算）；另外統計第一個候選就對的比例。',
          below: textarea(predictText, (v) => { predictText = v; saveText('bench.predict', v); }, { rows: 8, disabled: running }),
        }),
        card({
          title: checkbox(doCorrect, '整句校正：前文|注音|初稿|正確的句子', (v) => { doCorrect = v; }, { disabled: running }),
          desc: '前文可以空白；注音的每個音節以空格或聲調斷開（和打字時送給模型的一樣）。初稿和正確的句子相同時，模型不改才算對。# 開頭的行是註解。',
          below: textarea(correctText, (v) => { correctText = v; saveText('bench.correct', v); }, { rows: 8, disabled: running }),
        }),
        buttons(button('還原預設題目', () => {
          predictText = kDefaultPredict;
          correctText = kDefaultCorrect;
          saveText('bench.predict', predictText);
          saveText('bench.correct', correctText);
          ctx.rerender();
        }, { disabled: running }))),
      section('結果',
        card({
          below: [buttons(startButton, cancelButton), statusEl, h('div', { class: 'spacer' }), results],
        })),
    ];
  },
};
