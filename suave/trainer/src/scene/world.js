// The training ground: dusty sand, adobe walls, crates and barrels, a golden-hour sun and sky.
import * as THREE from 'three'
import { Sky } from 'three/examples/jsm/objects/Sky.js'
import { GLTFLoader } from 'three/examples/jsm/loaders/GLTFLoader.js'

export const SUN_DIR = new THREE.Vector3().setFromSphericalCoords(1, THREE.MathUtils.degToRad(90 - 17), THREE.MathUtils.degToRad(-128))

function rng(seed) {
  let s = seed >>> 0
  return () => { s = (s + 0x6D2B79F5) >>> 0; let t = s; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296 }
}

// canvas-painted adobe plaster: warm ochre with blotches, cracks and a darker skirt
function adobeTexture() {
  const c = document.createElement('canvas'); c.width = c.height = 512
  const g = c.getContext('2d')
  const r = rng(7)
  g.fillStyle = '#c99a6b'; g.fillRect(0, 0, 512, 512)
  for (let i = 0; i < 2600; i++) {
    const x = r() * 512, y = r() * 512, s = 2 + r() * 26
    const l = 52 + r() * 22
    g.fillStyle = `hsla(${24 + r() * 10}, ${38 + r() * 18}%, ${l}%, ${0.05 + r() * 0.08})`
    g.beginPath(); g.ellipse(x, y, s, s * (0.4 + r()), r() * 3, 0, Math.PI * 2); g.fill()
  }
  g.strokeStyle = 'rgba(80,50,30,0.28)'; g.lineWidth = 1
  for (let i = 0; i < 14; i++) {
    let x = r() * 512, y = r() * 512
    g.beginPath(); g.moveTo(x, y)
    for (let k = 0; k < 6; k++) { x += (r() - 0.5) * 40; y += r() * 30; g.lineTo(x, y) }
    g.stroke()
  }
  const grad = g.createLinearGradient(0, 380, 0, 512)
  grad.addColorStop(0, 'rgba(90,55,30,0)'); grad.addColorStop(1, 'rgba(90,55,30,0.35)')
  g.fillStyle = grad; g.fillRect(0, 380, 512, 132)
  const t = new THREE.CanvasTexture(c)
  t.colorSpace = THREE.SRGBColorSpace
  t.wrapS = t.wrapT = THREE.RepeatWrapping
  return t
}

// soft painted circle used for the training ring marking and the scorch decal
export function radialTexture(stops, size = 256) {
  const c = document.createElement('canvas'); c.width = c.height = size
  const g = c.getContext('2d')
  const grad = g.createRadialGradient(size / 2, size / 2, 0, size / 2, size / 2, size / 2)
  for (const [o, col] of stops) grad.addColorStop(o, col)
  g.fillStyle = grad; g.fillRect(0, 0, size, size)
  const t = new THREE.CanvasTexture(c); t.colorSpace = THREE.SRGBColorSpace
  return t
}

export function createRenderer(canvas) {
  const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, preserveDrawingBuffer: true, powerPreference: 'high-performance' })
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2))
  renderer.toneMapping = THREE.ACESFilmicToneMapping
  renderer.toneMappingExposure = 0.92
  renderer.outputColorSpace = THREE.SRGBColorSpace
  renderer.shadowMap.enabled = true
  renderer.shadowMap.type = THREE.PCFSoftShadowMap
  return renderer
}

