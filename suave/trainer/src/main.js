import * as THREE from 'three'
import { createRenderer, createWorld, followSun } from './scene/world.js'
import { FX } from './scene/fx.js'
import { Character, loadGLB, SkeletonLines } from './engine/character.js'
import { SuaveMove, buildScenario, PHASES, DURATION, T, RADII, MAX_RING, ramRoot, setTiming, TIMING, timeSpeed, SLOWMO } from './move/suave.js'
import { CLIPS, CYCLE_M, PAIRINGS, parseManifest, matchPairing } from './move/clips.js'
import { CameraRig, PRESETS, playCamera } from './camera.js'
import { Music } from './audio.js'

const params = new URLSearchParams(location.search)
const RENDER = params.has('render')
if (RENDER) document.body.classList.add('render')
const $ = id => document.getElementById(id)
const clamp01 = x => Math.min(1, Math.max(0, x))
const ss = (a, b, x) => { const u = clamp01((x - a) / (b - a)); return u * u * (3 - 2 * u) }
const up = new THREE.Vector3(0, 1, 0)

// ---------------------------------------------------------------------------------------------
const canvas = $('view')
const renderer = createRenderer(canvas)
const camera = new THREE.PerspectiveCamera(45, 1, 0.1, 5000)
camera.position.set(7, 3, -9)
const rig = new CameraRig(camera, canvas)
let world, fx, move
const cast = { ram: null, partners: {}, ring: [], shooters: [] }
const skeletons = new Map()
const music = new Music()

const state = {
  t: 0, playing: false, speed: 1, pairing: params.get('pairing') || 'soldier', ringSize: +(params.get('ring') ?? 8),
  shooters: params.get('shooters') !== '0', debug: params.has('debug'), preset: params.get('cam') || 'cuts', radius: +(params.get('radius') || 3), circleSpeed: params.get('circle') || 'auto',
  mode: 'trainer', ready: false, lastFrame: performance.now(), sa: { status: 'waiting', mtime: null, entries: [] },
  timing: params.get('timing') || null, timingTouched: params.has('timing'), useSA: params.get('sa') !== '0',
}

function toast(msg, ms = 2200) {
  const el = $('toast'); el.textContent = msg; el.classList.add('show')
  clearTimeout(toast.h); toast.h = setTimeout(() => el.classList.remove('show'), ms)
}

function resize() {
  const st = $('stage')
  const w = st.clientWidth, h = st.clientHeight
  renderer.setSize(w, h, false)
  camera.aspect = w / h
  camera.updateProjectionMatrix()
}
window.addEventListener('resize', resize)

// ---------------------------------------------------------------------------------------------
// staging: the trainer's default yard
const STAGE_RING = [[40, 9], [85, 11], [130, 8.5], [170, 12.5], [215, 9.5], [255, 12.5], [300, 8], [335, 10.5]]
const RING_KINDS = ['soldier', 'dea', 'civil', 'soldier', 'dea', 'soldier', 'civil', 'dea']
const SHOOTERS = [[15.1, 18.0, 'soldier'], [-18.3, 14.0, 'dea'], [-3.0, 23.6, 'soldier']]
const SHOOTER_KINDS = SHOOTERS.map(s => s[2])
const toward = (from, to) => Math.atan2(to.x - from.x, to.z - from.z)

function beatInfo() {
  const s = music.current
  return s ? { bpm: s.bpm || 124.17, firstBeat: s.firstBeat || 0 } : { bpm: 124.17, firstBeat: 0 }
}

function trainerScenario() {
  const ramPos = new THREE.Vector3(0, 0, -0.31)
  const partner = { kind: state.pairing, pos: new THREE.Vector3(0.0, 0, 3.1), yaw: Math.PI }
  const ring = STAGE_RING.slice(0, state.ringSize).map(([a, d], i) => {
    const r = THREE.MathUtils.degToRad(a)
    const pos = new THREE.Vector3(Math.sin(r) * d, 0, Math.cos(r) * d)
    return { kind: RING_KINDS[i], pos, yaw: toward(pos, ramPos) + (i % 2 ? 0.25 : -0.2) }
  })
  const shooters = state.shooters ? SHOOTERS.map(([x, z, kind]) => ({ kind, pos: new THREE.Vector3(x, 0, z), yaw: 0 })) : []
  const sc = buildScenario({ ramPos, ramYaw: 0, partner, ring, shooters, beat: beatInfo(), pairing: state.pairing })
  return sc
}

function show(ch, on) { move.showChar(ch, on); const sk = skeletons.get(ch); if (sk) sk.lines.visible = on && state.debug }

function saTiming() {
  const d = move.danceSource
  return d ? { release: d.release, pushDur: d.pushDur } : null
}
function applyTiming() {
  setTiming(state.timing || 'design', { bpm: beatInfo().bpm, sa: saTiming() })
  music.zero = T.musicZero
  $('scrub').max = DURATION
  buildTimelineUI(); renderBeats()
  if (state.t > DURATION) state.t = DURATION
}
function applyTrainer() {
  RADII.ring = state.radius
  world.marking.scale.setScalar(Math.max(0.35, (state.radius + 0.75) / 7.3))
  world.marking.position.set(0, 0.01, 0)
  applyTiming()
  const sc = trainerScenario()
  const c = { ram: cast.ram, partner: cast.partners[state.pairing], ring: cast.ring.slice(0, sc.members.length), shooters: cast.shooters.slice(0, sc.shooters.length) }
  for (const [k, p] of Object.entries(cast.partners)) show(p, k === state.pairing)
  cast.ring.forEach((ch, i) => show(ch, i < sc.members.length))
  cast.shooters.forEach((ch, i) => show(ch, i < sc.shooters.length))
  show(cast.ram, true)
  move.setScenario(sc, c)
  drawDebugRings()
}

