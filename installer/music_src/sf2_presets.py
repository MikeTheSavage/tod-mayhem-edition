"""List presets (bank, program, name) in a SoundFont 2 file. Usage: python -I sf2_presets.py file.sf2"""
import struct, sys

def presets(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:4] == b'RIFF' and data[8:12] == b'sfbk'
    pos = 12
    while pos < len(data):
        cid, size = data[pos:pos+4], struct.unpack('<I', data[pos+4:pos+8])[0]
        if cid == b'LIST' and data[pos+8:pos+12] == b'pdta':
            p, end = pos + 12, pos + 8 + size
            while p < end:
                sid, ssize = data[p:p+4], struct.unpack('<I', data[p+4:p+8])[0]
                if sid == b'phdr':
                    out = []
                    for i in range(ssize // 38 - 1):
                        rec = data[p+8+i*38:p+8+(i+1)*38]
                        name = rec[:20].split(b'\0')[0].decode('latin-1')
                        prog, bank = struct.unpack('<HH', rec[20:24])
                        out.append((bank, prog, name))
                    return sorted(out)
                p += 8 + ssize + (ssize & 1)
        pos += 8 + size + (size & 1)

if __name__ == '__main__':
    for b, p, n in presets(sys.argv[1]):
        print(f'{b:3d} {p:3d} {n}')