export async function createWorld(renderer) {
  const scene = new THREE.Scene()
  const haze = new THREE.Color('#e9c99a')
  scene.fog = new THREE.Fog(haze, 70, 430)

  // sky
  const sky = new Sky()
  sky.scale.setScalar(4000)
  const u = sky.material.uniforms
  u.turbidity.value = 7.5
  u.rayleigh.value = 1.6
  u.mieCoefficient.value = 0.006
  u.mieDirectionalG.value = 0.86
  u.sunPosition.value.copy(SUN_DIR)
  scene.add(sky)
  // environment lighting from the same sky (soft warm fill on the characters)
  const pmrem = new THREE.PMREMGenerator(renderer)
  const envScene = new THREE.Scene()
  const sky2 = new Sky(); sky2.scale.setScalar(1000)
  Object.assign(sky2.material.uniforms.turbidity, { value: 7.5 })
  sky2.material.uniforms.rayleigh.value = 1.6
  sky2.material.uniforms.mieCoefficient.value = 0.006
  sky2.material.uniforms.mieDirectionalG.value = 0.86
  sky2.material.uniforms.sunPosition.value.copy(SUN_DIR)
  envScene.add(sky2)
  scene.environment = pmrem.fromScene(envScene, 0.02).texture
  scene.environmentIntensity = 0.55

  // lights: low warm sun + sky/ground bounce
  const sun = new THREE.DirectionalLight('#ffbf78', 3.4)
  sun.position.copy(SUN_DIR).multiplyScalar(60)
  sun.castShadow = true
  sun.shadow.mapSize.set(2048, 2048)
  const sc = sun.shadow.camera
  sc.left = -24; sc.right = 24; sc.top = 24; sc.bottom = -24; sc.near = 1; sc.far = 160
  sun.shadow.bias = -0.0004
  sun.shadow.normalBias = 0.03
  scene.add(sun, sun.target)
  const hemi = new THREE.HemisphereLight('#a9c4f0', '#9a6a40', 0.7)
  scene.add(hemi)

  // ground
  const tl = new THREE.TextureLoader()
  const load = (p, srgb) => { const t = tl.load(p); t.wrapS = t.wrapT = THREE.RepeatWrapping; t.repeat.set(70, 70); t.anisotropy = 8; if (srgb) t.colorSpace = THREE.SRGBColorSpace; return t }
  const groundMat = new THREE.MeshStandardMaterial({
    map: load('/env/ground/gravelly_sand_diff_1k.jpg', true),
    normalMap: load('/env/ground/gravelly_sand_nor_gl_1k.jpg'),
    roughnessMap: load('/env/ground/gravelly_sand_arm_1k.jpg'),
    aoMap: load('/env/ground/gravelly_sand_arm_1k.jpg'),
    color: '#f2e2cf', roughness: 1, metalness: 0,
  })
  groundMat.normalScale.set(0.9, 0.9)
  const groundGeo = new THREE.PlaneGeometry(280, 280, 1, 1)
  groundGeo.rotateX(-Math.PI / 2)
  groundGeo.setAttribute('uv1', groundGeo.attributes.uv)
  const ground = new THREE.Mesh(groundGeo, groundMat)
  ground.receiveShadow = true
  scene.add(ground)

  // painted training circle (faded lime) at the origin, where the trainer stages the move
  const ringTex = radialTexture([[0, 'rgba(255,255,255,0)'], [0.9, 'rgba(255,255,255,0)'], [0.93, 'rgba(250,240,220,0.55)'], [0.97, 'rgba(250,240,220,0.5)'], [1, 'rgba(255,255,255,0)']], 512)
  const marking = new THREE.Mesh(new THREE.PlaneGeometry(14.6, 14.6).rotateX(-Math.PI / 2),
    new THREE.MeshStandardMaterial({ map: ringTex, transparent: true, depthWrite: false, roughness: 1, color: '#fff3dc', polygonOffset: true, polygonOffsetFactor: -2 }))
  marking.position.y = 0.01
  marking.receiveShadow = true
  scene.add(marking)

  const props = new THREE.Group()
  scene.add(props)
  // adobe walls around the yard (open towards the camera side, -Z)
  const adobe = adobeTexture()
  const wallMat = new THREE.MeshStandardMaterial({ map: adobe, roughness: 0.95, metalness: 0, color: '#f4dcc2' })
  const capMat = new THREE.MeshStandardMaterial({ color: '#a8754c', roughness: 0.9 })
  function wall(x, z, len, rotY, h = 2.6) {
    const g = new THREE.Group()
    const geo = new THREE.BoxGeometry(len, h, 0.55)
    const uv = geo.attributes.uv
    for (let i = 0; i < uv.count; i++) uv.setXY(i, uv.getX(i) * len / 4, uv.getY(i) * h / 4)
    const m = new THREE.Mesh(geo, wallMat); m.position.y = h / 2; m.castShadow = m.receiveShadow = true
    const cap = new THREE.Mesh(new THREE.BoxGeometry(len + 0.12, 0.14, 0.7), capMat); cap.position.y = h + 0.07; cap.castShadow = true
    g.add(m, cap); g.position.set(x, 0, z); g.rotation.y = rotY
    props.add(g)
    return g
  }
  wall(0, 30, 34, 0, 3.2)
  wall(-26, 14, 24, Math.PI / 2 - 0.25, 2.8)
  wall(27, 16, 22, -Math.PI / 2 + 0.2, 3.0)
  wall(-20, -24, 12, 0.5, 2.4)
  wall(23, -22, 10, -0.6, 2.4)
  // a little adobe hut in the back corner
  const hut = new THREE.Group()
  const hutBody = new THREE.Mesh(new THREE.BoxGeometry(7, 3.6, 5.5), wallMat); hutBody.position.y = 1.8
  hutBody.castShadow = hutBody.receiveShadow = true
  const roof = new THREE.Mesh(new THREE.BoxGeometry(7.8, 0.25, 6.3), new THREE.MeshStandardMaterial({ color: '#7d5132', roughness: 0.9 }))
  roof.position.y = 3.72; roof.castShadow = true
  const door = new THREE.Mesh(new THREE.PlaneGeometry(1.3, 2.2), new THREE.MeshStandardMaterial({ color: '#3d2617', roughness: 1 }))
  door.position.set(0.8, 1.1, -2.76); door.rotation.y = Math.PI
  hut.add(hutBody, roof, door)
  for (let i = -2; i <= 2; i++) {  // vigas poking out
    const v = new THREE.Mesh(new THREE.CylinderGeometry(0.09, 0.09, 0.9, 8), capMat)
    v.rotation.x = Math.PI / 2; v.position.set(i * 1.5, 3.35, -3.05); v.castShadow = true; hut.add(v)
  }
  hut.position.set(17, 0, 22); hut.rotation.y = -0.35
  props.add(hut)

  // crates and barrels (Poly Haven, CC0) as cover for the outside shooters and set dressing
  const gl = new GLTFLoader()
  const loadProp = async name => {
    const g = await gl.loadAsync(`/env/${name}/${name}.gltf`)
    g.scene.traverse(o => { if (o.isMesh) { o.castShadow = o.receiveShadow = true; if (o.material.map) o.material.map.colorSpace = THREE.SRGBColorSpace } })
    return g.scene
  }
  const [crate, mcrate, barrel] = await Promise.all([loadProp('wooden_crate_01'), loadProp('old_military_crate'), loadProp('Barrel_01')])
  const placements = [
    [crate, 13.5, 16.5, 0.3, 1.6], [crate, 14.6, 16.0, 1.1, 1.6], [crate, 14.0, 16.3, 0.7, 1.6, 1.0],
    [mcrate, -16.5, 12.5, 0.4, 1.4], [mcrate, -17.2, 13.6, -0.3, 1.4],
    [barrel, -15.5, 11.0, 0, 1.25], [barrel, 15.8, 14.6, 0, 1.25],
    [crate, -3.5, 21.8, 0.1, 1.6], [mcrate, -2.2, 21.5, 0.2, 1.4],
    [barrel, 21, -9, 0, 1.25], [barrel, 21.9, -8.3, 0, 1.25], [mcrate, -21.5, -7, 0.8, 1.4],
    [crate, 8, 26, 0.2, 1.6], [barrel, -9, 26.5, 0, 1.25], [barrel, 2.5, 21.2, 0, 1.25],
  ]
  for (const [src, x, z, ry, s, y = 0] of placements) {
    const o = src.clone()
    o.scale.setScalar(s); o.position.set(x, y, z); o.rotation.y = ry
    props.add(o)
  }
  // fence posts with rope marking the yard edge on the camera side
  const postMat = new THREE.MeshStandardMaterial({ color: '#6b4a30', roughness: 0.95 })
  for (let i = 0; i < 14; i++) {
    const x = -24 + i * 3.7
    const p = new THREE.Mesh(new THREE.CylinderGeometry(0.07, 0.09, 1.2, 7), postMat)
    p.position.set(x, 0.6, -30 + Math.sin(i * 1.7) * 0.3); p.rotation.z = Math.sin(i * 2.3) * 0.05; p.castShadow = true
    props.add(p)
  }
  // a ring of distant mesas and ridges, mostly swallowed by the haze
  const ridgeGeo = new THREE.CylinderGeometry(330, 340, 1, 240, 1, true)
  const rp = ridgeGeo.attributes.position
  const r = rng(11)
  const bumps = Array.from({ length: 11 }, () => [r() * Math.PI * 2, 0.08 + r() * 0.25, 8 + r() * 16])
  for (let i = 0; i < rp.count; i++) {
    if (rp.getY(i) > 0) {
      const a = Math.atan2(rp.getZ(i), rp.getX(i))
      let h = 6 + 4 * Math.sin(a * 7) + 3 * Math.sin(a * 17 + 1)
      for (const [c, w, hh] of bumps) { let d = Math.abs(a - c); d = Math.min(d, Math.PI * 2 - d); h += hh * Math.max(0, 1 - Math.pow(d / w, 6)) }
      rp.setY(i, Math.max(3, h))
    } else rp.setY(i, -2)
  }
  ridgeGeo.computeVertexNormals()
  const ridge = new THREE.Mesh(ridgeGeo, new THREE.MeshStandardMaterial({ color: '#b07850', roughness: 1, side: THREE.BackSide, fog: true }))
  scene.add(ridge)
  // a few dry shrubs (cheap cross billboards)
  const shrubTex = (() => {
    const c = document.createElement('canvas'); c.width = 128; c.height = 128
    const g = c.getContext('2d'); const rr = rng(3)
    g.strokeStyle = '#6e5232'
    for (let i = 0; i < 70; i++) {
      g.lineWidth = 1 + rr() * 1.5
      g.beginPath(); g.moveTo(64 + (rr() - 0.5) * 10, 128)
      g.quadraticCurveTo(64 + (rr() - 0.5) * 80, 70, 64 + (rr() - 0.5) * 120, 20 + rr() * 60); g.stroke()
    }
    const t = new THREE.CanvasTexture(c); t.colorSpace = THREE.SRGBColorSpace; return t
  })()
  const shrubMat = new THREE.MeshStandardMaterial({ map: shrubTex, transparent: true, alphaTest: 0.3, side: THREE.DoubleSide, roughness: 1 })
  for (let i = 0; i < 26; i++) {
    const a = r() * Math.PI * 2, d = 18 + r() * 24
    const s = 0.8 + r() * 1.0
    const g = new THREE.Group()
    for (let k = 0; k < 2; k++) {
      const pl = new THREE.Mesh(new THREE.PlaneGeometry(s * 1.6, s), shrubMat)
      pl.position.y = s / 2; pl.rotation.y = k * Math.PI / 2; pl.castShadow = true; g.add(pl)
    }
    g.position.set(Math.cos(a) * d, 0, Math.sin(a) * d)
    scene.add(g)
  }

  return { scene, sun, hemi, ground, marking, props, sky }
}

// keep the shadow frustum centred on the action
export function followSun(world, center) {
  world.sun.target.position.copy(center)
  world.sun.position.copy(center).addScaledVector(SUN_DIR, 60)
  world.sun.target.updateMatrixWorld()
}
