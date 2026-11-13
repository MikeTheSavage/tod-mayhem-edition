// Characters: TOD glb meshes with a fast CPU skinner, a deterministic pose sampler (pose = f(time),
// no mixer state, so the timeline can be scrubbed), procedural bone tweaks and two-bone arm IK.
//
// Quirks carried over from the TOD Asset Viewer (the author's model viewer):
//  * three.js GPU skinning renders these glbs blank/exploded, CPU skinning is exact -> we bake the
//    skin on the CPU every frame into a plain mesh (here with precomputed weights, ~1400 verts/char);
//  * glTF metalness defaults to 1 and renders TOD's diffuse textures black -> metalness 0;
//  * never auto-fit the camera to all bones (GamePivot sits far away); our skeleton overlay is lines only.
import * as THREE from 'three'
import { GLTFLoader } from 'three/examples/jsm/loaders/GLTFLoader.js'
import { clone as skClone } from 'three/examples/jsm/utils/SkeletonUtils.js'

const loader = new GLTFLoader()
const glbCache = new Map()
export function loadGLB(url) {
  if (!glbCache.has(url)) glbCache.set(url, loader.loadAsync(url))
  return glbCache.get(url)
}

const _v = new THREE.Vector3(), _v2 = new THREE.Vector3(), _v3 = new THREE.Vector3()
const _q = new THREE.Quaternion(), _q2 = new THREE.Quaternion(), _q3 = new THREE.Quaternion()
const _m = new THREE.Matrix4(), _m3 = new THREE.Matrix3(), _m3i = new THREE.Matrix3()

// --- clip sampling ---------------------------------------------------------------------------
// A ClipSampler binds a clip's tracks to a character's bone indices once, then samples any time.
export const FACE_BONES = new Set(['Chin', 'ChinR', 'ChinL', 'Eyelid', 'EyeBR', 'EyeBL', 'EyeR', 'EyeL'])
export class ClipSampler {
  constructor(clip, boneIndex, only = null) {
    this.name = clip.name
    this.duration = clip.duration
    this.tracks = []
    for (const tr of clip.tracks) {
      const dot = tr.name.lastIndexOf('.')
      const node = tr.name.slice(0, dot), prop = tr.name.slice(dot + 1)
      const bi = boneIndex.get(node)
      if (bi === undefined || (only && !only.has(node))) continue
      if (prop === 'quaternion') this.tracks.push({ bi, q: true, interp: tr.createInterpolant() })
      else if (prop === 'position') this.tracks.push({ bi, q: false, interp: tr.createInterpolant() })
    }
    this.bones = new Set(this.tracks.map(t => t.bi))
  }
}

// Pose = local quaternion + position per bone, in flat arrays.
export class Pose {
  constructor(n) { this.q = new Float32Array(n * 4); this.p = new Float32Array(n * 3) }
  copy(o) { this.q.set(o.q); this.p.set(o.p); return this }
}

function sampleInto(pose, sampler, time, weight) {
  for (const t of sampler.tracks) {
    const r = t.interp.evaluate(time)
    if (t.q) {
      const o = t.bi * 4
      if (weight >= 0.999) { pose.q[o] = r[0]; pose.q[o + 1] = r[1]; pose.q[o + 2] = r[2]; pose.q[o + 3] = r[3] }
      else {
        _q.fromArray(pose.q, o); _q2.set(r[0], r[1], r[2], r[3]).normalize()
        _q.slerp(_q2, weight).toArray(pose.q, o)
      }
    } else {
      const o = t.bi * 3
      const w = Math.min(1, weight)
      pose.p[o] += (r[0] - pose.p[o]) * w; pose.p[o + 1] += (r[1] - pose.p[o + 1]) * w; pose.p[o + 2] += (r[2] - pose.p[o + 2]) * w
    }
  }
}

export function clipTime(sampler, t, loop = true) {
  const d = Math.max(sampler.duration, 1e-4)
  if (loop) return ((t % d) + d) % d
  return THREE.MathUtils.clamp(t, 0, d)
}

