"""hdfx_stage.py: put together the complete HD effects file set for one game folder, in a STAGING folder.

    <out>\\d3d9.dll             the TOD HD release DLL, byte for byte (hdfx\\release\\d3d9.dll, sha256 checked)
    <out>\\tod_hd_fx.ini        hdfx\\payload\\tod_hd_fx.ini with the selection's key=value sets
    <out>\\tod_hd_light.txt     the material-class rows (shipped)
    <out>\\tod_hd_fx\\...        the shipped key lists + the manifests and sh.txt hdfx_regen.py makes from the game's
                               own archives (read only)

The installer copies the staged files into the game folder; the test harness copies them into the test copy.
FILES lists every path this feature owns in a game folder (an install removes exactly those when it is turned off).

    python -I hdfx_stage.py --game DIR --out DIR [--set key=value ...]
"""
import argparse
import hashlib
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HDFX = os.path.dirname(HERE)
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import hdfx_regen  # noqa: E402

RELEASE_DLL = os.path.join(HDFX, "release", "d3d9.dll")
RELEASE_SHA256 = "c34aab177364ad211e91796c33efce8655146777460a43fcc5d2267c74ee5f8f"   # TOD HD RELEASE.txt, 2026-10-01
PAYLOAD = os.path.join(HDFX, "payload")
INI = "tod_hd_fx.ini"
FILES = ["d3d9.dll", INI, "tod_hd_light.txt"]          # plus the folder below
FOLDER = "tod_hd_fx"
MARKER = "Gameplay+ HD effects"                         # first lines of our ini (an ini without it is not ours)


def sha_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def is_ours_ini(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return MARKER in f.read(400)
    except OSError:
        return False


def is_todhd_dll(path):
    """Any TOD HD fx d3d9.dll (ours ships one of its releases unchanged)."""
    try:
        with open(path, "rb") as f:
            d = f.read()
        return b"tod_hd_fx.ini" in d and b"TODHD_LOG_DIR" in d
    except OSError:
        return False


def apply_sets(text, sets):
    """key=value pairs into an ini text: replaced in place (comment kept), else appended."""
    want = {}
    for kv in sets or []:
        k, v = kv.split("=", 1)
        want[k.strip().lower()] = v.strip()
    out, done = [], set()
    for line in text.splitlines():
        s = line.strip()
        if "=" in s and not s.startswith(";") and not s.startswith("#"):
            k = s.split("=", 1)[0].strip().lower()
            if k in want:
                rest = line.split(";", 1)
                line = ("%-24s ; %s" % ("%s = %s" % (k, want[k]), rest[1].strip())) if len(rest) > 1 else "%s = %s" % (k, want[k])
                done.add(k)
        out.append(line)
    out += ["%s = %s" % (k, v) for k, v in want.items() if k not in done]
    return "\n".join(out) + "\n"


def merge_user_ini(shipped_text, user_text, sets):
    """The player's own tuned values survive a reinstall: every key the shipped ini has takes the player's value when
    the player's (ours) ini has that key, then the selection's sets are applied on top."""
    user = {}
    for line in (user_text or "").splitlines():
        s = line.split(";", 1)[0].strip()
        if "=" in s and not s.startswith("#"):
            k, v = s.split("=", 1)
            user[k.strip().lower()] = v.strip()
    keep = ["%s=%s" % (k, v) for k, v in user.items()]
    return apply_sets(apply_sets(shipped_text, keep), sets)


def stage(game_dir, out, sets=None, user_ini=None, log=print):
    if sha_file(RELEASE_DLL) != RELEASE_SHA256:
        raise RuntimeError("hdfx\\release\\d3d9.dll is not the TOD HD release it should be (sha256 differs)")
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    shutil.copyfile(RELEASE_DLL, os.path.join(out, "d3d9.dll"))
    shutil.copyfile(os.path.join(PAYLOAD, "tod_hd_light.txt"), os.path.join(out, "tod_hd_light.txt"))
    shutil.copytree(os.path.join(PAYLOAD, FOLDER), os.path.join(out, FOLDER))
    shipped = open(os.path.join(PAYLOAD, INI), encoding="utf-8").read()
    text = merge_user_ini(shipped, user_ini, sets) if user_ini else apply_sets(shipped, sets)
    with open(os.path.join(out, INI), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    res = hdfx_regen.main(["--game", game_dir, "--payload", PAYLOAD, "--out", os.path.join(out, FOLDER)], log=log)
    return res


def staged_files(out):
    """[relative path] of every file in a staged set (what an install copies)."""
    r = []
    for root, _dirs, files in os.walk(out):
        for f in files:
            r.append(os.path.relpath(os.path.join(root, f), out))
    return sorted(r)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--set", action="append")
    a = ap.parse_args(argv)
    r = stage(a.game, a.out, a.set)
    print(r)


if __name__ == "__main__":
    main()
