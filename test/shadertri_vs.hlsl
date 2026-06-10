// shadertri vertex shader: transform by mvp (c0-c3), pass color through
float4x4 mvp : register(c0);

struct VIn {
  float3 pos : POSITION;
  float4 color : COLOR0;
};

struct VOut {
  float4 pos : POSITION;
  float4 color : COLOR0;
};

VOut main(VIn i) {
  VOut o;
  o.pos = mul(float4(i.pos, 1.0), mvp);
  o.color = i.color;
  return o;
}
