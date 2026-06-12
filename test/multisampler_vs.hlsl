// multisampler VS: clip-space passthrough + UV passthrough.
struct VIn {
  float3 pos : POSITION;
  float2 uv : TEXCOORD0;
};

struct VOut {
  float4 pos : POSITION;
  float2 uv : TEXCOORD0;
};

VOut main(VIn i) {
  VOut o;
  o.pos = float4(i.pos, 1.0);
  o.uv = i.uv;
  return o;
}