// ---------------------------------------------------------------------------------------------
// debug overlays
let debugGroup
const ringDefs = [['grab', RADII.grab, '#5ee0ff'], ['ring', RADII.ring, '#ffc247'], ['pull', RADII.pull, '#ff6b5b']]
function circle(r, color, dashed) {
  const pts = []
  for (let i = 0; i <= 128; i++) { const a = i / 128 * Math.PI * 2; pts.push(new THREE.Vector3(Math.sin(a) * r, 0.04, Math.cos(a) * r)) }
  const g = new THREE.BufferGeometry().setFromPoints(pts)
  const m = dashed ? new THREE.LineDashedMaterial({ color, dashSize: 0.5, gapSize: 0.3, transparent: true, opacity: 0.95, depthTest: false })
    : new THREE.LineBasicMaterial({ color, transparent: true, opacity: 0.95, depthTest: false })
  const l = new THREE.Line(g, m); if (dashed) l.computeLineDistances(); l.renderOrder = 11
  return l
}
function drawDebugRings() {
  if (debugGroup) world.scene.remove(debugGroup)
  debugGroup = new THREE.Group()
  const sc = move.sc
  const grab = circle(RADII.grab, '#5ee0ff', true); grab.position.copy(sc.ramStart.pos)
  const ring = circle(RADII.ring, '#ffc247', false); ring.position.copy(sc.O)
  const pull = circle(RADII.pull, '#ff6b5b', true); pull.position.copy(sc.ramStart.pos)
  debugGroup.add(grab, ring, pull)
  // dive lane + slots
  const lane = new THREE.ArrowHelper(sc.escapeDir, sc.O.clone().setY(0.06), 8.5, 0x8ecae6, 0.5, 0.3)
  debugGroup.add(lane)
  for (const m of sc.members) {
    const dot = new THREE.Mesh(new THREE.RingGeometry(0.25, 0.32, 24).rotateX(-Math.PI / 2), new THREE.MeshBasicMaterial({ color: '#ffc247', depthTest: false, transparent: true }))
    dot.position.copy(m.slot).setY(0.05); dot.renderOrder = 11; debugGroup.add(dot)
  }
  debugGroup.visible = state.debug
  world.scene.add(debugGroup)
  debugGroup.userData.labels = [
    ['grab 5 m', sc.ramStart.pos.clone().add(new THREE.Vector3(RADII.grab * 0.7, 0.1, -RADII.grab * 0.7)), '#5ee0ff'],
    [`ring ${RADII.ring} m`, sc.O.clone().add(new THREE.Vector3(-RADII.ring * 0.72, 0.1, -RADII.ring * 0.69)), '#ffc247'],
    ['pull 15 m', sc.ramStart.pos.clone().add(new THREE.Vector3(RADII.pull * 0.7, 0.1, -RADII.pull * 0.71)), '#ff6b5b'],
    ['dive lane', sc.O.clone().addScaledVector(sc.escapeDir, 8.8).setY(0.1), '#8ecae6'],
  ]
}
function updateLabels() {
  const host = $('labels')
  if (!state.debug || !debugGroup) { host.innerHTML = ''; return }
  const st = $('stage'), w = st.clientWidth, h = st.clientHeight
  const labels = debugGroup.userData.labels
  while (host.children.length < labels.length) host.appendChild(document.createElement('div'))
  labels.forEach(([txt, p, col], i) => {
    const v = p.clone().project(camera)
    const el = host.children[i]
    el.textContent = txt; el.style.color = col
    el.style.display = v.z < 1 && Math.abs(v.x) < 1.1 && Math.abs(v.y) < 1.1 ? '' : 'none'
    el.style.left = ((v.x + 1) / 2 * w) + 'px'; el.style.top = ((1 - v.y) / 2 * h) + 'px'
  })
}
function setDebug(on) {
  state.debug = on
  $('debug').checked = on
  if (debugGroup) debugGroup.visible = on
  for (const [ch, sk] of skeletons) sk.lines.visible = on && ch.root.visible
}
function fmtContact(name, d) {
  if (d == null) return `<span class="dim">${name.padEnd(16)}  released</span>`
  const cls = d < 0.06 ? 'good' : d < 0.12 ? 'warn' : 'bad'
  return `${name.padEnd(16)} <span class="${cls}">${(d * 100).toFixed(1).padStart(5)} cm</span>`
}
function updateDebugReadout(info) {
  if (!state.debug) { $('debugOut').innerHTML = '<span class="dim">off</span>'; return }
  const c = move.contact || {}
  const lines = []
  if (c.sa) {
    // the Suave clips: every hand pair; their grips sit 14-19 cm palm to palm (as in the mocap source)
    const name = k => `Ram ${k[0]} > partner ${k[1]}`
    lines.push(`<span class="dim">closest hands (grip = palms &lt; 20 cm)</span>`)
    for (const k of ['LR', 'RL', 'LL', 'RR']) {
      const d = c.sa[k], cls = d < 0.2 ? 'good' : d < 0.3 ? 'warn' : 'dim'
      lines.push(`${name(k).padEnd(17)} <span class="${cls}">${(d * 100).toFixed(1).padStart(5)} cm</span>${c.sa.best.k === k ? ' <' : ''}`)
    }
    lines.push(fmtContact('Ram R > back', c.back))
    const cs = move.sc?.sa?.contactStats
    if (cs) {
      const pct = x => Math.round(x * 100) + '%'
      lines.push(`<span class="dim">whole dance, ${cs.samples} samples:
 closest pair median ${(cs.best.median * 100).toFixed(1)} cm
 gripping (&lt; 20 cm) ${pct(cs.best.under20cm)} of the time</span>`)
    }
  } else {
    lines.push(fmtContact('joined hands', c.joined))
    lines.push(c.planting ? '<span class="warn">Ram hand        planting dynamite</span>' : fmtContact('Ram hand > back', c.back))
    lines.push(fmtContact('hand > shoulder', c.shoulder))
  }
  const R = info?.R, Pr = info?.Pr
  lines.push(`<span class="dim">Ram ${R?.mode || '-'} · partner ${Pr?.mode || '-'}</span>`)
  lines.push(`<span class="dim">ring ${move.sc.members.length} · shooters ${move.sc.shooters.length} · beat ${beatInfo().bpm.toFixed(2)} bpm</span>`)
  $('debugOut').innerHTML = lines.join('\n')
}

