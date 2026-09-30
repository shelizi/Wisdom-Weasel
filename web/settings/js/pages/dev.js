import { section, card, toggle } from '../ui.js';
import { can } from '../state.js';

export default {
  id: 'dev',
  nav: '開發',
  icon: '',
  title: '開發',
  desc: '給開發與除錯用的選項。',
  hidden: () => !can('dev_console'),  // 鼠鬚管沒有開發終端

  render(ctx) {
    const { dev } = ctx.state;
    return [
      section(null,
        card({
          title: '開發終端（調試日誌）',
          desc: '另開一個視窗，即時顯示輸入法的調試日誌：按鍵、候選、模型載入、LLM 預測與校正的過程。'
            + '套用後直接開啟或關閉，不必重新啟動輸入法。開發終端的視窗沒有關閉按鈕（直接關掉會結束輸入法），要關請回到這裡。',
          control: toggle(dev.console, (v) => {
            dev.console = v;
            ctx.markDirty('dev');
          }),
        })),
    ];
  },
};
