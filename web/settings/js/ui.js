// 介面元件：以 h() 建立元素，其餘是設定頁常用的控制項。

export function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs || {})) {
    if (value === undefined || value === null || value === false) continue;
    if (key === 'class') el.className = value;
    else if (key === 'style' && typeof value === 'object') Object.assign(el.style, value);
    else if (key.startsWith('on') && typeof value === 'function') el.addEventListener(key.slice(2), value);
    else if (key in el && typeof value !== 'string') el[key] = value;
    else el.setAttribute(key, value === true ? '' : value);
  }
  append(el, children);
  return el;
}

function append(el, children) {
  for (const child of children) {
    if (child === undefined || child === null || child === false) continue;
    if (Array.isArray(child)) append(el, child);
    else el.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
}

// 多行文字（\n 換行）
export function lines(text) {
  const parts = String(text || '').split('\n');
  return parts.flatMap((part, i) => (i ? [h('br'), part] : [part]));
}

// 區塊標題
export function section(title, ...children) {
  return h('section', { class: 'section' }, title ? h('h2', {}, title) : null, ...children);
}

// 一列設定：左邊標題與說明，右邊控制項
export function card({ title, desc, control, below, disabled, class: cls }) {
  const header = title || desc || control;
  return h(
    'div',
    { class: 'card' + (disabled ? ' disabled' : '') + (cls ? ' ' + cls : '') + (header ? '' : ' bare') },
    header && h('div', { class: 'card-row' },
      h('div', { class: 'card-text' },
        title ? h('div', { class: 'card-title' }, title) : null,
        desc ? h('div', { class: 'card-desc' }, lines(desc)) : null),
      control ? h('div', { class: 'card-control' }, control) : null),
    below ? h('div', { class: 'card-below' }, below) : null);
}

export function toggle(checked, onchange, { disabled, label } = {}) {
  const input = h('input', {
    type: 'checkbox', class: 'switch-input', checked: !!checked, disabled: !!disabled,
    onchange: () => onchange(input.checked),
  });
  return h('label', { class: 'switch' + (disabled ? ' disabled' : '') }, input,
    h('span', { class: 'switch-track' }, h('span', { class: 'switch-thumb' })),
    h('span', { class: 'switch-label' }, label ?? (checked ? '開' : '關')));
}

// 核取方塊與文字（放在卡片的標題位置）
export function checkbox(checked, text, onchange, { disabled } = {}) {
  const input = h('input', { type: 'checkbox', checked: !!checked, disabled: !!disabled, onchange: () => onchange(input.checked) });
  return h('label', { class: 'check' + (disabled ? ' disabled' : '') }, input, h('span', {}, text));
}

// options：[{value, label}]
export function select(options, value, onchange, { disabled, width } = {}) {
  const el = h('select', { class: 'select', disabled: !!disabled, style: width ? { width } : null, onchange: () => onchange(el.value) },
    options.map((o) => h('option', { value: String(o.value), selected: String(o.value) === String(value) }, o.label)));
  return el;
}

export function input(value, oninput, { placeholder, type = 'text', disabled, width, onenter, maxlength, class: cls } = {}) {
  const el = h('input', {
    class: 'input' + (cls ? ' ' + cls : ''), type, value: value ?? '', placeholder, disabled: !!disabled, maxlength,
    style: width ? { width } : null,
    oninput: () => oninput(el.value),
    onkeydown: (e) => { if (e.key === 'Enter' && onenter) onenter(el.value); },
  });
  return el;
}

export function textarea(value, oninput, { rows = 4, disabled, placeholder } = {}) {
  const el = h('textarea', { class: 'textarea', rows, disabled: !!disabled, placeholder, oninput: () => oninput(el.value) });
  el.value = value ?? '';
  return el;
}

export function button(label, onclick, { primary, danger, disabled, title } = {}) {
  return h('button', {
    class: 'button' + (primary ? ' primary' : '') + (danger ? ' danger' : ''),
    disabled: !!disabled, title, onclick,
  }, label);
}

export function buttons(...items) {
  return h('div', { class: 'buttons' }, ...items);
}

// 可選取的清單。items：[{key, label, detail?, badge?}]；multiple 時可多選（Ctrl／Shift）
export function listbox(items, selected, onselect, { multiple, height, empty, ondblclick, onkey } = {}) {
  const chosen = new Set(Array.isArray(selected) ? selected : selected == null ? [] : [selected]);
  let anchor = null;
  const el = h('div', { class: 'listbox', tabindex: 0, style: height ? { height } : null, role: 'listbox' });
  const rows = items.map((item, index) => {
    const row = h('div', { class: 'list-item' + (chosen.has(item.key) ? ' selected' : ''), role: 'option' },
      h('span', { class: 'list-label' }, item.label),
      item.badge ? h('span', { class: 'badge' }, item.badge) : null,
      item.detail ? h('span', { class: 'list-detail' }, item.detail) : null);
    row.addEventListener('mousedown', (e) => {
      if (multiple && e.ctrlKey) {
        chosen.has(item.key) ? chosen.delete(item.key) : chosen.add(item.key);
        anchor = index;
      } else if (multiple && e.shiftKey && anchor !== null) {
        chosen.clear();
        const [a, b] = [Math.min(anchor, index), Math.max(anchor, index)];
        for (let i = a; i <= b; ++i) chosen.add(items[i].key);
      } else {
        chosen.clear();
        chosen.add(item.key);
        anchor = index;
      }
      refresh();
      onselect(multiple ? [...chosen] : item.key);
    });
    if (ondblclick) row.addEventListener('dblclick', () => ondblclick(item.key));
    return row;
  });
  function refresh() {
    rows.forEach((row, i) => row.classList.toggle('selected', chosen.has(items[i].key)));
  }
  el.addEventListener('keydown', (e) => {
    if (onkey && onkey(e, [...chosen])) return;
    if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
    e.preventDefault();
    const current = items.findIndex((it) => chosen.has(it.key));
    const next = Math.max(0, Math.min(items.length - 1, current + (e.key === 'ArrowDown' ? 1 : -1)));
    if (!items[next]) return;
    chosen.clear();
    chosen.add(items[next].key);
    anchor = next;
    refresh();
    rows[next].scrollIntoView({ block: 'nearest' });
    onselect(multiple ? [...chosen] : items[next].key);
  });
  if (!items.length && empty) el.append(h('div', { class: 'list-empty' }, empty));
  el.append(...rows);
  // 選取的項目捲到看得見的地方（清單放進畫面之後）
  const first = rows.find((row, i) => chosen.has(items[i].key));
  if (first) requestAnimationFrame(() => first.scrollIntoView({ block: 'nearest' }));
  return el;
}

// 確認對話框：回傳 Promise<boolean>
export function confirm(message, { title = '確認', ok = '確定', cancel = '取消', danger } = {}) {
  return new Promise((resolve) => {
    const close = (value) => {
      overlay.remove();
      document.removeEventListener('keydown', onkey, true);
      resolve(value);
    };
    const onkey = (e) => {
      if (e.key === 'Escape') { e.stopPropagation(); close(false); }
    };
    const okButton = button(ok, () => close(true), { primary: !danger, danger });
    const overlay = h('div', { class: 'overlay' },
      h('div', { class: 'dialog', role: 'dialog' },
        h('h3', {}, title),
        h('div', { class: 'dialog-body' }, lines(message)),
        h('div', { class: 'dialog-buttons' }, okButton, button(cancel, () => close(false)))));
    document.body.append(overlay);
    document.addEventListener('keydown', onkey, true);
    okButton.focus();
  });
}

// 輸入對話框：回傳 Promise<string|null>（取消為 null）
export function prompt(message, value = '', { title = '輸入', ok = '確定', maxlength } = {}) {
  return new Promise((resolve) => {
    const close = (result) => {
      overlay.remove();
      resolve(result);
    };
    const field = h('input', {
      class: 'input wide', value, maxlength,
      onkeydown: (e) => {
        if (e.key === 'Enter') close(field.value);
        else if (e.key === 'Escape') { e.stopPropagation(); close(null); }
      },
    });
    const overlay = h('div', { class: 'overlay' },
      h('div', { class: 'dialog', role: 'dialog' },
        h('h3', {}, title),
        h('div', { class: 'dialog-body' }, h('div', { class: 'field-label' }, lines(message)), field),
        h('div', { class: 'dialog-buttons' }, button(ok, () => close(field.value), { primary: true }), button('取消', () => close(null)))));
    document.body.append(overlay);
    field.focus();
    field.select();
  });
}

// 顯示在控制項旁的說明或結果（可更新）
export function note(text = '', cls = '') {
  const el = h('div', { class: 'note ' + cls });
  el.set = (value, extra = '') => {
    el.className = 'note ' + extra;
    el.replaceChildren(...lines(value));
  };
  el.set(text, cls);
  return el;
}
