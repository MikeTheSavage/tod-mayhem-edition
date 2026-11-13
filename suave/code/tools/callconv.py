"""callconv.py - static check of the calling convention of every game function the Suave code calls (read only).

For each function: the stack bytes its `ret` instructions pop (all rets inside the function's extent from the Ghidra
function table must agree), whether it reads ecx before writing it (a thiscall / fastcall hint), and its first bytes
(the prologue that sites.h REF rows pin). The expected values live in EXPECT below; exit code 0 = all match.

usage: python -I callconv.py [--show]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'tests', 'dll'))
from disasm import PE, EXE  # noqa: E402
import capstone  # noqa: E402

FUNCS_TSV = os.environ.get('TOD_FUNCS_TSV', 'funcs.tsv')

# va: (name, expected ret pop bytes, ecx use: True = read as this first, 'fwd' = passed unchanged to the first call
# (thiscall forwarding), 'no' = written before any read (not a this pointer))
EXPECT = {
    0x869E60: ('Node::SendMessage(id, args)', 8, True),
    0x86A340: ('Node::SendCommand(global id, args)', 8, True),
    0x86A250: ('Node::CallNative(native index, args)', 8, True),
    0x86A2B0: ('Node::TriggerScript(builtin index, global id, args)', 12, True),
    0x86A6A0: ('Node::GetProperty(raw id, out)', 8, True),
    0x869E80: ('Node::GetMember(slot, out)', 8, True),
    0x86B4B0: ('Node::TouchMember(slot) (rewind record)', 4, True),
    0x486950: ('Quadtree::Query(query*) (static; caller sets ecx=[0xA3B578])', 4, 'no'),
    0x8718D0: ('ListType::Release(list**)', 4, True),
    0x88CA80: ('Node::GetWorldPosition(float out[3])', 4, 'fwd'),
    0x88C9C0: ('Node::LocalToWorldDir(float buf[6])', 4, 'fwd'),
    0x88E600: ('Node::SetWorldPosition(const float[3])', 4, True),
    0x891360: ('Node::SetRotationY(float)', 4, True),
    0x88EB60: ('Node::FindChild({out, name}*)', 4, True),
    0x89E270: ('SoundSlot::set_streaming_sound(const char*)', 4, True),
    0x5010A0: ('AnimLayer::SetSpeedMultiplier(float)', 4, True),
    0x50B490: ('AnimLayer::CurrentSlot()', 0, True),
    0x9159E0: ('Character::GetModelRes() (tail-jumps to 0x851720)', 0, True),
    0x851720: ('Resource::Name() (the jump target)', 0, True),
    0x5BCD90: ('move eligibility cdecl(thread, {result, slot}*)', 0, 'no'),
    0x91D020: ('Navigator::IsPointOccluded(from[4], to[4]) -> al blocked (this +70h = ignore)', 8, True),
}


def extents():
    out = {}
    with open(FUNCS_TSV, encoding='utf-8', errors='replace') as f:
        for ln in f:
            p = ln.rstrip('\n').split('\t')
            if len(p) >= 3:
                try:
                    out[int(p[0], 16)] = int(p[2])
                except ValueError:
                    pass
    return out


def analyse(pe, va, size):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    code = pe.read(va, size)
    rets, use = set(), None
    first = []
    for ins in md.disasm(code, va):
        if len(first) < 4:
            first.append('%s %s' % (ins.mnemonic, ins.op_str))
        if ins.mnemonic == 'ret':
            rets.add(int(ins.op_str, 0) if ins.op_str else 0)
        if use is None:
            regs_r, regs_w = ins.regs_access()
            names_r = {ins.reg_name(r) for r in regs_r}
            names_w = {ins.reg_name(r) for r in regs_w}
            if names_r & {'ecx', 'cx', 'cl', 'ch'}:
                use = True
            elif 'ecx' in names_w:
                use = 'no'
            elif ins.mnemonic == 'call':
                use = 'fwd'
    return sorted(rets), use, first, code[:9].hex()


def main():
    pe = PE(EXE)
    ext = extents()
    bad = 0
    for va, (name, want_ret, want_this) in sorted(EXPECT.items()):
        size = ext.get(va)
        if size is None and va == 0x9159E0:
            size = 21   # a registered native (no call site, so Ghidra made no function): 0x9159E0..0x9159F4 ret
        if size is None:
            print('%08X %-50s NOT A FUNCTION START in funcs.tsv' % (va, name))
            bad += 1
            continue
        rets, this, first, head = analyse(pe, va, size + 8)   # Ghidra extents sometimes stop before the ret
        ok = rets == [want_ret] and this == want_this
        bad += not ok
        print('%08X %-52s ret %-3s ecx %-4s %s  head %s' % (
            va, name, ','.join(map(str, rets)), this, 'OK' if ok else 'MISMATCH (want ret %d, this %s)' % (want_ret, want_this),
            head))
        if '--show' in sys.argv:
            print('          ' + ' | '.join(first))
    print('ALL OK' if not bad else '%d MISMATCH(ES)' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
