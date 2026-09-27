import { section, card, checkbox, button, textarea } from '../ui.js';
import { profileSelect } from './profiles.js';

export default {
  id: 'typo',
  nav: '注音校正',
  icon: '',
  title: '注音校正',
  desc: '注音打錯字時的處理：Rime 容錯選字，或請語言模型依前文校正整句。',

  render(ctx) {
    const t = ctx.state.form.typo;
    const changed = () => ctx.markDirty('llm');
    const off = !t.llm;
    const prompt = textarea(t.prompt, (v) => { t.prompt = v; changed(); }, { rows: 7, disabled: off });

    return [
      section('校正方式',
        card({
          title: checkbox(t.rime, 'Rime 容錯：按到隔壁鍵、多打或少打一鍵時，找相近的注音選字', (v) => { t.rime = v; changed(); }),
          desc: '套用後重新部署；三個注音方案都會使用。',
        }),
        card({
          title: checkbox(t.llm, 'LLM 整句校正：打字停頓時請模型依前文與注音修正整句，標示「校正」（按 Tab 選用）', (v) => {
            t.llm = v;
            changed();
            ctx.rerender();
          }),
        })),
      section('LLM 整句校正',
        card({
          title: '校正使用的模型',
          disabled: off,
          control: [profileSelect(ctx, 'typo', { disabled: off }),
            button('管理模型…', () => ctx.go('models', { profile: ctx.state.use.typo }))],
        }),
        card({
          title: '校正提示詞',
          desc: 'Instruct 與 OpenAI 模型取代預設的校正指令；Base 模型放在範例前面。',
          disabled: off,
          control: button('還原預設', () => {
            t.prompt = ctx.state.init.defaults.correct_instruction;
            prompt.value = t.prompt;
            changed();
          }, { disabled: off }),
          below: prompt,
        })),
    ];
  },
};
