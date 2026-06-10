// SM3 test vertex shader: typical of what GTA IV-era engines emit
float4x4 g_mvp      : register(c0);
float4   g_tint     : register(c4);

struct VS_IN {
  float3 pos    : POSITION;
  float4 color  : COLOR0;
  float2 uv     : TEXCOORD0;
};

struct VS_OUT {
  float4 pos    : POSITION;
  float4 color  : COLOR0;
  float2 uv     : TEXCOORD0;
};

VS_OUT main(VS_IN i) {
  VS_OUT o;
  o.pos = mul(g_mvp, float4(i.pos, 1.0));
  o.color = i.color * g_tint;
  o.uv = i.uv;
  return o;
}
