#include <metal_stdlib>
using namespace metal;

// Vertex layout produced by the D3D9 layer for a D3DFVF_XYZRHW|D3DFVF_DIFFUSE
// triangle: position is already converted from screen-pixel coordinates to NDC
// on the CPU; color is the raw D3DCOLOR dword (0xAARRGGBB, little-endian).
struct TriangleVertex {
  packed_float4 position;
  uint          color;
};

struct RasterData {
  float4 position [[position]];
  float4 color;
};

vertex RasterData triangleVertex(uint vertexId [[vertex_id]],
                                 constant TriangleVertex* vertices [[buffer(0)]]) {
  TriangleVertex input = vertices[vertexId];
  RasterData output;
  output.position = float4(input.position);
  output.color = float4(float((input.color >> 16) & 0xffu),   // R
                        float((input.color >> 8)  & 0xffu),   // G
                        float((input.color)       & 0xffu),   // B
                        float((input.color >> 24) & 0xffu))   // A
                 / 255.0f;
  return output;
}

fragment float4 triangleFragment(RasterData input [[stage_in]]) {
  return input.color;
}
