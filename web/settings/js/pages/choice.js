import { h, section, card, checkbox, button, select, input, note, confirm, lines } from '../ui.js';
import { int } from '../state.js';
import { profileInfo } from './profiles.js';

// 介入程度：整句重排的門檻與信心校準的門檻一起設（原方案 §12）。數值依 TuneChoice 與使用者的校準樣本
export const kInterventions = [
  { value: 'conservative', label: '保守（很有把握才推薦）', margin: 12, min_confidence: 0.7 },
  { value: 'balanced', label: '平衡', margin: 4, min_confidence: 0.5 },
  { value: 'aggressive', label: '積極（常常推薦）', margin: 1, min_confidence: 0.3 },
];

function interventionOf(c) {
  const m = parseFloat(c.rerank_margin), p = parseFloat(c.min_confidence);
  const found = kInterventions.find((k) => k.margin === m && k.min_confidence === p);
  return found ? found.value : 'custom';
}

const kRerankModes = [
  { value: 'off', label: '關閉' },
  { value: 'shadow', label: '只統計（不顯示，先驗證效果）' },
  { value: 'on', label: '顯示為「推薦」' },
];

const kSpans = [
  { value: 1, label: '今天' },
  { value: 7, label: '最近 7 天' },
  { value: 30, label: '最近 30 天' },
  { value: 0, label: '全部' },
];
let span = 7;
let selectedRow = 0;
let downloading = false;
let downloadText = '';
let subscribed = false;
let tuning = false;          // 整句重排自動調整進行中
let tuneText = '';           // 進度或結果
let tuneGrid = null;         // 完成的網格：{tops, margins, cells:{top:{margin:{accuracy,wrong_change}}}, best:{top,margin}}
let autoCfg = null;          // 自動微調開關與排程狀態
let autoEntries = [];        // 最近的自動調整紀錄
let autoRunning = false;
let autoText = '';
let rankStatus = null;       // 學習式排序器狀態（loaded/type/trained/history）
let rankRunning = false;
let rankText = '';

function grammarStatus(g) {
  if (downloading) return downloadText || '下載中…';
  return g.ready ? '模型檔已下載（' + g.size_mb + ' MB）' : '尚未下載模型檔';
}

