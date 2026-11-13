// Camera presets with critically-damped smoothing, plus free orbit. Preset framings are computed
// from the move's pure root plans, so a rendered video and the live view frame the same shot.
import * as THREE from 'three'
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js'
import { ramRoot, ramBody, partnerRoot, pairYaw, T, PHASES } from './move/suave.js'

export const PRESETS = {
  cuts: 'Cuts (the shot list)',
  gameplay: 'Gameplay (over the shoulder)',
  front: 'Front three-quarter',
  top: 'Top-down',
  orbit: 'Orbit',
  free: 'Free orbit',
}

const up = new THREE.Vector3(0, 1, 0)
const fwd = y => new THREE.Vector3(Math.sin(y), 0, Math.cos(y))
const ss = (a, b, x) => { const u = Math.min(1, Math.max(0, (x - a) / (b - a))); return u * u * (3 - 2 * u) }

function damp(cur, target, lambda, dt) { return cur.lerp(target, 1 - Math.exp(-lambda * dt)) }
function dampAngle(a, b, lambda, dt) {
  let d = (b - a) % (Math.PI * 2); if (d > Math.PI) d -= Math.PI * 2; if (d < -Math.PI) d += Math.PI * 2
  return a + d * (1 - Math.exp(-lambda * dt))
}

export class CameraRig {
  constructor(camera, dom) {
    this.camera = camera
    this.controls = new OrbitControls(camera, dom)
    this.controls.enableDamping = true
    this.controls.dampingFactor = 0.08
    this.controls.maxPolarAngle = Math.PI * 0.495
    this.controls.minDistance = 1.2
    this.controls.maxDistance = 90
    this.controls.enabled = false
    this.preset = 'front'
    this.pos = new THREE.Vector3(6, 3, -8)
    this.target = new THREE.Vector3(0, 1, 0)
    this.yaw = null
    this.shake = 0
    this.snap = true
  }
  setPreset(p) {
    this.preset = p
    this.controls.enabled = p === 'free'
    if (p === 'free') { this.controls.target.copy(this.target); this.controls.update() }
    this.snap = p !== 'free'
    this.cutAt = null
  }
  // desired framing for the move at time t
  gameplayFraming(sc, t, R) {
    // behind Ram's (smoothed) heading, slightly right, like TOD's chase camera
    // during the escape the chase camera holds its heading, so Ram dives towards the lens with
    // the ring (and then the blast) behind him
    const yaw = t >= T.turn0 ? pairYaw(sc, T.turn0) : (this.yaw ?? R.yaw)
    const f = fwd(yaw), right = new THREE.Vector3(-f.z, 0, f.x)
    const base = ramBody(sc, t).setY((R.y || 0) * 0.6)
    const back = 3.4 + 2.6 * ss(T.turn0, T.air0, t) + 2.5 * ss(T.land, T.boom + 0.8, t)
    const pos = base.clone().addScaledVector(f, -back).addScaledVector(right, 0.85).addScaledVector(up, 2.05 + 0.8 * ss(T.land, T.boom + 0.8, t))
    const target = base.clone().addScaledVector(f, 3).addScaledVector(up, 1.3).addScaledVector(right, 0.3)
    return { pos, target, yawTarget: R.yaw, fov: 50 }
  }

