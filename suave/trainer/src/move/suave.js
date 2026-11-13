// The Suave move as a pure function of time: given a scenario (who stands where) and a time t,
// pose every actor and draw every effect.
// Two timing profiles:
//  * "design": the author's design notes (not included) (Activate 0-1, Gather 1-3,
//    Dance 3-13, Plant 13-14, Escape 14-15.5, Boom 15.5-18);
//  * "game": the game code's beat-locked timeline (suave/README.md): music start = dance frame 0, ring walks
//    in on beats 0-4, release at the clips' release frame, push/stumble clip, boom on beat 28.
import * as THREE from 'three'
import { CLIPS, CYCLE_M, ONE_SHOT_RATE } from './clips.js'
import { hash, rand, makeRifle, makePistol } from '../scene/fx.js'

export const RADII = { grab: 5, ring: 3, pull: 15 }   // ring default 3 m; the trainer has a slider
export const MAX_RING = 8
export let DURATION = 18
export const PHASES = []
export const T = {}
export const TIMING = { profile: 'design', bpm: 124.17, sa: null }

// Recompute every phase time. sa = { release, pushDur, clipStart? } when the Suave clips drive the pair.
// Slow motion of the dive (TOD's automatic shoot-dodge slow motion, softened for the preview)
export const SLOWMO = 0.33
const DIVE_AIR_GAME_S = 0.76

export function setTiming(profile, { bpm = 124.17, sa = null } = {}) {
  const B = 60 / bpm
  let I, R, gather0, gatherEnd, dance0, boom = null, clipStart
  if (profile === 'game') {
    // intro: 6 beats before the dance (a longer, clearer activation); the music's beat 0
    // (the segment start = dance frame 0) lands at the end of the intro, boom on its beat 28
    I = 6 * B
    R = I + (sa ? sa.release : 19 * B)
    gather0 = I + 0.1; gatherEnd = I + 4 * B - 0.05; dance0 = I + 4 * B
    boom = I + 28 * B
    clipStart = I
  } else {
    profile = 'design'
    // design doc phases, shifted by the 3 s intro: Activate 0-3, Gather 3-5, Dance 5-15, Plant 15-16
    I = 3.0
    R = 15.0
    gather0 = 3.0; gatherEnd = 4.9; dance0 = 5.0
    clipStart = sa ? R - sa.release : I
  }
  const Bi = I / 6                                   // one intro beat
  const P = sa ? sa.pushDur : 1.0
  const turn0 = R + P
  const A = DIVE_AIR_GAME_S / SLOWMO                 // slow-motion flight, timeline seconds
  if (boom === null) boom = turn0 + 0.44 + 0.7 * A
  const air0 = Math.max(boom - 0.7 * A, turn0 + 0.44)
  const jump = air0 - 0.1, run0 = jump - 0.34, land = air0 + A
  const getUp = land + 0.75
  const end = Math.max(boom + 2.6, land + 1.7)
  Object.assign(T, {
    intro: I, introBeat: Bi, musicZero: I,
    freezeEnd: 2 * Bi, titleEnd: 2.4 * Bi,
    reach0: 2 * Bi, approachEnd: 3.4 * Bi, pull0: 4 * Bi, hold: I,
    gather0, gatherEnd, dance0, release: R, pushDur: P, clipStart,
    // the plant: the lit dynamite goes from Ram's hand into the partner's belt (front)
    dynHand: R - 0.45, dynLit: R - 0.33, dynPlace: R + 0.3,
    spin0: R + 0.42, spin1: R + 0.84, push: R + 0.86,
    turn0, run0, jump, air0, land, boom, getUp, cool: getUp + 0.3, end,
    // the shot list of the ending (the design's shot list)
    shot4a: R - 0.2, shot4b: R + 0.85, shot5: air0 + 0.3, shot6: boom - 0.35,
    lookDown: air0 + 0.25,
  })
  DURATION = T.end
  const ph = [[0, I], [I, dance0], [dance0, R], [R, turn0], [turn0, boom], [boom, end]]
  const meta = [['activate', 'Activate', '#ffd166'], ['gather', 'Gather', '#f4a259'], ['dance', 'Dance', '#e76f51'],
    ['plant', 'Plant', '#d62828'], ['escape', 'Escape', '#8ecae6'], ['boom', 'Boom', '#ffb703']]
  PHASES.length = 0
  ph.forEach(([t0, t1], i) => PHASES.push({ id: meta[i][0], name: meta[i][1], color: meta[i][2], t0, t1 }))
  Object.assign(TIMING, { profile, bpm, sa })
  return TIMING
}

// time speed during the dive slow motion: SLOWMO from the jump, back to normal around the landing
export function timeSpeed(t) {
  if (t < T.air0 - 0.15) return 1
  if (t < T.air0 + 0.1) return 1 + (SLOWMO - 1) * (t - (T.air0 - 0.15)) / 0.25
  if (t < T.land - 0.2) return SLOWMO
  if (t < T.land + 0.45) return SLOWMO + (1 - SLOWMO) * (t - (T.land - 0.2)) / 0.65
  return 1
}
// game seconds elapsed between t0 and t (the explosion and the flung bodies run in game time)
export function gameAge(t0, t) {
  if (t <= t0) return t - t0
  let g = 0
  const n = Math.max(1, Math.ceil((t - t0) / 0.01))
  const h = (t - t0) / n
  for (let i = 0; i < n; i++) g += timeSpeed(t0 + (i + 0.5) * h) * h
  return g
}

setTiming('design')

const up = new THREE.Vector3(0, 1, 0)
const clamp01 = x => Math.min(1, Math.max(0, x))
const ss = (a, b, x) => { const u = clamp01((x - a) / (b - a)); return u * u * (3 - 2 * u) }
const sss = (a, b, x) => { const u = clamp01((x - a) / (b - a)); return u * u * u * (u * (u * 6 - 15) + 10) }
const yawOf = v => Math.atan2(v.x, v.z)
const fwdOf = (yaw, out = new THREE.Vector3()) => out.set(Math.sin(yaw), 0, Math.cos(yaw))
const leftOf = (yaw, out = new THREE.Vector3()) => out.set(Math.cos(yaw), 0, -Math.sin(yaw))
function angDiff(a, b) { let d = (b - a) % (Math.PI * 2); if (d > Math.PI) d -= Math.PI * 2; if (d < -Math.PI) d += Math.PI * 2; return d }
const lerpAng = (a, b, u) => a + angDiff(a, b) * u
const _v = new THREE.Vector3(), _v2 = new THREE.Vector3(), _q = new THREE.Quaternion(), _m = new THREE.Matrix4()

// ----------------------------------------------------------------------------------------------
// Scenario: built either by the trainer (a staged yard) or by play mode (the live positions).
export function buildScenario({ ramPos, ramYaw, partner, ring = [], shooters = [], beat, pairing = 'soldier' }) {
  const dir = new THREE.Vector3().subVectors(partner.pos, ramPos).setY(0)
  if (dir.lengthSq() < 1e-4) fwdOf(ramYaw, dir)
  dir.normalize()
  const theta0 = yawOf(dir)
  const sep = 0.44
  const sc = {
    pairing, ramStart: { pos: ramPos.clone(), yaw: ramYaw }, partnerStart: { ...partner, pos: partner.pos.clone() },
    O: ramPos.clone().addScaledVector(dir, sep / 2), theta0, sep, dir,
    escapeDir: dir.clone().negate(),            // Ram dives back the way he faced
    ring, members: [], shooters: shooters.map((s, i) => ({ ...s, idx: i })),
    beat: beat || { bpm: 124.17, firstBeat: 0 }, spins: [],
    partnerFling: { vy: 10.5, vx: 1.6, spin: 7.5 },
  }
  // partner spins on beats inside the dance, each 2 beats long
  const P = 60 / sc.beat.bpm
  const onBeat = x => sc.beat.firstBeat + Math.round((x - sc.beat.firstBeat) / P) * P
  for (const f of [0.29, 0.58, 0.82]) { const x = onBeat(T.dance0 + (T.release - T.dance0) * f); sc.spins.push({ t0: x, t1: x + 2 * P }) }
  layoutRing(sc)
  return sc
}

