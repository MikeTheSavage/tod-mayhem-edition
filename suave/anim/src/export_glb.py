"""export_glb.py - skinned glTF (.glb) of every Suave clip on its game character, for the web trainer (suave/trainer).

  python export_glb.py [--partners soldier,civil,dea_taskforce] [--circle]
Each glb = the staged character glb (game mesh + game textures, from the author's export stage folder, read only)
with its two old animations replaced by ONE animation: the clip at 30 fps, every joint's rotation, Bip translation
(the root motion: the whole dance travel lives in Bip; GamePivot stays put).
Space = TOD model space as in the staged glbs (+y up, +z = Ram's facing at the dance start; TOD data is left-handed,
so a glTF viewer shows the characters mirrored; scale the scene root by x -1 to see them as in the game).
Pair clips share one frame: put Ram's root and the partner's root at the same origin (no offset, no rotation).
Writes ../export/<clip>.glb and ../export/manifest.json.
"""
import argparse, json, os, struct, sys
import numpy as np
from scipy.spatial.transform import Rotation as Rot

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
import glbio
import retarget as rt
import pair as pr
import preview as pv

EXP = os.path.join(ROOT, "export")
FACE = ("Chin", "ChinR", "ChinL", "Eyelid", "EyeBR", "EyeBL", "EyeR", "EyeL")
SHORT = {"soldier": "soldier", "civil": "civil", "dea_taskforce": "dea"}
FPS = 30


def write_anim_glb(src_glb, skel, G, BIP, out, anim_name, dt=1.0 / FPS):
    gl, binb = glbio.read_glb(src_glb)
    binb = bytearray(binb)
    gl.pop("animations", None)
    L = skel.local(G)
    F = len(G)
    nodes = gl["nodes"]
    name_to_node = {n.get("name", ""): i for i, n in enumerate(nodes)}

    def add_view(arr, target=None):
        while len(binb) % 4:
            binb.append(0)
        off = len(binb)
        b = np.ascontiguousarray(arr, dtype=np.float32).tobytes()
        binb.extend(b)
        gl["bufferViews"].append({"buffer": 0, "byteOffset": off, "byteLength": len(b)})
        return len(gl["bufferViews"]) - 1

    def add_acc(arr, typ):
        arr = np.asarray(arr, dtype=np.float32)
        bv = add_view(arr)
        acc = {"bufferView": bv, "componentType": 5126, "count": int(arr.shape[0]), "type": typ}
        if typ == "SCALAR":
            acc["min"] = [float(arr.min())]; acc["max"] = [float(arr.max())]
        gl["accessors"].append(acc)
        return len(gl["accessors"]) - 1

    t_acc = add_acc(np.arange(F) * dt, "SCALAR")          # dt = beat-exact frame step (about 1/30 s)
    samplers, channels = [], []
    for i, nm in enumerate(skel.names):
        node = name_to_node.get("bone0" if nm == "" else nm)
        if node is None or nm in ("",) or nm in FACE:     # face bones: left to the facial layer
            continue
        q = Rot.from_matrix(L[:, i]).as_quat()                      # glTF rotation = standard quat of the local matrix
        for f in range(1, F):
            if np.dot(q[f], q[f - 1]) < 0:
                q[f] = -q[f]
        samplers.append({"input": t_acc, "output": add_acc(q, "VEC4"), "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": "rotation"}})
        if nm == "Bip":
            samplers.append({"input": t_acc, "output": add_acc(BIP, "VEC3"), "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": "translation"}})
    gl["animations"] = [{"name": anim_name, "samplers": samplers, "channels": channels}]
    gl["buffers"][0]["byteLength"] = len(binb)
    gl["asset"]["generator"] = "suave/anim export_glb (from tod_char_textured stage glb)"
    glbio.write_glb(out, gl, bytes(binb))
    return F


