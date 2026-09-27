import { h, section, card, toggle, checkbox, button, textarea, input, note, listbox } from '../ui.js';
import { profileSelect } from './profiles.js';

let testInput = '今天天氣很好，我們一起去';
let loadedText = '';
let loadedEl = null;
let polls = 0;
let pollTimer = null;
let pageCtx = null;  // 部署後輪詢用

function describeProbe(r) {
  if (!r.connected) return '無法連線到輸入法服務。';
  if (r.timeout) return '輸入法沒有回應（可能正在載入模型），請稍後再試。';
  if (r.status === 'disabled') return '輸入法目前沒有載入 LLM 模型（LLM 智慧預測已關閉）。';
  return '輸入法目前載入：' + r.model;
}

async function refreshLoaded(ctx) {
  try {
    const r = await ctx.call('llm.probe', {});
    loadedText = describeProbe(r);
  } catch (e) {
    loadedText = e.message;
  }
  if (loadedEl) loadedEl.set(loadedText);
}

// 重新部署後，輸入法會重新載入模型；每秒詢問一次目前載入的模型，更新顯示
window.addEventListener('settings-applied', () => {
  polls = 15;
  clearInterval(pollTimer);
  pollTimer = setInterval(() => {
    if (--polls <= 0) clearInterval(pollTimer);
    if (pageCtx) refreshLoaded(pageCtx);
  }, 1000);
});

export default {
  id: 'predict',
  nav: '智慧預測',
  icon: '',
  title: 'LLM 智慧預測',
  desc: '用語言模型預測你接下來要打的詞。',

  onShow(ctx) {
    pageCtx = ctx;
    refreshLoaded(ctx);
  },

  render(ctx) {
    const { form } = ctx.state;
    const f = form.predict;
    const changed = () => ctx.markDirty('llm');
    const off = !f.enabled;
    loadedEl = note(loadedText || '正在詢問輸入法…');

    const result = h('div');
    const testStatus = note('');
    const runTest = async () => {
      if (!testInput.trim()) {
        testStatus.set('請先輸入一段前文。');
        return;
      }
      runButton.disabled = true;
      testStatus.set('預測中…');
      result.replaceChildren();
      try {
        const r = await ctx.call('llm.probe', { context: testInput });
        loadedText = describeProbe(r);
        loadedEl.set(loadedText);
        if (!r.connected) testStatus.set('無法連線到輸入法服務，請確認小狼毫正在執行。', 'error');
        else if (r.timeout) testStatus.set('輸入法沒有回應（可能正在載入模型），請稍後再試。', 'error');
        else if (r.status === 'disabled') testStatus.set('LLM 智慧預測目前關閉，無法測試。請先啟用並按「套用」。');
        else {
          if (r.candidates.length)
            result.replaceChildren(listbox(r.candidates.map((c, i) => ({ key: i, label: (i + 1) + '.  ' + c })), null, () => {}, { height: 'auto' }));
          testStatus.set((r.candidates.length ? '' : '沒有產生候選。') + '耗時 ' + r.ms + ' ms（' + r.model + '）');
        }
      } catch (e) {
        testStatus.set(e.message, 'error');
      } finally {
        runButton.disabled = false;
      }
    };
    const runButton = button('預測', runTest);

    return [
      section(null,
        card({
          title: '啟用 LLM 智慧預測',
          desc: '所有預測候選的總開關；關閉時個人詞庫只在背景學習，不跳出候選。',
          control: toggle(f.enabled, (v) => {
            f.enabled = v;
            changed();
            ctx.rerender();
          }),
        }),
        card({
          title: checkbox(f.after_commit, '選字送出後，預測下一個詞', (v) => { f.after_commit = v; changed(); }, { disabled: off }),
          disabled: off,
        }),
        card({
          title: checkbox(f.while_typing, '打字停頓時，自動補完目前的字（按 Tab 選用）', (v) => { f.while_typing = v; changed(); }, { disabled: off }),
          disabled: off,
        })),
      section('模型',
        card({
          title: '預測使用的模型',
          desc: '在「語言模型」頁設定可用的模型。',
          disabled: off,
          control: [profileSelect(ctx, 'predict', { disabled: off }),
            button('管理模型…', () => ctx.go('models', { profile: ctx.state.use.predict }))],
          below: loadedEl,
        })),
      section('提示詞',
        card({
          title: '提示詞',
          desc: 'Base 模型放在前文前面引導續寫；Instruct 與 OpenAI 放在指令前面。可用來要求繁體、臺灣用語等。',
          disabled: off,
          below: textarea(f.prompt, (v) => { f.prompt = v; changed(); }, { rows: 4, disabled: off }),
        })),
      section('預測測試',
        card({
          title: '預測測試',
          desc: '用輸入法目前載入的模型，預測接在前文後面的詞。變更設定後請先按「套用」。',
          below: [
            h('div', { class: 'row' },
              h('div', { class: 'grow' }, input(testInput, (v) => { testInput = v; }, { onenter: runTest, width: '100%' })),
              runButton),
            h('div', { class: 'spacer' }),
            result,
            testStatus,
          ],
        })),
    ];
  },
};
