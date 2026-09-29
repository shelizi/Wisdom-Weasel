import { h, section, card, checkbox, button, buttons, select, input, note, listbox, confirm, lines } from '../ui.js';
import { newProfile, profileLabel, trim } from '../state.js';
import { profileUsage } from './profiles.js';

let scanned = [];        // 模型檔下拉選單的選項（完整路徑）
let files = null;        // 模型資料夾裡的檔案
let fileSel = null;      // 選取的模型檔（路徑）
let url = '';
let busy = false;        // 正在下載或複製
let jobText = '貼上 .gguf 的下載網址（Hugging Face 頁面網址也可以）';
let apiResult = null;    // {index, text}
let apiBusy = false;
let subscribed = false;

const fileName = (p) => String(p || '').split(/[\\/]/).pop();
const lower = (s) => String(s || '').toLowerCase();

export default {
  id: 'models',
  nav: '語言模型',
  icon: '',
  title: '語言模型',
  desc: '本機（llama.cpp）或 OpenAI 相容 API 的模型設定，可以設定多組。',

  onShow(ctx) {
    const { state } = ctx;
    if (state.selectedProfile == null || state.selectedProfile >= state.profiles.length)
      state.selectedProfile = state.profiles.length ? 0 : -1;
    if (!subscribed) {
      subscribed = true;
      ctx.on('modelJob', (e) => {
        jobText = e.message;
        if (e.state !== 'progress') {
          busy = false;
          if (e.state === 'done') {
            url = '';
            fileSel = e.path;
          }
          this.refreshFiles(ctx);
        } else {
          busy = true;
        }
        if (ctx.isCurrent(this)) {
          if (e.state === 'progress' && this.jobNote) this.jobNote.set(jobText);
          else ctx.rerender();
        }
      });
    }
    this.refreshFiles(ctx);
  },

  async refreshFiles(ctx) {
    const p = ctx.state.profiles[ctx.state.selectedProfile];
    try {
      [files, scanned] = await Promise.all([
        ctx.call('models.files'), ctx.call('models.scan', { current: p && !p.remote ? p.model_path : '' })]);
    } catch (e) {
      ctx.status(e.message, 'error');
    }
    if (ctx.isCurrent(this)) ctx.rerender();
  },

  render(ctx) {
    const { state } = ctx;
    const { profiles } = state;
    const index = state.selectedProfile ?? -1;
    const p = profiles[index];
    const changed = () => ctx.markDirty('llm');
    const local = state.form.local;

    // 左側：設定清單
    const list = listbox(profiles.map((q, i) => ({ key: i, label: profileLabel(q) })), index, (i) => {
      state.selectedProfile = i;
      apiResult = null;
      this.refreshFiles(ctx);
      ctx.rerender();
    }, { height: '300px', empty: '還沒有模型設定。' });
    const updateLabel = () => {
      const row = list.querySelectorAll('.list-label')[index];
      if (row) row.textContent = profileLabel(p);
    };
    const add = (copy) => {
      const q = copy && p ? { ...p, name: p.name + ' 複本' } : newProfile({ name: '新模型', model_type: 'Instruct' });
      profiles.push(q);
      state.selectedProfile = profiles.length - 1;
      changed();
      ctx.rerender();
      document.querySelector('.profile-name')?.select();
    };
    const remove = async () => {
      const { use } = state;
      let message = '確定要刪除「' + p.name + '」嗎？';
      if (use.predict === index || use.refine === index || use.typo === index || use.scorer === index)
        message += '\n\n這組設定正在使用中，刪除後請另外選擇模型。';
      if (!(await confirm(message, { title: '刪除模型設定', danger: true, ok: '刪除' }))) return;
      const shift = (i) => (i === index ? -1 : i > index ? i - 1 : i);
      use.predict = shift(use.predict);
      use.refine = shift(use.refine);
      use.typo = shift(use.typo);
      use.scorer = shift(use.scorer);
      profiles.splice(index, 1);
      state.selectedProfile = Math.min(index, profiles.length - 1);
      changed();
      ctx.rerender();
    };

    // 右側：編輯
    let editor;
    if (!p) {
      editor = h('div', { class: 'card' }, h('div', { class: 'muted' }, profileUsage(ctx, -1)));
    } else {
      const field = (label, control, extra) => h('div', { class: 'field' }, h('div', { class: 'field-label' }, label), control, extra);
      const setRemote = (remote) => {
        p.remote = remote;
        if (remote && !p.api_url) p.api_url = state.init.defaults.api_url;
        changed();
        ctx.rerender();
      };
      const modelOptions = [...scanned];
      if (p.model_path && !modelOptions.some((m) => lower(m) === lower(p.model_path))) modelOptions.push(p.model_path);
      const guessType = async (path) => {
        const type = await ctx.call('models.guessType', { path });
        if (type) p.model_type = type;
        ctx.rerender();
      };
      const local = [
        field('模型檔', h('div', { class: 'row' },
          h('div', { class: 'grow' }, select(
            [...(p.model_path ? [] : [{ value: '', label: '（請選擇模型檔）' }]), ...modelOptions.map((m) => ({ value: m, label: fileName(m) }))],
            p.model_path, (v) => {
              p.model_path = v;
              changed();
              guessType(v);
            }, { width: '100%' })),
          button('瀏覽…', async () => {
            const path = await ctx.call('models.pickFile');
            if (!path) return;
            if (!scanned.some((m) => lower(m) === lower(path))) scanned.push(path);
            p.model_path = path;
            changed();
            guessType(path);
          }))),
        field('模型類型', select([{ value: 'Base', label: 'Base' }, { value: 'Instruct', label: 'Instruct' }], p.model_type, (v) => {
          p.model_type = v;
          changed();
          ctx.rerender();
        })),
        h('div', { class: 'muted' }, 'Base 模型適合續寫預測；精煉需要能照指示回答的 Instruct（對話）模型。精煉用的模型和預測或校正已載入的是同一組（同一個檔、同樣類型）時直接借用，不再載入一份；否則精煉時另外載入一次，完成後釋放。'),
      ];
      const apiNote = note(apiResult && apiResult.index === index ? apiResult.text
        : '例如 https://api.openai.com/v1/chat/completions，或 Ollama 的 http://localhost:11434/v1/chat/completions。金鑰以明碼存在 weasel.custom.yaml。',
      apiResult && apiResult.index === index ? (apiResult.text.startsWith('✓') ? 'ok' : 'error') : '');
      const remote = [
        field('網址', input(p.api_url, (v) => { p.api_url = trim(v); changed(); }, { width: '100%' })),
        field('金鑰', input(p.api_key, (v) => { p.api_key = trim(v); changed(); }, { width: '100%', type: 'password' })),
        field('模型名稱', h('div', { class: 'row' },
          h('div', { class: 'grow' }, input(p.model, (v) => { p.model = trim(v); updateLabel(); changed(); }, { width: '100%' })),
          button(apiBusy ? '測試中…' : '測試連線', async () => {
            if (!p.api_url) {
              apiResult = { index, text: '請先填寫網址。' };
              ctx.rerender();
              return;
            }
            apiBusy = true;
            ctx.rerender();
            try {
              apiResult = { index, text: await ctx.call('api.test', { api_url: p.api_url, api_key: p.api_key, model: p.model }) };
            } catch (e) {
              apiResult = { index, text: '✗ ' + e.message };
            }
            apiBusy = false;
            if (ctx.isCurrent(this)) ctx.rerender();
          }, { disabled: apiBusy }))),
        apiNote,
      ];
      editor = h('div', { class: 'card' },
        field('名稱', input(p.name, (v) => { p.name = trim(v); updateLabel(); changed(); }, { width: '100%', class: 'profile-name' })),
        field('執行位置', h('div', { class: 'row' },
          h('label', { class: 'check' }, h('input', { type: 'radio', name: 'where', checked: !p.remote, onchange: () => setRemote(false) }), h('span', {}, '本機（llama.cpp）')),
          h('span', { style: { width: '16px' } }),
          h('label', { class: 'check' }, h('input', { type: 'radio', name: 'where', checked: p.remote, onchange: () => setRemote(true) }), h('span', {}, 'OpenAI 相容 API')))),
        p.remote ? remote : local,
        h('div', { class: 'spacer' }),
        checkbox(p.no_think, '關閉思考（思考型模型用）', (v) => { p.no_think = v; changed(); ctx.rerender(); }),
        h('div', { class: 'row', style: { marginTop: '8px' } },
          h('span', { class: p.no_think ? 'muted' : '' }, '思考長度上限'),
          input(String(p.think_tokens), (v) => {
            const text = trim(v);
            p.think_tokens = text === '' ? 2048 : Math.max(0, parseInt(text, 10) || 0);
            changed();
          }, { width: '90px', disabled: p.no_think }),
          h('span', { class: 'muted' }, 'token（0 = 不限制）')),
        h('div', { class: 'spacer' }),
        h('div', { class: 'note' }, lines(profileUsage(ctx, index))));
    }

    // 模型檔案
    const usedBy = (path) => profiles.filter((q) => !q.remote && lower(q.model_path) === lower(path)).map((q) => q.name);
    const fileList = files === null ? h('div', { class: 'listbox list-empty', style: { height: '160px' } }, '讀取中…')
      : listbox(files.map((f) => ({ key: f.path, label: f.name, detail: f.size, badge: usedBy(f.path).length ? '使用中' : null })),
        fileSel, (path) => {
          fileSel = path;
          deleteButton.disabled = busy || !fileSel;
        }, { height: '160px', empty: '模型資料夾裡還沒有 GGUF 模型檔。' });
    const deleteButton = button('移到回收筒', async () => {
      const f = files.find((x) => x.path === fileSel);
      if (!f) return;
      const users = usedBy(f.path);
      let message = '要把「' + f.name + '」移到資源回收筒嗎？';
      if (users.length) message += '\n\n這個檔案正被模型設定「' + users.join('、') + '」使用，移除後請改選其他模型檔。';
      if (!(await confirm(message, { title: '移除模型檔', ok: '移到回收筒', danger: true }))) return;
      try {
        jobText = await ctx.call('models.delete', { path: f.path });
        fileSel = null;
      } catch (e) {
        jobText = e.message;
      }
      this.refreshFiles(ctx);
    }, { disabled: busy || !fileSel });
    const startJob = async (method, params) => {
      try {
        let r = await ctx.call(method, params);
        if (r && r.exists) {
          const replace = await confirm('模型資料夾已有「' + r.exists + '」，要' + (method === 'models.download' ? '重新下載並' : '') + '取代嗎？',
            { title: method === 'models.download' ? '下載模型' : '加入模型檔', ok: '取代' });
          if (!replace) return;
          r = await ctx.call(method, { ...params, replace: true });
        }
        if (r && r.started) {
          busy = true;
          ctx.rerender();
        }
      } catch (e) {
        jobText = e.message;
        ctx.rerender();
      }
    };
    this.jobNote = note(jobText);

    return [
      section(null, h('p', { class: 'muted' }, '設定可用的模型；「智慧預測」、「注音校正」與「個人詞庫」的精煉可以各自選用其中一組。')),
      section('模型設定',
        h('div', { class: 'grid two' },
          h('div', {}, list, h('div', { class: 'spacer' }),
            buttons(button('新增', () => add(false)), button('複製', () => add(true), { disabled: !p }),
              button('刪除', remove, { disabled: !p }))),
          editor)),
      section('本機模型的執行設定',
        card({
          title: '智慧預測的上下文長度',
          desc: '預測用的本機模型一次能看的 token 數。調大會多用一些記憶體（KV 快取），但精煉用同一個模型時比較放得下，可以直接借用，不必再載入一份模型（精煉一批要 4096 左右）。整句校正與精煉自己載入時各有固定的長度。',
          control: select([2048, 4096, 8192, 16384].map((n) => ({ value: n, label: String(n) })),
            local.n_ctx, (v) => { local.n_ctx = Number(v); changed(); }, { width: '100px' }),
        }),
        card({
          title: 'GPU 層數',
          desc: '放到顯示卡上執行的層數：-1 = 全部、0 = 只用 CPU。預測、整句校正與精煉的本機模型都用這個設定。',
          control: input(local.gpu_layers, (v) => { local.gpu_layers = v; changed(); }, { width: '80px' }),
        }),
        card({
          title: 'CPU 執行緒',
          desc: '用 CPU 計算時的執行緒數。',
          control: input(local.threads, (v) => { local.threads = v; changed(); }, { width: '80px' }),
        })),
      section('模型檔案',
        card({
          title: '模型檔案（使用者資料夾的 models）',
          desc: state.init.defaults.models_dir,
          below: [
            fileList,
            h('div', { class: 'spacer' }),
            buttons(
              button('加入檔案…', async () => {
                const path = await ctx.call('models.pickFile');
                if (path) startJob('models.add', { path });
              }, { disabled: busy }),
              deleteButton,
              button('開啟資料夾', () => ctx.call('shell.reveal', { path: fileSel || '' }))),
            h('div', { class: 'spacer' }),
            h('div', { class: 'row' },
              h('div', { class: 'grow' }, input(url, (v) => { url = v; }, { width: '100%', disabled: busy, placeholder: 'https://huggingface.co/…/resolve/main/model.gguf' })),
              button(busy ? '取消' : '下載', () => {
                if (busy) ctx.call('models.cancel');
                else startJob('models.download', { url });
              })),
            this.jobNote,
          ],
        })),
    ];
  },
};
