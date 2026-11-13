// gp_mayhem_ps.hlsl - [mayhem_title] pixel shader: the burn-through and the burning MAYHEM, one pass over the effect
// region E (uv 0..1). Output premultiplied (blend ONE, INVSRCALPHA). The numpy twin used for tuning is
// mayhem_title/tools/burn_model.py (same constants, same formulas). Compiled by build.bat:
//   fxc /nologo /T ps_3_0 /E main /Fh gp_mayhem_ps.h /Vn g_mayhem_ps gp_mayhem_ps.hlsl
sampler2D sLet   : register(s0);   // lettering, premultiplied, mips
sampler2D sFx    : register(s1);   // R hole field, G flame band, B ember cracks, A burn noise
sampler2D sNoise : register(s2);   // two smooth noise channels (wrap)
sampler2D sFire  : register(s3);   // fire flipbook atlas 8 x 8 (additive)

float4 c0 : register(c0);   // p (front), t (s), opacity, calm (0 burning .. 1 settled after the reveal)
float4 c1 : register(c1);   // ignition u, v, E aspect, kT (T per E height)
float4 c2 : register(c2);   // ke (hole field -> T), rim, curl, scorch (T units)
float4 c3 : register(c3);   // noise amplitude (T), 1 / ramp, shimmer amount, fire amount
float4 c4 : register(c4);   // fire frame A (u0, v0), frame B (u0, v0) in the atlas
float4 c5 : register(c5);   // fire blend, top line v at u = 0, its slope, 1 / band height
float4 c6 : register(c6);   // crack amount, ember speck amount, 0, 0

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float4 m = tex2D(sFx, uv);
    float  p = c0.x, t = c0.y, calm = c0.w;
    float2 n2 = tex2D(sNoise, uv * float2(7.0, 4.41)).rg;
    float  n = n2.r;
    // burn time of this pixel: distance from the ignition (E heights) + ragged noise
    float2 dv = (uv - c1.xy) * float2(c1.z, 1.0);
    float  T = length(dv) * c1.w + (m.a - 0.5) * c3.x;
    float  dfront = p - T;                     // > 0: the front has passed
    float  dedge = (0.5 - m.r) * c2.x;         // > 0: inside the final hole
    float  d = min(dfront, dedge);             // distance behind the burning edge
    float  settle = saturate((dfront - dedge) / (c2.z * 2.0));
    float  rest = max(settle, calm);

    // ---- unburnt side: curled paper (warm back of the paper, browned at the edge), the fire's glow, the scorch
    float  x = -d;
    float  wc = c2.z * (1.0 - 0.75 * rest);
    float  cu = saturate(x / max(wc, 1e-5));
    float3 paper = float3(0.66, 0.46, 0.27) * (0.55 + 0.45 * sin(cu * 3.1416)) * (0.65 + 0.35 * cu) + float3(0.55, 0.22, 0.04) * (1.0 - cu);
    float  sc = saturate(1.0 - (x - wc) / c2.w); sc = sc * sc * (0.85 - 0.15 * n);
    float  gl = saturate(1.0 - x / (c2.z * 4.0)) * (1.0 - rest); gl = gl * gl * (0.8 + 0.2 * sin(t * 17.0 + n * 8.0));
    float  far = saturate((0.985 - m.r) * 60.0);
    float  incurl = step(x, wc);               // x < wc (x >= 0 here)
    float3 outc = lerp(float3(0.10, 0.05, 0.02) * sc, paper, incurl) + float3(1.0, 0.45, 0.08) * gl * 1.6;
    float  outa = lerp(sc * 0.92, 1.0, incurl);

    // ---- burnt side: the hot rim, its bloom, char with dying ember specks
    float  rimw = c2.y * (1.0 + 0.6 * settle);
    float  rim = saturate(1.0 - d / rimw);
    float  flick = 0.75 + 0.25 * sin(t * 13.0 + uv.x * 40.0 + n * 6.0);
    float  glow = rim * (1.0 - 0.88 * rest * (0.6 + 0.4 * flick));
    float3 hot = lerp(float3(1.0, 0.38, 0.05), float3(1.0, 0.92, 0.55), rim);
    float  bloom = saturate(1.0 - d / (c2.z * 2.5)) * (1.0 - rest);
    float  age = max(dfront - rimw, 0.0);
    float  speck = saturate((n - 0.72) * 6.0) * exp(-age * 6.0) * (1.0 - calm * 0.7) * c6.y;
    float3 ch = float3(0.055, 0.038, 0.028) * (0.75 + 0.5 * m.a) + float3(1.0, 0.35, 0.04) * speck * 0.8;
    float3 inc = ch * (1.0 - glow) + hot * glow * 1.6 + float3(1.0, 0.40, 0.06) * bloom * bloom * 0.6;

    // ---- MAYHEM inside the hole: dark -> glowing -> final; heat shimmer on the flames
    float  k = saturate((dfront - rimw) * c3.y);
    float2 sh = (tex2D(sNoise, uv * float2(4.0, 2.5) + float2(0.0, t * 0.55)).rg - 0.5) * float2(0.010, 0.014) * m.g * c3.z;
    float4 L = tex2D(sLet, uv + sh);
    float  la = L.a * saturate(k * 3.0);
    float  heat = 4.0 * k * (1.0 - k);
    float3 lc = L.rgb * (k * k) + float3(1.0, 0.42, 0.06) * heat * L.a * 0.9;
    // the looping fire over the flame tips (additive), the ember cracks flicker (additive)
    float  fy = 1.0 + (uv.y - (c5.y + c5.z * uv.x)) * c5.w;
    float2 fuv = float2(frac(uv.x * 1.05), saturate(fy)) * (0.996 / 8.0) + 0.002 / 8.0;
    float3 fire = lerp(tex2D(sFire, c4.xy + fuv).rgb, tex2D(sFire, c4.zw + fuv).rgb, c5.x);
    fire *= step(0.0, fy) * step(fy, 1.0) * saturate(fy * 4.0) * m.g * k * c3.w;
    float  cf = 0.55 + 0.45 * sin(t * 9.0 + n * 9.0);
    float3 crack = float3(1.0, 0.45, 0.08) * m.b * cf * k * c6.x;
    inc = lc + inc * (1.0 - la) + fire + crack;

    float  burnt = step(0.0, d);
    float3 col = lerp(outc * far, inc, burnt);
    float  a = lerp(outa * far, 1.0, burnt);
    return float4(col, a) * c0.z;
}
