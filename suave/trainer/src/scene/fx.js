// Effects: golden shield, bullet tracers, muzzle flashes, sparks, the dynamite prop, guns, and the
// explosion. Everything is drawn from a pure function of timeline time so scrubbing and video
// rendering give the same picture.
import * as THREE from 'three'
import { radialTexture } from './world.js'

export function hash(n) { const x = Math.sin(n * 127.1 + 311.7) * 43758.5453; return x - Math.floor(x) }
export function rand(seed, k) { return hash(seed * 17.13 + k * 3.71) }
const clamp01 = x => Math.min(1, Math.max(0, x))
const _v = new THREE.Vector3(), _v2 = new THREE.Vector3(), _v3 = new THREE.Vector3()

// ---- point sprites ----------------------------------------------------------------------------
const spriteVS = `
attribute float size; attribute float alpha; attribute vec3 pcolor; attribute float rot;
uniform float uScale;
varying float vAlpha; varying vec3 vColor; varying float vRot;
void main(){
  vec4 mv = modelViewMatrix * vec4(position,1.0);
  gl_Position = projectionMatrix * mv;
  gl_PointSize = size * uScale / max(0.1, -mv.z);
  vAlpha = alpha; vColor = pcolor; vRot = rot;
}`
const spriteFS = `
uniform sampler2D map;
varying float vAlpha; varying vec3 vColor; varying float vRot;
void main(){
  vec2 p = gl_PointCoord - 0.5;
  float c = cos(vRot), s = sin(vRot);
  p = mat2(c,-s,s,c) * p + 0.5;
  vec4 t = texture2D(map, p);
  gl_FragColor = vec4(vColor * t.rgb, t.a * vAlpha);
  if (gl_FragColor.a < 0.003) discard;
}`

export class SpritePool {
  constructor(max, texture, { additive = true, depthWrite = false } = {}) {
    this.max = max
    const g = new THREE.BufferGeometry()
    this.pos = new Float32Array(max * 3); this.col = new Float32Array(max * 3)
    this.alpha = new Float32Array(max); this.size = new Float32Array(max); this.rot = new Float32Array(max)
    g.setAttribute('position', new THREE.BufferAttribute(this.pos, 3))
    g.setAttribute('pcolor', new THREE.BufferAttribute(this.col, 3))
    g.setAttribute('alpha', new THREE.BufferAttribute(this.alpha, 1))
    g.setAttribute('size', new THREE.BufferAttribute(this.size, 1))
    g.setAttribute('rot', new THREE.BufferAttribute(this.rot, 1))
    this.mat = new THREE.ShaderMaterial({
      uniforms: { map: { value: texture }, uScale: { value: 600 } },
      vertexShader: spriteVS, fragmentShader: spriteFS, transparent: true, depthWrite,
      blending: additive ? THREE.AdditiveBlending : THREE.NormalBlending,
    })
    this.points = new THREE.Points(g, this.mat)
    this.points.frustumCulled = false
    this.n = 0
  }
  begin() { this.n = 0 }
  push(x, y, z, r, g, b, a, size, rot = 0) {
    if (this.n >= this.max) return
    const i = this.n++
    this.pos[i * 3] = x; this.pos[i * 3 + 1] = y; this.pos[i * 3 + 2] = z
    this.col[i * 3] = r; this.col[i * 3 + 1] = g; this.col[i * 3 + 2] = b
    this.alpha[i] = a; this.size[i] = size; this.rot[i] = rot
  }
  end(scale) {
    const g = this.points.geometry
    for (const k of ['position', 'pcolor', 'alpha', 'size', 'rot']) g.attributes[k].needsUpdate = true
    g.setDrawRange(0, this.n)
    this.mat.uniforms.uScale.value = scale
  }
}

