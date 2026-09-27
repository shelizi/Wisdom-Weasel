// 候選視窗字型字串（style/font_face 等）的解析與組合，格式與輸入法（WeaselUI）相同：
//   字型[:粗細][:樣式][:起點[:終點]], 字型2[:起點[:終點]], …
// - 逗號分隔多個字型；每個字型可限定 Unicode 範圍（十六進位，預設 0 ~ 10ffff），
//   範圍內的字依序用第一個符合的字型，都不符合時用系統的後備字型。
// - 粗細、樣式對整個字串生效，慣例放在第一個字型名稱後面。

// 順序與輸入法的比對規則相同（較長的名稱在前面的不能被較短的吃掉）
export const WEIGHTS = ['thin', 'extra_light', 'ultra_light', 'light', 'semi_light', 'medium', 'demi_bold',
  'semi_bold', 'bold', 'extra_bold', 'ultra_bold', 'black', 'heavy', 'extra_black', 'ultra_black'];
export const STYLES = ['italic', 'oblique', 'normal'];

export const WEIGHT_LABELS = {
  thin: '極細 thin', extra_light: '特細 extra_light', ultra_light: '特細 ultra_light', light: '細 light',
  semi_light: '半細 semi_light', regular: '標準 regular', medium: '中等 medium', demi_bold: '半粗 demi_bold',
  semi_bold: '半粗 semi_bold', bold: '粗 bold', extra_bold: '特粗 extra_bold', ultra_bold: '特粗 ultra_bold',
  black: '極粗 black', heavy: '極粗 heavy', extra_black: '超粗 extra_black', ultra_black: '超粗 ultra_black',
};
export const STYLE_LABELS = { normal: '正常 normal', italic: '斜體 italic', oblique: '傾斜 oblique' };

// CSS 的 font-weight（預覽用）
const WEIGHT_CSS = {
  thin: 100, extra_light: 200, ultra_light: 200, light: 300, semi_light: 350, regular: 400, medium: 500,
  demi_bold: 600, semi_bold: 600, bold: 700, extra_bold: 800, ultra_bold: 800, black: 900, heavy: 900,
  extra_black: 950, ultra_black: 950,
};

const tokenPattern = (words) => new RegExp('\\s*:\\s*(' + words.join('|') + ')(?![\\w])', 'gi');
const WEIGHT_RE = () => tokenPattern(WEIGHTS);
const STYLE_RE = () => tokenPattern(STYLES);

// 去掉逗號、冒號前後與首尾的空白，以及結尾多的逗號
export function normalize(face) {
  return String(face || '').replace(/,\s*$/, '').replace(/\s*(,|:|^|$)\s*/g, '$1');
}

// 字串裡的粗細或樣式（有多個時以最後一個為準）；沒有時為 regular / normal
export function getToken(face, kind) {
  let value = null;
  for (const m of String(face || '').matchAll(kind === 'weight' ? WEIGHT_RE() : STYLE_RE())) value = m[1].toLowerCase();
  return value || (kind === 'weight' ? 'regular' : 'normal');
}

// 設定粗細或樣式：先拿掉原本的，不是預設值時加在第一個字型名稱後面
export function setToken(face, kind, value) {
  let s = String(face || '').replace(kind === 'weight' ? WEIGHT_RE() : STYLE_RE(), '');
  const fallback = kind === 'weight' ? 'regular' : 'normal';
  if (value && value !== fallback)
    s = s.replace(/^(\s*[^:,]*[^:,\s])(\s*:\s*)?/, (m, name, sep) => name + ':' + value + (sep ? ':' : ''));
  return s;
}

// 在字串後面加一個字型（與原本字型視窗的「加入」相同）；range 為 null 時不限範圍
export function addFont(face, name, range) {
  let item = String(name || '').trim();
  if (!item) return face;
  if (range) {
    const start = String(range.start || '').replace(/^0+$/, '0');
    const end = String(range.end || '').replace(/^0+$/, '0');
    if (start === '0' || start === '') {
      if (end) item += '::' + end;
    } else {
      item += ':' + start;
      if (end) item += ':' + end;
    }
  }
  const s = String(face || '').trim();
  return (s ? s + ', ' : '') + item;
}

export const isHex = (s) => /^[0-9a-fA-F]+$/.test(String(s || ''));

// 拆成預覽用的清單：[{name, first, last}]（first、last 是數字），以及整體的粗細與樣式
export function parse(face) {
  const s = normalize(face);
  const weight = getToken(s, 'weight');
  const style = getToken(s, 'style');
  const hex = (text, fallback) => {
    const n = parseInt(text, 16);
    return text && isHex(text) && !Number.isNaN(n) ? n : fallback;
  };
  const fonts = [];
  s.split(',').forEach((item, i) => {
    // 粗細、樣式的標記只會在第一個字型上
    const clean = i === 0 ? item.replace(WEIGHT_RE(), '').replace(STYLE_RE(), '') : item;
    const parts = clean.split(':');
    if (!parts[0] || parts.length > 3) return;
    fonts.push({
      name: parts[0],
      first: parts.length >= 2 ? hex(parts[1], 0) : 0,
      last: parts.length === 3 ? hex(parts[2], 0x10ffff) : 0x10ffff,
    });
  });
  return { fonts, weight, style };
}

// 簡單模式：字串只有一個不限範圍的字型時回傳它的名稱（沒有設定時為 ''）；
// 用了多個字型或限定範圍時回傳 null（只能在進階設定編輯）
export function simpleFont(face) {
  if (!normalize(face)) return '';
  const { fonts } = parse(face);
  if (normalize(face).split(',').length !== 1 || fonts.length !== 1) return null;
  const f = fonts[0];
  return f.first === 0 && f.last === 0x10ffff ? f.name : null;
}

// 簡單模式換字型：保留原本的粗細與樣式；name 為 '' 時用預設字型
export function withFont(face, name) {
  if (!name) return '';
  // 都插在名稱後面：先放樣式再放粗細，結果是 名稱:粗細:樣式
  return setToken(setToken(name, 'style', getToken(face, 'style')), 'weight', getToken(face, 'weight'));
}

// 預覽的 CSS：每個字型一條 @font-face（local() + unicode-range），family 依序排；
// 後面再接原本的字型名稱，local() 找不到時也看得到字型
export function previewCss(face, family) {
  const { fonts, weight, style } = parse(face);
  const quote = (s) => '"' + String(s).replace(/["\\]/g, '\\$&') + '"';
  const rules = fonts.map((f, i) =>
    `@font-face { font-family: ${quote(family + '-' + i)}; src: local(${quote(f.name)}); ` +
    `unicode-range: U+${f.first.toString(16)}-${f.last.toString(16)}; }`);
  const families = [...fonts.map((f, i) => quote(family + '-' + i)), ...fonts.map((f) => quote(f.name)), 'sans-serif'];
  return {
    rules: rules.join('\n'),
    fontFamily: families.join(', '),
    fontWeight: WEIGHT_CSS[weight] || 400,
    fontStyle: style === 'normal' ? 'normal' : style,
  };
}
