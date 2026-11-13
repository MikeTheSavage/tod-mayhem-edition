"""glbio.py - minimal glTF-binary reader/writer (numpy) for the staged character glbs.

read_glb(path) -> (json dict, bin bytes); accessor(gl, bin, i) -> numpy array.
write_glb(path, gl, bin) writes a glb (JSON chunk padded with spaces, BIN chunk with zeros).
"""
import json, struct
import numpy as np

CT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
NC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_glb(path):
    b = open(path, "rb").read()
    magic, ver, length = struct.unpack_from("<4sII", b, 0)
    assert magic == b"glTF"
    off = 12
    gl, binb = None, b""
    while off < length:
        clen, ctype = struct.unpack_from("<I4s", b, off)
        chunk = b[off + 8: off + 8 + clen]
        if ctype == b"JSON":
            gl = json.loads(chunk.decode("utf-8"))
        elif ctype == b"BIN\0":
            binb = chunk
        off += 8 + clen
    return gl, binb


def accessor(gl, binb, i):
    a = gl["accessors"][i]
    bv = gl["bufferViews"][a["bufferView"]]
    dt = np.dtype(CT[a["componentType"]])
    n = NC[a["type"]]
    off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = bv.get("byteStride", 0)
    if stride and stride != dt.itemsize * n:
        raw = np.frombuffer(binb, dtype=np.uint8, count=stride * (a["count"] - 1) + dt.itemsize * n, offset=off)
        out = np.zeros((a["count"], n), dt)
        for k in range(a["count"]):
            out[k] = np.frombuffer(raw[k * stride: k * stride + dt.itemsize * n].tobytes(), dt)
        return out
    arr = np.frombuffer(binb, dtype=dt, count=a["count"] * n, offset=off)
    return arr.reshape(a["count"], n) if n > 1 else arr


def write_glb(path, gl, binb):
    js = json.dumps(gl, separators=(",", ":")).encode("utf-8")
    js += b" " * ((4 - len(js) % 4) % 4)
    binb = bytes(binb) + b"\0" * ((4 - len(binb) % 4) % 4)
    total = 12 + 8 + len(js) + (8 + len(binb) if binb else 0)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total))
        f.write(struct.pack("<I4s", len(js), b"JSON")); f.write(js)
        if binb:
            f.write(struct.pack("<I4s", len(binb), b"BIN\0")); f.write(binb)
