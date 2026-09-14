'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const root = path.resolve(__dirname, '..');
const html = fs.readFileSync(path.join(root, 'site', 'index.html'), 'utf8');
const firmwareHtml = fs.readFileSync(path.join(root, 'site', 'firmware.html'), 'utf8');
const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map(match => match[1]);

assert.strictEqual(new Set(ids).size, ids.length, 'page IDs must be unique');

for (const id of [
  'preview', 'file', 'image-drop', 'fit', 'processing', 'rotate-left',
  'rotate-right', 'connect', 'connect-all', 'send', 'progress', 'status',
  'firmware', 'ota-stage', 'ota-progress', 'ota-status', 'ota-install-panel',
  'ota-power-confirm', 'ota-arm', 'ota-install', 'ota-tools', 'connection-pill'
]) {
  assert(ids.includes(id), `missing required control #${id}`);
}

assert(/<details id="ota-tools" class="ota">/.test(html), 'firmware maintenance must be collapsed initially');
assert(/href="firmware\.html"/.test(html), 'control studio must link to firmware downloads');
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

for (const id of [
  'firmware', 'wiring', 'first-flash', 'ota', 'gallery'
]) {
  assert(new RegExp(`\\bid="${id}"`).test(firmwareHtml), `missing firmware page #${id}`);
}
assert(/script src="firmware\.js\?v=\d+"/.test(firmwareHtml), 'firmware page must load the external site script');
assert(!/<script(?!\s+src)[^>]*>/.test(firmwareHtml), 'firmware page must not use inline scripts (CSP)');
assert(!/\sstyle="/.test(firmwareHtml), 'firmware page must not use inline styles (CSP)');
const firmwareJs = fs.readFileSync(path.join(root, 'site', 'firmware.js'), 'utf8');
new vm.Script(firmwareJs, { filename: 'site/firmware.js' });
assert(firmwareJs.includes('prefers-reduced-motion'), 'site script must respect reduced motion');
assert(/B1 \/ v0\.1\.x/.test(firmwareHtml), 'firmware page must distinguish legacy OTA firmware');
assert(!/<[^>]+[“”][^>]*>/.test(firmwareHtml), 'firmware page tags must use valid ASCII attribute quotes');
assert(/href="index\.html#ota-tools"/.test(firmwareHtml), 'firmware page must link directly to the OTA tool');
assert(/0x000000/.test(firmwareHtml), 'firmware page must state the first SWire write address');
assert(/shasum -a 256/.test(firmwareHtml), 'firmware page must explain SHA-256 verification');
assert(/SWM → P6-3 SWS/.test(firmwareHtml), 'wiring must connect programmer SWM to tag SWS');
assert(/GND → P6-1 GND/.test(firmwareHtml), 'wiring must connect programmer ground to tag ground');
assert(/3\.3 V \/ PWR → P6-5 VCC/.test(firmwareHtml), 'programmer-power wiring must connect 3.3 V to tag VCC');
assert(/接线前先取下两颗电池/.test(firmwareHtml), 'wiring must require battery removal');
assert(/禁止 5 V/.test(firmwareHtml), 'wiring must reject 5 V power');
assert(/rf 0x000000 0x80000/.test(firmwareHtml), 'first flash must include a complete Flash backup');
assert(/we 0x000000 HINK_E0213A162_Community_v1\.0\.0\.bin/.test(firmwareHtml), 'first flash must write the curated image at zero');
for (const image of [
  'p6-pinout.jpg', 'original-front.jpg', 'original-back.jpg', 'community-result.jpg'
]) {
  assert(new RegExp(`assets/firmware/${image.replace('.', '\\.')}`).test(firmwareHtml),
    `firmware page must include ${image}`);
  assert(fs.existsSync(path.join(root, 'site', 'assets', 'firmware', image)),
    `missing firmware page image ${image}`);
}
for (const planningCopy of ['预留位置', '后续会用你提供', '待改造价签外观', '将标出']) {
  assert(!firmwareHtml.includes(planningCopy), `firmware page must not retain planning copy: ${planningCopy}`);
}
const firmwareNames = [...firmwareHtml.matchAll(/HINK_E0213A162_Community_[A-Za-z0-9_.-]+\.bin/g)].map(match => match[0]);
assert.deepStrictEqual([...new Set(firmwareNames)], ['HINK_E0213A162_Community_v1.0.0.bin'],
  'public page must contain exactly one curated firmware filename');

console.log('PASS: web studio, curated firmware, SWire wiring, update procedures, safety gates, and script syntax.');
