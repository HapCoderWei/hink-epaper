'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const root = path.resolve(__dirname, '..');
const html = fs.readFileSync(path.join(root, 'site', 'index.html'), 'utf8');
const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map(match => match[1]);

assert.strictEqual(new Set(ids).size, ids.length, 'page IDs must be unique');

for (const id of [
  'preview', 'file', 'image-drop', 'fit', 'processing', 'rotate-left',
  'rotate-right', 'connect', 'connect-all', 'send', 'progress', 'status',
  'firmware', 'ota-stage', 'ota-progress', 'ota-status', 'ota-install-panel',
  'ota-power-confirm', 'ota-arm', 'ota-install', 'connection-pill'
]) {
  assert(ids.includes(id), `missing required control #${id}`);
}

assert(/<details class="ota">/.test(html), 'firmware maintenance must be collapsed initially');
assert(/id="ota-install-panel" hidden/.test(html), 'install controls must stay hidden until verification');
assert(/id="preview" width="250" height="122"/.test(html), 'preview must use the landscape 250 x 122 view');
assert(/const DEVICE_WIDTH = 122, DEVICE_HEIGHT = 250/.test(html), 'BLE output must retain the native panel dimensions');
assert(/const landscapeX = DEVICE_HEIGHT - 1 - y/.test(html), 'BLE output must reverse the landscape x axis into native rows');
assert(/const landscapeY = x/.test(html), 'BLE output must map native columns into landscape y');

const nativeToLandscape = (x, y) => [249 - y, x];
assert.deepStrictEqual(nativeToLandscape(0, 249), [0, 0], 'native bottom-left must become landscape top-left');
assert.deepStrictEqual(nativeToLandscape(0, 0), [249, 0], 'native top-left must become landscape top-right');
assert.deepStrictEqual(nativeToLandscape(121, 249), [0, 121], 'native bottom-right must become landscape bottom-left');
assert.deepStrictEqual(nativeToLandscape(121, 0), [249, 121], 'native top-right must become landscape bottom-right');
assert(/id="status"[^>]*aria-live="polite"|role="status"[^>]*aria-live="polite"[\s\S]*id="status"/.test(html),
  'connection status must be announced accessibly');

const inlineScripts = [...html.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/g)]
  .map(match => match[1].trim())
  .filter(Boolean);
assert.strictEqual(inlineScripts.length, 1, 'expected one inline application script');
new vm.Script(inlineScripts[0], { filename: 'site/index.html' });

console.log('PASS: modern web structure, safety gates, unique controls, and inline script syntax.');