// ---------------------------------------------------------------------------------------------
// timeline UI
function buildTimelineUI() {
  const ph = $('phases'); ph.innerHTML = ''
  for (const p of PHASES) {
    const d = document.createElement('div')
    d.style.width = ((p.t1 - p.t0) / DURATION * 100) + '%'
    d.style.background = p.color
    const f = x => (Math.round(x * 100) / 100).toString()
    d.innerHTML = `${p.name}<small>${f(p.t0)}-${f(p.t1)}s</small>`
    d.title = `${p.name} ${f(p.t0)}-${f(p.t1)} s`
    d.dataset.id = p.id
    d.onclick = () => seek(p.t0 + 0.001)
    ph.appendChild(d)
  }
}
function renderBeats() {
  const host = $('beats'); host.innerHTML = ''
  const b = beatInfo(), P = 60 / b.bpm
  for (let k = 0, t = b.firstBeat; t < DURATION; k++, t += P) {
    const i = document.createElement('i'); i.style.left = (t / DURATION * 100) + '%'
    if (k % 4 === 0) i.className = 'bar'
    host.appendChild(i)
  }
}
function phaseAt(t) { return PHASES.find(p => t >= p.t0 && t < p.t1) || PHASES[PHASES.length - 1] }
function updateTimelineUI() {
  const t = state.t
  $('cursor').style.left = (t / DURATION * 100) + '%'
  $('scrub').value = t
  $('tread').textContent = `${t.toFixed(2)} / ${DURATION.toFixed(2)}`
  const p = phaseAt(t)
  $('phaseName').textContent = p.name
  $('phaseTime').textContent = t.toFixed(2) + ' s'
  for (const d of $('phases').children) d.classList.toggle('on', d.dataset.id === p.id)
  $('meterFill').style.width = ((1 - t / DURATION) * 100) + '%'
  $('play').innerHTML = state.playing ? '&#x23F8;' : '&#x25B6;'
}

// title, flash, time-freeze grading
function updateOverlays(t) {
  const active = state.mode === 'trainer' || play.move
  const tt = active ? t : 99
  const fe = T.freezeEnd, te = T.titleEnd
  $('flash').style.opacity = String(tt > 0 && tt < 0.3 ? 0.85 * (1 - ss(0, 0.3, tt)) : 0)
  // time freeze over the first two intro beats, then the dive's slow motion (softer grade)
  const freeze = tt > 0 && tt < fe + 0.45 ? 1 - ss(fe - 0.1, fe + 0.4, tt) : 0
  const slow = active && tt < 90 ? (1 - timeSpeed(tt)) / (1 - SLOWMO) : 0
  $('freeze').style.opacity = String(Math.max(0.8 * freeze, 0.5 * slow))
  const sat = 1 - 0.75 * freeze - 0.18 * slow
  canvas.style.filter = freeze > 0 || slow > 0 ? `saturate(${sat.toFixed(3)}) sepia(${(0.35 * freeze).toFixed(3)}) contrast(${(1 + 0.12 * freeze + 0.08 * slow).toFixed(3)})` : ''
  const title = $('title')
  if (tt > 0 && tt < te + 0.5) {
    const inU = ss(0.04, 0.22, tt)
    const scale = 1.9 - 0.9 * inU + 0.06 * ss(0.22, te, tt)
    title.style.opacity = String(inU * (1 - ss(te, te + 0.4, tt)))
    title.style.transform = `translate(-50%, -50%) scale(${scale.toFixed(3)}) rotate(${(-4 * (1 - inU)).toFixed(2)}deg)`
  } else title.style.opacity = '0'
}

// ---------------------------------------------------------------------------------------------
// playback
async function setPlaying(on) {
  if (on && state.t >= DURATION - 0.01) state.t = 0
  state.playing = on
  if (on) {
    const ok = await music.play(state.t, state.speed)
    state.musicOn = ok
  } else { music.stop(); state.musicOn = false }
  updateTimelineUI()
}
function seek(t) {
  state.t = THREE.MathUtils.clamp(t, 0, DURATION)
  rig.snap = rig.preset !== 'free'
  if (state.playing) music.play(state.t, state.speed)
  updateTimelineUI()
}
function restart() { seek(0); if (!state.playing) setPlaying(true) }

