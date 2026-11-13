"""check_smart.py - static checks of the Smart AI glue (smart_ai/code/smart_game.cpp) against the GOG TOD.exe (read only).

  1. every SM_FIELDS row (field, member offset, touch slot) and every other member the glue writes (DontHunt, the CS
     pair written by the morale tick) is proven by an SM_SLOT_xx REF row of sites.h that decodes, in the exe, to
     'push slot; mov ecx, node; call 0x86B4B0 (touch)' followed by a store to [reg + offset];
  2. the offsets agree with the class registry layout of character_sentry_ai (a layout dump from the
     author's research notes, path in env TOD_SENTRY_LAYOUT, read if set; member index = touch slot);
  3. SM_REFS in smart_game.cpp lists exactly the SM_ REF rows of sites.h (plus the shared rows it names, which exist);
  4. calling conventions: 0x5D4190 is cdecl (the call site cleans 8 bytes: SM_INIT_OUT), 0x86B4B0 and 0x88CA80 end in
     'ret 4' (thiscall with one argument), the call site of SM_INIT targets 0x5D4190 = R_SM_INIT;
  5. the reads the tick relies on: Current_state +1D4h, CombatCtrl +234h (data +8 / +10h), DontHunt +F0h, Is_Melee +ACh,
     Health +2Ch, DamageTaken +30h, Wait_until_Aggressive +CCh, CS +68h/+6Ch, root +40h / +14h / +18h are named in a
     matching REF.
Exit code 0 = all OK. usage: python -I check_smart.py"""
import os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests", "dll"))
from disasm import PE, EXE
import capstone

SITES_H = os.path.join(ROOT, "dll", "src", "sites.h")
GAME = os.path.join(ROOT, "smart_ai", "code", "smart_game.cpp")
LOGIC_H = os.path.join(ROOT, "smart_ai", "code", "smart_logic.h")
LAYOUT = os.environ.get("TOD_SENTRY_LAYOUT", "")
REGNAME = {"WAIT_AGGR": "Wait_until_Aggressive", "RETREAT": "Retreat_Range", "STRAFE_FROM": "Strafe_time_from",
           "STRAFE_TO": "Strafe_time_to", "CROUCH_FROM": "Crouch_time_from", "CROUCH_TO": "Crouch_time_to",
           "WAIT_FROM": "Wait_time_from", "WAIT_TO": "Wait_time_to", "PRO_CROUCH": "ProCrouch", "PRO_STRAFE": "ProStrafe",
           "PRO_STRAFE_CROUCH": "ProStrafeCrouch", "PRO_SD": "ProShootDodge", "PRO_SD_STRAFE_CROUCH": "ProShootDodgeStrafeCrouch",
           "PRO_SD_CROUCH": "ProShootDodgeCrouch", "PRO_CROUCH_SD_STRAFE": "ProCrouchShootDodgeStrafe",
           "PRO_STRAFE_SD_STRAFE": "ProStrafeShootdodgeStrafe", "STOPDIST": "StopDistance", "SD_MIN": "ShootDodge_Min_dist",
           "CS_HEALTHLEFT": "CS_HealthLeft", "CS_CHANGE": "CS_Change_for"}
bad = 0
def ok(cond, msg):
    global bad
    print(("OK   " if cond else "BAD  ") + msg)
    if not cond: bad += 1

