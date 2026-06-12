// multisampler PS: samples PS sampler slots s0..s7 simultaneously; one-hot
// weights (c0 = s0..s3, c1 = s4..s7) select which slot a draw probes.
sampler2D s0 : register(s0);
sampler2D s1 : register(s1);
sampler2D s2 : register(s2);
sampler2D s3 : register(s3);
sampler2D s4 : register(s4);
sampler2D s5 : register(s5);
sampler2D s6 : register(s6);
sampler2D s7 : register(s7);

float4 wA : register(c0);
float4 wB : register(c1);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  return tex2D(s0, uv) * wA.x + tex2D(s1, uv) * wA.y
       + tex2D(s2, uv) * wA.z + tex2D(s3, uv) * wA.w
       + tex2D(s4, uv) * wB.x + tex2D(s5, uv) * wB.y
       + tex2D(s6, uv) * wB.z + tex2D(s7, uv) * wB.w;
}