// ---------------------------------------------------------------------------------------------
// clip manifest pickup: poll and switch to the real clips when it appears or changes
async function pollSA() {
  try {
    const r = await fetch('/api/sa-manifest').then(r => r.json())
    if (!r.found) { state.sa.status = 'waiting'; renderClipStatus(); return }
    if (r.mtime === state.sa.mtime) return
    state.sa.mtime = r.mtime
    if (r.error) { state.sa.status = 'manifest unreadable: ' + r.error; renderClipStatus(); return }
    const parsed = parseManifest(r.manifest)
    state.sa.entries = parsed.entries
    state.sa.status = 'loading'
    renderClipStatus()
    state.sa.bpm = r.manifest.bpm || null
    await loadSAClips(parsed.entries)
    state.sa.status = 'loaded'
    if (!state.timingTouched) { state.timing = 'game'; $('timing').value = 'game'; preferMusicFor('game') }
    selectDanceSource()
    if (!play.on) applyTrainer()
    renderClipStatus()
    toast('Suave clips picked up from the manifest')
  } catch (e) { state.sa.status = 'error: ' + e.message; renderClipStatus() }
}
const saClipName = (e, i) => `sa:${e.role || 'clip'}:${e.pairing || e.character || 'all'}:${e.clip || i}`
async function loadSAClips(entries) {
  const all = [cast.ram, ...Object.values(cast.partners), ...cast.ring, ...cast.shooters]
  for (const [i, e] of entries.entries()) {
    if (!state.ready) $('loadingText').textContent = `Loading the Suave clips (${i + 1}/${entries.length}): ${e.file}`
    try {
      const g = await loadGLB('/sa/' + e.file.split(/[\\/]/).map(encodeURIComponent).join('/') + '?v=' + state.sa.mtime)
      let clip = g.animations.find(a => a.name === e.clip) || g.animations[0]
      if (!clip) continue
      clip = clip.clone(); clip.name = saClipName(e, i)
      e.clipName = clip.name
      e.duration = e.duration ?? clip.duration
      for (const ch of all) ch.addClips([clip])
    } catch (err) { e.error = err.message; console.warn('Suave clip failed', e.file, err) }
  }
}
// The ring sways on every beat. The first circle export swayed once per bar, so
// play it 4x unless the manifest says it is per beat (re-export) or a speed is picked.
function circleRate(circle) {
  const base = circle && circle.bpm ? beatInfo().bpm / circle.bpm : 1
  if (state.circleSpeed !== 'auto') return base * +state.circleSpeed
  const raw = circle?.raw || {}
  const perBeat = raw.sway_per_beat === true || raw.sways_per_beat >= 1 || raw.beats_per_sway === 1 || raw.beats_per_sway === 2 || /per[_ ]beat/i.test(JSON.stringify(raw))
  return base * (perBeat ? 1 : 4)
}
function selectDanceSource() {
  const es = state.useSA ? state.sa.entries.filter(e => e.clipName) : []
  const pick = role => es.find(e => e.role === role && matchPairing(e, state.pairing)) || null
  const lead = pick('lead'), follow = pick('follow'), push = pick('push'), stumble = pick('stumble')
  const circle = es.find(e => e.role === 'circle' || e.role === 'clap')
  // the intro clips (role "intro"): suave_intro_<pairing> for Ram, suave_grabbed_<pairing> for the partner
  const introE = es.filter(e => (e.role === 'intro' || e.role === 'grabbed') && matchPairing(e, state.pairing))
  const isGrabbed = e => e.role === 'grabbed' || /grabbed/i.test(e.file) || (e.character && !/ram/.test(e.character))
  const intro = introE.find(e => !isGrabbed(e)), grabbed = introE.find(isGrabbed)
  move.danceSource = lead && follow && push && stumble ? {
    lead: lead.clipName, follow: follow.clipName, push: push.clipName, stumble: stumble.clipName,
    bpm: lead.bpm || state.sa.bpm, release: lead.releaseTime ?? 9.181, pushDur: push.duration ?? 2.166,
    standIn: { push: push.standIn, stumble: stumble.standIn },
    intro: intro && grabbed ? intro.clipName : null, grabbed: intro && grabbed ? grabbed.clipName : null,
  } : null
  move.circleClip = circle ? circle.clipName : null
  move.circleRate = circleRate(circle)
  state.sa.circleStandIn = circle?.standIn
}
function renderClipStatus() {
  const s = state.sa
  const lines = []
  const d = move?.danceSource
  if (d) lines.push(`<span class="good">Suave clips</span> (${state.pairing}): lead + follow,\npush${d.standIn.push ? ' (stand-in)' : ''} + stumble${d.standIn.stumble ? ' (stand-in)' : ''}\nintro: ${d.intro ? 'Suave intro + grabbed' : 'placeholder (hands up, reach, pull)'}`)
  else lines.push(`<span class="warn">Placeholder</span> dance: game clips +\nprocedural merengue + arm IK hold`)
  lines.push(move?.circleClip ? `<span class="good">Suave circle loop</span>${state.sa.circleStandIn ? ' (stand-in)' : ''}` : 'circle: procedural clap')
  lines.push(`<span class="dim">timing: ${TIMING.profile === 'game' ? 'game (segment beat grid)' : 'design doc'}, ${DURATION.toFixed(2)} s</span>`)
  if (s.status === 'waiting') lines.push('<span class="dim">waiting for the clip manifest\nsuave/anim/export/manifest.json\n(checked every 5 s)</span>')
  else lines.push(`<span class="dim">clip manifest: ${s.status}, ${s.entries.length} entries</span>`)
  const errs = s.entries.filter(e => e.error)
  if (errs.length) lines.push(`<span class="bad">${errs.length} Suave clip(s) failed to load</span>`)
  $('clipStatus').innerHTML = lines.join('\n')
}