// ring slots around O, leaving the dive lane open between slot 0 and slot N-1; then the blast
export function layoutRing(sc) {
  const ring = sc.ring, O = sc.O
  const phiE = Math.atan2(sc.escapeDir.x, sc.escapeDir.z)
  const N = ring.length
  const slots = []
  for (let k = 0; k < N; k++) {
    const a = phiE + Math.PI / N + k * Math.PI * 2 / N
    slots.push(new THREE.Vector3(O.x + Math.sin(a) * RADII.ring, 0, O.z + Math.cos(a) * RADII.ring))
  }
  // assign members to slots greedily by distance (longest walks first)
  const free = slots.map((s, i) => i)
  const order = ring.map((m, i) => i).sort((a, b) => ring[b].pos.distanceTo(O) - ring[a].pos.distanceTo(O))
  const members = new Array(N)
  for (const i of order) {
    let best = 0, bd = Infinity
    free.forEach((s, fi) => { const d = slots[s].distanceTo(ring[i].pos); if (d < bd) { bd = d; best = fi } })
    const slot = slots[free.splice(best, 1)[0]]
    const D = ring[i].pos.distanceTo(slot)
    const depart = T.gather0 + 0.05 * i
    const arrive = Math.min(T.gatherEnd, depart + Math.max(0.9, D / 2.2))
    const speed = D / Math.max(0.3, arrive - depart)
    members[i] = {
      ...ring[i], idx: i, slot, depart, arrive, speed, gait: speed > 2.4 ? CLIPS.run : CLIPS.walk,
      dropT: Math.min(depart - 0.05, T.freezeEnd + 0.09 + 0.045 * i),
      yawSlot: yawOf(_v.subVectors(O, slot)),
      travelYaw: yawOf(_v.subVectors(slot, ring[i].pos)),
    }
  }
  sc.members = members
  sc.E = sc.saE ? sc.saE.clone() : partnerRoot(sc, T.boom - 1e-4).pos.clone()
  sc.members.forEach((m, i) => {
    const away = _v.subVectors(m.slot, sc.E).setY(0)
    const d = Math.max(0.5, away.length())
    away.normalize()
    m.fling = {
      dir: away.clone(), v: 10.5 - d * 0.45 + 2 * rand(i, 1), vy: 6.2 - d * 0.25 + 1.2 * rand(i, 2),
      spin: (rand(i, 3) - 0.5) * 2.5, delay: 0.03 + d * 0.012,
    }
  })
}

// ----------------------------------------------------------------------------------------------
// Pure root plans (also used by the camera and the bullets).
export function pairYaw(sc, t) {
  if (sc.sa) return sc.theta0   // the Suave clips carry their own travel inside the shared root frame
  // one slow full turn of the couple across the dance, so every camera sees both faces
  return sc.theta0 + Math.PI * 2 * sss(T.hold, T.release - 0.1, t)
}

// where Ram's body is (his pelvis inside the shared frame, sampled at scenario setup)
export function ramBody(sc, t) {
  const R = ramRoot(sc, t)
  if (sc.sa && t >= T.intro && t < T.turn0) return sampleTrack(sc.sa.ramTrack, t)
  return R.pos
}
function sampleTrack(track, t) {
  const { t0, dt, pts } = track
  const x = Math.min(pts.length - 1, Math.max(0, (t - t0) / dt))
  const i = Math.floor(x), f = x - i
  const a = pts[i], b = pts[Math.min(pts.length - 1, i + 1)]
  return new THREE.Vector3(a.x + (b.x - a.x) * f, 0, a.z + (b.z - a.z) * f)
}

export function ramRoot(sc, t) {
  const yawPair = pairYaw(sc, t)
  const f = fwdOf(yawPair)
  if (t < T.intro) {
    if (sc.sa?.intro) return { pos: sc.sa.F.clone(), yaw: sc.theta0, pitch: 0, mode: 'sa-intro', y: 0 }
    // the intro: hands up (beats 1-2), step in and take the hand (3-4), pull into the hold (5-6)
    const step = 0.3 * ss(T.reach0, T.approachEnd, t) * (1 - ss(T.pull0, T.intro, t))
    return {
      pos: sc.ramStart.pos.clone().addScaledVector(sc.dir, step), yaw: lerpAng(sc.ramStart.yaw, sc.theta0, ss(0.1, T.introBeat, t)),
      pitch: 0, y: 0, mode: t < T.reach0 ? 'intro-up' : 'grab', travel: step,
    }
  }
  if (sc.sa && t < T.turn0) {
    // shared root frame: both roots at the frame origin, the clips move the bodies
    return { pos: sc.sa.F.clone(), yaw: sc.theta0, pitch: 0, mode: t < T.release ? 'sa-dance' : 'sa-push', y: 0 }
  }
  const hold = sc.O.clone().addScaledVector(f, -sc.sep / 2)
  const res = { pos: hold, yaw: yawPair, pitch: 0, mode: 'dance', y: 0 }
  if (t < T.turn0) return res
  // escape: turn away from the partner, sprint, slow-motion shoot dodge towards the front camera, land, get up
  const e = sc.escapeDir
  const start = sc.sa ? sc.sa.ramEnd.clone() : sc.O.clone().addScaledVector(fwdOf(pairYaw(sc, T.turn0)), -sc.sep / 2)
  const yaw0 = sc.sa ? sc.sa.ramEndYaw : pairYaw(sc, T.turn0)
  const yawE = yawOf(e)
  res.yaw = lerpAng(yaw0, yawE, ss(T.turn0, Math.max(T.turn0 + 0.15, T.run0 + 0.12), t))
  let d = 0
  if (t < T.run0) { res.mode = 'run'; d = 0 }
  else if (t < T.jump) { const u = clamp01((t - T.run0) / (T.jump - T.run0)); d = 1.3 * u * u; res.mode = 'run' }
  else if (t < T.air0) { d = 1.3 + 0.55 * (t - T.jump) / (T.air0 - T.jump); res.mode = 'jump' }
  else if (t < T.land) {
    const u = (t - T.air0) / (T.land - T.air0)
    d = 1.85 + 5.1 * u
    res.y = 0.95 * 4 * u * (1 - u) + 0.15 * (1 - u)
    res.pitch = 0.55 * Math.sin(Math.PI * Math.min(1, u * 1.15))
    res.mode = 'air'
  } else {
    const u = clamp01((t - T.land) / 0.55)
    d = 6.95 + 0.9 * (1 - (1 - u) * (1 - u))
    res.mode = t < T.getUp ? 'land' : (t < T.cool ? 'rise' : 'cool')
    // cool guys don't look at explosions: Ram gets up facing the front camera, the blast behind him
  }
  res.pos = start.addScaledVector(e, d)
  res.travel = d
  return res
}

