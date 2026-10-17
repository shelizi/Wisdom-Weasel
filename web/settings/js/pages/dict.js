import { h, section, card, toggle, button, buttons, select, input, listbox, prompt, confirm, note } from '../ui.js';
import { trim } from '../state.js';

const kMaxShownWords = 3000;
const kLexPageSize = 50;

// 個人詞彙狀態
let data = null;          // {available, error, words:[[詞, 分數]], rules:[{kind, from, to}]}
let loading = false;
let filter = '';
let word = '';
let selectedWords = [];
let selectedRules = [];

// Rime 使用者詞典狀態
let dicts = null;
let selectedDict = null;

// 詞庫管理來源：'taiwan'（台灣詞庫）| 'personal'（個人詞彙）
let currentSource = 'taiwan';

// 台灣詞庫搜尋與分頁狀態
let lexQuery = '';
let lexFilter = 'all';    // 'all' | 'modified' | 'hidden'
let lexOffset = 0;
let lexTotal = 0;
let lexCounts = { all: 0, modified: 0, hidden: 0, conflicts: 0 };
let lexRows = [];
let lexLoading = false;
let lexError = '';
let lexSelectedKeys = []; // ['詞\t讀音碼']
let lexSearchSeq = 0;
let lexSearchTimer = null;

// 共用新增／修改表單狀態（內建詞庫）
let lexFormMode = 'add';  // 'add' | 'modify'
let lexFormOrigWord = '';
let lexFormOrigCode = '';
let lexFormOrigZhuyin = '';
let lexFormWord = '';
let lexFormReading = '';
let lexFormPref = 'normal';

window.addEventListener('personal-words-changed', () => { data = null; });
window.addEventListener('settings-applied', () => {
  lexSearchSeq++;
});

function rowKey(w, code) {
  return `${w}\t${code}`;
}

function parseRowKey(key) {
  const idx = String(key).indexOf('\t');
  return idx < 0 ? { word: String(key), code: '' } : { word: key.slice(0, idx), code: key.slice(idx + 1) };
}

function prefLabel(pref) {
  if (pref === 'low') return '降低';
  if (pref === 'high') return '提高';
  return '標準';
}

function statusBadgeInfo(row) {
  if (row.status === 'conflict_removed' || row.upstream_removed) {
    return { text: '⚠ 原始項目已移除，請檢查', cls: 'danger' };
  }
  if (row.status === 'hidden' || row.hidden) {
    return { text: '⊘ 已隱藏', cls: 'muted' };
  }
  if (row.status === 'added') {
    return { text: '＋ 新增', cls: 'ok' };
  }
  if (row.status === 'modified') {
    if (row.dedup_upstream) return { text: '✓ 已修改（與上游重複已去重）', cls: 'accent' };
    return { text: '✎ 已修改', cls: 'accent' };
  }
  return { text: '內建', cls: 'subtle' };
}

function pushUndo(lex, label) {
  if (!lex) return;
  lex.undoStack = lex.undoStack || [];
  lex.undoStack.push({
    label,
    edits: JSON.parse(JSON.stringify(lex.edits || [])),
  });
  if (lex.undoStack.length > 20) lex.undoStack.shift();
}

