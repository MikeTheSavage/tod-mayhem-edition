"""Ogg Vorbis header reader (stdlib only): pages, identification header, comments, total samples, page CRC check."""
import struct


def _crc_table():
    t = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) if r & 0x80000000 else (r << 1)
            r &= 0xFFFFFFFF
        t.append(r)
    return t


_T = _crc_table()


def ogg_crc(b):
    r = 0
    for c in b:
        r = ((r << 8) & 0xFFFFFFFF) ^ _T[((r >> 24) ^ c) & 0xFF]
    return r


def pages(data, check_crc=True):
    """Yield one dict per page: off, htype, granule, serial, seq, crc, crc_ok, segs, body_off, body_len, end."""
    o = 0
    n = len(data)
    while o < n:
        if data[o:o + 4] != b'OggS':
            raise ValueError('no OggS at %d' % o)
        ver, htype, gran, serial, seq, crc, nseg = struct.unpack_from('<BBqIIIB', data, o + 4)
        segs = data[o + 27:o + 27 + nseg]
        blen = sum(segs)
        end = o + 27 + nseg + blen
        ok = None
        if check_crc:
            page = bytearray(data[o:end])
            page[22:26] = b'\0\0\0\0'
            ok = ogg_crc(page) == crc
        yield dict(off=o, htype=htype, granule=gran, serial=serial, seq=seq, crc=crc, crc_ok=ok,
                   segs=bytes(segs), body_off=o + 27 + nseg, body_len=blen, end=end)
        o = end


def packets(data, max_packets=3):
    out = []
    cur = bytearray()
    for p in pages(data, check_crc=False):
        bo = p['body_off']
        for s in p['segs']:
            cur += data[bo:bo + s]
            bo += s
            if s < 255:
                out.append(bytes(cur))
                cur = bytearray()
                if len(out) >= max_packets:
                    return out
    return out


def info(data, check_crc=True):
    pk = packets(data, 2)
    idh = pk[0]
    if idh[:7] != b'\x01vorbis':
        raise ValueError('not vorbis')
    ver, ch, rate, bmax, bnom, bmin = struct.unpack_from('<IBIiii', idh, 7)
    bs = idh[28]
    com = {}
    vendor = ''
    if len(pk) > 1 and pk[1][:7] == b'\x03vorbis':
        c = pk[1]
        p = 7
        vl = struct.unpack_from('<I', c, p)[0]
        p += 4
        vendor = c[p:p + vl].decode('utf-8', 'replace')
        p += vl
        nc = struct.unpack_from('<I', c, p)[0]
        p += 4
        for _ in range(nc):
            ln = struct.unpack_from('<I', c, p)[0]
            p += 4
            kv = c[p:p + ln].decode('utf-8', 'replace')
            p += ln
            k, _, v = kv.partition('=')
            com.setdefault(k.upper(), []).append(v)
    last = None
    npages = 0
    bad = 0
    serials = set()
    eos = 0
    for p in pages(data, check_crc):
        npages += 1
        serials.add(p['serial'])
        if p['crc_ok'] is False:
            bad += 1
        if p['htype'] & 4:
            eos += 1
        if p['granule'] >= 0:
            last = p
    return dict(channels=ch, rate=rate, bitrate_nominal=bnom, bitrate_max=bmax, bitrate_min=bmin,
                blocksizes=(1 << (bs & 15), 1 << (bs >> 4)), vendor=vendor, comments=com,
                samples=last['granule'] if last else 0, pages=npages, bad_crc_pages=bad,
                streams=len(serials), eos_pages=eos)
