"""check_sites.py - static verification of every TODGameplay.dll patch site against the GOG TOD.exe (read only).

For every SITE and REF row of dll/src/sites.h:
  * the shipped bytes in the exe file equal the row's bytes;
  * the patch range starts and ends on instruction boundaries (capstone, decoding from check_va);
  * CALL rows: the patched instruction is a 5-byte call; its original target is printed and must equal the stub's
    tail-call / original-call constant in gp_game.cpp; JMP rows: the stub's return constant equals the patch end
    (or the original jmp target for DIAG);
  * no two sites overlap, and no site overlaps a hook site of the renderer bridge (dinput8) except the known shared
    culling operand (CAM_ASPECT, documented in HOOKS.md);
and prints the original instructions (the review listing used for HOOKS.md).
Exit code 0 = all OK. usage: python -I check_sites.py [--listing]"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import PE, EXE
import capstone

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SITES_H = os.path.join(ROOT, "dll", "src", "sites.h")
GAME_CPP = os.path.join(ROOT, "dll", "src", "gp_game.cpp")
SUAVE_CPP = os.path.join(ROOT, "suave", "code", "suave_game.cpp")   # the Suave module (R_TOUCH, R_USE_EXIT)
SMART_CPP = os.path.join(ROOT, "smart_ai", "code", "smart_game.cpp")   # Smart AI (R_SM_INIT)
SEAM_CPP = os.path.join(ROOT, "seamless", "code", "seam_game.cpp")     # seamless border swaps (R_SL_*)
FIX_CPP = os.path.join(ROOT, "dll", "src", "gp_fix.cpp")                # engine fixes (R_FIX_*)
DISP_CPP = os.path.join(ROOT, "dll", "src", "gp_display.cpp")           # [display] (R_DS_*)
DM_CPP = os.path.join(ROOT, "dll", "src", "gp_display_modes.cpp")       # [display] mode_menu (R_DM_*)
PAD_CPP = os.path.join(ROOT, "dll", "src", "gp_pad.cpp")                # [pad] (R_PADPRESENT, R_CTLSTART, ...)
PHYS_CPP = os.path.join(ROOT, "physics", "code", "phys_game.cpp")      # [physics] (R_PH_*)
MENU_CPP = os.path.join(ROOT, "dll", "src", "gp_menu.cpp")              # menu hooks (R_MN_*)
DIFF_CPP = os.path.join(ROOT, "dll", "src", "gp_difficulty.cpp")        # [difficulty] (P_DF_*)
MT_CPP = os.path.join(ROOT, "dll", "src", "gp_overlay.cpp")             # shared overlay hooks (R_MT_*, R_OV_FRAME)
MTT_CPP = os.path.join(ROOT, "dll", "src", "gp_mayhem.cpp")             # [mayhem_title]: its own TextBox hook (R_MT_TEXTBOX)
HUD_CPP = os.path.join(ROOT, "dll", "src", "gp_hud.cpp")                # [minimap] (P_HR_*)
HT_CPP = os.path.join(ROOT, "dll", "src", "gp_heat.cpp")                # [mayhem_heat] (R_HT_*)
MH_CPP = os.path.join(ROOT, "mayhem", "code", "mh_game.cpp")             # [mayhem] chain reactions / cover (R_MH_*)
RD_CPP = os.path.join(ROOT, "ragdoll", "code", "rd_game.cpp")          # [ragdoll] (R_CHARUPD, R_HITSEND)
EXPECTED_MD5 = "d9beb351b3c78ee9f14a766a368f3ff9"

# stub return / tail constants in gp_game.cpp that must match the sites
STUB_CONST = {   # site id -> (name of the DWORD constant in gp_game.cpp, "end" | "target")
    "LOOP_TOP": ("R_INACTIVE", "target"), "POST_INPUT": ("R_PADPOLL", "target"),
    "ARM_DIVE1": ("R_SENDMSG", "target"), "ARM_DIVE2": ("R_SENDMSG", "target"), "ARM_DIVE3": ("R_SENDMSG", "target"),
    "ARM_WIRE": ("R_SENDMSG", "target"), "MOUSE_DX": ("R_MDX", "end"), "MOUSE_DY": ("R_MDY", "end"),
    "YAW_WRITE": ("R_YAWW", "end"), "YAW_PAD": ("R_YAWP", "end"), "YAW_READ": ("R_YAWR", "end"),
    "DIAG": ("R_DIAG", "target"), "PICKUP": ("R_PICKUP", "end"), "CAM_ENTRY": ("R_CAM", "end"),
    "KB_PROC": ("P_KBPROC", "target"), "PAD_PROC": ("P_PADPROC", "target"),
    "CLK_GAME": (None, "target"), "CLK_REWIND": (None, "target"), "CLK_REAL": (None, "target"),
    "STEER_RET": ("R_STEER", "end"),
    "USE_GATE_A": ("R_TOUCH", "target"), "USE_GATE_B": ("R_TOUCH", "target"), "USE_GATE_G": ("R_TOUCH", "target"),
    "SM_INIT": ("R_SM_INIT", "target"),
    "SL_WALKREQ": ("R_SL_SENDCMD", "target"), "SL_LSPUSH": ("R_SL_COPUSH", "target"), "SL_LSSKIP": ("R_SL_LS_BACK", "end"), "SL_LSSKIP2": ("R_SL_LS2_BACK", "end"),
    "SL_POSTFADE": ("R_SL_POST_BACK", "end"), "SL_FADEIN": ("R_SL_SENDMSG", "target"), "SL_SWAP": ("R_SL_SWAP", "target"),
    "SL_SWAP_RG": ("R_SL_SWAP", "target"),
    "FIX_VEC_FREE": ("R_FIX_FREE", "target"), "FIX_WPN_OWNER": ("R_WO_CONT", "end"),
    "DS_RES_START": ("R_DS_RESTAB", "target"), "DS_WH_DIM": ("R_DS_ISWIDE", "target"), "DS_WH_DOT": ("R_DS_ISWIDE", "target"),
    "DS_SPR_MTX": ("R_DS_MATRIX", "target"), "DS_SPR2_MTX": ("R_DS_MATRIX", "target"),
    "DS_TXT_SCALE_A": ("R_DS_FONTSCALE", "target"), "DS_TXT_SCALE_B": ("R_DS_FONTSCALE", "target"),
    "DS_TXT_A_ROT": ("R_DS_TA_ROT", "end"), "DS_TXT_A_AX": ("R_DS_TA_AX", "end"), "DS_TXT_B_ROT": ("R_DS_TB_ROT", "end"),
    "DS_TXT_B_AX": ("R_DS_TB_AX", "end"), "DS_MOUSE": ("R_DS_MOUSE", "end"), "DS_LOADBAR": ("R_DS_QUAD", "target"),
    "DS_FS_LOOKUP": ("R_DS_MODEFIND", "target"), "DS_SR_LOOKUP": ("R_DS_MODEFIND", "target"), "DS_BL_INIT": ("R_DS_FSINIT", "target"),
    "DS_BL_WIN1": ("R_DS_WINSIZE", "target"), "DS_BL_WIN2": ("R_DS_WINSIZE", "target"),
    "PD_SEL_PROC": ("R_PADPRESENT", "target"), "PD_SEL_LOOK": ("R_PADPRESENT", "target"), "PD_SEL_CMD": ("R_PADPRESENT", "target"),
    "PD_TXTPARAM": ("R_TXTPARAM", "target"), "PD_EXPAND1": ("R_EXPAND", "target"), "PD_EXPAND2": ("R_EXPAND", "target"),
    "PD_EXPAND3": ("R_EXPAND", "target"), "PD_VIBRATE": ("R_VIBRATE", "target"), "PD_SHOT": ("R_SHOT_BACK", "end"),
    "MT_EXEC_A": ("R_MT_EXEC", "target"), "MT_EXEC_B": ("R_MT_EXEC", "target"), "OV_FRAME": ("R_OV_FRAME", "target"),
    "HT_KILL_E": ("R_HT_TOUCH", "target"), "HT_KILL_N": ("R_HT_TOUCH", "target"), "HT_CAR": ("R_HT_SENDMSG", "target"),
    "HT_DESTROY": ("R_HT_DESTROY_BACK", "end"), "HT_SCORE": ("R_HT_SCORE", "target"),
    "PH_BULLET": ("R_PH_LINE", "target"), "PH_SHELL": ("R_PH_SHELL", "target"), "PH_DESTROY": ("R_PH_DESTROY", "target"),
    "DS_MM_WININIT": ("R_DM_WININIT", "target"), "DS_MM_APPLY": ("R_DM_TABLE", "target"), "DS_MM_REVERT": ("R_DM_TABLE", "target"),
    "DS_MM_OLDGET": ("R_DM_MGET", "target"), "DS_MM_STORE": ("R_DM_MSET", "target"), "DS_MM_SETTEXT": ("R_DM_SETIDX", "end"),
    "MH_CAR_A": ("R_MH_CAR", "target"), "MH_CAR_B": ("R_MH_CAR", "target"), "MH_DO_HIT": ("R_MH_DOHIT", "target"), "MH_DRAW": ("R_MH_BLUR", "target"), "MH_DO_QUERY": ("R_MH_QUERY", "target"),
    "RD_CHARUPD": ("R_CHARUPD", "target"), "RD_HIT": ("R_HITSEND", "target"), "RD_CORPSE": ("R_CORPSE_BACK", "end"),
    "MN_GET_INIT": ("R_MN_GETTER", "target"), "MN_GET_RESET": ("R_MN_GETTER", "target"), "MN_SET": ("R_MN_SET", "end"),
}
# OPER sites that replace an immediate code address: the shipped immediate must equal the constant the wrapper calls
OPER_IMM_CONST = {"PD_CTLSTART": "R_CTLSTART"}
# DATA sites (a pointer in a data section): the 4 shipped bytes are a code address; the constant must equal it
DATA_CONST = {"SL_PROGRESS": "R_SL_PROG", "MT_SPRITE_VT": "R_MT_SPRITE", "MT_TEXTBOX_VT": "R_MT_TEXTBOX"}
# seamless jump targets that must be the REF rows proving them
JUMP_REF = {"R_SL_LS_EXIT": "SL_LS_EXIT84", "R_SL_POST_HUD": "SL_POST_HUD", "R_SL_LS_TEXT": "SL_LS_TEXT", "R_SL_TOUCH": "F_TOUCH", "R_FIX_CRT_REALLOC": "FIX_DEF_RLC", "R_WO_SKIP": "FIX_WPN_SKIP",
            "R_DS_TA_ROT_J": "DS_TA_ROT_J", "R_DS_TA_AX_J": "DS_TA_AX_J", "R_DS_TB_ROT_J": "DS_TB_ROT_J", "R_DS_TB_AX_J": "DS_TB_AX_J",
            "R_DS_MOUSE": "DS_MOUSE_J", "R_DS_QUAD": "DS_QUAD", "R_DS_MATRIX": "DS_MATRIX", "R_DS_FONTSCALE": "DS_FONTSCALE",
            "R_DS_ISWIDE": "DS_ISWIDE", "R_DS_MODEFIND": "DS_MODEFIND", "R_DS_WININIT": "DS_WININIT", "R_DS_WINSIZE": "DS_WINSIZE",
            "P_DM_RESET": "DS_MM_RESET", "P_DM_VHUD": "DS_MM_VHUD", "P_DM_ALLOC": "DS_MM_ALLOC",
            "P_DM_SENDCMD": "F_CMD", "P_DM_FIND": "F_FINDCHILD"}
# renderer bridge (todbridge.cpp, not included) patch ranges: va -> length
BRIDGE = {0x93D027: 5, 0x884EB0: 6, 0x911EA0: 6, 0x746C40: 8, 0x748840: 8, 0x8986E0: 6, 0x7478E0: 5, 0x421B30: 5,
          0x421530: 8, 0x434290: 6, 0x44F940: 6, 0x45D5E0: 5, 0x748AAC: 4, 0x445E8F: 4, 0x441837: 4, 0x44CFF0: 3,
          0x45BF90: 8, 0x87E04A: 4, 0x87E054: 4}
KNOWN_SHARED = {("CAM_ASPECT", 0x87E054)}
DUMMY = "00200010"   # 0x10002000, a DLL-like address, little endian
OPER_NEW = {"CAM_VIEW": "a1" + DUMMY + "90", "CAM_CULL": "05" + DUMMY, "CAM_ASPECT": DUMMY,
            "SBRAKE_A": "e94b01000090", "SBRAKE_B": "eb1e"}
OPER_WANT = {"CAM_VIEW": ("mov eax, dword ptr [0x10002000]; nop", 2), "CAM_CULL": ("movss xmm0, dword ptr [0x10002000]", 1),
             "CAM_ASPECT": ("movss xmm0, dword ptr [0x10002000]", 1),
             "SBRAKE_A": ("jmp 0x4f6ed5; nop", 2), "SBRAKE_B": ("jmp 0x4f6f63", 1)}
# [display] operand sites: imm32 of a mov [esp+N], imm (sizes / HUD canvas) or the address of a movss xmm0, [abs]
for _i, _v in enumerate((1280, 720, 1600, 900, 1920, 1080, 2560, 1440, 1920, 1080)):
    _id = "DS_RES_%s%d" % ("W" if _i % 2 == 0 else "H", _i // 2)
    OPER_NEW[_id] = _v.to_bytes(4, "little").hex(); OPER_WANT[_id] = ("mov dword ptr [esp + %d], 0x%x" % (4 if _i % 2 == 0 else 8, _v), 1)
OPER_NEW["PD_CTLSTART"] = DUMMY; OPER_WANT["PD_CTLSTART"] = ("mov eax, 0x10002000", 1)
OPER_NEW["DS_WH_H480"] =(480).to_bytes(4, "little").hex(); OPER_WANT["DS_WH_H480"] = ("mov dword ptr [esp + 0xc], 0x1e0", 1)
OPER_NEW["DS_WH_W853"] = (853).to_bytes(4, "little").hex(); OPER_WANT["DS_WH_W853"] = ("mov dword ptr [esp + 0xc], 0x355", 1)
for _id in ("DS_WH_SX", "DS_WH_SY", "DS_WH_DOTX", "DS_WH_DOTY", "DS_WH_BORDER"):
    OPER_NEW[_id] = DUMMY; OPER_WANT[_id] = ("movss xmm0, dword ptr [0x10002000]", 1)


def parse():
    txt = open(SITES_H, encoding="utf-8").read()
    sites, refs = [], []
    for m in re.finditer(r'^SITE\((\w+),\s*"([^"]*)",\s*(0x[0-9A-Fa-f]+),\s*"([0-9a-fA-F]*)",\s*(\d+),\s*(\d+),\s*(\w+),\s*"([^"]*)"\)', txt, re.M):
        sites.append(dict(id=m[1], feature=m[2], va=int(m[3], 16), hex=m[4].lower(), off=int(m[5]), len=int(m[6]), kind=m[7], what=m[8]))
    for m in re.finditer(r'^REF\((\w+),\s*(0x[0-9A-Fa-f]+),\s*"([0-9a-fA-F]*)",\s*"([^"]*)"\)', txt, re.M):
        refs.append(dict(id=m[1], va=int(m[2], 16), hex=m[3].lower(), why=m[4]))
    consts = {}
    src = open(GAME_CPP, encoding="utf-8").read() + "\n" + open(SUAVE_CPP, encoding="utf-8").read() + "\n" + open(SMART_CPP, encoding="utf-8").read()
    src += "\n" + open(SEAM_CPP, encoding="utf-8").read() + "\n" + open(FIX_CPP, encoding="utf-8").read() + "\n" + open(DISP_CPP, encoding="utf-8").read()
    src += "\n" + open(PAD_CPP, encoding="utf-8").read()
    src += "\n" + open(PHYS_CPP, encoding="utf-8").read()
    src += "\n" + open(DM_CPP, encoding="utf-8").read()
    src += "\n" + open(MENU_CPP, encoding="utf-8").read() + "\n" + open(DIFF_CPP, encoding="utf-8").read()
    src += "\n" + open(MT_CPP, encoding="utf-8").read() + "\n" + open(HUD_CPP, encoding="utf-8").read()
    src += "\n" + open(MTT_CPP, encoding="utf-8").read()   # [mayhem_title]'s own TextBox hook (R_MT_TEXTBOX)
    src += "\n" + open(HT_CPP, encoding="utf-8").read()   # [mayhem_heat] (R_HT_*)
    src += "\n" + open(MH_CPP, encoding="utf-8").read()
    src += "\n" + open(RD_CPP, encoding="utf-8").read()
    for m in re.finditer(r'\b(R_\w+|P_\w+)\s*=\s*(0x[0-9A-Fa-f]+|P_\w+)', src):
        consts[m[1]] = m[2]
    for k, v in list(consts.items()):
        if not v.startswith("0x"): consts[k] = consts.get(v, v)
    return sites, refs, {k: int(v, 16) for k, v in consts.items() if v.startswith("0x")}

def main():
    listing = "--listing" in sys.argv
    import hashlib
    pe = PE(EXE)
    md5 = hashlib.md5(pe.data).hexdigest()
    print("exe %s (%d bytes, md5 %s)" % (EXE, len(pe.data), md5))
    bad = 0
    if md5 != EXPECTED_MD5:
        print("FAIL: not the GOG TOD.exe"); return 1
    sites, refs, consts = parse()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    ranges = []
    for s in sites:
        n = len(s["hex"]) // 2
        got = pe.read(s["va"], n).hex()
        ok = got == s["hex"]
        insns = list(md.disasm(pe.read(s["va"], n + 16), s["va"]))
        starts = {i.address for i in insns}
        ends = {i.address + i.size for i in insns}
        a, b = s["va"] + s["off"], s["va"] + s["off"] + s["len"]
        bound = a in starts and b in ends
        covered = [i for i in insns if i.address >= a and i.address < b]
        cover_end = max(i.address + i.size for i in covered) if covered else a
        bound = bound and cover_end == b and s["off"] + s["len"] <= n
        note = ""
        if s["kind"] == "DATA":   # a data pointer, not code: the value must be the constant's original target
            sec = [x for x in pe.secs if x[1] <= s["va"] < x[1] + x[2]]
            bound = s["off"] == 0 and s["len"] == 4 and n == 4 and bool(sec) and sec[0][0] != ".text"
            val = int.from_bytes(pe.read(s["va"], 4), "little")
            cname = DATA_CONST.get(s["id"])
            if not cname or consts.get(cname) != val: ok = False; note += " %s=%s but the slot holds %08X;" % (cname, hex(consts.get(cname, 0)), val)
            note += " data pointer in %s, original value %08X" % (sec[0][0] if sec else "?", val)
            insns = []
        if s["kind"] == "OPER":   # the patch lies inside one instruction (or covers whole ones); decode the patched form
            host = [i for i in insns if i.address <= a < i.address + i.size][0]
            bound = b <= host.address + host.size or bound
            if s["id"] in OPER_NEW:
                orig = bytearray(pe.read(host.address, host.size + 8))
                new = bytes.fromhex(OPER_NEW[s["id"]])
                orig[a - host.address:a - host.address + len(new)] = new
                pi = list(md.disasm(bytes(orig), host.address))
                want_mn, want_n = OPER_WANT[s["id"]]
                got_txt = "; ".join("%s %s" % (i.mnemonic, i.op_str) for i in pi[:want_n])
                end_after = pi[want_n - 1].address + pi[want_n - 1].size if len(pi) >= want_n else 0
                if not got_txt.startswith(want_mn) or end_after != host.address + host.size:
                    ok = False; note += " patched form decodes as '%s';" % got_txt
                else: note += " patched form: %s" % got_txt
            if s["id"] in OPER_IMM_CONST:
                imm = int.from_bytes(pe.read(a, 4), "little"); cname = OPER_IMM_CONST[s["id"]]
                if consts.get(cname) != imm: ok = False; note += " %s=%s but the immediate is %08X;" % (cname, hex(consts.get(cname, 0)), imm)
                else: note += " original immediate %08X = %s" % (imm, cname)
        if s["kind"] in ("CALL", "JMP") and s["len"] < 5: bound = False
        first = covered[0] if covered else None
        if s["kind"] == "CALL":
            if not (first and first.mnemonic == "call" and first.size == 5): ok = False; note += " patched insn is not a 5-byte call;"
            tgt = int(first.op_str, 16) if first else 0
            cname, mode = STUB_CONST.get(s["id"], (None, None))
            if cname and consts.get(cname) != tgt: ok = False; note += " %s=%s but the call targets %08X;" % (cname, hex(consts.get(cname, 0)), tgt)
            note += " original target %08X" % tgt
        if s["kind"] == "JMP":
            cname, mode = STUB_CONST.get(s["id"], (None, None))
            if cname:
                want = b if mode == "end" else int(first.op_str, 16)
                if consts.get(cname) != want: ok = False; note += " %s=%s, expected %08X;" % (cname, hex(consts.get(cname, 0)), want)
                note += " stub returns to %08X" % want
        for (ra, rb, rid) in ranges:
            if a < rb and ra < b: ok = False; note += " overlaps %s;" % rid
        for bva, bl in BRIDGE.items():
            if a < bva + bl and bva < b and (s["id"], bva) not in KNOWN_SHARED: ok = False; note += " overlaps a bridge patch at %06X;" % bva
            if a < bva + bl and bva < b and (s["id"], bva) in KNOWN_SHARED: note += " (shares the bridge's cull operand %06X: first one wins, see HOOKS.md)" % bva
        ranges.append((a, b, s["id"]))
        status = "OK " if ok and bound else "BAD"
        if not (ok and bound): bad += 1
        print("%s SITE %-10s %-11s %08X+%d len %d %-4s bytes %s%s%s" % (status, s["id"], s["feature"], s["va"], s["off"], s["len"], s["kind"],
              "match" if got == s["hex"] else "DIFFER (%s)" % got, "" if bound else " NOT ON INSTRUCTION BOUNDARIES", note))
        if listing:
            for i in insns:
                if i.address >= s["va"] + n: break
                mark = ">>" if a <= i.address < b else "  "
                print("      %s %08X  %-22s %s %s" % (mark, i.address, i.bytes.hex(), i.mnemonic, i.op_str))
    for r in refs:
        n = len(r["hex"]) // 2
        got = pe.read(r["va"], n).hex() if n else ""
        ok = got == r["hex"]
        if not ok: bad += 1
        print("%s REF  %-12s %08X %s  %s" % ("OK " if ok else "BAD", r["id"], r["va"], "match" if ok else "DIFFER (%s)" % got, r["why"]))
    # the Suave gate refuses by jumping to R_USE_EXIT: it must be the use handler's common exit (REF SV_USE_EXIT)
    exit_ref = [r for r in refs if r["id"] == "SV_USE_EXIT"]
    if not exit_ref or consts.get("R_USE_EXIT") != exit_ref[0]["va"]:
        bad += 1; print("BAD  R_USE_EXIT (%s) is not the SV_USE_EXIT reference" % hex(consts.get("R_USE_EXIT", 0)))
    else: print("OK   R_USE_EXIT = %08X = SV_USE_EXIT (use_power_up common exit)" % consts["R_USE_EXIT"])
    # the seamless stubs jump to code proven by REF rows (same function, same frame as the patched site)
    for cname, rid in JUMP_REF.items():
        rr = [r for r in refs if r["id"] == rid]
        if not rr or consts.get(cname) != rr[0]["va"]:
            bad += 1; print("BAD  %s (%s) is not the %s reference" % (cname, hex(consts.get(cname, 0)), rid))
        else: print("OK   %s = %08X = %s" % (cname, consts[cname], rid))
    gen_ref = [r for r in refs if r["id"] == "SV_USE_GENERIC"]
    if not gen_ref or consts.get("R_USE_GENERIC") != gen_ref[0]["va"]:
        bad += 1; print("BAD  R_USE_GENERIC (%s) is not the SV_USE_GENERIC reference" % hex(consts.get("R_USE_GENERIC", 0)))
    else: print("OK   R_USE_GENERIC = %08X = SV_USE_GENERIC (generic branch after its touch: slot-10 Suave continues here)" % consts["R_USE_GENERIC"])
    print("%d sites, %d refs: %s" % (len(sites), len(refs), "ALL OK" if not bad else "%d PROBLEM(S)" % bad))
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
