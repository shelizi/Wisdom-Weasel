import { h, section, card, button, lines, toggle, select, input } from '../ui.js';
import { profileInfo } from './profiles.js';
import { can } from '../state.js';

const kConversions = [
  { value: 'tw2s', label: '只換字（例如 裡 → 里）' },
  { value: 'tw2sp', label: '連兩岸用詞一起換（例如 軟體 → 软件）' },
  { value: 'llm', label: '用語言模型在地化（依整句改用大陸的說法）' },
];
const kHotkeys = [
  { value: 'ctrl+shift+f', label: 'Ctrl + Shift + F' },
  { value: 'ctrl+shift+j', label: 'Ctrl + Shift + J' },
  { value: 'ctrl+alt+f', label: 'Ctrl + Alt + F' },
  { value: 'none', label: '不用快速鍵' },
];

// 簡體輸出（llm/simplified/*）：打字時照常是繁體，送出時才轉成簡體
function simplifiedSection(ctx) {
  const f = ctx.state.form.simplified;
  const changed = () => ctx.markDirty('llm');
  const known = kHotkeys.some((k) => k.value === f.hotkey);
  return section('簡體輸出',
    card({
      title: '送出時轉成簡體',
      desc: '組字與候選照常顯示繁體（智慧預測、推薦與個人詞庫都用繁體），送進應用程式的文字才轉成簡體（OpenCC）。'
        + (can('simplified_menu')
          ? '這裡是預設；打字時可以用快速鍵或系統匣選單的「簡體輸出」隨時切換，到下次套用設定或重新部署為止。'
          : '這裡是預設；打字時可以用快速鍵隨時切換，到下次套用設定或重新部署為止。'),
      control: toggle(f.enabled, (v) => { f.enabled = v; changed(); }),
    }),
    card({
      title: '轉換方式',
      desc: '連用詞一起換偶爾會換錯（例如專有名詞），需要時再打開。'
        + (can('commit_wait')
          ? '用語言模型在地化時，打字停頓就先在背景改寫目前的句子，送出時直接用；還沒改好就等一下，等不到改用「連用詞一起換」。'
          : '用語言模型在地化時，打字停頓就先在背景改寫目前的句子，送出時直接用；還沒改好就改用「連用詞一起換」（送出時不等，其他應用程式才不會跟著停住）。'),
      control: select(kConversions, f.config, (v) => { f.config = v; changed(); ctx.rerender(); }, { width: '360px' }),
    }),
    f.config === 'llm' ? card({
      title: '在地化使用的模型',
      desc: '在「語言模型」頁的「各功能使用的模型」選擇。',
      control: profileInfo(ctx, 'localize'),
      below: !can('commit_wait') ? null : h('div', { class: 'row' }, h('span', {}, '送出時最多等'),
        input(f.wait_ms, (v) => { f.wait_ms = v; changed(); }, { width: '80px' }),
        h('span', { class: 'muted' }, '毫秒（0～5000，預設 1000）；等的時候只有這個應用程式暫停，其他應用程式照常')),
    }) : null,
    card({
      title: '切換的快速鍵',
      desc: '在任何應用程式裡按下就切換繁簡輸出（會蓋過應用程式自己的快速鍵，例如 VS Code 的 Ctrl + Shift + F「在檔案中搜尋」）。',
      control: select(known ? kHotkeys : [...kHotkeys, { value: f.hotkey, label: f.hotkey }], f.hotkey,
        (v) => { f.hotkey = v; changed(); }, { width: '200px' }),
    }));
}

let focused = null;  // 目前查看簡介的方案
const details = new Map();

async function showDetails(ctx, id, box) {
  focused = id;
  if (!details.has(id)) {
    try {
      details.set(id, await ctx.call('schemas.details', { id }));
    } catch (e) {
      details.set(id, { name: id, description: e.message });
    }
  }
  if (focused !== id) return;
  const d = details.get(id) || {};
  box.replaceChildren(
    h('div', { class: 'card-title' }, d.name || id),
    d.author ? h('div', { class: 'muted' }, lines(d.author)) : null,
    h('div', { class: 'spacer' }),
    h('div', { class: 'status-text' }, d.description || '（沒有簡介）'));
}

export default {
  id: 'schemas',
  nav: '輸入方案',
  icon: '',
  title: '輸入方案',
  desc: '選擇要使用的輸入方案，並查看各方案的說明。',

  render(ctx) {
    const { state } = ctx;
    const box = h('div', { class: 'card schema-details' });
    const rows = state.schemas.map((schema) => {
      const input = h('input', {
        type: 'checkbox', checked: schema.selected, 'aria-label': schema.name,
        onchange: () => {
          schema.selected = input.checked;
          ctx.markDirty('schemas');
        },
      });
      const row = h('div', { class: 'list-item' + (schema.id === focused ? ' selected' : '') },
        h('label', { class: 'check' }, input), h('span', { class: 'list-label' }, schema.name),
        h('span', { class: 'list-detail' }, schema.id));
      row.addEventListener('click', (e) => {
        if (e.target === input) return;
        list.querySelectorAll('.list-item').forEach((r) => r.classList.remove('selected'));
        row.classList.add('selected');
        showDetails(ctx, schema.id, box);
      });
      return row;
    });
    const list = h('div', { class: 'listbox', style: { height: '340px' } }, rows);
    if (!focused && state.schemas.length) focused = state.schemas[0].id;
    if (focused) {
      rows[state.schemas.findIndex((s) => s.id === focused)]?.classList.add('selected');
      showDetails(ctx, focused, box);
    }

    return [
      section('方案',
        h('p', { class: 'muted' }, '勾選要使用的輸入方案；點選項目可查看方案簡介。'),
        h('div', { class: 'grid two' }, list, box)),
      section('切換方案',
        card({
          title: '方案選單',
          desc: '輸入時按下列快速鍵，可叫出方案選單以切換輸入方案或模式。',
          control: h('code', { class: 'hotkeys' }, state.init.schemas.hotkeys || '—'),
        }),
        card({
          title: '取得更多輸入方案',
          desc: '執行 Rime 的方案安裝程式（東風破）；完成後會重新讀取方案清單。',
          control: button('取得更多輸入方案…', async (e) => {
            e.target.disabled = true;
            ctx.status('方案安裝程式執行中，完成後關閉命令視窗即可。', 'busy');
            try {
              const result = await ctx.call('schemas.install');
              const selected = new Set(state.schemas.filter((s) => s.selected).map((s) => s.id));
              state.schemas = result.list.map((s) => ({ ...s, selected: selected.has(s.id) || s.selected }));
              state.init.schemas.hotkeys = result.hotkeys;
              details.clear();
              ctx.status('已重新讀取方案清單。', 'ok');
              ctx.rerender();
            } catch (err) {
              ctx.status(err.message, 'error');
            } finally {
              e.target.disabled = false;
            }
          }),
        })),
      simplifiedSection(ctx),
    ];
  },
};