export function partnerRoot(sc, t) {
  const yawPair = pairYaw(sc, t)
  const f = fwdOf(yawPair)
  const S = sc.partnerStart
  const look = t >= T.lookDown && t < T.boom ? ss(T.lookDown, T.lookDown + 0.5, t) : 0
  if (t < T.intro) {
    if (sc.sa?.intro) {
      const u = sss(T.reach0, T.intro, t)
      return { pos: sc.sa.F.clone().addScaledVector(sc.sa.introOffset, 1 - u), yaw: sc.theta0, mode: 'sa-grabbed', spin: 0, y: 0, pitch: 0, roll: 0, look: 0 }
    }
    const hold = sc.sa ? sc.sa.partnerHold.clone() : sc.O.clone().addScaledVector(f, sc.sep / 2)
    const holdYaw = sc.sa ? sc.sa.partnerYaw0 : yawPair + Math.PI
    const toRam = yawOf(_v.subVectors(sc.ramStart.pos, S.pos))
    const A = sc.ramStart.pos.clone().addScaledVector(_v.subVectors(S.pos, sc.ramStart.pos).setY(0).normalize(), Math.min(1.25, S.pos.distanceTo(sc.ramStart.pos)))
    const res = { pos: S.pos.clone(), yaw: lerpAng(S.yaw, toRam, ss(0.5 * T.introBeat, 2 * T.introBeat, t)), mode: 'idle', spin: 0, y: 0, pitch: 0, roll: 0, look: 0 }
    if (t >= T.reach0) {
      const u = ss(T.reach0, T.approachEnd, t)
      res.pos.lerp(A, u); res.yaw = toRam; res.mode = u < 1 ? 'approach' : 'held'; res.travel = S.pos.distanceTo(A) * u
    }
    if (t >= T.pull0) {
      const u = sss(T.pull0, T.intro, t)
      res.pos = A.clone().lerp(hold, u); res.yaw = lerpAng(toRam, holdYaw, u); res.mode = 'pulled'
      // swing round Ram instead of walking through him when the hold is on his far side
      const ram = sc.ramStart.pos.clone().setY(0)
      const mid = A.clone().lerp(hold, 0.5).setY(0)
      const away = mid.clone().sub(ram)
      if (away.lengthSq() < 1e-4) away.set(-sc.dir.z, 0, sc.dir.x)
      const need = 0.65 - away.length()
      if (need > 0) res.pos.addScaledVector(away.normalize(), need * Math.sin(Math.PI * u) * 1.15)
      res.travel = S.pos.distanceTo(A) + A.distanceTo(hold) * u
    }
    return res
  }
  if (sc.sa && t < T.boom) {
    return { pos: sc.sa.F.clone(), yaw: sc.theta0, mode: t < T.release ? 'sa-dance' : 'sa-stumble', spin: 0, y: 0, pitch: 0, roll: 0, look }
  }
  const hold = sc.O.clone().addScaledVector(f, sc.sep / 2)
  const res = { pos: hold, yaw: yawPair + Math.PI, mode: 'dance', spin: 0, y: 0, pitch: 0, roll: 0, look }
  let fP
  if (!sc.sa) {
    for (const s of sc.spins) if (t > s.t0 && t < s.t1) res.spin = Math.PI * 2 * sss(s.t0, s.t1, t)
    if (t > T.spin0 && t < T.spin1) res.spin = Math.PI * 2 * sss(T.spin0, T.spin1, t)
    if (t < T.push) return res
    // spun away: stumble backwards into the circle, then stand
    fP = fwdOf(pairYaw(sc, T.push))
    const from = sc.O.clone().addScaledVector(fP, sc.sep / 2)
    const tb = Math.min(t, T.boom)
    const u = clamp01((tb - T.push) / 1.35)
    const dist = 2.0 * (1 - Math.pow(1 - u, 2.2))
    res.pos = from.addScaledVector(fP, dist)
    res.yaw = pairYaw(sc, T.push) + Math.PI
    res.mode = u < 1 ? 'stumble' : 'stand'; res.travel = dist
    res.pitch = -0.12 * Math.sin(Math.PI * u)
  } else {
    // after the stumble clip: the body stands where the clip left it
    fP = sc.escapeDir.clone().negate()
    res.pos = sc.sa.partnerEnd.clone(); res.yaw = sc.sa.partnerEndYaw
  }
  if (t >= T.boom) {
    // blown sky-high (in game time: slow at first, back to normal speed with Ram's landing)
    const a = gameAge(T.boom, t)
    const F = sc.partnerFling
    const g = 9.8
    const tLand = 2 * F.vy / g
    res.look = 0
    if (a < tLand) {
      res.y = F.vy * a - 0.5 * g * a * a
      res.pos.addScaledVector(fP, F.vx * a)
      res.pitch = -F.spin * a
      res.mode = 'fly'
    } else {
      res.pos.addScaledVector(fP, F.vx * tLand + 0.6 * clamp01((a - tLand) / 0.3))
      res.pitch = 0; res.mode = 'down'; res.downAge = a - tLand
    }
  }
  return res
}

export function memberRoot(sc, m, t) {
  const res = { pos: m.pos.clone(), yaw: m.yaw, mode: 'idle', y: 0, pitch: 0, roll: 0, travel: 0 }
  if (t < m.depart) {
    if (t > m.depart - 0.25) res.yaw = lerpAng(m.yaw, m.travelYaw, ss(m.depart - 0.25, m.depart, t))
    res.mode = t < m.dropT ? 'idle' : 'alert'
    return res
  }
  if (t < m.arrive) {
    const u = ss(m.depart, m.arrive, t)
    res.pos.lerp(m.slot, u)
    res.travel = m.pos.distanceTo(m.slot) * u
    res.yaw = m.travelYaw
    res.mode = 'walk'
    return res
  }
  res.pos = m.slot.clone()
  res.yaw = lerpAng(m.travelYaw, m.yawSlot, ss(m.arrive, m.arrive + 0.4, t))
  res.mode = 'clap'
  const tb = T.boom + m.fling.delay
  if (t >= tb) {
    const a = gameAge(tb, t), F = m.fling, g = 9.8
    const tLand = 2 * F.vy / g
    res.yaw = m.yawSlot
    if (a < tLand) {
      res.pos.addScaledVector(F.dir, F.v * a)
      res.y = F.vy * a - 0.5 * g * a * a
      res.pitch = -0.9 * Math.min(1, a * 2) - 0.6 * a
      res.roll = F.spin * a * 0.4
      res.mode = 'fly'; res.flyAge = a
    } else {
      const s = clamp01((a - tLand) / 0.4)
      res.pos.addScaledVector(F.dir, F.v * tLand + 1.2 * (1 - (1 - s) * (1 - s)))
      res.mode = 'down'; res.downAge = a - tLand
    }
  }
  return res
}

// palm-centre distances between every hand of a and every hand of b (L/R = the characters' own sides)
export function handPairs(a, b, pa = new THREE.Vector3(), pb = new THREE.Vector3()) {
  const r = {}
  let best = { k: null, d: Infinity }
  for (const [ka, sa] of [['L', 'left'], ['R', 'right']]) for (const [kb, sb] of [['L', 'left'], ['R', 'right']]) {
    const d = a.palm(sa, pa).distanceTo(b.palm(sb, pb))
    r[ka + kb] = d
    if (d < best.d) best = { k: ka + kb, d }
  }
  r.best = best
  return r
}

export function shooterPlan(sc, s, t) {
  return { pos: s.pos, yaw: yawOf(_v.subVectors(ramBody(sc, t), s.pos)) }
}

// Bullet schedule for the outside shooters: [{shooter, fire, hit, seed}]
export function bulletSchedule(sc) {
  const out = []
  const first = T.gather0 + 0.75
  sc.shooters.forEach((s, i) => {
    for (let b = 0; ; b++) {
      const t0 = first + i * 0.47 + b * (1.55 + 0.3 * rand(i, b))
      if (!(t0 < T.boom - 0.4)) break
      for (let k = 0; k < 4; k++) out.push({ s: i, fire: t0 + k * 0.11, seed: i * 1000 + b * 10 + k })
    }
  })
  return out
}

// ----------------------------------------------------------------------------------------------
// Faces: the game's own facial clips (ram_facial_expression_*, shared by every body), layered on
// the face bones only. keys = [[time, expression], ...]; 0.2 s cross-fade at each key.
const FACE = n => 'ram_facial_expression_' + n
function faceLayers(t, keys) {
  let prev = keys[0][1], cur = keys[0][1], t0 = -9
  for (const [kt, n] of keys) { if (t < kt) break; if (n !== cur) { prev = cur; cur = n; t0 = kt } }
  return [{ clip: FACE(prev), face: true, time: 0, loop: false }, { clip: FACE(cur), face: true, time: 0, loop: false, weight: ss(t0, t0 + 0.2, t) }]
}
export function ramFaceKeys(sc) {
  const B = 60 / sc.beat.bpm
  const k = [[0, 'slicksmile']]
  // gather / dance: joy and slicksmile alternating every 2 bars
  for (let x = T.intro, i = 0; x < T.release - 0.4; x += 8 * B, i++) k.push([x, i % 2 ? 'slicksmile' : 'joy'])
  k.push([T.release - 0.4, 'slicksmile'], [T.land + 0.25, 'joy'], [T.cool, 'slicksmile'])
  return k
}
export function partnerFaceKeys(sc) {
  return [[0, 'default'], [0.3, 'surprise'], [T.reach0, 'disbelief'], [(T.dance0 + T.release) / 2, 'joy'],
    [T.lookDown, 'surprise'], [T.lookDown + 0.45, 'pain'], [T.boom + 0.6, 'pain']]
}
export function memberFaceKeys(m) {
  return [[0, 'default'], [Math.max(T.freezeEnd, m.depart - 0.3), 'surprise'], [m.arrive, 'joy'],
    [T.lookDown + 0.1 + m.idx * 0.04, 'surprise'], [T.boom + m.fling.delay, 'pain']]
}