function puffTexture(seed = 1) {
  const c = document.createElement('canvas'); c.width = c.height = 128
  const g = c.getContext('2d')
  for (let i = 0; i < 26; i++) {
    const a = rand(seed, i) * Math.PI * 2, d = rand(seed, i + 50) * 30
    const x = 64 + Math.cos(a) * d, y = 64 + Math.sin(a) * d, r = 18 + rand(seed, i + 99) * 26
    const grad = g.createRadialGradient(x, y, 0, x, y, r)
    grad.addColorStop(0, 'rgba(255,255,255,0.35)'); grad.addColorStop(1, 'rgba(255,255,255,0)')
    g.fillStyle = grad; g.fillRect(0, 0, 128, 128)
  }
  const t = new THREE.CanvasTexture(c); return t
}
function sparkTexture() {
  return radialTexture([[0, 'rgba(255,255,255,1)'], [0.25, 'rgba(255,255,255,0.8)'], [0.6, 'rgba(255,255,255,0.15)'], [1, 'rgba(255,255,255,0)']], 64)
}
function starTexture() {
  const c = document.createElement('canvas'); c.width = c.height = 128
  const g = c.getContext('2d')
  g.translate(64, 64)
  for (let k = 0; k < 5; k++) {
    g.rotate(Math.PI * 2 / 5 + 0.3)
    const grad = g.createLinearGradient(0, 0, 60, 0)
    grad.addColorStop(0, 'rgba(255,255,255,1)'); grad.addColorStop(1, 'rgba(255,255,255,0)')
    g.fillStyle = grad
    g.beginPath(); g.moveTo(0, -7); g.lineTo(62, 0); g.lineTo(0, 7); g.fill()
  }
  const rg = g.createRadialGradient(0, 0, 0, 0, 0, 30)
  rg.addColorStop(0, 'rgba(255,255,255,1)'); rg.addColorStop(1, 'rgba(255,255,255,0)')
  g.fillStyle = rg; g.fillRect(-64, -64, 128, 128)
  return new THREE.CanvasTexture(c)
}

// ---- camera-facing ribbons for tracers ---------------------------------------------------------
class RibbonPool {
  constructor(max) {
    this.max = max
    const g = new THREE.BufferGeometry()
    this.pos = new Float32Array(max * 4 * 3); this.uv = new Float32Array(max * 4 * 2); this.a = new Float32Array(max * 4)
    const idx = []
    for (let i = 0; i < max; i++) { const o = i * 4; idx.push(o, o + 1, o + 2, o + 2, o + 1, o + 3) }
    g.setIndex(idx)
    g.setAttribute('position', new THREE.BufferAttribute(this.pos, 3))
    g.setAttribute('uv', new THREE.BufferAttribute(this.uv, 2))
    g.setAttribute('alpha', new THREE.BufferAttribute(this.a, 1))
    this.mat = new THREE.ShaderMaterial({
      vertexShader: `attribute float alpha; varying vec2 vUv; varying float vA; void main(){ vUv=uv; vA=alpha; gl_Position=projectionMatrix*modelViewMatrix*vec4(position,1.0);} `,
      fragmentShader: `varying vec2 vUv; varying float vA; void main(){ float across = 1.0-abs(vUv.y*2.0-1.0); float along = vUv.x; vec3 c = mix(vec3(1.0,0.55,0.15), vec3(1.0,0.97,0.8), along*across); gl_FragColor = vec4(c*1.6, pow(across,1.5)*along*vA);} `,
      transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, side: THREE.DoubleSide,
    })
    this.mesh = new THREE.Mesh(g, this.mat)
    this.mesh.frustumCulled = false
    this.n = 0
  }
  begin() { this.n = 0 }
  push(a, b, width, alpha, camPos) {
    if (this.n >= this.max) return
    const i = this.n++
    const dir = _v.subVectors(b, a)
    const toCam = _v2.subVectors(camPos, a)
    const side = _v3.crossVectors(dir, toCam).normalize().multiplyScalar(width / 2)
    const P = this.pos, o = i * 12
    P[o] = a.x - side.x; P[o + 1] = a.y - side.y; P[o + 2] = a.z - side.z
    P[o + 3] = a.x + side.x; P[o + 4] = a.y + side.y; P[o + 5] = a.z + side.z
    P[o + 6] = b.x - side.x; P[o + 7] = b.y - side.y; P[o + 8] = b.z - side.z
    P[o + 9] = b.x + side.x; P[o + 10] = b.y + side.y; P[o + 11] = b.z + side.z
    const U = this.uv, u = i * 8
    U.set([0, 0, 0, 1, 1, 0, 1, 1], u)
    this.a.fill(alpha, i * 4, i * 4 + 4)
  }
  end() {
    const g = this.mesh.geometry
    g.attributes.position.needsUpdate = true; g.attributes.uv.needsUpdate = true; g.attributes.alpha.needsUpdate = true
    g.setDrawRange(0, this.n * 6)
  }
}