// ---------------------------------------------------------------------------------------------
// play as Ram
const play = {
  on: false, keys: new Set(), pos: new THREE.Vector3(0, 0, -12), yaw: 0, speed: 0, dist: 0,
  camYaw: 0, camPitch: 0.28, cam: {}, field: [], move: null, idleT: 0, drag: null,
}
const FIELD = [
  [3.2, -7.5, 'soldier', 0], [-4.5, -3.0, 'dea', 1], [5.5, 1.5, 'civil', 0], [-2, 4.5, 'soldier', 1], [8.5, -3.5, 'dea', 0],
  [-8, 6.5, 'soldier', 0], [0.5, 9, 'civil', 1], [10, 8, 'soldier', 0], [-11, -6, 'dea', 1], [-6.5, 12.5, 'soldier', 0],
  [13, -11, 'civil', 0], [15.6, 18.6, 'soldier', 0], [-18.6, 14.5, 'dea', 0], [-2.8, 24, 'soldier', 0],
]
function fieldChars() {
  // every enemy body in the cast takes part in the yard, matched by kind
  const pool = { soldier: [], civil: [], dea: [] }
  for (const [k, ch] of Object.entries(cast.partners)) pool[k].push(ch)
  cast.ring.forEach((ch, i) => pool[RING_KINDS[i]].push(ch))
  cast.shooters.forEach((ch, i) => pool[SHOOTER_KINDS[i]].push(ch))
  return pool
}
function enterPlay() {
  state.mode = 'play'; play.on = true
  document.body.classList.add('play')
  setPlaying(false)
  const pool = fieldChars()
  const used = new Set()
  play.field = FIELD.map(([x, z, kind, patrol], i) => {
    const ch = pool[kind].find(c => !used.has(c)) || Object.values(pool).flat().find(c => !used.has(c))
    used.add(ch)
    const home = new THREE.Vector3(x, 0, z)
    return { ch, kind: ch.kind, home, pos: home.clone(), yaw: toward(home, new THREE.Vector3(0, 0, -12)) + (i % 3 - 1) * 0.6, patrol, alive: true, phase: i * 0.7, travel: 0 }
  })
  for (const ch of Object.values(pool).flat()) show(ch, used.has(ch))
  play.pos.set(0, 0, -16); play.yaw = 0; play.camYaw = 0; play.cam = {}; play.move = null
  state.t = 0
  if (debugGroup) debugGroup.visible = false
  $('playMode').textContent = 'Back to the trainer (Esc)'
  $('playhint').classList.remove('hidden')
  toast('Play as Ram: WASD, Shift to run, drag to look, E for Suave')
}
function exitPlay() {
  play.on = false; play.move = null; state.mode = 'trainer'
  document.body.classList.remove('play')
  $('playMode').textContent = 'Enter the yard'
  $('playhint').classList.add('hidden')
  applyTrainer()
  setDebug(state.debug)
  rig.setPreset(state.preset); rig.snap = true
  seek(0)
}
function nearestEnemy() {
  let best = null, bd = Infinity
  for (const e of play.field) {
    if (!e.alive) continue
    const d = e.pos.distanceTo(play.pos)
    if (d < bd) { bd = d; best = e }
  }
  return best ? { e: best, d: bd } : null
}
function triggerSuave() {
  if (!play.on || play.move) return false
  const n = nearestEnemy()
  if (!n || n.d > RADII.grab) { toast(`No enemy within ${RADII.grab} m`); return false }
  const partner = n.e
  const others = play.field.filter(e => e.alive && e !== partner).map(e => ({ e, d: e.pos.distanceTo(play.pos) })).sort((a, b) => a.d - b.d)
  const ringE = others.filter(o => o.d <= RADII.pull).slice(0, state.ringSize).map(o => o.e)
  const shootE = state.shooters ? others.filter(o => o.d > RADII.pull).slice(0, 3).map(o => o.e) : []
  const pairing = partner.kind
  state.pairing = pairing; $('pairing').value = pairing
  selectDanceSource(); preferMusicFor(state.timing); applyTiming()
  const sc = buildScenario({
    ramPos: play.pos.clone(), ramYaw: play.yaw, pairing, beat: beatInfo(),
    partner: { kind: partner.kind, pos: partner.pos.clone(), yaw: partner.yaw },
    ring: ringE.map(e => ({ kind: e.kind, pos: e.pos.clone(), yaw: e.yaw })),
    shooters: shootE.map(e => ({ kind: e.kind, pos: e.pos.clone(), yaw: e.yaw })),
  })
  move.setScenario(sc, { ram: cast.ram, partner: partner.ch, ring: ringE.map(e => e.ch), shooters: shootE.map(e => e.ch) })
  play.move = { sc, partner, ring: ringE, shooters: shootE, involved: new Set([partner, ...ringE, ...shootE]) }
  rig.setPreset(state.preset === 'free' ? 'gameplay' : state.preset)
  rig.snap = false; rig.pos.copy(camera.position); rig.target.copy(play.cam.target || play.pos); rig.yaw = play.yaw
  state.t = 0
  setPlaying(true)
  toast(`SUAVE on the ${partner.kind} (${n.d.toFixed(1)} m), ring of ${ringE.length}, ${shootE.length} shooting`)
  return true
}
function endPlayMove() {
  const m = play.move
  const R = ramRoot(m.sc, DURATION)
  play.pos.copy(R.pos).setY(0); play.yaw = R.yaw; play.camYaw = R.yaw
  m.partner.alive = false
  m.ring.forEach(e => { e.alive = false })
  play.move = null
  setPlaying(false)
  state.t = 0
  const alive = play.field.filter(e => e.alive).length
  toast(alive ? `${play.field.length - alive} down, ${alive} left. Find the next partner.` : 'Yard cleared! Press R to reset.')
}
function resetYard() {
  for (const e of play.field) { e.alive = true; e.pos.copy(e.home); e.travel = 0 }
  play.pos.set(0, 0, -16); play.yaw = 0; play.camYaw = 0; play.move = null
  setPlaying(false)
}

function poseFieldEnemy(e, dt, frozen) {
  const ch = e.ch
  if (!e.alive) return   // lying where the blast threw them: bones and skin keep their last state
  const tau = play.idleT + e.phase
  let walkW = 0
  if (e.patrol && !frozen) {
    // slow patrol loop around home
    const w = 0.22, r = 2.2
    const a = tau * w
    const p = e.home.clone().add(new THREE.Vector3(Math.sin(a) * r, 0, Math.cos(a) * r - r))
    e.travel += p.distanceTo(e.pos)
    e.yaw = Math.atan2(Math.cos(a), -Math.sin(a))
    e.pos.copy(p); walkW = 1
  }
  const walkS = ch.sampler(CLIPS.walk)
  ch.model.position.y = ch.groundOffset
  ch.evalLayers([
    { clip: CLIPS.idle, time: tau * 0.4 },
    { clip: CLIPS.walk, time: e.travel / CYCLE_M.ram_walk * walkS.duration, weight: walkW },
    { clip: CLIPS.rifleIdle, time: tau * 0.4 },
  ])
  ch.root.position.copy(e.pos); ch.root.rotation.set(0, e.yaw, 0); ch.model.rotation.set(0, 0, 0)
  ch.updateWorld()
  ch.skin()
  move.placeRifle(ch, move.rifleMatrix(ch, new THREE.Matrix4()))
}

