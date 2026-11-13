"""closeup.py - arm close-up stills of a pairing, before vs after, Blender 5.2 Eevee on the game meshes.

  python closeup.py PARTNER BEFORE.npz AFTER.npz OUT.png [--frames f1,f2,...]   (25 fps pair npz files)
Frames default to the worst skin-candy / elbow-pole frames of BEFORE (armtwist metrics). Each frame: one camera on
the worst arm (elbow-wrist midpoint, 1.0 m away, from outside the pair), same camera for both versions.
Sheet: top row BEFORE, bottom row AFTER, labelled.
"""
import argparse, json, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

sys.dont_write_bytecode = True
import retarget as rt
import pair as pr
import preview as pv
import armtwist as at

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(os.path.dirname(HERE), "work")


def pick_frames(partner, npz, n=6):
    d = np.load(npz)
    cands = []
    for role, ident in (("lead", "ram"), ("follow", partner)):
        sk = rt.TodSkel(ident)
        G = d[role + "_G"]; P = sk.positions(G, d[role + "_BIP"])
        cd = at.skin_candy(ident, G, P)
        for S in ("L", "R"):
            f = cd[S]["frame_of_max"]
            cands.append((cd[S]["candy_deg_max"], f, role, S))
            ua, fa, ha = sk.idx[S + " UpperArm"], sk.idx[S + " Forearm"], sk.idx[S + " Hand"]
            a, b, c = P[:, ua], P[:, fa], P[:, ha]
            dd = c - a; dd /= np.linalg.norm(dd, axis=1, keepdims=True)
            e = (b - a) - dd * np.sum((b - a) * dd, 1, keepdims=True); en = np.linalg.norm(e, axis=1)
            e = e / np.maximum(en, 1e-9)[:, None]
            de = np.degrees(np.arccos(np.clip(np.sum(e[1:] * e[:-1], 1), -1, 1)))
            de[(en[1:] < 0.04) | (en[:-1] < 0.04)] = 0
            k = int(np.argmax(de))
            cands.append((de[k], k + 1, role, S))
    cands.sort(key=lambda x: -x[0])
    out, used = [], set()
    for v, f, role, S in cands:
        if all(abs(f - u) > 8 for u in used):
            out.append((int(f), role, S)); used.add(f)
        if len(out) == n:
            break
    return out