export default {
  id: 'dict',
  nav: '詞庫管理',
  icon: '\uE82D',
  title: '詞庫管理',
  desc: '管理隨附的內建台灣詞庫、個人學習詞彙與精煉規則，以及備份或匯出入 Rime 使用者詞典。',

  onShow(ctx) {
    if (ctx.state.dictSource) {
      currentSource = ctx.state.dictSource;
      delete ctx.state.dictSource;
    }
    if (currentSource === 'personal' && !data) this.loadWords(ctx);
    if (!dicts) this.loadDicts(ctx);
    if (!ctx.state.lexicon || !ctx.state.lexicon.info) {
      this.refreshLexiconStatus(ctx, false);
    }
    if (currentSource === 'taiwan') {
      this.searchLexicon(ctx, { resetPage: false });
    }
  },

  async refreshLexiconStatus(ctx, refreshService = true) {
    try {
      const res = await ctx.call('lexicon.status', { refresh: refreshService });
      if (res && res.taiwan && ctx.state.lexicon) {
        ctx.state.lexicon.info = res.taiwan;
        if (!ctx.state.dirty.has('lexicon')) {
          ctx.state.lexicon.savedEnabled = res.taiwan.enabled !== false;
          ctx.state.lexicon.enabled = res.taiwan.enabled !== false;
          ctx.state.lexicon.savedEdits = JSON.parse(JSON.stringify(res.taiwan.edits || []));
          ctx.state.lexicon.edits = JSON.parse(JSON.stringify(res.taiwan.edits || []));
        }
        if (ctx.isCurrent(this)) {
          if (this.updateStatusCard) this.updateStatusCard();
          else ctx.rerender();
        }
      }
    } catch (e) {
      if (refreshService) ctx.status(e.message, 'error');
    }
  },

  async searchLexicon(ctx, { resetPage = false } = {}) {
    const lex = ctx.state.lexicon;
    if (!lex || !lex.info || !lex.info.resource_ok) {
      lexLoading = false;
      lexError = (lex && lex.info && lex.info.resource_error) || '內建詞庫資源無法讀取。';
      lexRows = [];
      lexTotal = 0;
      if (this.updateLexiconList) this.updateLexiconList();
      return;
    }
    if (resetPage) lexOffset = 0;
    const seq = ++lexSearchSeq;
    lexLoading = true;
    lexError = '';
    if (this.updateLexiconList) this.updateLexiconList();

    try {
      const res = await ctx.call('lexicon.search', {
        id: 'wisdom_taiwan',
        query: lexQuery,
        filter: lexFilter,
        offset: lexOffset,
        limit: kLexPageSize,
        pending_edits: lex.edits,
      });
      if (seq !== lexSearchSeq) return; // 捨棄過期查詢回應，不覆蓋最新結果
      lexLoading = false;
      lexError = '';
      lexTotal = res.total || 0;
      lexCounts = res.counts || { all: 0, modified: 0, hidden: 0, conflicts: 0 };
      lexRows = res.rows || [];
      if (lexOffset > 0 && lexOffset >= lexTotal) {
        lexOffset = Math.max(0, Math.floor((lexTotal - 1) / kLexPageSize) * kLexPageSize);
        return this.searchLexicon(ctx, { resetPage: false });
      }
      if (this.updateLexiconList) this.updateLexiconList();
    } catch (e) {
      if (seq !== lexSearchSeq) return;
      lexLoading = false;
      lexError = e.message || '讀取詞庫失敗';
      lexRows = [];
      if (this.updateLexiconList) this.updateLexiconList();
    }
  },

  scheduleSearch(ctx) {
    clearTimeout(lexSearchTimer);
    lexSearchTimer = setTimeout(() => {
      this.searchLexicon(ctx, { resetPage: true });
    }, 120);
  },

  findRow(key) {
    const { word: w, code: c } = parseRowKey(key);
    return lexRows.find((r) => r.word === w && r.code === c) || null;
  },

  upsertEdit(lex, editObj) {
    const idx = lex.edits.findIndex((e) => e.word === editObj.word && e.code === editObj.code);
    if (idx >= 0) lex.edits[idx] = editObj;
    else lex.edits.push(editObj);
  },

  removeEdit(lex, w, c) {
    lex.edits = lex.edits.filter((e) => !(e.word === w && e.code === c));
  },

  async submitLexiconForm(ctx) {
    const lex = ctx.state.lexicon;
    if (!lex || !lex.info || !lex.info.resource_ok) return;
    const rawWord = trim(lexFormWord);
    const rawReading = trim(lexFormReading);
    if (!rawWord) {
      ctx.status('請輸入詞彙（1～12 個漢字）。', 'error');
      return;
    }
    if (!rawReading) {
      ctx.status('請輸入完整注音讀音（不自動猜測讀音）。', 'error');
      return;
    }

    try {
      const v = await ctx.call('lexicon.validateEdit', {
        id: 'wisdom_taiwan',
        op: lexFormMode,
        word: rawWord,
        reading: rawReading,
        pref: lexFormPref,
        orig_word: lexFormMode === 'modify' ? lexFormOrigWord : '',
        orig_code: lexFormMode === 'modify' ? lexFormOrigCode : '',
      });

      if (lexFormMode === 'add') {
        const existing = lex.edits.find((e) => e.word === v.word && e.code === v.code && !e.hidden);
        if (existing) {
          ctx.status(`「${v.word}（${v.zhuyin}）」已在個人修改清單中，將更新其排序偏好。`, 'ok');
        }
        pushUndo(lex, `新增「${v.word}」`);
        this.upsertEdit(lex, {
          word: v.word,
          code: v.code,
          zhuyin: v.zhuyin,
          hidden: false,
          pref: v.pref,
          origin_base: Boolean(v.in_base),
          base_weight: v.base_weight,
          replaced_from_word: '',
          replaced_from_code: '',
        });
        lexFormWord = '';
        lexFormReading = '';
        lexFormPref = 'normal';
        lexSelectedKeys = [rowKey(v.word, v.code)];
        ctx.syncLexiconDirty();
        ctx.status(`已加入「${v.word}（${v.zhuyin}）」（待套用部署後生效）。`, 'ok');
      } else {
        // 修改模式：可修改讀音、修改寫法、或調整排序偏好
        pushUndo(lex, `修改「${lexFormOrigWord}」`);
        const sameKey = (v.word === lexFormOrigWord && v.code === lexFormOrigCode);
        if (sameKey) {
          const prevEdit = lex.edits.find((e) => e.word === v.word && e.code === v.code);
          this.upsertEdit(lex, {
            word: v.word,
            code: v.code,
            zhuyin: v.zhuyin,
            hidden: false,
            pref: v.pref,
            origin_base: prevEdit ? Boolean(prevEdit.origin_base) : Boolean(v.in_base),
            base_weight: v.base_weight,
            replaced_from_word: prevEdit ? (prevEdit.replaced_from_word || '') : '',
            replaced_from_code: prevEdit ? (prevEdit.replaced_from_code || '') : '',
          });
        } else {
          // 讀音或寫法變更：隱藏原項目（若原項屬於內建基底），並新增替代項，於同一次套用生效
          const origEdit = lex.edits.find((e) => e.word === lexFormOrigWord && e.code === lexFormOrigCode);
          const origInBase = Boolean(v.orig_in_base || (origEdit && origEdit.origin_base));
          if (origInBase) {
            this.upsertEdit(lex, {
              word: lexFormOrigWord,
              code: lexFormOrigCode,
              zhuyin: lexFormOrigZhuyin,
              hidden: true,
              pref: 'normal',
              origin_base: true,
              base_weight: v.orig_base_weight || 1000,
              replaced_from_word: '',
              replaced_from_code: '',
            });
          } else {
            this.removeEdit(lex, lexFormOrigWord, lexFormOrigCode);
          }
          this.upsertEdit(lex, {
            word: v.word,
            code: v.code,
            zhuyin: v.zhuyin,
            hidden: false,
            pref: v.pref,
            origin_base: Boolean(v.in_base),
            base_weight: v.base_weight,
            replaced_from_word: origInBase ? lexFormOrigWord : (origEdit ? origEdit.replaced_from_word || '' : ''),
            replaced_from_code: origInBase ? lexFormOrigCode : (origEdit ? origEdit.replaced_from_code || '' : ''),
          });
        }
        lexFormMode = 'add';
        lexFormOrigWord = '';
        lexFormOrigCode = '';
        lexFormOrigZhuyin = '';
        lexFormWord = '';
        lexFormReading = '';
        lexFormPref = 'normal';
        lexSelectedKeys = [rowKey(v.word, v.code)];
        ctx.syncLexiconDirty();
        ctx.status(`已更新「${v.word}（${v.zhuyin}）」（待套用部署後生效）。`, 'ok');
      }
      await this.searchLexicon(ctx, { resetPage: false });
      if (this.updateFormBox) this.updateFormBox();
      if (this.updatePendingBar) this.updatePendingBar();
      if (this.updateStatusCard) this.updateStatusCard();
    } catch (e) {
      ctx.status(e.message, 'error');
    }
  },

  startModifyRow(ctx, row) {
    if (!row) return;
    lexFormMode = 'modify';
    lexFormOrigWord = row.word;
    lexFormOrigCode = row.code;
    lexFormOrigZhuyin = row.zhuyin;
    lexFormWord = row.word;
    lexFormReading = row.zhuyin;
    lexFormPref = row.pref || 'normal';
    if (this.updateFormBox) {
      this.updateFormBox();
      if (this.formWordInput) this.formWordInput.focus();
    }
  },

  changeSelectedPref(ctx, newPref) {
    const lex = ctx.state.lexicon;
    if (!lex || !lexSelectedKeys.length) {
      ctx.status('請先選取要調整排序偏好的詞彙項目。', 'error');
      return;
    }
    pushUndo(lex, '調整排序偏好');
    let count = 0;
    for (const key of lexSelectedKeys) {
      const row = this.findRow(key);
      const { word: w, code: c } = parseRowKey(key);
      const existing = lex.edits.find((e) => e.word === w && e.code === c);
      const inBase = row ? Boolean(row.in_base) : Boolean(existing && existing.origin_base);
      const baseWeight = row ? row.base_weight : (existing ? existing.base_weight : 1000);
      if (inBase && newPref === 'normal' && (!existing || !existing.hidden) && (!existing || !existing.replaced_from_word)) {
        // 內建項目設回「標準」且未隱藏：等同無額外權重偏好修改
        this.removeEdit(lex, w, c);
      } else {
        this.upsertEdit(lex, {
          word: w,
          code: c,
          zhuyin: row ? row.zhuyin : (existing ? existing.zhuyin : ''),
          hidden: false,
          pref: newPref,
          origin_base: inBase,
          base_weight: baseWeight,
          replaced_from_word: existing ? (existing.replaced_from_word || '') : '',
          replaced_from_code: existing ? (existing.replaced_from_code || '') : '',
        });
      }
      count++;
    }
    ctx.syncLexiconDirty();
    ctx.status(`已將 ${count} 項設為「${prefLabel(newPref)}」排序偏好（待套用）。`, 'ok');
    this.searchLexicon(ctx, { resetPage: false });
    if (this.updatePendingBar) this.updatePendingBar();
    if (this.updateStatusCard) this.updateStatusCard();
  },

  hideSelectedRows(ctx) {
    const lex = ctx.state.lexicon;
    if (!lex || !lexSelectedKeys.length) {
      ctx.status('請先選取要隱藏的詞彙項目（可多選）。', 'error');
      return;
    }
    pushUndo(lex, `隱藏 ${lexSelectedKeys.length} 項`);
    let count = 0;
    for (const key of lexSelectedKeys) {
      const row = this.findRow(key);
      const { word: w, code: c } = parseRowKey(key);
      const existing = lex.edits.find((e) => e.word === w && e.code === c);
      const inBase = row ? Boolean(row.in_base) : Boolean(existing && existing.origin_base);
      if (!inBase && (!existing || !existing.origin_base)) {
        // 使用者純新增但尚未在基底中的項目：直接移除該筆新增
        this.removeEdit(lex, w, c);
      } else {
        this.upsertEdit(lex, {
          word: w,
          code: c,
          zhuyin: row ? row.zhuyin : (existing ? existing.zhuyin : ''),
          hidden: true,
          pref: 'normal',
          origin_base: true,
          base_weight: row ? row.base_weight : (existing ? existing.base_weight : 1000),
          replaced_from_word: '',
          replaced_from_code: '',
        });
      }
      count++;
    }
    ctx.syncLexiconDirty();
    ctx.status(`已將 ${count} 項標記為「隱藏此項」（僅此內建詞庫不再提供，待套用生效）。`, 'ok');
    this.searchLexicon(ctx, { resetPage: false });
    if (this.updatePendingBar) this.updatePendingBar();
    if (this.updateStatusCard) this.updateStatusCard();
  },

  async restoreSelectedRows(ctx) {
    const lex = ctx.state.lexicon;
    if (!lex || !lexSelectedKeys.length) {
      ctx.status('請先選取要還原內建值的項目。', 'error');
      return;
    }
    const targets = [];
    for (const key of lexSelectedKeys) {
      const { word: w, code: c } = parseRowKey(key);
      const e = lex.edits.find((x) => x.word === w && x.code === c);
      if (e) targets.push(e);
    }
    if (!targets.length) {
      ctx.status('所選項目目前皆為內建原始值，無須還原。');
      return;
    }
    if (targets.length > 1) {
      const ok = await confirm(
        `確定要將所選的 ${targets.length} 筆個人修改還原為目前內建詞庫的值嗎？\n\n還原後仍可使用「撤回還原」恢復，且不會刪除公開詞庫檔案。`,
        { title: '批次還原內建值', ok: '還原內建值' }
      );
      if (!ok) return;
    }
    pushUndo(lex, `還原 ${targets.length} 項內建值`);
    for (const e of targets) {
      this.removeEdit(lex, e.word, e.code);
      if (e.replaced_from_word && e.replaced_from_code) {
        this.removeEdit(lex, e.replaced_from_word, e.replaced_from_code);
      }
    }
    ctx.syncLexiconDirty();
    ctx.status(`已還原 ${targets.length} 筆項目為內建值（可按「撤回」恢復；按「套用」完成部署）。`, 'ok');
    this.searchLexicon(ctx, { resetPage: false });
    if (this.updateFormBox) this.updateFormBox();
    if (this.updatePendingBar) this.updatePendingBar();
    if (this.updateStatusCard) this.updateStatusCard();
  },

  async restoreAllEdits(ctx) {
    const lex = ctx.state.lexicon;
    if (!lex || !lex.edits.length) {
      ctx.status('目前沒有任何個人修改項目。');
      return;
    }
    const count = lex.edits.length;
    const ok = await confirm(
      `確定要移除全部 ${count} 筆台灣詞庫個人修改，全部還原為目前內建詞庫版本嗎？\n\n此操作不會刪除公開詞庫檔案或個人學習紀錄，且套用前可按「撤回」或「取消變更」恢復。`,
      { title: '全部還原內建值', ok: `還原全部 ${count} 筆`, danger: true }
    );
    if (!ok) return;
    pushUndo(lex, `全部還原（${count} 筆）`);
    lex.edits = [];
    ctx.syncLexiconDirty();
    ctx.status(`已還原全部 ${count} 筆個人修改（可按「撤回」恢復；按「套用」部署生效）。`, 'ok');
    this.searchLexicon(ctx, { resetPage: true });
    if (this.updateFormBox) this.updateFormBox();
    if (this.updatePendingBar) this.updatePendingBar();
    if (this.updateStatusCard) this.updateStatusCard();
  },

  undoLastLexiconAction(ctx) {
    const lex = ctx.state.lexicon;
    if (!lex || !lex.undoStack || !lex.undoStack.length) return;
    const prev = lex.undoStack.pop();
    lex.edits = prev.edits;
    ctx.syncLexiconDirty();
    ctx.status(`已撤回：${prev.label}。`, 'ok');
    this.searchLexicon(ctx, { resetPage: false });
    if (this.updateFormBox) this.updateFormBox();
    if (this.updatePendingBar) this.updatePendingBar();
    if (this.updateStatusCard) this.updateStatusCard();
  },

  async exportLexiconEdits(ctx) {
    const lex = ctx.state.lexicon;
    if (!lex) return;
    if (!lex.edits.length) {
      ctx.status('目前沒有個人修改可匯出。', 'error');
      return;
    }
    try {
      const path = await ctx.call('lexicon.pickFile', { op: 'export' });
      if (!path) return;
      const res = await ctx.call('lexicon.exportEdits', {
        id: 'wisdom_taiwan',
        path,
        edits: lex.edits,
        reveal: true,
      });
      ctx.status(`已匯出 ${res.exported} 筆台灣詞庫個人修改。`, 'ok');
    } catch (e) {
      ctx.status(e.message, 'error');
    }
  },

  async importLexiconEdits(ctx) {
    const lex = ctx.state.lexicon;
    if (!lex) return;
    try {
      const path = await ctx.call('lexicon.pickFile', { op: 'import' });
      if (!path) return;
      const preview = await ctx.call('lexicon.importPreview', {
        id: 'wisdom_taiwan',
        path,
        current_edits: lex.edits,
      });
      this.showImportPreviewDialog(ctx, preview);
    } catch (e) {
      ctx.status(e.message, 'error');
    }
  },

  showImportPreviewDialog(ctx, preview) {
    const added = preview.added || [];
    const changed = preview.changed || [];
    const conflicts = preview.conflicts || [];
    const invalid = preview.invalid || [];
    const merged = preview.merged_edits || [];

    const close = () => overlay.remove();
    const applyImport = () => {
      const lex = ctx.state.lexicon;
      pushUndo(lex, '匯入個人修改');
      lex.edits = JSON.parse(JSON.stringify(merged));
      ctx.syncLexiconDirty();
      close();
      ctx.status(`已載入匯入的個人修改（新增 ${added.length} 筆、變更 ${changed.length} 筆），請按「套用」完成部署。`, 'ok');
      this.searchLexicon(ctx, { resetPage: true });
      if (this.updateFormBox) this.updateFormBox();
      if (this.updatePendingBar) this.updatePendingBar();
      if (this.updateStatusCard) this.updateStatusCard();
    };

    const renderList = (items, mapFn) => {
      if (!items.length) return h('div', { class: 'muted' }, '無');
      return h('div', { class: 'lex-preview-list' }, items.slice(0, 100).map(mapFn));
    };

    const canConfirm = added.length > 0 || changed.length > 0 || conflicts.length > 0;
    const overlay = h('div', { class: 'overlay' },
      h('div', { class: 'dialog dialog-wide', role: 'dialog', 'aria-label': '匯入台灣詞庫個人修改預覽' },
        h('h3', {}, '匯入台灣詞庫個人修改（預覽）'),
        h('div', { class: 'dialog-body lex-dialog-scroll' },
          h('div', { class: 'lex-summary-grid' },
            h('div', { class: 'lex-stat-box' }, h('div', { class: 'muted' }, '來源基底版本'), h('strong', {}, preview.base_version || '未標示')),
            h('div', { class: 'lex-stat-box' }, h('div', { class: 'muted' }, '新增項目'), h('strong', {}, `${added.length} 筆`)),
            h('div', { class: 'lex-stat-box' }, h('div', { class: 'muted' }, '變更項目'), h('strong', {}, `${changed.length} 筆`)),
            h('div', { class: 'lex-stat-box' }, h('div', { class: 'muted' }, '衝突／上游移除'), h('strong', {}, `${conflicts.length} 筆`)),
            h('div', { class: 'lex-stat-box' }, h('div', { class: 'muted' }, '無效略過'), h('strong', {}, `${invalid.length} 筆`))),
          added.length ? h('div', { class: 'field' },
            h('div', { class: 'field-label' }, `新增項目（${added.length} 筆）`),
            renderList(added, (it) => h('div', { class: 'lex-preview-item' },
              `${it.word}（${it.zhuyin}）— ${it.hidden ? '隱藏' : '排序：' + prefLabel(it.pref)}`))) : null,
          conflicts.length ? h('div', { class: 'field' },
            h('div', { class: 'field-label' }, `衝突與覆蓋項目（${conflicts.length} 筆）`),
            renderList(conflicts, (it) => h('div', { class: 'lex-preview-item warn' },
              `${it.word}（${it.zhuyin}）— ${it.reason || '覆蓋現有修改'}`))) : null,
          invalid.length ? h('div', { class: 'field' },
            h('div', { class: 'field-label' }, `無效並略過的項目（${invalid.length} 筆）`),
            renderList(invalid, (it) => h('div', { class: 'lex-preview-item danger' },
              `第 ${it.index} 筆「${it.word || ''} ${it.reading || ''}」：${it.reason}`))) : null,
          h('div', { class: 'note' }, '匯入只會更新台灣詞庫的個人修改紀錄（不執行任何路徑或外部指令），確認後需按「套用」才會重新部署生效。')
        ),
        h('div', { class: 'dialog-buttons' },
          button('套用匯入至待部署變更', applyImport, { primary: true, disabled: !canConfirm }),
          button('取消', close))
      )
    );
    document.body.append(overlay);
  },

  showAttributionDialog(ctx) {
    const info = (ctx.state.lexicon && ctx.state.lexicon.info) || {};
    const m = info.manifest || {};
    const ch = m.chewing || {};
    const close = () => overlay.remove();

    const revealZip = async () => {
      try {
        await ctx.call('lexicon.sourceArchive', { id: 'wisdom_taiwan', op: 'reveal' });
      } catch (e) {
        ctx.status(e.message, 'error');
      }
    };
    const exportZip = async () => {
      try {
        const dest = await ctx.call('lexicon.pickFile', { op: 'exportSources' });
        if (!dest) return;
        await ctx.call('lexicon.sourceArchive', { id: 'wisdom_taiwan', op: 'export', dest, reveal: true });
        ctx.status('已匯出台灣詞庫來源包 (wisdom_taiwan.sources.zip)。', 'ok');
      } catch (e) {
        ctx.status(e.message, 'error');
      }
    };

    const overlay = h('div', { class: 'overlay' },
      h('div', { class: 'dialog dialog-wide', role: 'dialog', 'aria-label': '台灣詞庫來源與授權詳情' },
        h('h3', {}, '台灣詞庫 — 來源與授權詳情'),
        h('div', { class: 'dialog-body lex-dialog-scroll' },
          h('div', { class: 'note' },
            '本詞庫為編譯期整合之單一合併擴充詞庫（wisdom_taiwan）。詞彙可能同時存在於多個上游來源，以下來源清單說明資料出處與授權，並非互斥分類，亦不宣稱單一詞彙僅屬唯一來源。'),
          h('div', { class: 'spacer' }),
          h('table', { class: 'table' },
            h('thead', {},
              h('tr', {},
                h('th', {}, '來源名稱'),
                h('th', {}, '固定版本／修訂號'),
                h('th', {}, '授權條款'),
                h('th', {}, '規模與修改說明'))),
            h('tbody', {},
              h('tr', {},
                h('td', {}, h('strong', {}, '小麥注音 (OpenVanilla McBopomofo)')),
                h('td', {}, h('code', {}, (m.revision || '—').slice(0, 12))),
                h('td', {}, 'MIT／BSD-3-Clause (libtabe)'),
                h('td', { class: 'wrap' },
                  '轉換 BPMFMappings.txt、BPMFBase.txt 與 phrase.occ 為 Rime 帶調拼音碼；保留來源讀音，詞頻換算為 count×10 並限制於 100..10000。')),
              h('tr', {},
                h('td', {}, h('strong', {}, '新酷音詞庫 (libchewing-data)')),
                h('td', {}, h('code', {}, (ch.revision || '—').slice(0, 12))),
                h('td', {}, ch.license || 'LGPL-2.1-or-later'),
                h('td', { class: 'wrap' },
                  `共處理 ${(ch.entries || 134026).toLocaleString()} 筆（新增 ${(ch.added || 16323).toLocaleString()} 筆、與小麥重疊 ${(ch.overlaps || 117703).toLocaleString()} 筆、更新權重 ${(ch.weights_updated || 82729).toLocaleString()} 筆）。選取 2..7 字漢字詞，詞頻以 log1p 正規化至 100..1000，重疊詞與讀音取最大權重（不相加）。`)),
              h('tr', {},
                h('td', {}, h('strong', {}, '本專案補充詞 (supplement.tsv)')),
                h('td', {}, h('code', {}, info.version || '隨版本釋出')),
                h('td', {}, '隨本專案授權釋出'),
                h('td', { class: 'wrap' },
                  `共 ${(m.supplement_entries || info.supplement_entries || 0).toLocaleString()} 筆台灣常用補充詞，保底基礎權重 1000。`))
            )
          ),
          h('div', { class: 'spacer' }),
          h('div', { class: 'field' },
            h('div', { class: 'field-label' }, '詞頻轉換與合併策略'),
            h('div', { class: 'note' }, m.frequency_policy || 'McBopomofo: count*10 clipped 100..10000; Chewing: log1p normalized 100..1000; duplicate word+reading: max, never sum; supplement: minimum 1000')),
          h('div', { class: 'field' },
            h('div', { class: 'field-label' }, `隨附完整來源包（wisdom_taiwan.sources.zip${info.sources_zip_size_text ? '，' + info.sources_zip_size_text : ''}）`),
            h('div', { class: 'note' }, '內含完整上游原始輸入、轉換腳本、音節表、補充詞表、重建說明與 LGPL-2.1／GPL-2.0 授權全文（不含任何個人學習資料）。'),
            h('div', { class: 'spacer' }),
            buttons(
              button('在檔案管理員顯示來源包', revealZip, { disabled: !info.sources_zip_exists }),
              button('匯出來源包 (.zip)…', exportZip, { disabled: !info.sources_zip_exists })
            )),
          h('div', { class: 'field' },
            h('div', { class: 'field-label' }, '完整授權與聲明（wisdom_taiwan.NOTICE.txt）'),
            h('pre', { class: 'lex-notice-box' }, info.notice || '（找不到授權說明檔內容）'))
        ),
        h('div', { class: 'dialog-buttons' }, button('關閉', close, { primary: true }))
      )
    );
    document.body.append(overlay);
  },

  // -------------------------------------------------------------------------
  // 個人詞彙（既有即時生效模式）

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

  // 修改：A 加入、M 合併、R 封鎖、D 刪除、X/Y/U/Z 移除合併／加入／封鎖／拆解規則
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

  // -------------------------------------------------------------------------
  // 畫面繪製

  renderTaiwanSection(ctx) {
    const lex = ctx.state.lexicon || {
      info: { resource_ok: false, resource_error: '尚未載入內建詞庫資訊。' },
      enabled: true,
      savedEnabled: true,
      edits: [],
      savedEdits: [],
      undoStack: [],
    };
    const statusCardSlot = h('div');
    const pendingBarSlot = h('div');
    const listSlot = h('div');
    const formSlot = h('div');

    this.updateStatusCard = () => {
      const info = lex.info || {};
      const st = info.status || {};
      const isDirty = ctx.state.dirty.has('lexicon');

      const badgeEls = [];
      if (isDirty) {
        badgeEls.push(h('span', { class: 'lex-badge accent', 'data-status-code': 'pending_apply' }, '● 待套用'));
      }
      for (const b of st.badges || []) {
        const prefix = b.tone === 'ok' ? '✓ '
          : b.tone === 'danger' ? '✗ '
          : b.tone === 'warn' ? '⚠ '
          : b.tone === 'info' ? '⏳ ' : '○ ';
        badgeEls.push(h('span', { class: `lex-badge ${b.tone || 'muted'}`, 'data-status-code': b.code }, prefix + b.label));
      }

      const wordsCount = info.words ? Number(info.words).toLocaleString() : '—';
      const entriesCount = info.entries ? Number(info.entries).toLocaleString() : '—';
      const verText = info.version || '—';

      const statsLine = info.resource_ok
        ? `${wordsCount} 個不同詞 · ${entriesCount} 組詞與讀音 · 版本 ${verText}`
        : '無法讀取詞庫統計（資源缺失或損壞）';

      const belowItems = [];
      if (!info.resource_ok) {
        belowItems.push(
          h('div', { class: 'lex-alert danger', role: 'alert' },
            h('strong', {}, '資源缺失或損壞：'),
            info.resource_error || '找不到內建台灣詞庫資源檔，請重新執行安裝或部署工具。')
        );
      }
      if (st.deploy_failed) {
        belowItems.push(
          h('div', { class: 'lex-alert danger', role: 'alert' },
            h('div', {},
              h('strong', {}, '部署失敗／服務無回應'),
              st.deploy_stage ? `（失敗階段：${st.deploy_stage}）` : ''),
            st.deploy_error ? h('div', { class: 'muted' }, st.deploy_error) : null,
            h('div', { class: 'spacer' }),
            buttons(
              button('重試套用', () => ctx.apply(), { primary: true, disabled: ctx.isApplying() }),
              button('重新檢查服務狀態', () => this.refreshLexiconStatus(ctx, true))
            ))
        );
      }
      if (lex.enabled && st.schema_applicable === false) {
        belowItems.push(
          h('div', { class: 'lex-alert warn' },
            '目前選用的輸入方案未使用此詞庫（台灣詞庫適用於：注音、注音快打、注音臺灣正體）。您可在「輸入方案」頁勾選注音方案。')
        );
      }
      if (lex.enabled && info.resource_ok && !st.deploy_failed && !st.service_confirmed && !isDirty) {
        belowItems.push(
          h('div', { class: 'lex-alert info' },
            st.deployed_on_disk
              ? '詞庫檔案已部署完成，但尚未收到執行中輸入法服務載入此版本的確認回報（可切換至輸入法打字或按「重新檢查狀態」確認）。'
              : '詞庫設定尚未完成部署，請按下方「套用」完成部署。')
        );
      }
      if (!lex.enabled) {
        belowItems.push(
          h('div', { class: 'lex-alert muted', 'data-testid': 'lexicon-disable-notice' },
            '停用後，這份詞庫不再提供詞彙；個人學習或其他詞庫仍可能提供相同的詞。停用不會刪除詞庫來源、授權檔、個人修改或個人學習資料。')
        );
      }

      belowItems.push(
        h('div', { class: 'row lex-meta-row' },
          h('span', { class: 'muted grow' },
            '來源：小麥注音（McBopomofo）、新酷音（libchewing-data）、本專案補充詞 ｜ 適用方案：注音、注音（快打）、注音（臺灣正體）'),
          button('來源與授權…', () => this.showAttributionDialog(ctx)),
          button('重新檢查狀態', () => this.refreshLexiconStatus(ctx, true)))
      );

      statusCardSlot.replaceChildren(
        card({
          title: h('div', { class: 'row lex-header-row' },
            h('strong', {}, info.name || '台灣詞庫'),
            h('span', { class: 'muted' }, statsLine),
            h('span', { class: 'grow' }),
            h('div', { class: 'lex-badges' }, badgeEls)),
          desc: '隨程式提供的繁體中文（台灣）公開常用詞庫。切換開關或修改詞彙後，請按「套用」一次完成部署。',
          control: toggle(lex.enabled, (v) => {
            lex.enabled = v;
            ctx.syncLexiconDirty();
            this.updateStatusCard();
            this.updatePendingBar();
          }, { disabled: !info.resource_ok || ctx.isApplying(), label: lex.enabled ? '啟用' : '停用' }),
          below: belowItems,
        })
      );
    };

    this.updatePendingBar = () => {
      const isDirty = ctx.state.dirty.has('lexicon');
      const hasUndo = lex.undoStack && lex.undoStack.length > 0;
      if (!isDirty && !hasUndo) {
        pendingBarSlot.replaceChildren();
        return;
      }
      const enabledChanged = lex.enabled !== lex.savedEnabled;
      const parts = [];
      if (enabledChanged) parts.push(lex.enabled ? '啟用台灣詞庫' : '停用台灣詞庫');
      if (isDirty) parts.push(`個人修改共 ${lex.edits.length} 筆`);
      pendingBarSlot.replaceChildren(
        h('div', { class: 'lex-pending-bar', 'data-testid': 'lexicon-pending-bar' },
          h('div', { class: 'grow' },
            isDirty
              ? h('span', {}, h('strong', {}, '有未套用變更：'), parts.join('、') + '（重新部署期間輸入法會暫停片刻）')
              : h('span', { class: 'muted' }, '可撤回剛才的操作')),
          buttons(
            hasUndo ? button('撤回上一步', () => this.undoLastLexiconAction(ctx), { disabled: ctx.isApplying() }) : null,
            isDirty ? button('取消變更', () => {
              ctx.cancelLexiconChanges();
              this.searchLexicon(ctx, { resetPage: false });
            }, { disabled: ctx.isApplying() }) : null,
            isDirty ? button('套用', () => ctx.apply(), { primary: true, disabled: ctx.isApplying() }) : null
          ))
      );
    };

    // 搜尋輸入框獨立建立，搜尋時絕不重建 DOM，完整保留輸入焦點與游標位置
    const searchInput = input(lexQuery, (v) => {
      lexQuery = v;
      this.scheduleSearch(ctx);
    }, {
      placeholder: '搜尋詞彙、注音（如：發想、ㄈㄚ ㄒㄧㄤˇ）或拼音碼',
      width: '280px',
      class: 'lex-search-input',
    });

    const filterBarSlot = h('div', { class: 'row' });
    this.updateLexiconList = () => {
      const info = lex.info || {};
      const disabled = !info.resource_ok || ctx.isApplying();
      searchInput.disabled = !info.resource_ok;

      // 更新篩選器標籤與筆數
      const allCount = lexCounts.all || info.entries || 0;
      const filters = [
        { id: 'all', label: `全部 (${allCount.toLocaleString()})` },
        { id: 'modified', label: `只看我的修改 (${(lexCounts.modified || 0).toLocaleString()})` },
        { id: 'hidden', label: `已隱藏 (${(lexCounts.hidden || 0).toLocaleString()})` },
      ];
      const filterNodes = [
        h('div', { class: 'segmented', role: 'tablist', 'aria-label': '詞彙篩選' },
          filters.map((f) => h('button', {
            class: 'segment' + (lexFilter === f.id ? ' active' : ''),
            onclick: () => {
              if (lexFilter === f.id) return;
              lexFilter = f.id;
              this.searchLexicon(ctx, { resetPage: true });
            },
          }, f.label))),
      ];
      if (lexCounts.conflicts > 0) {
        filterNodes.push(
          h('span', { class: 'lex-badge danger' }, `⚠ 上游移除衝突：${lexCounts.conflicts} 筆`)
        );
      }
      filterBarSlot.replaceChildren(...filterNodes);

      if (lexError) {
        listSlot.replaceChildren(
          h('div', { class: 'lex-alert danger', role: 'alert' }, `載入失敗：${lexError}`)
        );
        return;
      }
      if (lexLoading && !lexRows.length) {
        listSlot.replaceChildren(
          h('div', { class: 'listbox list-empty', style: { height: '280px' } }, '正在搜尋詞庫…')
        );
        return;
      }
      if (!lexRows.length) {
        listSlot.replaceChildren(
          h('div', { class: 'listbox list-empty', style: { height: '220px' } },
            lexQuery ? `查無符合「${lexQuery}」的詞彙。` : '目前篩選條件下沒有詞彙項目。')
        );
        return;
      }

      const chosen = new Set(lexSelectedKeys);
      let anchor = null;

      const rowEls = lexRows.map((row, idx) => {
        const key = rowKey(row.word, row.code);
        const isSelected = chosen.has(key);
        const badge = statusBadgeInfo(row);
        const toggleRow = (multi, range) => {
          if (range && anchor !== null) {
            chosen.clear();
            const [a, b] = [Math.min(anchor, idx), Math.max(anchor, idx)];
            for (let i = a; i <= b; ++i) chosen.add(rowKey(lexRows[i].word, lexRows[i].code));
          } else if (multi) {
            chosen.has(key) ? chosen.delete(key) : chosen.add(key);
            anchor = idx;
          } else {
            chosen.clear();
            chosen.add(key);
            anchor = idx;
          }
          lexSelectedKeys = [...chosen];
          refreshSelection();
        };

        const tr = h('div', {
          class: 'lex-row' + (isSelected ? ' selected' : '') + (row.hidden ? ' is-hidden' : ''),
          role: 'option',
          'aria-selected': isSelected ? 'true' : 'false',
          'data-row-key': key,
          onclick: (e) => toggleRow(e.ctrlKey || e.metaKey, e.shiftKey),
        },
          h('div', { class: 'lex-cell lex-col-check' },
            h('input', {
              type: 'checkbox',
              checked: isSelected,
              'aria-label': `選取 ${row.word}（${row.zhuyin}）`,
              onclick: (e) => {
                e.stopPropagation();
                toggleRow(true, e.shiftKey);
              },
            })),
          h('div', { class: 'lex-cell lex-col-word' },
            h('span', { class: 'lex-word-text' }, row.word),
            row.replaced_from_word
              ? h('span', { class: 'lex-subtext' }, `原：${row.replaced_from_word}（${row.replaced_from_zhuyin || row.replaced_from_code}）`)
              : null),
          h('div', { class: 'lex-cell lex-col-reading' },
            h('span', { class: 'lex-zhuyin-text' }, row.zhuyin),
            h('span', { class: 'lex-subtext' }, row.code)),
          h('div', { class: 'lex-cell lex-col-pref' },
            h('span', { class: 'lex-pref-tag ' + (row.pref || 'normal') }, `排序：${prefLabel(row.pref)}`),
            h('span', { class: 'lex-subtext', title: 'Rime 詞庫基礎排序權重（非個人打字次數）' }, `基礎權重 ${row.effective_weight}`)),
          h('div', { class: 'lex-cell lex-col-status' },
            h('span', { class: `lex-badge ${badge.cls}` }, badge.text),
            row.dirty ? h('span', { class: 'lex-badge accent' }, '待套用') : null),
          h('div', { class: 'lex-cell lex-col-actions' },
            button('修改', (e) => {
              e.stopPropagation();
              lexSelectedKeys = [key];
              refreshSelection();
              this.startModifyRow(ctx, row);
            }, { disabled }),
            !row.hidden
              ? button('隱藏', (e) => {
                e.stopPropagation();
                lexSelectedKeys = [key];
                this.hideSelectedRows(ctx);
              }, { disabled })
              : null,
            row.modified
              ? button('還原內建值', (e) => {
                e.stopPropagation();
                lexSelectedKeys = [key];
                this.restoreSelectedRows(ctx);
              }, { disabled })
              : null)
        );
        return tr;
      });

      const refreshSelection = () => {
        const s = new Set(lexSelectedKeys);
        rowEls.forEach((el, i) => {
          const k = rowKey(lexRows[i].word, lexRows[i].code);
          const sel = s.has(k);
          el.classList.toggle('selected', sel);
          el.setAttribute('aria-selected', sel ? 'true' : 'false');
          const cb = el.querySelector('input[type="checkbox"]');
          if (cb) cb.checked = sel;
        });
      };

      const listContainer = h('div', {
        class: 'lex-table-box',
        tabIndex: 0,
        role: 'listbox',
        'aria-label': '台灣詞庫詞彙清單',
        onkeydown: (e) => {
          if (disabled || !lexRows.length) return;
          const curIdx = lexRows.findIndex((r) => chosen.has(rowKey(r.word, r.code)));
          if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
            e.preventDefault();
            const next = Math.max(0, Math.min(lexRows.length - 1, (curIdx < 0 ? 0 : curIdx) + (e.key === 'ArrowDown' ? 1 : -1)));
            const k = rowKey(lexRows[next].word, lexRows[next].code);
            chosen.clear();
            chosen.add(k);
            lexSelectedKeys = [k];
            anchor = next;
            refreshSelection();
            rowEls[next].scrollIntoView({ block: 'nearest' });
          } else if (e.key === 'F2' || e.key === 'Enter') {
            if (curIdx >= 0) {
              e.preventDefault();
              this.startModifyRow(ctx, lexRows[curIdx]);
            }
          } else if (e.key === 'Delete') {
            if (lexSelectedKeys.length) {
              e.preventDefault();
              this.hideSelectedRows(ctx);
            }
          }
        },
      },
        h('div', { class: 'lex-table-header' },
          h('div', { class: 'lex-col-check' }, ''),
          h('div', { class: 'lex-col-word' }, '詞彙'),
          h('div', { class: 'lex-col-reading' }, '讀音（注音／代碼）'),
          h('div', { class: 'lex-col-pref' }, '排序偏好／基礎權重'),
          h('div', { class: 'lex-col-status' }, '修改狀態'),
          h('div', { class: 'lex-col-actions' }, '操作')),
        h('div', { class: 'lex-table-body' }, rowEls)
      );

      const totalPages = Math.max(1, Math.ceil(lexTotal / kLexPageSize));
      const curPage = Math.floor(lexOffset / kLexPageSize) + 1;
      const pager = h('div', { class: 'row lex-pager' },
        h('span', { class: 'muted grow' },
          `顯示第 ${(lexOffset + 1).toLocaleString()}–${Math.min(lexTotal, lexOffset + lexRows.length).toLocaleString()} 筆，共 ${lexTotal.toLocaleString()} 筆（基礎權重僅供 Rime 靜態排序參考，非個人使用次數）`),
        button('上一頁', () => {
          if (lexOffset >= kLexPageSize) {
            lexOffset -= kLexPageSize;
            this.searchLexicon(ctx, { resetPage: false });
          }
        }, { disabled: lexOffset === 0 || lexLoading }),
        h('span', { class: 'muted' }, `第 ${curPage} / ${totalPages} 頁`),
        button('下一頁', () => {
          if (lexOffset + kLexPageSize < lexTotal) {
            lexOffset += kLexPageSize;
            this.searchLexicon(ctx, { resetPage: false });
          }
        }, { disabled: lexOffset + kLexPageSize >= lexTotal || lexLoading })
      );

      listSlot.replaceChildren(listContainer, pager);
    };

    this.updateFormBox = () => {
      const info = lex.info || {};
      const disabled = !info.resource_ok || ctx.isApplying();
      const isModify = lexFormMode === 'modify';

      this.formWordInput = input(lexFormWord, (v) => { lexFormWord = v; }, {
        disabled,
        maxlength: 12,
        placeholder: '詞彙（1～12 個漢字）',
        width: '160px',
        onenter: () => this.submitLexiconForm(ctx),
      });
      const readingInput = input(lexFormReading, (v) => { lexFormReading = v; }, {
        disabled,
        maxlength: 80,
        placeholder: '完整注音（如：ㄈㄚ ㄒㄧㄤˇ）',
        width: '210px',
        onenter: () => this.submitLexiconForm(ctx),
      });
      const prefSelect = select([
        { value: 'low', label: '排序：降低' },
        { value: 'normal', label: '排序：標準' },
        { value: 'high', label: '排序：提高' },
      ], lexFormPref, (v) => { lexFormPref = v; }, { disabled, width: '130px' });

      formSlot.replaceChildren(
        h('div', { class: 'lex-editor-box' },
          h('div', { class: 'field-label' },
            isModify
              ? `修改項目：原「${lexFormOrigWord}（${lexFormOrigZhuyin}）」— 可直接修改讀音、改寫法或調整排序偏好（原項將自動隱藏並新增替代項，於同一次套用生效）`
              : '新增或修改詞彙（需填寫詞彙與完整注音，音節數須與字數一致）'),
          h('div', { class: 'row' },
            this.formWordInput,
            readingInput,
            prefSelect,
            button(isModify ? '儲存修改' : '加入詞彙', () => this.submitLexiconForm(ctx), { primary: true, disabled }),
            isModify ? button('取消修改', () => {
              lexFormMode = 'add';
              lexFormOrigWord = '';
              lexFormOrigCode = '';
              lexFormOrigZhuyin = '';
              lexFormWord = '';
              lexFormReading = '';
              lexFormPref = 'normal';
              this.updateFormBox();
            }) : null),
          h('div', { class: 'spacer' }),
          h('div', { class: 'row' },
            button('修改所選…', () => {
              if (!lexSelectedKeys.length) {
                ctx.status('請先在清單選取一筆要修改的詞彙。', 'error');
                return;
              }
              const row = this.findRow(lexSelectedKeys[0]);
              if (row) this.startModifyRow(ctx, row);
            }, { disabled }),
            button('隱藏此項', () => this.hideSelectedRows(ctx), { disabled }),
            button('還原內建值', () => this.restoreSelectedRows(ctx), { disabled }),
            select([
              { value: '', label: '批次調整排序偏好…' },
              { value: 'high', label: '設為「提高」' },
              { value: 'normal', label: '設為「標準」' },
              { value: 'low', label: '設為「降低」' },
            ], '', (v) => {
              if (v) this.changeSelectedPref(ctx, v);
            }, { disabled, width: '165px' }),
            h('span', { class: 'grow' }),
            button('匯出個人修改…', () => this.exportLexiconEdits(ctx), { disabled: !lex.edits.length }),
            button('匯入個人修改…', () => this.importLexiconEdits(ctx), { disabled }),
            button('全部還原內建值…', () => this.restoreAllEdits(ctx), { danger: true, disabled: !lex.edits.length || disabled })),
          h('div', { class: 'note' },
            '提示：「隱藏此項」僅使台灣詞庫不再提供該組詞與讀音，並非全域封鎖；「排序偏好」僅調整本詞庫基礎權重參考，不會覆蓋個人學習紀錄。所有內建詞庫變更須按「套用」重新部署後生效。')
        )
      );
    };

    this.updateStatusCard();
    this.updatePendingBar();
    this.updateLexiconList();
    this.updateFormBox();

    return [
      statusCardSlot,
      pendingBarSlot,
      section('台灣詞庫詞彙管理（需按「套用」部署生效）',
        card({
          below: [
            h('div', { class: 'row' },
              '搜尋',
              searchInput,
              lexQuery ? button('清除搜尋', () => {
                lexQuery = '';
                searchInput.value = '';
                this.searchLexicon(ctx, { resetPage: true });
                searchInput.focus();
              }) : null,
              filterBarSlot),
            h('div', { class: 'spacer' }),
            listSlot,
            h('div', { class: 'spacer' }),
            formSlot,
          ],
        })),
    ];
  },

  renderPersonalSection(ctx) {
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
    const ruleText = (r) => (r.kind === 'merge' ? '合併　' + r.from + ' → ' + r.to
      : r.kind === 'split' ? '拆解　' + r.from + ' → ' + r.to.split(' ').join('／')
        : r.kind === 'add' ? '加入　' + r.from : '封鎖　' + r.from);
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

    return [
      section('個人詞庫的詞彙與精煉規則（操作立即送交輸入法生效）',
        card({
          below: [
            h('div', { class: 'row' }, '搜尋', input(filter, (v) => {
              filter = v;
              renderWords();
            }, { disabled, width: '220px' }), countEl,
            button('重新整理', () => this.loadWords(ctx)),
            button('個人詞庫設定…', () => ctx.go('personal'))),
            h('div', { class: 'spacer' }),
            h('div', { class: 'grid two' },
              wordBox,
              h('div', {}, h('div', { class: 'field-label' }, '規則（加入／封鎖／合併／拆解）'), ruleList)),
            h('div', { class: 'spacer' }),
            h('div', { class: 'row' },
              wordInput,
              button('加入', () => {
                const w = validWord('請先輸入要加入的詞（40 字以內）。');
                if (w) this.edit(ctx, ['A\t' + w]);
              }, { disabled }),
              button('修改…', () => {
                if (!need(selectedWords, '請先選取要修改的詞。')) return;
                this.rename(ctx, selectedWords[0]);
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
                  (r.kind === 'merge' ? 'X\t' : r.kind === 'split' ? 'Z\t' : r.kind === 'add' ? 'Y\t' : 'U\t') + r.from));
              }, { disabled })),
            h('div', { class: 'note' }, '選取後按「修改…」（或雙擊、按 F2）可直接修改。「刪除」只移除這次，之後打到還會再學；「封鎖」僅在個人學習與精煉中刪除且不再學習（非 Rime 全域候選封鎖）；修改與合併會記成規則，「重新精煉全部」時也會套用。'),
          ],
        })),
    ];
  },

  render(ctx) {
    const userDictSectionId = 'rime-user-dict-section';
    const dictList = dicts === null
      ? h('div', { class: 'listbox list-empty', style: { height: '160px' } }, '讀取中…')
      : listbox(dicts.map((d) => ({ key: d, label: d })), selectedDict, (d) => {
        selectedDict = d;
        ctx.rerender();
      }, { height: '160px', empty: '沒有使用者詞典。' });

    const isTaiwan = currentSource === 'taiwan';
    const lexDirty = ctx.state.dirty.has('lexicon');

    const sourceHeader = card({
      below: [
        h('div', { class: 'row' },
          h('strong', {}, '管理詞庫來源'),
          select([
            { value: 'taiwan', label: '台灣詞庫（內建）' + (lexDirty ? ' — 有未套用變更' : '') },
            { value: 'personal', label: '個人詞彙（學習與精煉規則）' },
          ], currentSource, (v) => {
            currentSource = v;
            ctx.rerender();
            if (v === 'taiwan') this.searchLexicon(ctx, { resetPage: false });
            else if (!data) this.loadWords(ctx);
          }, { width: '260px' }),
          h('div', { class: 'segmented', role: 'tablist', 'aria-label': '快速切換詞庫來源' },
            h('button', {
              class: 'segment' + (isTaiwan ? ' active' : ''),
              onclick: () => {
                if (currentSource === 'taiwan') return;
                currentSource = 'taiwan';
                ctx.rerender();
                this.searchLexicon(ctx, { resetPage: false });
              },
            }, '台灣詞庫（內建）'),
            h('button', {
              class: 'segment' + (!isTaiwan ? ' active' : ''),
              onclick: () => {
                if (currentSource === 'personal') return;
                currentSource = 'personal';
                ctx.rerender();
                if (!data) this.loadWords(ctx);
              },
            }, '個人詞彙')),
          h('span', { class: 'grow' }),
          button('使用者詞典備份／匯入匯出 ↓', () => {
            const el = document.getElementById(userDictSectionId);
            if (el) el.scrollIntoView({ behavior: 'smooth', block: 'start' });
          })),
        h('div', { class: 'note' },
          isTaiwan
            ? '目前管理：隨程式提供的公開「台灣詞庫」與你的個人修改紀錄。此處的開關與修改需要按「套用」重新部署後生效（跨分頁切換會保留未套用變更）。'
            : '目前管理：個人打字學習詞彙與 LLM 精煉規則。此處的加入、修改、刪除、合併與封鎖會立即送交執行中的輸入法生效。'),
      ],
    });

    const activeSourceContent = isTaiwan
      ? this.renderTaiwanSection(ctx)
      : this.renderPersonalSection(ctx);

    const rimeDictSection = h('div', { id: userDictSectionId },
      section('輸入法使用者詞典（Rime 備份／匯入匯出）',
        card({
          below: [
            dictList,
            h('div', { class: 'spacer' }),
            buttons(
              button('備份', () => this.dictCommand(ctx, 'backup'), { disabled: !selectedDict }),
              button('還原…', () => this.dictCommand(ctx, 'restore')),
              button('匯出文字碼表…', () => this.dictCommand(ctx, 'export'), { disabled: !selectedDict }),
              button('匯入文字碼表…', () => this.dictCommand(ctx, 'import'), { disabled: !selectedDict })),
            note('備份／還原是整個 Rime 使用者詞典（userdb）的快照；匯出／匯入為文字碼表，與上方「台灣詞庫個人修改 (.json)」格式獨立不混用。操作時輸入法會暫停片刻。'),
          ],
        })));

    return [
      sourceHeader,
      ...activeSourceContent,
      rimeDictSection,
    ];
  },
};