// --- fast CPU skin ---------------------------------------------------------------------------
class CpuSkin {
  constructor(sm, material) {
    this.sm = sm
    const g = sm.geometry
    const pos = g.attributes.position, nrm = g.attributes.normal
    const n = pos.count
    this.n = n
    const bind = sm.bindMatrix
    this.bp = new Float32Array(n * 3)   // bind-space positions (bindMatrix * v)
    this.bn = new Float32Array(n * 3)
    const nm = new THREE.Matrix3().getNormalMatrix(bind)
    for (let i = 0; i < n; i++) {
      _v.fromBufferAttribute(pos, i).applyMatrix4(bind); this.bp.set([_v.x, _v.y, _v.z], i * 3)
      if (nrm) { _v.fromBufferAttribute(nrm, i).applyMatrix3(nm).normalize(); this.bn.set([_v.x, _v.y, _v.z], i * 3) }
    }
    this.ji = new Uint16Array(n * 4); this.jw = new Float32Array(n * 4)
    const si = g.attributes.skinIndex, sw = g.attributes.skinWeight
    for (let i = 0; i < n; i++) {
      let tot = 0
      for (let k = 0; k < 4; k++) { this.ji[i * 4 + k] = si.getComponent(i, k); this.jw[i * 4 + k] = sw.getComponent(i, k); tot += this.jw[i * 4 + k] }
      if (tot > 0) for (let k = 0; k < 4; k++) this.jw[i * 4 + k] /= tot
    }
    const geo = new THREE.BufferGeometry()
    geo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(n * 3), 3))
    geo.setAttribute('normal', new THREE.BufferAttribute(new Float32Array(n * 3), 3))
    if (g.attributes.uv) geo.setAttribute('uv', g.attributes.uv)
    if (g.index) geo.setIndex(g.index)
    for (const gr of g.groups) geo.addGroup(gr.start, gr.count, gr.materialIndex)
    this.geo = geo
    this.mesh = new THREE.Mesh(geo, material)
    this.mesh.castShadow = true
    this.mesh.receiveShadow = true
    this.mesh.frustumCulled = false
    this.mesh.matrixAutoUpdate = false
    this.mats = new Float32Array(sm.skeleton.bones.length * 16)
  }
  update() {
    const sm = this.sm, sk = sm.skeleton
    sk.update()
    // M_j = inverse(mesh world) * boneWorld_j * boneInverse_j   (attached bind mode)
    const inv = _m.copy(sm.matrixWorld).invert()
    const bm = sk.boneMatrices, M = this.mats
    const tmp = new THREE.Matrix4()
    for (let j = 0; j < sk.bones.length; j++) {
      tmp.fromArray(bm, j * 16).premultiply(inv).toArray(M, j * 16)
    }
    const P = this.geo.attributes.position.array, N = this.geo.attributes.normal.array
    const bp = this.bp, bn = this.bn, ji = this.ji, jw = this.jw
    for (let i = 0; i < this.n; i++) {
      const x = bp[i * 3], y = bp[i * 3 + 1], z = bp[i * 3 + 2]
      const nx = bn[i * 3], ny = bn[i * 3 + 1], nz = bn[i * 3 + 2]
      let ox = 0, oy = 0, oz = 0, px = 0, py = 0, pz = 0
      for (let k = 0; k < 4; k++) {
        const w = jw[i * 4 + k]
        if (w === 0) continue
        const o = ji[i * 4 + k] * 16
        ox += w * (M[o] * x + M[o + 4] * y + M[o + 8] * z + M[o + 12])
        oy += w * (M[o + 1] * x + M[o + 5] * y + M[o + 9] * z + M[o + 13])
        oz += w * (M[o + 2] * x + M[o + 6] * y + M[o + 10] * z + M[o + 14])
        px += w * (M[o] * nx + M[o + 4] * ny + M[o + 8] * nz)
        py += w * (M[o + 1] * nx + M[o + 5] * ny + M[o + 9] * nz)
        pz += w * (M[o + 2] * nx + M[o + 6] * ny + M[o + 10] * nz)
      }
      P[i * 3] = ox; P[i * 3 + 1] = oy; P[i * 3 + 2] = oz
      const l = Math.hypot(px, py, pz) || 1
      N[i * 3] = px / l; N[i * 3 + 1] = py / l; N[i * 3 + 2] = pz / l
    }
    this.geo.attributes.position.needsUpdate = true
    this.geo.attributes.normal.needsUpdate = true
    // the baked mesh sits exactly where the (hidden) skinned mesh is
    this.mesh.matrix.copy(sm.matrixWorld)
    this.mesh.matrixWorld.copy(sm.matrixWorld)
  }
}

