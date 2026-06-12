// fogtest VS: clip-space passthrough + constant fog factor via oFog.
// vs_2_0 so dxso emits the legacy RasterOutFog output the PS fog path reads.
float4 cfg : register(c4); // x = fog factor written to oFog

struct VOut {
  float4 pos : POSITION;
  float fog : FOG;
};

VOut main(float3 p : POSITION) {
  VOut o;
  o.pos = float4(p, 1.0);
  o.fog = cfg.x;
  return o;
}
