#!/usr/bin/env python3
"""
TOD static model -> glTF extractor (no runtime, no restools, no SecuROM).

Reads a compiled asset block (.main/.submap/.mission; the AssetBlockReader format: a 24-byte
AssetSharedHeader then the info buffer) and pulls real mesh geometry + skeleton straight out of
the serialized VertexBuffer/IndexBuffer objects in the info buffer.

The correct walk (verified against overdose.main):
  ModelAsset(+0x30 pivot ptr, +0x34 count)
    pivot (stride 0x7C): +0x1C name, +0x24 SkinnedMeshBuffer ptr, +0x28 count, +0x4C parent
      SkinnedMeshBuffer (stride 0x3C): +0x00 -> MeshBuffer   (+0x04 Mesh is NULL in compiled assets)
        MeshBuffer: +0x0C -> VertexBuffer, +0x10 -> IndexBuffer
          VertexBuffer: +0x04 count, +0x08 FVF, +0x0C byteLen, +0x10 -> raw verts, +0x14 stride(u16)
          IndexBuffer : +0x00 count, +0x04 primTypeIdx(0=strip), +0x0C -> raw u16 indices
Vertex FVF: stride32 = pos12+normal12+uv8 ; stride36 = +diffuse(DWORD)@24,uv@28 ; stride40 = +uv2@32.
Self-relative pointers: abs = (stored & ~3) + field_addr ; 0 / 0x80000000 = null.
"""
import struct, sys, os, argparse
import tod_model_to_gltf as g   # reuse build_glb / write_glb / world_mats

def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def i32(b, o): return struct.unpack_from('<i', b, o)[0]
def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def f32(b, o): return struct.unpack_from('<f', b, o)[0]

def load_info(path):
    data = open(path, 'rb').read()
    # AssetSharedHeader: timestamp, propCk, cmdCk, totalResources, infoSize, dataSize
    ts, pck, cck, total, infosize, datasize = struct.unpack_from('<IIIiII', data, 0)
    if not (0 < total < 100000 and 0 < infosize < len(data)):
        raise ValueError('not an AssetBlock (.main/.submap/.mission) or unexpected header')
    return data[24:24 + infosize]

def deref(info, field_abs):
    v = u32(info, field_abs)
    if v == 0 or v == 0x80000000:
        return None
    return (field_abs + (v & 0xFFFFFFFC)) & 0xFFFFFFFF

def cstr(info, off, n=64):
    if off is None: return ''
    return info[off:off + n].split(b'\0', 1)[0].decode('latin-1', 'replace')

def find_model_records(info):
    """Enumerate all MODEL asset records: 16-aligned, type==3, name resolves to '/...model'."""
    recs = []
    for p in range(0, len(info) - 8, 16):
        if u32(info, p) != 3:
            continue
        npos = deref(info, p + 4)
        if npos is None or npos >= len(info) or info[npos:npos+1] != b'/':
            continue
        name = cstr(info, npos, 256)   # 256 not 64: long paths (virgillo_brothers_badguy/...) need it
        if name.lower().endswith('.model'):
            recs.append((p, name))
    return recs

def parse_vertex(vb_bytes, off, stride):
    px, py, pz = f32(vb_bytes, off), f32(vb_bytes, off+4), f32(vb_bytes, off+8)
    nx, ny, nz = f32(vb_bytes, off+12), f32(vb_bytes, off+16), f32(vb_bytes, off+20)
    uvo = 28 if stride == 36 else 24            # diffuse DWORD sits at 24 when stride==36
    u, v = f32(vb_bytes, off+uvo), f32(vb_bytes, off+uvo+4)
    return (px, py, pz), (u, v), (nx, ny, nz)

