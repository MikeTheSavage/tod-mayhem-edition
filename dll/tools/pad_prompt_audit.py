"""pad_prompt_audit.py - every PC prompt that names a key, and the pad picture [pad] shows for it before and after the
per-text action hints (dll/src/gp_pad_prompts.cpp), against the button that works (the action's console binding).

  python -I pad_prompt_audit.py CONTROLDICT.json STRINGS.tsv OUT.tsv

CONTROLDICT.json: the ControlDict.fragment entity dump (tools/*.py level-layout extractor, overdose.main record 796).
STRINGS.tsv: id / file / ordinal / key / value of the uk text slots. Reads nothing of the game itself.

Rules modelled (the DLL's, default ini, no [pad_bind]):
  * pad buttons = the XBox ControlSetup's virtual PS2 buttons, merged into the PC table; the DLL's defaults put
    menu_show on START and leave the console stats pause unbound
  * virtual -> physical (default map_*): L1<-LT R1<-RT L2<-LB R2<-RB CROSS<-A CIRCLE<-B SQUARE<-X TRIANGLE<-Y
    START<-START SELECT<-BACK, sticks / d-pad as they are; the picture is the PHYSICAL button (PlayStation names)
  * [%N] gets the KEY of the action the script means (its remappable PC key); BEFORE = the first PC action bound to
    that key that has a pad binding (table order), AFTER = the hinted action (gp_pad_prompts.cpp HINTS)
  * literals: BEFORE [Enter] / [Space] -> A, [Esc] -> B, [A] -> Y; AFTER menu_select / tutorial_skip, menu_back (or the
    text's esc hint), menu_delete
"""
import json, re, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
PS = {"A": "CROSS", "B": "CIRCLE", "X": "SQUARE", "Y": "TRIANGLE", "LB": "L1", "RB": "R1", "LT": "L2", "RT": "R2",
      "START": "OPTIONS", "BACK": "SHARE", "L3": "L3", "R3": "R3", "UP": "D-UP", "DOWN": "D-DOWN", "LEFT": "D-LEFT",
      "RIGHT": "D-RIGHT", "LSTICK": "LSTICK", "RSTICK": "RSTICK"}
VIRT = {"L1": "LT", "R1": "RT", "L2": "LB", "R2": "RB", "CROSS": "A", "CIRCLE": "B", "SQUARE": "X", "TRIANGLE": "Y",
        "START": "START", "SELECT": "BACK", "L3": "L3", "R3": "R3", "D_UP": "UP", "D_DOWN": "DOWN", "D_LEFT": "LEFT",
        "D_RIGHT": "RIGHT", "A_UP": "LSTICK", "A_DOWN": "LSTICK", "A_LEFT": "LSTICK", "A_RIGHT": "LSTICK",
        "C_UP": "RSTICK", "C_DOWN": "RSTICK", "C_LEFT": "RSTICK", "C_RIGHT": "RSTICK"}
SLOTFILE = {"Tutorial_PC": "uk_Tutorial_PC.txt", "Misc_PC": "uk_Misc_PC.txt", "Menu": "uk_Menu.txt"}


def hints():
    src = open(os.path.join(HERE, "..", "src", "gp_pad_prompts.cpp"), encoding="utf-8").read()
    src = src.replace("{LOCO}", '{"change_power_up_left", "change_power_up_right", "use_powerup"}')
    out = {}
    for m in re.finditer(r'\{SLOT_(TUTORIAL_PC|MISC_PC), (\d+), \{([^}]*)\}, (nullptr|"[^"]*")\},\s*//\s*(.*)', src):
        slot = {"TUTORIAL_PC": "Tutorial_PC", "MISC_PC": "Misc_PC"}[m.group(1)]
        params = [p.strip().strip('"') for p in m.group(3).split(",") if p.strip() and p.strip() != "nullptr"]
        esc = None if m.group(4) == "nullptr" else m.group(4).strip('"')
        out[(slot, int(m.group(2)))] = (params, esc, m.group(5).strip())
    return out


