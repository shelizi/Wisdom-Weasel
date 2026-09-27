// 外觀頁的字體區塊。預設是簡單模式：三種文字各選一個字型與大小；
// 多個字型、Unicode 範圍、粗細與樣式收在「進階設定」裡。
import { h, card, select, input, button, checkbox } from '../ui.js';
import {
  WEIGHTS, STYLES, WEIGHT_LABELS, STYLE_LABELS, getToken, setToken, addFont, isHex, previewCss, simpleFont, withFont,
} from '../fontface.js';

// 三種文字：字型與大小的設定鍵
const FIELDS = [
  { face: 'font_face', point: 'font_point', label: '候選字' },
  { face: 'label_font_face', point: 'label_font_point', label: '標籤' },
  { face: 'comment_font_face', point: 'comment_font_point', label: '註解' },
];
let field = FIELDS[0];  // 進階設定正在編輯的
let advancedOpen = false;
let pick = '';
let range = { on: false, start: '0', end: '10ffff' };
let sample = '小狼毫輸入法';
let zoom = 100;

function fontSummary(f) {
  const describe = (face, point) => (face ? face.split(',')[0].split(':')[0] : '預設字體') + '，' + point + ' pt';
  return '候選字：' + describe(f.font_face, f.font_point) + '　標籤：' + f.label_font_point +
    ' pt　註解：' + f.comment_font_point + ' pt';
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

function pointInput(value, onchange) {
  const el = input(String(value), (v) => {
    const n = parseInt(v, 10);
    if (!Number.isNaN(n) && n >= 0 && n <= 99) onchange(n);
  }, { type: 'number', width: '80px' });
  el.min = 0;
  el.max = 99;
  return el;
}

// systemFonts：系統字型清單（還沒讀到時為 null）
export function fontEditor(ctx, systemFonts) {
  const fonts = ctx.state.style.fonts;
  const summary = h('div', { class: 'card-desc' }, fontSummary(fonts));
  const previewBox = h('div', { class: 'font-preview-box' }, fontPreview(fonts));
  const simpleBox = h('div', { class: 'font-simple' });
  const advancedBox = h('div');
  const changed = () => {
    ctx.markDirty('style');
    summary.textContent = fontSummary(fonts);
    previewBox.replaceChildren(fontPreview(fonts));
  };

  // 簡單模式：每種文字一列（字型、大小）
  const renderSimple = () => {
    simpleBox.replaceChildren(...FIELDS.map((f) => {
      const current = simpleFont(fonts[f.face]);
      const options = [];
      if (current === null) options.push({ value: '__custom__', label: '自訂（見進階設定）' });
      options.push({ value: '', label: '預設字體' });
      const names = [...(systemFonts || [])];
      // 設定裡的名稱不在清單上（例如英文名稱）時也列出來
      if (current && !names.includes(current)) names.unshift(current);
      names.forEach((name) => options.push({ value: name, label: name }));
      const fontSelect = select(options, current === null ? '__custom__' : current, (v) => {
        if (v === '__custom__') return;  // 維持原本的自訂字串
        fonts[f.face] = withFont(fonts[f.face], v);
        changed();
        renderSimple();
        renderAdvanced();
      }, { width: '100%' });
      return h('div', { class: 'font-simple-row' },
        h('span', { class: 'font-simple-label' }, f.label),
        h('div', { class: 'grow' }, fontSelect),
        pointInput(fonts[f.point], (n) => {
          fonts[f.point] = n;
          changed();
          if (f === field && advancedPoint) advancedPoint.value = String(n);
        }),
        h('span', { class: 'muted' }, 'pt'));
    }));
  };

  // 進階設定：直接編輯字串、粗細與樣式、加入限定範圍的字型
  let advancedPoint = null;
  const renderAdvanced = () => {
    if (!advancedOpen) {
      advancedBox.replaceChildren();
      advancedPoint = null;
      return;
    }
    const faceInput = input(fonts[field.face], (v) => {
      fonts[field.face] = v;
      weightSelect.value = getToken(v, 'weight');
      styleSelect.value = getToken(v, 'style');
      changed();
      renderSimple();
    }, { width: '100%', placeholder: '字型名稱，可用逗號串接多個（例如 Segoe UI:0:7f, Microsoft JhengHei）' });
    const setFace = (v) => {
      fonts[field.face] = v;
      faceInput.value = v;
      changed();
      renderSimple();
    };
    const weightSelect = select(['regular', ...WEIGHTS].map((w) => ({ value: w, label: WEIGHT_LABELS[w] })),
      getToken(fonts[field.face], 'weight'), (v) => setFace(setToken(fonts[field.face], 'weight', v)), { width: '170px' });
    const styleSelect = select(STYLES.slice().sort().map((s) => ({ value: s, label: STYLE_LABELS[s] })),
      getToken(fonts[field.face], 'style'), (v) => setFace(setToken(fonts[field.face], 'style', v)), { width: '150px' });
    advancedPoint = pointInput(fonts[field.point], (n) => {
      fonts[field.point] = n;
      changed();
      renderSimple();
    });

    const datalist = h('datalist', { id: 'system-fonts' }, (systemFonts || []).map((name) => h('option', { value: name })));
    const pickInput = input(pick, (v) => { pick = v; }, { width: '240px', placeholder: systemFonts ? '搜尋系統字型…' : '讀取字型清單中…' });
    pickInput.setAttribute('list', 'system-fonts');
    const hexInput = (key, fallback) => {
      const el = input(range[key], (v) => { range[key] = v; }, { width: '90px', disabled: !range.on });
      // 不是十六進位時還原成預設值（與原本字型視窗相同）
      el.addEventListener('blur', () => {
        if (!isHex(el.value)) el.value = range[key] = fallback;
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
          renderAdvanced();
        },
      }, f.label)));

    advancedBox.replaceChildren(h('div', { class: 'font-advanced' },
      tabs,
      h('div', { class: 'spacer' }),
      h('div', { class: 'field' }, h('div', { class: 'field-label' }, field.label + '的字型'), faceInput),
      h('div', { class: 'row' },
        h('span', { class: 'field-label' }, '大小（pt）'), advancedPoint,
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
          range = { on: false, start: '0', end: '10ffff' };
          renderAdvanced();
        })),
      h('div', { class: 'note' }, '前面的字型優先；限定範圍的字型只用在範圍內的字（十六進位，例如 4e00 ～ 9fff 是常用漢字）。粗細與樣式套用到整個字串。')));
  };

  const advancedToggle = button(advancedOpen ? '隱藏進階設定' : '進階設定…', () => {
    advancedOpen = !advancedOpen;
    advancedToggle.textContent = advancedOpen ? '隱藏進階設定' : '進階設定…';
    renderAdvanced();
  });

  renderSimple();
  renderAdvanced();

  return card({
    title: '候選視窗的字體',
    control: summary,
    below: [
      simpleBox,
      h('div', { class: 'row', style: { marginTop: '10px' } },
        h('span', { class: 'field-label' }, '預覽'),
        input(sample, (v) => {
          sample = v;
          previewBox.replaceChildren(fontPreview(fonts));
        }, { width: '220px' }),
        select([100, 125, 150, 200].map((z) => ({ value: z, label: z + '%' })), zoom, (v) => {
          zoom = Number(v);
          previewBox.replaceChildren(fontPreview(fonts));
        }, { width: '90px' }),
        h('span', { class: 'grow' }),
        advancedToggle),
      previewBox,
      advancedBox,
    ],
  });
}
