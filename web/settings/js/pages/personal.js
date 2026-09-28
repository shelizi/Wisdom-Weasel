import { h, section, card, toggle, checkbox, button, buttons, select, input, confirm } from '../ui.js';
import { profileSelect } from './profiles.js';

let statusText = '';
let running = false;
let timer = null;
let ticks = 0;
let personalDirty = false;  // 個人詞庫的設定改了還沒套用
window.addEventListener('settings-applied', () => { personalDirty = false; });

export default {
  id: 'personal',
  nav: '個人詞庫',
  icon: '',
  title: '個人詞庫',
  desc: '從你打過的字學習常用詞，優先出現在預測候選中；可定時用 LLM 精煉。',

  // 顯示時向輸入法要最新狀態，並每秒更新（精煉進度）；每 5 秒請輸入法重寫一次狀態
  onShow(ctx) {
    ticks = 0;
    this.poll(ctx, true);
    clearInterval(timer);
    timer = setInterval(() => this.poll(ctx, ++ticks % 5 === 0), 1000);
  },

  onHide() {
    clearInterval(timer);
    timer = null;
  },

  async poll(ctx, refresh) {
    try {
      const r = await ctx.call('personal.status', { refresh });
      const changedRunning = r.running !== running;
      running = r.running;
      if (r.text !== statusText || changedRunning) {
        statusText = r.text;
        if (this.statusEl) this.statusEl.textContent = statusText;
        if (changedRunning && ctx.isCurrent(this)) this.updateButtons();
      }
    } catch {
      // 狀態只是顯示用，失敗就等下一次
    }
  },

  async command(ctx, command) {
    if (command === 4) {
      if (!(await confirm('確定要清除個人詞庫的所有資料嗎？\n\n包含學到的詞、原始輸入紀錄與所有封存，清除後無法復原。',
        { title: '清除個人詞庫', danger: true, ok: '清除' }))) return;
    } else if (personalDirty) {
      ctx.status('個人詞庫的設定有變更，請先按「套用」再精煉。', 'error');
      return;
    }
    try {
      await ctx.call('personal.command', { command });
      ctx.status(command === 4 ? '已清除個人詞庫。' : '已開始精煉，完成後會顯示結果。', 'ok');
      window.dispatchEvent(new Event('personal-words-changed'));
      if (command !== 4) running = true;  // 輸入法回覆前先停用按鈕，避免連按
      this.updateButtons();
      this.poll(ctx, false);
    } catch (e) {
      ctx.status(e.message, 'error');
    }
  },

  render(ctx) {
    const f = ctx.state.form.personal;
    const changed = () => {
      personalDirty = true;
      ctx.markDirty('llm');
    };
    const off = !f.enabled;
    this.statusEl = h('div', { class: 'status-text' }, statusText || '正在讀取狀態…');
    const refineButton = button('立即精煉', () => this.command(ctx, 2));
    const refineAllButton = button('重新精煉全部', () => this.command(ctx, 3));
    const clearButton = button('清除所有資料…', () => this.command(ctx, 4), { danger: true });
    this.updateButtons = () => {
      // 精煉按鈕要輸入法那邊的個人詞庫開著；清除永遠可以
      refineButton.disabled = refineAllButton.disabled = !f.enabled || running;
      clearButton.disabled = running;
    };
    this.updateButtons();

    return [
      section(null,
        card({
          title: '啟用個人詞庫',
          desc: '從你送出的文字學習常用詞與接續；候選在開啟「智慧預測」時出現。',
          control: toggle(f.enabled, (v) => {
            f.enabled = v;
            changed();
            ctx.rerender();
          }),
        })),
      section('學習',
        card({
          title: '候選中最多幾個來自個人詞庫', disabled: off,
          control: select([0, 1, 2, 3, 4, 5].map((n) => ({ value: n, label: String(n) })), f.max,
            (v) => { f.max = Number(v); changed(); }, { disabled: off, width: '90px' }),
        }),
        card({
          title: '久未使用的詞淘汰半衰期（天）', disabled: off,
          control: input(f.half_life, (v) => { f.half_life = v; changed(); }, { disabled: off, width: '90px' }),
        }),
        card({
          title: checkbox(f.keep_log, '保留原始輸入紀錄（加密保存；精煉與「重新精煉全部」需要用到）', (v) => { f.keep_log = v; changed(); }, { disabled: off }),
          disabled: off,
        }),
        card({
          title: checkbox(f.filter, '學習前先用本機模型過濾不通順的輸入', (v) => { f.filter = v; changed(); }, { disabled: off }),
          desc: '亂按、亂湊的字不學（片段每字平均分數低於 llm/personal/filter/min_logprob，預設 -7.5）。需要本機模型（智慧預測或整句校正用 llama.cpp）；沒有時照常學習。常打的詞與重複打了三次的片段不會被擋。',
          disabled: off,
        }),
        card({
          title: checkbox(f.rime_boost, '讓常打的詞在注音選字時排前面', (v) => { f.rime_boost = v; changed(); }, { disabled: off }),
          desc: '產生個人詞表給 Rime；常用詞會以明文存在使用者資料夾。',
          disabled: off,
        })),
      section('定時精煉',
        card({
          title: h('div', { class: 'row' }, '每隔',
            input(f.interval, (v) => { f.interval = v; changed(); }, { disabled: off, width: '70px' }), '天，在停止打字時自動精煉'),
          desc: '0 = 只手動。精煉後原始紀錄會封存，再重新累積。',
          disabled: off,
        }),
        card({
          title: '精煉使用的模型',
          desc: 'LLM 會刪除錯字與無意義片段、合併不一致的寫法，並把整句拆成詞或片語分別學習（拆的時候不能改字；想整句保留時到「管理詞彙」加入該句）。選「不使用 LLM」時只做統計整理、不送出資料；選雲端 API 時，常用詞與部分例句會送到該服務。',
          disabled: off,
          control: [profileSelect(ctx, 'refine', { disabled: off }),
            button('管理模型…', () => ctx.go('models', { profile: ctx.state.use.refine >= 0 ? ctx.state.use.refine : undefined }))],
        })),
      section('資料',
        card({
          below: [
            this.statusEl,
            h('div', { class: 'spacer' }),
            buttons(refineButton, refineAllButton, button('管理詞彙…', () => ctx.go('dict')), clearButton),
            h('div', { class: 'note' }, '「重新精煉全部」會用所有封存與累積中的原始紀錄從頭重建詞庫，再精煉一次。資料在本機加密（金鑰由系統保管，只有你的帳號能解開），存在使用者資料夾的 personal 目錄。'),
          ],
        })),
    ];
  },
};
