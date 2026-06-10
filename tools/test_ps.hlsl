// SM3 test pixel shader: texture sample + arithmetic + clip (discard path)
sampler2D g_tex   : register(s0);
float4    g_fade  : register(c0);

struct PS_IN {
  float4 color  : COLOR0;
  float2 uv     : TEXCOORD0;
};

float4 main(PS_IN i) : COLOR {
  float4 t = tex2D(g_tex, i.uv);
  float4 c = t * i.color * g_fade;
  clip(c.a - 0.01);
  return c;
}
