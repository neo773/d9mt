// In-process D3D9 shader translation: SM1-3 bytecode -> SPIR-V (vendored
// DXVK dxso) -> MSL (vendored SPIRV-Cross), plus the reflection data the
// runtime needs to feed the generated shader.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct D9MTShaderInfo {
  std::string msl;        // generated Metal source, entry point "main0"
  bool isVertexShader = false;

  // vertex inputs (VS only): D3DDECLUSAGE semantic -> [[attribute(N)]]
  struct Input {
    uint32_t usage;       // D3DDECLUSAGE / DxsoUsage value
    uint32_t usageIndex;
    uint32_t location;    // stage_in attribute index
  };
  std::vector<Input> inputs;

  // set-0 argument buffer ([[buffer(0)]]): 8 bytes per [[id]] on tier 2.
  // -1 means the shader does not reference that resource.
  uint32_t abEntryCount = 0;  // max id + 1
  int idCbuffer = -1;         // "c": { int4 i[16]; float4 f[N]; }
  int idClipInfo = -1;        // "clip_info": float4 clip_planes[6]
  int idSpecState = -1;       // "spec_state": 15 dwords
  struct TexBinding {
    uint32_t samplerSlot;     // D3D9 sampler index parsed from "tN_..."
    uint32_t abId;
    bool shadow;              // the depth2d "..._shadow" variant
  };
  std::vector<TexBinding> textures;

  int rsBufferIndex = -1;          // render_state push block [[buffer(N)]]
  int samplerHeapIndex = -1;       // set-15 sampler heap [[buffer(N)]], or -1

  // MSL function constants (from SPIR-V spec constants): {constant_id,
  // default value}. ALL of them must be supplied at newFunction time —
  // creating the function unspecialized crashes PSO creation.
  std::vector<std::pair<uint32_t, uint32_t>> specConstants;

  uint32_t floatConstCount = 0;    // 256 (VS) / 224 (PS)
};

// Returns false and fills err on failure. bytecode is the D3D9 shader
// function blob handed to Create{Vertex,Pixel}Shader.
bool d9mt_translate(const void *bytecode, D9MTShaderInfo &out,
                    std::string &err);
