import { h, section, card, checkbox, button, select, input, note, confirm, lines } from '../ui.js';
import { profileSelect } from './profiles.js';

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
    }
    this.refresh(ctx);
  },

  async refresh(ctx) {
    try {
      const [stats, log, grammar, calibration] = await Promise.all([
        ctx.call('stats.get', { span }), ctx.call('choicelog.status'), ctx.call('grammar.status'),
        ctx.call('calibration.get')]);
      this.stats = stats;
      this.calibration = calibration;
      this.log = log;
      ctx.state.init.grammar = grammar;
      downloading = grammar.downloading || downloading;
      if (ctx.isCurrent(this)) ctx.rerender();
    } catch (e) {
      ctx.status(e.message, 'error');
    }
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
          desc: '打字停頓時，比較整句裡同音字的通順度。使用智慧預測或注音校正已載入的本機模型（API 模型不支援）。',
        }),
        card({
          title: '整句重排',
          desc: 'Rime 先給出符合注音的前 10 個整句，本機模型依前文挑最通順的一句；比 Rime 第一句好超過門檻才推薦（Tab 套用）。'
            + '候選一律來自 Rime，模型只負責排序。建議先選「只統計」用一陣子，在下面的選字統計看「整句重排」欄（改對／會改）再決定要不要顯示；'
            + '顯示時取代上面的同音字推薦。開關時會重新部署，注音方案的候選窗也會多出幾個整句候選。',
          control: select(kRerankModes, c.rerank, (v) => {
            c.rerank = v;
            ctx.markDirty('llm');
            ctx.rerender();
          }, { width: '240px' }),
          below: [
            h('div', { class: 'row' }, h('span', { class: c.rerank === 'off' ? 'muted' : '' }, '評分模型'),
              profileSelect(ctx, 'scorer', { disabled: c.rerank === 'off' && !c.rescore })),
            h('div', { class: 'note' }, '建議用小的 Base 模型（例如 Qwen3-0.6B-Base）：在開放繁中測試集上，Top-1 從約 57～64% 提升到 70～74%，改錯不到 2%。'
              + '沒選時借用智慧預測或注音校正已載入的本機模型。'),
            h('div', { class: 'row' }, h('span', { class: c.rerank === 'off' ? 'muted' : '' }, '門檻'),
              input(c.rerank_margin, (v) => { c.rerank_margin = v; ctx.markDirty('llm'); },
                { width: '80px', disabled: c.rerank === 'off' }),
              h('span', { class: 'muted' }, '（整句 log 機率，預設 2；越大越保守）')),
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
