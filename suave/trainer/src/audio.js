// Music: the Suave segments (suave/audio) when they exist, else cuts of a local copy of the song read at
// runtime through the dev server (/music/local, path from the git-ignored config.local.json).
// The timeline clock follows the audio clock while music plays, so beats stay on the beat.

export class Music {
  constructor() {
    this.ctx = null
    this.buffers = new Map()     // url -> Promise<AudioBuffer>
    this.segments = []           // [{id,label,url,start,bpm,firstBeat,source}]
    this.current = null
    this.src = null
    this.gain = null
    this.muted = false
    this.volume = 0.8
  }
  ensureCtx() {
    if (!this.ctx) {
      this.ctx = new (window.AudioContext || window.webkitAudioContext)()
      this.gain = this.ctx.createGain(); this.gain.gain.value = this.muted ? 0 : this.volume
      this.gain.connect(this.ctx.destination)
    }
    if (this.ctx.state === 'suspended') this.ctx.resume()
    return this.ctx
  }
  async discover() {
    const segs = []
    // the segments first
    try {
      const r = await fetch('/api/sb-audio').then(r => r.json())
      if (r.found) {
        const meta = {}
        for (const v of Object.values(r.meta || {})) {
          const list = Array.isArray(v) ? v : (v.segments || v.clips || [])
          for (const m of list) { const k = String(m.file || m.path || m.name || '').split(/[\\/]/).pop(); if (k) meta[k.toLowerCase()] = { ...m, ...(v.bpm && !m.bpm ? { bpm: v.bpm } : {}) } }
        }
        for (const f of r.files) {
          const base = f.rel.split('/').pop()
          const m = meta[base.toLowerCase().replace(/\.(ogg|wav|mp3|m4a|flac)$/i, '')] || {}
          segs.push({
            id: 'sb:' + f.rel, label: `Segment ${base.replace(/\.[a-z0-9]+$/i, '')}${m.label ? ': ' + m.label : ''}`, url: '/sb/' + f.rel.split('/').map(encodeURIComponent).join('/'),
            start: +(m.start_in_file ?? m.offset ?? 0) || 0, bpm: +(m.bpm || 124.17), seconds: m.seconds || null, songStart: m.song_start_s ?? null, firstBeat: +(m.first_beat ?? m.firstBeat ?? m.beat_offset ?? 0) || 0,
            source: 'sb', file: f.rel,
          })
        }
      }
    } catch { /* no segments yet */ }
    try {
      const c = await fetch('/api/config').then(r => r.json())
      if (c.music) for (const s of c.music.segments) segs.push({ ...s, id: 'song:' + s.id, url: c.music.url, source: 'local-mp3' })
    } catch { /* offline */ }
    this.segments = segs
    if (!this.current || !segs.find(s => s.id === this.current.id)) this.current = segs[0] || null
    return segs
  }
  select(id) { this.current = this.segments.find(s => s.id === id) || this.current; this.stop() }
  load(url) {
    if (!this.buffers.has(url)) {
      const ctx = this.ensureCtx()
      this.buffers.set(url, fetch(url).then(r => { if (!r.ok) throw new Error('music ' + r.status); return r.arrayBuffer() }).then(b => ctx.decodeAudioData(b)))
    }
    return this.buffers.get(url)
  }
  async prepare() { if (this.current) { try { await this.load(this.current.url) } catch (e) { console.warn('music unavailable', e.message); return false } } return true }
  // start playing at timeline time t with the given speed; returns false if not possible
  async play(t, speed) {
    this.stop()
    if (!this.current || this.muted) return false
    const ctx = this.ensureCtx()
    let buf
    try { buf = await this.load(this.current.url) } catch { return false }
    const src = ctx.createBufferSource()
    src.buffer = buf
    src.playbackRate.value = speed
    src.connect(this.gain)
    // the segment's beat 0 (start) plays at timeline time `zero` (the end of the intro); song cuts
    // can play the bars before it (pre-roll), the segment files start later
    const off = this.current.start + t - (this.zero || 0)
    if (off >= buf.duration) return false
    if (off >= 0) src.start(ctx.currentTime + 0.02, off)
    else src.start(ctx.currentTime + 0.02 - off / speed, 0)
    // gentle fade out at the end of the move
    this.src = src
    this.t0 = t; this.ctx0 = ctx.currentTime + 0.02; this.speed = speed
    return true
  }
  // timeline time from the audio clock (null when not playing)
  clock() {
    if (!this.src || !this.ctx) return null
    return this.t0 + Math.max(0, this.ctx.currentTime - this.ctx0) * this.speed
  }
  setVolume(v) { this.volume = v; if (this.gain) this.gain.gain.value = this.muted ? 0 : v }
  setMuted(m) { this.muted = m; if (this.gain) this.gain.gain.value = m ? 0 : this.volume; if (m) this.stop() }
  // fade in on the first downbeat of the intro, fade out at the end of the move
  fadeTail(t, end = 18) {
    if (!this.gain || this.muted) return
    const audible = Math.max(0, (this.zero || 0) - (this.current?.start || 0))
    const fin = Math.min(1, Math.max(0, (t - audible) / 0.45))
    const fout = Math.min(1, Math.max(0, (end + 0.6 - t) / 1.2))
    this.gain.gain.value = this.volume * fin * fout
  }
  stop() { if (this.src) { try { this.src.stop() } catch { /* already stopped */ } this.src.disconnect(); this.src = null } }
}
