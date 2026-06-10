// Host-side test for src/d3d9/d9mt_translate.cpp: run the in-process
// translation chain on a compiled shader blob and dump the reflection
// the runtime will consume.
#include <cstdio>
#include <fstream>
#include <vector>

#include "../src/d3d9/d9mt_translate.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: translate-test <in.vso|in.pso> [out.metal]\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  std::vector<char> bytecode((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());

  D9MTShaderInfo si;
  std::string err;
  if (!d9mt_translate(bytecode.data(), si, err)) {
    fprintf(stderr, "FAIL: %s\n", err.c_str());
    return 1;
  }

  printf("type: %s\n", si.isVertexShader ? "vs" : "ps");
  printf("floatConstCount: %u\n", si.floatConstCount);
  for (auto &i : si.inputs)
    printf("input: usage=%u usageIndex=%u -> attribute(%u)\n", i.usage,
           i.usageIndex, i.location);
  printf("ab entries: %u\n", si.abEntryCount);
  printf("id cbuffer=%d clip_info=%d spec_state=%d\n", si.idCbuffer,
         si.idClipInfo, si.idSpecState);
  for (auto &t : si.textures)
    printf("texture: s%u%s -> id(%u)\n", t.samplerSlot,
           t.shadow ? " (shadow)" : "", t.abId);
  printf("render_state buffer index: %d\n", si.rsBufferIndex);
  printf("sampler heap buffer index: %d\n", si.samplerHeapIndex);

  if (argc > 2) {
    std::ofstream out(argv[2]);
    out << si.msl;
    printf("wrote %zu bytes of MSL to %s\n", si.msl.size(), argv[2]);
  }
  return 0;
}
