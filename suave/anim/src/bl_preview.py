"""bl_preview.py - (Blender 5.2) render a Suave preview: game character meshes (staged glbs) skinned on the CPU with
joint matrices computed by preview.py, a ring of soldiers, front three-quarter camera, even light.

  blender -b --factory-startup --python bl_preview.py -- JOB.json
JOB.json: {"npz": joints file, "out_dir": frames dir, "width", "height", "fps", "frames": F, "camera": {...},
           "instances": [{"name", "glb", "key"}...], "src_dir": folder of glbio.py}
npz[key]: (F, J, 4, 4) joint world matrices in TOD model space (glb joint order), already placed in the scene.
TOD space is left-handed; the scene converts with (x, y, z) -> (-x, -z, y) (mirror to true handedness, y-up -> z-up)
and reverses the triangle winding, so the characters look the way the game shows them.
"""
import json, math, os, sys
import bpy
import numpy as np

argv = sys.argv[sys.argv.index("--") + 1:]
job = json.load(open(argv[0]))
sys.path.insert(0, job["src_dir"])
import glbio

CONV = np.array([[-1, 0, 0], [0, 0, -1], [0, 1, 0]], dtype=np.float64)    # TOD -> Blender (det -1)
bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
data = np.load(job["npz"])
texdir = os.path.join(job["out_dir"], "_tex")
os.makedirs(texdir, exist_ok=True)

_glb_cache = {}


def load_glb(path):
    if path in _glb_cache:
        return _glb_cache[path]
    gl, b = glbio.read_glb(path)
    skin = gl["skins"][0]
    jn = [gl["nodes"][j].get("name", "") for j in skin["joints"]]
    ibm = glbio.accessor(gl, b, skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)  # column-major
    mats = []
    for i, m in enumerate(gl.get("materials", [])):
        mat = bpy.data.materials.new("%s_m%d" % (os.path.basename(path), i))
        mat.use_nodes = True
        nt = mat.node_tree
        bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
        bsdf.inputs["Metallic"].default_value = 0.0
        bsdf.inputs["Roughness"].default_value = 0.8
        if "Specular IOR Level" in bsdf.inputs:
            bsdf.inputs["Specular IOR Level"].default_value = 0.25
        pbr = m.get("pbrMetallicRoughness", {})
        if "baseColorTexture" in pbr:
            tex = gl["textures"][pbr["baseColorTexture"]["index"]]
            img = gl["images"][tex["source"]]
            bv = gl["bufferViews"][img["bufferView"]]
            raw = b[bv.get("byteOffset", 0): bv.get("byteOffset", 0) + bv["byteLength"]]
            ext = ".png" if "png" in img.get("mimeType", "png") else ".jpg"
            fn = os.path.join(texdir, "%s_%d%s" % (os.path.basename(path).replace(".glb", ""), tex["source"], ext))
            open(fn, "wb").write(raw)
            im = bpy.data.images.load(fn)
            tn = nt.nodes.new("ShaderNodeTexImage")
            tn.image = im
            nt.links.new(tn.outputs["Color"], bsdf.inputs["Base Color"])
            if m.get("alphaMode", "OPAQUE") != "OPAQUE":
                nt.links.new(tn.outputs["Alpha"], bsdf.inputs["Alpha"])
        elif "baseColorFactor" in pbr:
            bsdf.inputs["Base Color"].default_value = pbr["baseColorFactor"]
        mats.append(mat)
    prims = []
    for mesh in gl["meshes"]:
        for pr in mesh["primitives"]:
            at = pr["attributes"]
            P = glbio.accessor(gl, b, at["POSITION"]).astype(np.float64)
            uv = glbio.accessor(gl, b, at["TEXCOORD_0"]).astype(np.float64) if "TEXCOORD_0" in at else None
            J = glbio.accessor(gl, b, at["JOINTS_0"]).astype(np.int64)
            W = glbio.accessor(gl, b, at["WEIGHTS_0"]).astype(np.float64)
            W = W / np.maximum(W.sum(1, keepdims=True), 1e-9)
            I = glbio.accessor(gl, b, pr["indices"]).astype(np.int64).reshape(-1, 3)
            prims.append({"P": P, "uv": uv, "J": J, "W": W, "I": I, "mat": mats[pr["material"]] if "material" in pr else None})
    _glb_cache[path] = (jn, ibm, prims)
    return _glb_cache[path]


