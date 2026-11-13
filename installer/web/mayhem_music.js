/* mayhem_music.js - the MAYHEM music: synchronised looping STEMS of a licensed track (no synthesis).
 *
 * Files (read only, produced separately): installer\web\music\stem1_guitar.ogg, stem2_bass_perc.ogg, stem3_trumpet.ogg,
 * stem4_strings_choir.ogg and music.json (stem order, BPM, time signature, loop length in samples, credit line). The
 * server lists what exists (GET /api/music). Missing files: those stems stay silent; no stems: the section is silent.
 *
 * Every Mayhem toggle that is on adds one instrument: n toggles on = stems 1..n (up to 3); stem 4 joins when every
 * Mayhem toggle is on, or with the 4th toggle when there are 4 or more. No filters: the layering is the intensity.
 *
 * Playback: all stems are decoded to AudioBuffers and their AudioBufferSourceNodes (loop = true) start at the SAME
 * AudioContext time, so they stay sample-locked. They keep running (muted at gain 0) while any toggle is on; a stem
 * fades in or out on its own GainNode over FADE seconds starting at the next bar line (BPM and beats per bar from
 * music.json). The whole bus fades in over 0.8 s with the first toggle and out over 1.5 s after the last one, then the
 * sources stop. Works on an AudioContext or an OfflineAudioContext (the tests render it offline).
 *
 * Interface used by app.js (the only coupling):
 *   const m = new MayhemMusic(audioCtx, destination); await m.load(); m.setLevel(on, total); m.setVolume(0..1); m.info
 */