// ---- golden shield ------------------------------------------------------------------------------
const MAX_HITS = 10
function makeShield() {
  const hits = []
  for (let i = 0; i < MAX_HITS; i++) hits.push(new THREE.Vector4(0, 1, 0, 99))
  const mat = new THREE.ShaderMaterial({
    uniforms: { uTime: { value: 0 }, uHits: { value: hits }, uOpacity: { value: 0 } },
    vertexShader: `varying vec3 vN; varying vec3 vView; varying vec3 vLocal;
      void main(){ vLocal = normalize(position); vec4 wp = modelMatrix*vec4(position,1.0); vN = normalize(mat3(modelMatrix)*normal); vView = normalize(cameraPosition - wp.xyz); gl_Position = projectionMatrix*viewMatrix*wp; }`,
    fragmentShader: `uniform float uTime; uniform vec4 uHits[${MAX_HITS}]; uniform float uOpacity;
      varying vec3 vN; varying vec3 vView; varying vec3 vLocal;
      void main(){
        float f = 1.0 - abs(dot(normalize(vN), normalize(vView)));
        float rim = pow(f, 2.6);
        // faint hex-ish lattice
        vec3 p = vLocal * 9.0;
        float lat = smoothstep(0.92, 1.0, max(abs(sin(p.x*3.1+p.y)), max(abs(sin(p.y*3.1+p.z)), abs(sin(p.z*3.1+p.x)))));
        float shimmer = 0.5 + 0.5*sin(uTime*3.0 + vLocal.y*8.0);
        float ripple = 0.0;
        for (int i=0;i<${MAX_HITS};i++){
          vec4 h = uHits[i];
          float age = h.w;
          if (age < 0.0 || age > 0.6) continue;
          float d = acos(clamp(dot(vLocal, normalize(h.xyz)), -1.0, 1.0));
          float r = age * 2.4;
          ripple += (smoothstep(0.12, 0.0, abs(d - r)) * 0.9 + smoothstep(0.35, 0.0, d) * 1.6 * (1.0 - age/0.15)) * (1.0 - age/0.6);
        }
        vec3 gold = vec3(1.0, 0.72, 0.22);
        float a = (rim*0.5 + lat*0.05*shimmer + 0.01) * uOpacity + ripple*uOpacity;
        gl_FragColor = vec4(gold*(1.2 + ripple*1.5), clamp(a, 0.0, 1.0));
      }`,
    transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, side: THREE.FrontSide,
  })
  const m = new THREE.Mesh(new THREE.SphereGeometry(1, 48, 32), mat)
  m.renderOrder = 5
  return m
}