// ----------------------------------------------------------------------------------------------
// The evaluator: owns the cast and props, poses everything for a given time.
export class SuaveMove {
  constructor(cast, fx) {
    this.cast = cast        // { ram, partners: {soldier, civil, dea}, ring: [...], shooters: [...] }
    this.fx = fx
    this.sc = null
    this.danceSource = null // set when the Suave clips are available for the active pairing
    this.circleClip = null
    this.useShooters = true
    this.rifles = new Map()
    this.pistols = [makePistol(), makePistol()]
    this.pistols.forEach(p => { p.visible = false; fx.scene.add(p) })
    this.contact = null
  }

  rifleFor(ch) {
    let r = this.rifles.get(ch)
    if (!r) { r = makeRifle(); r.matrixAutoUpdate = false; this.fx.scene.add(r); this.rifles.set(ch, r) }
    return r
  }

  get partner() { return this.cast.partner }

  showChar(ch, on) {
    ch.root.visible = on; ch.bakedGroup.visible = on
    const r = this.rifles.get(ch); if (r) r.visible = on
  }

  // cast = { ram, partner, ring: [chars matching sc.members], shooters: [chars matching sc.shooters] }
  setScenario(sc, cast) {
    this.sc = sc
    this.cast = cast
    if (this.danceSource) this.prepareSA(sc)
    this.bullets = bulletSchedule(sc)
    // gun drop start transforms: pose each holder at its drop time once
    this.dropMats = new Map()
    this.partnerDropT = T.reach0 + 0.15
    this.faces = { ram: ramFaceKeys(sc), partner: partnerFaceKeys(sc) }
    sc.members.forEach(m => { m.faceKeys = memberFaceKeys(m) })
    const holders = [[this.partner, null, this.partnerDropT], ...sc.members.map((m, i) => [this.cast.ring[i], m, m.dropT])]
    for (const [ch, m, td] of holders) {
      if (m) this.poseMember(ch, m, td, true)
      else this.posePartner(ch, td, true)
      this.dropMats.set(ch, this.rifleMatrix(ch, new THREE.Matrix4()))
    }
  }

  // the Suave pair clips share one root frame. Find the frame origin that keeps Ram where he stands,
  // sample where the bodies go, and lay the ring out around the real dance.
  prepareSA(sc) {
    const src = this.danceSource
    const ram = this.cast.ram, P = this.cast.partner
    sc.sa = {
      lead: src.lead, follow: src.follow, push: src.push, stumble: src.stumble, intro: src.intro || null, grabbed: src.grabbed || null,
      rate: src.bpm ? sc.beat.bpm / src.bpm : 1, F: new THREE.Vector3(),
      pushLen: (ram.sampler(src.push)?.duration || 2.166) - 1e-3,
      stumbleLen: (P.sampler(src.stumble)?.duration || 2.166) - 1e-3,
    }
    const poseAt = (ch, clip, time, loop) => {
      ch.evalLayers([{ clip, time, loop }])
      ch.root.position.copy(sc.sa.F); ch.root.rotation.set(0, sc.theta0, 0); ch.model.rotation.set(0, 0, 0)
      ch.model.position.y = ch.groundOffset
      ch.updateWorld()
    }
    // frame origin: Ram's pelvis at dance frame 0 = where Ram stands
    poseAt(ram, src.lead, this.saTime(0), true)
    sc.sa.F.copy(sc.ramStart.pos).sub(ram.pelvisXZ())
    // Ram's body over the dance and the push (camera, bullets)
    const pts = []
    for (let t = 0; t <= T.turn0 + 1e-6; t += 0.1) {
      if (t < T.intro) { pts.push(sc.ramStart.pos.clone().setY(0)); continue }
      if (t < T.release) poseAt(ram, src.lead, this.saTime(t), true)
      else poseAt(ram, src.push, Math.min(t - T.release, sc.sa.pushLen), false)
      pts.push(ram.pelvisXZ())
    }
    sc.sa.ramTrack = { t0: 0, dt: 0.1, pts }
    poseAt(ram, src.lead, this.saTime(0), true)
    const ram0 = ram.pelvisXZ()
    poseAt(ram, src.push, sc.sa.pushLen, false)
    sc.sa.ramEnd = ram.pelvisXZ()
    sc.sa.ramEndYaw = yawOf(ram.bodyForward())
    poseAt(P, src.follow, this.saTime(0), true)
    const p0 = P.pelvisXZ()
    sc.sa.partnerHold = p0.clone()
    sc.sa.partnerYaw0 = yawOf(P.bodyForward())
    sc.sa.introOffset = new THREE.Vector3()
    if (src.intro && src.grabbed) {
      // the intro clips end on the dance's first frame; slide the partner from where they stood
      poseAt(P, src.grabbed, 0, false)
      sc.sa.introOffset = sc.partnerStart.pos.clone().setY(0).sub(P.pelvisXZ())
    }
    poseAt(P, src.stumble, sc.sa.stumbleLen, false)
    sc.sa.partnerEnd = P.pelvisXZ()
    sc.sa.partnerEndYaw = yawOf(P.bodyForward())
    sc.saE = sc.sa.partnerEnd.clone()
    // hand contact over the whole dance (Suave clips): every Ram palm against every partner palm. The clips' own
    // grip metric: palm centres 14-19 cm apart while gripping (as in the mocap source)
    const series = []
    const pa = new THREE.Vector3(), pb = new THREE.Vector3()
    for (let t = T.intro; t < T.release; t += 0.1) {
      poseAt(ram, src.lead, this.saTime(t), true); poseAt(P, src.follow, this.saTime(t), true)
      series.push(handPairs(ram, P, pa, pb))
    }
    const summarize = a => { const s = [...a].sort((x, y) => x - y); return { median: s[Math.floor(s.length / 2)], min: s[0], max: s[s.length - 1], under20cm: s.filter(x => x < 0.2).length / s.length, under10cm: s.filter(x => x < 0.1).length / s.length } }
    sc.sa.contactStats = { best: summarize(series.map(x => x.best.d)), samples: series.length,
      pairs: Object.fromEntries(['LL', 'LR', 'RL', 'RR'].map(k => [k, summarize(series.map(x => x[k]))])) }
    // ring centred on the couple, dive lane pointing away from where the partner ends up
    sc.O = ram0.clone().add(p0).multiplyScalar(0.5)
    const away = sc.sa.ramEnd.clone().sub(sc.sa.partnerEnd).setY(0)
    if (away.lengthSq() > 1e-4) sc.escapeDir = away.normalize()
    layoutRing(sc)
  }

  // ---- helpers --------------------------------------------------------------------------
  beatPos(t) { const b = this.sc.beat; return (t - b.firstBeat) * b.bpm / 60 }

  place(ch, root, extraPitch = 0) {
    ch.root.position.set(root.pos.x, root.y || 0, root.pos.z)
    ch.root.rotation.set(0, root.yaw + (root.spin || 0), 0)
    ch.model.rotation.set((root.pitch || 0) + extraPitch, 0, root.roll || 0)
    ch.updateWorld()
  }

  locoTime(ch, clip, travel) {
    const s = ch.sampler(clip)
    return (travel / (CYCLE_M[clip] || 1.5)) * (s ? s.duration : 0.5)
  }

