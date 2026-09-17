'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const zlib = require('zlib');

const root = path.resolve(__dirname, '..');
const html = fs.readFileSync(path.join(root, 'site', 'gallery-maker.html'), 'utf8');
const css = fs.readFileSync(path.join(root, 'site', 'gallery-maker.css'), 'utf8');
const js = fs.readFileSync(path.join(root, 'site', 'gallery-maker.js'), 'utf8');
const coreJs = fs.readFileSync(path.join(root, 'site', 'gallery-maker-core.js'), 'utf8');
const indexHtml = fs.readFileSync(path.join(root, 'site', 'index.html'), 'utf8');
const firmwareHtml = fs.readFileSync(path.join(root, 'site', 'firmware.html'), 'utf8');

// ── Page structure ──

// Unique IDs
const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map(m => m[1]);
assert.strictEqual(new Set(ids).size, ids.length, 'gallery-maker page IDs must be unique');

// Required controls
for (const id of [
  'edit-canvas', 'preview-canvas', 'file-input', 'drop-zone',
  'fit-select', 'processing-select', 'zoom-slider',
  'pos-x', 'pos-y', 'rotate-left', 'rotate-right',
  'slug-input', 'title-input', 'category-input',
  'download-btn', 'save-dir-btn', 'validation-msg',
  'status-bar', 'status-text',
  'color-white', 'color-black', 'color-red', 'output-size'
]) {
  assert(ids.includes(id), `missing required element #${id}`);
}

// Canvas dimensions
assert(/id="edit-canvas"[^>]*width="500"[^>]*height="244"/.test(html) ||
       /id="edit-canvas"[^>]*height="244"[^>]*width="500"/.test(html),
       'edit canvas must be 500×244');
assert(/id="preview-canvas"[^>]*width="250"[^>]*height="122"/.test(html) ||
       /id="preview-canvas"[^>]*height="122"[^>]*width="250"/.test(html),
       'preview canvas must be 250×122');

// No inline event handlers
assert(!/on(click|change|input|load|error|submit|keydown|keyup|mousedown|mouseup|touchstart|touchend|touchmove|dragover|drop|paste)\s*=/.test(html),
  'gallery-maker page must not use inline event handlers');

// No external dependencies
assert(!/<script[^>]+src="(?!image-processing\.js|gallery-maker-core\.js|gallery-maker\.js)[^"]+"/.test(html),
  'gallery-maker must only load its local image-processing and maker scripts');

// References image-processing.js
assert(/image-processing\.js\?v=adaptive-3/.test(html), 'must reference image-processing.js with correct version');

// Accessibility: aria-labels on canvases
assert(/id="edit-canvas"[^>]*aria-label="[^"]+"/.test(html), 'edit canvas needs aria-label');
assert(/id="preview-canvas"[^>]*aria-label="[^"]+"/.test(html), 'preview canvas needs aria-label');

// Labels for form inputs
for (const id of ['fit-select', 'processing-select', 'zoom-slider', 'pos-x', 'pos-y', 'slug-input', 'title-input', 'category-input']) {
  assert(new RegExp(`for="${id}"`).test(html), `missing <label for="${id}">`);
}

// ── JS syntax ──
new vm.Script(js, { filename: 'site/gallery-maker.js' });
new vm.Script(coreJs, { filename: 'site/gallery-maker-core.js' });

// JS uses HinkImage from image-processing.js
assert(js.includes('HinkImage.calculatePlacement'), 'must use HinkImage.calculatePlacement');
assert(js.includes('HinkImage.resolveMode'), 'must use HinkImage.resolveMode');
assert(js.includes('HinkImage.quantizePixels'), 'must use HinkImage.quantizePixels');
assert(js.includes('HinkImage.downscaleByMajority'), 'must use HinkImage.downscaleByMajority');
assert(/var osW = DEV_W \* OS, osH = DEV_H \* OS/.test(js),
  'oversampling must start from 250×122 device resolution');
assert(js.includes('HinkGalleryMakerCore.encodeRgbPng'), 'must use the explicit RGB PNG encoder');

// JS has File System Access API support
assert(js.includes('showDirectoryPicker'), 'must support File System Access API directory picker');
assert(js.includes('getFileHandle'), 'must use getFileHandle for directory operations');

