'use strict';

(function () {
  var reduceMotion = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  // Scroll-spy: highlight the current section in the top navigation.
  var navLinks = Array.prototype.slice.call(document.querySelectorAll('.nav-links a[href^="#"]'));
  var linkByHash = {};
  navLinks.forEach(function (link) { linkByHash[link.getAttribute('href').slice(1)] = link; });
  var sections = navLinks
    .map(function (link) { return document.getElementById(link.getAttribute('href').slice(1)); })
    .filter(Boolean);

  function setActive(id) {
    navLinks.forEach(function (link) { link.classList.toggle('active', link === linkByHash[id]); });
  }

  if ('IntersectionObserver' in window && sections.length) {
    var spy = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) setActive(entry.target.id);
      });
    }, { rootMargin: '-40% 0px -55% 0px' });
    sections.forEach(function (section) { spy.observe(section); });
  }

  // Reveal-on-scroll for analysis blocks; content stays visible without JS.
  if ('IntersectionObserver' in window && !reduceMotion) {
    var revealTargets = document.querySelectorAll('.analysis-block, .release-card, .wiring-card, .photo-card, .ota-panel, .evidence-grid .stat-card');
    revealTargets.forEach(function (el) { el.classList.add('reveal-pending'); });
    var revealer = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) {
          entry.target.classList.add('revealed');
          revealer.unobserve(entry.target);
        }
      });
    }, { rootMargin: '0px 0px -8% 0px' });
    revealTargets.forEach(function (el) { revealer.observe(el); });
  }

  // Copy buttons (e.g. the published SHA-256 digest).
  document.querySelectorAll('.copy-btn[data-copy]').forEach(function (btn) {
    var original = btn.textContent;
    btn.addEventListener('click', function () {
      var text = btn.getAttribute('data-copy');
      function done() {
        btn.textContent = '已复制';
        btn.classList.add('copied');
        window.setTimeout(function () {
          btn.textContent = original;
          btn.classList.remove('copied');
        }, 1600);
      }
      function fallback() {
        var area = document.createElement('textarea');
        area.value = text;
        area.setAttribute('readonly', '');
        area.className = 'clipboard-proxy';
        document.body.appendChild(area);
        area.select();
        try { document.execCommand('copy'); done(); } catch (err) { /* leave the digest selectable */ }
        document.body.removeChild(area);
      }
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(done, fallback);
      } else {
        fallback();
      }
    });
  });

  // Gallery: fetch manifest.json and render art cards.
  var artGrid = document.getElementById('art-grid');
  if (artGrid) {
    var FALLBACK_MSG = '内置图集暂时不可用，可直接打开传图工具上传自己的图片。';
    function showFallback() {
      artGrid.innerHTML = '';
      var p = document.createElement('p');
      p.className = 'art-fallback';
      p.textContent = FALLBACK_MSG;
      artGrid.appendChild(p);
    }

    fetch(artGrid.getAttribute('data-manifest') || 'assets/gallery/manifest.json')
      .then(function (res) {
        if (!res.ok) throw new Error('fetch failed');
        return res.json();
      })
      .then(function (manifest) {
        if (!manifest || !Array.isArray(manifest.images) || manifest.images.length === 0) {
          showFallback();
          return;
        }
        artGrid.innerHTML = '';
        manifest.images.forEach(function (item) {
          var card = document.createElement('a');
          card.className = 'art-card';
          var linkPrefix = artGrid.getAttribute('data-link-prefix') || 'index.html?art=';
          card.href = linkPrefix + encodeURIComponent(item.id);

          var thumb = document.createElement('div');
          thumb.className = 'art-thumb';
          var img = document.createElement('img');
          img.loading = 'lazy';
          img.src = 'assets/gallery/' + item.file;
          img.alt = item.title;
          thumb.appendChild(img);

          var meta = document.createElement('div');
          meta.className = 'art-meta';
          var title = document.createElement('strong');
          title.textContent = item.title;
          var cat = document.createElement('span');
          cat.className = 'art-cat';
          cat.textContent = item.category;
          meta.appendChild(title);
          meta.appendChild(cat);

          card.appendChild(thumb);
          card.appendChild(meta);
          artGrid.appendChild(card);
        });
      })
      .catch(showFallback);
  }

  // Lightbox for zoomable images.
  var zoomImgs = document.querySelectorAll('img[data-zoom]');
  if (zoomImgs.length) {
    var box = document.createElement('div');
    box.className = 'lightbox';
    box.style.display = 'none';
    box.innerHTML = '<img alt="">';
    box.setAttribute('role', 'dialog');
    box.setAttribute('aria-label', '图片预览');
    document.body.appendChild(box);
    var boxImg = box.querySelector('img');

    function open(src, alt) {
      boxImg.src = src;
      boxImg.alt = alt || '';
      box.style.display = 'flex';
      // force reflow before adding the class for the transition
      box.offsetHeight; // eslint-disable-line no-unused-expressions
      box.classList.add('open');
    }

    function close() {
      box.classList.remove('open');
      box.addEventListener('transitionend', function handler() {
        if (!box.classList.contains('open')) box.style.display = 'none';
        box.removeEventListener('transitionend', handler);
      });
    }

    zoomImgs.forEach(function (img) {
      img.addEventListener('click', function () { open(img.src, img.alt); });
    });
    box.addEventListener('click', close);
    document.addEventListener('keydown', function (e) {
      if (e.key === 'Escape' && box.classList.contains('open')) close();
    });
  }
})();