  // rifle between the hands: grip at the rear palm, barrel through the front palm
  rifleMatrix(ch, out) {
    const a = ch.palm('right', new THREE.Vector3()), b = ch.palm('left', new THREE.Vector3())
    const chest = ch.spine3.getWorldPosition(new THREE.Vector3())
    const fwd = ch.forward(new THREE.Vector3())
    const da = a.clone().sub(chest).dot(fwd), db = b.clone().sub(chest).dot(fwd)
    const rear = da < db ? a : b, front = da < db ? b : a
    const z = front.clone().sub(rear)
    if (z.lengthSq() < 0.01) z.copy(fwd)
    z.normalize()
    const x = new THREE.Vector3().crossVectors(up, z).normalize()
    const y = new THREE.Vector3().crossVectors(z, x)
    out.makeBasis(x, y, z).setPosition(rear.addScaledVector(y, -0.03).addScaledVector(z, -0.06))
    return out
  }

  dropMatrix(ch, t0, t, seed, out) {
    const M0 = this.dropMats.get(ch)
    if (!M0) return null
    const p0 = new THREE.Vector3(), q0 = new THREE.Quaternion(), s0 = new THREE.Vector3()
    M0.decompose(p0, q0, s0)
    const a = Math.max(0, t - t0)
    const tFall = Math.sqrt(2 * Math.max(0.05, p0.y - 0.05) / 9.8)
    const u = Math.min(1, a / tFall)
    const p = p0.clone()
    p.y = Math.max(0.05, p0.y - 4.9 * a * a)
    if (a > tFall) { const b = a - tFall; p.y = 0.05 + Math.max(0, 0.9 * b - 4.9 * b * b) * (b < 0.2 ? 1 : 0) }
    const fwd = new THREE.Vector3(0, 0, 1).applyQuaternion(q0).setY(0).normalize()
    const yaw = Math.atan2(fwd.x, fwd.z) + (rand(seed, 1) - 0.5) * 1.2
    const lying = new THREE.Quaternion().setFromEuler(new THREE.Euler(0, yaw, Math.PI / 2, 'YXZ'))
    const q = q0.clone().slerp(lying, sss(0, 1, u * 1.05))
    return out.compose(p, q, new THREE.Vector3(1, 1, 1))
  }

  // ---- actors ---------------------------------------------------------------------------
  // Placeholder merengue: a marching step on every beat (walk cycle at low weight, two beats per
  // cycle), hip roll and torso counter-sway. phase shifts mirror the partner's feet.
  danceLayers(ch, t, amp, phase = 0) {
    const bp = this.beatPos(t) + phase
    const walk = ch.sampler(CLIPS.walk)
    return [
      { clip: CLIPS.idle, time: t * 0.4 },
      { clip: CLIPS.walk, time: ((bp / 2) % 1 + 1) % 1 * walk.duration, weight: 0.42 * amp },
    ]
  }
  danceSway(ch, t, amp, phase = 0) {
    const bp = this.beatPos(t) + phase
    const fwd = ch.forward(new THREE.Vector3())
    const s = Math.sin(Math.PI * bp)
    // knees dip on the beat
    ch.model.position.y = ch.groundOffset - 0.03 * amp * Math.pow(Math.abs(Math.cos(Math.PI * bp)), 4)
    ch.updateWorld()
    ch.rotateWorld(ch.pelvis, fwd, 0.11 * amp * s)
    ch.rotateWorld(ch.pelvis, up, 0.10 * amp * Math.sin(Math.PI * bp + 0.6))
    ch.rotateWorld(ch.spine2, fwd, -0.13 * amp * s)
    ch.rotateWorld(ch.neck, fwd, -0.02 * amp * s)
  }

  poseRam(t) {
    const sc = this.sc, ram = this.cast.ram, P = this.partner
    const R = ramRoot(sc, t)
    const yawPair = pairYaw(sc, t)
    const f = fwdOf(yawPair), left = leftOf(yawPair)
    let layers = [], amp = 0, afterPlace = null
    ram.model.position.y = ram.groundOffset
    if (R.mode === 'sa-intro') {
      layers = [{ clip: sc.sa.intro, time: t * sc.sa.rate, loop: false }]
    } else if (R.mode === 'sa-dance') {
      layers = [{ clip: sc.sa.lead, time: this.saTime(t), loop: true }]
    } else if (R.mode === 'sa-push') {
      layers = [{ clip: sc.sa.push, time: Math.min(t - T.release, sc.sa.pushLen), loop: false }]
    } else if (R.mode === 'intro-up' || R.mode === 'grab') {
      // hips start moving with the music; a small step in while reaching
      amp = 0.35 * ss(0.3, T.reach0, t)
      layers = this.danceLayers(ram, t, amp, 0)
      // the Suave dance takes over on its first frame
      if (sc.sa) layers.push({ clip: sc.sa.lead, time: 0, loop: false, weight: ss(T.intro - 0.35, T.intro, t) })
    } else if (R.mode === 'dance') {
      { amp = 0.55 + 0.45 * ss(T.hold, T.dance0, t); layers = this.danceLayers(ram, t, amp, 0) }
    } else if (R.mode === 'run') {
      layers = [{ clip: CLIPS.idle, time: 0 }, { clip: CLIPS.run, time: this.locoTime(ram, CLIPS.run, (R.travel || 0) + 0.3), weight: ss(T.turn0, T.run0 + 0.1, t) }]
    } else if (R.mode === 'jump') {
      const u = (t - T.jump) / (T.air0 - T.jump)
      layers = [{ clip: CLIPS.diveJump, time: u * 0.1, loop: false }, { clip: CLIPS.diveJumpGun, time: u * 0.1, loop: false }]
    } else if (R.mode === 'air') {
      const u = (t - T.air0) / (T.land - T.air0)
      const tc = Math.min(0.26, u * 0.36)
      layers = [{ clip: CLIPS.diveAir, time: tc, loop: false }, { clip: CLIPS.diveAirGun, time: Math.min(0.12, tc), loop: false }]
    } else if (R.mode === 'land' || R.mode === 'rise' || R.mode === 'cool') {
      const tl = Math.min(0.466, (t - T.land) * 0.62)
      layers = [{ clip: CLIPS.diveLand, time: tl, loop: false }, { clip: CLIPS.diveLandGun, time: tl, loop: false, weight: 1 - ss(T.getUp, T.cool, t) }]
      if (t > T.getUp) {
        layers.push({ clip: CLIPS.idle, time: t * 0.4, weight: ss(T.getUp, T.cool, t) })
        if (t > T.cool) layers.push({ clip: CLIPS.cool, time: Math.min(0.76, (t - T.cool - 0.35) * ONE_SHOT_RATE * 1.3), loop: false, weight: ss(T.cool + 0.2, T.cool + 0.6, t) })
      }
    }
    ram.evalLayers(layers.concat(faceLayers(t, this.faces.ram)))
    this.place(ram, R)
    if (amp > 0) this.danceSway(ram, t, amp, 0)
    if (R.mode === 'intro-up') this.handsUp(ram, t, ss(0.15, 0.6 * T.reach0, t) * (1 - ss(T.reach0 - 0.15, T.reach0 + 0.1, t)))
    if (R.mode === 'run') ram.model.position.y = ram.groundOffset
    return { R, f, left, yawPair, amp }
  }

