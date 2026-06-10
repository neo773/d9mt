// shadertri pixel shader: interpolated color times a constant tint (c0)
float4 tint : register(c0);

float4 main(float4 color : COLOR0) : COLOR {
  return color * tint;
}