def extract_model(info, base, name):
    pivots, meshes = [], []
    # ModelAsset record: +0x20 TextureReferencesList (8B refs: +0x00 asset-ptr, +0x04 name-ptr), +0x30 pivots
    texrefs = []
    tl = deref(info, base + 0x20); tc = i32(info, base + 0x24)
    if tl is not None and 0 < tc < 4096:
        for ti in range(tc):
            nm = cstr(info, deref(info, tl + ti * 8 + 0x04), 256)
            texrefs.append(nm)
    piv_ptr = deref(info, base + 0x30)
    piv_cnt = i32(info, base + 0x34)
    if piv_ptr is None or not (0 < piv_cnt < 4096):
        return None
    for pi in range(piv_cnt):
        pv = piv_ptr + pi * 0x7C
        pos = [f32(info, pv + 0x00), f32(info, pv + 0x04), f32(info, pv + 0x08)]
        quat = [f32(info, pv + 0x0C), f32(info, pv + 0x10), f32(info, pv + 0x14), f32(info, pv + 0x18)]
        pname = cstr(info, deref(info, pv + 0x1C), 48)
        parent = i32(info, pv + 0x4C)
        pivots.append({'pos': pos, 'quat': quat, 'parent': parent, 'name': pname, 'nSkin': 0})
        sm_ptr = deref(info, pv + 0x24)
        sm_cnt = i32(info, pv + 0x28)
        if sm_ptr is None or not (0 < sm_cnt < 4096):
            continue
        pivots[pi]['nSkin'] = sm_cnt
        for si in range(sm_cnt):
            sm = sm_ptr + si * 0x3C
            mb = deref(info, sm + 0x00)
            if mb is None:
                continue
            # m_TextureReferences (SkinnedMeshBuffer+0x08): {self-rel begin, int count}.
            # Each entry is an index into the model TextureReferencesList. entry[0] is the
            # default texture/variant for this submesh (verified vs ram/civil/chicken).
            tr_b = deref(info, sm + 0x08); tr_c = i32(info, sm + 0x0C)
            texidx = i32(info, tr_b) if (tr_b is not None and 0 < tr_c < 4096) else -1
            vb = deref(info, mb + 0x0C)
            ib = deref(info, mb + 0x10)
            if vb is None or ib is None:
                continue
            vcount = i32(info, vb + 0x00)   # capacity @+0x00 is the real count; +0x04 (ListSize) is 0 in compiled assets
            vlen = i32(info, vb + 0x0C)
            stride = u16(info, vb + 0x14)
            vbuf = deref(info, vb + 0x10)
            icount = i32(info, ib + 0x00)
            ibuf = deref(info, ib + 0x0C)
            if vbuf is None or ibuf is None or stride not in (32, 36, 40):
                continue
            if vcount <= 0 or vcount > 200000 or vlen != vcount * stride:
                continue
            # smooth-skin stream (characters): MeshBuffer+0x44 -> 96B records, +0x48 -> u16 infl counts
            skin_ptr = deref(info, mb + 0x44)
            infl_ptr = deref(info, mb + 0x48)
            skinned = skin_ptr is not None and infl_ptr is not None
            verts, joints, weights = [], [], []
            for vi in range(vcount):
                p, uv, n = parse_vertex(info, vbuf + vi * stride, stride)
                verts.append((p, uv, (0.0, 0.0), n, (1.0, 1.0, 1.0, 1.0), b'\0\0\0\0', (0, 0, 0, 0)))
                if skinned:
                    rec = skin_ptr + vi * 96
                    ninf = u16(info, infl_ptr + vi * 2)
                    ninf = max(1, min(4, ninf if 0 < ninf <= 4 else 1))
                    bi = [i32(info, rec + 0x20 + k * 4) for k in range(4)]
                    wt = [f32(info, rec + 0x40 + k * 4) for k in range(4)]
                    jj = [(bi[k] if (k < ninf and 0 <= bi[k] < piv_cnt) else 0) for k in range(4)]
                    ww = [(wt[k] if k < ninf else 0.0) for k in range(4)]
                    s = sum(ww) or 1.0
                    joints.append(jj); weights.append([w / s for w in ww])
            indices = list(struct.unpack_from('<%dH' % icount, info, ibuf)) if 0 < icount < 4000000 else []
            ptype = i32(info, ib + 0x04)
            meshes.append({'pivot': pi, 'isTriangles': 1 if ptype else 0,  # primTypeIdx 0 = strip
                           'palette': [], 'verts': verts, 'indices': indices,
                           'skinned': skinned, 'joints': joints, 'weights': weights,
                           'texidx': texidx, 'source_flags': u32(info, sm + 0x38),
                           'source_mesh_offset': sm, 'source_lod': u32(info, sm + 0x38) & 31,
                           # MeshBuffer flags are distinct from SkinnedMeshBuffer flags.
                           # MB bit 2 requests the native environment-resource resolver.
                           'source_meshbuffer_offset': mb,
                           'source_meshbuffer_flags': u32(info, mb + 0x04)})
    return {'name': name, 'pivots': pivots, 'meshes': meshes, 'textures': texrefs, 'bound': [0, 0, 0, 0]}

