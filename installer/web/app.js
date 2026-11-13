/* app.js - the Gameplay+ Installer page (the installer design, ported to plain HTML/CSS/JS).
 * Cards and the Mayhem section are built from the server's catalogue; every control writes a real sub-option.
 * Screens: main -> progress -> done, restore; dialogs: game running, restore confirm (Esc closes). */
(function () {
  'use strict';
  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => [...r.querySelectorAll(s)];
  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  const params = new URLSearchParams(location.search);
  const ICON = {
    check: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="square" aria-hidden="true"><path d="M5 12.5l4.5 4.5L19 7.5"></path></svg>',
    chev: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="square" aria-hidden="true"><path d="M6 9l6 6 6-6"></path></svg>',
    warn: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" aria-hidden="true"><path d="M12 3L1.8 20.5h20.4z"></path><path d="M12 9.5v5.5M12 17.2v1.3"></path></svg>',
    selIco: '<svg class="sel-ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.8" stroke-linecap="square" aria-hidden="true"><path d="M6 9l6 6 6-6"></path></svg>',
    shield: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="square" aria-hidden="true"><path d="M12 2.8l7.8 2.9v5.9c0 4.6-3.3 8.3-7.8 9.6-4.5-1.3-7.8-5-7.8-9.6V5.7z"></path><path d="M8.3 12l2.6 2.6 4.8-5"></path></svg>',
    x: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3.2" stroke-linecap="square" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18"></path></svg>',
  };
  const S = {
    cat: null, sel: null, state: null, installed: null, check: null, game: '', backupRoot: '', kind: 'full',
    open: new Set(), listen: null, job: null, polling: null, backups: [], pick: null, restoreRoot: '',
    score: null, audioCtx: null, muted: false, volume: 0.8, unlocked: false, stepSeen: {}, lastFocus: null,
  };

  async function api(path, body, tries = 6) {
    for (let i = 0; ; i++) {
      try {
        const r = await fetch(path, body === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
        return await r.json();
      } catch (e) {                         // the server is busy (data build): try again shortly
        if (i >= tries - 1) throw e;
        await new Promise((res) => setTimeout(res, 300 * (i + 1)));
      }
    }
  }
  window.addEventListener('error', (e) => { api('/api/log', { msg: String(e.message) + ' @' + e.lineno }).catch(() => {}); });

  // ------------------------------------------------------------------ screens and dialogs
  const SCREENS = ['main', 'progress', 'done', 'restore'];
  function show(name, focusSel) {
    for (const s of SCREENS) $('#scr-' + s).hidden = s !== name;
    if (name === 'restore') loadBackups();
    if (focusSel) setTimeout(() => { const e = $(focusSel); if (e) e.focus(); }, 40);
  }
  function openDlg(id) {
    S.lastFocus = document.activeElement;
    const d = $('#' + id); d.hidden = false;
    setTimeout(() => { const f = $('[data-autofocus]', d); if (f) f.focus(); }, 30);
  }
  function closeDlg(id) {
    $('#' + id).hidden = true;
    if (S.lastFocus && S.lastFocus.focus) S.lastFocus.focus();
  }
  $$('.overlay').forEach((o) => {
    $$('[data-close]', o).forEach((b) => b.addEventListener('click', () => closeDlg(o.id)));
    o.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') { e.preventDefault(); closeDlg(o.id); }
      if (e.key === 'Tab') {                       // keep focus inside the dialog
        const f = $$('button', o); const i = f.indexOf(document.activeElement);
        if (e.shiftKey && i <= 0) { e.preventDefault(); f[f.length - 1].focus(); }
        else if (!e.shiftKey && i === f.length - 1) { e.preventDefault(); f[0].focus(); }
      }
    });
  });

  // ------------------------------------------------------------------ cards
  const on = (id) => !!S.sel.groups[id];
  const missing = (g) => (g.requires || []).filter((r) => !on(r));
  const titleOf = (id) => (S.cat.groups.find((g) => g.id === id) || {}).title || id;
  const mayhemGroups = () => S.cat.groups.filter((g) => g.section === 'mayhem');
  const heat = () => mayhemGroups().filter((g) => on(g.id)).length;
  // the design draws three fire heights: 1, 2, 3+ Mayhem toggles (the music adds one stem per toggle up to 4)
  const fireLevel = () => Math.min(3, heat());
  const subSpec = (id) => { for (const g of S.cat.groups) { const s = g.subs.find((x) => x.id === id); if (s) return s; } return null; };
  // a sub-option is usable when its card is on and the sub-option it needs (and that one's own need) is on
  function subReady(s) {
    for (let r = s.requires, n = 0; r && n < 8; n++) { if (!S.sel.subs[r]) return false; const p = subSpec(r); r = p && p.requires; }
    return true;
  }

  function cardHTML(g) {
    const mh = g.section === 'mayhem';
    const c = mh ? 'mh-card' : 'card';
    const inline = mh ? (g.id === 'difficulty' ? '<div class="lvls"><span class="lvl lvl--hard">HARD</span><span class="lvl lvl--brutal">BRUTAL</span><span class="lvl lvl--psycho">PSYCHO</span></div>' : '') : '';
    const range = mh && g.id === 'more_enemies' ? `
      <div class="mh-range"><input class="rng rng--hot" type="range" min="6" max="9" step="1" data-sub="max_live_enemies" aria-label="Enemies at once"><output class="mh-count"></output></div>
      <div class="ticks" aria-hidden="true"><span>6</span><span>7</span><span>8</span><span>9</span></div>` : '';
    const needs = (g.requires || []).length ? `<p class="mh-needs" hidden>Needs ${esc(g.requires.map((r) => titleOf(r).toUpperCase()).join(' + '))} on.</p>` : '';
    const subs = g.subs.filter((s) => !(mh && s.id === 'max_live_enemies')).map((s) => subHTML(g, s)).join('');
    return `<div class="${c}" data-id="${g.id}">
      <div class="${c}-head">
        <div class="card-tt"><h3 class="${c}-title" id="t-${g.id}">${esc(g.title.toUpperCase())}</h3><div class="badges"></div></div>
        <button type="button" class="sw${mh ? ' sw--hot' : ''}" role="switch" aria-checked="false" aria-labelledby="t-${g.id}"><span class="sw-knob"></span><span class="sw-txt" aria-hidden="true">OFF</span></button>
      </div>
      ${g.warn_off ? `<p class="warn" role="status" hidden>${ICON.warn}<span>${esc(g.warn_off)}</span></p>` : ''}
      <p class="warn warn--check" role="status" hidden>${ICON.warn}<span></span></p>
      ${inline}${range}${needs}
      <button type="button" class="card-more" aria-expanded="false" aria-controls="d-${g.id}"><span>Details</span>${ICON.chev}</button>
      <div class="card-details" id="d-${g.id}">
        <p class="card-desc">${esc(g.tagline)}</p>
        ${g.note ? `<p class="card-note">${esc(g.note)}</p>` : ''}
        ${g.id === 'display' ? '<p class="card-note card-note--kept" hidden></p>' : ''}
        ${g.experimental ? `<p class="exp-note">${ICON.warn}<span>Experimental: may have bugs; turn it off if you hit problems.</span></p>` : ''}
        ${subs}
        <ul class="fx"></ul>
      </div>
    </div>`;
  }

  function subHTML(g, s) {
    const id = 'sub-' + s.id;
    const w = `data-subwrap="${s.id}"`;
    if (s.type === 'check') {
      const tag = s.experimental ? ' <span class="tag-exp tag-exp--sm" title="Experimental"><span>Experimental</span></span>' : '';
      return `<label class="chk" ${w}><input type="checkbox" id="${id}" data-sub="${s.id}"><span class="chk-box">${ICON.check}</span><span>${esc(s.label)}${tag}</span></label>`;
    }
    if (s.type === 'select' && s.options.length <= 2) {
      return `<fieldset class="opts" ${w} data-group-sub="${s.id}"><legend class="${s.id === 'display_mode' ? 'field-label' : 'sr-only'}">${esc(s.label)}</legend>${s.options.map(([v, l]) =>
        `<label class="opt"><input type="radio" name="${id}" value="${esc(v)}" data-sub="${s.id}"><span class="opt-dot"></span><span>${esc(l)}</span></label>`).join('')}</fieldset>`;
    }
    if (s.type === 'select') {
      return `<div class="field" ${w}><label class="field-label" for="${id}">${esc(s.label)}</label><div class="sel-wrap"><select id="${id}" class="sel" data-sub="${s.id}">${
        s.options.map(([v, l]) => `<option value="${esc(v)}">${esc(l)}</option>`).join('')}</select>${ICON.selIco}</div></div>`;
    }
    if (s.type === 'range') {
      return `<div class="field" ${w}><div class="field-row"><label class="field-label" for="${id}">${esc(s.label)}</label><output class="field-val" for="${id}"></output></div>
        <input id="${id}" class="rng" type="range" min="${s.min}" max="${s.max}" step="${s.step}" data-sub="${s.id}"></div>`;
    }
    if (s.type === 'file') {
      return `<div class="field filerow" ${w}><span class="field-label" id="f-${s.id}">${esc(s.label)}</span>
        <div class="filepick"><span class="filepath" data-file="${s.id}" aria-labelledby="f-${s.id}"></span>
        <button type="button" class="btn-ghost btn-ghost--sm" data-pick="${s.id}">Choose file</button>
        <button type="button" class="btn-ghost btn-ghost--sm" data-clear="${s.id}">Clear</button></div>
        <p class="file-status" data-status="${s.id}" role="status"></p></div>`;
    }
    if (s.type === 'key') {
      return `<div class="keyrow" ${w}><span class="field-label" id="k-${s.id}">${esc(s.label)}</span><button type="button" class="key" data-key="${s.id}" aria-labelledby="k-${s.id} kv-${s.id}"><span id="kv-${s.id}"></span></button></div>`;
    }
    return '';
  }

  function buildCards() {
    $('#cards').innerHTML = S.cat.groups.filter((g) => g.section === 'main').map(cardHTML).join('');
    $('#mh-grid').innerHTML = mayhemGroups().map(cardHTML).join('');
    $('#soon').innerHTML = S.cat.coming_soon.map((s) => `<li class="chip-soon" title="${esc(s.tagline)}">${esc(s.title)}</li>`).join('');
    $('.mh-soon').hidden = !S.cat.coming_soon.length;       // every chip of the first version is a real toggle now
    for (const g of S.cat.groups) {
      const c = $(`[data-id="${g.id}"]`);
      $('.sw', c).addEventListener('click', () => toggle(g.id));
      $('.card-more', c).addEventListener('click', () => { if (S.open.has(g.id)) S.open.delete(g.id); else S.open.add(g.id); render(); });
      $$('[data-sub]', c).forEach((inp) => {
        const sid = inp.dataset.sub, spec = g.subs.find((x) => x.id === sid);
        const ev = inp.type === 'range' ? 'input' : 'change';
        inp.addEventListener(ev, () => {
          if (spec.type === 'check') S.sel.subs[sid] = inp.checked;
          else if (spec.type === 'range') S.sel.subs[sid] = Number(inp.value);
          else S.sel.subs[sid] = inp.value;
          // picking a start mode is an explicit pick: it should apply at the next start
          if (sid === 'display_mode') S.sel.subs.display_apply = true;
          changed(spec.type === 'range');
        });
      });
      $$('[data-key]', c).forEach((b) => keyField(b, g));
      $$('[data-pick]', c).forEach((b) => b.addEventListener('click', async () => {
        const sid = b.dataset.pick, spec = g.subs.find((x) => x.id === sid);
        const r = await api('/api/browse-file', { title: 'Pick your copy of the song', filter: spec.filetypes, start: S.sel.subs[sid] });
        if (r.path) { S.sel.subs[sid] = r.path; changed(); checkSong(sid); }
      }));
      $$('[data-clear]', c).forEach((b) => b.addEventListener('click', () => { S.sel.subs[b.dataset.clear] = ''; S.songMsg = null; changed(); }));
    }
  }

  // a picked song file: the server reads it and says whether it is the song (the install repeats the same check)
  async function checkSong(sid) {
    const path = S.sel.subs[sid];
    S.songMsg = path ? { path, busy: true } : null; render();
    if (!path) return;
    let r;
    try { r = await api('/api/song', { path }); } catch (e) { r = { ok: false, message: 'The installer could not check the file.' }; }
    if (S.sel.subs[sid] === path) { S.songMsg = Object.assign({ path }, r); render(); }
  }

  // key fields: click, then press a key (or a side / middle mouse button); Esc cancels, Tab leaves
  const CODE = {
    Space: 'SPACE', Tab: 'TAB', Enter: 'RETURN', ShiftLeft: 'LSHIFT', ShiftRight: 'RSHIFT', ControlLeft: 'LCONTROL',
    ControlRight: 'RCONTROL', AltLeft: 'LALT', AltRight: 'RALT', CapsLock: 'CAPSLOCK', Backquote: 'GRAVE', Minus: 'MINUS',
    Equal: 'EQUALS', BracketLeft: 'LBRACKET', BracketRight: 'RBRACKET', Backslash: 'BACKSLASH', Semicolon: 'SEMICOLON',
    Quote: 'APOSTROPHE', Comma: 'COMMA', Period: 'PERIOD', Slash: 'SLASH', Backspace: 'BACKSPACE', NumpadAdd: 'ADD',
    NumpadSubtract: 'SUBTRACT', NumpadMultiply: 'MULTIPLY', NumpadDivide: 'DIVIDE', NumpadDecimal: 'DECIMAL',
    NumpadEnter: 'NUMPADENTER', ArrowUp: 'UP', ArrowDown: 'DOWN', ArrowLeft: 'LEFT', ArrowRight: 'RIGHT', Home: 'HOME',
    End: 'END', PageUp: 'PGUP', PageDown: 'PGDN', Insert: 'INSERT', Delete: 'DELETE', Pause: 'PAUSE',
    NumLock: 'NUMLOCK', ScrollLock: 'SCROLL',
  };
  function keyName(e) {
    const c = e.code || '';
    if (/^Key[A-Z]$/.test(c)) return c.slice(3);
    if (/^Digit[0-9]$/.test(c)) return c.slice(5);
    if (/^F([1-9]|1[0-2])$/.test(c)) return c;
    if (/^Numpad[0-9]$/.test(c)) return 'NUMPAD' + c.slice(6);
    return CODE[c] || null;
  }
  const keyLabel = (k) => ({ MOUSE3: 'Middle mouse', MOUSE4: 'Mouse 4', MOUSE5: 'Mouse 5', SPACE: 'Space', LSHIFT: 'L Shift', RSHIFT: 'R Shift',
    LCONTROL: 'L Ctrl', RCONTROL: 'R Ctrl', LALT: 'L Alt', RALT: 'R Alt', RETURN: 'Enter', CAPSLOCK: 'Caps Lock' }[k] || k);
  function keyField(b, g) {
    const sid = b.dataset.key;
    const set = (k) => {
      const other = S.cat.groups.flatMap((x) => x.subs).filter((x) => x.type === 'key' && x.id !== sid);
      const clash = other.find((x) => S.sel.subs[x.id] === k);
      if (clash) { S.keyMsg = `${keyLabel(k)} is already the ${clash.label.toLowerCase()} key.`; S.listen = null; render(); return; }
      if (S.cat.key_names && !S.cat.key_names.includes(k)) { S.keyMsg = keyLabel(k) + ' cannot be used.'; S.listen = null; render(); return; }
      S.keyMsg = ''; S.sel.subs[sid] = k; S.listen = null; changed();
    };
    b.addEventListener('click', () => { S.listen = S.listen === sid ? null : sid; S.keyMsg = ''; render(); });
    b.addEventListener('blur', () => { if (S.listen === sid) { S.listen = null; render(); } });
    b.addEventListener('keydown', (e) => {
      if (S.listen !== sid) return;
      if (e.key === 'Tab') { S.listen = null; render(); return; }
      e.preventDefault();
      if (e.key === 'Escape') { S.listen = null; render(); return; }
      const k = keyName(e);
      if (k) set(k); else { S.keyMsg = 'That key cannot be used.'; S.listen = null; render(); }
    });
    b.addEventListener('mousedown', (e) => {
      if (S.listen !== sid || e.button === 0) return;
      e.preventDefault();
      const k = { 1: 'MOUSE3', 3: 'MOUSE4', 4: 'MOUSE5' }[e.button];
      if (k) set(k);
    });
    b.addEventListener('contextmenu', (e) => { if (S.listen === sid) e.preventDefault(); });
  }

  const fill = (f) => `calc(9px + (100% - 18px) * ${Math.max(0, Math.min(1, f)).toFixed(3)}) 8px, 100% 8px`;

  function render() {
    const det = (S.installed && (S.installed.saved || S.installed.detected)) || null;
    const effects = (S.check && S.check.effects) || {};
    for (const g of S.cat.groups) {
      const c = $(`[data-id="${g.id}"]`), isOn = on(g.id), miss = missing(g), open = S.open.has(g.id);
      c.className = (g.section === 'mayhem' ? 'mh-card ' : 'card ') + (isOn ? 'is-on' : 'is-off') + (open ? ' is-open' : '') +
        (g.warn_off && !isOn ? ' is-warn' : '') + (miss.length ? ' is-needs' : '');
      const sw = $('.sw', c);
      sw.setAttribute('aria-checked', isOn ? 'true' : 'false');
      sw.disabled = miss.length > 0;
      $('.sw-txt', sw).textContent = isOn ? 'ON' : 'OFF';
      const w = $('.warn', c); if (w) w.hidden = isOn;
      const more = $('.card-more', c);
      more.setAttribute('aria-expanded', open ? 'true' : 'false');
      $('span', more).textContent = open ? 'Hide details' : 'Details';
      // badges: experimental (hazard tag) + install state
      let b = g.experimental ? '<span class="tag-exp" title="Experimental"><span>Experimental</span></span>' : '';
      if (det) {
        const was = !!det.groups[g.id];
        if (was && isOn) b += `<span class="badge badge--now">${ICON.check}Installed now</span>`;
        else if (was && !isOn) b += '<span class="badge badge--off">Will be switched off</span>';
        else if (!was && isOn) b += '<span class="badge badge--new">New</span>';
      }
      $('.badges', c).innerHTML = b;
      const needs = $('.mh-needs', c); if (needs) needs.hidden = !miss.length;
      // a warning of the check that belongs to this card (HD Effects: another d3d9.dll is set aside)
      const cw = $('.warn--check', c);
      let cwt = ((S.check && S.check.warnings) || []).find((x) => g.id === 'hdfx' && /d3d9\.dll/.test(x));
      if (g.id === 'suave' && !S.sel.subs.suave_song) cwt = 'The music is not included. No song file chosen: Suave installs without music. Open Details to pick your own copy of the song.';
      if (g.id === 'suave' && S.sel.subs.suave_song && S.songMsg && S.songMsg.path === S.sel.subs.suave_song && S.songMsg.ok === false && !S.songMsg.busy) cwt = 'Your song file was not accepted: ' + S.songMsg.message;
      cw.hidden = !(cwt && isOn); if (cwt) $('span', cw).textContent = cwt;
      if (g.id === 'display') {
        const kept = S.check && S.check.display_kept, kn = $('.card-note--kept', c);
        kn.hidden = !kept;
        if (kept) kn.textContent = `In game you kept ${kept.w}\u00d7${kept.h} ${kept.mode}: that mode wins at the next start` +
          (S.sel.subs.display_apply ? ' unless you untick the box below.' : '. Tick the box below to start in the mode above instead.');
        const ap = $('[data-subwrap="display_apply"]', c); if (ap) ap.hidden = !kept;
      }
      // sub-options
      for (const s of g.subs) {
        const v = S.sel.subs[s.id];
        const ready = subReady(s);
        const dis = !isOn || !ready;
        const wrap = $(`[data-subwrap="${s.id}"]`, c);
        if (wrap) { wrap.classList.toggle('is-dim', isOn && !ready); wrap.title = isOn && !ready ? 'Needs: ' + ((subSpec(s.requires) || {}).label || s.requires) : ''; }
        if (s.type === 'key') {
          const kb = $(`[data-key="${s.id}"]`, c);
          const listening = S.listen === s.id;
          kb.classList.toggle('is-listening', listening);
          $('span', kb).textContent = listening ? 'Press a key' : keyLabel(v);
          kb.disabled = dis;
          continue;
        }
        if (s.type === 'file') {
          const pth = $(`[data-file="${s.id}"]`, c), st = $(`[data-status="${s.id}"]`, c);
          const m = S.songMsg && S.songMsg.path === v ? S.songMsg : null;
          pth.textContent = v || 'No file chosen'; pth.title = v || '';
          pth.classList.toggle('is-empty', !v);
          $(`[data-pick="${s.id}"]`, c).disabled = dis; $(`[data-clear="${s.id}"]`, c).disabled = dis || !v;
          st.className = 'file-status' + (m && !m.busy ? (m.ok ? ' is-ok' : ' is-bad') : '');
          st.textContent = !v ? 'No file: Suave installs and works without music.' : !m ? '' : m.busy ? 'Checking the file\u2026' : m.message;
          continue;
        }
        if (s.type === 'select' && s.options.length <= 2) {
          $$(`input[data-sub="${s.id}"]`, c).forEach((r) => { r.checked = r.value === String(v); r.disabled = dis; });
          continue;
        }
        const inp = $(`[data-sub="${s.id}"]`, c);
        if (!inp) continue;
        if (s.type === 'check') inp.checked = !!v; else inp.value = v;
        inp.disabled = dis;
        if (s.type === 'range') {
          inp.style.backgroundSize = fill((v - s.min) / (s.max - s.min));
          const out = $('output', inp.closest('.field, .mh-card'));
          if (out) out.textContent = v + (s.unit || '');
        }
      }
      let km = $('.key-msg', c);
      if (g.subs.some((s) => s.type === 'key')) {
        if (!km) { km = document.createElement('p'); km.className = 'key-msg'; $('.fx', c).before(km); }
        km.textContent = S.keyMsg || ''; km.hidden = !S.keyMsg;
      }
      const fx = (effects[g.id] || []).map((e) => `<li><b class="k-${e.kind}">${e.kind === 'note' ? 'info' : e.kind}</b><code>${esc(e.text)}</code></li>`);
      if (g.section === 'mayhem' && effects.mayhem_title) fx.push(...effects.mayhem_title.map((e) => `<li><b class="k-${e.kind}">${e.kind}</b><code>${esc(e.text)}</code></li>`));
      $('.fx', c).innerHTML = fx.join('');
    }
    const h = heat(), fl = fireLevel();
    const mh = $('#mayhem');
    mh.className = 'mayhem heat-' + fl + (S.rm ? ' rm' : '');
    $('#win').className = 'to-win win-heat-' + fl + (S.rm ? ' rm' : '');
    const ex = $('#mh-extra');
    ex.hidden = !(S.state && S.state.mayhem_title);
    ex.textContent = h ? 'MAYHEM burns through the title screen: on' : 'MAYHEM title screen: on with any Mayhem toggle';
    ex.classList.toggle('is-on', h > 0);
  }

  function enforce() {
    let ch = true;
    while (ch) {
      ch = false;
      for (const g of S.cat.groups) if (on(g.id) && missing(g).length) { S.sel.groups[g.id] = false; ch = true; }
    }
  }

  function toggle(id) {
    const g = S.cat.groups.find((x) => x.id === id);
    if (missing(g).length) return;
    S.sel.groups[id] = !S.sel.groups[id];
    enforce();
    if (g.section === 'mayhem') music();
    changed();
  }

  // ------------------------------------------------------------------ music (stems in web/music, read only)
  function ensureAudio() {
    if (S.audioCtx) return;
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    S.audioCtx = new AC();
    S.score = new MayhemMusic(S.audioCtx, S.audioCtx.destination);
    S.score.setVolume(S.muted ? 0 : S.volume);
    S.score.load();
  }
  function music() {
    if (!S.unlocked || !(S.state && S.state.music && S.state.music.available)) return;   // silent until the first click
    ensureAudio();
    if (!S.score) return;
    if (S.audioCtx.state === 'suspended') S.audioCtx.resume();
    S.score.setVolume(S.muted ? 0 : S.volume);
    S.score.setLevel(heat(), mayhemGroups().length);
  }
  function renderAudio() {
    const m = $('#mute');
    m.setAttribute('aria-pressed', S.muted ? 'true' : 'false');
    m.setAttribute('aria-label', S.muted ? 'Unmute music' : 'Mute music');
    $('#audio').classList.toggle('is-muted', S.muted);
    const v = $('#volume'); v.value = Math.round(S.volume * 100); v.style.setProperty('--fill', `calc(9px + (100% - 18px) * ${S.volume.toFixed(3)})`);
  }
  document.addEventListener('pointerdown', () => {
    if (S.unlocked) return;
    S.unlocked = true;
    setTimeout(() => { if (S.cat) music(); }, 0);
  }, true);
  $('#mute').addEventListener('click', () => {
    S.muted = !S.muted; renderAudio();
    if (S.score) S.score.setVolume(S.muted ? 0 : S.volume);
    api('/api/prefs', { muted: S.muted });
  });
  $('#volume').addEventListener('input', (e) => {
    S.volume = Number(e.target.value) / 100;
    if (S.muted) { S.muted = false; api('/api/prefs', { muted: false }); }
    renderAudio();
    if (S.score) S.score.setVolume(S.volume);
  });
  $('#volume').addEventListener('change', () => api('/api/prefs', { volume: S.volume }));
  function renderCredit(m) {
    const c = $('#credit');
    const meta = (m && m.meta) || {};
    const line = m && m.available ? (typeof meta.credit === 'string' ? meta.credit : [meta.title, meta.artist, meta.license].filter(Boolean).join(' · ')) : '';
    c.hidden = !line;
    c.textContent = line ? 'Music: ' + (meta.title ? meta.title + ': ' : '') + line : '';
    c.title = c.textContent;
  }
  window.mayhemMusicState = () => S.score ? { running: S.score.running, fading: S.score.fading, level: S.score.level, bar: S.score.barSeconds,
    loopEnd: S.score.loopEnd, bus: +S.score.bus.gain.value.toFixed(3), stems: S.score.stems.map((x) => ({ name: x.name, loaded: !!x.buffer, gain: +x.gain.gain.value.toFixed(3) })),
    ctx: S.audioCtx.state } : null;

  // ------------------------------------------------------------------ check / footer
  let checkTimer = null, checkSeq = 0;
  function changed(slow) {
    render();
    clearTimeout(checkTimer);
    checkTimer = setTimeout(runCheck, slow ? 300 : 100);
  }
  async function runCheck() {
    const seq = ++checkSeq;
    const r = await api('/api/check', { game: S.game, selection: S.sel, backup_root: S.backupRoot, backup_kind: S.kind });
    if (seq !== checkSeq) return r;
    S.check = r;
    render(); renderFoot();
    return r;
  }
  const isRunningProblem = (p) => /is running/i.test(p);
  function renderFoot() {
    const c = S.check || {};
    const badge = $('#exe-badge');
    if (c.exe && c.exe.present) {
      badge.className = 'loc-ok' + (c.exe.match ? '' : ' is-warn');
      badge.innerHTML = c.exe.match ? ICON.check + '<span>GOG TOD.exe</span>' : ICON.warn + '<span>Other TOD.exe</span>';
      badge.title = c.exe.text;
    } else { badge.className = 'loc-ok is-bad'; badge.innerHTML = ICON.x + '<span>No TOD.exe</span>'; badge.title = ''; }
    const sp = $('#space-txt'), meter = $('#meter');
    let worst = null;
    if (c.space) for (const [d, e] of Object.entries(c.space.drives)) if (!worst || e.need / Math.max(1, e.free) > worst.need / Math.max(1, worst.free)) worst = Object.assign({ d }, e);
    if (worst) {
      sp.textContent = `Needs ${worst.need_h} on ${worst.d}, ${worst.free_h} free`;
      sp.title = Object.entries(c.space.drives).map(([d, e]) => `${d}: ${e.what.join(', ')}`).join('; ');
      sp.classList.toggle('is-low', !worst.ok);
      meter.classList.toggle('is-low', !worst.ok);
      $('span', meter).style.width = Math.min(100, Math.max(2, worst.need / Math.max(1, worst.free) * 100)).toFixed(1) + '%';
    } else { sp.textContent = ''; $('span', meter).style.width = '0%'; }
    const probs = (c.problems || []).filter((p) => !isRunningProblem(p));
    const msgs = probs.map((p) => `<span class="m-bad">${esc(p)}</span>`).concat((c.warnings || [])
      .filter((w) => !/Game may crash|double-free crash/i.test(w) && !(on('hdfx') && /d3d9\.dll/.test(w)))   // that one is on its card
      .map((w) => `<span class="m-warn">${esc(w)}</span>`));
    const fm = $('#foot-msg'); fm.innerHTML = msgs.join(''); fm.hidden = !msgs.length;
    // INSTALL: off when something blocks it; a running game is answered by the dialog on click
    $('#install').disabled = !c.plan || probs.length > 0;
  }

  $('#game').addEventListener('change', (e) => loadState(e.target.value.trim()));
  $('#backup-root').addEventListener('change', (e) => { S.backupRoot = e.target.value.trim(); runCheck(); });
  $('#game-browse').addEventListener('click', async () => {
    const r = await api('/api/browse', { title: 'Pick the Total Overdose folder (the one with TOD.exe)', start: S.game });
    if (r.path) loadState(r.path);
  });
  $('#backup-browse').addEventListener('click', async () => {
    const r = await api('/api/browse', { title: 'Pick where the Gameplay+ backups go', start: S.backupRoot });
    if (r.path) { S.backupRoot = r.path; $('#backup-root').value = r.path; runCheck(); }
  });
  $$('input[name="bk-kind"]').forEach((r) => r.addEventListener('change', () => { S.kind = r.value; runCheck(); }));

  // ------------------------------------------------------------------ install / jobs
  async function tryInstall() {
    const btn = $('#install');
    if (btn.disabled) return;
    btn.setAttribute('aria-busy', 'true');
    const c = await runCheck();
    btn.removeAttribute('aria-busy');
    if ((c.problems || []).some(isRunningProblem)) { openDlg('dlg-running'); return; }
    if (!c.plan || (c.problems || []).length) return;
    const r = await api('/api/install', { game: S.game, selection: S.sel, backup_root: S.backupRoot, backup_kind: S.kind });
    if (r.error) { $('#foot-msg').hidden = false; $('#foot-msg').innerHTML = `<span class="m-bad">${esc(r.error)}</span>`; return; }
    startPolling('install');
  }
  $('#install').addEventListener('click', tryInstall);
  $('#err-retry').addEventListener('click', () => {
    closeDlg('dlg-running');
    if (S.pendingRestore) { S.pendingRestore = false; doRestore(); } else tryInstall();
  });

  // the job's real steps, shown as the design's few big steps; the active one names the real step
  // [label, step ids, optional: shown only when this install runs that step (job.flags, else the selection)]
  const MACRO = {
    install: [['Backing up the game folder', ['preflight', 'music', 'backup']], ['Building data from your game files', ['build', 'prefetch']],
      ['Copying mod files', ['data', 'dlls', 'ini']], ['Building the HD effects from your game', ['hdfx'], 'hdfx'],
      ['Baking the minimap from your game', ['minimap'], 'minimap'], ['Writing settings', ['display', 'difficulty', 'finish']]],
    restore: [['Checking the backup', ['preflight']], ['Restoring game files', ['verify']], ['Removing mod files', ['finish']]],
  };
  const WEIGHT = { preflight: 2, music: 4, backup: 30, build: 40, prefetch: 6, data: 6, dlls: 2, ini: 3, hdfx: 2, minimap: 2, display: 2, difficulty: 2, finish: 7, verify: 90 };
  const WEIGHT_ON = { hdfx: 10, minimap: 45 };   // when the step really runs (the bake is the longest step after the data build)
  const EST = { build: 130, prefetch: 10, hdfx: 20, minimap: 300 };   // seconds, for a smooth bar where the job reports no bytes
  function optionalOn(j, key) {
    if (j.flags && key in j.flags) return !!j.flags[key];
    if (key === 'hdfx') return on('hdfx');
    if (key === 'minimap') return on('hud') && !!S.sel.subs.hud_minimap;
    return false;
  }

  function startPolling(kind) {
    S.stepSeen = {};
    $('#scr-prog-t').textContent = kind === 'install' ? 'INSTALLING' : 'RESTORING';
    $('#log').hidden = true; $('#log-toggle').setAttribute('aria-expanded', 'false');
    show('progress', '#scr-prog-t');
    clearInterval(S.polling);
    if (params.get('shot') !== '1') S.polling = setInterval(poll, 400);   // shot=1: one poll (headless screenshots)
    poll();
  }
  function jobPct(j) {
    let total = 0, done = 0;
    for (const s of j.steps) {
      const w = (WEIGHT_ON[s.id] && optionalOn(j, s.id) ? WEIGHT_ON[s.id] : WEIGHT[s.id]) || 2; total += w;
      if (s.status === 'done' || s.status === 'warn' || s.status === 'skipped') done += w;
      else if (s.status === 'running') {
        let f = 0;
        if (j.progress && j.progress.total) f = j.progress.done / j.progress.total;
        else if (EST[s.id]) { S.stepSeen[s.id] = S.stepSeen[s.id] || Date.now(); f = Math.min(0.95, (Date.now() - S.stepSeen[s.id]) / 1000 / EST[s.id]); }
        done += w * f;
      }
    }
    return j.state === 'done' ? 100 : Math.min(99, Math.floor(done / Math.max(1, total) * 100));
  }
  function renderJob(j) {
    const pct = jobPct(j);
    $('#pct-n').textContent = pct;
    $('#prog-bar').style.width = pct + '%';
    $('#prog-track').setAttribute('aria-valuenow', pct);
    const by = {}; j.steps.forEach((s) => { by[s.id] = s; });
    const running = j.steps.find((s) => s.status === 'running');
    const failed = j.steps.find((s) => s.status === 'failed');
    const maps = running && running.id === 'minimap' && j.progress && j.progress.total ? `: map ${Math.min(j.progress.done + 1, j.progress.total)} of ${j.progress.total}` : '';
    $('#prog-sub').textContent = failed ? failed.detail : running ? running.label + (running.detail ? ': ' + running.detail : maps) : '';
    $('#steps').innerHTML = MACRO[j.kind].filter(([, , opt]) => !opt || optionalOn(j, opt)).map(([label, ids]) => {
      const st = ids.map((i) => by[i]).filter(Boolean);
      const isFailed = st.some((s) => s.status === 'failed');
      const isDone = st.length && st.every((s) => ['done', 'warn', 'skipped'].includes(s.status));
      const isActive = !isFailed && !isDone && st.some((s) => s.status === 'running' || ['done', 'warn', 'skipped'].includes(s.status));
      const cls = isFailed ? 'is-failed' : isDone ? 'is-done' : isActive ? 'is-active' : 'is-todo';
      const sub = '';
      const ico = isFailed ? ICON.x : isDone ? ICON.check.replace('stroke-width="3"', 'stroke-width="3.2"') : isActive ? '<span class="spin"></span>' : '';
      return `<li class="step ${cls}"><span class="step-ico" aria-hidden="true">${ico}</span><span class="step-txt">${esc(label)}${isActive ? '…' : ''}</span>${sub ? `<span class="step-sub">${esc(sub)}</span>` : ''}<span class="sr-only">${isFailed ? 'Failed' : isDone ? 'Done' : isActive ? 'In progress' : 'Waiting'}</span></li>`;
    }).join('');
    const pre = $('#log'), atEnd = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 8;
    pre.textContent = j.log.join('\n');
    if (atEnd) pre.scrollTop = pre.scrollHeight;
  }
  $('#log-toggle').addEventListener('click', () => {
    const l = $('#log'); l.hidden = !l.hidden; $('#log-toggle').setAttribute('aria-expanded', l.hidden ? 'false' : 'true');
  });
  async function poll() {
    const r = await api('/api/job');
    const j = r.job; if (!j) return;
    S.job = j;
    renderJob(j);
    if (j.state !== 'running' && (S.polling || params.get('shot') !== '1')) {
      clearInterval(S.polling); S.polling = null;
      setTimeout(() => showDone(j), j.state === 'done' ? 600 : 900);
    }
  }

  function showDone(j) {
    const r = j.result || {};
    const body = $('#done-body'), title = $('#done-title'), sub = $('#done-sub');
    const scr = $('#scr-done');
    scr.classList.toggle('is-restore', j.kind === 'restore' || j.state !== 'done');
    const meta = `<div class="done-meta"><p class="mini-label">Backup</p><code>${esc(r.backup || '')}</code><p class="mini-label">Log</p><code>${esc(r.log || '')}</code></div>`;
    if (j.kind === 'install' && j.state === 'done') {
      title.textContent = '¡LISTO!'; title.lang = 'es';
      const where = [S.sel.groups.difficulty && 'DIFFICULTY LEVEL', S.sel.groups.display && 'SCREEN RESOLUTION'].filter(Boolean);
      sub.textContent = `Installed in ${Math.round(j.seconds)} s. Start the game from GOG as usual.` + (where.length ? ` In the game: OPTIONS > SETTINGS > ${where.join(' / ')}.` : '');
      const warned = j.steps.filter((s) => s.status === 'warn' && s.id !== 'preflight');
      const chips = S.cat.groups.map((g) => {
        const isOn = (r.summary.on || []).includes(g.title);
        if (!isOn) return '';
        const hot = g.section === 'mayhem';
        const label = g.id === 'more_enemies' ? `${g.title} · ${S.sel.subs.max_live_enemies}` : g.title;
        return `<li class="sum-chip${hot ? ' is-hot' : ''}">${ICON.check}${esc(label)}</li>`;
      }).join('');
      const off = (r.summary.off || []).map((t) => `<li class="sum-chip is-off">${ICON.x}${esc(t)}</li>`).join('');
      body.innerHTML = `<ul class="sum" aria-label="Installed">${chips}${off}</ul>
        ${warned.map((s) => `<p class="warn done-warn">${ICON.warn}<span>${esc(s.label)}: ${esc(s.detail)}</span></p>`).join('')}
        <div class="undo"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="square" aria-hidden="true"><path d="M3.5 12a8.5 8.5 0 1 0 2.6-6.1L3.5 8.5"></path><path d="M3.5 3.5v5h5"></path></svg>
        <strong class="undo-t">UNDO ANYTIME</strong><button type="button" class="btn-ghost" id="done-restore">Restore a backup</button></div>${meta}`;
    } else if (j.kind === 'restore' && j.state === 'done') {
      title.textContent = 'RESTORED'; title.lang = 'en';
      sub.textContent = `The game folder matches the backup again (${Math.round(j.seconds)} s): ${(r.copied || []).length} files copied back, ${r.identical} already identical, ${(r.removed || []).length} removed.`;
      body.innerHTML = `<ul class="done-list">${(r.copied || []).map((f) => `<li>↺ ${esc(f)}</li>`).join('')}${(r.removed || []).map((f) => `<li>✕ ${esc(f)}</li>`).join('')}</ul>${meta}`;
    } else {
      title.textContent = 'STOPPED'; title.lang = 'en';
      sub.textContent = j.error || 'The job stopped.';
      body.innerHTML = `<div class="undo">${ICON.warn}<strong class="undo-t">${r.touched ? 'THE GAME FOLDER WAS PARTLY CHANGED' : 'NOTHING WAS CHANGED'}</strong>${r.touched ? '<button type="button" class="btn-ghost" id="done-restore">Restore a backup</button>' : ''}</div>${meta}`;
    }
    const dr = $('#done-restore'); if (dr) dr.addEventListener('click', () => show('restore', '#scr-rest-t'));
    show('done', '#done-title');
  }
  $('#done-close').addEventListener('click', () => loadState(S.game).then(() => show('main', '#install')));

  // ------------------------------------------------------------------ restore
  async function loadBackups() {
    const root = S.restoreRoot || S.backupRoot;
    $('#rest-root').textContent = root;
    const r = await api('/api/backups?root=' + encodeURIComponent(root));
    S.backups = r.backups || [];
    if (!S.backups.some((b) => b.path === S.pick)) S.pick = S.backups.length ? S.backups[0].path : null;
    renderBackups();
  }
  function renderBackups() {
    const list = $('#bk-list');
    const fmtDate = (iso) => { const d = new Date(iso); return isNaN(d) ? [iso, ''] : [d.toLocaleDateString('en-US', { month: 'short', day: 'numeric', year: 'numeric' }), d.toTimeString().slice(0, 5)]; };
    list.innerHTML = '<legend class="sr-only">Choose a backup</legend>' + (S.backups.length ? S.backups.map((b, i) => {
      const [date, time] = fmtDate(b.created);
      const before = b.selection ? Object.entries(b.selection.groups).filter(([, v]) => v).map(([k]) => titleOf(k)).join(', ') : '';
      const size = b.bytes >= 1073741824 ? (b.bytes / 1073741824).toFixed(1) + ' GB' : (b.bytes / 1048576).toFixed(0) + ' MB';
      return `<label class="bk${S.pick === b.path ? ' is-sel' : ''}"><input type="radio" name="bk-pick" value="${i}"${S.pick === b.path ? ' checked' : ''}><span class="opt-dot"></span>
        <span class="bk-main"><span class="bk-date">${esc(date)}<small>${esc(time)}</small><span class="bk-kind-tag">${b.kind === 'full' ? 'FULL' : 'CHANGED FILES'}${i === S.backups.length - 1 ? ' · OLDEST' : ''}</span></span>
        <small class="bk-before">${before ? 'Before it: ' + esc(before) : 'Made before the first Gameplay+ install'}</small></span>
        <span class="bk-size">${esc(size)} · ${b.files} files</span><span class="bk-ok">${ICON.shield}Verified</span></label>`;
    }).join('') : '<p class="bk-empty">No Gameplay+ backups in this folder yet.</p>');
    $$('input[name="bk-pick"]', list).forEach((r) => r.addEventListener('change', () => { S.pick = S.backups[Number(r.value)].path; renderBackups(); $$('input[name="bk-pick"]', list)[Number(r.value)].focus(); }));
    $('#rest-go').disabled = !S.pick;
  }
  $('#go-restore').addEventListener('click', () => show('restore', '#scr-rest-t'));
  $('#rest-back').addEventListener('click', () => show('main', '#go-restore'));
  $('#rest-browse').addEventListener('click', async () => {
    const r = await api('/api/browse', { title: 'Pick the folder that holds the Gameplay+ backups', start: S.restoreRoot || S.backupRoot });
    if (r.path) { S.restoreRoot = r.path; loadBackups(); }
  });
  $('#rest-go').addEventListener('click', () => {
    if (!S.pick) return;
    const b = S.backups.find((x) => x.path === S.pick);
    $('#cf-d').textContent = `Into ${S.game}: files that differ from the ${b && b.kind === 'full' ? 'full' : 'changed-files'} backup of ${b ? b.created.replace('T', ' ') : ''} are copied back; files the installer added since are removed.`;
    openDlg('dlg-confirm');
  });
  async function doRestore() {
    const r = await api('/api/restore', { game: S.game, backup_dir: S.pick });
    if (r.error) {
      if (/running/i.test(r.error)) { S.pendingRestore = true; openDlg('dlg-running'); }
      return;
    }
    startPolling('restore');
  }
  $('#cf-go').addEventListener('click', async () => {
    closeDlg('dlg-confirm');
    const c = await runCheck();
    if ((c.problems || []).some(isRunningProblem)) { S.pendingRestore = true; openDlg('dlg-running'); return; }
    doRestore();
  });
  $('#all-off').addEventListener('click', () => {
    for (const g of S.cat.groups) S.sel.groups[g.id] = false;
    music(); show('main', '#install'); changed();
  });

  // ------------------------------------------------------------------ boot
  async function loadState(game) {
    const st = await api('/api/state' + (game ? '?game=' + encodeURIComponent(game) : ''));
    const first = !S.cat;
    S.state = st; S.cat = st.catalog; S.game = st.game || ''; S.installed = st.installed;
    S.sel = JSON.parse(JSON.stringify(st.selection));
    S.backupRoot = (st.prefs && st.prefs.last_backup_root && !st.sandbox ? st.prefs.last_backup_root : '') || st.backup_root || '';
    if (st.prefs) { if (typeof st.prefs.muted === 'boolean') S.muted = st.prefs.muted; if (typeof st.prefs.volume === 'number') S.volume = st.prefs.volume; }
    $('#game').value = S.game; $('#game').closest('.path').title = S.game;
    $('#backup-root').value = S.backupRoot; $('#backup-root').closest('.path').title = S.backupRoot;
    if (first) buildCards();
    renderAudio(); renderCredit(st.music); render();
    if (S.sel.subs.suave_song) checkSong('suave_song');
    await runCheck();
    return st;
  }

  S.rm = params.get('rm') === '1';
  setInterval(() => api('/api/ping').catch(() => {}), 5000);
  loadState(params.get('game') || '').then((st) => {
    // view parameters for screenshots / tests (no install is started by them)
    if (params.get('open')) params.get('open').split(',').forEach((id) => S.open.add(id));
    for (const [k, v] of [['on', true], ['off', false]]) if (params.get(k)) params.get(k).split(',').forEach((id) => { if (id in S.sel.groups) S.sel.groups[id] = v; });
    enforce();
    if (params.get('mayhem') !== null) {
      const n = Number(params.get('mayhem'));
      mayhemGroups().forEach((g, i) => { S.sel.groups[g.id] = i < n; });
      enforce();
    }
    if (params.get('scroll') === 'mayhem') setTimeout(() => $('#mayhem').scrollIntoView({ block: 'end' }), 60);
    changed();
    if (st.job && st.job.state === 'running') startPolling(st.job.kind);
    else if (st.job && params.get('screen') === 'done') { renderJob(st.job); showDone(st.job); }
    else if (st.job && params.get('screen') === 'progress') { renderJob(st.job); show('progress'); }
    else if (params.get('screen') === 'restore') show('restore');
    else if (params.get('screen') === 'error') openDlg('dlg-running');
  });
})();
