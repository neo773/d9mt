// fogtest PS: solid red. ps_2_0 (< 3.0) so the dxso compiler appends the
// fixed-function fog stage gated by the FogEnabled/PixelFogMode spec bits.
float4 main() : COLOR {
  return float4(1.0, 0.0, 0.0, 1.0);
}
