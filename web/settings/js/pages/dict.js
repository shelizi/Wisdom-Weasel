import { h, section, card, button, buttons, input, listbox, prompt, note } from '../ui.js';
import { trim } from '../state.js';

const kMaxShownWords = 3000;
let data = null;          // {available, error, words:[[詞, 分數]], rules:[{kind, from, to}]}
let loading = false;
let filter = '';
let word = '';
let selectedWords = [];
let selectedRules = [];
let dicts = null;
let selectedDict = null;
window.addEventListener('personal-words-changed', () => { data = null; });

export default {
  id: 'dict',
  nav: '詞庫管理',
  icon: '',
  title: '詞庫管理',
  desc: '維護個人詞庫的詞彙與精煉規則，以及備份、匯出輸入法的使用者詞典。',

  onShow(ctx) {
    if (!data) this.loadWords(ctx);
    if (!dicts) this.loadDicts(ctx);
  },

  async loadWords(ctx) {
    loading = true;
    try {
      data = await ctx.call('words.load');
    } catch (e) {
      data = { available: false, error: e.message, words: [], rules: [] };
    }
    loading = false;
    selectedWords = selectedWords.filter((w) => data.words.some(([x]) => x === w));
    selectedRules = [];
    if (ctx.isCurrent(this)) ctx.rerender();
  },

  async loadDicts(ctx) {
    try {
      dicts = await ctx.call('dicts.list');
    } catch (e) {
      dicts = [];
      ctx.status(e.message, 'error');
    }
    if (ctx.isCurrent(this)) ctx.rerender();
  },

  // 修改：A 加入、M 合併、R 封鎖、D 刪除、X/Y/U 移除合併／加入／封鎖規則
  async edit(ctx, lines, message) {
    if (!lines.length) return;
    try {
      await ctx.call('words.edit', { lines });
      await this.loadWords(ctx);
      ctx.status(message || '已更新個人詞庫（' + lines.length + ' 項）。', 'ok');
    } catch (e) {
      ctx.status(e.message, 'error');
    }
  },

  async rename(ctx, oldWord) {
    const value = await prompt('新的寫法（之後再打原寫法也會算到新寫法）', oldWord, { title: '修改詞彙', maxlength: 40 });
    if (value === null) return;
    const newWord = trim(value);
    if (!newWord || newWord === oldWord) return;
    if (/[\t\r\n]/.test(newWord)) {
      ctx.status('詞彙不能包含 Tab 或換行。', 'error');
      return;
    }
    // 直接修改詞彙：以「合併」完成（原寫法 → 新寫法）
    this.edit(ctx, ['M\t' + oldWord + '\t' + newWord], '已把「' + oldWord + '」改成「' + newWord + '」。');
  },

  async dictCommand(ctx, op) {
    if (op !== 'restore' && !selectedDict) return;
    let path = '';
    if (op !== 'backup') {
      // 先選檔（不必暫停輸入法），確定要做了才暫停
      path = await ctx.call('dicts.pickFile', { op, name: selectedDict || '' });
      if (!path) return;
    }
    ctx.status('輸入法暫停中，正在處理詞典…', 'busy');
    try {
      const report = await ctx.call('dicts.run', { op, name: selectedDict || '', path });
      ctx.status(report, 'ok');
      this.loadDicts(ctx);
    } catch (e) {
      ctx.status(e.message, 'error');
    }
  },

  render(ctx) {
    const available = data && data.available;
    const disabled = !available;
    // 詞彙清單與筆數：搜尋時只重畫這兩個
    const wordBox = h('div');
    const countEl = h('span', { class: 'muted grow' });
    const renderWords = () => {
      const f = trim(filter);
      const matched = data ? data.words.filter(([w]) => !f || w.includes(f)) : [];
      let count;
      if (!data) count = loading ? '讀取中…' : '';
      else if (!available) count = data.error || '個人詞庫目前關閉，或輸入法沒有回應。';
      else if (!f) count = '共 ' + data.words.length + ' 個詞（依常用程度排序）';
      else count = '符合 ' + matched.length + ' 個（共 ' + data.words.length + ' 個）';
      if (matched.length > kMaxShownWords) count += '，顯示前 ' + kMaxShownWords + ' 個';
      countEl.textContent = count;
      wordBox.replaceChildren(listbox(
        matched.slice(0, kMaxShownWords).map(([w, score]) => ({ key: w, label: w, detail: score.toFixed(1) })),
        selectedWords, (keys) => { selectedWords = keys; }, {
          multiple: true, height: '300px', empty: available ? '沒有符合的詞。' : '',
          ondblclick: (w) => !disabled && this.rename(ctx, w),
          onkey: (e, keys) => {
            if (disabled || !keys.length) return false;
            if (e.key === 'F2') { this.rename(ctx, keys[0]); return true; }
            if (e.key === 'Delete') { this.edit(ctx, keys.map((w) => 'D\t' + w)); return true; }
            return false;
          },
        }));
    };
    renderWords();
    const rules = data ? data.rules : [];
    const ruleText = (r) => (r.kind === 'merge' ? '合併　' + r.from + ' → ' + r.to : r.kind === 'add' ? '加入　' + r.from : '封鎖　' + r.from);
    const ruleList = listbox(rules.map((r, i) => ({ key: i, label: ruleText(r) })), selectedRules,
      (keys) => { selectedRules = keys; }, { multiple: true, height: '300px', empty: available ? '還沒有規則。' : '' });

    const wordInput = input(word, (v) => { word = v; }, { disabled, maxlength: 40, placeholder: '詞彙', width: '180px' });
    const need = (list, message) => {
      if (!list.length) ctx.status(message, 'error');
      return list.length > 0;
    };
    const validWord = (message) => {
      const w = trim(word);
      if (!w || w.length > 40) {
        ctx.status(message, 'error');
        return null;
      }
      return w;
    };

    const dictList = dicts === null ? h('div', { class: 'listbox list-empty', style: { height: '160px' } }, '讀取中…')
      : listbox(dicts.map((d) => ({ key: d, label: d })), selectedDict, (d) => {
        selectedDict = d;
        ctx.rerender();
      }, { height: '160px', empty: '沒有使用者詞典。' });

    return [
      section('個人詞庫的詞彙',
        card({
          below: [
            h('div', { class: 'row' }, '搜尋', input(filter, (v) => {
              filter = v;
              renderWords();
            }, { disabled, width: '220px' }), countEl,
            button('重新整理', () => this.loadWords(ctx))),
            h('div', { class: 'spacer' }),
            h('div', { class: 'grid two' },
              wordBox,
              h('div', {}, h('div', { class: 'field-label' }, '規則（加入／封鎖／合併）'), ruleList)),
            h('div', { class: 'spacer' }),
            h('div', { class: 'row' },
              wordInput,
              button('加入', () => {
                const w = validWord('請先輸入要加入的詞（40 字以內）。');
                if (w) this.edit(ctx, ['A\t' + w]);
              }, { disabled }),
              button('合併為此詞', () => {
                if (!need(selectedWords, '請先選取要合併的詞（可多選）。')) return;
                const w = validWord('請輸入正確的寫法；所選的詞會併到這個詞。');
                if (w) this.edit(ctx, selectedWords.filter((x) => x !== w).map((x) => 'M\t' + x + '\t' + w));
              }, { disabled }),
              button('刪除', () => need(selectedWords, '請先選取要刪除的詞（可多選）。') &&
                this.edit(ctx, selectedWords.map((w) => 'D\t' + w)), { disabled }),
              button('封鎖', () => need(selectedWords, '請先選取要封鎖的詞（可多選）。') &&
                this.edit(ctx, selectedWords.map((w) => 'R\t' + w)), { disabled }),
              h('span', { class: 'grow' }),
              button('移除規則', () => {
                if (!need(selectedRules, '請先選取右方要移除的規則。')) return;
                this.edit(ctx, selectedRules.map((i) => rules[i]).map((r) =>
                  (r.kind === 'merge' ? 'X\t' : r.kind === 'add' ? 'Y\t' : 'U\t') + r.from));
              }, { disabled })),
            h('div', { class: 'note' }, '雙擊詞彙（或按 F2）可直接修改。「刪除」只移除這次，之後打到還會再學；「封鎖」刪除且不再學習；修改與合併會記成規則，「重新精煉全部」時也會套用。'),
          ],
        })),
      section('輸入法使用者詞典（Rime）',
        card({
          below: [
            dictList,
            h('div', { class: 'spacer' }),
            buttons(
              button('備份', () => this.dictCommand(ctx, 'backup'), { disabled: !selectedDict }),
              button('還原…', () => this.dictCommand(ctx, 'restore')),
              button('匯出文字碼表…', () => this.dictCommand(ctx, 'export'), { disabled: !selectedDict }),
              button('匯入文字碼表…', () => this.dictCommand(ctx, 'import'), { disabled: !selectedDict })),
            note('備份／還原是整個詞典的快照；匯出／匯入為文字碼表。操作時輸入法會暫停片刻。'),
          ],
        })),
    ];
  },
};
