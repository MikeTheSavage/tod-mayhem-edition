#!/usr/bin/env python3
"""
TOD model (.tmdl intermediate, dumped by restools) -> glTF 2.0 (.glb) for UE5.

v1 exports STATIC geometry: each mesh's vertices are baked by its owning pivot's world
transform (bind pose), giving correct geometry for static props and rigid models. The pivot
hierarchy is also emitted as a JSON sidecar (skeleton) for later skeletal/animation work.
Smooth-skin per-vertex weights are NOT baked here (they are runtime-assembled by the engine;
see docs/model_format.md "Remaining").

.tmdl layout: see CompiledModelAsset::DumpData in restools/AssetBlockReader.cpp.
"""
import sys, os, json, struct, argparse, base64

class R:
    def __init__(self, b): self.b=b; self.o=0
    def u32(self): v=struct.unpack_from('<I',self.b,self.o)[0]; self.o+=4; return v
    def i32(self): v=struct.unpack_from('<i',self.b,self.o)[0]; self.o+=4; return v
    def f32(self): v=struct.unpack_from('<f',self.b,self.o)[0]; self.o+=4; return v
    def raw(self,n): v=self.b[self.o:self.o+n]; self.o+=n; return v
    def s(self): n=self.u32(); return self.raw(n).decode('latin-1','replace')

VSTRIDE = 3+2+2+3+4  # floats per vertex in .tmdl, then 4 idx bytes + 4 weight floats
def parse_tmdl(path):
    r=R(open(path,'rb').read())
    assert r.raw(4)==b'TMD1', 'bad magic'
    m={'name':r.s()}
    m['bound']=[r.f32() for _ in range(4)]
    m['textures']=[r.s() for _ in range(r.u32())]
    pv=[]
    for _ in range(r.u32()):
        p={'pos':[r.f32() for _ in range(3)],
           'quat':[r.f32() for _ in range(4)],   # x,y,z,w
           'parent':r.i32(), 'name':r.s(), 'nSkin':r.u32()}
        pv.append(p)
    m['pivots']=pv
    meshes=[]; m['skipped']=[]
    total=len(r.b)
    for mi in range(r.u32()):
        mesh={'pivot':r.u32(),'isTriangles':r.u32()}
        palc=r.u32(); mesh['palette']=[r.u32() for _ in range(palc)]
        vc=r.u32()
        # Smooth-skinned meshes report a bogus FacesListSize (geometry is runtime-assembled,
        # see docs/model_format.md). Skip any mesh whose verts would overrun the file.
        if r.o + vc*76 > total:
            m['skipped'].append({'mesh':mi,'pivot':mesh['pivot'],'declaredVerts':vc,
                                 'palette':len(mesh['palette'])})
            break
        verts=[]
        for _ in range(vc):
            pos=(r.f32(),r.f32(),r.f32())
            uv0=(r.f32(),r.f32()); uv1=(r.f32(),r.f32())
            nrm=(r.f32(),r.f32(),r.f32())
            col=(r.f32(),r.f32(),r.f32(),r.f32())
            bidx=r.raw(4); w=(r.f32(),r.f32(),r.f32(),r.f32())
            verts.append((pos,uv0,uv1,nrm,col,bidx,w))
        ic=r.u32(); idx=list(struct.unpack_from('<%dH'%ic,r.b,r.o)) if ic else []; r.o+=ic*2
        mesh['verts']=verts; mesh['indices']=idx
        meshes.append(mesh)
    m['meshes']=meshes
    return m

# --- quaternion/matrix math for baking pivot world transforms ---
def quat_mat(q):
    x,y,z,w=q
    return [
        [1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
        [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y)],
    ]
def mul44(A,B):
    return [[sum(A[i][k]*B[k][j] for k in range(4)) for j in range(4)] for i in range(4)]
def trs(pos,q):
    R3=quat_mat(q)
    return [[R3[0][0],R3[0][1],R3[0][2],pos[0]],
            [R3[1][0],R3[1][1],R3[1][2],pos[1]],
            [R3[2][0],R3[2][1],R3[2][2],pos[2]],
            [0,0,0,1]]
def world_mats(pivots):
    out=[None]*len(pivots)
    def get(i):
        if out[i] is not None: return out[i]
        L=trs(pivots[i]['pos'],pivots[i]['quat'])
        par=pivots[i]['parent']
        out[i]=mul44(get(par),L) if 0<=par<len(pivots) and par!=i else L
        return out[i]
    for i in range(len(pivots)): get(i)
    return out
def xform(M,p):
    return (M[0][0]*p[0]+M[0][1]*p[1]+M[0][2]*p[2]+M[0][3],
            M[1][0]*p[0]+M[1][1]*p[1]+M[1][2]*p[2]+M[1][3],
            M[2][0]*p[0]+M[2][1]*p[1]+M[2][2]*p[2]+M[2][3])
def xform3(M,n):  # rotation only
    return (M[0][0]*n[0]+M[0][1]*n[1]+M[0][2]*n[2],
            M[1][0]*n[0]+M[1][1]*n[1]+M[1][2]*n[2],
            M[2][0]*n[0]+M[2][1]*n[1]+M[2][2]*n[2])

def strip_to_tris(idx):
    out=[]
    for i in range(len(idx)-2):
        a,b,c=idx[i],idx[i+1],idx[i+2]
        if a==b or b==c or a==c: continue
        out += [a,b,c] if (i&1)==0 else [a,c,b]
    return out