function updatePlay(dt) {
  play.idleT += dt
  const ram = cast.ram
  if (play.move) return   // the move drives everything; see frame()
  // input
  const k = play.keys
  const fwdIn = (k.has('KeyW') || k.has('ArrowUp') ? 1 : 0) - (k.has('KeyS') || k.has('ArrowDown') ? 1 : 0)
  const sideIn = (k.has('KeyD') || k.has('ArrowRight') ? 1 : 0) - (k.has('KeyA') || k.has('ArrowLeft') ? 1 : 0)
  const run = k.has('ShiftLeft') || k.has('ShiftRight')
  const cf = new THREE.Vector3(Math.sin(play.camYaw), 0, Math.cos(play.camYaw))
  const cr = new THREE.Vector3(-cf.z, 0, cf.x)
  const dir = cf.multiplyScalar(fwdIn).addScaledVector(cr, sideIn)
  const want = dir.lengthSq() > 0 ? (run ? 4.4 : 1.7) : 0
  play.speed += (want - play.speed) * (1 - Math.exp(-8 * dt))
  if (dir.lengthSq() > 0) {
    dir.normalize()
    const ty = Math.atan2(dir.x, dir.z)
    let d = ty - play.yaw; d = Math.atan2(Math.sin(d), Math.cos(d))
    play.yaw += d * (1 - Math.exp(-10 * dt))
  }
  const step = new THREE.Vector3(Math.sin(play.yaw), 0, Math.cos(play.yaw)).multiplyScalar(play.speed * dt)
  play.pos.add(step)
  play.pos.x = THREE.MathUtils.clamp(play.pos.x, -24, 24); play.pos.z = THREE.MathUtils.clamp(play.pos.z, -28, 27)
  // keep Ram out of standing enemies
  for (const e of play.field) {
    if (!e.alive) continue
    const d = play.pos.clone().sub(e.pos).setY(0); const l = d.length()
    if (l < 0.75 && l > 1e-4) play.pos.addScaledVector(d.normalize(), 0.75 - l)
  }
  play.dist += play.speed * dt
  const ww = clamp01(play.speed / 1.7), rw = clamp01((play.speed - 1.9) / 2.2)
  const walk = ram.sampler(CLIPS.walk), runS = ram.sampler(CLIPS.run)
  ram.model.position.y = ram.groundOffset
  ram.evalLayers([
    { clip: CLIPS.idle, time: play.idleT * 0.4 },
    { clip: CLIPS.walk, time: play.dist / CYCLE_M.ram_walk * walk.duration, weight: ww },
    { clip: CLIPS.run, time: play.dist / CYCLE_M.ram_run * runS.duration, weight: rw },
  ])
  ram.root.position.copy(play.pos); ram.root.rotation.set(0, play.yaw, 0); ram.model.rotation.set(0, 0, 0)
  ram.updateWorld(); ram.skin()
  move.pistols.forEach(p => { p.visible = false })
  fx.begin(); fx.end(camera, $('stage').clientHeight * renderer.getPixelRatio())
  for (const e of play.field) poseFieldEnemy(e, dt, false)
  // hint
  const n = nearestEnemy()
  const hint = $('playhint')
  if (n && n.d <= RADII.grab) hint.innerHTML = `<span style="color:#ffc247">E</span> &nbsp;SUAVE &middot; ${n.e.kind} ${n.d.toFixed(1)} m`
  else hint.textContent = n ? `Nearest enemy ${n.d.toFixed(1)} m (grab within ${RADII.grab} m)` : 'Yard cleared (R to reset)'
  playCamera(camera, play.pos, play.yaw, play.camYaw, play.camPitch, dt, play.cam)
}

// ---------------------------------------------------------------------------------------------
// frame
let lastInfo = null
function evaluateMove(dt) {
  lastInfo = move.evaluate(state.t, camera)
  fx.end(camera, $('stage').clientHeight * renderer.getPixelRatio())
  rig.update(move.sc, state.t, dt, state.t >= T.boom ? state.t - T.boom : null, { ram: move.cast.ram, partner: move.cast.partner })
  followSun(world, move.sc.O)
}
function frame(now) {
  const dt = Math.min(0.1, (now - state.lastFrame) / 1000)
  state.lastFrame = now
  if (!state.ready || RENDER) { requestAnimationFrame(frame); return }
  if (state.playing) {
    const c = state.musicOn ? music.clock() : null
    state.t = c !== null ? c : state.t + dt * state.speed
    if (state.t >= DURATION) {
      state.t = DURATION
      if (play.move) endPlayMove()
      else { state.playing = false; music.stop() }
    }
    music.fadeTail(state.t, DURATION)
  }
  if (play.on && !play.move) {
    updatePlay(dt)
    followSun(world, play.pos)
    updateOverlays(99)
  } else {
    evaluateMove(dt)
    if (play.on) for (const e of play.field) if (!play.move.involved.has(e)) poseFieldEnemy(e, dt, true)
    updateOverlays(state.t)
  }
  for (const [ch, sk] of skeletons) if (sk.lines.visible) sk.update()
  updateTimelineUI()
  updateLabels()
  updateDebugReadout(lastInfo)
  renderer.render(world.scene, camera)
  requestAnimationFrame(frame)
}

// deterministic stepping for the video renderer and the headless checks
function renderAt(t, dt = 1 / 30) {
  state.t = t
  evaluateMove(dt)
  updateOverlays(t)
  for (const [ch, sk] of skeletons) if (sk.lines.visible) sk.update()
  updateTimelineUI(); updateLabels(); updateDebugReadout(lastInfo)
  renderer.render(world.scene, camera)
  return { phase: phaseAt(t).id, ram: lastInfo.R.mode, partner: lastInfo.Pr.mode, contact: move.contact }
}

