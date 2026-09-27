// 各功能選用模型設定的下拉選單（智慧預測、注音校正、個人詞庫精煉共用）
import { select } from '../ui.js';
import { profileLabel } from '../state.js';

// which：predict / typo / refine。精煉的第一項是「不使用 LLM」
export function profileSelect(ctx, which, { disabled } = {}) {
  const { profiles, use } = ctx.state;
  const options = [];
  if (which === 'refine') options.push({ value: -1, label: '不使用 LLM（只做統計整理）' });
  else if (use[which] < 0 || use[which] >= profiles.length) options.push({ value: -1, label: profiles.length ? '（請選擇模型）' : '（尚未設定模型）' });
  profiles.forEach((p, i) => options.push({ value: i, label: profileLabel(p) }));
  const value = use[which] >= 0 && use[which] < profiles.length ? use[which] : -1;
  return select(options, value, (v) => {
    use[which] = Number(v);
    ctx.markDirty('llm');
  }, { disabled, width: '320px' });
}

// 這組設定被哪些功能使用（語言模型頁顯示）
export function profileUsage(ctx, index) {
  const { use, form, profiles } = ctx.state;
  if (index < 0) return '按「新增」建立一組模型設定。';
  const uses = [];
  if (use.predict === index) uses.push('智慧預測');
  if (use.refine === index) uses.push('個人詞庫精煉');
  if (form.typo.llm && use.typo === index) uses.push('注音校正');
  if (!uses.length) return '目前沒有被使用。可在「智慧預測」、「注音校正」或「個人詞庫」頁選用。';
  let text = '用於：' + uses.join('、');
  const p = profiles[index];
  if (!p.remote && p.model_type === 'Base' && use.refine === index)
    text += '\n注意：Base 模型不會照指示回答，精煉效果可能很差。';
  return text;
}
