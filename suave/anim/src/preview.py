"""preview.py - preview MP4 of one Suave pairing (Blender 5.2 Eevee, game meshes + textures, Suavemente under it).

  python preview.py PARTNER [--size 1280x720] [--step 1] [--frames a:b] [--no-video]
Timeline (30 fps): the intro (suave_intro / suave_grabbed, 6 beats), the dance from its frame 0 to the release
(beat 19), the push / stumble stand-ins (4.5 beats), then the partner's lookdown (1.5 s; Ram holds his last push pose),
then a short hold; a ring of 8 soldiers (looks ts0/ts1/ts2) on a 3 m ring does the one-sway-per-beat circle loop,
phase-shifted by one beat each (neighbours sway to opposite sides). Camera: in front of the pair (perpendicular to the
Ram-partner line at the dance start, Ram's front-left), inside the ring, tracking the pair's centre.
Audio: the music segment for the pairing (suave/audio/build/suave_mus_<k>.ogg: soldier 1, civil 2, DEA 3), whose
time 0 is the intro's beat 0 (a downbeat).
Outputs: ../previews/suave_<partner>.mp4 (+ frames in ../work/frames_<partner>/).
"""
import argparse, json, os, subprocess, sys
import numpy as np

sys.dont_write_bytecode = True
import pair as pr
import circle as cr
import retarget as rt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORK = os.path.join(ROOT, "work")
PREV = os.path.join(ROOT, "previews")
BLENDER = os.path.join(os.environ["LOCALAPPDATA"], r"Microsoft\WindowsApps\blender-launcher.exe")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
MP3 = os.environ.get("TODSONG_MP3", "Suavemente.mp3")
AUDIO_START = 0.373 + 32 * pr.BEAT
FPS = 30
RING_R = 3.0
RING_N = 8
CAM_AZ = -50         # degrees from Ram's start facing (+z): perpendicular to the pair line (partner at about +40 deg)
CAM_DIST = 2.85        # inside the 3 m ring, between two members (they stay beside / behind the camera)
STAGE = os.environ.get("TOD_STAGE_DIR", "stage")  # folder with the exported character .glb files
SOLDIER_LOOKS = [STAGE + "/c57f9245ca9.glb", STAGE + "/c49cd53ccd0.glb",
                 STAGE + "/cdec9f9c35c.glb"]
CONV = np.array([[-1, 0, 0], [0, 0, -1], [0, 1, 0]], dtype=float)


def mats(G, P):
    F, J = G.shape[:2]
    M = np.zeros((F, J, 4, 4)); M[..., :3, :3] = G; M[..., :3, 3] = P; M[..., 3, 3] = 1
    return M


def get_pair(partner, fps):
    path = os.path.join(WORK, "pair_%s_%d.npz" % (partner, fps))
    if not os.path.exists(path):
        o, r, _ = pr.build(partner, fps, verbose=False)
        pr.save(o, r, partner, fps, path)
    d = np.load(path)
    rep = json.load(open(path.replace(".npz", ".json")))
    return d, rep


def get_circle(fps):
    path = os.path.join(WORK, "circle_%d.npz" % fps)
    if not os.path.exists(path):
        out, rep = cr.build(fps, verbose=False)
        np.savez_compressed(path, **out); json.dump(rep, open(path.replace(".npz", ".json"), "w"), indent=1)
    return np.load(path)


def glb_order(glb, skel):
    import glbio
    gl, _ = glbio.read_glb(glb)
    names = [gl["nodes"][j].get("name", "") for j in gl["skins"][0]["joints"]]
    return [skel.idx["" if n == "bone0" else n] for n in names]


MUSIC = {"soldier": 1, "civil": 2, "dea_taskforce": 3}
SEG = os.path.join(os.path.dirname(os.path.dirname(ROOT)), "suave", "audio", "build", "suave_mus_%d.ogg")


def timeline(d, rep, hold_s=0.6, partner=None):
    rel = rep["release_frame"]
    tl = {}
    it = np.load(os.path.join(WORK, "intro_%s_%d.npz" % (partner, FPS))) if partner else None
    ld = np.load(os.path.join(WORK, "lookdown_%s_%d.npz" % (partner, FPS))) if partner else None
    for r in ("lead", "follow"):
        sk = rt.TodSkel("ram" if r == "lead" else partner)
        G = np.concatenate([it[r + "_G"][:-1], d[r + "_G"][:rel], d[r + "_foG"]])
        P = np.concatenate([it[r + "_P"][:-1], d[r + "_P"][:rel], d[r + "_foP"]])
        nl = len(ld["follow_G"]) - 1
        if r == "follow":
            G = np.concatenate([G, ld["follow_G"][1:]]); P = np.concatenate([P, ld["follow_P"][1:]])
        else:
            G = np.concatenate([G, np.repeat(G[-1:], nl, 0)]); P = np.concatenate([P, np.repeat(P[-1:], nl, 0)])
        nh = int(round(hold_s * FPS))
        G = np.concatenate([G, np.repeat(G[-1:], nh, 0)]); P = np.concatenate([P, np.repeat(P[-1:], nh, 0)])
        tl[r] = (G, P)
    return tl