instances = []
for inst in job["instances"]:
    jn, ibm, prims = load_glb(inst["glb"])
    objs = []
    for k, p in enumerate(prims):
        me = bpy.data.meshes.new("%s_%d" % (inst["name"], k))
        I = p["I"][:, ::-1]                                   # winding reversed (the mirror flips it)
        V = (p["P"] @ CONV.T)
        me.from_pydata(V.tolist(), [], I.tolist())
        if p["uv"] is not None:
            uvl = me.uv_layers.new(name="UV")
            loops = I.reshape(-1)
            uvs = p["uv"][loops].copy()
            uvs[:, 1] = 1.0 - uvs[:, 1]                       # glTF v down -> Blender v up
            uvl.data.foreach_set("uv", uvs.reshape(-1).astype(np.float32))
        if p["mat"] is not None:
            me.materials.append(p["mat"])
        for poly in me.polygons:
            poly.use_smooth = True
        ob = bpy.data.objects.new(me.name, me)
        sc.collection.objects.link(ob)
        objs.append((ob, p))
    instances.append({"key": inst["key"], "ibm": ibm, "objs": objs})

# ground, world, lights, camera
cam_cfg = job["camera"]
bpy.ops.mesh.primitive_circle_add(vertices=96, radius=job.get("ground_radius", 14.0), fill_type="NGON",
                                  location=(cam_cfg["center"][0], cam_cfg["center"][1], job["ground_z"]))
ground = bpy.context.active_object
gm = bpy.data.materials.new("ground"); gm.use_nodes = True
gb = next(n for n in gm.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
gb.inputs["Base Color"].default_value = (0.42, 0.37, 0.31, 1); gb.inputs["Roughness"].default_value = 0.95
ground.data.materials.append(gm)
ring = job.get("ring_marks")
if ring:
    bpy.ops.mesh.primitive_torus_add(major_radius=ring["radius"], minor_radius=0.02,
                                     location=(ring["center"][0], ring["center"][1], job["ground_z"] + 0.002))
    t = bpy.context.active_object; t.scale[2] = 0.05
    tm = bpy.data.materials.new("ring"); tm.use_nodes = True
    tb = next(n for n in tm.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    tb.inputs["Base Color"].default_value = (0.55, 0.5, 0.42, 1)
    t.data.materials.append(tm)
world = bpy.data.worlds.new("w"); sc.world = world; world.use_nodes = True
bg = world.node_tree.nodes["Background"]; bg.inputs[0].default_value = (0.55, 0.6, 0.66, 1); bg.inputs[1].default_value = 0.8


def light(name, energy, rot, angle=25):
    d = bpy.data.lights.new(name, "SUN"); d.energy = energy; d.angle = math.radians(angle)
    o = bpy.data.objects.new(name, d); o.rotation_euler = [math.radians(a) for a in rot]
    sc.collection.objects.link(o)


yaw_cam = cam_cfg["yaw_deg"]
light("key", 2.6, (50, 0, yaw_cam - 40))
light("fill", 1.1, (65, 0, yaw_cam + 50))
light("rim", 1.0, (-55, 0, yaw_cam))
cd = bpy.data.cameras.new("cam"); cd.lens = cam_cfg.get("lens", 40); cd.sensor_width = 36
cam = bpy.data.objects.new("cam", cd); sc.collection.objects.link(cam); sc.camera = cam
cam.location = cam_cfg["location"]
tgt = bpy.data.objects.new("tgt", None); sc.collection.objects.link(tgt)
con = cam.constraints.new("TRACK_TO"); con.target = tgt; con.track_axis = "TRACK_NEGATIVE_Z"; con.up_axis = "UP_Y"
look = np.array(cam_cfg["look"])                      # (F, 3) Blender coords per frame

sc.render.engine = "BLENDER_EEVEE"
try:
    sc.eevee.taa_render_samples = job.get("samples", 32)
except Exception:
    pass
try:
    sc.eevee.use_shadows = True
except Exception:
    pass
sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = job["width"], job["height"], 100
sc.render.image_settings.file_format = "PNG"
sc.view_settings.view_transform = "Standard"
sc.view_settings.look = "None"

F = job["frames"]
first = job.get("first", 0)
last = job.get("last", F)
step = job.get("step", 1)
for f in range(first, last, step):
    for inst in instances:
        M = data[inst["key"]][f]                                  # (J,4,4) joint world, TOD
        S = M @ inst["ibm"]                                      # skin matrices
        for ob, p in inst["objs"]:
            V = np.concatenate([p["P"], np.ones((len(p["P"]), 1))], 1)
            out = np.zeros((len(V), 3))
            for k in range(4):
                Sk = S[p["J"][:, k]]                             # (n,4,4)
                out += p["W"][:, k:k + 1] * np.einsum("nab,nb->na", Sk[:, :3, :], V)
            Vb = out @ CONV.T
            ob.data.vertices.foreach_set("co", Vb.reshape(-1).astype(np.float32))
            ob.data.update()
    if "locations" in cam_cfg:                                   # per-frame camera (close-ups)
        cam.location = cam_cfg["locations"][f]
    if "lenses" in cam_cfg:
        cd.lens = cam_cfg["lenses"][f]
    tgt.location = look[f].tolist()
    sc.render.filepath = os.path.join(job["out_dir"], "f%05d.png" % f)
    bpy.ops.render.render(write_still=True)
    print("PREVIEW_FRAME", f, flush=True)
print("PREVIEW_DONE")
