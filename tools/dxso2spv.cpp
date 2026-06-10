// Thin wrapper over DXVK's vendored dxso translator:
// D3D9 shader bytecode (SM1.1-3.0) in, raw SPIR-V out.
// Everything under vendor/dxvk is verbatim DXVK source (zlib/libpng license).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "../vendor/dxvk/src/dxso/dxso_module.h"
#include "../vendor/dxvk/src/dxso/dxso_reader.h"
#include "../vendor/dxvk/src/dxvk/dxvk_shader.h"
#include "../vendor/dxvk/src/d3d9/d3d9_caps.h"

using namespace dxvk;

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: dxso2spv <in.vso|in.pso> <out.spv>\n");
    return 2;
  }

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  std::vector<char> bytecode(
      (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  try {
    DxsoReader reader(bytecode.data());
    DxsoModule module(reader);

    const DxsoProgramInfo &info = module.info();
    bool isVS = info.type() == DxsoProgramTypes::VertexShader;
    fprintf(stderr, "%s shader, version %u.%u\n",
            isVS ? "vertex" : "pixel", info.majorVersion(),
            info.minorVersion());

    DxsoAnalysisInfo analysis = module.analyze();

    DxsoModuleInfo moduleInfo = {};
    // defaults from DxsoOptions member initializers; HWVP layout, same
    // values D3D9DeviceEx::DetermineConstantLayouts() uses without SWVP
    D3D9ConstantLayout layout = {};
    layout.floatCount = isVS ? caps::MaxFloatConstantsVS
                             : caps::MaxSM3FloatConstantsPS;
    layout.intCount = caps::MaxOtherConstants;
    layout.boolCount = caps::MaxOtherConstants;
    layout.bitmaskCount = align(layout.boolCount, 32u) / 32u;

    Rc<DxvkShader> shader =
        module.compile(moduleInfo, argv[1], analysis, layout);

    SpirvCodeBuffer code = shader->getRawCode();

    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char *>(code.data()), code.size());
    out.close();
    fprintf(stderr, "wrote %zu bytes of SPIR-V to %s\n", (size_t)code.size(),
            argv[2]);
  } catch (const DxvkError &e) {
    fprintf(stderr, "translation failed: %s\n", e.message().c_str());
    return 1;
  }
  return 0;
}