  posePartner(ch, t, poseOnly = false) {
    const sc = this.sc
    const Pr = partnerRoot(sc, t)
    let layers
    const frozenT = t < T.freezeEnd ? 0 : t - T.freezeEnd
    ch.model.position.y = ch.groundOffset
    let amp = 0
    if (Pr.mode === 'sa-grabbed') layers = [{ clip: sc.sa.grabbed, time: t * sc.sa.rate, loop: false }]
    else if (Pr.mode === 'sa-dance') layers = [{ clip: sc.sa.follow, time: this.saTime(t), loop: true }]
    else if (Pr.mode === 'sa-stumble') layers = [{ clip: sc.sa.stumble, time: Math.min(t - T.release, sc.sa.stumbleLen), loop: false }]
    else if (Pr.mode === 'idle') layers = [{ clip: CLIPS.idle, time: frozenT * 0.4 }, { clip: CLIPS.rifleIdle, time: frozenT * 0.4, weight: 1 - ss(this.partnerDropT, this.partnerDropT + 0.25, t) }]
    else if (Pr.mode === 'approach') layers = [{ clip: CLIPS.idle, time: 0 }, { clip: CLIPS.walk, time: this.locoTime(ch, CLIPS.walk, Pr.travel), weight: 0.8 }]
    else if (Pr.mode === 'held' || Pr.mode === 'stand') layers = [{ clip: CLIPS.idle, time: t * 0.4 }]
    else if (Pr.mode === 'pulled') {
      layers = [{ clip: CLIPS.idle, time: 0 }, { clip: CLIPS.walk, time: this.locoTime(ch, CLIPS.walk, Pr.travel), weight: 0.6 }]
      if (sc.sa) layers.push({ clip: sc.sa.follow, time: 0, loop: false, weight: ss(T.intro - 0.35, T.intro, t) })
    }
    else if (Pr.mode === 'dance') {
      { amp = 0.55 + 0.45 * ss(T.hold, T.dance0, t); layers = this.danceLayers(ch, t, amp, 1) }
    } else if (Pr.mode === 'stumble') {
      const sb = this.saStumble
      if (sb) layers = [{ clip: sb, time: (t - T.push), loop: false }]
      else layers = [{ clip: CLIPS.idle, time: 0 }, { clip: CLIPS.runBack, time: this.locoTime(ch, CLIPS.runBack, Pr.travel), weight: 0.8 }]
    } else if (Pr.mode === 'fly') layers = [{ clip: CLIPS.flyBackX, time: 0, loop: false }]
    else layers = [{ clip: CLIPS.landBack, time: Math.min(0.26, (Pr.downAge || 0) * 0.5), loop: false }]
    ch.evalLayers(layers.concat(faceLayers(t, this.faces.partner)))
    this.place(ch, Pr)
    if (amp > 0) this.danceSway(ch, t, amp * (Pr.spin ? 0.4 : 1), 1)
    if (Pr.look > 0 && !poseOnly) this.lookDown(ch, Pr.look)
    return Pr
  }

  poseMember(ch, m, t, poseOnly = false) {
    const sc = this.sc
    const M = memberRoot(sc, m, t)
    const tau = Math.max(0, t - T.freezeEnd) + m.idx * 0.37
    let layers = [], clap = 0
    ch.model.position.y = ch.groundOffset
    const rifleW = 1 - ss(m.dropT, m.dropT + 0.25, t)
    if (M.mode === 'idle' || M.mode === 'alert') {
      layers = [{ clip: CLIPS.idle, time: tau * 0.4 }, { clip: CLIPS.rifleIdle, time: tau * 0.4, weight: rifleW }]
    } else if (M.mode === 'walk') {
      layers = [{ clip: CLIPS.idle, time: 0 }, { clip: m.gait, time: this.locoTime(ch, m.gait, M.travel), weight: ss(m.depart, m.depart + 0.15, t) * (1 - ss(m.arrive - 0.12, m.arrive, t)) }]
    } else if (M.mode === 'clap') {
      const circle = this.circleClip
      if (circle) layers = [{ clip: CLIPS.idle, time: 0 }, { clip: circle, time: this.circleTime(t, m.idx), loop: true, weight: ss(m.arrive - 0.1, m.arrive + 0.3, t) }]
      else {
        const bp = this.beatPos(t)
        const walk = ch.sampler(CLIPS.walk)
        layers = [{ clip: CLIPS.idle, time: tau * 0.4 }, { clip: CLIPS.walk, time: ((bp / 2 + m.idx * 0.5) % 1 + 1) % 1 * walk.duration, weight: 0.22 * ss(m.arrive + 0.2, T.dance0, t) }]
        clap = ss(m.arrive + 0.1, m.arrive + 0.6, t)
      }
    } else if (M.mode === 'fly') {
      layers = [{ clip: CLIPS.flyBackX, time: 0, loop: false }]
    } else {
      layers = [{ clip: CLIPS.landBack, time: Math.min(0.26, (M.downAge || 0) * 0.5), loop: false }]
    }
    ch.evalLayers(layers.concat(faceLayers(t, m.faceKeys || memberFaceKeys(m))))
    this.place(ch, M)
    if (clap > 0 && !poseOnly) this.clapArms(ch, t, clap, m.idx)
    M.rifleW = rifleW
    return M
  }

  // "easy now / here we go": both hands up beside the head, palms out
  handsUp(ch, t, w) {
    if (w <= 0.001) return
    const fwd = ch.forward(new THREE.Vector3())
    const left = new THREE.Vector3(fwd.z, 0, -fwd.x)
    const head = ch.head.getWorldPosition(new THREE.Vector3())
    const wob = 0.03 * Math.sin(this.beatPos(t) * Math.PI)
    ch.armIK('left', head.clone().addScaledVector(left, 0.36).addScaledVector(fwd, 0.22).addScaledVector(up, 0.08 + wob), _v2.copy(left).multiplyScalar(0.8).addScaledVector(up, -0.6), w)
    ch.armIK('right', head.clone().addScaledVector(left, -0.36).addScaledVector(fwd, 0.22).addScaledVector(up, 0.08 - wob), _v2.copy(left).multiplyScalar(-0.8).addScaledVector(up, -0.6), w)
  }
  // the partner stops and looks down at the sparking dynamite in his belt, terrified: shoulders up,
  // elbows bent and tucked, hands up near the chest and chin, head and neck bent down to the belt
  lookDown(ch, w) {
    const fwd = ch.bodyForward(new THREE.Vector3())
    const left = new THREE.Vector3(fwd.z, 0, -fwd.x)
    ch.updateWorld()
    ch.rotateWorld(ch.spine2, left, 0.06 * w)
    // shoulders hunch: clavicles roll up
    ch.rotateWorld(ch.chain.left.clav, fwd, -0.22 * w)
    ch.rotateWorld(ch.chain.right.clav, fwd, 0.22 * w)
    ch.rotateWorld(ch.neck, left, 0.3 * w)
    ch.rotateWorld(ch.head, left, 0.3 * w)
    // hands up just below the chin, either side of the face, elbows bent and tucked
    const head = ch.head.getWorldPosition(new THREE.Vector3())
    for (const [side, s] of [['left', 1], ['right', -1]]) {
      const hand = head.clone().addScaledVector(fwd, 0.26).addScaledVector(left, 0.17 * s).addScaledVector(up, -0.2)
      const pole = left.clone().multiplyScalar(0.9 * s).addScaledVector(up, -1).addScaledVector(fwd, -0.2)
      ch.armIK(side, hand, pole, 0.95 * w)
    }
    // palms out: turn the hands so they face away from the body
    for (const [side, s] of [['left', 1], ['right', -1]]) {
      const c = ch.chain[side]
      const hp = c.hand.getWorldPosition(new THREE.Vector3())
      const tip = ch.palm(side, new THREE.Vector3())
      const along = tip.sub(hp).normalize()
      ch.rotateWorld(c.hand, along, 0.9 * s * w)
      ch.rotateWorld(c.hand, left, -0.5 * w)
    }
  }

  clapArms(ch, t, w, idx) {
    const bp = this.beatPos(t)
    const fr = ((bp % 1) + 1) % 1
    const open = Math.pow(Math.sin(Math.PI * fr), 0.7)
    const fwd = ch.forward(new THREE.Vector3())
    const left = new THREE.Vector3(fwd.z, 0, -fwd.x)
    const chest = ch.spine3.getWorldPosition(new THREE.Vector3())
    // every other bar the hands clap up high
    const high = Math.floor(bp / 4 + idx * 0.5) % 2 === 0 ? 0 : 1
    const c = chest.clone().addScaledVector(fwd, 0.3 + 0.05 * high).addScaledVector(up, 0.02 + 0.32 * high)
    const gap = 0.045 + 0.34 * open
    const pole = new THREE.Vector3().copy(up).multiplyScalar(-1).addScaledVector(fwd, -0.3)
    ch.model.position.y = ch.groundOffset - 0.035 * (1 - open)
    ch.updateWorld()
    ch.armIK('left', c.clone().addScaledVector(left, gap / 2), pole.clone().addScaledVector(left, 0.8), w)
    ch.armIK('right', c.clone().addScaledVector(left, -gap / 2), pole.clone().addScaledVector(left, -0.8), w)
    ch.rotateWorld(ch.head, left, -0.06 * open)
  }

