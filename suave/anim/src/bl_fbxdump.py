"""bl_fbxdump.py - (Blender) dump a Mixamo-rig FBX animation as per-frame bone world matrices.

  blender -b --factory-startup --python bl_fbxdump.py -- IN.fbx OUT.npz
OUT.npz: names (bones), parents, fps, rest (n,4,4) armature-space rest matrices (bone head, Blender bone axes),
world (F,n,4,4) per frame (pose bone matrix in world space, scene units = metres after import scale).
Blender world is z-up; the pose matrices include the armature object's transform.
"""
import sys
import bpy
import numpy as np

argv = sys.argv[sys.argv.index("--") + 1:]
src, out = argv[0], argv[1]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=src, automatic_bone_orientation=False)
sc = bpy.context.scene
arm = next(o for o in sc.objects if o.type == "ARMATURE")
act = arm.animation_data.action if arm.animation_data else None
f0, f1 = (int(act.frame_range[0]), int(act.frame_range[1])) if act else (sc.frame_start, sc.frame_end)
names = [b.name for b in arm.data.bones]
parents = [names.index(b.parent.name) if b.parent else -1 for b in arm.data.bones]
mw = np.array(arm.matrix_world)
rest = np.array([mw @ np.array(b.matrix_local) for b in arm.data.bones])
W = []
for f in range(f0, f1 + 1):
    sc.frame_set(f)
    W.append([mw @ np.array(pb.matrix) for pb in (arm.pose.bones[n] for n in names)])
np.savez_compressed(out, names=np.array(names), parents=np.array(parents), fps=sc.render.fps / sc.render.fps_base,
                    rest=rest, world=np.array(W), frames=np.array([f0, f1]))
print("FBXDUMP", src, len(names), "bones", f1 - f0 + 1, "frames", sc.render.fps, names[:6])
