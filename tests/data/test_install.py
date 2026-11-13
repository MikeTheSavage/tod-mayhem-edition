"""install_gameplay_mod.py on copies only (work/tests/install_*): idempotent, exact undo, line ends kept."""
import os
import shutil
import unittest

from common import GOG, HD, WORK

import install_gameplay_mod as inst
from gameplay.gamefs import parse_filesystem, read_config_lines

VANILLA = b'fullscreen = false\r\nscreensize = 1920,1080\r\n'
HD_CFG = (b'# TOD HD texture pack + fx DLL\r\nfilesystem = hd_override1.zip, hd_override2.zip, blocks.naz, sounds.naz, '
          b'videos00.naz, videos01.naz\r\nlogdumpfile = tod_hd_log.txt\r\nfullscreen = false\r\n')


def quiet(*a):
    pass


class Install(unittest.TestCase):
    def folder(self, name, content):
        d = os.path.join(WORK, 'install_' + name)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        p = os.path.join(d, 'configpc.txt')
        open(p, 'wb').write(content)
        return p

    def fs(self, p):
        return parse_filesystem(read_config_lines(p)[0])[1]

    def roundtrip(self, name, content, expect_order):
        p = self.folder(name, content)
        self.assertTrue(inst.install(p, log=quiet))
        once = open(p, 'rb').read()
        self.assertEqual(self.fs(p), expect_order)
        self.assertFalse(inst.install(p, log=quiet))                 # idempotent
        self.assertEqual(open(p, 'rb').read(), once)
        self.assertEqual(open(p + inst.BAK, 'rb').read(), content)   # backup = original
        nl = b'\r\n' if b'\r\n' in content else b'\n'
        self.assertNotIn(b'\n', once.replace(nl, b''))               # line ends kept
        self.assertTrue(inst.uninstall(p, log=quiet))
        self.assertEqual(open(p, 'rb').read(), content)              # exact undo
        self.assertFalse(inst.uninstall(p, log=quiet))
        return p

    def test_vanilla_config(self):
        self.roundtrip('vanilla', VANILLA, [inst.ZIP, 'blocks.naz', 'sounds.naz', 'videos00.naz', 'videos01.naz'])

    def test_vanilla_lf_no_final_newline(self):
        self.roundtrip('lf', b'fullscreen = false\nscreensize = 800,600',
                       [inst.ZIP, 'blocks.naz', 'sounds.naz', 'videos00.naz', 'videos01.naz'])

    def test_tod_hd_config(self):
        self.roundtrip('hd', HD_CFG, [inst.ZIP, 'hd_override1.zip', 'hd_override2.zip', 'blocks.naz', 'sounds.naz',
                                      'videos00.naz', 'videos01.naz'])

    def test_existing_line_with_zip_elsewhere_is_reordered(self):
        p = self.folder('reorder', b'filesystem = a.zip, todgameplay_data.zip, blocks.naz\r\n')
        inst.install(p, log=quiet)
        self.assertEqual(self.fs(p), [inst.ZIP, 'a.zip', 'blocks.naz'])
        inst.uninstall(p, log=quiet)
        self.assertEqual(self.fs(p), ['a.zip', 'blocks.naz'])

    def test_zip_copy_and_removal(self):
        p = self.folder('zipcopy', VANILLA)
        z = os.path.join(os.path.dirname(p), 'built', inst.ZIP)
        os.makedirs(os.path.dirname(z))
        open(z, 'wb').write(b'PK\x05\x06' + bytes(18))
        inst.install(p, z, log=quiet)
        dst = os.path.join(os.path.dirname(p), inst.ZIP)
        self.assertTrue(os.path.exists(dst))
        self.assertTrue(os.path.exists(dst + '.manifest.json'))
        inst.uninstall(p, log=quiet)
        self.assertFalse(os.path.exists(dst))
        self.assertFalse(os.path.exists(dst + '.manifest.json'))
        self.assertEqual(open(p, 'rb').read(), VANILLA)

    def test_refuses_while_tod_runs(self):
        p = self.folder('running', VANILLA)
        open(os.path.join(os.path.dirname(p), 'TOD.exe'), 'wb').write(b'MZ')
        real = inst.game_running
        inst.game_running = lambda: True
        try:
            with self.assertRaises(inst.InstallError):
                inst.install(p, log=quiet)
            self.assertEqual(open(p, 'rb').read(), VANILLA)
        finally:
            inst.game_running = real

    def test_on_copies_of_the_real_configs(self):
        for name, src in (('gog_copy', os.path.join(GOG, 'configpc.txt')), ('hd_copy', os.path.join(HD, 'configpc.txt'))):
            if not os.path.exists(src):
                continue
            content = open(src, 'rb').read()
            p = self.folder(name, content)
            inst.install(p, log=quiet)
            order = self.fs(p)
            self.assertEqual(order[0], inst.ZIP)
            if b'hd_override1.zip' in content:
                self.assertEqual(order[1], 'hd_override1.zip')
            inst.uninstall(p, log=quiet)
            self.assertEqual(open(p, 'rb').read(), content)


if __name__ == '__main__':
    unittest.main()