  poseShooter(ch, s, t) {
    const sc = this.sc
    const plan = shooterPlan(sc, s, t)
    const last = this.bullets.filter(b => b.s === s.idx && b.fire <= t).pop()
    const since = last ? t - last.fire : 9
    const firing = since < 0.6 && t < T.boom - 0.2
    const layers = [
      { clip: CLIPS.idle, time: t * 0.4 + s.idx },
      { clip: CLIPS.rifleIdle, time: t * 0.4 },
      { clip: CLIPS.aim, time: 0, weight: ss(1.3, 1.7, t) },
    ]
    if (firing && since < 0.2) layers.push({ clip: CLIPS.shoot, time: since, loop: false })
    if (t > T.boom + 0.15 && t < T.boom + 1.4) layers.push({ clip: CLIPS.defend, time: (t - T.boom - 0.15) * ONE_SHOT_RATE, loop: false, weight: ss(T.boom + 0.15, T.boom + 0.3, t) * (1 - ss(T.boom + 1.1, T.boom + 1.4, t)) })
    ch.model.position.y = ch.groundOffset
    ch.evalLayers(layers)
    this.place(ch, { pos: plan.pos, yaw: plan.yaw })
  }

  // ---- Suave clip timing (placeholder until the manifest lands) ----------------------------
  saTime(t) { return (t - T.clipStart) * (this.sc.sa?.rate || 1) }
  // the ring loop, member i two beats later than member i-1 (as in the anim previews / the game code)
  circleTime(t, idx) {
    const B = 60 / this.sc.beat.bpm
    return (t - T.dance0) * (this.circleRate || 1) + idx * 2 * B
  }

