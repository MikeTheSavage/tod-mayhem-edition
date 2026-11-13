"""The hud_textures data option (HD HUD icons): the shipped art (hud_textures/art), the texture writer
(data/lib/todtexture.py) and the overdose.main rewrite against the GOG game's blocks (read only)."""
import json
import os
import struct
import unittest

from common import GOG, HAVE_GOG, ROOT

from gameplay import builder, hud_textures as ht   # puts data/lib on sys.path
from gameplay.gamefs import GameFS
import todblock as tb      # noqa: E402
import todtexture as tt    # noqa: E402

LIST = os.path.join(ROOT, 'hud_textures', 'textures.json')
SIZES = {'pickups': {(32, 32): (256, 256)}, 'ammo': {(32, 32): (256, 256), (8, 16): (128, 256)},
         'weapons': {(128, 64): (512, 256)}, 'combo': {(64, 256): (128, 512)}}
COUNTS = {'pickups': 15, 'ammo': 13, 'weapons': 28, 'combo': 1}


def level0_alpha(row, payload):
    w, h = row['w'], row['h']
    return [[payload[(y * w + x) * 4 + 3] for x in range(w)] for y in range(h)]


class Manifest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.man = ht.load_manifest()
        cls.rows = cls.man['textures']
        cls.payloads = {r['name']: ht.read_payload(r) for r in cls.rows}

    def test_groups_and_sizes(self):
        by = {}
        for r in self.rows:
            by.setdefault(r['group'], []).append(r)
            self.assertEqual(SIZES[r['group']][tuple(r['original'])], (r['w'], r['h']), r['name'])
        self.assertEqual({g: len(v) for g, v in by.items()}, COUNTS)

    def test_engine_rules(self):
        for r in self.rows:
            w, h, ow, oh = r['w'], r['h'], r['original'][0], r['original'][1]
            self.assertEqual(r['fmt'], 'RGBA32')
            self.assertTrue(tt.is_pow2(w) and tt.is_pow2(h), r['name'])
            self.assertEqual(w * oh, h * ow, '%s: aspect changed' % r['name'])
            k = w // ow
            self.assertTrue(k >= 2 and tt.is_pow2(k) and w == ow * k, r['name'])
            self.assertEqual(r['levels'], tt.full_chain(w, h))
            self.assertEqual(r['bytes'], tt.payload_size('RGBA32', w, h, r['levels']))
            self.assertLessEqual(r['bytes'], tt.TEXTURE_CAP, r['name'])
            self.assertEqual(len(self.payloads[r['name']]), r['bytes'])

    def test_total_size(self):
        tot = sum(r['bytes'] for r in self.rows if r['enabled'])
        self.assertLess(tot, 32 * 1024 * 1024)       # the always-resident overdose.main grows by under 32 MiB

    def test_list_matches_manifest(self):
        lst = json.load(open(LIST, encoding='utf-8'))['textures']
        self.assertEqual([e['name'] for e in lst], [r['name'] for r in self.rows])
        for e, r in zip(lst, self.rows):
            self.assertEqual((e['resource'], bool(e['enabled']), tuple(e['size'])), (r['resource'], r['enabled'], (r['w'], r['h'])))

    def test_mips_are_box_averages(self):
        """Level 1 alpha is the 2x2 average of level 0 alpha (premultiplied box filter), within rounding."""
        for name in ('LocoMove_Toro', 'gun_Light', 'BulletLine_Bullet'):
            r = next(x for x in self.rows if x['name'] == name)
            p = self.payloads[name]
            w, h = r['w'], r['h']
            a0 = p[3:w * h * 4:4]
            a1 = p[w * h * 4 + 3:w * h * 4 + (w // 2) * (h // 2) * 4:4]
            for y in range(0, h // 2, 7):
                for x in range(0, w // 2, 5):
                    avg = (a0[2 * y * w + 2 * x] + a0[2 * y * w + 2 * x + 1] + a0[(2 * y + 1) * w + 2 * x] +
                           a0[(2 * y + 1) * w + 2 * x + 1]) / 4.0
                    self.assertLessEqual(abs(a1[y * (w // 2) + x] - avg), 1.0, (name, x, y))

    def test_combo_strip_layout(self):
        """Upper half fully transparent black (the part the UV animation slides in), lower half drawn (the neon
        badge is glow on transparency: at least 10 % of the lower half clearly visible, some of it fully bright)."""
        r = next(x for x in self.rows if x['group'] == 'combo')
        p = self.payloads[r['name']]
        w, h = r['w'], r['h']
        upper = p[:w * (h // 2) * 4]
        self.assertEqual(upper, bytes(len(upper)))
        lower_alpha = p[w * (h // 2) * 4 + 3:w * h * 4:4]
        self.assertGreater(sum(1 for a in lower_alpha if a >= 128), len(lower_alpha) // 10)
        self.assertGreater(sum(1 for a in lower_alpha if a >= 250), 0)

    def test_no_dark_fringe(self):
        """Transparent texels next to opaque ones carry a fill colour, not black (no dark halo under bilinear)."""
        for name in ('LocoMove_Rewind', 'Melee_BaseballBat'):
            r = next(x for x in self.rows if x['name'] == name)
            p = self.payloads[name]
            w = r['w']
            dark = edge = 0
            for i in range(0, r['w'] * r['h'] - 1):
                if p[i * 4 + 3] == 0 and p[(i + 1) * 4 + 3] > 64 and (i + 1) % w:
                    edge += 1
                    if max(p[i * 4:i * 4 + 3]) < 8:
                        dark += 1
            self.assertGreater(edge, 10)
            self.assertLess(dark, edge // 4, name)


class Writer(unittest.TestCase):
    def rec(self, fmt=tt.FMT_A8R8G8B8, w=32, h=32):
        return dict(name='/x.bmp', gfx=0x40, offset=0, end=0x100, texinfo=0x80, w=w, h=h, res_w=w, res_h=h,
                    fmt=fmt, levels_word=0xBAADF000 | (tt.full_chain(w, h) << 1), mip_mode=1, ti_pair4=(w, h))

    def test_refusals(self):
        r = self.rec()
        with self.assertRaises(tt.TextureError):
            tt.texture_patches(r, 'RGBA32', 256, 128, 9)          # aspect
        with self.assertRaises(tt.TextureError):
            tt.texture_patches(r, 'RGBA32', 96, 96, 7)            # not a power of two
        with self.assertRaises(tt.TextureError):
            tt.texture_patches(r, 'RGBA32', 256, 256, 1)          # mip mode needs the full chain
        with self.assertRaises(tt.TextureError):
            tt.texture_patches(self.rec(tt.FMT_DXT1), 'RGBA32', 256, 256, 9)    # DXT1 stays DXT1
        with self.assertRaises(tt.TextureError):
            tt.check_replacement(r, 'RGBA32', 512, 512, 10, bytes(tt.payload_size('RGBA32', 512, 512, 10)))  # cap
        with self.assertRaises(tt.TextureError):
            tt.check_replacement(r, 'RGBA32', 256, 256, 9, bytes(100))           # wrong payload size

    def test_patch_fields(self):
        p = dict(tt.texture_patches(self.rec(), 'RGBA32', 256, 256, 9))
        self.assertEqual(p[0x40 + 0x0C], struct.pack('<II', 256, 256))
        self.assertEqual(p[0x40 + 0x14], struct.pack('<II', 256, 256))
        self.assertEqual(struct.unpack('<I', p[0x40 + 0x24])[0], 0xBAADF000 | (9 << 1))
        self.assertEqual(p[0x80 + 0x4C], struct.pack('<II', 256, 256))

    def test_payload_sizes(self):
        self.assertEqual(tt.payload_size('RGBA32', 256, 256, 9), 349524)
        self.assertEqual(tt.payload_size('RGBA32', 512, 256, 10), 699052)
        self.assertEqual(tt.payload_size('RGBA32', 128, 512, 10), 349532)
        self.assertEqual(tt.payload_size('RGBA32', 128, 256, 9), 174764)


@unittest.skipUnless(HAVE_GOG, 'needs the GOG game')
class OverdoseMain(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        arch = tb.open_naz(GOG)
        cls.raw = arch.read(ht.ENTRY)
        cls.man = ht.load_manifest()
        cls.out, cls.rep = ht.apply(cls.raw, cls.man)

    def test_all_replaced(self):
        self.assertEqual(self.rep['skipped'], [])
        self.assertEqual(len(self.rep['textures']), sum(r['enabled'] for r in self.man['textures']))
        self.assertLessEqual(tb.Block(self.out, '.main').max_asset, tt.TEXTURE_CAP)

    def test_read_back_and_untouched(self):
        old, new = tb.Block(self.raw, '.main'), tb.Block(self.out, '.main')
        recs = tt.textures(new)
        changed = set()
        for r in self.man['textures']:
            t = recs[r['resource'].lower()]
            self.assertEqual((t['fmt_name'], t['w'], t['h'], t['levels'], t['res_w'], t['res_h']),
                             ('RGBA32', r['w'], r['h'], r['levels'], r['w'], r['h']))
            self.assertEqual(new.payloads[t['index']], ht.read_payload(r))
            changed.add(t['index'])
        self.assertEqual(len(old.payloads), len(new.payloads))
        self.assertEqual(len(old.info), len(new.info))
        for k in range(len(old.payloads)):
            if k not in changed:
                self.assertEqual(old.payloads[k], new.payloads[k], k)
        self.assertEqual((old.timestamp, old.prop_ck, old.cmd_ck), (new.timestamp, new.prop_ck, new.cmd_ck))

    def test_placement_inside_original_box(self):
        """Each icon's level-0 content box (alpha > 16) lies inside the original's box scaled up (1 texel slack)."""
        blk = tb.Block(self.raw, '.main')
        recs = tt.textures(blk)
        for r in self.man['textures']:
            if r['group'] == 'combo':
                continue
            o = recs[r['resource'].lower()]
            ow, oh, orgba = tt.decode_rgba_level0(o, blk.payloads[o['index']])
            k = r['w'] // ow
            oxs = [x for y in range(oh) for x in range(ow) if orgba[(y * ow + x) * 4 + 3] > 16]
            oys = [y for y in range(oh) for x in range(ow) if orgba[(y * ow + x) * 4 + 3] > 16]
            a = level0_alpha(r, ht.read_payload(r))
            xs = [x for y in range(r['h']) for x in range(r['w']) if a[y][x] > 16]
            ys = [y for y in range(r['h']) for x in range(r['w']) if a[y][x] > 16]
            self.assertGreaterEqual(min(xs), min(oxs) * k - 1, r['name'])
            self.assertGreaterEqual(min(ys), min(oys) * k - 1, r['name'])
            self.assertLessEqual(max(xs) + 1, (max(oxs) + 1) * k + 1, r['name'])
            self.assertLessEqual(max(ys) + 1, (max(oys) + 1) * k + 1, r['name'])
            # and it fills the box along at least one axis (contain fit)
            fill_x = (max(xs) + 1 - min(xs)) / ((max(oxs) + 1 - min(oxs)) * k)
            fill_y = (max(ys) + 1 - min(ys)) / ((max(oys) + 1 - min(oys)) * k)
            self.assertGreater(max(fill_x, fill_y), 0.97, r['name'])

    def test_disabled_row_left_alone(self):
        man = json.loads(json.dumps(self.man))
        for r in man['textures']:
            r['enabled'] = r['name'] != 'gun_Light'
        out, rep = ht.apply(self.raw, man)
        self.assertNotIn('gun_Light', [t['name'] for t in rep['textures']])
        old, new = tb.Block(self.raw, '.main'), tb.Block(out, '.main')
        i = tt.textures(old)['/data/textures/hud/gun_light.bmp']['index']
        self.assertEqual(old.payloads[i], new.payloads[i])

    def test_bad_payload_is_skipped(self):
        man = json.loads(json.dumps(self.man))
        man['textures'][0]['sha256'] = '0' * 64
        out, rep = ht.apply(self.raw, man)
        self.assertEqual([s['name'] for s in rep['skipped']], [man['textures'][0]['name']])
        self.assertEqual(len(rep['textures']), len(man['textures']) - 1)

    def test_builder_composes_with_suave_anims(self):
        """hud_textures runs last and starts from the overdose.main another option already rebuilt."""
        ctx = builder.Context(GameFS(GOG, skip=[builder.ZIP_NAME]))
        rep, entries = builder.build_blocks(ctx, {'suave_anims': 'on', 'hud_textures': 'on'}, log=lambda *a: None)
        main = [d for e, d in entries if e == ht.ENTRY]
        self.assertEqual(len(main), 1)
        self.assertEqual(rep['hud_textures']['started_from'], 'output of another option')
        b = tb.Block(main[0], '.main')
        self.assertEqual(tt.textures(b)['/data/textures/hud/gun_light.bmp']['w'], 512)
        blk = next(x for x in rep['blocks'] if x['entry'] == ht.ENTRY)
        self.assertEqual(blk['options'], ['hud_textures', 'suave_anims'])


if __name__ == '__main__':
    unittest.main()
