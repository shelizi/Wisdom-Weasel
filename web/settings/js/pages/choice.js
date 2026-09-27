import { h, section, card, checkbox, button, select, note, confirm, lines } from '../ui.js';

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
      const [stats, log, grammar] = await Promise.all([
        ctx.call('stats.get', { span }), ctx.call('choicelog.status'), ctx.call('grammar.status')]);
      this.stats = stats;
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
    const columns = ['版本', '設定', '送出', '第一候選', '送出後刪', '換字', '推薦套用', 'LLM 採用'];
    const table = h('table', { class: 'table' },
      h('thead', {}, h('tr', {}, columns.map((c) => h('th', {}, c)))),
      h('tbody', {}, stats.map((r, i) => {
        const tr = h('tr', { class: i === selectedRow ? 'selected' : '', onclick: () => {
          selectedRow = i;
          table.querySelectorAll('tbody tr').forEach((row, j) => row.classList.toggle('selected', j === i));
          detail.replaceChildren(...lines(r.detail));
        } }, [r.version, r.settings, r.commits, r.first_ok, r.deleted, r.changed, r.recommended, r.llm].map((v) => h('td', {}, v)));
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
