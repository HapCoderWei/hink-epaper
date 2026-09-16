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

// downscaleByMajority: pure white 3x block → white
const whiteBlock = image(3, 3, [255, 255, 255]);
assert.deepStrictEqual([...HinkImage.downscaleByMajority(whiteBlock, 3).data.subarray(0, 4)], [255, 255, 255, 255]);

// downscaleByMajority: 5 white + 4 red in 3x3 → white (white wins majority)
const mixed = image(3, 3, [255, 255, 255]);
for (let i = 0; i < 4; i++) {
  mixed.data[i * 4] = 207; mixed.data[i * 4 + 1] = 32; mixed.data[i * 4 + 2] = 40;
}
const mixedResult = HinkImage.downscaleByMajority(mixed, 3);
assert.strictEqual(mixedResult.data[0], 255, 'white-majority block must stay white');

// downscaleByMajority: 5 red + 4 white → red (red wins)
const redMajority = image(3, 3, [207, 32, 40]);
for (let i = 0; i < 4; i++) {
  redMajority.data[i * 4] = 255; redMajority.data[i * 4 + 1] = 255; redMajority.data[i * 4 + 2] = 255;
}
const redMajorityResult = HinkImage.downscaleByMajority(redMajority, 3);
assert.strictEqual(redMajorityResult.data[0], 207, 'red-majority block must become red');

// downscaleByMajority: exact 3x → 1x
assert.strictEqual(HinkImage.downscaleByMajority(image(6, 6, [17, 17, 17]), 3).width, 2);

// Tightened red gating: muted red [170,70,75] still detected as red in threshold mode
const mutedThreshold = HinkImage.quantizePixels(image(8, 8, [170, 70, 75]), 'threshold');
assert.deepStrictEqual([...colors(mutedThreshold)], ['207,32,40'], 'muted red must still threshold to red');

// Tightened red gating: pink antialiasing tone [220,170,175] must NOT become red
const pink = image(8, 8, [220, 170, 175]);
HinkImage.quantizePixels(pink, 'threshold');
assert(!colors(pink).has('207,32,40'), 'pink antialiasing tone must not become red');

// Supersampled quantization: red logo on white with pink edges → no isolated speckles
(function testSupersampledRedLogo() {
  const W = 30, H = 30, OS = 3;
  const big = image(W * OS, H * OS, [255, 255, 255]);
  // Red rectangle at 3x coords: x 9..20, y 9..20
  for (let y = 9; y <= 20; y++) {
    for (let x = 9; x <= 20; x++) {
      const p = (y * W * OS + x) * 4;
      big.data[p] = 207; big.data[p + 1] = 32; big.data[p + 2] = 40;
    }
  }
  // Pink antialiasing border (1 px around the red rect)
  for (let x = 8; x <= 21; x++) {
    for (const y of [8, 21]) {
      const p = (y * W * OS + x) * 4;
      big.data[p] = 220; big.data[p + 1] = 170; big.data[p + 2] = 175;
    }
  }
  for (let y = 9; y <= 20; y++) {
    for (const x of [8, 21]) {
      const p = (y * W * OS + x) * 4;
      big.data[p] = 220; big.data[p + 1] = 170; big.data[p + 2] = 175;
    }
  }
  HinkImage.quantizePixels(big, 'threshold');
  const small = HinkImage.downscaleByMajority(big, OS);
  assert.strictEqual(small.width, W);
  assert.strictEqual(small.height, H);

  // White area: top-left corner (far from red rect at downscaled 3,3..6,6) must be pure white
  assert.strictEqual(small.data[0], 255, 'top-left must be white');
  assert.strictEqual(small.data[1], 255, 'top-left must be white');

  // Count non-white pixels in the clearly white zone (rows 0..2, cols 0..2)
  // which is far from the red rectangle (downscaled 3,3..6,6)
  let whiteZoneSpeckles = 0;
  for (let y = 0; y < 3; y++) {
    for (let x = 0; x < 3; x++) {
      const p = (y * W + x) * 4;
      if (small.data[p] !== 255 || small.data[p + 1] !== 255 || small.data[p + 2] !== 255)
        whiteZoneSpeckles++;
    }
  }
  assert.strictEqual(whiteZoneSpeckles, 0, 'white area must have zero color speckles after supersampled quantization');
})();

console.log('PASS: palette, adaptive processing, dithering, red preservation, rotation, downscaleByMajority, tightened red gating, supersampled quantization.');
