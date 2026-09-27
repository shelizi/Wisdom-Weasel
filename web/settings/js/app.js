import { call, on } from './bridge.js';
import { state, loadLlm, collectChanges, validate } from './state.js';
import { h, confirm, button } from './ui.js';
import schemas from './pages/schemas.js';
import style from './pages/style.js';
import predict from './pages/predict.js';
import typo from './pages/typo.js';
import choice from './pages/choice.js';
import models from './pages/models.js';
import personal from './pages/personal.js';
import dict from './pages/dict.js';

// 順序與原本設定視窗的分頁相同（--page 參數用的編號）
const pages = [schemas, style, predict, typo, choice, models, personal, dict];
const params = new URLSearchParams(location.search);
let current = null;
let applying = false;

const nav = document.getElementById('nav');
const content = document.getElementById('content');
const statusEl = document.getElementById('status');

// ---------------------------------------------------------------------------
// 給各頁用的共用功能

export const ctx = {
  state,
  call,
  on,
  status(text, kind = '') {
    statusEl.textContent = text || '';
    statusEl.className = 'status ' + kind;
  },
  markDirty(category) {
    state.dirty.add(category);
    updateButtons();
  },
  go(id, options) {
    const page = pages.find((p) => p.id === id);
    if (page) show(page, options);
  },
  // 重新畫目前這頁（結構有變時）；捲動位置不變
  rerender() {
    if (!current) return;
    const scroll = content.scrollTop;
    renderPage(current);
    content.scrollTop = scroll;
  },
  isCurrent(page) {
    return current === page;
  },
};

// ---------------------------------------------------------------------------
// 主題：0 跟隨系統、1 淺色、2 深色

const media = window.matchMedia('(prefers-color-scheme: dark)');
let themePref = 0;

function applyTheme() {
  const dark = themePref === 2 || (themePref === 0 && media.matches);
  document.documentElement.dataset.theme = dark ? 'dark' : 'light';
  call('app.titleBar', { dark }).catch(() => {});
}
media.addEventListener('change', () => themePref === 0 && applyTheme());

function themeSelector() {
  const select = h('select', { class: 'select nav-theme', 'aria-label': '設定視窗主題', onchange: () => {
    themePref = Number(select.value);
    applyTheme();
    call('app.setTheme', { theme: themePref }).catch(() => {});
  } }, ['跟隨系統', '淺色', '深色'].map((label, i) => h('option', { value: i, selected: i === themePref }, label)));
  return h('div', { class: 'nav-footer' }, h('div', { class: 'nav-footer-label' }, '設定視窗主題'), select);
}

// ---------------------------------------------------------------------------
// 導覽與分頁

function renderNav() {
  nav.replaceChildren(
    h('div', { class: 'nav-title' }, '小狼毫設定'),
    h('div', { class: 'nav-items' }, pages.map((page) =>
      h('button', {
        class: 'nav-item' + (page === current ? ' active' : ''),
        onclick: () => show(page),
      }, h('span', { class: 'nav-icon', 'aria-hidden': 'true' }, page.icon), h('span', {}, page.nav)))),
    themeSelector());
}

function renderPage(page) {
  content.replaceChildren(
    h('header', { class: 'page-header' }, h('h1', {}, page.title), h('p', { class: 'page-desc' }, page.desc)),
    h('div', { class: 'page-body' }, page.render(ctx)));
}

function show(page, options = {}) {
  if (current && current !== page && current.onHide) current.onHide(ctx);
  current = page;
  if (options.profile !== undefined) state.selectedProfile = options.profile;
  renderNav();
  renderPage(page);
  content.scrollTop = 0;
  if (page.onShow) page.onShow(ctx);
}

// ---------------------------------------------------------------------------
// 套用

const applyButton = document.getElementById('apply');
const okButton = document.getElementById('ok');
const cancelButton = document.getElementById('cancel');

function updateButtons() {
  applyButton.disabled = applying || state.dirty.size === 0;
  okButton.disabled = applying;
}

async function apply() {
  if (applying) return false;
  if (state.dirty.size === 0) {
    ctx.status('沒有需要套用的變更。');
    return true;
  }
  if (state.dirty.has('schemas') && !state.schemas.some((s) => s.selected)) {
    ctx.go('schemas');
    ctx.status('請至少選用一個輸入方案。', 'error');
    return false;
  }
  if (state.dirty.has('llm')) {
    const problem = validate();
    if (problem) {
      ctx.go(problem.page, { profile: problem.profile });
      ctx.status(problem.message, 'error');
      return false;
    }
  }
  const changes = collectChanges();
  applying = true;
  updateButtons();
  ctx.status('正在重新部署…', 'busy');
  try {
    const result = await call('settings.apply', changes);
    state.dirty.clear();
    if (result.deployed) {
      const selectedProfile = state.selectedProfile;
      loadLlm(result.llm);
      state.selectedProfile = selectedProfile;
      state.form.choice.grammar = state.loaded.grammar = result.grammar.enabled;
      state.init.grammar = result.grammar;
      if (changes.schemas) state.init.schemas.list = state.schemas.map((s) => ({ ...s }));
      if (changes.style) state.init.style = { ...state.init.style, active: state.style.active, fonts: { ...state.style.fonts } };
    }
    ctx.status(result.message, result.message.includes('；') ? 'warning' : 'ok');
    if (current) ctx.rerender();
    if (result.deployed) window.dispatchEvent(new Event('settings-applied'));
    return true;
  } catch (e) {
    ctx.status(e.message, 'error');
    return false;
  } finally {
    applying = false;
    updateButtons();
  }
}

async function requestClose() {
  if (state.dirty.size > 0 && !applying) {
    const discard = await confirm('有尚未套用的變更，要放棄這些變更並關閉嗎？', { title: '關閉設定', ok: '放棄變更', danger: true });
    if (!discard) return;
  }
  call('app.close');
}

applyButton.addEventListener('click', apply);
okButton.addEventListener('click', async () => {
  if (await apply()) call('app.close');
});
cancelButton.addEventListener('click', requestClose);
on('closeRequested', requestClose);
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && !document.querySelector('.overlay')) requestClose();
});

// ---------------------------------------------------------------------------
// 啟動

async function boot() {
  try {
    const init = await call('init');
    state.init = init;
    themePref = Number(params.get('theme') || init.theme || 0);
    applyTheme();
    state.schemas = init.schemas.list.map((s) => ({ ...s }));
    state.style = { active: init.style.active, fonts: { ...init.style.fonts } };
    loadLlm(init.llm);
    state.form.choice.grammar = state.loaded.grammar = init.grammar.enabled;
    const start = Math.max(0, Math.min(pages.length - 1, Number(params.get('page') || 0)));
    show(pages[start]);
    updateButtons();
  } catch (e) {
    content.replaceChildren(h('div', { class: 'fatal' }, h('h1', {}, '無法載入設定'), h('p', {}, e.message),
      button('關閉', () => call('app.close'))));
  }
  // 截圖模式等畫面穩定；一般模式讓視窗知道網頁已就緒
  call('app.ready');
}

boot();
