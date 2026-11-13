// gp_mayhem_vs.hlsl - [mayhem_title] vertex shader (vs_3_0, needed by the ps_3_0 pixel shader): screen pixels -> clip
// space of the current viewport (c0 = 2 / width, -2 / height, offsets), colour and uv passed through.
// Compiled by build.bat: fxc /nologo /T vs_3_0 /E main /Fh gp_mayhem_vs.h /Vn g_mayhem_vs gp_mayhem_vs.hlsl
float4 c0 : register(c0);
struct VO { float4 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
VO main(float3 p : POSITION, float4 col : COLOR0, float2 uv : TEXCOORD0)
{
    VO o;
    o.pos = float4(p.x * c0.x + c0.z, p.y * c0.y + c0.w, 0.0, 1.0);
    o.col = col;
    o.uv = uv;
    return o;
}
