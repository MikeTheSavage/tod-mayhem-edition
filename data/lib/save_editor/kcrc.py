"""Kapow engine CRC (TOD.exe generic_crc32 @0x465840 + crc32_gen @0x4658A0, per TOD_tools reconstruction).
Bitwise: reg=(reg<<1)|bit, bits fed LSB-first per byte, if old top bit set reg^=0x04C11DB7; init 0;
finished by feeding the zero 'salt' bytes. Equivalent: direct MSB-first CRC over bit-reversed bytes."""
POLY = 0x04C11DB7
_REV = bytes(int('{:08b}'.format(i)[::-1], 2) for i in range(256))
_TAB = []
for i in range(256):
    c = i << 24
    for _ in range(8):
        c = ((c << 1) ^ POLY) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
    _TAB.append(c)

def update(reg, data):
    """Augmented-register update (no trailing zeros). Returns new register."""
    # augmented form: reg' = reg shifted by 8 with incoming byte bits. Convert using direct algorithm on
    # a running 'direct' value is not identical, so implement augmented with table:
    # augmented step for one byte b (reversed r): top = reg>>24; reg = ((reg<<8)|r) ^ T[top]
    tab = _TAB; rev = _REV
    for b in data:
        top = reg >> 24
        reg = (((reg << 8) & 0xFFFFFFFF) | rev[b]) ^ tab[top]
    return reg

def crc(data, salt_len=4):
    return update(update(0, data), b'\0' * salt_len)

def crc_bitwise(data, salt_len=4):
    reg = 0
    for b in bytes(data) + b'\0' * salt_len:
        for i in range(8):
            top = reg & 0x80000000
            reg = ((reg << 1) | ((b >> i) & 1)) & 0xFFFFFFFF
            if top:
                reg ^= POLY
    return reg