(function () {
  'use strict';
  const BUS_IN = 0.8, BUS_OUT = 1.5, FADE = 1.2;
  const DEFAULT_STEMS = ['stem1_guitar.ogg', 'stem2_bass_perc.ogg', 'stem3_trumpet.ogg', 'stem4_strings_choir.ogg'];

  function num(...vals) { for (const v of vals) { const n = Number(v); if (v !== undefined && v !== null && v !== '' && isFinite(n) && n > 0) return n; } return null; }

  class MayhemMusic {
    constructor(ctx, out, opts) {
      this.ctx = ctx;
      this.apiUrl = (opts && opts.api) || '/api/music';
      this.level = 0; this.total = 3;
      this.volume = 0.8;
      this.info = null;
      this.stems = [];             // [{name, buffer, gain, src}]
      this.running = false;
      this.fading = false;
      this.bus = ctx.createGain(); this.bus.gain.value = 0;        // the whole-bus fade
      this.vol = ctx.createGain(); this.vol.gain.value = this.volume;
      this.bus.connect(this.vol); this.vol.connect(out);
    }

    async load() {
      if (this._loading) return this._loading;
      this._loading = (async () => {
        try {
          const info = await (await fetch(this.apiUrl)).json();
          this.info = info;
          if (!info.available) return false;
          const m = info.meta || {};
          this.bpm = num(m.bpm, m.BPM, m.tempo);
          const sig = String(m.time_signature || m.timeSignature || m.meter || '3/4');
          this.beatsPerBar = num(m.beatsPerBar, m.beats_per_bar, parseInt(sig, 10)) || 3;
          const loaded = await Promise.all(info.stems.map(async (s) => {
            if (!s.url) return null;
            try { return { name: s.file, buffer: await this.ctx.decodeAudioData(await (await fetch(s.url)).arrayBuffer()) }; }
            catch (e) { return null; }            // unreadable: that stem stays silent
          }));
          // keep the slot order (stem k = toggle k) even when a file is missing
          this.stems = loaded.map((s, i) => {
            const g = this.ctx.createGain(); g.gain.value = 0; g.connect(this.bus);
            return { name: s ? s.name : info.stems[i].file, buffer: s ? s.buffer : null, gain: g, src: null, on: false };
          });
          const any = this.stems.find((s) => s.buffer);
          if (!any) return false;
          const samples = num(m.loop_length_samples, m.loopLengthSamples, m.loop_samples, m.loopSamples, m.loop_length);
          const rate = num(m.sample_rate, m.sampleRate);
          const d = any.buffer.duration;
          if (samples) {
            let sec = samples / (rate || 48000);
            if (!rate && Math.abs(samples / 44100 - d) < Math.abs(sec - d)) sec = samples / 44100;
            this.loopEnd = Math.min(sec, d);
          } else this.loopEnd = d;
          if (this.level > 0) this._apply(this.ctx.currentTime, true);
          return true;
        } catch (e) {
          this.stems = [];
          return false;
        }
      })();
      return this._loading;
    }

    get barSeconds() { return this.bpm ? 60 / this.bpm * this.beatsPerBar : 0; }

    wanted(i) {                    // stem i (0-based) on for `level` of `total` Mayhem toggles?
      const n = this.level;
      if (n <= 0) return false;
      if (i < 3) return i < n;
      return n >= 4 || n >= this.total;   // the last stem: everything on (or a 4th toggle)
    }

    setVolume(v) {
      this.volume = v;
      const t = this.ctx.currentTime;
      this.vol.gain.cancelScheduledValues(t);
      this.vol.gain.setTargetAtTime(v, t, 0.05);
    }

    setLevel(n, total) {
      this.level = Math.max(0, n | 0);
      if (total) this.total = Math.max(1, total | 0);
      if (!this.stems.some((s) => s.buffer)) return;      // loading (applied when ready) or nothing to play
      this._apply(this.ctx.currentTime, false);
    }

    nextBar(t) {
      const bar = this.barSeconds;
      if (!bar || !this.running) return t;
      const k = Math.ceil((t - this.t0) / bar - 1e-6);
      return this.t0 + Math.max(0, k) * bar;
    }

    _apply(now, first) {
      const ctx = this.ctx;
      if (this.level === 0) { this._fadeOut(now); return; }
      let fresh = false;
      if (!this.running) {
        // start every stem at the same context time: sample-locked from here on
        this.t0 = now + 0.06;
        for (const s of this.stems) {
          if (!s.buffer) continue;
          const src = ctx.createBufferSource();
          src.buffer = s.buffer; src.loop = true; src.loopStart = 0; src.loopEnd = this.loopEnd;
          src.connect(s.gain);
          src.start(this.t0, 0);
          s.src = src;
        }
        this.running = true; fresh = true;
      }
      clearTimeout(this._stopTimer);
      if (fresh || this.fading) {
        this.fading = false;
        const g = this.bus.gain, t = fresh ? this.t0 : now;
        g.cancelScheduledValues(now);
        g.setValueAtTime(fresh ? 0 : g.value, t);
        g.linearRampToValueAtTime(1, t + BUS_IN);
      }
      const at = fresh ? this.t0 : this.nextBar(now);
      this.stems.forEach((s, i) => {
        const want = this.wanted(i);
        if (fresh) { s.gain.gain.setValueAtTime(want ? 1 : 0, at); s.on = want; return; }
        if (want === s.on) return;
        s.on = want;
        const g = s.gain.gain;
        g.cancelScheduledValues(now);
        g.setValueAtTime(g.value, now);
        g.setValueAtTime(g.value, at);                    // hold until the bar line ...
        g.linearRampToValueAtTime(want ? 1 : 0, at + FADE);   // ... then fade
      });
    }

    _fadeOut(now) {
      if (!this.running || this.fading) return;
      this.fading = true;
      const g = this.bus.gain;
      g.cancelScheduledValues(now);
      g.setValueAtTime(g.value, now);
      g.linearRampToValueAtTime(0, now + BUS_OUT);
      const end = now + BUS_OUT + 0.05;
      const done = () => {
        if (!this.fading) return;
        for (const s of this.stems) { if (s.src) { try { s.src.stop(); } catch (e) { /* stopped */ } s.src.disconnect(); } s.src = null; s.on = false; s.gain.gain.cancelScheduledValues(0); s.gain.gain.value = 0; }
        this.running = false; this.fading = false;
      };
      if (typeof OfflineAudioContext !== 'undefined' && this.ctx instanceof OfflineAudioContext) {
        this.ctx.suspend(Math.ceil((end + 0.01) * this.ctx.sampleRate / 128) * 128 / this.ctx.sampleRate)
          .then(() => { done(); this.ctx.resume(); }).catch(() => {});
      } else {
        // a toggle that comes back during the fade cancels this (fading turns false), the sources keep running
        this._stopTimer = setTimeout(() => {
          if (this.fading) done();
        }, (end - this.ctx.currentTime) * 1000 + 30);
      }
    }
  }

  window.MayhemMusic = MayhemMusic;
})();
