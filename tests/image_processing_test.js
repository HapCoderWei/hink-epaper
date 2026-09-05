'use strict';
const assert = require('assert');
require('../site/image-processing.js');

function image(width, height, pixel) {
  const data = new Uint8ClampedArray(width * height * 4);
  for (let i = 0; i < data.length; i += 4) {
    data[i] = pixel[0]; data[i + 1] = pixel[1];
    data[i + 2] = pixel[2]; data[i + 3] = 255;
  }
  return { width, height, data };
}

function colors(result) {
  const out = new Set();
  for (let i = 0; i < result.data.length; i += 4)
    out.add(`${result.data[i]},${result.data[i + 1]},${result.data[i + 2]}`);
  return out;
}

function stripedImage(width, height, colorsToUse) {
  const result = image(width, height, colorsToUse[0]);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const color = colorsToUse[x % colorsToUse.length];
      const p = (y * width + x) * 4;
      result.data[p] = color[0]; result.data[p + 1] = color[1]; result.data[p + 2] = color[2];
    }
  }
  return result;
}

for (const mode of ['threshold', 'floyd-steinberg']) {
  for (const sample of [[255,255,255], [17,17,17], [207,32,40]]) {
    const result = HinkImage.quantizePixels(image(8, 8, sample), mode);
    assert.deepStrictEqual([...colors(result)], [sample.join(',')]);
  }
}

const gray = HinkImage.quantizePixels(image(32, 32, [128,128,128]), 'floyd-steinberg');
const grayColors = colors(gray);
assert(grayColors.has('17,17,17') && grayColors.has('255,255,255'));
assert(!grayColors.has('207,32,40'), 'neutral gray must not create red speckles');

const mutedRed = HinkImage.quantizePixels(image(32, 32, [170,70,75]), 'floyd-steinberg');
assert(colors(mutedRed).has('207,32,40'), 'red shading must retain the red plane');

assert.throws(() => HinkImage.quantizePixels(image(1, 1, [0,0,0]), 'unknown'));

const logo = stripedImage(100, 10, [[255,255,255], [0,0,0], [240,240,240], [12,12,12]]);
assert.strictEqual(HinkImage.resolveMode(logo, 'auto'), 'threshold');
const photoGradient = { width: 256, height: 1, data: new Uint8ClampedArray(256 * 4) };
for (let x = 0; x < 256; x++) {
  photoGradient.data[x * 4] = photoGradient.data[x * 4 + 1] = photoGradient.data[x * 4 + 2] = x;
  photoGradient.data[x * 4 + 3] = 255;
}
assert.strictEqual(HinkImage.resolveMode(photoGradient, 'auto'), 'floyd-steinberg');
assert.deepStrictEqual([...colors(HinkImage.quantizePixels(logo, 'auto'))].sort(), ['17,17,17', '255,255,255'].sort());

const normal = HinkImage.calculatePlacement(100, 50, 122, 250, 'contain', 0);
assert.strictEqual(normal.turns, 0);
assert.strictEqual(normal.drawWidth, 122);
assert.strictEqual(normal.drawHeight, 61);

const right = HinkImage.calculatePlacement(100, 50, 122, 250, 'contain', 1);
assert.strictEqual(right.turns, 1);
assert.strictEqual(right.drawWidth, 244);
assert.strictEqual(right.drawHeight, 122);
assert.strictEqual(right.angle, Math.PI / 2);

const left = HinkImage.calculatePlacement(100, 50, 122, 250, 'contain', -1);
assert.strictEqual(left.turns, 3);
assert.strictEqual(left.angle, Math.PI * 1.5);
assert.throws(() => HinkImage.calculatePlacement(100, 50, 122, 250, 'stretch', 0));

console.log('PASS: palette, adaptive processing, dithering, red preservation, rotation.');
