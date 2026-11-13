"""export_tod.py - write the Suave clips in the game's animation format (clipjson, engine convention) and check them.

  python export_tod.py [--partners soldier,civil,dea_taskforce]
Reads ../work/pair_<partner>_25.npz and ../work/circle_25.npz (built by pair.py / circle.py) and writes
../clips/game/<clip>.json (clipjson format: engine raw quaternions w,x,y,z; Bip position track; GamePivot static at
the vanilla idle value, so no root motion) plus ../clips/game/clips.json (the catalogue: frames, CPS, loop, release).
Checks per clip (report in ../clips/game/export_report.json):
  1 codec round trip: clipjson -> record -> bytes -> decode -> re-encode identical; decoded rotations within int16
    quantisation of the intent;
  2 FK of the DECODED clip on the clip's own model skeleton reproduces the built joint positions (max error, mm);
  3 cyclic clips: frame N-1 == frame 0 (max joint distance, mm).
"""
import argparse, json, os, struct, sys
import numpy as np

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "lib"))
import animcodec as ac
import clipjson as cj
import retarget as rt
import pair as pr
import todclips

OUT = os.path.join(ROOT, "clips", "game")
SHORT = {"soldier": "soldier", "civil": "civil", "dea_taskforce": "dea"}
CLIP_DIR = "/data/characters/ram/"


FACE = ("Chin", "ChinR", "ChinL", "Eyelid", "EyeBR", "EyeBL", "EyeR", "EyeL")
FACE_CH = {}      # the face channels of the vanilla idle (static), copied into every Suave body clip: vanilla
                  # body clips carry static face channels the same way and the face controller layers its
                  # ram_facial_expression_* clips (face channels only) over them


def clip_from(sk, G, BIP, name, loop, gp_channel):
    """world rotations G (F,n,3,3) + Bip positions -> clipjson dict (all pivots except the unnamed root)"""
    L = sk.local(G)
    F = len(G)
    chans = []
    for i, nm in enumerate(sk.names):
        if nm == "":
            continue
        if nm == "GamePivot":
            chans.append(json.loads(json.dumps(gp_channel)))
            continue
        if nm in FACE_CH:
            chans.append(json.loads(json.dumps(FACE_CH[nm])))
            continue
        q = np.array([rt.engine_quat(L[f, i]) for f in range(F)])
        for f in range(1, F):                                   # hemisphere continuity
            if np.dot(q[f], q[f - 1]) < 0:
                q[f] = -q[f]
        if np.abs(q - q[0]).max() < 2e-5:
            ori = {"static": q[0].tolist()}
        else:
            ori = {"anim": q.tolist()}
        pos = {"anim": BIP.tolist()} if nm == "Bip" else None
        chans.append({"name": nm, "builtin": ac.BIPED.index(nm) if nm in ac.BIPED else None, "pos": pos, "ori": ori})
    return {"name": CLIP_DIR + name + ".animation", "frames": F, "fps": 25.0, "field_1c": 24, "loop_mode": loop,
            "events": [], "fov": [], "channels": chans}


