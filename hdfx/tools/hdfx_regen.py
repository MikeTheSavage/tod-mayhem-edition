"""hdfx_regen.py: build the game-derived files of the HD effects from the PLAYER'S OWN game files.

The TOD HD d3d9.dll recognises textures by the bytes the game uploads (level-0 SHA-256 / CRC32 behind a 32-byte
prefix filter). Those identities, and the sky's irradiance, are game data: they cannot be shipped, and the TOD HD
release's copies are for its HD texture pack (only 16 of its 3,507 material-class rows match vanilla textures). So
the installer runs this on the player's machine, read only, against the archives on the game's configpc.txt
`filesystem =` line (the Gameplay+ data zip first, then blocks.naz), and writes:

  tod_hd_fx\\light\\manifest.txt      material class map: light\\classes.txt (shipped key list) -> the bytes the game has
  tod_hd_fx\\particles\\manifest.txt  sprite identities: particles\\keys.txt (shipped key + class list) -> the bytes
  tod_hd_fx\\sky\\manifest.txt        the jungle's (legacy set 0) five skybox faces -> the bytes; only used to scope
                                     the sky ambient (sky_scope = 1) -- the sky dome itself stays off
  tod_hd_fx\\light\\sh.txt            the sky irradiance as 9 SH coefficients, baked from those same vanilla faces
                                     (the TOD HD one is baked from a Poly Haven HDRI, which stays out)

Formats are exactly the ones the DLL parses (todhd_light.inl LtLoadClasses, todhd_part.inl PtGlobalInit,
todhd_sky.inl SkGlobalInit, todhd_light.inl LtLoadSH). Nothing is written outside --out.

    python -I hdfx_regen.py --game "C:\\GOG Games\\Total Overdose" --payload ..\\payload --out <dir>\\tod_hd_fx
"""
import argparse
import math
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import hdfx_gamedata as gd  # noqa: E402

TARGET_TABLE = 4600          # FX_MAXTARGETS in the DLL: every manifest row that is not already a target takes one
SKY_SET0 = "skybox_night/"   # todhd_skysets.inl g_skSet[0]: the legacy jungle set, the only one that arms the ambient
                             # while sky_sets / sky_dome are off


def _dec(k):
    return re.sub("%([0-9a-fA-F]{2})", lambda m: chr(int(m.group(1), 16)), k)


def _enc(k):
    return "".join(c if (c.isdigit() or ("a" <= c <= "z") or c in "_./-") else "%%%02x" % ord(c) for c in k)


def read_pairs(path, n=2):
    out = []
    for raw in open(path, encoding="utf-8", errors="replace").read().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        c = line.split()
        if len(c) >= n:
            out.append(c[:n])
    return out


def class_manifest(rows, classes):
    want = {}
    for cls, key in classes:
        want.setdefault(_dec(key.lower()), cls.lower())
    found = {}
    for r in rows:
        hits = sorted({want[k] for k in r["keys"] if k in want})
        if not hits:
            continue
        key = sorted(k for k in r["keys"] if k in want)[0]
        found[r["sha256"]] = dict(r, cls=hits[0], key=key, conflict=len(hits) > 1)
    out = sorted(found.values(), key=lambda e: (e["cls"], e["key"], e["sha256"]))
    seen = {k for e in out for k in e["keys"]}
    missing = [k for k in sorted(want) if k not in seen]
    return out, missing


def particle_manifest(rows, keys):
    want = {}
    for k, cls in keys:
        want.setdefault(k.lower(), cls.lower())
    out = []
    for r in rows:
        leaves = sorted({k.split("/")[-1] for k in r["keys"]} & set(want))
        if leaves:
            out.append(dict(r, leaf=leaves[0], cls=want[leaves[0]]))
    out.sort(key=lambda e: (e["leaf"], e["sha256"]))
    missing = sorted(set(want) - {e["leaf"] for e in out})
    return out, missing


def sky_manifest(rows, sky_keys):
    want = {k.lower(): f.upper() for f, k in sky_keys if SKY_SET0 in k.lower()}
    out = []
    for r in rows:
        for k in r["keys"]:
            if k in want:
                out.append(dict(r, face=want[k], key=k))
                break
    out.sort(key=lambda e: (e["face"], e["sha256"]))
    return out, sorted(set(want) - {e["key"] for e in out})