// JS has no network requests (no fetch/XMLHttpRequest to external URLs)
assert(!(/fetch\s*\(\s*['"]https?:\/\//.test(js)), 'must not fetch external URLs');
assert(!(/XMLHttpRequest/.test(js)), 'must not use XMLHttpRequest');

// ── CSS ──
assert(css.includes('prefers-reduced-motion'), 'CSS must respect reduced motion');

// ── Navigation cross-links ──
// gallery-maker.html links to index.html and firmware.html
assert(/href="index\.html"/.test(html), 'gallery-maker must link to index.html');
assert(/href="firmware\.html"/.test(html), 'gallery-maker must link to firmware.html');

// index.html links to gallery-maker.html
assert(/href="gallery-maker\.html"/.test(indexHtml), 'index.html must link to gallery-maker.html');

// firmware.html links to gallery-maker.html
assert(/href="gallery-maker\.html"/.test(firmwareHtml), 'firmware.html must link to gallery-maker.html');

// ── Security ──
assert(!/\sstyle="/.test(html), 'gallery-maker page must not use inline styles');
// CSP: page has meta CSP or relies on inherited policy
// No base-uri, no form-action to external
assert(!(/form\s+action\s*=\s*["']https?:/.test(html)), 'no external form actions');

// ── Responsive ──
assert(/max-width:\s*700px/.test(css), 'CSS must have mobile breakpoint');
assert(/max-width:\s*960px/.test(css), 'CSS must have tablet breakpoint');

// ── Fit modes ──
assert(/value="contain"/.test(html), 'must offer contain fit mode');
assert(/value="cover"/.test(html), 'must offer cover fit mode');

// ── Processing modes ──
assert(/value="auto"/.test(html), 'must offer auto processing mode');
assert(/value="threshold"/.test(html), 'must offer threshold processing mode');
assert(/value="floyd-steinberg"/.test(html), 'must offer floyd-steinberg processing mode');

// ── No test inputs in gallery ──
const galleryDir = path.join(root, 'site', 'assets', 'gallery');
const galleryFiles = fs.readdirSync(galleryDir);
assert(!galleryFiles.includes('spider-man.webp'), 'test input spider-man.webp must not be in gallery');
assert(!galleryFiles.includes('LightMac.jpeg'), 'test input LightMac.jpeg must not be in gallery');

async function testRgbPngEncoder() {
  require(path.join(root, 'site', 'gallery-maker-core.js'));
  const catalog = {
    version: 1,
    images: {
      'alpha-image': { title: 'Alpha', category: '测试' },
      'legacy-photo': { title: 'Legacy', category: '测试' }
    }
  };
  const manifest = {
    version: 1,
    images: [
      { id: 'alpha-image', file: 'alpha-image.png', title: 'Alpha', category: '测试' },
      { id: 'legacy-photo', file: 'legacy-photo.jpeg', title: 'Legacy', category: '测试' }
    ]
  };
  const metadata = global.HinkGalleryMakerCore.buildGalleryMetadata(
    catalog,
    manifest,
    { slug: 'middle-image', title: 'Middle', category: '' },
    24
  );
  assert.deepStrictEqual(metadata.manifest.images.map(entry => entry.id),
    ['alpha-image', 'legacy-photo', 'middle-image'], 'browser manifest order must be deterministic');
  assert.strictEqual(metadata.manifest.images[1].file, 'legacy-photo.jpeg',
    'browser update must preserve an existing non-PNG file extension');
  assert.strictEqual(metadata.manifest.images[2].file, 'middle-image.png');
  assert.strictEqual(metadata.catalog.images['middle-image'].category, '未分类');
  assert(!catalog.images['middle-image'], 'metadata builder must not mutate the original catalog');
  assert.throws(() => global.HinkGalleryMakerCore.buildGalleryMetadata(
    catalog, manifest, { slug: 'alpha-image', title: 'Duplicate', category: '测试' }, 24
  ), /已存在/, 'duplicate slug must be rejected');
  assert.throws(() => global.HinkGalleryMakerCore.buildGalleryMetadata(
    catalog, { version: 1, images: [] }, { slug: 'new-image', title: 'New', category: '测试' }, 24
  ), /不一致/, 'catalog/manifest drift must be rejected');

  const width = 500;
  const height = 244;
  const palette = [[255, 255, 255], [17, 17, 17], [207, 32, 40]];
  const rgba = new Uint8ClampedArray(width * height * 4);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const color = palette[(Math.floor(x / 2) + Math.floor(y / 2)) % palette.length];
      const offset = (y * width + x) * 4;
      rgba[offset] = color[0];
      rgba[offset + 1] = color[1];
      rgba[offset + 2] = color[2];
      rgba[offset + 3] = 7; // encoder must discard alpha, not preserve it
    }
  }

  const blob = await global.HinkGalleryMakerCore.encodeRgbPng({ width, height, data: rgba });
  const info = await global.HinkGalleryMakerCore.inspectPng(blob);
  assert.deepStrictEqual(info, {
    width: 500,
    height: 244,
    bitDepth: 8,
    colorType: 2,
    hasAlpha: false
  }, 'export must be 500×244, 8-bit truecolor RGB without alpha');
  assert(blob.size <= 300 * 1024, 'representative three-color PNG must stay below 300 KiB');

  const bytes = Buffer.from(await blob.arrayBuffer());
  const idatParts = [];
  for (let offset = 8; offset < bytes.length;) {
    const length = bytes.readUInt32BE(offset);
    const type = bytes.subarray(offset + 4, offset + 8).toString('ascii');
    if (type === 'IDAT') idatParts.push(bytes.subarray(offset + 8, offset + 8 + length));
    offset += 12 + length;
  }
  const raw = zlib.inflateSync(Buffer.concat(idatParts));
  assert.strictEqual(raw.length, height * (1 + width * 3), 'PNG scanline byte count must be RGB');
  for (let y = 0; y < height; y++) {
    assert.strictEqual(raw[y * (1 + width * 3)], 0, 'each scanline must use filter type 0');
  }
}

testRgbPngEncoder().then(() => {
  console.log('PASS: gallery-maker structure, navigation, RGB PNG encoding, accessibility, processing modes, directory API, and local-only dependencies.');
}).catch(error => {
  console.error(error);
  process.exitCode = 1;
});