def main():
    cd = json.load(open(sys.argv[1], encoding="utf-8"))
    pad = {}        # action -> first virtual button
    pc = []         # (action, key) in PC table order
    for e in cd["entities"]:
        p, pr = e["path"], e.get("props", {})
        if "keystr" not in pr:
            continue
        if p.startswith("XBox/") and "/cheats/" not in p:
            pad.setdefault(pr["name"], pr["keystr"])
        elif p.startswith("PC/"):
            pc.append((pr["name"], pr["keystr"], pr.get("AllowReMapping", True)))
    pad["menu_show"] = "START"
    pad.pop("pause", None)

    def glyph(action):
        v = pad.get(action)
        return PS[VIRT[v]] if v else None

    def keys_of(action):   # the remappable PC keys (the ones the controls menu shows), else all
        ks = [k for a, k, rm in pc if a.lower() == action.lower()]
        rk = [k for a, k, rm in pc if a.lower() == action.lower() and rm]
        out = []
        for k in rk or ks:
            if k not in out:
                out.append(k)
        return out

    def old_rule(key):
        for a, k, rm in pc:
            if k == key and glyph(a):
                return a
        return None

    strings = {}
    for line in open(sys.argv[2], encoding="utf-8"):
        f = line.rstrip("\n").split("\t")
        if len(f) >= 5 and f[1] in SLOTFILE.values():
            strings[(f[1], int(f[2]))] = f[4]
    H = hints()
    rows = []
    LIT_OLD = {"[Enter]": "menu_select", "[Space]": "tutorial_skip", "[Esc]": "menu_back", "[A]": "menu_delete"}
    LIT_OLD_BTN = {"[Enter]": "CROSS", "[Space]": "CROSS", "[Esc]": "CIRCLE", "[A]": "TRIANGLE"}
    for slot, fn in SLOTFILE.items():
        for (f, o), text in sorted(strings.items(), key=lambda x: x[0][1]):
            if f != fn:
                continue
            params = sorted(set(int(n) for n in re.findall(r"\[%(\d)\]", text)))
            lits = re.findall(r"\[(?:Enter|Esc|A|Space)\]", text)
            if not params and not lits:
                continue
            h = H.get((slot, o))
            short = text.replace("\\n", " ").replace("\t", " ")
            short = short[:70] + ("..." if len(short) > 70 else "")
            for n in params:
                want = h[0][n] if h and n < len(h[0]) else None
                act = want.split("|")[0] if want else None
                keys = keys_of(act) if act else []
                befores = [old_rule(k) for k in keys]
                after_act = None
                if want:
                    after_act = act
                    for k in keys[:1]:
                        cands = [a for a, kk, rm in pc if kk == k and glyph(a)]
                        for w in want.split("|"):
                            if any(c.lower() == w.lower() for c in cands):
                                after_act = w
                                break
                if (slot, o) == ("Misc_PC", 86):   # the HUD use prompt: the DLL reads the HUD's vehicle / use flags
                    for ctx, a2 in (("next to a vehicle", "use_vehicle"), ("other usable (save point ...)", "use")):
                        rows.append([slot, o, "[%%%d]" % n, short + " (" + ctx + ")", a2, " / ".join(keys),
                                     " / ".join(b or "-" for b in befores), " / ".join(glyph(b) or "-" for b in befores),
                                     a2, glyph(a2), glyph(a2), "HUD flags data +38h / +24h"])
                    continue
                rows.append([slot, o, "[%%%d]" % n, short, act or "?", " / ".join(keys) or "(none on PC)",
                             " / ".join(b or "-" for b in befores) or "?",
                             " / ".join(glyph(b) or "-" for b in befores) or "?",
                             after_act or "-", glyph(after_act) or "-", glyph(act) or "-", (h[2] if h else "no hint")])
            for lit in sorted(set(lits)):
                aact = (h[1] if h and h[1] and lit == "[Esc]" else None) or LIT_OLD[lit]
                rows.append([slot, o, lit, short, aact, lit.strip("[]"), "(fixed)", LIT_OLD_BTN[lit], aact,
                             glyph(aact), glyph(aact), (h[2] if h else "menu default")])
    hdr = ["slot", "ordinal", "param", "text", "action meant", "PC key", "before: action", "before: glyph",
           "after: action", "after: glyph", "works (action's pad button)", "source"]
    with open(sys.argv[3], "w", encoding="utf-8", newline="\n") as f:
        f.write("\t".join(hdr) + "\n")
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")
    bad_b = sum(1 for r in rows if r[7] != "?" and any(g != r[10] for g in r[7].split(" / ")))
    bad_a = sum(1 for r in rows if r[9] != r[10])
    unk = sum(1 for r in rows if r[7] == "?")
    print("%d prompt parameters / literals; wrong picture before %d (+%d with no PC key: not known), after %d"
          % (len(rows), bad_b, unk, bad_a))
    for r in rows:
        if r[7] == "?" or any(g != r[10] for g in r[7].split(" / ")) or r[9] != r[10]:
            print("  %s#%s %s %s: before %s (%s), after %s, works %s" % (r[0], r[1], r[2], r[4], r[7], r[6], r[9], r[10]))


if __name__ == "__main__":
    main()
