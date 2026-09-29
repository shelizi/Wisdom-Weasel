import { h, section, card, toggle, listbox } from '../ui.js';
import { fontEditor } from './fonteditor.js';

const previews = new Map();
let systemFonts = null;  // 系統字型清單（第一次顯示時讀取）

async function loadPreview(ctx, id, img) {
  if (!previews.has(id)) {
    try {
      previews.set(id, await ctx.call('style.preview', { id }));
    } catch {
      previews.set(id, null);
    }
  }
  const url = previews.get(id);
  if (img.dataset.scheme !== id) return;
  img.hidden = !url;
  if (url) img.src = url;
}

export default {
  id: 'style',
  nav: '外觀',
  icon: '',
  title: '外觀',
  desc: '候選視窗的配色、字體與顯示的資訊。',

  async onShow(ctx) {
    if (systemFonts) return;
    try {
      systemFonts = await ctx.call('fonts.list');
    } catch {
      systemFonts = [];
    }
    if (ctx.isCurrent(this)) ctx.rerender();
  },

  render(ctx) {
    const { state } = ctx;
    const schemes = state.init.style.schemes;
    const img = h('img', { class: 'preview', alt: '配色預覽', hidden: true });
    const show = (id) => {
      img.dataset.scheme = id;
      loadPreview(ctx, id, img);
    };
    const list = listbox(
      schemes.map((s) => ({ key: s.id, label: s.name, detail: s.author })),
      state.style.active,
      (id) => {
        state.style.active = id;
        ctx.markDirty('style');
        show(id);
      },
      { height: '360px', empty: '沒有可用的配色。' });
    if (state.style.active) show(state.style.active);

    return [
      section('配色',
        h('div', { class: 'grid two' }, list, h('div', { class: 'card preview-box' }, img))),
      section('字體', fontEditor(ctx, systemFonts)),
      section('候選窗',
        card({
          title: '在整句後面顯示機率',
          desc: '打字停頓時，本機模型依前文比較 Rime 的前幾個整句，在候選窗的整句後面標示機率（例如「今天天氣很好 62%」，評過的整句加起來是 100%）。'
            + '用「選字策略」的評分模型（沒選時借用智慧預測或注音校正已載入的本機模型），沒有本機模型時不顯示。'
            + '要看到多個整句，請在「選字策略」打開「長句的整句候選」。',
          control: toggle(state.form.choice.show_probability, (v) => {
            state.form.choice.show_probability = v;
            ctx.markDirty('llm');
          }),
        })),
    ];
  },
};