# ------------------------------------------------------------------------------------------- the sky irradiance
SRGB = [((c / 255.0) / 12.92 if c / 255.0 <= 0.04045 else (((c / 255.0) + 0.055) / 1.055) ** 2.4) for c in range(256)]


def _texels(fmt, w, h, l0):
    """[(r, g, b)] linear, row-major, top row first."""
    if fmt == "RGBA32":
        return [(SRGB[l0[i + 2]], SRGB[l0[i + 1]], SRGB[l0[i]]) for i in range(0, w * h * 4, 4)]
    px = [None] * (w * h)                               # DXT1
    o = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            c0, c1 = l0[o] | l0[o + 1] << 8, l0[o + 2] | l0[o + 3] << 8
            bits = l0[o + 4] | l0[o + 5] << 8 | l0[o + 6] << 16 | l0[o + 7] << 24
            o += 8

            def rgb(c):
                return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
            a, b = rgb(c0), rgb(c1)
            if c0 > c1:
                pal = [a, b, tuple((2 * x + y) // 3 for x, y in zip(a, b)), tuple((x + 2 * y) // 3 for x, y in zip(a, b))]
            else:
                pal = [a, b, tuple((x + y) // 2 for x, y in zip(a, b)), (0, 0, 0)]
            for j in range(16):
                x, y = bx + (j & 3), by + (j >> 2)
                if x < w and y < h:
                    c = pal[(bits >> (2 * j)) & 3]
                    px[y * w + x] = (SRGB[c[0]], SRGB[c[1]], SRGB[c[2]])
    return px


def bake_sh(faces, log=print):
    """faces {'UP'|'FR'|...: (fmt, w, h, l0)} -> (9 x rgb coefficients, note lines).

    The engine's cube has no DOWN face. The irradiance is made ZONAL (it depends only on the normal's height): the
    radiance is averaged over azimuth per elevation band, so the face orientation (which way FR points in the world)
    cannot be got wrong; the sun's own direction is the DLL's separate directional light anyway. Below -45 degrees
    (under the side faces) the lowest side-face rows continue. Then exactly the convention of TOD HD's
    tools\\make_sh.py: Lambert-convolved, the BRDF's 1/pi folded in, SCALED so the mean irradiance over all normals is 1 in
    luminance, written in the DLL's basis order 1, y, z, x, xy, yz, 3z^2-1, xz, x^2-y^2."""
    nb = 90                                               # 2-degree elevation bands, -90..+90
    acc = [[0.0, 0.0, 0.0, 0.0] for _ in range(nb)]       # r g b weight (solid angle)

    def add(d, c, wgt):
        el = math.degrees(math.asin(max(-1.0, min(1.0, d[1]))))
        b = min(nb - 1, max(0, int((el + 90.0) / 2.0)))
        a = acc[b]
        a[0] += c[0] * wgt; a[1] += c[1] * wgt; a[2] += c[2] * wgt; a[3] += wgt
    for f, (fmt, w, h, l0) in faces.items():
        px = _texels(fmt, w, h, l0)
        for y in range(h):
            for x in range(w):
                u, v = 2.0 * (x + 0.5) / w - 1.0, 1.0 - 2.0 * (y + 0.5) / h
                d = (u, 1.0, v) if f == "UP" else (u, v, 1.0)      # side faces: v up; azimuth does not matter here
                n = math.sqrt(d[0] ** 2 + d[1] ** 2 + d[2] ** 2)
                d = (d[0] / n, d[1] / n, d[2] / n)
                add(d, px[y * w + x], 1.0 / n ** 3)               # cube texel solid angle
    lowest = next(i for i in range(nb) if acc[i][3] > 0)
    for i in range(nb):                                   # fill the bands no face covers (under the side faces)
        if acc[i][3] == 0:
            src = acc[lowest] if i < lowest else acc[max(j for j in range(nb) if acc[j][3] > 0)]
            acc[i] = list(src)
    L = [(a[0] / a[3], a[1] / a[3], a[2] / a[3]) for a in acc]
    # Legendre projection over t = sin(elevation) (dOmega = 2pi dt for a zonal function): a_l = (2l+1)/2 int L P_l dt
    a = [[0.0] * 3 for _ in range(3)]
    steps = 2000
    for k in range(steps):
        t = -1.0 + (k + 0.5) * 2.0 / steps
        el = math.degrees(math.asin(t))
        c = L[min(nb - 1, max(0, int((el + 90.0) / 2.0)))]
        P = (1.0, t, 0.5 * (3 * t * t - 1))
        for l in range(3):
            for ch in range(3):
                a[l][ch] += (2 * l + 1) / 2.0 * c[ch] * P[l] * (2.0 / steps)
    # irradiance (uniform radiance 1 -> 1): E(t) = a0 + (2/3) a1 t + (1/4) a2 (3t^2-1)/2  =  A + B t + C (3t^2 - 1)
    A = a[0]
    B = [x * 2.0 / 3.0 for x in a[1]]
    C = [x / 8.0 for x in a[2]]
    lum = 0.2126 * A[0] + 0.7152 * A[1] + 0.0722 * A[2]
    s = 1.0 / lum if lum > 1e-6 else 1.0
    A, B, C = [x * s for x in A], [x * s for x in B], [x * s for x in C]
    # the DLL's basis: E = c0 .282095 + c1 .488603 y + ... + c6 .315392 (3z^2-1) + c8 .546274 (x^2-y^2)
    # and 3y^2-1 = -(3z^2-1)/2 - 3(x^2-y^2)/2
    c = [[0.0] * 3 for _ in range(9)]
    for ch in range(3):
        c[0][ch] = A[ch] / 0.282095
        c[1][ch] = B[ch] / 0.488603
        c[6][ch] = -C[ch] / (2 * 0.315392)
        c[8][ch] = -1.5 * C[ch] / 0.546274

    def ev(t):
        return [A[ch] + B[ch] * t + C[ch] * (3 * t * t - 1) for ch in range(3)]
    up, dn, hz = ev(1.0), ev(-1.0), ev(0.0)
    note = ["scale x %.4f (the faces' own mean irradiance luminance was %.5f)" % (s, lum),
            "what it evaluates to: up %.4f %.4f %.4f | horizon %.4f %.4f %.4f | down %.4f %.4f %.4f" % (*up, *hz, *dn)]
    return c, note


# ------------------------------------------------------------------------------------------------- writing
def write(path, lines):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, path)


def main(argv=None, log=print):
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--payload", default=os.path.join(os.path.dirname(HERE), "payload"),
                    help="the shipped hdfx payload (the key lists)")
    ap.add_argument("--out", required=True, help="the tod_hd_fx folder to write into")
    ap.add_argument("--archives", default=None, help="comma list instead of configpc.txt's filesystem line")
    a = ap.parse_args(argv)
    t0 = time.time()
    pfx = os.path.join(a.payload, "tod_hd_fx")
    arch = [x.strip() for x in a.archives.split(",")] if a.archives else None
    src_note = "the archives of %s (%s)" % (a.game, ", ".join(arch or gd.filesystem_archives(a.game)))
    rows, st = gd.harvest(a.game, arch, log=log)
    log("hdfx: %d distinct textures in %d blocks (%s; %d unreadable blocks) in %.0f s"
        % (len(rows), st["blocks"], ", ".join(st["archives"]), st["errors"], time.time() - t0))
    out = os.path.abspath(a.out)
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")

    classes = read_pairs(os.path.join(pfx, "light", "classes.txt"))
    crow, cmiss = class_manifest(rows, classes)
    lines = ["# Gameplay+ HD effects -- per-pixel lighting: which BYTES each texture key in light\\classes.txt is.",
             "# GENERATED on this PC by hdfx\\tools\\hdfx_regen.py from %s, %s." % (src_note, stamp),
             "# Do not edit (edit classes.txt and run the installer again).",
             "# columns: class  key  sha256_l0  crc32_l0  fmt  w  h  levels  prefix32  source"]
    for e in crow:
        lines.append("%-16s %-64s %s %s %-7s %5d %5d %3d %s %s" % (e["cls"], _enc(e["key"]), e["sha256"], e["crc32"],
                                                                 e["fmt"], e["w"], e["h"], e["levels"], e["prefix32"],
                                                                 "vanilla"))
    write(os.path.join(out, "light", "manifest.txt"), lines)

    pkeys = read_pairs(os.path.join(pfx, "particles", "keys.txt"))
    prow, pmiss = particle_manifest(rows, pkeys)
    lines = ["# Gameplay+ HD effects -- the particle and decal sprite textures, by the SHA-256 of the bytes the game uploads.",
             "# GENERATED on this PC by hdfx\\tools\\hdfx_regen.py from %s, %s." % (src_note, stamp),
             "# key  sha256_of_level_0  format  width  height  levels  first_32_bytes  class"]
    for e in prow:
        lines.append("%s %s %s %d %d %d %s %s" % (e["leaf"], e["sha256"], e["fmt"], e["w"], e["h"], e["levels"],
                                                  e["prefix32"], e["cls"]))
    write(os.path.join(out, "particles", "manifest.txt"), lines)

    skeys = read_pairs(os.path.join(pfx, "sky", "keys.txt"))
    srow, smiss = sky_manifest(rows, skeys)
    lines = ["# Gameplay+ HD effects -- the jungle's skybox faces (the legacy sky set 0, %s), by the bytes the game uploads." % SKY_SET0,
             "# Only the sky ambient's level scope (sky_scope = 1) uses them: sky_dome stays 0, the game draws its own sky.",
             "# GENERATED on this PC by hdfx\\tools\\hdfx_regen.py from %s, %s." % (src_note, stamp),
             "# face key sha256_l0 fmt w h levels prefix32 source"]
    for e in srow:
        lines.append("%-3s %-62s %s %-7s %5d %5d %3d %s vanilla" % (e["face"], e["key"], e["sha256"], e["fmt"], e["w"],
                                                                     e["h"], e["levels"], e["prefix32"]))
    write(os.path.join(out, "sky", "manifest.txt"), lines)

    want = {k.lower(): f.upper() for f, k in skeys if SKY_SET0 in k.lower()}
    copies = gd.read_level0(a.game, list(want), arch)
    faces = {want[k]: v[0] for k, v in copies.items() if v}
    if len(faces) == 5:
        c, note = bake_sh(faces, log)
        lines = ["# Gameplay+ HD effects -- the sky's irradiance as 9 spherical-harmonic coefficients (L2), read by the",
                 "# TOD HD d3d9.dll (todhd_light.inl LtLoadSH).  GENERATED on this PC by hdfx\\tools\\hdfx_regen.py, %s," % stamp,
                 "# from the game's OWN jungle skybox (%s: %s), not from an HDRI." % (SKY_SET0, ", ".join(sorted(faces))),
                 "# Zonal (averaged over azimuth; the cube has no DOWN face, so below -45 deg the lowest rows continue),",
                 "# Lambert-convolved with the BRDF's 1/pi folded in, scaled to a mean irradiance of 1 (luminance).",
                 "# basis order: 1, y, z, x, xy, yz, 3z^2-1, xz, x^2-y^2"] + ["# " + n for n in note]
        lines += ["sh %d %.6f %.6f %.6f" % (i, c[i][0], c[i][1], c[i][2]) for i in range(9)]
        write(os.path.join(out, "light", "sh.txt"), lines)
        log("hdfx: sky irradiance baked from the vanilla jungle sky: %s" % note[1])
    else:
        log("hdfx: WARNING the jungle skybox faces were not all found (%s): no sh.txt, the per-pixel lighting stays "
            "off (the DLL logs it)" % sorted(faces))
    total = len(crow) + len(prow) + len(srow)
    log("hdfx: light\\manifest.txt %d rows (%d of %d keys missing), particles\\manifest.txt %d rows (%d keys missing: %s), "
        "sky\\manifest.txt %d rows; at most %d of the DLL's %d target slots"
        % (len(crow), len(cmiss), len(classes), len(prow), len(pmiss), ", ".join(pmiss) or "-", len(srow), total,
           TARGET_TABLE))
    if total > TARGET_TABLE:
        log("hdfx: WARNING more rows than the DLL's target table: the class rows past the limit take `default`")
    log("hdfx: done in %.0f s" % (time.time() - t0))
    return dict(classes=len(crow), particles=len(prow), sky=len(srow), sh=len(faces) == 5, targets=total)


if __name__ == "__main__":
    main()