function prepMaterial(m) {
  m = m.clone()
  if ('metalness' in m) m.metalness = 0
  if ('roughness' in m) m.roughness = Math.max(m.roughness ?? 1, 0.72)
  if (m.map) { m.map.colorSpace = THREE.SRGBColorSpace; m.map.anisotropy = 4 }
  m.side = THREE.DoubleSide
  return m
}

// --- character ------------------------------------------------------------------------------
export class Character {
  constructor(gltf, { kind, label, library, mirror = false } = {}) {
    this.kind = kind
    this.label = label || kind
    this.root = new THREE.Group()          // world placement (ground point + yaw)
    this.root.name = 'char:' + this.label
    this.model = skClone(gltf.scene)
    // the raw TOD exports are mirror images of the game (text on textures reads backwards):
    // flip X on the model so the bone named "L ..." is on the character's left again
    this.mirror = mirror
    if (mirror) this.model.scale.x = -1
    this.root.add(this.model)
    this.bones = []
    this.boneIndex = new Map()
    this.byName = {}
    this.skins = []
    this.model.traverse(o => {
      if (o.isSkinnedMesh) this.skinned = this.skinned || o
    })
    const skel = this.skinned.skeleton
    // Bones are the skeleton's joints, in skeleton order, located in the CLONED model.
    skel.bones.forEach((b, i) => { this.bones.push(b); this.boneIndex.set(b.name, i); this.byName[b.name] = b })
    this.rest = new Pose(this.bones.length)
    this.bones.forEach((b, i) => { b.quaternion.toArray(this.rest.q, i * 4); b.position.toArray(this.rest.p, i * 3) })
    this.pose = new Pose(this.bones.length)
    this.model.traverse(o => {
      if (!o.isSkinnedMesh) return
      o.visible = false
      const mat = Array.isArray(o.material) ? o.material.map(prepMaterial) : prepMaterial(o.material)
      const s = new CpuSkin(o, mat)
      this.skins.push(s)
    })
    this.bakedGroup = new THREE.Group()
    this.skins.forEach(s => this.bakedGroup.add(s.mesh))
    this.samplers = new Map()
    this.library = library || gltf.animations
    this.libraryByName = new Map(this.library.map(c => [c.name, c]))
    this.pickHands()
    // ground offset from the rest pose: lowest skinned vertex
    this.applyPose(this.rest)
    this.model.updateMatrixWorld(true)
    this.skins.forEach(s => s.update())
    let minY = Infinity, maxY = -Infinity
    for (const s of this.skins) {
      const P = s.geo.attributes.position.array
      for (let i = 1; i < P.length; i += 3) { minY = Math.min(minY, P[i]); maxY = Math.max(maxY, P[i]) }
    }
    this.groundOffset = -minY
    this.height = maxY - minY
    this.model.position.y = this.groundOffset
    this.props = []
    this.ikPoleScratch = new THREE.Vector3()
  }

  // Which bone chain is on the character's visual left? The raw TOD exports are mirror images
  // (the bone named "L ..." sits on the visual right), reflected exports are not. Measure it.
  pickHands() {
    const b = this.byName
    const L = b['L_Hand'] ? 'L_' : 'L ', R = b['R_Hand'] ? 'R_' : 'R '
    this.root.updateMatrixWorld(true)
    const lx = this.root.worldToLocal(b[L + 'Hand'].getWorldPosition(_v)).x
    // facing +Z with Y up, the visual left is +X
    const mirrored = lx < 0
    this.mirrored = mirrored
    const left = mirrored ? R : L, right = mirrored ? L : R
    this.chain = {
      left: { upper: b[left + 'UpperArm'], fore: b[left + 'Forearm'], twist: b[left + 'ForeTwist'], hand: b[left + 'Hand'], finger: b[left + 'Finger1'], clav: b[left + 'Clavicle'] },
      right: { upper: b[right + 'UpperArm'], fore: b[right + 'Forearm'], twist: b[right + 'ForeTwist'], hand: b[right + 'Hand'], finger: b[right + 'Finger1'], clav: b[right + 'Clavicle'] },
      leftThigh: b[left + 'Thigh'], rightThigh: b[right + 'Thigh'],
      leftFoot: b[left + 'Foot'], rightFoot: b[right + 'Foot'],
    }
    this.pelvis = b['Pelvis']; this.spine = b['Spine']; this.spine2 = b['Spine2']; this.spine3 = b['Spine3']
    this.neck = b['Neck']; this.head = b['Head']; this.bip = b['Bip']
  }