// ---- props --------------------------------------------------------------------------------------
const metal = new THREE.MeshStandardMaterial({ color: '#2a2a2c', roughness: 0.45, metalness: 0.6 })
const wood = new THREE.MeshStandardMaterial({ color: '#6b3f22', roughness: 0.7, metalness: 0 })
export function makeRifle() {
  // barrel along +Z, origin at the rear grip
  const g = new THREE.Group()
  const add = (geo, mat, x, y, z) => { const m = new THREE.Mesh(geo, mat); m.position.set(x, y, z); m.castShadow = true; g.add(m); return m }
  add(new THREE.BoxGeometry(0.055, 0.09, 0.42), metal, 0, 0.03, 0.16)
  add(new THREE.CylinderGeometry(0.014, 0.014, 0.42, 8).rotateX(Math.PI / 2), metal, 0, 0.05, 0.56)
  add(new THREE.BoxGeometry(0.05, 0.1, 0.26), wood, 0, -0.01, -0.16)
  add(new THREE.BoxGeometry(0.04, 0.16, 0.06), metal, 0, -0.07, 0.2).rotation.x = 0.25
  add(new THREE.BoxGeometry(0.045, 0.06, 0.2), wood, 0, -0.005, 0.42)
  g.userData.muzzle = new THREE.Vector3(0, 0.05, 0.78)
  return g
}
export function makePistol() {
  const g = new THREE.Group()
  const add = (geo, mat, x, y, z) => { const m = new THREE.Mesh(geo, mat); m.position.set(x, y, z); m.castShadow = true; g.add(m); return m }
  add(new THREE.BoxGeometry(0.032, 0.045, 0.2), metal, 0, 0.035, 0.06)
  add(new THREE.BoxGeometry(0.03, 0.1, 0.045), metal, 0, -0.02, -0.01).rotation.x = -0.25
  g.userData.muzzle = new THREE.Vector3(0, 0.04, 0.17)
  return g
}
export function makeDynamite() {
  const g = new THREE.Group()
  const red = new THREE.MeshStandardMaterial({ color: '#b51f17', roughness: 0.55 })
  const tape = new THREE.MeshStandardMaterial({ color: '#d8c49a', roughness: 0.8 })
  // three sticks, 20 cm x 3 cm, taped; origin at the sticks' centre, fuse up (+y), front = +z
  for (let i = -1; i <= 1; i++) {
    const s = new THREE.Mesh(new THREE.CylinderGeometry(0.015, 0.015, 0.2, 12), red)
    s.position.set(i * 0.031, 0, 0); s.castShadow = true; g.add(s)
  }
  for (const y of [-0.055, 0.045]) {
    const b = new THREE.Mesh(new THREE.BoxGeometry(0.1, 0.016, 0.034), tape); b.position.y = y; g.add(b)
  }
  const curve = new THREE.CatmullRomCurve3([new THREE.Vector3(0, 0.1, 0), new THREE.Vector3(0.01, 0.125, 0.006), new THREE.Vector3(0.026, 0.14, 0.0), new THREE.Vector3(0.04, 0.135, -0.006)])
  const fuse = new THREE.Mesh(new THREE.TubeGeometry(curve, 10, 0.0025, 5), new THREE.MeshStandardMaterial({ color: '#3b2a1c', roughness: 1 }))
  g.add(fuse)
  g.userData.fuseTip = new THREE.Vector3(0.04, 0.135, -0.006)
  return g
}