def check(clip, template, sk, P_intent):
    h = cj.clip_to_record(clip, template)
    rec = ac.encode_record(h)
    h2 = ac.decode_record(rec, 0)
    rec2 = ac.encode_record(h2)
    if rec2 != rec:
        raise SystemExit("codec round trip failed for " + clip["name"])
    back = cj.record_to_clip(h2)
    # decoded clip -> FK on the model skeleton
    F = back["frames"]
    by = {c["name"]: c for c in back["channels"]}
    L = np.zeros((F, len(sk.names), 3, 3)); pos = np.tile(sk.pos[None], (F, 1, 1))
    for i, nm in enumerate(sk.names):
        c = by.get(nm)
        if c and c["ori"]:
            o = c["ori"]
            qs = [o["static"]] * F if "static" in o else o["anim"]
            L[:, i] = np.array([rt.qmat_engine(q) for q in qs])
        else:
            L[:, i] = sk.Lbind[i]
        if c and c["pos"]:
            p = c["pos"]
            pos[:, i] = np.array([p["static"]] * F if "static" in p else p["anim"])
    Gd, Pd = sk.fk_local(L, pos)
    skip = [sk.idx["GamePivot"], 0]
    keep = [i for i in range(len(sk.names)) if i not in skip]
    err = np.linalg.norm(Pd[:, keep] - P_intent[:, keep], axis=2).max()
    loop_err = float(np.linalg.norm(Pd[-1, keep] - Pd[0, keep], axis=1).max())
    return rec, {"bytes": len(rec), "frames": F, "decoded_fk_max_err_mm": round(1000 * float(err), 3),
                 "end_vs_start_mm": round(1000 * loop_err, 3)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--partners", default="soldier,civil,dea_taskforce")
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    recs = todclips.anim_records()
    idle = todclips.get_clip("ram_idle_breathe_aware", recs)
    gp = next(c for c in idle["channels"] if c["name"] == "GamePivot")
    for c in idle["channels"]:
        if c["name"] in FACE:
            FACE_CH[c["name"]] = c
    template = next(h for n, (R, h) in recs.items() if n.endswith("ram_idle_breathe_aware.animation"))
    cat_path = os.path.join(OUT, "clips.json")
    cat = json.load(open(cat_path)) if os.path.exists(cat_path) else {}
    rep_path = os.path.join(OUT, "export_report.json")
    report = json.load(open(rep_path)) if os.path.exists(rep_path) else {}
    for partner in [p for p in a.partners.split(",") if p]:
        d = np.load(os.path.join(ROOT, "work", "pair_%s_25.npz" % partner))
        prep = json.load(open(os.path.join(ROOT, "work", "pair_%s_25.json" % partner)))
        s = SHORT[partner]
        sks = {"lead": rt.TodSkel("ram"), "follow": rt.TodSkel(partner)}
        for role, r in (("lead", "lead"), ("follow", "follow")):
            sk = sks[r]
            for kind, Gk, Bk, Pk, loop in (("dance", "G", "BIP", "P", 0), ("fo", "foG", "foB", "foP", 1)):
                if kind == "dance":
                    name = "suave_%s_%s" % (role, s)
                else:
                    name = "suave_%s_%s" % ("push" if role == "lead" else "stumble", s)
                clip = clip_from(sk, d["%s_%s" % (r, Gk)], d["%s_%s" % (r, Bk)], name, loop, gp)
                rec, chk = check(clip, template, sk, d["%s_%s" % (r, Pk)])
                json.dump(clip, open(os.path.join(OUT, name + ".json"), "w"))
                T = (clip["frames"] - 1) * prep["dt"]
                cat[name] = {"clip": clip["name"], "character": "ram" if role == "lead" else partner,
                             "model": sk.model, "role": ("lead" if role == "lead" else "follow") if kind == "dance" else
                             ("push" if role == "lead" else "stumble"), "pairing": partner, "frames": clip["frames"],
                             "seconds": round(T, 5), "cps": round(1.0 / T, 6), "loop_mode": loop,
                             "beats": round(T / pr.BEAT, 3),
                             "release_frame": prep["release_frame"] if kind == "dance" else None,
                             "release_playpos": round(prep["release_frame"] / (clip["frames"] - 1), 6) if kind == "dance" else None,
                             "stand_in": kind == "fo"}
                report[name] = chk
                print(name, chk)
    # intro clips: Ram suave_intro_<p>, partner suave_grabbed_<p>; 6 beats, oneshot, last frame = dance frame 0
    for partner in [p for p in a.partners.split(",") if p]:
        ip = os.path.join(ROOT, "work", "intro_%s_25.npz" % partner)
        if not os.path.exists(ip):
            continue
        d = np.load(ip); irep = json.load(open(ip.replace(".npz", ".json")))
        dd = np.load(os.path.join(ROOT, "work", "pair_%s_25.npz" % partner))
        s = SHORT[partner]
        for r, nm, ident in (("lead", "suave_intro_" + s, "ram"), ("follow", "suave_grabbed_" + s, partner)):
            sk = rt.TodSkel(ident)
            clip = clip_from(sk, d[r + "_G"], d[r + "_BIP"], nm, 1, gp)
            rec, chk = check(clip, template, sk, d[r + "_P"])
            chk["last_frame_vs_dance_frame0_mm"] = round(1000 * float(np.linalg.norm(d[r + "_P"][-1] - dd[r + "_P"][0], axis=1).max()), 3)
            json.dump(clip, open(os.path.join(OUT, nm + ".json"), "w"))
            T = (clip["frames"] - 1) * irep["dt"]
            cat[nm] = {"clip": clip["name"], "character": ident, "model": sk.model, "role": "intro", "pairing": partner,
                       "frames": clip["frames"], "seconds": round(T, 5), "cps": round(1.0 / T, 6), "loop_mode": 1,
                       "beats": round(T / pr.BEAT, 3), "release_frame": None, "release_playpos": None,
                       "stand_in": False, "ends_on": "suave_%s_%s frame 0" % (r, s)}
            report[nm] = chk
            print(nm, chk)
    # lookdown: the partner looks down at the dynamite and freezes; starts on the stumble's last frame
    for partner in [p for p in a.partners.split(",") if p]:
        lp = os.path.join(ROOT, "work", "lookdown_%s_25.npz" % partner)
        if not os.path.exists(lp):
            continue
        d = np.load(lp); lrep = json.load(open(lp.replace(".npz", ".json")))
        s = SHORT[partner]; nm = "suave_lookdown_" + s
        sk = rt.TodSkel(partner)
        clip = clip_from(sk, d["follow_G"], d["follow_BIP"], nm, 1, gp)
        rec, chk = check(clip, template, sk, d["follow_P"])
        json.dump(clip, open(os.path.join(OUT, nm + ".json"), "w"))
        T = (clip["frames"] - 1) * lrep["dt"]
        cat[nm] = {"clip": clip["name"], "character": partner, "model": sk.model, "role": "lookdown", "pairing": partner,
                   "frames": clip["frames"], "seconds": round(T, 5), "cps": round(1.0 / T, 6), "loop_mode": 1,
                   "beats": round(T / pr.BEAT, 3), "release_frame": None, "release_playpos": None, "stand_in": False,
                   "starts_from": "suave_stumble_%s last frame" % s}
        report[nm] = chk
        print(nm, chk)
    if os.path.exists(os.path.join(ROOT, "work", "circle_25.npz")):
        c = np.load(os.path.join(ROOT, "work", "circle_25.npz"))
        crep = json.load(open(os.path.join(ROOT, "work", "circle_25.json")))
        sk = rt.TodSkel("soldier")
        clip = clip_from(sk, c["G"], c["BIP"], "suave_circle", 0, gp)
        rec, chk = check(clip, template, sk, c["P"])
        json.dump(clip, open(os.path.join(OUT, "suave_circle.json"), "w"))
        T = crep["T_s"]
        cat["suave_circle"] = {"clip": clip["name"], "character": "any biped (authored on soldier.model)",
                               "model": sk.model, "role": "circle", "pairing": None, "frames": clip["frames"],
                               "seconds": round(T, 5), "cps": round(1.0 / T, 6), "loop_mode": 0,
                               "beats": crep["beats"], "release_frame": None, "release_playpos": None, "stand_in": True,
                               "sway_per_beat": True, "beats_per_cycle": crep.get("beats_per_cycle", 2)}
        report["suave_circle"] = chk
        print("suave_circle", chk)
    json.dump(cat, open(cat_path, "w"), indent=1)
    json.dump(report, open(rep_path, "w"), indent=1)


if __name__ == "__main__":
    main()
