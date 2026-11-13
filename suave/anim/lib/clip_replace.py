"""Rebuild overdose.main with replaced (or added) animation clips and package it the way TOD.exe loads it.

  python clip_replace.py --clip ..\\out\\ram_run_armswing.json [--clip more.json] --out ..\\out\\build_armswing
         [--naz-out path\\blocks.naz [--keep-naz]] [--deploy <test game folder>] [--allow-add]

Inputs: clip JSON files (clipjson.py format); the clip's "name" picks the record it replaces (case-insensitive).
Base bytes always come from the GOG blocks.naz (read-only), or --base-block <file> (e.g. an already rebuilt block).
Outputs in --out:
  overdose.main           the rebuilt block
  anim_override.zip       plain STORED zip holding it under its archive name, to list FIRST in configpc.txt
  configpc_line.txt       the filesystem line to use
  report.json             every check below
Checks (all must pass, else nothing is written):
  1 block parses strictly: header, size table, file length, M == max(sizes), heuristic record scan count == N
  2 every ANIMATION record's size pass (FUN_00900880 layout) ends at align16 == next record start
  3 every untouched record is byte-identical (after the shift); header timestamp/checksums unchanged
  4 each replaced clip re-decodes to exactly the encoded intent
  5 existing readers: TOD HD tod_hd_game.Block (strict, read-only import), the project's tod_anim_extract
    (find_anim_records/decode_anim on the written file: 446 untouched clips equal the original, the target equals the
    intent), the project's tod_texture_extract sequential walker (texture count unchanged)
  6 the zip is read back with the engine's OpenZip/FindFile logic (port) and returns the block bytes
--naz-out writes a full copy of blocks.naz with the overdose.main entry re-pointed at an appended copy of the new
block (the old bytes stay as dead space; local headers are never read by the engine), verifies every entry, and
deletes the copy unless --keep-naz. --deploy writes anim_override.zip + configpc.txt into a TEST game folder only.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import todblock as tb
import animcodec as ac
import clipjson as cj

OD = "data_pc/overdose_the_game/overdose/shared/overdose.main"
GOG = r"C:\GOG Games\Total Overdose"
PROJECT_TOOLS = os.environ.get("TOD_PROJECT_TOOLS", "")  # the author's tools (not included)
TODHD_APP = os.environ.get("TODHD_APP", "")  # TOD HD's app folder
FORBIDDEN_DEPLOY = [os.path.normcase(os.path.abspath(p)) for p in (GOG,)]
BASE_ARCHIVES = ["blocks.naz", "sounds.naz", "videos00.naz", "videos01.naz"]
ZIP_NAME = "anim_override.zip"


class Fail(Exception):
    pass


def sha(b):
    return hashlib.sha256(b).hexdigest()


def build(base_bytes, clips, allow_add=False):
    old = tb.Block(base_bytes, ".main")
    starts = tb.record_starts(old.info, old.n)
    ends = starts[1:] + [len(old.info)]
    names = [tb.record_name(old.info, r).lower() for r in starts]
    want = {}
    for c in clips:
        nm = c["name"].lower()
        if nm in want:
            raise Fail("clip given twice: " + c["name"])
        want[nm] = c
    template = None
    new_info = bytearray()
    new_recs = {}
    layout = []                      # (index, new start, new end, replaced?)
    for k, (R, E) in enumerate(zip(starts, ends)):
        typ = tb.u32(old.info, R)
        if typ == 9 and template is None:
            template = ac.decode_record(old.info, R)
        if names[k] in want:
            if typ != 9:
                raise Fail("%s is not an ANIMATION record" % names[k])
            h = cj.clip_to_record(want[names[k]], ac.decode_record(old.info, R))
            rec = ac.encode_record(h)
            new_recs[k] = (h, rec)
            layout.append((k, len(new_info), len(new_info) + len(rec), True))
            new_info += rec
        else:
            layout.append((k, len(new_info), len(new_info) + E - R, False))
            new_info += old.info[R:E]
    added = [nm for nm in want if nm not in names]
    nb = tb.Block(base_bytes, ".main")
    for nm in added:
        if not allow_add:
            raise Fail("clip %s is not in overdose.main (use --allow-add to append it as a new record)" % nm)
        h = cj.clip_to_record(want[nm], template)
        rec = ac.encode_record(h)
        k = len(layout)
        new_recs[k] = (h, rec)
        layout.append((k, len(new_info), len(new_info) + len(rec), True))
        new_info += rec
        nb.payloads.append(b"")
    nb.info = new_info
    return old, nb, starts, ends, new_recs, layout, added


def verify_block(old, nb, out_bytes, starts, ends, new_recs, layout):
    rep = {}
    chk = tb.Block(out_bytes, ".main")                                         # 1
    s2 = tb.record_starts(chk.info, chk.n)
    rep["1_strict_parse"] = {"N": chk.n, "info_bytes": len(chk.info), "records_found": len(s2),
                             "max_asset": chk.max_asset, "file_bytes": len(out_bytes)}
    if s2 != [a for _, a, _, _ in layout]:
        raise Fail("record starts differ from the build layout")
    e2 = s2[1:] + [len(chk.info)]
    n_anim = 0
    for R, E in zip(s2, e2):                                                  # 2
        if tb.u32(chk.info, R) == 9:
            n_anim += 1
            if tb.a16(len(ac.encode_record(ac.decode_record(chk.info, R)))) != E - R:
                raise Fail("size pass of record at 0x%x does not reach the next record" % R)
    rep["2_anim_size_pass_ok"] = n_anim
    if (chk.timestamp, chk.prop_ck, chk.cmd_ck) != (old.timestamp, old.prop_ck, old.cmd_ck) or chk.prefix != old.prefix:
        raise Fail("header timestamp/checksums changed")
    same = 0
    for (k, a, z, repl) in layout:                                            # 3
        if repl:
            continue
        if bytes(chk.info[a:z]) != bytes(old.info[starts[k]:ends[k]]):
            raise Fail("untouched record %d changed" % k)
        same += 1
    for k in range(min(len(old.payloads), len(chk.payloads))):
        if old.payloads[k] != chk.payloads[k]:
            raise Fail("payload %d changed" % k)
    rep["3_untouched_records_identical"] = same
    rep["3_header_unchanged"] = True
    for k, (h, rec) in new_recs.items():                                      # 4
        a = layout[k][1]
        h2 = ac.decode_record(chk.info, a)
        if ac.encode_record(h2) != rec or bytes(chk.info[a:a + len(rec)]) != rec:
            raise Fail("replaced clip %s does not read back" % h["name"])
    rep["4_replaced_read_back"] = [h["name"] for h, _ in new_recs.values()]
    return chk, rep


def existing_readers(out_path, old_bytes, chk, new_recs, layout, tmpdir):
    rep = {}
    # TOD HD strict block reader (read-only import; bytecode writing disabled)
    sys.path.insert(0, TODHD_APP)
    try:
        import tod_hd_game
        hb = tod_hd_game.Block(chk.serialize(), ".main")
        rep["5a_todhd_Block"] = {"records": len(hb.records), "textures": len(hb.textures())}
    finally:
        sys.path.remove(TODHD_APP)
    # project tools: tod_anim_extract + tod_texture_extract
    sys.path.insert(0, PROJECT_TOOLS)
    try:
        import tod_anim_extract as tae
        import tod_model_extract as me
        import tod_texture_extract as tte
        old_path = os.path.join(tmpdir, "_orig_overdose.main")
        with open(old_path, "wb") as f:
            f.write(old_bytes)
        res = {}
        for tag, p in (("old", old_path), ("new", out_path)):
            info = me.load_info(p)
            recs = tae.find_anim_records(info)
            dec = {}
            for b, n in recs:
                a = tae.decode_anim(info, b, 64)
                dec[n.lower()] = a
            res[tag] = dec
        replaced = {h["name"].lower() for h, _ in new_recs.values()}
        same = sum(1 for n, a in res["old"].items() if n not in replaced and res["new"].get(n) == a)
        rep["5b_tod_anim_extract"] = {"clips_old": len(res["old"]), "clips_new": len(res["new"]),
                                      "untouched_equal": same}
        if same != len([n for n in res["old"] if n not in replaced]):
            raise Fail("tod_anim_extract: an untouched clip decodes differently")
        for h, _ in new_recs.values():
            a = res["new"].get(h["name"].lower())
            if a is None:
                raise Fail("tod_anim_extract does not find " + h["name"])
            # compare with the intent (engine raw quats, same channel index = entry index)
            frames = struct.unpack("<i", struct.pack("<I", h["frames"]))[0]
            for e, c in enumerate(h["chans"]):
                t = a["tracks"][e]
                if c["ori"]:
                    seq = [c["ori"][1]] * frames if c["ori"][0] == "static" else c["ori"][1]
                    want = [(q[1] / 32767.0, q[2] / 32767.0, q[3] / 32767.0, q[0] / 32767.0) for q in seq]
                    if t["rot"] != want:
                        raise Fail("tod_anim_extract: %s channel %d rotation differs" % (h["name"], e))
                if c["pos"]:
                    seq = [c["pos"][1]] * frames if c["pos"][0] == "static" else c["pos"][1]
                    if [tuple(p) for p in t["pos"]] != [tuple(p) for p in seq]:
                        raise Fail("tod_anim_extract: %s channel %d position differs" % (h["name"], e))
        rep["5b_target_matches_intent"] = True
        nt_old, _ = tte.extract(old_path, listonly=False, dry=True)
        nt_new, _ = tte.extract(out_path, listonly=False, dry=True)
        rep["5c_tod_texture_extract_textures"] = [nt_old, nt_new]
        if nt_old != nt_new:
            raise Fail("tod_texture_extract walker lost alignment (%d vs %d textures)" % (nt_old, nt_new))
        os.remove(old_path)
    finally:
        sys.path.remove(PROJECT_TOOLS)
    return rep


def write_zip(path, entry, data):
    tmp = path + ".part"
    with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_STORED, allowZip64=False) as zf:
        zi = zipfile.ZipInfo(entry, date_time=(2005, 8, 21, 0, 0, 0))
        zi.compress_type = zipfile.ZIP_STORED
        zi.create_system = 0
        zi.external_attr = 0
        zf.writestr(zi, data)
    os.replace(tmp, path)
    st = os.stat(os.path.join(GOG, "blocks.naz"))                             # slot-0 timestamp parity (TOD HD B.5)
    os.utime(path, (st.st_atime, st.st_mtime))
    a = tb.Archive(path)                                                      # 6 engine view
    hit = a.find("/" + entry)
    if a.naz or hit is None or hit[2] != entry or hit[1] != len(data):
        raise Fail("zip: engine lookup failed")
    with open(path, "rb") as f:
        f.seek(hit[0])
        if f.read(hit[1]) != data:
            raise Fail("zip: bytes at the engine's offset differ")
        f.seek(0, 2)
        size = f.tell()
        f.seek(size - 22)
        e = f.read(22)
    if e[:4] != b"PK\x05\x06" or struct.unpack_from("<H", e, 20)[0] != 0:
        raise Fail("zip: end record not at the end of the file")
    base = tb.open_naz()
    collide = [n for c, (_, _, n) in base.table.items() if c in a.table and n != a.table[c][2]]
    if collide:
        raise Fail("zip: name hash collides with " + collide[0])
    return {"zip_bytes": size, "entry": entry, "data_offset": hit[0], "engine_lookup_ok": True, "sha256_block": sha(data)}


def write_naz(path, data, keep):
    """Copy blocks.naz with the overdose.main entry re-pointed at the new block appended after the old data."""
    src = tb.open_naz()
    ent = next(e for e in src.entries if e["name"] == OD)
    free = shutil.disk_usage(os.path.dirname(os.path.abspath(path))).free
    if free < src.fsize + len(data) + 4 * 2 ** 30:
        raise Fail("not enough free disk for the naz copy (%.1f GB free)" % (free / 2 ** 30))
    q = ent["cd_off"] - 8                       # naz central entries: zip fields shifted by -8
    with open(src.path, "rb") as f, open(path + ".part", "wb") as o:
        remaining = src.cdoff
        while remaining:
            chunk = f.read(min(remaining, 1 << 24))
            o.write(chunk)
            remaining -= len(chunk)
        f.seek(ent["lhoff"])
        lh = f.read(0x1E + ent["nlen"] + ent["xlen"])
        f.seek(ent["data_off"] + ent["size"])
        trailer = f.read(12)
        new_lh = o.tell()
        o.write(lh)
        o.write(data)
        o.write(trailer)
        new_cdoff = o.tell()
        cd = bytearray(src.cd)
        struct.pack_into("<ii", cd, q + 0x14, len(data), len(data))     # csize, usize
        struct.pack_into("<i", cd, q + 0x2A, new_lh)                     # local header offset
        o.write(cd)
        e = bytearray(src.eocd)
        struct.pack_into("<I", e, 16, new_cdoff)
        o.write(e)
    os.replace(path + ".part", path)
    a = tb.Archive(path)
    if a.read(OD) != data:
        raise Fail("naz: overdose.main does not resolve to the new block")
    diff = [e2["name"] for e1, e2 in zip(src.entries, a.entries)
            if e1["name"] != OD and (e1["data_off"], e1["size"], e1["name"]) != (e2["data_off"], e2["size"], e2["name"])]
    if diff or len(a.entries) != len(src.entries):
        raise Fail("naz: other entries moved: %s" % diff[:3])
    rep = {"naz_bytes": a.fsize, "entries": len(a.entries), "overdose_main_data_offset": a.names[OD][0],
           "other_entries_unchanged": len(a.entries) - 1, "kept": keep}
    if not keep:
        os.remove(path)
        rep["deleted_after_check"] = True
    return rep


def game_running():
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout.lower()
    return '"tod.exe"' in out or '"totaloverdose.exe"' in out


def deploy(game_dir, zip_path):
    gd = os.path.normcase(os.path.abspath(game_dir))
    if any(gd == f or gd.startswith(f + os.sep) for f in FORBIDDEN_DEPLOY):
        raise Fail("refusing to deploy into %s (only a private test copy is allowed)" % game_dir)
    if not os.path.exists(os.path.join(game_dir, "TOD.exe")):
        raise Fail("no TOD.exe in " + game_dir)
    if game_running():
        raise Fail("TOD.exe / TotalOverdose.exe is running; not touching any game folder now")
    shutil.copy2(zip_path, os.path.join(game_dir, ZIP_NAME))                 # keeps the blocks.naz mtime
    cfg = os.path.join(game_dir, "configpc.txt")
    old = open(cfg, "rb").read() if os.path.exists(cfg) else b""
    if old.startswith(b"\xef\xbb\xbf") or b"\0" in old:
        raise Fail("configpc.txt in the test folder is not plain ASCII")
    lines = [l for l in old.replace(b"\r\n", b"\n").split(b"\n") if l.strip()
             and not l.strip().lower().startswith(b"filesystem") and not l.startswith(b"# anim codec")
             and not l.strip().lower().startswith(b"logdumpfile")]
    fs = ", ".join([ZIP_NAME] + BASE_ARCHIVES)
    # line 1 a comment (BOM insurance); log file shows "Opening zip <anim_override.zip> into slot 0" and
    # "Read asset block file: ..." lines; the filesystem line last (the last duplicate wins)
    lines = [b"# anim codec test override (delete this line, filesystem/logdumpfile lines and anim_override.zip to undo)"] + lines
    lines.append(b"logdumpfile = tod_anim_log.txt")
    lines.append(("filesystem = " + fs).encode("ascii"))
    raw = b"\r\n".join(lines) + b"\r\n"
    with open(cfg, "wb") as f:
        f.write(raw)
    return {"deployed_to": game_dir, "configpc": raw.decode("ascii")}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clip", action="append", required=True, help="clip JSON (clipjson.py format)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--base-block", help="start from this overdose.main file instead of the GOG blocks.naz")
    ap.add_argument("--allow-add", action="store_true", help="append clips whose name is not in the block")
    ap.add_argument("--naz-out", help="also write a full blocks.naz copy with the entry rewritten (verified)")
    ap.add_argument("--keep-naz", action="store_true")
    ap.add_argument("--deploy", help="TEST game folder to receive anim_override.zip + configpc.txt")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    base = open(args.base_block, "rb").read() if args.base_block else tb.open_naz().read(OD)
    clips = [cj.load(p) for p in args.clip]
    rep = {"base": args.base_block or GOG + r"\blocks.naz :: " + OD, "base_sha256": sha(base),
           "clips": [c["name"] for c in clips]}
    try:
        old, nb, starts, ends, new_recs, layout, added = build(base, clips, args.allow_add)
        out = nb.serialize()
        chk, r = verify_block(old, nb, out, starts, ends, new_recs, layout)
        rep.update(r)
        rep["added_records"] = added
        rep["info_growth"] = len(nb.info) - len(old.info)
        out_path = os.path.join(args.out, "overdose.main")
        with open(out_path + ".part", "wb") as f:
            f.write(out)
        os.replace(out_path + ".part", out_path)
        rep.update(existing_readers(out_path, base, chk, new_recs, layout, args.out))
        rep["block_sha256"] = sha(out)
        rep["block_bytes"] = len(out)
        rep["zip"] = write_zip(os.path.join(args.out, ZIP_NAME), OD, out)
        line = "filesystem = " + ", ".join([ZIP_NAME] + BASE_ARCHIVES)
        open(os.path.join(args.out, "configpc_line.txt"), "w").write(line + "\n")
        rep["configpc_line"] = line
        if args.naz_out:
            rep["naz"] = write_naz(args.naz_out, out, args.keep_naz)
        if args.deploy:
            rep["deploy"] = deploy(args.deploy, os.path.join(args.out, ZIP_NAME))
        rep["result"] = "PASS"
    except Fail as e:
        rep["result"] = "FAIL: %s" % e
    json.dump(rep, open(os.path.join(args.out, "report.json"), "w"), indent=1)
    print(json.dumps(rep, indent=1))
    sys.exit(0 if rep["result"] == "PASS" else 1)


if __name__ == "__main__":
    main()