  // ---- the frame ------------------------------------------------------------------------
  evaluate(t, camera) {
    const sc = this.sc, fx = this.fx, cast = this.cast
    const ram = cast.ram, P = this.partner
    fx.begin()
    // bodies first (no arm IK yet)
    const Pr = this.posePartner(P, t)
    const { R, f, left, yawPair, amp } = this.poseRam(t)
    sc.members.forEach((m, i) => { const r = this.poseMember(cast.ring[i], m, t); m._last = r })
    if (this.useShooters) sc.shooters.forEach((s, i) => this.poseShooter(cast.shooters[i], s, t))

    // ---- the hold: arm IK between the partners (placeholder clips only) ----
    const hold = (R.mode === 'dance' || R.mode === 'grab') ? 1 : 0
    const saBlend = sc.sa && R.mode === 'grab' ? 1 - ss(T.intro - 0.35, T.intro, t) : 1
    const contact = { joined: null, back: null, shoulder: null }
    if (hold) {
      const bp = this.beatPos(t)
      let spinRaise = 0
      for (const s of sc.spins) spinRaise = Math.max(spinRaise, ss(s.t0 - 0.25, s.t0, t) * (1 - ss(s.t1, s.t1 + 0.25, t)))
      spinRaise = Math.max(spinRaise, ss(T.spin0 - 0.1, T.spin0 + 0.05, t) * (1 - ss(T.spin1, T.push, t)))
      const pPos = Pr.pos, rPos = R.pos
      const shoulderH = 1.36
      // joined hands, swaying a little with the music
      const J = new THREE.Vector3().copy(sc.O).addScaledVector(left, 0.36).setY(shoulderH + 0.04 * Math.sin(Math.PI * bp / 2))
      const Jspin = pPos.clone().addScaledVector(f, -0.1).setY(P.height * 1.08)
      J.lerp(Jspin, spinRaise)
      let gripW = 1, backW = 1 - spinRaise, shoulderW = 1 - spinRaise
      let ramPushW = 0, partnerGripW = 1
      let G = J.clone()
      if (R.mode === 'grab') {
        // reach for the partner's hand, then pull them into the hold
        const pHand = P.palm('right', new THREE.Vector3())
        // meet the partner's hand while they step in (beats 3-4), then pull them into the hold (5-6)
        const meet = pHand.clone().lerp(ram.chain.left.upper.getWorldPosition(new THREE.Vector3()), 0.35)
        G = meet.lerp(J, sss(T.pull0, T.hold, t))
        gripW = ss(T.reach0, T.approachEnd, t) * saBlend
        partnerGripW = ss(T.reach0 + 0.3 * (T.approachEnd - T.reach0), T.approachEnd, t) * saBlend
        backW = ss(T.pull0 + 0.6 * (T.hold - T.pull0), T.hold, t) * saBlend; shoulderW = backW
      }
      const plantW = ss(T.dynHand - 0.15, T.dynHand, t) * (1 - ss(T.dynPlace + 0.05, T.spin0 + 0.1, t))
      if (t > T.push - 0.06) { ramPushW = ss(T.push - 0.06, T.push, t) * (1 - ss(T.push + 0.12, T.turn0, t)); gripW *= 1 - ramPushW; backW = 0 }
      // ---- partner arms ----
      P.updateWorld()
      const pPole = _v2.copy(left).multiplyScalar(0.6).addScaledVector(up, -1).addScaledVector(f, 0.3)
      P.armIK('right', G.clone().addScaledVector(f, 0.02), pPole, R.mode === 'grab' ? partnerGripW : gripW)
      const rShoulder = ram.chain.right.upper.getWorldPosition(new THREE.Vector3()).addScaledVector(up, 0.07).addScaledVector(f, -0.03)
      P.armIK('left', rShoulder, _v2.copy(left).multiplyScalar(-0.6).addScaledVector(up, -1), shoulderW * (1 - ss(T.push - 0.1, T.push, t)))
      // ---- Ram arms ----
      ram.armIK('left', G.clone().addScaledVector(f, -0.02), _v2.copy(left).multiplyScalar(0.6).addScaledVector(up, -1).addScaledVector(f, -0.3), gripW)
      const pBack = P.spine2.getWorldPosition(new THREE.Vector3()).addScaledVector(f, 0.07).addScaledVector(left, -0.11).addScaledVector(up, 0.04)
      let rightTgt = pBack.clone()
      const pushTgt = P.spine3.getWorldPosition(new THREE.Vector3()).addScaledVector(f, -0.16)
      if (ramPushW > 0) {
        ram.armIK('left', pushTgt.clone().addScaledVector(left, 0.14), _v2.copy(up).negate().addScaledVector(left, 0.6), ramPushW)
        rightTgt.lerp(pushTgt.clone().addScaledVector(left, -0.14), ramPushW)
      }
      ram.armIK('right', rightTgt, _v2.copy(left).multiplyScalar(-0.7).addScaledVector(up, -1), Math.max(backW * (1 - plantW), ramPushW))
      // contact readout
      contact.joined = ram.palm('left', new THREE.Vector3()).distanceTo(P.palm('right', new THREE.Vector3()))
      contact.back = backW > 0.5 && plantW < 0.2 && ramPushW < 0.2 ? ram.palm('right', new THREE.Vector3()).distanceTo(pBack) : null
      contact.planting = plantW >= 0.2
      contact.shoulder = shoulderW > 0.5 ? P.palm('left', new THREE.Vector3()).distanceTo(rShoulder) : null
    } else if (R.mode === 'sa-dance' || R.mode === 'sa-push') {
      const hp = handPairs(ram, P, new THREE.Vector3(), new THREE.Vector3())
      contact.sa = hp
      contact.joined = hp.best.d
      contact.back = ram.palm('right', new THREE.Vector3()).distanceTo(P.spine2.getWorldPosition(new THREE.Vector3()))
    }
    // ---- the plant (every clip source): Ram's right hand takes the lit dynamite from his hip and
    // slips it into the front of the partner's belt ----
    const plantW = ss(T.dynHand - 0.2, T.dynHand, t) * (1 - ss(T.dynPlace + 0.1, T.dynPlace + 0.45, t))
    if (plantW > 0) {
      const rf = ram.bodyForward(new THREE.Vector3()), rl = new THREE.Vector3(rf.z, 0, -rf.x)
      const ownHip = ram.pelvis.getWorldPosition(new THREE.Vector3()).addScaledVector(rl, -0.24).addScaledVector(rf, 0.06)
      const belt = this.beltPoint(P)
      const tgt = ownHip.lerp(belt, ss(T.dynHand + 0.05, T.dynPlace - 0.05, t))
      ram.armIK('right', tgt, _v2.copy(rl).multiplyScalar(-0.7).addScaledVector(up, -1), plantW)
      contact.planting = true; contact.back = null
    }
    if (Pr.mode === 'stumble') {
      // flailing arms
      const w = 1 - ss(T.boom - 0.2, T.boom, t)
      const ph = (t - T.push) * 9
      const fP = P.forward(new THREE.Vector3()), lP = new THREE.Vector3(fP.z, 0, -fP.x)
      const head = P.head.getWorldPosition(new THREE.Vector3())
      P.armIK('left', head.clone().addScaledVector(lP, 0.45).addScaledVector(up, 0.25 + 0.15 * Math.sin(ph)).addScaledVector(fP, 0.15 * Math.cos(ph)), _v2.set(0, -1, 0), w * 0.9)
      P.armIK('right', head.clone().addScaledVector(lP, -0.45).addScaledVector(up, 0.2 + 0.15 * Math.cos(ph * 1.1)).addScaledVector(fP, 0.15 * Math.sin(ph)), _v2.set(0, -1, 0), w * 0.9)
    }
    this.contact = contact

    // ---- skin everyone ----
    ram.skin(); P.skin()
    sc.members.forEach((m, i) => cast.ring[i].skin())
    if (this.useShooters) sc.shooters.forEach((s, i) => cast.shooters[i].skin())

    // ---- props ----
    const M = new THREE.Matrix4()
    // partner rifle: held until grabbed, then dropped
    this.placeRifle(P, t < this.partnerDropT ? this.rifleMatrix(P, M) : this.dropMatrix(P, this.partnerDropT, t, 99, M))
    sc.members.forEach((m, i) => {
      const ch = cast.ring[i]
      this.placeRifle(ch, t < m.dropT ? this.rifleMatrix(ch, M) : this.dropMatrix(ch, m.dropT, t, i, M))
    })
    if (this.useShooters) sc.shooters.forEach((s, i) => this.placeRifle(cast.shooters[i], this.rifleMatrix(cast.shooters[i], M)))
    // pistols during the escape
    const gunsOut = t > T.run0 + 0.1 && t < T.cool + 0.2
    this.pistols.forEach((p, k) => {
      p.visible = gunsOut
      if (!gunsOut) return
      const side = k ? 'left' : 'right'
      const c = ram.chain[side]
      const h = c.hand.getWorldPosition(new THREE.Vector3())
      const tip = ram.palm(side, new THREE.Vector3())
      const z = tip.clone().sub(h).normalize()
      const x = new THREE.Vector3().crossVectors(up, z).normalize()
      const y = new THREE.Vector3().crossVectors(z, x)
      p.matrixAutoUpdate = false
      p.matrix.makeBasis(x, y, z).setPosition(tip.addScaledVector(y, 0.01))
      p.matrixWorld.copy(p.matrix)
    })
    // muzzle flashes from Ram's pistols in mid-air
    if (R.mode === 'air' || R.mode === 'jump') {
      for (const ts of [0.2, 0.55, 0.9, 1.25].map(x => T.air0 + x)) {
        const k = Math.round(ts * 100) % 2
        const p = this.pistols[k]
        fx.muzzleFlash(p.userData.muzzle.clone().applyMatrix4(p.matrix), t - ts, ts * 10)
      }
    }
    // dynamite: lit in Ram's hand, then slipped into the front of the partner's belt
    const dynFrom = T.dynHand - 0.1
    const dynOn = T.dynPlace
    if (t > dynFrom && t < T.boom) {
      if (t < dynOn) {
        const h = ram.palm('right', new THREE.Vector3())
        const m = new THREE.Matrix4().makeRotationFromEuler(new THREE.Euler(0.4, R.yaw, 0.3, 'YXZ')).setPosition(h)
        fx.placeDynamite(m)
      } else {
        fx.placeDynamite(this.dynamiteMatrix(P))
      }
      if (t > T.dynLit) fx.fuseSpark(fx.fuseTipWorld(), t)
    }

    // ---- bullets: Ram has the move's armour (god mode). Outside rounds miss and spark on the
    // ground around him; no shield is drawn ----
    const chest = ram.spine2.getWorldPosition(new THREE.Vector3())
    if (this.useShooters) {
      const camPos = camera.position
      for (const b of this.bullets) {
        if (b.s >= sc.shooters.length || t < b.fire) continue
        const s = sc.shooters[b.s]
        const plan = shooterPlan(sc, s, b.fire)
        const muzzle = new THREE.Vector3().copy(s.pos).addScaledVector(fwdOf(plan.yaw), 0.95).add(_v.set(0, 1.42, 0))
        const age = t - b.fire
        fx.muzzleFlash(muzzle, age, b.seed)
        const body = ramBody(sc, b.fire)
        const toS = _v.subVectors(s.pos, body).setY(0).normalize()
        const side = new THREE.Vector3(toS.z, 0, -toS.x)
        const hitP = body.clone().addScaledVector(toS, 0.7 + 1.6 * rand(b.seed, 1)).addScaledVector(side, (rand(b.seed, 2) - 0.5) * 2.6).setY(0.03)
        const flight = muzzle.distanceTo(hitP) / 95
        if (age < flight) {
          const head = muzzle.clone().lerp(hitP, age / flight)
          const tail = head.clone().addScaledVector(_v2.subVectors(hitP, muzzle).normalize(), -Math.min(4, head.distanceTo(muzzle)))
          fx.tracer(tail, head, 1, camPos)
        } else {
          const a2 = age - flight
          const n = new THREE.Vector3(0, 1, 0).addScaledVector(toS, 0.5).normalize()
          if (a2 < 0.4) fx.sparkBurst(hitP, n, a2, b.seed, 9, 3.2, 0.32)
          fx.dustPuff(hitP, a2, b.seed)
        }
      }
    }

    // ---- explosion ----
    if (t >= T.boom) fx.explosion(sc.E, gameAge(T.boom, t))

    return { R, Pr, f, left, yawPair, chest, contact }
  }

  // front of the partner's belt (where the dynamite goes)
  beltPoint(P) {
    // the front of the belt, measured on the skinned body, sticks' centre just inside the surface
    const pf = P.bodyForward(new THREE.Vector3())
    const base = P.pelvis.getWorldPosition(new THREE.Vector3()).addScaledVector(up, 0.08)
    const depth = P.surfaceAlong(base, pf, 0.06)
    return base.addScaledVector(pf, (depth > 0.05 && depth < 0.35 ? depth : 0.15) - 0.006)
  }
  // the dynamite's matrix: follows the partner's pelvis and facing, mostly vertical, fuse up
  dynamiteMatrix(P) {
    const pf = P.bodyForward(new THREE.Vector3())
    const lean = new THREE.Vector3(0, 1, 0).applyQuaternion(P.model.getWorldQuaternion(new THREE.Quaternion()))
    const y = lean.normalize()
    const z = pf.clone().addScaledVector(y, -pf.dot(y)).normalize()
    const x = new THREE.Vector3().crossVectors(y, z)
    const m = new THREE.Matrix4().makeBasis(x, y, z)
    m.multiply(new THREE.Matrix4().makeRotationFromEuler(new THREE.Euler(-0.12, 0, 0.1)))
    return m.setPosition(this.beltPoint(P))
  }

  placeRifle(ch, M) {
    const r = this.rifleFor(ch)
    if (!M) { r.visible = false; return }
    r.visible = ch.root.visible
    r.matrix.copy(M); r.matrixWorld.copy(M)
  }
}