def build_glb(model, bake=True, sanity=False):
    Wm = world_mats(model['pivots']) if bake else None
    positions=[]; normals=[]; uvs=[]; indices=[]
    prims=[]  # (idx_offset, idx_count, base_vertex, vcount)
    for mesh in model['meshes']:
        if not mesh['verts']: continue
        M = Wm[mesh['pivot']] if (bake and mesh['pivot']<len(Wm)) else None
        base=len(positions)
        for (pos,uv0,uv1,nrm,col,bidx,w) in mesh['verts']:
            p = xform(M,pos) if M else pos
            n = xform3(M,nrm) if M else nrm
            positions.append(p); normals.append(n); uvs.append((uv0[0],uv0[1]))
        local = mesh['indices'] if mesh['isTriangles'] else strip_to_tris(mesh['indices'])
        off=len(indices)
        indices += [base+i for i in local]
        prims.append((off,len(indices)-off,base,len(mesh['verts'])))
    if sanity:
        xs=[p[0] for p in positions]; ys=[p[1] for p in positions]; zs=[p[2] for p in positions]
        def rng(a): return (round(min(a),2),round(max(a),2)) if a else (0,0)
        print('  verts=%d tris=%d  bounds x%s y%s z%s'%(len(positions),len(indices)//3,rng(xs),rng(ys),rng(zs)))
    # pack buffer: indices (u32) then positions/normals/uv (f32)
    ibin=struct.pack('<%dI'%len(indices),*indices)
    def pad4(b): return b + b'\x00'*((4-len(b)%4)%4)
    ibin=pad4(ibin)
    pbin=struct.pack('<%df'%(len(positions)*3),*[c for p in positions for c in p])
    nbin=struct.pack('<%df'%(len(normals)*3),*[c for n in normals for c in n])
    ubin=struct.pack('<%df'%(len(uvs)*2),*[c for u in uvs for c in u])
    blob=ibin+pbin+nbin+ubin
    def mm(a,d):
        if not a: return [0]*d,[0]*d
        return [min(v[i] for v in a) for i in range(d)],[max(v[i] for v in a) for i in range(d)]
    pmin,pmax=mm(positions,3)
    bv=[{'buffer':0,'byteOffset':0,'byteLength':len(ibin)},
        {'buffer':0,'byteOffset':len(ibin),'byteLength':len(pbin)},
        {'buffer':0,'byteOffset':len(ibin)+len(pbin),'byteLength':len(nbin)},
        {'buffer':0,'byteOffset':len(ibin)+len(pbin)+len(nbin),'byteLength':len(ubin)}]
    acc=[{'bufferView':0,'componentType':5125,'count':len(indices),'type':'SCALAR'},
         {'bufferView':1,'componentType':5126,'count':len(positions),'type':'VEC3','min':pmin,'max':pmax},
         {'bufferView':2,'componentType':5126,'count':len(normals),'type':'VEC3'},
         {'bufferView':3,'componentType':5126,'count':len(uvs),'type':'VEC2'}]
    gltf={'asset':{'version':'2.0','generator':'tod_model_to_gltf'},
          'buffers':[{'byteLength':len(blob)}],
          'bufferViews':bv,'accessors':acc,
          'meshes':[{'primitives':[{'attributes':{'POSITION':1,'NORMAL':2,'TEXCOORD_0':3},
                                    'indices':0,'mode':4}]}],
          'nodes':[{'mesh':0,'name':model['name']}],
          'scenes':[{'nodes':[0]}],'scene':0}
    return gltf, blob

def write_glb(path, gltf, blob):
    jb=json.dumps(gltf,separators=(',',':')).encode('utf-8')
    jb+=b' '*((4-len(jb)%4)%4)
    bb=blob+b'\x00'*((4-len(blob)%4)%4)
    total=12+8+len(jb)+8+len(bb)
    with open(path,'wb') as f:
        f.write(struct.pack('<III',0x46546C67,2,total))
        f.write(struct.pack('<II',len(jb),0x4E4F534A)); f.write(jb)
        f.write(struct.pack('<II',len(bb),0x004E4942)); f.write(bb)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('tmdl'); ap.add_argument('-o','--out',default=None)
    ap.add_argument('--no-bake',action='store_true',help='do not bake pivot transforms')
    ap.add_argument('--skeleton',action='store_true',help='also write <out>.skeleton.json')
    args=ap.parse_args()
    model=parse_tmdl(args.tmdl)
    print('model %r: %d pivots, %d meshes, %d texrefs'%(model['name'],len(model['pivots']),
          len(model['meshes']),len(model['textures'])))
    gltf,blob=build_glb(model,bake=not args.no_bake,sanity=True)
    out=args.out or os.path.splitext(args.tmdl)[0]+'.glb'
    write_glb(out,gltf,blob); print('wrote',out,'(%d bytes)'%os.path.getsize(out))
    if args.skeleton:
        sk=[{'i':i,'name':p['name'],'parent':p['parent'],'pos':p['pos'],'quat':p['quat']}
            for i,p in enumerate(model['pivots'])]
        sp=os.path.splitext(out)[0]+'.skeleton.json'
        json.dump({'model':model['name'],'textures':model['textures'],'pivots':sk},
                  open(sp,'w'),indent=1); print('wrote',sp)

if __name__=='__main__': main()