def main():
    pe = PE(EXE)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    sites = open(SITES_H, encoding="utf-8").read()
    game = open(GAME, encoding="utf-8").read()
    refs = {m[1]: (int(m[2], 16), m[3].lower()) for m in re.finditer(r'^REF\((\w+),\s*(0x[0-9A-Fa-f]+),\s*"([0-9a-fA-F]*)"', sites, re.M)}
    # ---- 1. slot / offset proofs
    def proof(slot, off):
        r = refs.get("SM_SLOT_%02X" % slot)
        if not r: return "no REF SM_SLOT_%02X" % slot
        va, hx = r
        if pe.read(va, len(hx) // 2).hex() != hx: return "REF bytes differ in the exe"
        ins = list(md.disasm(pe.read(va, len(hx) // 2), va))
        if not ins or ins[0].mnemonic != "push" or int(ins[0].op_str, 0) != slot: return "does not start with push %Xh" % slot
        if not any(i.mnemonic == "call" and i.op_str == "0x86b4b0" for i in ins[:4]): return "no touch call"
        last = ins[-1]
        dst = last.op_str.split(",")[0]
        if not (last.mnemonic in ("mov", "movss") and re.search(r"\+ %s\]$" % re.escape(hex(off)), dst)): return "last insn '%s %s' is not a store to +%Xh" % (last.mnemonic, last.op_str, off)
        return None
    fields = [(m[1], int(m[2], 16), int(m[3], 16)) for m in re.finditer(r"\{sm::F_(\w+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)\}", game)]
    enum = re.search(r"enum Field \{(.*?)\};", open(LOGIC_H, encoding="utf-8").read(), re.S)[1]
    enum_names = [n for n in re.findall(r"\bF_(\w+)", enum) if n != "COUNT"]
    ok(sorted(f[0] for f in fields) == sorted(enum_names), "SM_FIELDS covers every Field of smart_logic.h (%d)" % len(fields))
    for name, off, slot in fields:
        e = proof(slot, off)
        ok(e is None, "field %-22s +%03Xh slot %02Xh %s" % (name, off, slot, "proven by SM_SLOT_%02X" % slot if e is None else e))
    extra = [("DontHunt (squad)", int(re.search(r"S_DONTHUNT = (0x[0-9A-Fa-f]+)", game)[1], 16), int(re.search(r"SLOT_DONTHUNT = (0x[0-9A-Fa-f]+)", game)[1], 16))]
    for m in re.finditer(r"write_f\(e\.node, e\.data, (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+),", game):
        extra.append(("morale write", int(m[2], 16), int(m[1], 16)))
    ok(len(extra) >= 3 and {(o, s) for _, o, s in extra} == {(0xF0, 0x33), (0x68, 0x11), (0x6C, 0x12)}, "the tick writes only DontHunt and the two CS values (%d literal writes found)" % len(extra))
    for name, off, slot in extra:
        e = proof(slot, off)
        ok(e is None, "%-28s +%03Xh slot %02Xh %s" % (name, off, slot, "proven" if e is None else e))
    # read-only members named as constants
    for cname, slot in (("S_SUBTYPE", 1), ("S_HEALTH", 2), ("S_DAMAGE", 3), ("S_ISMELEE", 0x22), ("S_ISBUDDY", 0x25), ("S_MORALE", 0x2D), ("S_DONTHUNT", 0x33)):
        off = int(re.search(cname + r" = (0x[0-9A-Fa-f]+)", game)[1], 16)
        e = proof(slot, off)
        ok(e is None, "read %-10s +%03Xh (slot %02Xh) %s" % (cname, off, slot, "proven" if e is None else e))
    # ---- 2. registry layout
    if os.path.exists(LAYOUT):
        lay = {}
        for line in open(LAYOUT, encoding="utf-8"):
            m = re.match(r"p(\d+)\s+\+0x([0-9A-F]+)\s+raw=\d+\s+(?:b:)?(\S+)", line)
            if m: lay[m[3]] = (int(m[1]), int(m[2], 16))
        for name, off, slot in fields:
            reg = REGNAME.get(name)
            got = lay.get(reg)
            ok(got == (slot, off), "layout: %-26s member p%s +%s (glue: slot %d +%Xh)" % (reg, got and got[0], got and hex(got[1]), slot, off))
        for reg, off in (("Current_state", 0x1D4), ("DontHunt", 0xF0), ("Morale", 0xD8), ("Is_Melee", 0xAC), ("Health", 0x2C), ("DamageTaken", 0x30)):
            ok(lay.get(reg, (0, -1))[1] == off, "layout: %-14s at +%Xh" % (reg, off))
    else:
        print("--   layout file not found (%s): registry cross-check skipped" % LAYOUT)
    # ---- 3. REF list
    listed = re.search(r"SM_REFS\[\] = \{.*?\n(.*?)\};", game, re.S)[1]
    listed = set(re.findall(r'"(\w+)"', listed))
    sm_rows = {k for k in refs if k.startswith("SM_")}
    ok(sm_rows <= listed, "every SM_ REF row is checked at run time (%d rows)" % len(sm_rows))
    missing = [k for k in listed if k not in refs]
    ok(not missing, "every REF the glue checks exists in sites.h%s" % (": missing " + ", ".join(missing) if missing else " (%d)" % len(listed)))
    for k in sorted(listed):
        if k in refs:
            va, hx = refs[k]
            ok(pe.read(va, len(hx) // 2).hex() == hx, "REF %-20s %08X bytes match" % (k, va))
    # ---- 4. calling conventions and the site's target
    def first_ret(va, limit=0x2000):
        for i in md.disasm(pe.read(va, limit), va):
            if i.mnemonic == "ret": return i.op_str
        return None
    ok(first_ret(0x86B4B0) == "4", "0x86B4B0 touch: first ret is 'ret 4' (thiscall, 1 arg)")
    ok(first_ret(0x88CA80) == "4", "0x88CA80 GetWorldPosition: first ret is 'ret 4' (thiscall, 1 arg)")
    ok(first_ret(0x5D4190, 0x1000) == "", "0x5D4190 AI init: plain ret (cdecl)")
    site = re.search(r'^SITE\(SM_INIT,\s*"[^"]*",\s*(0x[0-9A-Fa-f]+),\s*"([0-9a-f]+)",\s*(\d+)', sites, re.M)
    va = int(site[1], 16) + int(site[3])
    call = list(md.disasm(pe.read(va, 5), va))[0]
    r_init = int(re.search(r"R_SM_INIT = (0x[0-9A-Fa-f]+)", game)[1], 16)
    ok(call.mnemonic == "call" and int(call.op_str, 16) == r_init == 0x5D4190, "SM_INIT %08X: %s %s = R_SM_INIT" % (va, call.mnemonic, call.op_str))
    after = list(md.disasm(pe.read(va + 5, 8), va + 5))
    ok(after[1].mnemonic == "add" and after[1].op_str == "esp, 8", "SM_INIT: caller cleans 8 bytes (2 cdecl args) after the call")
    print("check_smart: %s" % ("ALL OK" if not bad else "%d PROBLEM(S)" % bad))
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