// ---- the FX system -----------------------------------------------------------------------------
export class FX {
  constructor(scene) {
    this.scene = scene
    this.shield = makeShield(); this.shield.visible = false; scene.add(this.shield)
    this.sparks = new SpritePool(1600, sparkTexture())
    this.fire = new SpritePool(400, puffTexture(3))
    this.smoke = new SpritePool(400, puffTexture(9), { additive: false })
    this.flashes = new SpritePool(64, starTexture())
    this.tracers = new RibbonPool(80)
    scene.add(this.smoke.points, this.fire.points, this.sparks.points, this.flashes.points, this.tracers.mesh)
    this.smoke.points.renderOrder = 6; this.fire.points.renderOrder = 7; this.sparks.points.renderOrder = 8; this.flashes.points.renderOrder = 9
    this.boomLight = new THREE.PointLight('#ffaa55', 0, 40, 1.6)
    scene.add(this.boomLight)
    this.fuseLight = new THREE.PointLight('#ffb050', 0, 0.12, 2)   // a few cm of glow, no torso flood
    scene.add(this.fuseLight)
    this.shock = new THREE.Mesh(new THREE.RingGeometry(0.85, 1, 64).rotateX(-Math.PI / 2),
      new THREE.MeshBasicMaterial({ color: '#ffe0a8', transparent: true, opacity: 0, depthWrite: false, blending: THREE.AdditiveBlending }))
    this.shock.position.y = 0.05
    scene.add(this.shock)
    this.scorch = new THREE.Mesh(new THREE.PlaneGeometry(1, 1).rotateX(-Math.PI / 2),
      new THREE.MeshBasicMaterial({ map: radialTexture([[0, 'rgba(15,8,4,0.95)'], [0.45, 'rgba(30,16,8,0.7)'], [1, 'rgba(40,20,10,0)']]), transparent: true, depthWrite: false, opacity: 0, polygonOffset: true, polygonOffsetFactor: -3 }))
    this.scorch.position.y = 0.02
    scene.add(this.scorch)
    this.fireball = new THREE.Mesh(new THREE.SphereGeometry(1, 32, 20), new THREE.MeshBasicMaterial({ color: '#ffb04a', transparent: true, opacity: 0, depthWrite: false, blending: THREE.AdditiveBlending }))
    scene.add(this.fireball)
    this.dynamite = makeDynamite(); this.dynamite.visible = false; scene.add(this.dynamite)
  }

  begin() {
    this.sparks.begin(); this.fire.begin(); this.smoke.begin(); this.flashes.begin(); this.tracers.begin()
    this.shield.visible = false; this.boomLight.intensity = 0; this.fuseLight.intensity = 0
    this.shock.material.opacity = 0; this.scorch.material.opacity = 0; this.fireball.material.opacity = 0
    this.dynamite.visible = false
  }
  end(camera, viewportHeight) {
    const scale = viewportHeight / (2 * Math.tan(THREE.MathUtils.degToRad(camera.fov) / 2))
    this.sparks.end(scale); this.fire.end(scale); this.smoke.end(scale); this.flashes.end(scale); this.tracers.end()
  }

  // shield around Ram's chest; hits = [{dir: Vector3 (unit, world), age}]
  drawShield(center, radius, opacity, t, hits, camPos = null) {
    // fade the shield out when the camera gets inside or right next to it (close-ups)
    if (camPos) { const d = camPos.distanceTo(center); opacity *= Math.min(1, Math.max(0, (d - radius - 0.4) / 1.6)) }
    if (opacity <= 0.001) return
    this.shield.visible = true
    this.shield.position.copy(center)
    this.shield.scale.setScalar(radius)
    const u = this.shield.material.uniforms
    u.uTime.value = t; u.uOpacity.value = opacity
    for (let i = 0; i < MAX_HITS; i++) {
      const h = hits[i]
      if (h) u.uHits.value[i].set(h.dir.x, h.dir.y, h.dir.z, h.age)
      else u.uHits.value[i].set(0, 1, 0, 99)
    }
  }

  tracer(a, b, alpha, camPos) { this.tracers.push(a, b, 0.045, alpha, camPos) }

  muzzleFlash(p, age, seed) {
    if (age < 0 || age > 0.06) return
    const k = 1 - age / 0.06
    this.flashes.push(p.x, p.y, p.z, 1.0, 0.85, 0.45, k, 0.45 + 0.25 * hash(seed), hash(seed + 1) * 6)
  }

