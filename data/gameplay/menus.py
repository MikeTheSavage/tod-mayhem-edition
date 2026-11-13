"""Shared helper for the game's own option menus.

Menus are data: `HUD.fragment` of overdose.main, page nodes `InterMission_HUD/OptionSettingsMenu/<page>`, one
`Trigger_Root_Menu` entry per row. A row's values are text ordinals into
`/data/Localize/Localization_Menu/<lang>_Menu.txt` of the language block `overdose_<lang>.main` (357 entries in all five
languages of the GOG data).

Two kinds of edit, both save-safe (no create record anywhere):
  * entry_op(ctx, path, prop, value, note)  - a set op on an existing entry (e.g. NotPcOption false, a marker in an unused
    member). Goes through the builder's normal verification like every option.
  * MenuText(ordinal, key, expect, texts)   - replace the value of an EXISTING Menu text entry (same ordinal in every
    language, so a language block that is not edited still has an entry there: no out-of-range lookup, no crash).
    Options declare them in TEXT_EDITS[(option, level)]; builder.build_menu_texts applies them per language block,
    starting from the block another option already edited (suave's uk block) when there is one.

New menu ENTRIES (a new row) need a create record in HUD.fragment, which is not the last fragment instanced in
overdose.main: that would shift every later node index and break save loading (see step C in gameplay/suave_anims.py). Do not.
A new VALUE in an existing row: give the text here and let the DLL append it at run time (dll/src/gp_menu.cpp
gp_menu_append_value), so a game without the DLL keeps the vanilla values.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(os.path.dirname(HERE), 'lib')
if LIB not in sys.path:
    sys.path.insert(0, LIB)
import todblock as tb   # noqa: E402
import texlib as tl     # noqa: E402

MAIN = 'data_pc/overdose_the_game/overdose/shared/overdose.main'
F_HUD = '/data/Scripts/fragments/HUD.fragment'
SETTINGS = 'InterMission_HUD/OptionSettingsMenu/Settings'
LANGS = ['uk', 'de', 'fr', 'es', 'it']
MENU_TXT = '/data/Localize/Localization_Menu/uk_Menu.txt'   # the record keeps the uk name in every language block


def lang_block(lang):
    return 'data_pc/overdose_the_game/overdose/%s/overdose_%s.main' % (lang, lang)


class MenuError(Exception):
    pass


class MenuText:
    """Replace Menu text `ordinal` (key `key`, value starting with `expect` in uk) by texts[lang] (texts['*'] = any)."""

    def __init__(self, ordinal, key, expect, texts):
        self.ordinal, self.key, self.expect, self.texts = ordinal, key, expect, dict(texts)

    def text_for(self, lang):
        return self.texts.get(lang, self.texts.get('*'))


# ------------------------------------------------------------------ the text edits of every option (option, level) ->
# (languages, [MenuText]). uk_Menu #77 "FILL_IN" (key FILL_IN) is a filler no menu entry uses (menus.json) and only an
# Xbox start-message widget names it: the free slot for BRUTAL.
BRUTAL_ORDINAL = 77
BRUTAL_TEXT = MenuText(BRUTAL_ORDINAL, 'FILL_IN', 'FILL_IN',
                       {'uk': 'BRUTAL', 'de': 'BRUTAL', 'fr': 'BRUTAL', 'es': 'BRUTAL', 'it': 'BRUTALE'})
# uk_Menu #101 "DEBUG COMPLETE" (key MENU_Debug_Complete): debug menu text, no menu entry uses it, only an Xbox
# widget field names it: the slot for PSYCHO (fifth value; its own feature, enemies with loco moves, comes later).
PSYCHO_ORDINAL = 101
PSYCHO_TEXT = MenuText(PSYCHO_ORDINAL, 'MENU_Debug_Complete', 'DEBUG COMPLETE',
                       {'uk': 'PSYCHO', 'de': 'PSYCHO', 'fr': 'PSYCHO', 'es': 'PSICOPATA', 'it': 'PSICOPATICO'})
TEXT_EDITS = {
    ('difficulty_menu', 'on'): (['uk'], [BRUTAL_TEXT, PSYCHO_TEXT]),
    ('difficulty_menu', 'on_all_languages'): (LANGS, [BRUTAL_TEXT, PSYCHO_TEXT]),
}


def text_edits_for(selection):
    """{lang: [MenuText]} for the selected option levels; two options replacing the same ordinal are refused."""
    out, owner = {}, {}
    for (opt, level), (langs, edits) in TEXT_EDITS.items():
        if selection.get(opt) != level:
            continue
        for lang in langs:
            for e in edits:
                k = (lang, e.ordinal)
                if k in owner and owner[k] != opt:
                    raise MenuError('Menu text #%d (%s) is replaced by both %s and %s' % (e.ordinal, lang, owner[k], opt))
                owner[k] = opt
                out.setdefault(lang, []).append(e)
    return out


# ------------------------------------------------------------------ set ops on entries
def entry_node(ctx, path):
    """(doc, node) of a menu entry, e.g. path = SETTINGS + '/difficulty'."""
    doc = ctx.doc(MAIN, F_HUD)
    n = doc.find(path)
    if n is None:
        raise MenuError('no menu entry %s in HUD.fragment' % path)
    ty = doc.type_of(n) or ''
    if not ty.startswith('Trigger_Root_Menu'):
        raise MenuError('%s is a %s, not a Trigger_Root_Menu' % (path, ty))
    return doc, n


def entry_op(ctx, path, prop, value, note, Op):
    """Set op (Op class from options.py) of `prop` on the entry at `path`; None when it already holds `value`."""
    doc, n = entry_node(ctx, path)
    old = doc.get(n, prop)
    if old == value:
        return None
    return Op(MAIN, F_HUD, n, prop, value, old, note, doc.path_of(n))


def entry_values(ctx, path):
    """[(ValueString, OptionValueType)] of slots 1..5 as the data holds them (None = not set = the class default)."""
    doc, n = entry_node(ctx, path)
    return [(doc.get(n, 'ValueString%d' % k), doc.get(n, 'OptionValueType%d' % k)) for k in range(1, 6)]


# ------------------------------------------------------------------ text replacement in a language block
def apply_texts(raw, lang, edits):
    """overdose_<lang>.main bytes -> (new bytes, report). Verified: every record byte-identical except the Menu text
    record, every payload identical, the edited entries read back, every other entry identical."""
    b = tb.Block(raw, '.main')
    info = bytes(b.info)
    starts = tb.record_starts(info, b.n)
    ends = starts[1:] + [len(info)]
    hits = [(i, R, E, t) for i, R, E, t in tl.find_text_records(info, starts) if t.name == MENU_TXT]
    if len(hits) != 1:
        raise MenuError('%s: %s found %d times' % (lang, MENU_TXT, len(hits)))
    ti, R, E, trec = hits[0]
    pairs = trec.pairs()
    rep = []
    for e in edits:
        if e.ordinal >= len(pairs):
            raise MenuError('%s: Menu text #%d does not exist (%d entries)' % (lang, e.ordinal, len(pairs)))
        k_, v_ = tl.codes_to_str(pairs[e.ordinal][0]), tl.codes_to_str(pairs[e.ordinal][1])
        new = e.text_for(lang)
        if new is None:
            raise MenuError('%s: no %s text for Menu #%d' % (lang, lang, e.ordinal))
        if k_ != e.key:
            raise MenuError('%s: Menu #%d is %r = %r, expected key %s' % (lang, e.ordinal, k_, v_, e.key))
        if v_ == new:
            continue
        if lang == 'uk' and not v_.startswith(e.expect):
            raise MenuError('uk: Menu #%d is %r, expected %r (another mod changed it?)' % (e.ordinal, v_, e.expect))
        err, _ = tl.check_entry(pairs[e.ordinal][0], tl.str_to_codes(new))
        if err:
            raise MenuError('%s: Menu #%d: %s' % (lang, e.ordinal, err[0]))
        pairs[e.ordinal] = (pairs[e.ordinal][0], tl.str_to_codes(new))
        rep.append(dict(lang=lang, ordinal=e.ordinal, key=k_, old=v_, new=new))
    if not rep:
        return raw, dict(lang=lang, texts=[], unchanged=True)
    new_text, note = tl.build_record(trec, pairs)
    out = bytearray()
    for k, (R0, E0) in enumerate(zip(starts, ends)):
        out += new_text if k == ti else info[R0:E0]
    nb = tb.Block(b.serialize(), '.main')
    nb.info = bytearray(out)
    nb.payloads = list(b.payloads)
    new = nb.serialize()
    # verify
    chk = tb.Block(new, '.main')
    cinfo = bytes(chk.info)
    cst = tb.record_starts(cinfo, chk.n)
    cends = cst[1:] + [len(cinfo)]
    if chk.n != b.n or chk.prop_ck != b.prop_ck or chk.cmd_ck != b.cmd_ck or chk.timestamp != b.timestamp:
        raise MenuError('%s: header check failed' % lang)
    for k in range(b.n):
        same = cinfo[cst[k]:cends[k]] == info[starts[k]:ends[k]]
        if (k == ti) == same:
            raise MenuError('%s: record %d %s' % (lang, k, 'did not change' if k == ti else 'changed'))
        if chk.payloads[k] != b.payloads[k]:
            raise MenuError('%s: payload %d changed' % (lang, k))
    t2 = [t for i, R0, E0, t in tl.find_text_records(cinfo, cst) if t.name == MENU_TXT][0]
    p2, p0 = t2.pairs(), trec.pairs()
    edited = {r['ordinal']: r['new'] for r in rep}
    if len(p2) != len(p0):
        raise MenuError('%s: Menu entry count %d -> %d' % (lang, len(p0), len(p2)))
    for o in range(len(p2)):
        if o in edited:
            if tl.codes_to_str(p2[o][1]) != edited[o] or p2[o][0] != p0[o][0]:
                raise MenuError('%s: Menu #%d reads back %r' % (lang, o, tl.codes_to_str(p2[o][1])))
        elif p2[o] != p0[o]:
            raise MenuError('%s: Menu #%d changed' % (lang, o))
    return new, dict(lang=lang, entry=lang_block(lang), records=b.n, text_record=ti, text_note=note, texts=rep,
                     entries=len(p2), info_before=len(info), info_after=len(cinfo))