// ---------------------------------------------------------------------------------------------
// UI wiring
function setPreset(p) {
  state.preset = p
  rig.setPreset(p)
  for (const c of $('cams').children) c.classList.toggle('active', c.dataset.p === p)
}
function wireUI() {
  const cams = $('cams')
  Object.entries(PRESETS).forEach(([k, label], i) => {
    const b = document.createElement('button'); b.className = 'chip'; b.dataset.p = k
    b.innerHTML = `${label.replace(/ \(.*\)/, '')}<kbd>${i + 1}</kbd>`; b.title = label
    b.onclick = () => setPreset(k); cams.appendChild(b)
  })
  $('pairing').value = state.pairing
  $('pairing').onchange = e => { state.pairing = e.target.value; selectDanceSource(); preferMusicFor(state.timing); if (!play.on) applyTrainer(); renderClipStatus() }
  $('radius').value = state.radius; $('radiusVal').textContent = state.radius.toFixed(1) + ' m'
  $('radius').oninput = e => { state.radius = +e.target.value; $('radiusVal').textContent = state.radius.toFixed(1) + ' m'; if (!play.on) applyTrainer() }
  $('circleSpeed').value = state.circleSpeed
  $('circleSpeed').onchange = e => { state.circleSpeed = e.target.value; selectDanceSource(); if (!play.on) applyTrainer() }
  $('ring').value = state.ringSize; $('ringVal').textContent = state.ringSize
  $('ring').oninput = e => { state.ringSize = +e.target.value; $('ringVal').textContent = state.ringSize; if (!play.on) applyTrainer() }
  $('shooters').checked = state.shooters
  $('shooters').onchange = e => { state.shooters = e.target.checked; if (!play.on) applyTrainer() }
  $('mute').onchange = e => { music.setMuted(e.target.checked); if (state.playing && !e.target.checked) music.play(state.t, state.speed).then(ok => state.musicOn = ok); else state.musicOn = false }
  $('debug').checked = state.debug
  $('debug').onchange = e => setDebug(e.target.checked)
  $('music').onchange = e => { state.musicManual = e.target.value !== 'auto'; if (e.target.value === 'auto') { preferMusicFor(state.timing); return } music.select(e.target.value); if (!play.on) applyTrainer(); if (state.playing) music.play(state.t, state.speed).then(ok => state.musicOn = ok) }
  $('timing').value = state.timing || 'design'
  $('timing').onchange = e => { state.timing = e.target.value; state.timingTouched = true; preferMusicFor(state.timing); if (!play.on) applyTrainer(); renderClipStatus() }
  $('useSA').checked = state.useSA
  $('useSA').onchange = e => { state.useSA = e.target.checked; selectDanceSource(); if (!play.on) applyTrainer(); renderClipStatus() }
  $('play').onclick = () => setPlaying(!state.playing)
  $('restart').onclick = restart
  $('speed').onchange = e => { state.speed = +e.target.value; if (state.playing) music.play(state.t, state.speed).then(ok => state.musicOn = ok) }
  const scrub = $('scrub')
  scrub.oninput = e => { if (state.playing) setPlaying(false); seek(+e.target.value) }
  $('playMode').onclick = () => (play.on ? exitPlay() : enterPlay())
  $('recWebm').onclick = recordWebm
  window.addEventListener('keydown', e => {
    if (e.target.tagName === 'SELECT' || e.target.tagName === 'INPUT' && e.target.type !== 'range' && e.target.type !== 'checkbox') return
    if (play.on) {
      play.keys.add(e.code)
      if (e.code === 'KeyE') triggerSuave()
      if (e.code === 'Escape') exitPlay()
      if (e.code === 'KeyR' && !play.move) resetYard()
      if (e.code === 'Space' && play.move) { e.preventDefault(); setPlaying(!state.playing) }
      if (['Space', 'ArrowUp', 'ArrowDown'].includes(e.code)) e.preventDefault()
      return
    }
    if (e.code === 'Space') { e.preventDefault(); setPlaying(!state.playing) }
    else if (e.code === 'KeyR') restart()
    else if (e.code === 'KeyG') setDebug(!state.debug)
    else if (e.code === 'KeyP') enterPlay()
    else if (/^Digit[1-6]$/.test(e.code)) setPreset(Object.keys(PRESETS)[+e.code.slice(5) - 1])
    else if (e.code === 'ArrowRight') { setPlaying(false); seek(state.t + (e.shiftKey ? 1 : 1 / 30)) }
    else if (e.code === 'ArrowLeft') { setPlaying(false); seek(state.t - (e.shiftKey ? 1 : 1 / 30)) }
  })
  window.addEventListener('keyup', e => play.keys.delete(e.code))
  window.addEventListener('blur', () => play.keys.clear())
  canvas.addEventListener('pointerdown', e => { if (play.on && !play.move) { play.drag = { x: e.clientX, y: e.clientY }; canvas.setPointerCapture(e.pointerId) } })
  canvas.addEventListener('pointermove', e => {
    if (!play.drag) return
    play.camYaw -= (e.clientX - play.drag.x) * 0.006
    play.camPitch = THREE.MathUtils.clamp(play.camPitch + (e.clientY - play.drag.y) * 0.004, -0.1, 0.9)
    play.drag = { x: e.clientX, y: e.clientY }
  })
  canvas.addEventListener('pointerup', () => { play.drag = null })
}

// the segments are cut for the game timing (28 beats, boom on the last beat); the longer song cuts
// cover the 18 s design timeline
// music per partner type (design doc): soldier = segment 1, civil = 2, DEA = 3, else 4
const PAIR_SEG = { soldier: 1, civil: 2, dea: 3 }
function preferMusicFor(profile) {
  if (params.get('seg') || state.musicManual) return
  const n = PAIR_SEG[state.pairing] || 4
  const sb = music.segments.filter(s => s.source === 'sb'), song = music.segments.filter(s => s.source === 'local-mp3')
  const pickN = list => list.find(s => { const k = String(s.file || s.id); return k.includes(`_${n}.`) || k.endsWith(`_${n}`) }) || list[n - 1] || list[3] || list[0]
  // the song cuts can play the 6 beats before the segment (the intro fades in on a downbeat);
  // the 28-beat segment files start at the dance, so they come second
  const want = pickN(song) || pickN(sb)
  if (want && want !== music.current) { music.select(want.id); $('music').value = want.id; if (state.playing) music.play(state.t, state.speed).then(ok => state.musicOn = ok) }
}
function fillMusic() {
  const sel = $('music'); sel.innerHTML = ''
  if (!music.segments.length) { const o = document.createElement('option'); o.textContent = 'No music found (see README)'; sel.appendChild(o); sel.disabled = true; return }
  { const o = document.createElement('option'); o.value = 'auto'; o.textContent = 'Auto: per partner (soldier 1, civil 2, DEA 3)'; sel.appendChild(o) }
  for (const s of music.segments) { const o = document.createElement('option'); o.value = s.id; o.textContent = s.label; sel.appendChild(o) }
  sel.disabled = false
  const want = params.get('seg')
  if (want && music.segments.find(s => s.id === want)) music.select(want)
  sel.value = 'auto'
}

// in-app capture of the current camera (WebM, canvas + music; DOM title overlay not included)
let recorder = null
async function recordWebm() {
  if (recorder) { recorder.stop(); return }
  const stream = canvas.captureStream(30)
  try {
    music.ensureCtx()
    const dest = music.ctx.createMediaStreamDestination()
    music.gain.connect(dest)
    dest.stream.getAudioTracks().forEach(tr => stream.addTrack(tr))
  } catch { /* video only */ }
  const chunks = []
  recorder = new MediaRecorder(stream, { mimeType: MediaRecorder.isTypeSupported('video/webm;codecs=vp9') ? 'video/webm;codecs=vp9' : 'video/webm', videoBitsPerSecond: 12e6 })
  recorder.ondataavailable = e => e.data.size && chunks.push(e.data)
  recorder.onstop = () => {
    const a = document.createElement('a')
    a.href = URL.createObjectURL(new Blob(chunks, { type: 'video/webm' }))
    a.download = `suave_${state.preset}_${state.pairing}.webm`; a.click()
    recorder = null; $('recWebm').classList.remove('rec'); $('recWebm').textContent = 'Record this camera (WebM)'
  }
  recorder.start()
  $('recWebm').classList.add('rec'); $('recWebm').textContent = 'Stop recording'
  seek(0); setPlaying(true)
  const stopAt = () => { if (!recorder) return; if (!state.playing && state.t >= DURATION - 0.01) recorder.stop(); else setTimeout(stopAt, 200) }
  setTimeout(stopAt, 500)
}