  // face=true: only the face bones (Chin, ChinR/L, Eyelid, EyeBR/L, EyeR/L), to layer an expression
  sampler(name, face = false) {
    if (!name) return null
    const key = face ? name + '|face' : name
    let s = this.samplers.get(key)
    if (s === undefined) {
      const clip = this.libraryByName.get(name)
      s = clip ? new ClipSampler(clip, this.boneIndex, face ? FACE_BONES : null) : null
      this.samplers.set(key, s)
    }
    return s
  }
  addClips(clips) {
    for (const c of clips) { this.libraryByName.set(c.name, c); this.samplers.delete(c.name); this.samplers.delete(c.name + '|face') }
  }
  has(name) { return this.libraryByName.has(name) }

  // layers: [{ clip, time, weight=1, loop=true }]; later layers override earlier ones per bone
  evalLayers(layers) {
    const pose = this.pose.copy(this.rest)
    for (const L of layers) {
      if (!L || (L.weight ?? 1) <= 0.001) continue
      const s = typeof L.clip === 'string' ? this.sampler(L.clip, !!L.face) : L.clip
      if (!s) continue
      sampleInto(pose, s, clipTime(s, L.time || 0, L.loop !== false), L.weight ?? 1)
    }
    this.applyPose(pose)
    return pose
  }
  applyPose(pose) {
    for (let i = 0; i < this.bones.length; i++) {
      const b = this.bones[i]
      b.quaternion.fromArray(pose.q, i * 4)
      b.position.fromArray(pose.p, i * 3)
    }
  }
  updateWorld() { this.root.updateMatrixWorld(true) }

  // rotate a bone about a WORLD axis (after updateWorld). Works through mirrored parents: for an
  // orthogonal parent matrix M, M^-1 R(a, t) M = R(M^-1 a, det(M) t).
  rotateWorld(bone, axis, angle) {
    if (!angle) return
    const m3 = _m3.setFromMatrix4(bone.parent.matrixWorld)
    const det = m3.determinant()
    const a = _v.copy(axis).applyMatrix3(_m3i.copy(m3).invert()).normalize()
    _q.setFromAxisAngle(a, det < 0 ? -angle : angle)
    bone.quaternion.premultiply(_q)
    bone.updateMatrixWorld(true)
  }

  // Two-bone arm IK: place the elbow in the plane of (shoulder->target, pole), then swing the
  // upper arm and the forearm onto it (swing-only, so the clip's twist is kept). Weight-blended.
  // palm=true solves so the palm centre (not the wrist) lands on the target
  armIK(side, target, pole, weight = 1, palm = true) {
    if (weight <= 0.001) return
    if (palm) {
      let tgt = target.clone()
      for (let it = 0; it < 2; it++) {
        const c = this.chain[side]
        const wrist = c.hand.getWorldPosition(new THREE.Vector3())
        const off = this.palm(side, new THREE.Vector3()).sub(wrist)
        this.armIK(side, target.clone().sub(off), pole, weight, false)
      }
      return
    }
    const c = this.chain[side]
    const a = c.upper.getWorldPosition(new THREE.Vector3())
    const b = c.fore.getWorldPosition(new THREE.Vector3())
    const h = c.hand.getWorldPosition(new THREE.Vector3())
    const t = new THREE.Vector3().copy(h).lerp(target, weight)
    const lab = a.distanceTo(b), lcb = b.distanceTo(h)
    const lat = THREE.MathUtils.clamp(t.distanceTo(a), Math.abs(lab - lcb) + 0.01, lab + lcb - 0.004)
    const at = t.clone().sub(a).normalize()
    // angle at the shoulder between the target line and the upper arm (law of cosines)
    const cosA = THREE.MathUtils.clamp((lab * lab + lat * lat - lcb * lcb) / (2 * lab * lat), -1, 1)
    const alpha = Math.acos(cosA)
    const p = (pole ? pole.clone() : b.clone().sub(a)).addScaledVector(at, -(pole ? pole.dot(at) : b.clone().sub(a).dot(at)))
    if (p.lengthSq() < 1e-8) p.set(0, -1, 0).addScaledVector(at, at.y)
    p.normalize()
    const elbow = a.clone().addScaledVector(at, Math.cos(alpha) * lab).addScaledVector(p, Math.sin(alpha) * lab)
    const target2 = a.clone().addScaledVector(at, lat)
    const forePrev = c.fore.quaternion.clone()
    this.swingWorld(c.upper, b.clone().sub(a).normalize(), elbow.clone().sub(a).normalize())
    const b2 = c.fore.getWorldPosition(new THREE.Vector3())
    const h2 = c.hand.getWorldPosition(new THREE.Vector3())
    this.swingWorld(c.fore, h2.sub(b2).normalize(), target2.sub(b2).normalize())
    // keep the forearm twist bone following the forearm (it is a sibling under the upper arm)
    if (c.twist) {
      _q.copy(forePrev).invert().multiply(c.twist.quaternion)
      c.twist.quaternion.copy(c.fore.quaternion).multiply(_q)
      c.twist.updateMatrixWorld(true)
    }
  }
  swingWorld(bone, from, to) {
    const d = THREE.MathUtils.clamp(from.dot(to), -1, 1)
    if (d > 0.999999) return
    const axis = new THREE.Vector3().crossVectors(from, to)
    if (axis.lengthSq() < 1e-12) return
    this.rotateWorld(bone, axis.normalize(), Math.acos(d))
  }