def _inv_rigid(M):
    """Inverse of a rigid row-major 4x4 (R|t) -> (R^T | -R^T t)."""
    RT = [[M[j][i] for j in range(3)] for i in range(3)]
    t = [M[i][3] for i in range(3)]
    nt = [-(RT[i][0]*t[0] + RT[i][1]*t[1] + RT[i][2]*t[2]) for i in range(3)]
    return [RT[0]+[nt[0]], RT[1]+[nt[1]], RT[2]+[nt[2]], [0, 0, 0, 1]]

def _colmajor(M):
    return [M[r][c] for c in range(4) for r in range(4)]

def build_skinned_glb(model):
    """glTF with a skin: joint nodes + per-vertex JOINTS_0/WEIGHTS_0 + inverse bind matrices.
    Vertices stay in bind pose (un-baked); UE deforms them via the skeleton."""
    pivots = model['pivots']; npiv = len(pivots)
    Wm = g.world_mats(pivots)
    nodes = []
    for i, p in enumerate(pivots):
        nodes.append({'name': p['name'] or 'bone%d' % i, 'translation': p['pos'], 'rotation': p['quat']})
    for i, p in enumerate(pivots):
        ch = [j for j, q in enumerate(pivots) if q['parent'] == i and j != i]
        if ch: nodes[i]['children'] = ch
    roots = [i for i, p in enumerate(pivots) if not (0 <= p['parent'] < npiv and p['parent'] != i)]
    mesh_node = len(nodes)
    nodes.append({'mesh': 0, 'skin': 0, 'name': model['name']})

    pos, nrm, uv, joints, weights, idx = [], [], [], [], [], []
    for m in model['meshes']:
        if not m['verts']: continue
        base = len(pos)
        for vi, v in enumerate(m['verts']):
            pos.append(v[0]); nrm.append(v[3]); uv.append(v[1])
            if m.get('skinned') and m['joints']:
                joints.append(m['joints'][vi]); weights.append(m['weights'][vi])
            else:
                joints.append([m['pivot'], 0, 0, 0]); weights.append([1.0, 0.0, 0.0, 0.0])
        loc = m['indices'] if m['isTriangles'] else g.strip_to_tris(m['indices'])
        idx += [base + i for i in loc]

    blob = bytearray(); bv = []; acc = []
    def pack(data):
        o = len(blob); blob.extend(data); blob.extend(b'\0' * ((4 - len(blob) % 4) % 4)); return o, len(data)
    def acc_add(off, ln, comp, typ, mn=None, mx=None, count=None):
        bv.append({'buffer': 0, 'byteOffset': off, 'byteLength': ln})
        a = {'bufferView': len(bv) - 1, 'componentType': comp, 'count': count, 'type': typ}
        if mn is not None: a['min'] = mn; a['max'] = mx
        acc.append(a); return len(acc) - 1
    # indices u32
    o, l = pack(struct.pack('<%dI' % len(idx), *idx)); ia = acc_add(o, l, 5125, 'SCALAR', count=len(idx))
    # positions
    pmin = [min(p[i] for p in pos) for i in range(3)]; pmax = [max(p[i] for p in pos) for i in range(3)]
    o, l = pack(struct.pack('<%df' % (len(pos)*3), *[c for p in pos for c in p]))
    pa = acc_add(o, l, 5126, 'VEC3', pmin, pmax, len(pos))
    o, l = pack(struct.pack('<%df' % (len(nrm)*3), *[c for n in nrm for c in n])); na = acc_add(o, l, 5126, 'VEC3', count=len(nrm))
    o, l = pack(struct.pack('<%df' % (len(uv)*2), *[c for u in uv for c in u])); ua = acc_add(o, l, 5126, 'VEC2', count=len(uv))
    jcomp = 5121 if npiv < 256 else 5123; jfmt = 'B' if npiv < 256 else 'H'
    o, l = pack(struct.pack('<%d%s' % (len(joints)*4, jfmt), *[c for j in joints for c in j])); ja = acc_add(o, l, jcomp, 'VEC4', count=len(joints))
    o, l = pack(struct.pack('<%df' % (len(weights)*4), *[c for w in weights for c in w])); wa = acc_add(o, l, 5126, 'VEC4', count=len(weights))
    ibm = [c for i in range(npiv) for c in _colmajor(_inv_rigid(Wm[i]))]
    o, l = pack(struct.pack('<%df' % len(ibm), *ibm)); iba = acc_add(o, l, 5126, 'MAT4', count=npiv)

    gltf = {'asset': {'version': '2.0', 'generator': 'tod_model_extract(skinned)'},
            'buffers': [{'byteLength': len(blob)}], 'bufferViews': bv, 'accessors': acc,
            'meshes': [{'primitives': [{'attributes': {'POSITION': pa, 'NORMAL': na, 'TEXCOORD_0': ua,
                        'JOINTS_0': ja, 'WEIGHTS_0': wa}, 'indices': ia, 'mode': 4}]}],
            'skins': [{'joints': list(range(npiv)), 'inverseBindMatrices': iba,
                       'skeleton': roots[0] if roots else 0}],
            'nodes': nodes, 'scenes': [{'nodes': roots + [mesh_node]}], 'scene': 0}
    return gltf, bytes(blob)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('block', help='path to .main/.submap/.mission block file')
    ap.add_argument('-m', '--model', default=None, help='only this model (substring match)')
    ap.add_argument('-o', '--outdir', default=None)
    ap.add_argument('--list', action='store_true', help='just list models in the block')
    args = ap.parse_args()
    info = load_info(args.block)
    recs = find_model_records(info)
    print('block %s: %d MODEL records' % (os.path.basename(args.block), len(recs)))
    if args.list:
        for _, n in recs[:200]:
            print('  ', n)
        return
    outdir = args.outdir or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out', 'models')
    os.makedirs(outdir, exist_ok=True)
    done = ok = 0
    for base, name in recs:
        if args.model and args.model.lower() not in name.lower():
            continue
        done += 1
        m = extract_model(info, base, name)
        if not m or not any(mesh['verts'] for mesh in m['meshes']):
            print('  skip (no geometry):', name); continue
        try:
            is_skinned = any(mesh.get('skinned') for mesh in m['meshes'])
            if is_skinned:
                gltf, blob = build_skinned_glb(m)
            else:
                gltf, blob, *_ = g.build_glb(m, bake=True, sanity=False)
            base_name = os.path.splitext(os.path.basename(name))[0]
            g.write_glb(os.path.join(outdir, base_name + '.glb'), gltf, blob)
            if m['textures']:
                json.dump({'model': name, 'textures': m['textures']},
                          open(os.path.join(outdir, base_name + '.textures.json'), 'w'), indent=1)
            nv = sum(len(mesh['verts']) for mesh in m['meshes'])
            print('  %-38s %2d meshes %6d verts %s -> %s.glb' % (
                base_name, len(m['meshes']), nv, 'SKINNED' if is_skinned else 'static', base_name))
            ok += 1
        except Exception as e:
            print('  FAIL', name, e)
    print('extracted %d/%d models -> %s' % (ok, done, outdir))

if __name__ == '__main__':
    main()
