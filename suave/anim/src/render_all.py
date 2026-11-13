"""render_all.py - render the three preview MP4s and the contact sheet (run detached; takes ~25 min).

  python render_all.py [partners...]
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
parts = sys.argv[1:] or ["soldier", "civil", "dea_taskforce"]
for p in parts:
    fr = os.path.join(ROOT, "work", "frames_" + p)
    if os.path.isdir(fr):
        for f in os.listdir(fr):
            if f.endswith(".png"):
                os.remove(os.path.join(fr, f))
    subprocess.run([sys.executable, "-B", os.path.join(HERE, "preview.py"), p], check=True)
    print("rendered", p, flush=True)
frames = [0, 45, 90, 135, 180, 225, 270, 315]
files = []
for p in ["soldier", "civil", "dea_taskforce"]:
    files += [os.path.join(ROOT, "work", "frames_" + p, "f%05d.png" % f) for f in frames]
if all(os.path.exists(f) for f in files):
    subprocess.run([sys.executable, "-B", os.path.join(HERE, "montage.py"),
                    os.path.join(ROOT, "previews", "suave_contact_sheet.png"), "8", "400"] + files, check=True)
print("RENDER_ALL_DONE", flush=True)