def job_for(partner, npz, picks, tag, size=(560, 420), wide=False):
    d = np.load(npz)
    sks = {"lead": rt.TodSkel("ram"), "follow": rt.TodSkel(partner)}
    glbs = {"lead": pr.GLB["ram"], "follow": pr.GLB[partner]}
    arrays, inst = {}, []
    Ps = {}
    for r in ("lead", "follow"):
        sk = sks[r]
        G = d[r + "_G"][[f for f, _, _ in picks]]
        P = sk.positions(G, d[r + "_BIP"][[f for f, _, _ in picks]])
        Ps[r] = P
        o = pv.glb_order(glbs[r], sk)
        arrays[r] = pv.mats(G, P)[:, o]
        inst.append({"name": r, "glb": glbs[r], "key": r})
    locs, looks = [], []
    for k, (f, role, S) in enumerate(picks):
        sk = sks[role]; P = Ps[role][k]
        el, wr = P[sk.idx[S + " Forearm"]], P[sk.idx[S + " Hand"]]
        look = 0.5 * (el + wr)
        cen = 0.5 * (Ps["lead"][k, 1] + Ps["follow"][k, 1]) if wide != "follow" else Ps["follow"][k, 1]
        out = look - cen; out[1] = 0
        if np.linalg.norm(out) < 0.05:
            out = np.array([1.0, 0, 0])
        out /= np.linalg.norm(out)
        ax = wr - el; ax /= np.linalg.norm(ax)
        side = np.cross(ax, [0, 1.0, 0])
        if np.linalg.norm(side) < 0.2:
            side = out
        side /= np.linalg.norm(side)
        if np.dot(side, out) < 0:
            side = -side
        cam = look + 0.95 * (0.6 * side + 0.4 * out) / np.linalg.norm(0.6 * side + 0.4 * out) + [0, 0.25, 0]
        if wide == "follow":           # the partner alone, from his front-left, chest height
            Pf = Ps["follow"][k]; skf = sks["follow"]
            Gf_ = np.load(npz)["follow_G"][f]
            fw = Gf_[skf.idx["Bip"]] @ skf.Gb[skf.idx["Bip"]].T @ np.array([0, 0, 1.0]); fw[1] = 0; fw /= np.linalg.norm(fw)
            look = Pf[skf.idx["Spine2"]] + [0, 0.05, 0]
            sd = np.cross([0, 1.0, 0], fw)
            cam = look + 1.9 * (0.85 * fw + 0.5 * sd) / np.linalg.norm(0.85 * fw + 0.5 * sd) + [0, 0.15, 0]
        elif wide:                     # whole pair, front three-quarter of Ram's start facing
            look = cen * [1, 0, 1] + [0, pr.GROUND + 0.95, 0]
            az = np.radians(-65)
            cam = look + 3.6 * np.array([np.sin(az), 0, np.cos(az)]) + [0, 0.45, 0]
        locs.append((pv.CONV @ cam).tolist()); looks.append((pv.CONV @ look).tolist())
    out_dir = os.path.join(WORK, "closeup_%s_%s" % (partner, tag))
    os.makedirs(out_dir, exist_ok=True)
    for fn in os.listdir(out_dir):
        if fn.endswith(".png"):
            os.remove(os.path.join(out_dir, fn))
    jnpz = os.path.join(WORK, "closeup_%s_%s.npz" % (partner, tag))
    np.savez(jnpz, **arrays)
    C = 0.5 * (Ps["lead"][0, 1] + Ps["follow"][0, 1])
    job = {"npz": jnpz, "out_dir": out_dir, "width": size[0], "height": size[1], "fps": 30, "frames": len(picks),
           "first": 0, "last": len(picks), "step": 1, "src_dir": HERE, "instances": inst,
           "camera": {"location": locs[0], "locations": locs, "look": looks, "lens": 40,
                      "center": (pv.CONV @ (C * [1, 0, 1])).tolist(), "yaw_deg": 0.0},
           "samples": 16, "ground_z": pr.GROUND}
    jp = os.path.join(WORK, "closeup_%s_%s.json" % (partner, tag))
    json.dump(job, open(jp, "w"))
    return jp, out_dir


def render(jp, log, timeout=900):
    with open(log, "w") as lf:
        subprocess.run([pv.BLENDER, "-b", "--factory-startup", "--python", os.path.join(HERE, "bl_preview.py"), "--", jp],
                       stdout=lf, stderr=subprocess.STDOUT, check=False, timeout=timeout)


def sheet(partner, picks, dirs, out):
    ims = {t: [Image.open(os.path.join(dirs[t], "f%05d.png" % k)).convert("RGB") for k in range(len(picks))] for t in dirs}
    w, h = ims["before"][0].size
    S = Image.new("RGB", (w * len(picks), h * 2 + 24), (20, 20, 20))
    dr = ImageDraw.Draw(S)
    dr.text((6, 5), "%s: arm close-ups, top = before, bottom = after (hinge elbow + 1/3, 2/3 roll share)" % partner,
            fill=(255, 255, 255))
    for row, t in enumerate(("before", "after")):
        for k, im in enumerate(ims[t]):
            x, y = k * w, 24 + row * h
            S.paste(im, (x, y))
            f, role, Sd = picks[k]
            lab = "%s  %s %s arm  %.2f s" % (t, "Ram" if role == "lead" else partner, Sd, f / 25.0)
            dr.rectangle([x, y, x + 8 + 6 * len(lab), y + 15], fill=(0, 0, 0))
            dr.text((x + 4, y + 2), lab, fill=(255, 255, 255))
    S.save(out)
    return out


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("partner"); ap.add_argument("before"); ap.add_argument("after"); ap.add_argument("out")
    ap.add_argument("--frames", default=None)
    a = ap.parse_args()
    if a.frames:
        picks = []
        for tok in a.frames.split(","):
            f, role, S = tok.split(":")
            picks.append((int(f), role, S))
    else:
        picks = pick_frames(a.partner, a.before)
    print("frames", picks, flush=True)
    dirs = {}
    for tag, npz in (("before", a.before), ("after", a.after)):
        jp, od = job_for(a.partner, npz, picks, tag)
        render(jp, os.path.join(WORK, "closeup_%s_%s.log" % (a.partner, tag)))
        dirs[tag] = od
    print(sheet(a.partner, picks, dirs, a.out))
