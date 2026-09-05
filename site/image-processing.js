/* HINK 122x250 black/white/red image processing. No network or dependencies. */
(function (root) {
  'use strict';

  const WHITE = [255, 255, 255];
  const BLACK = [17, 17, 17];
  const RED = [207, 32, 40];

  function clamp8(value) {
    return Math.max(0, Math.min(255, value));
  }

  function isRedHue(r, g, b) {
    return r > 82 && r > g * 1.18 && r > b * 1.12 && r - g > 20;
  }

  function luma(r, g, b) {
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
  }

  function distance(r, g, b, color) {
    const dr = r - color[0];
    const dg = g - color[1];
    const db = b - color[2];
    return 0.2126 * dr * dr + 0.7152 * dg * dg + 0.0722 * db * db;
  }

  function nearestColor(r, g, b, redEligible) {
    let result = luma(r, g, b) < 145 ? BLACK : WHITE;
    let best = distance(r, g, b, result);
    if (redEligible) {
      const redDistance = distance(r, g, b, RED);
      if (redDistance < best) result = RED;
    }
    return result;
  }

  function hardThreshold(data) {
    for (let i = 0; i < data.length; i += 4) {
      const r = data[i], g = data[i + 1], b = data[i + 2];
      let color;
      if (r > 105 && r > g * 1.28 && r > b * 1.18) color = RED;
      else color = luma(r, g, b) < 145 ? BLACK : WHITE;
      data[i] = color[0];
      data[i + 1] = color[1];
      data[i + 2] = color[2];
      data[i + 3] = 255;
    }
  }

  function diffuse(work, width, height, x, y, er, eg, eb, weight) {
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    const p = (y * width + x) * 3;
    work[p] += er * weight;
    work[p + 1] += eg * weight;
    work[p + 2] += eb * weight;
  }

  function floydSteinberg(data, width, height) {
    const work = new Float32Array(width * height * 3);
    for (let i = 0, p = 0; i < data.length; i += 4, p += 3) {
      work[p] = data[i];
      work[p + 1] = data[i + 1];
      work[p + 2] = data[i + 2];
    }

    for (let y = 0; y < height; y++) {
      /* Serpentine rows avoid the diagonal streaking produced by always
         diffusing in one direction on a narrow portrait display. */
      const leftToRight = (y & 1) === 0;
      const start = leftToRight ? 0 : width - 1;
      const end = leftToRight ? width : -1;
      const step = leftToRight ? 1 : -1;
      for (let x = start; x !== end; x += step) {
        const wp = (y * width + x) * 3;
        const dp = (y * width + x) * 4;
        const r = clamp8(work[wp]);
        const g = clamp8(work[wp + 1]);
        const b = clamp8(work[wp + 2]);
        /* Gate red with the original hue. This prevents neutral shadows from
           accumulating chromatic error and turning into red freckles. */
        const color = nearestColor(r, g, b,
          isRedHue(data[dp], data[dp + 1], data[dp + 2]));
        data[dp] = color[0];
        data[dp + 1] = color[1];
        data[dp + 2] = color[2];
        data[dp + 3] = 255;

        const er = r - color[0];
        const eg = g - color[1];
        const eb = b - color[2];
        diffuse(work, width, height, x + step, y, er, eg, eb, 7 / 16);
        diffuse(work, width, height, x - step, y + 1, er, eg, eb, 3 / 16);
        diffuse(work, width, height, x, y + 1, er, eg, eb, 5 / 16);
        diffuse(work, width, height, x + step, y + 1, er, eg, eb, 1 / 16);
      }
    }
  }

  function automaticMode(imageData) {
    let paletteLike = 0;
    const pixels = imageData.data;
    const total = pixels.length / 4;
    for (let i = 0; i < pixels.length; i += 4) {
      const r = pixels[i], g = pixels[i + 1], b = pixels[i + 2];
      const closest = Math.min(
        distance(r, g, b, WHITE),
        distance(r, g, b, BLACK),
        distance(r, g, b, RED));
      /* Logos, text and barcodes already consist mostly of display-native
         colors. Diffusing their JPEG/antialiasing residue only softens edges. */
      if (closest < 1600) paletteLike++;
    }
    return paletteLike / total >= 0.78 ? 'threshold' : 'floyd-steinberg';
  }

  function resolveMode(imageData, requestedMode) {
    if (!imageData || !imageData.data || !imageData.width || !imageData.height)
      throw new TypeError('Invalid ImageData');
    if (requestedMode === 'auto') return automaticMode(imageData);
    if (requestedMode === 'threshold' || requestedMode === 'floyd-steinberg')
      return requestedMode;
    throw new RangeError('Unknown quantization mode');
  }

  function quantizePixels(imageData, mode) {
    const resolvedMode = resolveMode(imageData, mode);
    if (resolvedMode === 'threshold') hardThreshold(imageData.data);
    else
      floydSteinberg(imageData.data, imageData.width, imageData.height);
    return imageData;
  }

  function calculatePlacement(sourceWidth, sourceHeight, targetWidth, targetHeight, fit, quarterTurns) {
    if (![sourceWidth, sourceHeight, targetWidth, targetHeight].every(value => value > 0))
      throw new RangeError('Image dimensions must be positive');
    if (fit !== 'cover' && fit !== 'contain')
      throw new RangeError('Unknown image fit mode');

    const turns = ((quarterTurns % 4) + 4) % 4;
    const rotatedWidth = (turns & 1) ? sourceHeight : sourceWidth;
    const rotatedHeight = (turns & 1) ? sourceWidth : sourceHeight;
    const scale = fit === 'cover'
      ? Math.max(targetWidth / rotatedWidth, targetHeight / rotatedHeight)
      : Math.min(targetWidth / rotatedWidth, targetHeight / rotatedHeight);

    return {
      turns,
      angle: turns * Math.PI / 2,
      drawWidth: sourceWidth * scale,
      drawHeight: sourceHeight * scale
    };
  }

  root.HinkImage = { quantizePixels, resolveMode, calculatePlacement };
})(typeof globalThis !== 'undefined' ? globalThis : window);