  // palm centre (between wrist and knuckles)
  palm(side, out = new THREE.Vector3()) {
    const c = this.chain[side]
    c.hand.getWorldPosition(out)
    if (c.finger) out.lerp(c.finger.getWorldPosition(_v), 0.55)
    return out
  }

  skin() { for (const s of this.skins) s.update() }

  // where the body faces (from the hips), for clips that turn the body inside a shared root frame
  bodyForward(out = new THREE.Vector3()) {
    const l = this.chain.leftThigh.getWorldPosition(new THREE.Vector3()).sub(this.chain.rightThigh.getWorldPosition(new THREE.Vector3()))
    return out.crossVectors(l, new THREE.Vector3(0, 1, 0)).setY(0).normalize()
  }
  pelvisXZ(out = new THREE.Vector3()) { return this.pelvis.getWorldPosition(out).setY(0) }

  // how far the skinned surface reaches from `origin` along `dir` (world space), looking only at
  // vertices within `radius` of that ray: e.g. the front of the belt for a prop tucked into it
  surfaceAlong(origin, dir, radius = 0.07) {
    let best = -1
    const v = new THREE.Vector3()
    for (const s of this.skins) {
      const P = s.geo.attributes.position.array, M = s.mesh.matrixWorld
      for (let i = 0; i < P.length; i += 3) {
        v.set(P[i], P[i + 1], P[i + 2]).applyMatrix4(M).sub(origin)
        const along = v.dot(dir)
        if (along <= best) continue
        if (v.lengthSq() - along * along < radius * radius) best = along
      }
    }
    return best
  }

  // forward (+Z of the model) in world space
  forward(out = new THREE.Vector3()) { return out.set(0, 0, 1).applyQuaternion(this.root.quaternion) }
}

// Build bone line segments for the debug overlay (no camera refit; see BoneViz quirk).
export class SkeletonLines {
  constructor(character, color = 0x66d9ff) {
    this.c = character
    this.pairs = []
    const set = new Set(character.bones)
    for (const b of character.bones) {
      if (set.has(b.parent) && !/GamePivot|Eye|Chin|Finger.1|ForeTwist1/.test(b.name)) this.pairs.push([b.parent, b])
    }
    const geo = new THREE.BufferGeometry()
    geo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(this.pairs.length * 6), 3))
    this.lines = new THREE.LineSegments(geo, new THREE.LineBasicMaterial({ color, depthTest: false, transparent: true, opacity: 0.9 }))
    this.lines.frustumCulled = false
    this.lines.renderOrder = 10
  }
  update() {
    const pos = this.lines.geometry.attributes.position.array
    this.pairs.forEach(([p, c], i) => {
      p.getWorldPosition(_v); c.getWorldPosition(_v2)
      pos[i * 6] = _v.x; pos[i * 6 + 1] = _v.y; pos[i * 6 + 2] = _v.z
      pos[i * 6 + 3] = _v2.x; pos[i * 6 + 4] = _v2.y; pos[i * 6 + 5] = _v2.z
    })
    this.lines.geometry.attributes.position.needsUpdate = true
  }
}
