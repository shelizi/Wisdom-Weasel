import { h, section, card, listbox, select, input, button, checkbox } from '../ui.js';
import { WEIGHTS, STYLES, WEIGHT_LABELS, STYLE_LABELS, getToken, setToken, addFont, isHex, previewCss } from '../fontface.js';

const previews = new Map();
let systemFonts = null;  // 系統字型清單（第一次顯示時讀取）

// 三種文字：字型與大小的設定鍵
const FIELDS = [
  { face: 'font_face', point: 'font_point', label: '候選字' },
  { face: 'label_font_face', point: 'label_font_point', label: '標籤' },
  { face: 'comment_font_face', point: 'comment_font_point', label: '註解' },
];
let field = FIELDS[0];
let pick = '';
let range = { on: false, start: '0', end: '10ffff' };
let sample = '小狼毫輸入法';
let zoom = 100;

function fontSummary(f) {
  const describe = (face, point) => (face ? face.split(',')[0].split(':')[0] : '預設字體') + '，' + point + ' pt';
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

// 模擬候選視窗：標籤、候選字、註解各用自己的字型與大小
function fontPreview(fonts) {
  const css = {};
  const rules = FIELDS.map((f, i) => {
    css[f.face] = previewCss(fonts[f.face], 'wwp' + i);
    return css[f.face].rules;
  }).join('\n');
  const span = (f, text, cls) => {
    const c = css[f.face];
    const point = Number(fonts[f.point]) || 0;
    return h('span', {
      class: cls,
      style: {
        fontFamily: c.fontFamily, fontWeight: c.fontWeight, fontStyle: c.fontStyle,
        fontSize: point + 'pt', display: point > 0 ? '' : 'none',
      },
    }, text);
  };
  const rows = [[sample || '小狼毫輸入法', 'xiao lang hao'], ['輸入', 'shu ru'], ['Weasel 123', '~']];
  return h('div', { class: 'font-preview', style: { zoom: zoom / 100 } },
    h('style', {}, rules),
    rows.map(([text, comment], i) => h('div', { class: 'fp-row' + (i === 0 ? ' fp-first' : '') },
      span(FIELDS[1], (i + 1) + '.', 'fp-label'), span(FIELDS[0], text, 'fp-text'), span(FIELDS[2], comment, 'fp-comment'))));
}

function fontEditor(ctx) {
  const { state } = ctx;
  const fonts = state.style.fonts;
  const changed = () => {
    ctx.markDirty('style');
    summary.textContent = fontSummary(fonts);
    previewBox.replaceChildren(fontPreview(fonts));
  };
  const summary = h('div', { class: 'card-desc' }, fontSummary(fonts));
  const previewBox = h('div', { class: 'font-preview-box' }, fontPreview(fonts));

  const faceInput = input(fonts[field.face], (v) => {
    fonts[field.face] = v;
    weightSelect.value = getToken(v, 'weight');
    styleSelect.value = getToken(v, 'style');
    changed();
  }, { width: '100%', placeholder: '字型名稱，可用逗號串接多個（例如 Segoe UI:0:7f, Microsoft JhengHei）' });
  const setFace = (v) => {
    fonts[field.face] = v;
    faceInput.value = v;
    changed();
  };
  const weightSelect = select(['regular', ...WEIGHTS].map((w) => ({ value: w, label: WEIGHT_LABELS[w] })),
    getToken(fonts[field.face], 'weight'), (v) => setFace(setToken(fonts[field.face], 'weight', v)), { width: '170px' });
  const styleSelect = select(STYLES.slice().sort().map((s) => ({ value: s, label: STYLE_LABELS[s] })),
    getToken(fonts[field.face], 'style'), (v) => setFace(setToken(fonts[field.face], 'style', v)), { width: '150px' });
  const pointInput = input(String(fonts[field.point]), (v) => {
    const n = parseInt(v, 10);
    if (!Number.isNaN(n) && n >= 0 && n <= 99) {
      fonts[field.point] = n;
      changed();
    }
  }, { type: 'number', width: '80px' });
  pointInput.min = 0;
  pointInput.max = 99;

  // 加入字型：從系統字型挑一個，可限定 Unicode 範圍
  const datalist = h('datalist', { id: 'system-fonts' }, (systemFonts || []).map((name) => h('option', { value: name })));
  const pickInput = input(pick, (v) => { pick = v; }, { width: '240px', placeholder: systemFonts ? '搜尋系統字型…' : '讀取字型清單中…' });
  pickInput.setAttribute('list', 'system-fonts');
  const hexInput = (key, fallback) => {
    const el = input(range[key], (v) => { range[key] = v; }, { width: '90px', disabled: !range.on });
    // 不是十六進位時還原成預設值（與原本字型視窗相同）
    el.addEventListener('blur', () => {
      if (!isHex(el.value)) {
        el.value = range[key] = fallback;
      }
    });
    return el;
  };
  const startInput = hexInput('start', '0');
  const endInput = hexInput('end', '10ffff');
  const rangeCheck = checkbox(range.on, '限定 Unicode 範圍', (v) => {
    range.on = v;
    startInput.disabled = endInput.disabled = !v;
  });

  const tabs = h('div', { class: 'segmented' }, FIELDS.map((f) =>
    h('button', {
      class: 'segment' + (f === field ? ' active' : ''),
      onclick: () => {
        field = f;
        ctx.rerender();
      },
    }, f.label)));

  return card({
    title: '候選視窗的字體',
    control: summary,
    below: [
      tabs,
      h('div', { class: 'spacer' }),
      h('div', { class: 'field' }, h('div', { class: 'field-label' }, field.label + '的字型'), faceInput),
      h('div', { class: 'row' },
        h('span', { class: 'field-label' }, '大小（pt）'), pointInput,
        h('span', { class: 'field-label' }, '粗細'), weightSelect,
        h('span', { class: 'field-label' }, '樣式'), styleSelect),
      h('div', { class: 'spacer' }),
      h('div', { class: 'row' },
        h('span', { class: 'field-label' }, '加入字型'), pickInput, datalist, rangeCheck, startInput,
        h('span', { class: 'muted' }, '～'), endInput,
        button('加入', () => {
          const name = pick.trim();
          if (!name) {
            ctx.status('請先選擇要加入的字型。', 'error');
            return;
          }
          setFace(addFont(fonts[field.face], name, range.on ? { start: range.start, end: range.end } : null));
          pick = '';
          pickInput.value = '';
          range = { on: false, start: '0', end: '10ffff' };
          rangeCheck.querySelector('input').checked = false;
          startInput.value = '0';
          endInput.value = '10ffff';
          startInput.disabled = endInput.disabled = true;
        })),
      h('div', { class: 'note' }, '前面的字型優先；限定範圍的字型只用在範圍內的字（十六進位，例如 4e00 ～ 9fff 是常用漢字）。粗細與樣式套用到整個字串。'),
      h('div', { class: 'spacer' }),
      h('div', { class: 'row' },
        h('span', { class: 'field-label' }, '預覽'),
        input(sample, (v) => {
          sample = v;
          previewBox.replaceChildren(fontPreview(fonts));
        }, { width: '240px' }),
        select([100, 125, 150, 200].map((z) => ({ value: z, label: z + '%' })), zoom, (v) => {
          zoom = Number(v);
          previewBox.replaceChildren(fontPreview(fonts));
        }, { width: '90px' })),
      previewBox,
    ],
  });
}

export default {
  id: 'style',
  nav: '外觀',
  icon: '',
  title: '外觀',
  desc: '候選視窗的配色與字體。',

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
      section('字體', fontEditor(ctx)),
    ];
  },
};
