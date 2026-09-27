import { h, section, card, button, lines } from '../ui.js';

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
    ];
  },
};
