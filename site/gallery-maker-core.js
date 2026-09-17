/* Pure helpers for HINK Gallery Maker. Browser and Node compatible. */
(function (root) {
  'use strict';

  var PNG_SIGNATURE = new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]);
  var crcTable = null;

  function writeUint32(target, offset, value) {
    target[offset] = (value >>> 24) & 255;
    target[offset + 1] = (value >>> 16) & 255;
    target[offset + 2] = (value >>> 8) & 255;
    target[offset + 3] = value & 255;
  }

  function getCrcTable() {
    if (crcTable) return crcTable;
    crcTable = new Uint32Array(256);
    for (var n = 0; n < 256; n++) {
      var c = n;
      for (var k = 0; k < 8; k++) {
        c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
      }
      crcTable[n] = c >>> 0;
    }
    return crcTable;
  }

  function crc32(bytes) {
    var table = getCrcTable();
    var crc = 0xffffffff;
    for (var i = 0; i < bytes.length; i++) {
      crc = table[(crc ^ bytes[i]) & 255] ^ (crc >>> 8);
    }
    return (crc ^ 0xffffffff) >>> 0;
  }

  function joinBytes(parts) {
    var length = parts.reduce(function (total, part) { return total + part.length; }, 0);
    var joined = new Uint8Array(length);
    var offset = 0;
    parts.forEach(function (part) {
      joined.set(part, offset);
      offset += part.length;
    });
    return joined;
  }

  function pngChunk(type, data) {
    var typeBytes = new TextEncoder().encode(type);
    var body = joinBytes([typeBytes, data]);
    var chunk = new Uint8Array(12 + data.length);
    writeUint32(chunk, 0, data.length);
    chunk.set(body, 4);
    writeUint32(chunk, 8 + data.length, crc32(body));
    return chunk;
  }

  function assertImageData(imageData) {
    if (!imageData || !Number.isInteger(imageData.width) || !Number.isInteger(imageData.height) ||
        imageData.width <= 0 || imageData.height <= 0 ||
        !imageData.data || imageData.data.length !== imageData.width * imageData.height * 4) {
      throw new TypeError('Invalid RGBA image data');
    }
  }

  async function encodeRgbPng(imageData) {
    assertImageData(imageData);
    if (typeof CompressionStream === 'undefined') {
      throw new Error('CompressionStream is unavailable');
    }

    var width = imageData.width;
    var height = imageData.height;
    var rgba = imageData.data;
    var stride = 1 + width * 3;
    var raw = new Uint8Array(stride * height);

    for (var y = 0; y < height; y++) {
      var row = y * stride;
      raw[row] = 0; // PNG filter type: None
      for (var x = 0; x < width; x++) {
        var source = (y * width + x) * 4;
        var target = row + 1 + x * 3;
        raw[target] = rgba[source];
        raw[target + 1] = rgba[source + 1];
        raw[target + 2] = rgba[source + 2];
      }
    }

    var compressedResponse = new Response(
      new Blob([raw]).stream().pipeThrough(new CompressionStream('deflate'))
    );
    var compressed = new Uint8Array(await compressedResponse.arrayBuffer());

    var ihdr = new Uint8Array(13);
    writeUint32(ihdr, 0, width);
    writeUint32(ihdr, 4, height);
    ihdr[8] = 8; // bit depth
    ihdr[9] = 2; // color type 2: truecolor RGB, no alpha
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    var png = joinBytes([
      PNG_SIGNATURE,
      pngChunk('IHDR', ihdr),
      pngChunk('IDAT', compressed),
      pngChunk('IEND', new Uint8Array(0))
    ]);
    return new Blob([png], { type: 'image/png' });
  }

  async function inspectPng(blob) {
    var bytes = new Uint8Array(await blob.arrayBuffer());
    if (bytes.length < 33 || !PNG_SIGNATURE.every(function (value, i) { return bytes[i] === value; })) {
      throw new Error('Not a PNG file');
    }
    return {
      width: ((bytes[16] << 24) | (bytes[17] << 16) | (bytes[18] << 8) | bytes[19]) >>> 0,
      height: ((bytes[20] << 24) | (bytes[21] << 16) | (bytes[22] << 8) | bytes[23]) >>> 0,
      bitDepth: bytes[24],
      colorType: bytes[25],
      hasAlpha: bytes[25] === 4 || bytes[25] === 6
    };
  }

  function buildGalleryMetadata(catalog, manifest, nextImage, maxImages) {
    var slugPattern = /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/;
    if (!catalog || catalog.version !== 1 || !catalog.images || Array.isArray(catalog.images)) {
      throw new Error('catalog.json 结构不正确。');
    }
    if (!manifest || manifest.version !== 1 || !Array.isArray(manifest.images)) {
      throw new Error('manifest.json 结构不正确。');
    }
    if (!nextImage || !slugPattern.test(nextImage.slug) || !nextImage.title || !nextImage.title.trim()) {
      throw new Error('新图片的 slug 或标题不合法。');
    }

    var existingFiles = {};
    manifest.images.forEach(function (entry) {
      if (!entry || !slugPattern.test(entry.id) || typeof entry.file !== 'string' || !entry.file) {
        throw new Error('manifest.json 包含无效条目。');
      }
      if (existingFiles[entry.id]) throw new Error('manifest.json 包含重复 id。');
      existingFiles[entry.id] = entry.file;
    });

    var catalogSlugs = Object.keys(catalog.images).sort();
    var manifestSlugs = Object.keys(existingFiles).sort();
    if (JSON.stringify(catalogSlugs) !== JSON.stringify(manifestSlugs)) {
      throw new Error('catalog.json 与 manifest.json 的图片列表不一致，请先运行图集构建脚本修复。');
    }
    if (catalogSlugs.length >= maxImages) {
      throw new Error('图集已达到 ' + maxImages + ' 张上限。');
    }
    if (catalog.images[nextImage.slug] || existingFiles[nextImage.slug]) {
      throw new Error('slug “' + nextImage.slug + '” 已存在，请换一个文件名。');
    }

    var nextCatalog = JSON.parse(JSON.stringify(catalog));
    nextCatalog.images[nextImage.slug] = {
      title: nextImage.title.trim(),
      category: nextImage.category && nextImage.category.trim() ? nextImage.category.trim() : '未分类'
    };
    var nextSlugs = Object.keys(nextCatalog.images).sort();
    var nextManifest = {
      version: 1,
      images: nextSlugs.map(function (slug) {
        return {
          id: slug,
          file: slug === nextImage.slug ? slug + '.png' : existingFiles[slug],
          title: nextCatalog.images[slug].title,
          category: nextCatalog.images[slug].category
        };
      })
    };
    return { catalog: nextCatalog, manifest: nextManifest };
  }

  root.HinkGalleryMakerCore = {
    encodeRgbPng: encodeRgbPng,
    inspectPng: inspectPng,
    buildGalleryMetadata: buildGalleryMetadata
  };
})(typeof globalThis !== 'undefined' ? globalThis : window);