export default {
  id: 'choice',
  nav: '選字策略',
  icon: '',
  title: '選字策略',
  desc: '用統計語言模型改善整句選字，並記錄選字的準確度，方便比較調整前後的效果。',

  onShow(ctx) {
    if (!subscribed) {
      subscribed = true;
      ctx.on('grammar', async (e) => {
        if (e.state === 'progress') {
          downloading = true;
          downloadText = '下載中… ' + e.percent + '%';
        } else {
          downloading = false;
          downloadText = '';
          ctx.state.init.grammar = await ctx.call('grammar.status');
          ctx.status(e.state === 'done' ? '語言模型已下載，勾選「用語言模型改善整句選字」後按套用。'
            : '語言模型下載失敗：' + e.message, e.state === 'done' ? 'ok' : 'error');
        }
        if (ctx.isCurrent(this)) ctx.rerender();
      });
      ctx.on('rerank_tune', (e) => this.onTuneEvent(ctx, e));
      ctx.on('rerank_auto', (e) => this.onAutoEvent(ctx, e));
      ctx.on('rank_train', (e) => this.onRankEvent(ctx, e));
    }
    this.refresh(ctx);
  },

  async refresh(ctx) {
    try {
      const [stats, log, grammar, calibration, history, cfg, rank] = await Promise.all([
        ctx.call('stats.get', { span }), ctx.call('choicelog.status'), ctx.call('grammar.status'),
        ctx.call('calibration.get'), ctx.call('rerank.history'), ctx.call('rerank.auto_cfg'),
        ctx.call('rank.status')]);
      this.stats = stats;
      this.calibration = calibration;
      this.log = log;
      autoEntries = history.entries || [];
      autoCfg = cfg;
      rankStatus = rank;
      ctx.state.init.grammar = grammar;
      downloading = grammar.downloading || downloading;
      if (ctx.isCurrent(this)) ctx.rerender();
    } catch (e) {
      ctx.status(e.message, 'error');
    }
  },

  // 整句重排自動調整：用選字紀錄裡自己的 Top-K 整句，拿目前的評分模型掃「門檻 × 評幾句」
  async startTune(ctx) {
    const { state } = ctx;
    // 評分沒選時借智慧預測或注音校正的本機模型（與輸入法端的借用相同）
    const local = (i) => (i >= 0 && i < state.profiles.length && !state.profiles[i].remote ? i : -1);
    const i = [state.use.scorer, state.use.predict, state.use.typo].map(local).find((x) => x >= 0);
    const p = i === undefined ? null : state.profiles[i];
    if (!p || p.remote) {
      ctx.status('請先在「各功能使用的模型」幫評分（或智慧預測）選一組本機模型。', 'error');
      return;
    }
    if (!p.model_path) {
      ctx.status('評分用的模型設定還沒選模型檔。', 'error');
      return;
    }
    const f = state.form;
    try {
      await ctx.call('rerank.tune', {
        profile: { remote: false, model_path: p.model_path, model_type: p.model_type },
        options: {
          n_ctx: f.local.n_ctx,
          n_gpu_layers: int(f.local.gpu_layers, 0),
          n_threads: int(f.local.threads, 4),
        },
        context: int(f.choice.rerank_context, 30),
        alpha: parseFloat(f.choice.rerank_alpha) || 0,
        personal: parseFloat(f.choice.rerank_personal) || 0,
      });
    } catch (e) {
      ctx.status(e.message, 'error');
      return;
    }
    tuning = true;
    tuneGrid = null;
    tuneText = '準備中…';
    ctx.rerender();
  },

  onTuneEvent(ctx, e) {
    if (e.state === 'pause') {
      tuneText = e.message;
    } else if (e.state === 'load') {
      if (e.ok) {
        tuneText = '模型已載入（' + e.ms + ' ms），正在讀取選字紀錄並評分…';
      } else {
        tuning = false;
        tuneText = '無法載入模型（檔案不存在、記憶體不足或格式不支援）。';
      }
    } else if (e.state === 'score') {
      tuneText = '用目前的模型評分：' + e.done + ' / ' + e.total + ' 筆';
    } else if (e.state === 'done') {
      tuning = false;
      if (!(e.top >= 2)) {
        tuneText = '紀錄裡沒有足夠的整句候選可以比較（至少 2 句），請改用「只統計」打一些字之後再試。';
        if (ctx.isCurrent(this)) ctx.rerender();
        return;
      }
      const c = ctx.state.form.choice;
      c.rerank_margin = String(e.margin);
      c.rerank_sentences = String(e.top);
      if (int(c.sentences, 0) < e.top) c.sentences = String(e.top);
      ctx.markDirty('llm');
      tuneGrid = e.grid && e.grid.length ? {
        tops: [...new Set(e.grid.map((g) => g.top))],
        margins: [...new Set(e.grid.map((g) => g.margin))],
        cells: Object.fromEntries(e.grid.map((g) => [g.top + '/' + g.margin, g])),
        best: { top: e.top, margin: e.margin },
      } : null;
      tuneText = '最佳參數：評 ' + e.top + ' 句、門檻 ' + e.margin + '（' + e.cases + ' 筆紀錄裡準確率 '
        + Math.round(e.accuracy * 1000) / 10 + '%，把原本對的改掉 ' + Math.round(e.wrong_change * 1000) / 10
        + '%）。已填入並套用。';
      ctx.rerender();
      ctx.apply().then((ok) => {
        if (!ok) {
          tuneText = tuneText.replace('已填入並套用。', '參數已填入表單，但套用未完成。');
          if (ctx.isCurrent(this)) ctx.rerender();
        }
      });
      return;
    } else if (e.state === 'cancelled') {
      tuning = false;
      tuneText = '已取消。';
    } else if (e.state === 'error') {
      tuning = false;
      tuneText = '自動調整失敗：' + e.message;
    }
    if (ctx.isCurrent(this)) ctx.rerender();
  },

  // 把 top/margin 填入表單並套用（自動微調與回復共用）
  applyParams(ctx, top, margin) {
    const c = ctx.state.form.choice;
    c.rerank_margin = String(margin);
    c.rerank_sentences = String(top);
    if (int(c.sentences, 0) < top) c.sentences = String(top);
    ctx.markDirty('llm');
    ctx.apply();
    this.refresh(ctx);
  },

  onAutoEvent(ctx, e) {
    if (e.state === 'pause') {
      autoText = e.message;
    } else if (e.state === 'score') {
      autoText = '用目前的模型評分：' + e.done + ' / ' + e.total + ' 筆';
    } else if (e.state === 'done' || e.state === 'error' || e.state === 'cancelled') {
      autoRunning = false;
      if (e.action === 'apply' || e.action === 'rollback') {
        this.applyParams(ctx, e.top, e.margin);
        autoText = (e.action === 'rollback' ? '偵測到修正率上升，已回復：' : '已調整：')
          + (e.message || '評 ' + e.top + ' 句、門檻 ' + e.margin);
      } else {
        autoText = e.message || (e.state === 'error' ? '檢查失敗。' : '');
      }
    }
    if (ctx.isCurrent(this)) ctx.rerender();
  },

  async startAuto(ctx) {
    try {
      const r = await ctx.call('rerank.auto', {});
      if (r && r.action === 'rollback') {
        this.applyParams(ctx, r.top, r.margin);
        autoText = '偵測到修正率上升，已回復：評 ' + r.top + ' 句、門檻 ' + r.margin;
        ctx.rerender();
        return;
      }
      if (r && r.action === 'skip') {
        autoText = '已跳過：' + (r.reason || '');
        ctx.rerender();
        return;
      }
      if (r && r.started) {
        autoRunning = true;
        autoText = '準備中…';
        ctx.rerender();
      }
    } catch (e) {
      ctx.status(e.message, 'error');
    }
  },

  async toggleAuto(ctx, v) {
    try {
      autoCfg = await ctx.call('rerank.auto_cfg', { enabled: v });
      if (ctx.isCurrent(this)) ctx.rerender();
    } catch (e) {
      ctx.status(e.message, 'error');
    }
  },

  async revert(ctx) {
    try {
      const r = await ctx.call('rerank.revert');
      if (!r || !r.found) {
        ctx.status('沒有可回復的參數。', 'error');
        return;
      }
      this.applyParams(ctx, r.top, r.margin);
      autoText = '已回復：評 ' + r.top + ' 句、門檻 ' + r.margin;
      ctx.rerender();
    } catch (e) {
      ctx.status(e.message, 'error');
    }
  },

  autoStatusLine() {
    const labels = { apply: '調整', rollback: '回復', hold: '維持', skip: '跳過' };
    const fmt = (e) => {
      const d = new Date(e.ts);
      const when = (d.getMonth() + 1) + '/' + d.getDate() + ' '
        + String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
      let s = when + '　' + (labels[e.action] || e.action);
      if (e.top) s += '：評 ' + e.top + ' 句、門檻 ' + e.margin;
      if (e.note) s += '（' + e.note + '）';
      return s;
    };
    const parts = [];
    if (autoCfg) {
      parts.push(autoCfg.enabled
        ? (autoCfg.scheduled ? '排程已設定（每週）' : '已開啟，但排程未建立')
        : '自動微調關閉');
      if (autoCfg.last_run_ms) {
        const d = new Date(autoCfg.last_run_ms);
        parts.push('上次檢查 ' + (d.getMonth() + 1) + '/' + d.getDate() + ' '
          + String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0'));
      }
    }
    const recent = autoEntries.slice(0, 5).map(fmt);
    return parts.concat(recent).join('\n');
  },

  onRankEvent(ctx, e) {
    if (e.state === 'pause') {
      rankText = e.message;
    } else if (e.state === 'score') {
      rankText = '用目前的模型評分：' + e.done + ' / ' + e.total + ' 筆';
    } else if (e.state === 'done' || e.state === 'error' || e.state === 'cancelled') {
      rankRunning = false;
      rankText = e.message || (e.exported ? '已匯出模型。' : '沒有匯出。');
      this.refresh(ctx);
    }
    if (ctx.isCurrent(this)) ctx.rerender();
  },

  async startRank(ctx) {
    try {
      const r = await ctx.call('rank.train', {});
      if (r && r.started) {
        rankRunning = true;
        rankText = '準備中…';
        ctx.rerender();
      } else if (r && r.message) {
        rankText = r.message;
        ctx.rerender();
      }
    } catch (e) {
      ctx.status(e.message, 'error');
    }
  },

  rankStatusLine() {
    if (!rankStatus) return '讀取中…';
    const parts = [];
    if (rankStatus.loaded) {
      const t = rankStatus.trained || {};
      parts.push('排序器使用中（' + rankStatus.type + '，門檻 '
        + Math.round((rankStatus.margin || 0) * 100) / 100
        + (t.val_acc ? '；驗證 ' + Math.round(t.val_acc * 1000) / 10 + '%' : '') + '）');
    } else {
      parts.push('還沒有排序器：用選字紀錄訓一個小模型，贏過目前設定才啟用。');
    }
    const last = (rankStatus.history || [])[0];
    if (last && last.ts) {
      const d = new Date(last.ts);
      parts.push('上次訓練 ' + (d.getMonth() + 1) + '/' + d.getDate()
        + '：' + last.type + ' 驗證 ' + Math.round((last.val_acc || 0) * 1000) / 10
        + '%（公式 ' + Math.round((last.base_acc || 0) * 1000) / 10 + '%）');
    }
    return parts.join('\n');
  },

  // 自動調整的網格：每列一個「評幾句」、每欄一個門檻；格內是準確率，選中的打勾
  renderTuneGrid() {
    if (!tuneGrid) return null;
    const { tops, margins, cells, best } = tuneGrid;
    return h('div', { class: 'table-wrap' }, h('table', { class: 'table' },
      h('thead', {}, h('tr', {}, h('th', {}, '評幾句＼門檻'), ...margins.map((m) => h('th', {}, String(m))))),
      h('tbody', {}, tops.map((t) => h('tr', {}, h('td', {}, t + ' 句'),
        ...margins.map((m) => {
          const g = cells[t + '/' + m];
          if (!g) return h('td', {}, '—');
          const pct = Math.round(g.accuracy * 1000) / 10 + '%';
          return h('td', { title: '把原本對的改掉 ' + Math.round(g.wrong_change * 1000) / 10 + '%' },
            t === best.top && m === best.margin ? '✓ ' + pct : pct);
        }))))));
  },

  render(ctx) {
    const { form, init } = ctx.state;
    const c = form.choice;
    const g = init.grammar;

    // 選字統計
    const stats = this.stats || [];
    if (selectedRow >= stats.length) selectedRow = 0;
    const detail = h('div', { class: 'card-desc status-text' }, stats.length ? lines(stats[selectedRow].detail) : '這段期間還沒有紀錄。');
    const columns = ['版本', '設定', '送出', '第一候選', '送出後刪', '換字', '推薦套用', 'LLM 採用', '整句重排'];
    const table = h('table', { class: 'table' },
      h('thead', {}, h('tr', {}, columns.map((c) => h('th', {}, c)))),
      h('tbody', {}, stats.map((r, i) => {
        const tr = h('tr', { class: i === selectedRow ? 'selected' : '', onclick: () => {
          selectedRow = i;
          table.querySelectorAll('tbody tr').forEach((row, j) => row.classList.toggle('selected', j === i));
          detail.replaceChildren(...lines(r.detail));
        } }, [r.version, r.settings, r.commits, r.first_ok, r.deleted, r.changed, r.recommended, r.llm, r.rerank].map((v) => h('td', {}, v)));
        return tr;
      })));

    const log = this.log;
    const logStatus = log ? (log.records ? '已記錄 ' + log.records + ' 筆（' + log.kb + ' KB）' : '還沒有紀錄') : '';

    return [
      section('語言模型',
        card({
          title: checkbox(c.grammar, '用語言模型改善整句選字（RIME octagram，離線運作）', (v) => {
            if (v && !g.ready) {
              c.grammar = false;
              ctx.status('請先下載語言模型檔。', 'error');
              ctx.rerender();
              return;
            }
            c.grammar = v;
            ctx.markDirty('grammar');
          }),
          desc: '選字時參考詞與詞的搭配，長句與少見的組合改善較明顯。需要先下載約 41 MB 的模型檔；套用後重新部署，三個注音方案都會使用。',
          below: h('div', { class: 'row' },
            h('span', { class: 'muted grow' }, grammarStatus(g)),
            button(downloading ? '取消下載' : g.ready ? '重新下載' : '下載模型', async () => {
              try {
                const r = await ctx.call('grammar.download');
                downloading = !r.cancelled;
                downloadText = '';
                ctx.rerender();
              } catch (e) {
                ctx.status(e.message, 'error');
              }
            })),
        })),
      section('推薦',
        card({
          title: checkbox(c.rescore, '用本機模型推薦更通順的同音字（候選第一個標示「推薦」，按 Tab 套用到組字區）', (v) => {
            c.rescore = v;
            ctx.markDirty('llm');
          }),
          desc: '打字停頓時，比較整句裡同音字的通順度。用下面「整句重排」的評分模型（只能用本機模型）。',
        }),
        card({
          title: '長句的整句候選',
          desc: 'Rime 預設只有第一個候選是整句，第二名起是句首的詞（方便從句首一段一段選），句子越長越顯得短。'
            + '打開後，候選窗前面會列出幾個完整的整句（多半只差一兩個字），句首的詞會移到後面。'
            + '開啟整句重排時會自動補到「評幾句」的數量。套用後會重新部署。',
          control: h('div', { class: 'row' },
            input(c.sentences, (v) => { c.sentences = v; ctx.markDirty('llm'); }, { width: '60px' }),
            h('span', { class: 'muted' }, '句（0 = 關閉，最多 63）')),
        }),
        card({
          title: '整句重排',
          desc: 'Rime 先給出符合注音的幾個整句，本機模型依前文挑最通順的一句；比 Rime 第一句好超過門檻才推薦（Tab 套用）。'
            + '候選一律來自 Rime，模型只負責排序。建議先選「只統計」用一陣子，在下面的選字統計看「整句重排」欄（改對／會改）再決定要不要顯示；'
            + '顯示時取代上面的同音字推薦。開關時會重新部署，候選窗會列出至少「評幾句」個整句候選（見上面的「長句的整句候選」）。',
          control: select(kRerankModes, c.rerank, (v) => {
            c.rerank = v;
            ctx.markDirty('llm');
            ctx.rerender();
          }, { width: '240px' }),
          below: [
            h('div', { class: 'row' }, h('span', { class: c.rerank === 'off' && !c.rescore && !c.show_probability ? 'muted' : '' }, '評分模型（推薦與整句機率也用）'),
              profileInfo(ctx, 'scorer', { disabled: c.rerank === 'off' && !c.rescore && !c.show_probability })),
            h('div', { class: 'row' }, h('span', { class: c.rerank === 'off' ? 'muted' : '' }, '介入程度'),
              select([...kInterventions, { value: 'custom', label: '自訂（下面的門檻）' }], interventionOf(c), (v) => {
                const k = kInterventions.find((x) => x.value === v);
                if (!k) return;
                c.rerank_margin = String(k.margin);
                c.min_confidence = String(k.min_confidence);
                ctx.markDirty('llm');
                ctx.rerender();
              }, { width: '240px', disabled: c.rerank === 'off' })),
            h('div', { class: 'row' }, h('span', { class: c.rerank === 'off' ? 'muted' : '' }, '門檻'),
              input(c.rerank_margin, (v) => { c.rerank_margin = v; ctx.markDirty('llm'); },
                { width: '80px', disabled: c.rerank === 'off' }),
              h('span', { class: 'muted' }, '（整句 log 機率；越大越保守。信心門檻在下面的「信心校準」）')),
            h('div', { class: 'row' }, h('span', { class: c.rerank === 'off' ? 'muted' : '' }, '前文'),
              input(c.rerank_context, (v) => { c.rerank_context = v; ctx.markDirty('llm'); },
                { width: '60px', disabled: c.rerank === 'off' }),
              h('span', { class: 'muted' }, '字（給模型看的前文長度，0～200）'),
              h('span', { class: c.rerank === 'off' ? 'muted' : '' }, '長度正規化'),
              input(c.rerank_alpha, (v) => { c.rerank_alpha = v; ctx.markDirty('llm'); },
                { width: '60px', disabled: c.rerank === 'off' }),
              h('span', { class: 'muted' }, '（0～1；0 = 不正規化）')),
            h('div', { class: 'row' }, h('span', { class: c.rerank === 'off' ? 'muted' : '' }, '個人詞庫加分'),
              input(c.rerank_personal, (v) => { c.rerank_personal = v; ctx.markDirty('llm'); },
                { width: '60px', disabled: c.rerank === 'off' }),
              h('span', { class: 'muted' }, '（0 = 不用；句子裡有你常打的詞就加分，每個詞加 權重 × log(1 + 詞分數)）')),
            h('div', { class: 'row' }, h('span', { class: c.rerank === 'off' ? 'muted' : '' }, '評幾句'),
              input(c.rerank_sentences, (v) => { c.rerank_sentences = v; ctx.markDirty('llm'); },
                { width: '60px', disabled: c.rerank === 'off' }),
              h('span', { class: 'muted' }, '句（模型最多評幾句整句，2～63；越多越準但越慢）')),
            h('div', { class: 'row' },
              button(tuning ? '調整中…' : '依目前模型自動調整', () => this.startTune(ctx),
                { disabled: tuning }),
              button('取消', () => ctx.call('rerank.cancel'), { disabled: !tuning }),
              h('span', { class: 'muted grow' }, tuneText)),
            h('div', { class: 'row' }, h('span', { class: 'muted' },
              '自動調整：讀選字紀錄裡你的整句候選，用上面的評分模型掃「評幾句 × 門檻」，把準確率最高的組合填入並套用。'
                + '需要選字紀錄有整句候選（開啟「記錄選字過程」後打過字即有）；調整期間輸入法會暫時放掉模型。')),
            this.renderTuneGrid(),
            h('div', { class: 'row' },
              checkbox(autoCfg ? autoCfg.enabled : false,
                '自動微調：定期檢查選字紀錄，每次最多調一格「評幾句／門檻」；偵測到修正率上升會自動回復（每週執行，需要 100 筆以上紀錄）',
                (v) => this.toggleAuto(ctx, v)),
              h('span', { class: 'muted grow' }),
              button(autoRunning ? '檢查中…' : '立即檢查', () => this.startAuto(ctx), { disabled: autoRunning || tuning }),
              button('回復上次', () => this.revert(ctx))),
            h('div', { class: 'card-desc status-text' },
              autoText || autoCfg ? lines([autoText, this.autoStatusLine()].filter(Boolean).join('\n')) : '讀取中…'),
          ],
        }),
        card({
          title: '學習式排序器',
          desc: '用你的選字紀錄訓練一個小排序模型（線性或 GBDT，取代上面手調的公式）：'
            + '每句候選用「模型分數、名次、個人詞庫、現行公式分數」等特徵重打分數。'
            + '訓練時最後 20% 紀錄留作驗證——贏過目前設定且錯改沒變多才啟用，不夠好就維持公式。'
            + '需要 50 筆以上有整句候選的紀錄；訓練期間輸入法會暫時放掉模型。',
          below: [
            h('div', { class: 'row' },
              button(rankRunning ? '訓練中…' : '訓練排序器', () => this.startRank(ctx),
                { disabled: rankRunning || tuning || autoRunning }),
              h('span', { class: 'muted grow' }, rankText)),
            h('div', { class: 'card-desc status-text' }, lines(this.rankStatusLine())),
          ],
        }),
        card({
          title: '信心校準',
          desc: '推薦與校正各自記下「比原句通順多少」和你有沒有採用（只有數字，不含打字內容），擬合出採用機率。'
            + '累積 10 筆後，候選窗會標示機率（例如「推薦 82%」），推薦與校正依機率排先後，低於門檻的不顯示。',
          below: [
            h('div', { class: 'row' }, h('span', {}, '低於'),
              input(c.min_confidence, (v) => { c.min_confidence = v; ctx.markDirty('llm'); }, { width: '80px' }),
              h('span', { class: 'muted' }, '就不顯示（0～0.9，預設 0.5；0 = 都顯示）')),
            h('div', { class: 'card-desc status-text' },
              this.calibration && this.calibration.length ? lines(this.calibration.join('\n'))
                : this.calibration ? '還沒有樣本：出現推薦或校正後，送出或按 Tab 時會記錄。' : '讀取中…'),
          ],
        })),
      section('選字統計',
        card({
          title: '選字統計',
          desc: '只記錄次數，不記錄打字內容。每個版本與設定組合分開統計。',
          control: [
            select(kSpans, span, (v) => {
              span = Number(v);
              selectedRow = 0;
              this.refresh(ctx);
            }, { width: '140px' }),
            button('重設統計', async () => {
              if (!(await confirm('要清除所有選字統計嗎？', { title: '選字統計', danger: true, ok: '清除' }))) return;
              await ctx.call('stats.reset');
              ctx.status('已清除選字統計。', 'ok');
              this.refresh(ctx);
            }),
          ],
          below: [h('div', { class: 'table-wrap' }, stats.length ? table : h('div', { class: 'list-empty' }, this.stats ? '這段期間還沒有紀錄。' : '讀取中…')),
            h('div', { class: 'spacer' }), stats.length ? detail : null],
        })),
      section('選字紀錄',
        card({
          title: checkbox(c.log, '記錄選字過程（加密，只存在本機），用來分析哪些字常選錯、訓練專屬的選字模型', (v) => {
            c.log = v;
            ctx.markDirty('llm');
          }),
          below: h('div', { class: 'row' },
            h('span', { class: 'muted grow' }, logStatus),
            button('清除紀錄', async () => {
              if (!(await confirm('要刪除所有選字紀錄嗎？之後訓練選字模型會少了這些資料。', { title: '選字紀錄', danger: true, ok: '刪除' }))) return;
              try {
                await ctx.call('choicelog.clear');
                ctx.status('已刪除選字紀錄。', 'ok');
              } catch (e) {
                ctx.status(e.message, 'error');
              }
              this.refresh(ctx);
            }, { disabled: !log || !log.records })),
        })),
    ];
  },
};
