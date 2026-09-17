/* HINK Gallery Maker – browser-only image-to-epaper tool. No dependencies. */
(function () {
  'use strict';

  var DEV_W = 250, DEV_H = 122;
  var OUT_W = 500, OUT_H = 244;
  var OS = 3; // oversample factor for quantization quality
  var MAX_FILE_SIZE = 300 * 1024; // 300 KiB
  var MAX_GALLERY = 24;

  // ── DOM refs ──
  var canvasEdit = document.getElementById('edit-canvas');
  var ctxEdit = canvasEdit.getContext('2d', { willReadFrequently: true });
  var canvasPreview = document.getElementById('preview-canvas');
  var ctxPreview = canvasPreview.getContext('2d', { willReadFrequently: true });

  var fileInput = document.getElementById('file-input');
  var dropZone = document.getElementById('drop-zone');
  var fitSelect = document.getElementById('fit-select');
  var processingSelect = document.getElementById('processing-select');
  var zoomSlider = document.getElementById('zoom-slider');
  var zoomVal = document.getElementById('zoom-val');
  var posXSlider = document.getElementById('pos-x');
  var posYSlider = document.getElementById('pos-y');
  var posXVal = document.getElementById('pos-x-val');
  var posYVal = document.getElementById('pos-y-val');
  var rotateLeftBtn = document.getElementById('rotate-left');
  var rotateRightBtn = document.getElementById('rotate-right');
  var rotationStatus = document.getElementById('rotation-status');
  var processingHint = document.getElementById('processing-hint');

  var slugInput = document.getElementById('slug-input');
  var titleInput = document.getElementById('title-input');
  var categoryInput = document.getElementById('category-input');

  var downloadBtn = document.getElementById('download-btn');
  var saveDirBtn = document.getElementById('save-dir-btn');
  var validationMsg = document.getElementById('validation-msg');
  var statusBar = document.getElementById('status-bar');
  var statusText = document.getElementById('status-text');

  var colorWhite = document.getElementById('color-white');
  var colorBlack = document.getElementById('color-black');
  var colorRed = document.getElementById('color-red');
  var outputSize = document.getElementById('output-size');

  // ── State ──
  var sourceImage = null;
  var rotation = 0; // 0..3 quarter-turns
  var customZoom = 1;
  var offsetX = 0, offsetY = 0; // -100..100
  var dragging = false;
  var dragStartX = 0, dragStartY = 0;
  var dragStartOX = 0, dragStartOY = 0;
  var dirHandle = null;
  var lastPngBlob = null;
  var lastPngSize = 0;
  var lastPngIsRgb = false;
  var renderRevision = 0;
  var supportsDirectoryAccess = 'showDirectoryPicker' in window;

  // ── Init ──
  canvasEdit.width = OUT_W;
  canvasEdit.height = OUT_H;
  canvasPreview.width = DEV_W;
  canvasPreview.height = DEV_H;
  drawPlaceholder();
  updateControls();

  // ── Placeholder ──
  function drawPlaceholder() {
    ctxEdit.fillStyle = '#fff';
    ctxEdit.fillRect(0, 0, OUT_W, OUT_H);
    ctxEdit.fillStyle = '#cf2028';
    ctxEdit.fillRect(0, 0, 168, OUT_H);
    ctxEdit.fillStyle = '#111';
    ctxEdit.fillRect(340, 0, 160, OUT_H);
    ctxEdit.strokeStyle = '#111';
    ctxEdit.lineWidth = 2;
    ctxEdit.strokeRect(183, 16, 144, 208);
    ctxEdit.beginPath();
    ctxEdit.moveTo(184, 17);
    ctxEdit.lineTo(327, 223);
    ctxEdit.moveTo(327, 17);
    ctxEdit.lineTo(184, 223);
    ctxEdit.stroke();

    ctxPreview.fillStyle = '#fff';
    ctxPreview.fillRect(0, 0, DEV_W, DEV_H);
    ctxPreview.fillStyle = '#cf2028';
    ctxPreview.fillRect(0, 0, 84, DEV_H);
    ctxPreview.fillStyle = '#111';
    ctxPreview.fillRect(170, 0, 80, DEV_H);
    ctxPreview.strokeStyle = '#111';
    ctxPreview.lineWidth = 1;
    ctxPreview.strokeRect(91.5, 8.5, 72, 104);
    ctxPreview.beginPath();
    ctxPreview.moveTo(92, 9);
    ctxPreview.lineTo(164, 113);
    ctxPreview.moveTo(164, 9);
    ctxPreview.lineTo(92, 113);
    ctxPreview.stroke();
  }

  // ── Image loading ──
  function loadImage(file) {
    if (!file || !file.type.startsWith('image/')) {
      setStatus('error', '无法读取这张图片，请改用 PNG、JPG 或 WebP。');
      return;
    }
    var img = new Image();
    img.onload = function () {
      sourceImage = img;
      rotation = 0;
      customZoom = 1;
      offsetX = 0;
      offsetY = 0;
      zoomSlider.value = 100;
      zoomVal.textContent = '100%';
      posXSlider.value = 0;
      posYSlider.value = 0;
      posXVal.textContent = '0';
      posYVal.textContent = '0';
      updateRotationLabel();
      setStatus('ready', file.name + ' · ' + img.width + '×' + img.height + ' · 已载入');
      updateControls();
      render();
      URL.revokeObjectURL(img.src);
    };
    img.onerror = function () {
      setStatus('error', '无法读取这张图片，请改用 PNG、JPG 或 WebP。');
      URL.revokeObjectURL(img.src);
    };
    img.src = URL.createObjectURL(file);
  }

  fileInput.addEventListener('change', function () {
    if (fileInput.files[0]) loadImage(fileInput.files[0]);
  });

  // Drag & drop
  ['dragenter', 'dragover'].forEach(function (type) {
    dropZone.addEventListener(type, function (e) { e.preventDefault(); dropZone.classList.add('is-dragging'); });
  });
  ['dragleave', 'drop'].forEach(function (type) {
    dropZone.addEventListener(type, function (e) { e.preventDefault(); dropZone.classList.remove('is-dragging'); });
  });
  dropZone.addEventListener('drop', function (e) {
    if (e.dataTransfer.files[0]) loadImage(e.dataTransfer.files[0]);
  });
  dropZone.addEventListener('keydown', function (e) {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); fileInput.click(); }
  });

  // Paste
  document.addEventListener('paste', function (e) {
    var files = [].slice.call(e.clipboardData.files);
    var img = files.find(function (f) { return f.type.startsWith('image/'); });
    if (img) { e.preventDefault(); loadImage(img); }
  });

  // ── Controls ──
  function updateControls() {
    var hasImage = !!sourceImage;
    rotateLeftBtn.disabled = rotateRightBtn.disabled = !hasImage;
    downloadBtn.disabled = !hasImage || !lastPngBlob || lastPngSize > MAX_FILE_SIZE;
    updateSaveButton();
  }

  fitSelect.addEventListener('change', render);
  processingSelect.addEventListener('change', render);

  zoomSlider.addEventListener('input', function () {
    customZoom = parseInt(zoomSlider.value, 10) / 100;
    zoomVal.textContent = zoomSlider.value + '%';
    render();
  });

  posXSlider.addEventListener('input', function () {
    offsetX = parseInt(posXSlider.value, 10);
    posXVal.textContent = offsetX;
    render();
  });

  posYSlider.addEventListener('input', function () {
    offsetY = parseInt(posYSlider.value, 10);
    posYVal.textContent = offsetY;
    render();
  });

  rotateLeftBtn.addEventListener('click', function () { setRotation(rotation - 1); });
  rotateRightBtn.addEventListener('click', function () { setRotation(rotation + 1); });

  function setRotation(turns) {
    rotation = ((turns % 4) + 4) % 4;
    updateRotationLabel();
    render();
  }

  function updateRotationLabel() {
    var labels = ['横向默认', '右转 90°', '旋转 180°', '左转 90°'];
    rotationStatus.textContent = labels[rotation];
    if (sourceImage && sourceImage.height > sourceImage.width && (rotation & 1) === 0) {
      rotationStatus.textContent += ' · 竖图可转 90°';
    }
  }

  // ── Canvas drag to pan ──
  canvasEdit.addEventListener('mousedown', function (e) {
    if (!sourceImage) return;
    dragging = true;
    dragStartX = e.clientX;
    dragStartY = e.clientY;
    dragStartOX = offsetX;
    dragStartOY = offsetY;
    canvasEdit.style.cursor = 'grabbing';
  });
  document.addEventListener('mousemove', function (e) {
    if (!dragging) return;
    var dx = e.clientX - dragStartX;
    var dy = e.clientY - dragStartY;
    // Convert pixel delta to -100..100 range (approximate mapping)
    var rect = canvasEdit.getBoundingClientRect();
    var scaleX = 200 / rect.width;
    var scaleY = 200 / rect.height;
    offsetX = Math.max(-100, Math.min(100, Math.round(dragStartOX + dx * scaleX)));
    offsetY = Math.max(-100, Math.min(100, Math.round(dragStartOY + dy * scaleY)));
    posXSlider.value = offsetX;
    posYSlider.value = offsetY;
    posXVal.textContent = offsetX;
    posYVal.textContent = offsetY;
    render();
  });
  document.addEventListener('mouseup', function () {
    if (dragging) { dragging = false; canvasEdit.style.cursor = ''; }
  });

  // Touch drag
  canvasEdit.addEventListener('touchstart', function (e) {
    if (!sourceImage || e.touches.length !== 1) return;
    dragging = true;
    dragStartX = e.touches[0].clientX;
    dragStartY = e.touches[0].clientY;
    dragStartOX = offsetX;
    dragStartOY = offsetY;
  }, { passive: true });
  canvasEdit.addEventListener('touchmove', function (e) {
    if (!dragging) return;
    e.preventDefault();
    var dx = e.touches[0].clientX - dragStartX;
    var dy = e.touches[0].clientY - dragStartY;
    var rect = canvasEdit.getBoundingClientRect();
    var scaleX = 200 / rect.width;
    var scaleY = 200 / rect.height;
    offsetX = Math.max(-100, Math.min(100, Math.round(dragStartOX + dx * scaleX)));
    offsetY = Math.max(-100, Math.min(100, Math.round(dragStartOY + dy * scaleY)));
    posXSlider.value = offsetX;
    posYSlider.value = offsetY;
    posXVal.textContent = offsetX;
    posYVal.textContent = offsetY;
    render();
  }, { passive: false });
  canvasEdit.addEventListener('touchend', function () { dragging = false; });

  // ── Render pipeline ──
  function render() {
    if (!sourceImage) return;

    var revision = ++renderRevision;
    lastPngBlob = null;
    lastPngSize = 0;
    lastPngIsRgb = false;
    outputSize.textContent = '处理中…';
    updateControls();

    // 1. Compute placement at oversampled resolution
    var osW = DEV_W * OS, osH = DEV_H * OS;
    var placement = HinkImage.calculatePlacement(
      sourceImage.width, sourceImage.height, osW, osH, fitSelect.value, rotation
    );

    // Apply custom zoom
    var drawW = placement.drawWidth * customZoom;
    var drawH = placement.drawHeight * customZoom;

    // Canvas-relative movement keeps drag direction stable for contain, cover
    // and additional zoom. At the extremes part of the image remains visible.
    var shiftX = (offsetX / 100) * osW * 0.45;
    var shiftY = (offsetY / 100) * osH * 0.45;

    // 2. Draw to oversampled offscreen canvas
    var offscreen = document.createElement('canvas');
    offscreen.width = osW;
    offscreen.height = osH;
    var offCtx = offscreen.getContext('2d', { willReadFrequently: true });
    offCtx.fillStyle = '#fff';
    offCtx.fillRect(0, 0, osW, osH);
    offCtx.save();
    offCtx.imageSmoothingEnabled = true;
    offCtx.imageSmoothingQuality = 'high';
    offCtx.translate(osW / 2 + shiftX, osH / 2 + shiftY);
    offCtx.rotate(placement.angle);
    offCtx.drawImage(sourceImage, -drawW / 2, -drawH / 2, drawW, drawH);
    offCtx.restore();

    // 3. Quantize at high resolution
    var hiRes = offCtx.getImageData(0, 0, osW, osH);
    var resolvedMode = HinkImage.resolveMode(hiRes, processingSelect.value);
    HinkImage.quantizePixels(hiRes, resolvedMode);

    // 4. Downscale by majority vote to device resolution
    var deviceImage = HinkImage.downscaleByMajority(hiRes, OS);
    ctxPreview.putImageData(deviceImage, 0, 0);

    // 5. Draw 2x nearest-neighbor to edit canvas (500×244)
    ctxEdit.imageSmoothingEnabled = false;
    ctxEdit.clearRect(0, 0, OUT_W, OUT_H);
    ctxEdit.fillStyle = '#fff';
    ctxEdit.fillRect(0, 0, OUT_W, OUT_H);
    // Create temp canvas at device res and draw scaled
    var tmpCanvas = document.createElement('canvas');
    tmpCanvas.width = DEV_W;
    tmpCanvas.height = DEV_H;
    tmpCanvas.getContext('2d').putImageData(deviceImage, 0, 0);
    ctxEdit.drawImage(tmpCanvas, 0, 0, DEV_W, DEV_H, 0, 0, OUT_W, OUT_H);

    // 6. Update processing hint
    if (processingSelect.value === 'auto') {
      processingHint.textContent = resolvedMode === 'threshold'
        ? '已识别为文字 / Logo：关闭抖动，保留清晰边缘。'
        : '已识别为照片：使用误差扩散模拟平滑阴影。';
    } else if (resolvedMode === 'threshold') {
      processingHint.textContent = '清晰边缘模式不会生成抖动杂点，适合文字、Logo 和条码。';
    } else {
      processingHint.textContent = '照片模式用像素密度模拟阴影；纯色文字可能显得不够锐利。';
    }

    // 7. Color statistics
    var pixels = deviceImage.data;
    var total = DEV_W * DEV_H;
    var wCount = 0, bCount = 0, rCount = 0;
    for (var i = 0; i < pixels.length; i += 4) {
      var r = pixels[i], g = pixels[i + 1], b = pixels[i + 2];
      if (r === 255 && g === 255 && b === 255) wCount++;
      else if (r === 207 && g === 32 && b === 40) rCount++;
      else bCount++;
    }
    colorWhite.textContent = '白 ' + Math.round(wCount / total * 100) + '%';
    colorBlack.textContent = '黑 ' + Math.round(bCount / total * 100) + '%';
    colorRed.textContent = '红 ' + Math.round(rCount / total * 100) + '%';

    // 8. Generate export PNG blob. The revision prevents a stale async
    // result replacing a newer adjustment.
    generateExportPng(revision);
    validateExport();
  }

  function canvasPngBlob() {
    return new Promise(function (resolve, reject) {
      canvasEdit.toBlob(function (blob) {
        if (blob) resolve(blob);
        else reject(new Error('浏览器无法生成 PNG。'));
      }, 'image/png');
    });
  }

  async function generateExportPng(revision) {
    try {
      var blob;
      if (typeof CompressionStream !== 'undefined') {
        blob = await HinkGalleryMakerCore.encodeRgbPng(
          ctxEdit.getImageData(0, 0, OUT_W, OUT_H)
        );
      } else {
        blob = await canvasPngBlob();
      }
      var pngInfo = await HinkGalleryMakerCore.inspectPng(blob);
      if (revision !== renderRevision) return;
      lastPngBlob = blob;
      lastPngSize = blob.size;
      lastPngIsRgb = pngInfo.width === OUT_W && pngInfo.height === OUT_H &&
        pngInfo.bitDepth === 8 && pngInfo.colorType === 2 && !pngInfo.hasAlpha;
      outputSize.textContent = (blob.size / 1024).toFixed(1) + ' KiB';
      validateExport();
      updateControls();
    } catch (error) {
      if (revision !== renderRevision) return;
      lastPngBlob = null;
      lastPngSize = 0;
      lastPngIsRgb = false;
      outputSize.textContent = '生成失败';
      setStatus('error', 'PNG 生成失败：' + error.message);
      validateExport();
      updateControls();
    }
  }

  // ── Validation ──
  function validateExport() {
    var msgs = [];
    if (!sourceImage) {
      msgs.push({ level: 'warn', text: '尚未载入图片。' });
    }
    if (lastPngSize > MAX_FILE_SIZE) {
      msgs.push({ level: 'error', text: 'PNG 文件 ' + (lastPngSize / 1024).toFixed(1) + ' KiB 超过 300 KiB 限制。' });
    }
    if (lastPngBlob && lastPngSize <= MAX_FILE_SIZE && lastPngIsRgb) {
      msgs.push({ level: 'ok', text: '500×244 RGB PNG · 无透明通道 · ' + (lastPngSize / 1024).toFixed(1) + ' KiB · 校验通过' });
    } else if (lastPngBlob && lastPngSize <= MAX_FILE_SIZE) {
      msgs.push({ level: 'warn', text: '已生成兼容 PNG，但当前浏览器不能保证 RGB 无透明通道；可下载使用，写入项目图集请改用桌面 Chrome。' });
    }
    // Slug validation for save
    var slug = slugInput.value.trim();
    if (dirHandle) {
      if (!slug) {
        msgs.push({ level: 'warn', text: '保存到图集需要填写文件名 slug。' });
      } else if (!/^[a-z0-9]([a-z0-9-]*[a-z0-9])?$/.test(slug)) {
        msgs.push({ level: 'error', text: 'slug 只允许小写字母、数字和连字符，且不能以连字符开头或结尾。' });
      }
      if (!titleInput.value.trim()) {
        msgs.push({ level: 'warn', text: '保存到图集需要填写标题。' });
      }
    }
    if (msgs.length === 0) {
      validationMsg.textContent = '';
      validationMsg.className = 'validation-msg';
      return;
    }
    // Show highest severity
    var worst = msgs.find(function (m) { return m.level === 'error'; }) ||
                msgs.find(function (m) { return m.level === 'warn'; }) ||
                msgs[0];
    validationMsg.textContent = msgs.map(function (m) { return m.text; }).join(' ');
    validationMsg.className = 'validation-msg ' + worst.level;
  }

  function updateSaveButton() {
    if (!dirHandle) {
      saveDirBtn.textContent = supportsDirectoryAccess ? '选择图集目录' : '浏览器不支持目录保存';
      saveDirBtn.disabled = !supportsDirectoryAccess;
    } else {
      saveDirBtn.textContent = '保存到本地图集';
      var slug = slugInput.value.trim();
      var metadataValid = /^[a-z0-9]([a-z0-9-]*[a-z0-9])?$/.test(slug) &&
        !!titleInput.value.trim();
      var canSave = !!sourceImage && !!lastPngBlob && lastPngIsRgb &&
        lastPngSize > 0 && lastPngSize <= MAX_FILE_SIZE && metadataValid;
      saveDirBtn.disabled = !canSave;
    }
  }

  // Listen to metadata changes
  slugInput.addEventListener('input', function () {
    // Sanitize: lowercase, strip invalid chars
    slugInput.value = slugInput.value.toLowerCase().replace(/[^a-z0-9-]/g, '');
    validateExport();
    updateSaveButton();
  });
  titleInput.addEventListener('input', function () { validateExport(); updateSaveButton(); });
  categoryInput.addEventListener('input', function () { validateExport(); updateSaveButton(); });

  // ── Status ──
  function setStatus(level, text) {
    statusBar.className = 'status-bar ' + level;
    statusText.textContent = text;
  }

  // ── Download ──
  downloadBtn.addEventListener('click', function () {
    if (!lastPngBlob) return;
    var slug = slugInput.value.trim() || 'hink-gallery-image';
    var a = document.createElement('a');
    a.href = URL.createObjectURL(lastPngBlob);
    a.download = slug + '.png';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(a.href);
  });

  // ── File System Access: directory selection ──
  saveDirBtn.addEventListener('click', async function () {
    if (!dirHandle) {
      // Select directory
      if (!('showDirectoryPicker' in window)) {
        setStatus('error', '当前浏览器不支持目录选择，请使用桌面 Chrome，或使用下载按钮。');
        return;
      }
      try {
        dirHandle = await window.showDirectoryPicker({ mode: 'readwrite' });
      } catch (e) {
        if (e.name === 'AbortError') return; // user cancelled
        setStatus('error', '目录选择失败：' + e.message);
        return;
      }

      // A real gallery directory has both metadata files. Requiring both keeps
      // a mistaken ordinary folder from being modified.
      try {
        await dirHandle.getFileHandle('manifest.json');
        await dirHandle.getFileHandle('catalog.json');
      } catch (_) {
        dirHandle = null;
        setStatus('error', '所选目录不是图集目录（需要 manifest.json 和 catalog.json）。请选择 site/assets/gallery/。');
        updateSaveButton();
        return;
      }
      setStatus('ready', '已授权图集目录。');
      updateSaveButton();
      validateExport();
      return;
    }

    // Save to directory
    await saveToDirectory();
  });

  async function saveToDirectory() {
    var slug = slugInput.value.trim();
    var title = titleInput.value.trim();
    var category = categoryInput.value.trim();

    // Validate slug
    if (!slug || !/^[a-z0-9]([a-z0-9-]*[a-z0-9])?$/.test(slug)) {
      setStatus('error', 'slug 不合法：只允许小写字母、数字和连字符。');
      return;
    }
    if (!title) {
      setStatus('error', '标题不能为空。');
      return;
    }
    if (!lastPngBlob || !lastPngIsRgb || lastPngSize > MAX_FILE_SIZE) {
      setStatus('error', '输出必须是 500×244、无透明通道且不超过 300 KiB 的 RGB PNG。');
      return;
    }

    saveDirBtn.disabled = true;
    setStatus('', '正在校验并保存到本地图集…');

    var pngCreated = false;
    var catalogWritten = false;
    var manifestWritten = false;
    var oldCatalogText = '';
    var oldManifestText = '';

    async function readTextFile(name) {
      var handle = await dirHandle.getFileHandle(name);
      return (await handle.getFile()).text();
    }

    async function writeFile(name, content) {
      var handle = await dirHandle.getFileHandle(name, { create: true });
      var writable = await handle.createWritable();
      try {
        await writable.write(content);
        await writable.close();
      } catch (error) {
        try { await writable.abort(); } catch (_) { /* best effort */ }
        throw error;
      }
    }

    try {
      // Read and fully validate both metadata files before writing anything.
      oldCatalogText = await readTextFile('catalog.json');
      oldManifestText = await readTextFile('manifest.json');
      var catalog = JSON.parse(oldCatalogText);
      var manifest = JSON.parse(oldManifestText);
      var metadata = HinkGalleryMakerCore.buildGalleryMetadata(
        catalog,
        manifest,
        { slug: slug, title: title, category: category },
        MAX_GALLERY
      );
      catalog = metadata.catalog;
      manifest = metadata.manifest;

      // Check the actual target too. Permission errors must not be mistaken for
      // a missing file.
      try {
        await dirHandle.getFileHandle(slug + '.png');
        throw new Error('文件 ' + slug + '.png 已存在，第一版不允许覆盖。');
      } catch (error) {
        if (error.name !== 'NotFoundError') throw error;
      }

      // Write only after all validation succeeds. If a later write fails, the
      // catch block restores the original JSON and removes the new PNG.
      await writeFile(slug + '.png', lastPngBlob);
      pngCreated = true;
      await writeFile('catalog.json', JSON.stringify(catalog, null, 2) + '\n');
      catalogWritten = true;
      await writeFile('manifest.json', JSON.stringify(manifest, null, 2) + '\n');
      manifestWritten = true;

      setStatus('ready', '已保存：' + slug + '.png、catalog.json 和 manifest.json 已更新。');
    } catch (e) {
      var rollbackErrors = [];
      if (manifestWritten) {
        try { await writeFile('manifest.json', oldManifestText); } catch (error) { rollbackErrors.push(error.message); }
      }
      if (catalogWritten) {
        try { await writeFile('catalog.json', oldCatalogText); } catch (error) { rollbackErrors.push(error.message); }
      }
      if (pngCreated) {
        try { await dirHandle.removeEntry(slug + '.png'); } catch (error) { rollbackErrors.push(error.message); }
      }
      var rollbackNote = rollbackErrors.length ? '；自动回退未完全成功：' + rollbackErrors.join('、') : '';
      setStatus('error', '保存失败：' + e.message + rollbackNote);
    } finally {
      updateSaveButton();
    }
  }

  // ── Keyboard: Escape clears focus ──
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape') {
      document.activeElement.blur();
    }
  });

  // ── Initial state ──
  setStatus('', '拖放、粘贴或选择一张图片开始。');
  updateSaveButton();
})();