def verify_glb(path, skel, P_intent):
    """re-read the glb, FK its first animation through the glb's own node tree, compare joint positions"""
    gl, b = glbio.read_glb(path)
    an = gl["animations"][0]
    nodes = gl["nodes"]
    F = len(P_intent)
    rot = {}; tr = {}
    for ch in an["channels"]:
        s = an["samplers"][ch["sampler"]]
        v = glbio.accessor(gl, b, s["output"]).astype(np.float64)
        (rot if ch["target"]["path"] == "rotation" else tr)[ch["target"]["node"]] = v
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent[c] = i
    W = {}

    def world(i):
        if i in W:
            return W[i]
        n = nodes[i]
        R = Rot.from_quat(rot[i]).as_matrix() if i in rot else np.tile(Rot.from_quat(n.get("rotation", [0, 0, 0, 1])).as_matrix(), (F, 1, 1))
        t = tr[i] if i in tr else np.tile(np.array(n.get("translation", [0, 0, 0]), float), (F, 1))
        M = np.zeros((F, 4, 4)); M[:, :3, :3] = R; M[:, :3, 3] = t; M[:, 3, 3] = 1
        if i in parent:
            M = world(parent[i]) @ M
        W[i] = M
        return M
    err = 0.0
    for i, n in enumerate(nodes):
        nm = n.get("name")
        if nm in skel.idx and nm not in ("GamePivot",) and "mesh" not in n:
            err = max(err, float(np.linalg.norm(world(i)[:, :3, 3] - P_intent[:, skel.idx[nm]], axis=1).max()))
    return round(1000 * err, 3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--partners", default="soldier,civil,dea_taskforce")
    ap.add_argument("--circle", action="store_true")
    a = ap.parse_args()
    os.makedirs(EXP, exist_ok=True)
    mpath = os.path.join(EXP, "manifest.json")
    man = json.load(open(mpath)) if os.path.exists(mpath) else {}
    man.update({
        "about": "Suave loco move animations. Skinned glTF on the game characters (staged glbs = the game "
                 "meshes and textures), 30 fps, TOD model space (left-handed: mirror the root by x -1 to view as in "
                 "game), +y up, +z = Ram's facing at the dance start. Pair clips share one frame: place both roots "
                 "at the same origin. Root motion = the Bip translation track (GamePivot static).",
        "fps": FPS, "frame_step_note": "frames are spaced duration_s / (frames - 1) (beat-exact, about 1/30 s); the glb sampler times are exact", "bpm": pr.BPM, "beat_s": round(pr.BEAT, 6),
        "song": {"file": "Elvis Crespo - Suavemente.mp3", "first_beat_s": 0.373,
                 "downbeats": "0.373 + 4k x beat_s (bar starts)",
                 "preview_audio_start_s": round(pv.AUDIO_START, 4),
                 "note": "clip time 0 = a downbeat; start the clip on any song downbeat (e.g. preview_audio_start_s)"},
        "source": "CoMPAS3D (Rosie-Lab), Pair5 song2 take1, 13.84-28.36 s, CC-BY-NC-4.0; circle: Rokoko free "
                  "mocap GentleSwayingDancing (stand-in)",
        "clips": man.get("clips", {})})
    import time
    man["updated"] = time.strftime("%Y-%m-%d %H:%M:%S")
    man["face_bones"] = ("not animated in the glbs (Chin, ChinR/L, Eyelid, EyeBR/L, EyeR/L stay at the bind pose): layer "
                         "the game's ram_facial_expression_* clips on them; the game clips carry the vanilla idle's "
                         "static face values like every vanilla body clip")
    man["arm_convention"] = ("game Biped arms: Forearm a pure hinge about the UpperArm's local +y (no elbow roll, "
                             "no elbow flips), the forearm roll in the Hand shared 1/3 ForeTwist, 2/3 ForeTwist1 "
                             "(measured on 390 vanilla Ram clips); loop seam blended in local space")
    for partner in [p for p in a.partners.split(",") if p]:
        d, rep = pv.get_pair(partner, FPS)
        s = SHORT[partner]
        for role, r, glb in (("lead", "lead", pr.GLB["ram"]), ("follow", "follow", pr.GLB[partner])):
            sk = rt.TodSkel("ram" if r == "lead" else partner)
            for kind in ("dance", "fo"):
                if kind == "dance":
                    name = "suave_%s_%s" % (role, s); G, B, P = d[r + "_G"], d[r + "_BIP"], d[r + "_P"]
                else:
                    name = "suave_%s_%s" % ("push" if role == "lead" else "stumble", s)
                    G, B, P = d[r + "_foG"], d[r + "_foB"], d[r + "_foP"]
                out = os.path.join(EXP, name + ".glb")
                F = write_anim_glb(glb, sk, G, B, out, name, rep["dt"])
                err = verify_glb(out, sk, P)
                dur = (F - 1) * rep["dt"]
                man["clips"][name] = {
                    "file": name + ".glb", "character": "ram" if role == "lead" else partner,
                    "model": sk.model, "stage_glb": glb,
                    "role": role if kind == "dance" else ("push" if role == "lead" else "stumble"),
                    "pairing": partner, "frames": F, "duration_s": round(dur, 5),
                    "loop": kind == "dance", "beats": round(dur / pr.BEAT, 3),
                    "release_frame": rep["release_frame"] if kind == "dance" else None,
                    "release_time_s": round(rep["release_s"], 5) if kind == "dance" else None,
                    "follow_on_starts_at_dance_frame": rep["release_frame"] if kind == "fo" else None,
                    "beat_offset_s": 0.0, "stand_in": kind == "fo", "glb_fk_check_max_err_mm": err,
                    "game_clip": "/data/characters/ram/%s.animation" % name}
                print(name, F, "frames, glb FK err %.3f mm" % err)
    # intro clips: Ram suave_intro_<p>, partner suave_grabbed_<p>
    for partner in [p for p in a.partners.split(",") if p]:
        ip = os.path.join(ROOT, "work", "intro_%s_%d.npz" % (partner, FPS))
        if not os.path.exists(ip):
            continue
        d = np.load(ip); irep = json.load(open(ip.replace(".npz", ".json")))
        s = SHORT[partner]
        for r, name, ident in (("lead", "suave_intro_" + s, "ram"), ("follow", "suave_grabbed_" + s, partner)):
            sk = rt.TodSkel(ident)
            out = os.path.join(EXP, name + ".glb")
            F = write_anim_glb(pr.GLB[ident], sk, d[r + "_G"], d[r + "_BIP"], out, name, irep["dt"])
            err = verify_glb(out, sk, d[r + "_P"])
            dur = (F - 1) * irep["dt"]
            man["clips"][name] = {
                "file": name + ".glb", "character": ident, "model": sk.model, "stage_glb": pr.GLB[ident],
                "role": "intro", "pairing": partner, "frames": F, "duration_s": round(dur, 5), "loop": False,
                "beats": round(dur / pr.BEAT, 3), "release_frame": None, "beat_offset_s": 0.0, "stand_in": False,
                "ends_on": "suave_%s_%s frame 0 (exact)" % (r, s), "then": "suave_%s_%s" % (r, s),
                "starts_at_music_beat": 0,
                "start_pose": "Ram 0.42 m behind his dance-start spot; partner 0.30 m further out, turned 110 deg away"
                              if r == "lead" else "partner 0.30 m further out than his dance-start spot, turned 110 deg away from Ram",
                "beats_story": "Ram 0-2 hands up palms out, 2-4 steps in + reaches, takes the hand on beat 4, 4-6 pulls into "
                               "the hold; partner 0-2 turns in surprise, 2-4 frozen, hand taken on beat 4, 4-6 pulled in",
                "glb_fk_check_max_err_mm": err, "game_clip": "/data/characters/ram/%s.animation" % name}
            print(name, F, "frames, glb FK err %.3f mm" % err)
    # lookdown
    for partner in [p for p in a.partners.split(",") if p]:
        lp = os.path.join(ROOT, "work", "lookdown_%s_%d.npz" % (partner, FPS))
        if not os.path.exists(lp):
            continue
        d = np.load(lp); lrep = json.load(open(lp.replace(".npz", ".json")))
        s = SHORT[partner]; name = "suave_lookdown_" + s
        sk = rt.TodSkel(partner)
        out = os.path.join(EXP, name + ".glb")
        F = write_anim_glb(pr.GLB[partner], sk, d["follow_G"], d["follow_BIP"], out, name, lrep["dt"])
        err = verify_glb(out, sk, d["follow_P"])
        dur = (F - 1) * lrep["dt"]
        man["clips"][name] = {
            "file": name + ".glb", "character": partner, "model": sk.model, "stage_glb": pr.GLB[partner],
            "role": "lookdown", "pairing": partner, "frames": F, "duration_s": round(dur, 5), "loop": False,
            "beats": round(dur / pr.BEAT, 3), "release_frame": None, "beat_offset_s": 0.0, "stand_in": False,
            "starts_from": "suave_stumble_%s last frame (exact)" % s,
            "story": "stops, head drops to the dynamite in the front of the belt (Pelvis), shoulders hunch, hands up by "
                     "the chin, palms out, frozen and trembling from 0.6 s",
            "glb_fk_check_max_err_mm": err, "game_clip": "/data/characters/ram/%s.animation" % name}
        print(name, F, "frames, glb FK err %.3f mm" % err)
    if a.circle:
        c = pv.get_circle(FPS)
        crep = json.load(open(os.path.join(ROOT, "work", "circle_%d.json" % FPS)))
        sk = rt.TodSkel("soldier")
        out = os.path.join(EXP, "suave_circle.glb")
        F = write_anim_glb(pv.SOLDIER_LOOKS[0], sk, c["G"], c["BIP"], out, "suave_circle", crep["T_s"] / (len(c["G"]) - 1))
        err = verify_glb(out, sk, c["P"])
        man["clips"]["suave_circle"] = {
            "file": "suave_circle.glb", "character": "soldier (shared biped clip: any enemy model can play it)",
            "model": sk.model, "stage_glb": pv.SOLDIER_LOOKS[0], "other_looks": pv.SOLDIER_LOOKS[1:],
            "role": "circle", "pairing": None, "frames": F, "duration_s": round(crep["T_s"], 5), "loop": True,
            "beats": crep["beats"], "release_frame": None, "beat_offset_s": 0.0, "stand_in": True,
            "sway_per_beat": True, "beats_per_sway": 1, "beats_per_cycle": crep.get("beats_per_cycle", 2),
            "sway_note": "one sway (to one side) on every beat at 124.17 BPM: a left-right cycle every 2 beats, 12 cycles per loop; feet planted",
            "ring": {"radius_m": pv.RING_R, "members": pv.RING_N, "face": "+z toward the ring centre",
                     "phase_offset_beats_per_member": 1},
            "glb_fk_check_max_err_mm": err, "game_clip": "/data/characters/ram/suave_circle.animation"}
        print("suave_circle", F, "frames, glb FK err %.3f mm" % err)
    json.dump(man, open(mpath, "w"), indent=1)
    print(mpath)


if __name__ == "__main__":
    main()
