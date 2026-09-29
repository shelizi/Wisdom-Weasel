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
  { which: 'refine', label: '個人詞庫精煉', page: '個人詞庫', on: (f) => f.personal.enabled,
    desc: '需要能照指示回答的 Instruct 模型。選「不使用 LLM」時只做統計整理、不送出資料；選雲端 API 時，常用詞與部分例句會送到該服務。' },
];

// 目前選用的索引；不合用（超出範圍、評分選到 API）時是 -1
function selected(ctx, which) {
  const { profiles, use } = ctx.state;
  const i = use[which];
  return i >= 0 && i < profiles.length && (which !== 'scorer' || !profiles[i].remote) ? i : -1;
}

// 沒選時的說法：精煉是「不使用 LLM」；評分是「借用已載入的本機模型」
function noneLabel(ctx, which) {
  if (which === 'refine') return '不使用 LLM（只做統計整理）';
  if (which === 'scorer') return '借用智慧預測或注音校正的本機模型';
  return ctx.state.profiles.length ? '（請選擇模型）' : '（尚未設定模型）';
}

// 下拉選單（語言模型頁用）
export function profileSelect(ctx, which, { disabled } = {}) {
  const { profiles, use } = ctx.state;
  const options = [{ value: -1, label: noneLabel(ctx, which) }];
  profiles.forEach((p, i) => {
    if (which !== 'scorer' || !p.remote) options.push({ value: i, label: profileLabel(p) });
  });
  return select(options, selected(ctx, which), (v) => {
    use[which] = Number(v);
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
  const uses = kModelUses.filter((u) => u.on(form) && selected(ctx, u.which) === index).map((u) => u.label);
  if (!uses.length) return '目前沒有被使用。可在上面的「各功能使用的模型」選用。';
  let text = '用於：' + uses.join('、');
  const p = profiles[index];
  if (!p.remote && p.model_type === 'Base' && selected(ctx, 'refine') === index)
    text += '\n注意：Base 模型不會照指示回答，精煉效果可能很差。';
  return text;
}
