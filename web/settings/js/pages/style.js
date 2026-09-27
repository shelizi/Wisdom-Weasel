import { h, section, card, button, listbox } from '../ui.js';

const previews = new Map();

function fontSummary(f) {
  const describe = (face, point) => (face ? face.split(',')[0] : '預設字體') + '，' + point + ' pt';
  return '候選字：' + describe(f.font_face, f.font_point) + '　標籤：' + f.label_font_point +
    ' pt　註解：' + f.comment_font_point + ' pt';
}

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
  desc: '候選視窗的配色與字體。',

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
      section('字體',
        card({
          title: '候選視窗的字體',
          desc: fontSummary(state.style.fonts),
          control: button('調整字體…', async () => {
            try {
              const fonts = await ctx.call('fonts.pick', state.style.fonts);
              if (!fonts) return;
              state.style.fonts = fonts;
              ctx.markDirty('style');
              ctx.rerender();
            } catch (e) {
              ctx.status(e.message, 'error');
            }
          }),
        })),
    ];
  },
};