  desired(sc, t, info) {
    const R = ramRoot(sc, t)
    const O = sc.O
    const e = sc.escapeDir
    const p = this.preset
    if (p === 'gameplay') return this.gameplayFraming(sc, t, R)
    if (p === 'front') {
      // on the dive side, three-quarter: Ram dives towards the lens with the blast behind him
      const side = new THREE.Vector3(e.z, 0, -e.x)
      const dir = e.clone().multiplyScalar(Math.cos(0.12)).addScaledVector(side, Math.sin(0.12)).normalize()
      const late = ss(T.turn0 - 0.1, T.turn0 + 1.0, t)
      const dist = 10.5 + 4.5 * late
      const pos = O.clone().addScaledVector(dir, dist).addScaledVector(up, 3.0 + 0.6 * late)
      const target = O.clone().addScaledVector(up, 0.9).lerp(ramBody(sc, t).setY(1.0).lerp(O.clone().setY(1.6), 0.45), late)
      return { pos, target, fov: 40 }
    }
    if (p === 'top') {
      const pos = O.clone().addScaledVector(e, 5).addScaledVector(up, 24)
      return { pos, target: O.clone().addScaledVector(e, 1.2), fov: 50 }
    }
    if (p === 'orbit') {
      const a = Math.atan2(e.x, e.z) + 0.6 + t * 0.32
      const pos = O.clone().add(new THREE.Vector3(Math.sin(a) * 12.5, 4.6, Math.cos(a) * 12.5))
      return { pos, target: O.clone().addScaledVector(up, 1.1), fov: 45 }
    }
    if (p === 'cuts') return this.cuts(sc, t, R)
    return null
  }
  // The shot list from the design doc: the intro ("a longer, clearer activation": push-in with the
  // title and time freeze / over Ram's shoulder as he takes the hand / two-shot as he pulls the partner
  // in), a high orbiting wide while the ring forms, cuts on bar lines during the dance (low-angle hero
  // / over the partner's shoulder on Ram / a ring member's view / over Ram's shoulder on the partner /
  // wide), then the money-shot ending (the shot list of the design): 4a dynamite into the belt,
  // 4b low in front of Ram as he spins the partner away and launches the slow-motion dive, 5 the
  // partner looking down, 6 front on Ram mid-dive with the blast erupting behind him.
  cuts(sc, t, R) {
    const A = this.actors
    const O = sc.O, e = sc.escapeDir
    const side = new THREE.Vector3(e.z, 0, -e.x)
    const v = () => new THREE.Vector3()
    const ramP = A ? A.ram.pelvisXZ() : ramBody(sc, t)
    const ramHead = A ? A.ram.head.getWorldPosition(v()) : ramP.clone().setY(1.65)
    const pP = A ? A.partner.pelvisXZ() : partnerRoot(sc, t).pos
    const pHead = A ? A.partner.head.getWorldPosition(v()) : pP.clone().setY(1.65)
    const mid = ramP.clone().lerp(pP, 0.5)
    const f0 = fwd(sc.theta0)
    const s0 = new THREE.Vector3(f0.z, 0, -f0.x)
    const Bi = T.introBeat
    if (t < 2 * Bi) {
      // 1: quick push-in on Ram (title, time freeze), front three-quarter
      const u = ss(0, 2 * Bi, t)
      const dir = f0.clone().multiplyScalar(0.8).addScaledVector(s0, -0.6).normalize()
      return { id: 'push-in', k: 14, fov: 36, pos: ramP.clone().addScaledVector(dir, 4.6 - 2.3 * u).setY(1.55), target: ramHead.clone().setY(ramHead.y - 0.12) }
    }
    if (t < 4 * Bi) {
      // 2: over Ram's shoulder towards the partner (he steps in and takes the hand)
      const back = ramHead.clone().sub(pHead).setY(0).normalize()
      const pos = ramHead.clone().addScaledVector(back, 1.3).addScaledVector(new THREE.Vector3(back.z, 0, -back.x), -0.55)
      pos.y = ramHead.y + 0.2
      return { id: 'intro-ots', k: 8, fov: 42, pos, target: pHead.clone().setY(pHead.y - 0.2) }
    }
    if (t < T.intro) {
      // 3: two-shot from the side as Ram pulls the partner into the hold
      // side-on to the line between them (live), so both bodies are in frame
      // side-on to the line between the two bodies, so both are in frame (the clips' hold can be side by side)
      const axis = pP.clone().sub(ramP).setY(0)
      if (axis.lengthSq() < 0.06) axis.copy(sc.partnerStart.pos).sub(sc.ramStart.pos).setY(0)
      axis.normalize()
      const perp = new THREE.Vector3(axis.z, 0, -axis.x)
      if (perp.dot(s0) > 0) perp.negate()
      return { id: 'two-shot', k: 7, fov: 42, pos: mid.clone().addScaledVector(perp, 3.4).setY(1.45), target: mid.clone().setY(1.25) }
    }
    if (t < T.dance0) {
      // the ring forms: high wide shot, slowly orbiting
      const a = Math.atan2(e.x, e.z) + 0.7 + (t - T.intro) * 0.22
      return { id: 'orbit-wide', k: 8, fov: 46, pos: O.clone().add(new THREE.Vector3(Math.sin(a) * 11, 7.5, Math.cos(a) * 11)), target: O.clone().setY(0.6) }
    }
    if (t < T.shot4a) {
      // the dance: a cut on every bar line (4 beats)
      const B = 60 / sc.beat.bpm
      const bar = Math.floor((t - sc.beat.firstBeat - T.musicZero + 1e-4) / (4 * B))
      const n = sc.members.length
      const kind = ((bar % 5) + 5) % 5
      const axis = pP.clone().sub(ramP).setY(0)
      if (axis.lengthSq() < 1e-4) axis.copy(f0)
      axis.normalize()
      const perp = new THREE.Vector3(axis.z, 0, -axis.x).multiplyScalar(bar % 10 < 5 ? 1 : -1)
      if (kind === 0) {
        // low-angle hero on the pair
        // inside the ring so no member's back fills the foreground
        return { id: 'hero' + bar, k: 6, fov: 54, pos: mid.clone().addScaledVector(perp, 2.1).addScaledVector(axis, -0.5).setY(0.45), target: mid.clone().setY(1.4) }
      }
      if (kind === 1 || kind === 3) {
        // over one dancer's shoulder onto the other's face (1: on Ram, 3: on the partner)
        const [near, far] = kind === 1 ? [pHead, ramHead] : [ramHead, pHead]
        const back = near.clone().sub(far).setY(0).normalize()
        const pos = near.clone().addScaledVector(back, 1.1).addScaledVector(new THREE.Vector3(back.z, 0, -back.x), 0.45)
        pos.y = near.y + 0.15
        return { id: 'ots' + bar, k: 7, fov: 34, pos, target: far.clone().setY(far.y - 0.04) }
      }
      if (kind === 2 && n) {
        // a ring member's view of the couple
        const m = sc.members[(bar * 3) % n]
        const out = m.slot.clone().sub(O).setY(0).normalize()
        const sideM = new THREE.Vector3(out.z, 0, -out.x)
        return { id: 'member' + bar, k: 6, fov: 42, pos: m.slot.clone().addScaledVector(out, 0.9).addScaledVector(sideM, 0.45).setY(1.85), target: mid.clone().setY(1.25) }
      }
      const a = Math.atan2(e.x, e.z) + 0.5 + bar * 1.7
      return { id: 'wide' + bar, k: 5, fov: 44, pos: O.clone().add(new THREE.Vector3(Math.sin(a) * 9.5, 3.2, Math.cos(a) * 9.5)), target: O.clone().setY(1.0) }
    }
    if (t < T.shot4b) {
      // 4a: close-up on Ram's hand and the partner's belt, Ram's slick smile in frame
      const pf = A ? A.partner.bodyForward(v()) : e.clone().negate()
      const belt = pP.clone().addScaledVector(pf, 0.17).setY(0.95)
      const axis = pP.clone().sub(ramP).setY(0).normalize()
      const perp = new THREE.Vector3(axis.z, 0, -axis.x)
      const tgt = belt.clone().lerp(ramHead, 0.5)
      return { id: '4a-dynamite', k: 9, fov: 46, pos: tgt.clone().addScaledVector(perp, 1.35).addScaledVector(axis, -0.25).setY(tgt.y + 0.05), target: tgt }
    }
    const start = ramRoot(sc, T.turn0).pos
    if (t < T.shot5) {
      // 4b: low, in front of Ram: he spins the partner away and dives towards the lens
      // inside the ring, retreating ahead of Ram as he runs and dives towards it
      const p = A ? ramP : ramBody(sc, t)
      const lead = t < T.turn0 ? start : ramRoot(sc, t).pos
      // on the dive lane's centre line with a narrower lens, so the ring members either side stay out of frame
      const along = lead.clone().sub(O).dot(e)
      return { id: '4b-low-front', k: 7, fov: 40, pos: O.clone().addScaledVector(e, Math.max(3.6, along + 3.4)).setY(0.45), target: p.clone().setY(0.9 + (R.y || 0) * 0.7) }
    }
    if (t < T.shot6) {
      // 5: the partner stops and looks down at the sparking dynamite
      const pf = A ? A.partner.bodyForward(v()) : e.clone().negate()
      const ps = new THREE.Vector3(pf.z, 0, -pf.x)
      // medium close-up from just above chest height, a little to the side: the head tilting down
      // towards the dynamite in the belt, the face readable under the cap brim
      const head = A ? A.partner.head.getWorldPosition(v()).y : 1.6
      const chest = A ? A.partner.spine3.getWorldPosition(v()).y : 1.35
      return { id: '5-look-down', k: 8, fov: 38, pos: pP.clone().addScaledVector(pf, 2.0).addScaledVector(ps, 0.7).setY(chest + 0.08), target: pP.clone().addScaledVector(pf, 0.1).setY(head - 0.32) }
    }
    // 6: back to the front angle on Ram mid-dive; the blast erupts behind him, he lands towards us
    // tracks ahead of Ram while he flies, holds once he has landed
    const u = ss(T.land, T.end, t)
    const p = ramBody(sc, Math.min(t, T.land + 0.3))
    const now = ramBody(sc, t)
    return {
      id: '6-front-dive', k: 9, fov: 48,
      pos: p.clone().addScaledVector(e, 4.4 + 1.0 * u).addScaledVector(side, 0.8).setY(0.95 + 0.35 * u),
      target: now.clone().setY(0.75 + (R.y || 0) * 0.6).lerp(sc.E.clone().setY(1.8), 0.15),
    }
  }

