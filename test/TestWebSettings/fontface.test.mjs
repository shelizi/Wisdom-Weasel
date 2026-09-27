// 字型字串的解析與組合：node test/TestWebSettings/fontface.test.mjs
import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const f = await import(pathToFileURL(path.join(here, '../../web/settings/js/fontface.js')));

let passed = 0;
function test(name, fn) {
  fn();
  ++passed;
}

test('normalize', () => {
  assert.equal(f.normalize(' Segoe UI : 0 : 7f ,  微軟正黑體 , '), 'Segoe UI:0:7f,微軟正黑體');
});

test('weight and style tokens', () => {
  assert.equal(f.getToken('Segoe UI:bold:italic, X', 'weight'), 'bold');
  assert.equal(f.getToken('Segoe UI:semi_light', 'weight'), 'semi_light');
  assert.equal(f.getToken('Segoe UI:extra_bold', 'weight'), 'extra_bold');
  assert.equal(f.getToken('Segoe UI', 'weight'), 'regular');
  assert.equal(f.getToken('Segoe UI:italic', 'style'), 'italic');
  assert.equal(f.getToken('Segoe UI', 'style'), 'normal');
});

test('setToken inserts after the first font name, also for CJK names', () => {
  assert.equal(f.setToken('Segoe UI:0:7f, 微軟正黑體', 'weight', 'bold'), 'Segoe UI:bold:0:7f, 微軟正黑體');
  assert.equal(f.setToken('微軟正黑體, Segoe UI', 'weight', 'bold'), '微軟正黑體:bold, Segoe UI');
  assert.equal(f.setToken('微軟正黑體', 'style', 'italic'), '微軟正黑體:italic');
  // 換成別的值、改回預設
  assert.equal(f.setToken('Segoe UI:bold, X', 'weight', 'light'), 'Segoe UI:light, X');
  assert.equal(f.setToken('Segoe UI:bold:0:7f, X', 'weight', 'regular'), 'Segoe UI:0:7f, X');
  assert.equal(f.setToken('Segoe UI:bold:italic', 'style', 'normal'), 'Segoe UI:bold');
});

test('addFont matches the old dialog', () => {
  assert.equal(f.addFont('', 'Segoe UI', null), 'Segoe UI');
  assert.equal(f.addFont('Segoe UI', '微軟正黑體', null), 'Segoe UI, 微軟正黑體');
  assert.equal(f.addFont('A', 'B', { start: '0', end: '7f' }), 'A, B::7f');
  assert.equal(f.addFont('A', 'B', { start: '000', end: '10ffff' }), 'A, B::10ffff');
  assert.equal(f.addFont('A', 'B', { start: '4e00', end: '9fff' }), 'A, B:4e00:9fff');
  assert.equal(f.addFont('A', 'B', { start: '4e00', end: '' }), 'A, B:4e00');
  assert.equal(f.addFont('A', '  ', null), 'A');
});

test('parse ranges like the renderer', () => {
  const p = f.parse('Segoe UI:bold:0:7f, 微軟正黑體:4e00, Emoji::ffff, Bad:zz:yy, Too:1:2:3');
  assert.equal(p.weight, 'bold');
  assert.deepEqual(p.fonts, [
    { name: 'Segoe UI', first: 0, last: 0x7f },
    { name: '微軟正黑體', first: 0x4e00, last: 0x10ffff },
    { name: 'Emoji', first: 0, last: 0xffff },
    { name: 'Bad', first: 0, last: 0x10ffff },  // 不是十六進位：用預設範圍
  ]);
});

test('preview css', () => {
  const css = f.previewCss('Segoe UI:bold:0:7f, 微軟正黑體', 'p');
  assert.match(css.rules, /font-family: "p-0"; src: local\("Segoe UI"\); unicode-range: U\+0-7f;/);
  assert.match(css.rules, /font-family: "p-1"; src: local\("微軟正黑體"\); unicode-range: U\+0-10ffff;/);
  assert.equal(css.fontFamily, '"p-0", "p-1", "Segoe UI", "微軟正黑體", sans-serif');
  assert.equal(css.fontWeight, 700);
  assert.equal(css.fontStyle, 'normal');
});

test('simple mode', () => {
  assert.equal(f.simpleFont(''), '');
  assert.equal(f.simpleFont('Microsoft JhengHei'), 'Microsoft JhengHei');
  assert.equal(f.simpleFont('微軟正黑體:bold:italic'), '微軟正黑體');
  assert.equal(f.simpleFont('A::10ffff'), 'A');
  assert.equal(f.simpleFont('A, B'), null);
  assert.equal(f.simpleFont('A:4e00:9fff'), null);
  assert.equal(f.withFont('A:bold:italic', 'B'), 'B:bold:italic');
  assert.equal(f.withFont('A, B:0:7f', 'C'), 'C');
  assert.equal(f.withFont('A:bold', ''), '');
});

console.log(`all passed (${passed})`);
