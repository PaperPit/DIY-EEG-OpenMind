/* guide-kit.js — shared runtime for the 3D guides of «Народное ЭЭГ».
   Classic script (not a module), loaded in <head>, so it also works from a local folder (file://).

   What it does:
   1. Theme: dark "DIY lab" look by default, light theme as a toggle. The choice is shared with the start page (localStorage "eeg-theme").
   2. Render on demand: the 3D scene is drawn only while something moves (rotation, step animation, timers),
      instead of 60 times a second all the time. Saves battery and keeps phones cool.
   3. Adaptive quality: lower pixel density on phones, and lower it further on the fly if frames get slow.
   4. Fallback: if WebGL is missing or the model fails to load, show a clear message and step pictures instead of an endless "Loading…".
   5. Phones: after "Next" the page scrolls so the step text starts right under the 3D view.

   Page API (used by the inline module of each guide):
     GuideKit.dpr()                                    pixel ratio to start with
     GuideKit.loop(renderer, frame, {controls, resize, idleFps})   replaces renderer.setAnimationLoop(frame)
       idleFps: frames per second while nothing moves (a number or a function), 0 = draw nothing when idle
     GuideKit.poke(ms)                                 ask for frames for the next ms milliseconds
*/
(function () {
  'use strict';
  var root = document.documentElement;
  var store = {
    get: function (k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set: function (k, v) { try { localStorage.setItem(k, v); } catch (e) {} }
  };

  /* ---------- 1. theme ---------- */
  root.dataset.theme = store.get('eeg-theme') === 'light' ? 'light' : 'dark';
  function setTheme(t) {
    root.dataset.theme = t; store.set('eeg-theme', t);
    var b = document.querySelector('.kit-theme');
    if (b) b.setAttribute('aria-label', t === 'dark' ? 'Светлая тема' : 'Тёмная тема');
    poke(300);
  }

  /* ---------- 2. render on demand ---------- */
  var now = function () { return performance.now(); };
  var raf0 = window.requestAnimationFrame.bind(window);
  var st0 = window.setTimeout.bind(window);
  var activeUntil = 0, L = null, scheduled = false, idleTimer = 0, onScreen = true, lastDraw = 0;
  var REDUCED = matchMedia('(prefers-reduced-motion: reduce)').matches;

  function poke(ms) {
    activeUntil = Math.max(activeUntil, now() + (ms || 300));
    wake();
  }
  function wake() {
    if (!L || scheduled || !onScreen || document.hidden) return;
    scheduled = true; raf0(tick);
  }
  // Step animations in the guides run on requestAnimationFrame and setTimeout. Any such call means "something moves".
  window.requestAnimationFrame = function (cb) { activeUntil = Math.max(activeUntil, now() + 150); wake(); return raf0(cb); };
  window.setTimeout = function (fn, ms) {
    if (typeof fn !== 'function') return st0.apply(window, arguments);
    var args = Array.prototype.slice.call(arguments, 2);
    return st0(function () { try { return fn.apply(this, args); } finally { poke(250); } }, ms);
  };

  function tick(t) {
    scheduled = false;
    if (!L) return;
    var n = now(), active = n < activeUntil;
    L.frame(t);
    lastDraw = n;
    if (active) measure(n);
    else lastFrame = 0;
    if (active) wake();
    else {
      var fps = typeof L.idleFps === 'function' ? L.idleFps() : L.idleFps;
      if (fps) { clearTimeout(idleTimer); idleTimer = st0(wake, 1000 / fps); }
    }
  }

  /* ---------- 3. adaptive quality ---------- */
  var coarse = matchMedia('(pointer: coarse)').matches;
  var startDpr = Math.min(window.devicePixelRatio || 1, coarse ? 1.5 : 2);
  var dpr = startDpr, lastFrame = 0, slow = 0, frames = 0, coolUntil = 0;
  function measure(n) {
    if (lastFrame && n - lastFrame < 250) {
      frames++;
      if (n - lastFrame > 34) slow++;            // slower than ~30 frames per second
      if (frames >= 40) {
        if (slow > 24 && dpr > 0.75 && n > coolUntil) {
          dpr = Math.max(0.75, dpr - 0.25);
          L.renderer.setPixelRatio(dpr);
          if (L.resize) L.resize();
          coolUntil = n + 2000;
        }
        frames = 0; slow = 0;
      }
    }
    lastFrame = n;
  }

  function loop(renderer, frame, opts) {
    opts = opts || {};
    L = { renderer: renderer, frame: frame, resize: opts.resize, idleFps: REDUCED ? 0 : (opts.idleFps || 0) };
    if (opts.controls) {
      opts.controls.addEventListener('change', function () { poke(200); });
      opts.controls.addEventListener('start', function () { poke(600); });
    }
    var cv = renderer.domElement;
    if ('IntersectionObserver' in window) {
      new IntersectionObserver(function (es) { onScreen = es[0].isIntersecting; if (onScreen) poke(200); }).observe(cv);
    }
    if ('ResizeObserver' in window) new ResizeObserver(function () { poke(200); }).observe(cv.parentNode || cv);
    ['pointerdown', 'pointermove', 'wheel', 'keydown', 'touchstart'].forEach(function (ev) {
      addEventListener(ev, function (e) { if (ev !== 'pointermove' || e.buttons) poke(400); }, { passive: true });
    });
    addEventListener('resize', function () { poke(300); });
    document.addEventListener('visibilitychange', function () { if (!document.hidden) poke(300); });
    new MutationObserver(function () { poke(200); }).observe(root, { attributes: true, attributeFilter: ['data-theme'] });
    var ld = document.getElementById('loading'); if (ld) ld.remove();
    poke(1500);
  }

  /* ---------- 4. fallback ---------- */
  function hasWebGL() {
    try { var c = document.createElement('canvas'); return !!(c.getContext('webgl2') || c.getContext('webgl')); } catch (e) { return false; }
  }
  function fallback(why) {
    var ld = document.getElementById('loading');
    if (!ld || ld.classList.contains('fail')) return;
    ld.classList.add('fail');
    var spec = (root.getAttribute('data-steps') || '').split(':'), pre = spec[0], n = +spec[1] || 0, th = '';
    for (var i = 1; i <= n; i++) {
      var id = pre + (i < 10 ? '0' : '') + i;
      th += '<figure><img src="img/steps/' + id + '.jpg" alt="" loading="lazy"><figcaption>Шаг ' + i + '</figcaption></figure>';
    }
    ld.innerHTML = '<div class="fb"><b>3D-модель не открылась</b><p>' + why + '</p>' +
      '<p>Что можно сделать: обновите браузер, включите в настройках браузера аппаратное ускорение или откройте страницу на компьютере. ' +
      'Печатная версия шагов: <a href="03-checklist.html">чек-лист</a>.</p>' + (th ? '<div class="fbthumbs">' + th + '</div>' : '') + '</div>';
  }
  if (!hasWebGL()) {
    document.addEventListener('DOMContentLoaded', function () {
      fallback('Этот браузер или устройство не поддерживает WebGL, без него 3D не работает.');
    });
  }
  addEventListener('error', function (e) {
    if (!document.getElementById('loading')) return;
    var msg = String(e.message || '');
    fallback(/WebGL/i.test(msg) ? 'Браузер не дал включить WebGL (графику 3D). Иногда он выключен в настройках или драйвер видеокарты устарел.'
      : 'Ошибка при загрузке модели: ' + (msg || 'неизвестная').slice(0, 160));
  });
  if (!window.THREE_KIT) {
    document.addEventListener('DOMContentLoaded', function () {
      if (!window.THREE_KIT) fallback('Не загрузилась библиотека three.js (vendor/three-kit.r160.min.js). Если вы открыли файл из папки, проверьте, что рядом лежат папки vendor и assets.');
    });
  }

  /* ---------- 5. page chrome: theme button, phone scrolling ---------- */
  document.addEventListener('DOMContentLoaded', function () {
    var brand = document.querySelector('.phead .brand');
    if (brand) {
      var b = document.createElement('button');
      b.type = 'button'; b.className = 'kit-theme';
      b.setAttribute('aria-label', root.dataset.theme === 'dark' ? 'Светлая тема' : 'Тёмная тема');
      b.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z" fill="currentColor"/></svg>';
      b.addEventListener('click', function () { setTheme(root.dataset.theme === 'dark' ? 'light' : 'dark'); });
      brand.appendChild(b);
    }
    var pbody = document.getElementById('pbody'), stage = document.querySelector('.stage');
    var narrow = matchMedia('(max-width: 900px)');
    if (pbody && stage) {
      new MutationObserver(function () {
        // the step nav keeps the current step in view
        var cur = document.querySelector('.steps button.cur');
        if (cur && cur.scrollIntoView && narrow.matches) cur.scrollIntoView({ block: 'nearest', inline: 'center' });
        if (!narrow.matches) return;
        var top = pbody.getBoundingClientRect().top + scrollY - stage.getBoundingClientRect().height - 8;
        if (scrollY > top + 4) window.scrollTo({ top: top, behavior: REDUCED ? 'auto' : 'smooth' });
      }).observe(pbody, { childList: true });
    }
  });

  window.GuideKit = { dpr: function () { return dpr; }, loop: loop, poke: poke, setTheme: setTheme };
})();