  // spark burst at p along normal n, age seconds after the hit
  sparkBurst(p, n, age, seed, count = 12, speed = 4.5, life = 0.38) {
    if (age < 0 || age > life) return
    for (let i = 0; i < count; i++) {
      const r1 = rand(seed, i), r2 = rand(seed, i + 40), r3 = rand(seed, i + 80)
      // random direction in the hemisphere around n
      const d = _v.set(r1 * 2 - 1, r2 * 2 - 1, r3 * 2 - 1).normalize().addScaledVector(n, 1.2).normalize()
      const sp = speed * (0.4 + 0.8 * rand(seed, i + 120))
      const a = age
      const x = p.x + d.x * sp * a, y = p.y + d.y * sp * a - 4.9 * a * a, z = p.z + d.z * sp * a
      const k = 1 - a / life
      this.sparks.push(x, y, z, 1.0, 0.75 + 0.2 * k, 0.35 * k, k, 0.07 + 0.05 * k)
    }
    if (age < 0.07) this.flashes.push(p.x, p.y, p.z, 1, 0.8, 0.4, 1 - age / 0.07, 0.6, seed)
  }

  // a little dust kicked up by a round hitting the ground
  dustPuff(p, age, seed) {
    if (age < 0 || age > 0.9) return
    const u = age / 0.9
    for (let i = 0; i < 3; i++) {
      const a = rand(seed, i + 7) * Math.PI * 2
      const r = 0.05 + 0.25 * u
      this.smoke.push(p.x + Math.cos(a) * r, p.y + 0.08 + 0.35 * u * (0.6 + rand(seed, i)), p.z + Math.sin(a) * r, 0.62, 0.5, 0.38, 0.55 * (1 - u), 0.35 + 0.6 * u, a)
    }
  }

  fuseSpark(p, t) {
    for (let i = 0; i < 6; i++) {
      const ph = (t * 7 + i / 6) % 1
      const s = Math.floor(t * 7 + i / 6) * 13 + i
      const d = _v.set(rand(s, 1) - 0.5, rand(s, 2) * 0.8 + 0.2, rand(s, 3) - 0.5).normalize()
      const x = p.x + d.x * ph * 0.09, y = p.y + d.y * ph * 0.09 - ph * ph * 0.05, z = p.z + d.z * ph * 0.09
      this.sparks.push(x, y, z, 1, 0.8, 0.3, 1 - ph, 0.012)
    }
    this.flashes.push(p.x, p.y, p.z, 1, 0.75, 0.35, 0.55 + 0.35 * hash(Math.floor(t * 30)), 0.045, t * 20)
    this.fuseLight.position.copy(p)
    this.fuseLight.intensity = 0.006 + hash(Math.floor(t * 24)) * 0.006
  }

  placeDynamite(matrix) {
    this.dynamite.visible = true
    this.dynamite.matrixAutoUpdate = false
    this.dynamite.matrix.copy(matrix)
    this.dynamite.matrixWorld.copy(matrix)
  }
  fuseTipWorld(out = new THREE.Vector3()) { return out.copy(this.dynamite.userData.fuseTip).applyMatrix4(this.dynamite.matrix) }