  update(sc, t, dt, boomAge, actors = null) {
    this.actors = actors
    const cam = this.camera
    if (this.preset === 'free' || !sc) {
      this.controls.update()
      this.target.copy(this.controls.target); this.pos.copy(cam.position)
    } else {
      const R = ramRoot(sc, t)
      if (this.yaw === null || this.snap) this.yaw = R.yaw
      // the chase camera swings round lazily (TOD's camera trails the player's heading)
      this.yaw = dampAngle(this.yaw, R.yaw, R.mode === 'dance' ? 0.6 : 1.6, dt)
      const d = this.desired(sc, t, null)
      const cut = d.id !== undefined && d.id !== this.cutAt
      if (this.snap || cut) { this.pos.copy(d.pos); this.target.copy(d.target); this.snap = false; this.cutAt = d.id; if (d.fov) { cam.fov = d.fov; cam.updateProjectionMatrix() } }
      else {
        const k = d.k || (this.preset === 'gameplay' ? 5 : 3.2)
        damp(this.pos, d.pos, k, dt); damp(this.target, d.target, k * 1.4, dt)
      }
      if (d.fov && Math.abs(cam.fov - d.fov) > 0.01) { cam.fov += (d.fov - cam.fov) * (1 - Math.exp(-4 * dt)); cam.updateProjectionMatrix() }
      cam.position.copy(this.pos)
      cam.lookAt(this.target)
    }
    // explosion shake (deterministic in time)
    if (boomAge !== null && boomAge >= 0 && boomAge < 1.6) {
      const a = 0.22 * Math.exp(-boomAge * 3.0)
      const s = boomAge * 37
      cam.position.x += Math.sin(s * 1.3) * a; cam.position.y += Math.sin(s * 1.7 + 1) * a * 0.7; cam.position.z += Math.sin(s * 1.1 + 2) * a
    }
  }
}

export function playCamera(camera, ramPos, ramYaw, camYaw, pitch, dt, state) {
  // chase camera for play mode: yaw follows the mouse, height eases with pitch
  const f = fwd(camYaw)
  const right = new THREE.Vector3(-f.z, 0, f.x)
  const dist = 4.2
  const desired = ramPos.clone().addScaledVector(f, -dist * Math.cos(pitch)).addScaledVector(right, 0.7).add(new THREE.Vector3(0, 1.6 + dist * Math.sin(pitch), 0))
  const target = ramPos.clone().addScaledVector(f, 2.5).add(new THREE.Vector3(0, 1.35, 0)).addScaledVector(right, 0.3)
  if (!state.pos) { state.pos = desired.clone(); state.target = target.clone() }
  damp(state.pos, desired, 8, dt); damp(state.target, target, 10, dt)
  camera.position.copy(state.pos)
  camera.lookAt(state.target)
}