def make_job(partner, size=(1280, 720), first=0, last=None, step=1):
    d, rep = get_pair(partner, FPS)
    c = get_circle(FPS)
    tl = timeline(d, rep, partner=partner)
    F = len(tl["lead"][0])
    sks = {"ram": rt.TodSkel("ram"), partner: rt.TodSkel(partner), "soldier": rt.TodSkel("soldier")}
    arrays, inst = {}, []
    o = glb_order(pr.GLB["ram"], sks["ram"])
    arrays["ram"] = mats(*tl["lead"])[:, o]
    inst.append({"name": "ram", "glb": pr.GLB["ram"], "key": "ram"})
    o = glb_order(pr.GLB[partner], sks[partner])
    arrays["partner"] = mats(*tl["follow"])[:, o]
    inst.append({"name": partner, "glb": pr.GLB[partner], "key": "partner"})
    # pair centre over the clip (TOD pair frame) -> ring centre and camera
    mid = 0.5 * (tl["lead"][1][:, 1] + tl["follow"][1][:, 1])
    C = mid.mean(0) * [1, 0, 1]
    nC = len(c["G"]) - 1
    beat_f = pr.BEAT * FPS
    for k in range(RING_N):
        ang = np.radians(CAM_AZ) + 2 * np.pi * (k + 0.5) / RING_N      # no member right at the camera
        pos = C + RING_R * np.array([np.sin(ang), 0, np.cos(ang)])
        face = np.arctan2(*(C - pos)[[0, 2]])        # +z toward the centre
        R = pr.Ry(face)
        idx = (np.arange(F) + int(round(1 * k * beat_f))) % nC
        G = np.einsum("ab,fjbc->fjac", R, c["G"][idx])
        P = np.einsum("ab,fjb->fja", R, c["P"][idx]) + pos
        glb = SOLDIER_LOOKS[k % 3]
        o = glb_order(glb, sks["soldier"])
        arrays["ring%d" % k] = mats(G, P)[:, o]
        inst.append({"name": "ring%d" % k, "glb": glb, "key": "ring%d" % k})
    # camera: front three-quarter of Ram's start facing (+z), inside the ring, tracking the smoothed pair centre
    yaw_c = np.radians(CAM_AZ)
    camp = C + np.array([np.sin(yaw_c), 0, np.cos(yaw_c)]) * CAM_DIST + [0, pr.GROUND + 1.9, 0]
    look = mid.copy(); look[:, 1] = pr.GROUND + 1.0
    a = 0.06
    for f in range(1, F):
        look[f] = look[f - 1] + a * (look[f] - look[f - 1])
    look = 0.5 * look + 0.5 * (C + [0, pr.GROUND + 1.0, 0])
    out_dir = os.path.join(WORK, "frames_%s" % partner)
    os.makedirs(out_dir, exist_ok=True)
    npz = os.path.join(WORK, "job_%s.npz" % partner)
    np.savez(npz, **arrays)
    cam_b = CONV @ camp
    job = {"npz": npz, "out_dir": out_dir, "width": size[0], "height": size[1], "fps": FPS, "frames": F,
           "first": first, "last": last or F, "step": step, "src_dir": HERE, "instances": inst,
           "camera": {"location": cam_b.tolist(), "look": (look @ CONV.T).tolist(), "lens": 19,
                      "center": (CONV @ C).tolist(), "yaw_deg": float(np.degrees(np.arctan2(cam_b[0] - (CONV @ C)[0], -(cam_b[1] - (CONV @ C)[1]))))},
           "ring_marks": {"radius": RING_R, "center": (CONV @ C).tolist()}, "samples": 32, "ground_z": pr.GROUND}
    jp = os.path.join(WORK, "job_%s.json" % partner)
    json.dump(job, open(jp, "w"))
    return jp, job, rep


def render(jp, log):
    with open(log, "w") as lf:
        subprocess.run([BLENDER, "-b", "--factory-startup", "--python", os.path.join(HERE, "bl_preview.py"), "--", jp],
                       stdout=lf, stderr=subprocess.STDOUT, check=False)


def video(job, partner):
    os.makedirs(PREV, exist_ok=True)
    out = os.path.join(PREV, "suave_%s.mp4" % partner)
    dur = job["frames"] / FPS
    cmd = [FFMPEG, "-y", "-v", "error", "-framerate", str(FPS), "-i", os.path.join(job["out_dir"], "f%05d.png"),
           "-t", "%.3f" % dur, "-i", SEG % MUSIC[partner],
           "-map", "0:v", "-map", "1:a", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18",
           "-c:a", "aac", "-b:a", "192k", "-af", "afade=t=out:st=%.3f:d=0.6" % (dur - 0.6), "-shortest", out]
    subprocess.run(cmd, check=True)
    return out


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("partner")
    ap.add_argument("--size", default="1280x720")
    ap.add_argument("--frames", default=None)
    ap.add_argument("--step", type=int, default=1)
    ap.add_argument("--no-video", action="store_true")
    a = ap.parse_args()
    w, h = map(int, a.size.split("x"))
    f0, f1 = (map(int, a.frames.split(":")) if a.frames else (0, None))
    jp, job, rep = make_job(a.partner, (w, h), f0, f1, a.step)
    print("job", jp, "frames", job["frames"])
    render(jp, os.path.join(WORK, "render_%s.log" % a.partner))
    if not a.no_video:
        print(video(job, a.partner))
