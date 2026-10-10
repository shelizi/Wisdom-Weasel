// 各功能選用的模型：選單集中在「語言模型」頁；其他頁只顯示目前用哪一組，按「更改…」過去改
import { h, select, button } from '../ui.js';
import { profileLabel } from '../state.js';

// 用到模型設定的功能（語言模型頁「各功能使用的模型」的順序）
export const kModelUses = [
  { which: 'predict', label: '智慧預測', page: '智慧預測', on: (f) => f.predict.enabled,
    desc: '本機或 API 都可以；Base 模型續寫預測通常較自然。' },
  { which: 'typo', label: 'LLM 整句校正', page: '注音校正', on: (f) => f.typo.llm,
    desc: '本機或 API 都可以。' },
  { which: 'scorer', label: '推薦、整句重排與整句機率的評分', page: '選字策略', on: (f) => f.choice.rescore || f.choice.rerank !== 'off' || f.choice.show_probability,
    desc: '只能用本機模型。建議小的 Base 模型（例如 Qwen3-0.6B-Base）：在開放繁中測試集上，整句重排的 Top-1 從約 57～64% 提升到 70～74%，改錯不到 2%。' },
  { which: 'localize', label: '簡體在地化', page: '輸入方案', on: (f) => f.simplified.config === 'llm',
    desc: '送出前把臺灣用語改成大陸簡體的說法。Instruct 或翻譯模型比較可靠；本機或 API 都可以，選 API 時送出的每一句都會傳到該服務。',
    warn: (p) => (p && !p.remote && p.model_type === 'Base' ? 'Base 模型不會照指示改寫，只能靠範例引導，容易多接句子或不改。' : '') },
  { which: 'refine', label: '個人詞庫精煉', page: '個人詞庫', on: (f) => f.personal.enabled,
    desc: '需要能照指示回答的 Instruct 模型。選「停用」時只做統計整理、不送出資料；選雲端 API 時，常用詞與部分例句會送到該服務。' },
];

// 目前選用的索引；不合用（超出範圍、評分選到 API）時是 -1
function selected(ctx, which) {
  const { profiles, use } = ctx.state;
  const i = use[which];
  return i >= 0 && i < profiles.length && (which !== 'scorer' || !profiles[i].remote) ? i : -1;
}

// 沒選時的說法：精煉是「停用」；評分是「借用已載入的本機模型」
function noneLabel(ctx, which) {
  if (which === 'refine') return '停用（只做統計整理）';
  if (which === 'scorer') return '借用智慧預測或注音校正的本機模型';
  return ctx.state.profiles.length ? '（請選擇模型）' : '（尚未設定模型）';
}

// 這個功能目前有沒有在用模型（跟模型設定本身無關）
function featureOn(ctx, which) {
  const { form } = ctx.state;
  if (which === 'predict') return form.predict.enabled;
  if (which === 'typo') return form.typo.llm;
  if (which === 'localize') return form.simplified.config === 'llm';
  if (which === 'refine') return selected(ctx, 'refine') >= 0;
  const c = form.choice;
  return c.rescore || c.rerank !== 'off' || c.show_probability;
}

// 選「停用」只關這個功能，模型設定留著。再選回一組模型就把功能打開。
function applyChoice(ctx, which, value) {
  const { form, use } = ctx.state;
  if (value === 'off') {
    if (which === 'predict') form.predict.enabled = false;
    else if (which === 'typo') form.typo.llm = false;
    else if (which === 'scorer') {
      form.choice.rescore = false;
      form.choice.rerank = 'off';
      form.choice.show_probability = false;
    } else if (which === 'localize') {
      if (form.simplified.config === 'llm') form.simplified.config = 'tw2s';
    } else if (which === 'refine') use.refine = -1;
    return;
  }
  const index = Number(value);
  use[which] = index;
  if (index < 0) return;
  if (which === 'predict') form.predict.enabled = true;
  else if (which === 'typo') form.typo.llm = true;
  else if (which === 'localize') form.simplified.config = 'llm';
  else if (which === 'scorer') {
    const c = form.choice;
    if (!c.rescore && c.rerank === 'off' && !c.show_probability) c.rescore = true;
  }
}

// 下拉選單（語言模型頁用）。「停用」關的是功能，不是模型。
export function profileSelect(ctx, which, { disabled } = {}) {
  const { profiles } = ctx.state;
  const options = [{ value: 'off', label: which === 'refine' ? '停用（只做統計整理）' : '停用' }];
  if (which === 'scorer') options.push({ value: '-1', label: noneLabel(ctx, which) });
  else if (which !== 'refine') options.push({ value: '-1', label: noneLabel(ctx, which) });
  profiles.forEach((p, i) => {
    if (which !== 'scorer' || !p.remote) options.push({ value: i, label: profileLabel(p) });
  });
  const picked = selected(ctx, which);
  const value = !featureOn(ctx, which) ? 'off' : (picked >= 0 ? picked : -1);
  return select(options, value, (v) => {
    applyChoice(ctx, which, v);
    ctx.markDirty('llm');
    ctx.rerender();
  }, { disabled, width: '320px' });
}

// 各功能頁：顯示目前用的模型，「更改…」到語言模型頁
export function profileInfo(ctx, which, { disabled } = {}) {
  const i = selected(ctx, which);
  const text = i >= 0 ? profileLabel(ctx.state.profiles[i]) : noneLabel(ctx, which);
  return [
    h('span', { class: i >= 0 || which === 'scorer' || which === 'refine' ? '' : 'muted' }, text),
    button('更改…', () => ctx.go('models', i >= 0 ? { profile: i } : {}), { disabled }),
  ];
}

// 這組設定被哪些功能使用（語言模型頁顯示）
export function profileUsage(ctx, index) {
  const { form, profiles } = ctx.state;
  if (index < 0) return '按「新增」建立一組模型設定。';
  const p = profiles[index];
  const uses = kModelUses.filter((u) => u.on(form) && selected(ctx, u.which) === index).map((u) => u.label);
  if (!uses.length) return '目前沒有被使用。可在上面的「各功能使用的模型」選用。';
  let text = '用於：' + uses.join('、');
  if (!p.remote && p.model_type === 'Base' && selected(ctx, 'refine') === index)
    text += '\n注意：Base 模型不會照指示回答，精煉效果可能很差。';
  return text;
}