  // the explosion at E, age seconds after detonation
  explosion(E, age) {
    if (age < 0) return
    // flash + fireball core
    const f = clamp01(1 - age / 0.28)
    this.fireball.position.set(E.x, E.y + 1.0 + age * 1.5, E.z)
    this.fireball.scale.setScalar(0.5 + 3.6 * clamp01(age / 0.2))
    this.fireball.material.opacity = f * f * 0.55
    this.boomLight.position.set(E.x, E.y + 2.5, E.z)
    this.boomLight.intensity = 180 * Math.exp(-age * 3.2) * (0.85 + 0.15 * hash(Math.floor(age * 30)))
    // shockwave on the ground
    if (age < 0.8) {
      const s = 0.5 + 16 * Math.pow(age / 0.8, 0.6)
      this.shock.position.set(E.x, 0.06, E.z); this.shock.scale.setScalar(s)
      this.shock.material.opacity = 0.75 * (1 - age / 0.8)
    }
    // scorch
    this.scorch.position.set(E.x, 0.025, E.z); this.scorch.scale.setScalar(9)
    this.scorch.material.opacity = clamp01(age / 0.25) * 0.9
    // fire puffs
    for (let i = 0; i < 70; i++) {
      const life = 0.7 + 1.0 * rand(5, i)
      const a = age - rand(6, i) * 0.12
      if (a < 0 || a > life) continue
      const u = a / life
      const dir = _v.set(rand(7, i) * 2 - 1, rand(8, i) * 1.3, rand(9, i) * 2 - 1).normalize()
      const reach = 1.5 + 3.5 * rand(10, i)
      const d = reach * (1 - Math.pow(1 - Math.min(1, u * 2.2), 3))
      const x = E.x + dir.x * d, y = E.y + 0.8 + Math.max(0, dir.y) * d + u * u * 3.5, z = E.z + dir.z * d
      const heat = 1 - u
      this.fire.push(x, y, z, 1.0, 0.32 + 0.42 * heat * heat, 0.06 + 0.3 * Math.pow(heat, 4), (1 - u) * (1 - u) * 0.5, 1.8 + 3.2 * u + 1.2 * rand(11, i), rand(12, i) * 6 + u)
    }
    // smoke
    for (let i = 0; i < 90; i++) {
      const life = 2.6 + 1.8 * rand(15, i)
      const a = age - 0.12 - rand(16, i) * 0.35
      if (a < 0 || a > life) continue
      const u = a / life
      const dir = _v.set(rand(17, i) * 2 - 1, rand(18, i) * 0.6, rand(19, i) * 2 - 1).normalize()
      const reach = 1.5 + 5.0 * rand(20, i)
      const d = reach * (1 - Math.pow(1 - Math.min(1, u * 1.8), 2))
      const x = E.x + dir.x * d + u * 1.5, y = E.y + 1.0 + dir.y * d + u * 7.0 * (0.4 + rand(21, i)), z = E.z + dir.z * d
      const g = 0.3 + 0.2 * rand(22, i) + 0.15 * u
      const alpha = Math.min(1, a * 4) * Math.pow(1 - u, 1.5) * 0.6
      this.smoke.push(x, y, z, g * 1.2, g, g * 0.82, alpha, 2.6 + 4.2 * u, rand(23, i) * 6 + u * 0.8)
    }
    // dust ring along the ground
    for (let i = 0; i < 60; i++) {
      const a = age - 0.05
      if (a < 0 || a > 2.2) continue
      const u = a / 2.2
      const ang = i / 60 * Math.PI * 2 + rand(25, i) * 0.1
      const d = 1 + 11 * (1 - Math.pow(1 - Math.min(1, a / 0.9), 2)) * (0.8 + 0.3 * rand(26, i))
      this.smoke.push(E.x + Math.cos(ang) * d, 0.4 + u * 1.3, E.z + Math.sin(ang) * d, 0.62, 0.5, 0.38, (1 - u) * 0.45, 1.8 + 2.8 * u, rand(27, i) * 6)
    }
    // sparks / debris
    for (let i = 0; i < 220; i++) {
      const life = 0.8 + 1.2 * rand(30, i)
      if (age > life) continue
      const dir = _v.set(rand(31, i) * 2 - 1, 0.25 + rand(32, i) * 1.1, rand(33, i) * 2 - 1).normalize()
      const sp = 9 + 16 * rand(34, i)
      const drag = Math.exp(-age * 1.4)
      const dist = sp * (1 - drag) / 1.4
      const x = E.x + dir.x * dist, y = E.y + 0.9 + dir.y * dist - 4.9 * age * age, z = E.z + dir.z * dist
      if (y < 0.02) continue
      const k = 1 - age / life
      this.sparks.push(x, y, z, 1, 0.65 + 0.3 * k, 0.25 * k, k, 0.08 + 0.06 * k)
    }
    if (age < 0.12) this.flashes.push(E.x, E.y + 1.2, E.z, 1, 0.8, 0.5, 0.8 * (1 - age / 0.12), 9, 0.3)
  }
}