// ---------------------------------------------------------------------------------------------
async function boot() {
  resize()
  const setLoading = s => { $('loadingText').textContent = s }
  setLoading('Building the yard…')
  world = await createWorld(renderer)
  fx = new FX(world.scene)
  const cfg = await fetch('/api/config').then(r => r.json()).catch(() => ({}))
  if (!cfg.todOk) setLoading('TOD exports not found: set "todOut" in config.local.json')
  setLoading('Loading Ram and the enemies (game meshes)…')
  const files = { ram: 'ram.charanim.glb', soldier: 'soldier.charanim.glb', civil: 'civil.charanim.glb', dea: 'dea_taskforce.charanim.glb' }
  const glbs = {}
  await Promise.all(Object.entries(files).map(async ([k, f]) => { glbs[k] = await loadGLB('/tod/' + f); setLoading(`Loaded ${f}`) }))
  const make = (kind, label) => {
    const ch = new Character(glbs[kind], { kind, label, mirror: true })
    world.scene.add(ch.root); world.scene.add(ch.bakedGroup)
    const sk = new SkeletonLines(ch, kind === 'ram' ? 0xffd34d : 0x66d9ff); sk.lines.visible = false
    world.scene.add(sk.lines); skeletons.set(ch, sk)
    return ch
  }
  cast.ram = make('ram', 'Ram')
  for (const k of Object.keys(PAIRINGS)) cast.partners[k] = make(k, 'partner-' + k)
  RING_KINDS.forEach((k, i) => cast.ring.push(make(k, `ring-${i}`)))
  SHOOTER_KINDS.forEach((k, i) => cast.shooters.push(make(k, `shooter-${i}`)))
  move = new SuaveMove({ ram: cast.ram, partner: cast.partners.soldier, ring: [], shooters: [] }, fx)
  await music.discover()
  fillMusic()
  wireUI()
  if (!state.timing) state.timing = 'design'
  preferMusicFor(state.timing)
  applyTrainer()
  setPreset(state.preset)
  setDebug(state.debug)
  renderClipStatus()
  await pollSA()
  setInterval(pollSA, 5000)
  $('loading').classList.add('hidden')
  state.ready = true
  if (params.has('t')) seek(+params.get('t'))
  window.__trainer.ready = true
  requestAnimationFrame(frame)
}

window.__trainer = {
  ready: false, state, renderAt,
  setPreset, setPlaying, seek, restart,
  setPairing: p => { state.pairing = p; $('pairing').value = p; selectDanceSource(); preferMusicFor(state.timing); applyTrainer() },
  setRadius: r => { state.radius = r; $('radius').value = r; $('radiusVal').textContent = r.toFixed(1) + ' m'; applyTrainer() },
  setRing: n => { state.ringSize = n; $('ring').value = n; $('ringVal').textContent = n; applyTrainer() },
  setShooters: on => { state.shooters = on; $('shooters').checked = on; applyTrainer() },
  setDebug, enterPlay, exitPlay, triggerSuave,
  play: () => ({ on: play.on, pos: play.pos.toArray(), move: !!play.move, alive: play.field.filter(e => e.alive).length, t: state.t }),
  teleportRam: (x, z, yaw = 0) => { play.pos.set(x, 0, z); play.yaw = yaw; play.camYaw = yaw },
  keyDown: code => play.keys.add(code), keyUp: code => play.keys.delete(code),
  info: () => ({ preset: rig.preset, cam: camera.position.toArray(), target: rig.target.toArray(), pairing: state.pairing, partnerKind: move.cast.partner.kind, ring: move.sc.members.length, shooters: move.sc.shooters.length, contact: move.contact, contactStats: move.sc.sa?.contactStats || null, sa: state.sa.status, danceSource: move.danceSource, music: music.current && { id: music.current.id, bpm: music.current.bpm } }),
  music, _move: () => move,
  // the game's facial clips on every body: face-bone tracks found, and the expressions move the face
  faceCheck: () => {
    const out = {}
    const chars = { ram: cast.ram, ...Object.fromEntries(Object.entries(cast.partners)) }
    for (const [k, ch] of Object.entries(chars)) {
      const res = {}
      for (const ex of ['default', 'joy', 'slicksmile', 'surprise', 'disbelief', 'pain', 'anger', 'sceptic']) {
        const smp = ch.sampler('ram_facial_expression_' + ex, true)
        res[ex] = smp ? smp.tracks.length : 0
      }
      // how far the chin and eyelid move between the default face and pain
      const ang = (a, b, bone) => { const i = ch.boneIndex.get(bone); const q1 = new THREE.Quaternion().fromArray(a.q, i * 4), q2 = new THREE.Quaternion().fromArray(b.q, i * 4); return +(q1.angleTo(q2) * 180 / Math.PI).toFixed(1) }
      const p1 = ch.evalLayers([{ clip: 'ram_facial_expression_default', face: true }]); const a = { q: p1.q.slice() }
      const p2 = ch.evalLayers([{ clip: 'ram_facial_expression_pain', face: true }]); const b = { q: p2.q.slice() }
      out[k] = { tracks: res, chinDeg: ang(a, b, 'Chin'), eyelidDeg: ang(a, b, 'Eyelid'), browDeg: ang(a, b, 'EyeBR') }
    }
    return out
  },
  duration: () => DURATION, times: () => ({ ...T }), timing: () => TIMING.profile, phases: () => PHASES.map(p => ({ ...p })),
  setTimingProfile: p => { state.timing = p; state.timingTouched = true; $('timing').value = p; preferMusicFor(p); applyTrainer(); renderClipStatus() },
  setUseSA: on => { state.useSA = on; $('useSA').checked = on; selectDanceSource(); applyTrainer(); renderClipStatus() },
}
if (RENDER) {
  const w = +(params.get('w') || 1280), h = +(params.get('h') || 720)
  $('stage').style.width = w + 'px'; $('stage').style.height = h + 'px'
  renderer.setPixelRatio(1)
}
boot().catch(e => { console.error(e); $('loadingText').textContent = 'Failed: ' + e.message })
