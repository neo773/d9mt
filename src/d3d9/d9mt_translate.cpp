// Thin wrapper over the vendored DXVK dxso translator and SPIRV-Cross:
// the same chain tools/dxso2msl.sh runs offline, but in-process.
// Everything under vendor/ is verbatim upstream source.

#include "d9mt_translate.h"

#include "../../vendor/dxvk/src/dxso/dxso_module.h"
#include "../../vendor/dxvk/src/dxso/dxso_reader.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_shader.h"
#include "../../vendor/dxvk/src/d3d9/d3d9_caps.h"

#include "../../vendor/spirv-cross/spirv_msl.hpp"

using namespace dxvk;

// dxvk defines these once per dll; we are that dll here
// (dxso_options.cpp is excluded from the build: its ctor is device-coupled)
dxvk::DxsoOptions::DxsoOptions() {}
dxvk::Logger dxvk::Logger::s_instance("d9mt_dxso.log");

namespace {

// max automatic [[id]] seen in descriptor set 0, for sizing the argument
// buffer (tier 2: one 8-byte slot per id)
void track_ab_id(uint32_t id, uint32_t &count) {
  if (id + 1 > count)
    count = id + 1;
}

} // namespace

bool d9mt_translate(const void *bytecode, D9MTShaderInfo &out,
                    std::string &err) {
  try {
    DxsoReader reader(reinterpret_cast<const char *>(bytecode));
    DxsoModule module(reader);

    const DxsoProgramInfo &info = module.info();
    out.isVertexShader = info.type() == DxsoProgramTypes::VertexShader;

    DxsoAnalysisInfo analysis = module.analyze();

    DxsoModuleInfo moduleInfo = {};
    // HWVP layout, same values D3D9DeviceEx::DetermineConstantLayouts()
    // uses without SWVP
    D3D9ConstantLayout layout = {};
    layout.floatCount = out.isVertexShader ? caps::MaxFloatConstantsVS
                                           : caps::MaxSM3FloatConstantsPS;
    layout.intCount = caps::MaxOtherConstants;
    layout.boolCount = caps::MaxOtherConstants;
    layout.bitmaskCount = align(layout.boolCount, 32u) / 32u;
    out.floatConstCount = layout.floatCount;

    Rc<DxvkShader> shader = module.compile(moduleInfo, "d9mt", analysis,
                                           layout);

    // vertex input signature: semantic -> linker slot (= SPIR-V location,
    // = MSL [[attribute(N)]]); the isgn is produced by compile()
    if (out.isVertexShader) {
      const DxsoIsgn &isgn = module.isgn();
      for (uint32_t i = 0; i < isgn.elemCount; i++) {
        const DxsoIsgnEntry &e = isgn.elems[i];
        out.inputs.push_back({static_cast<uint32_t>(e.semantic.usage),
                              e.semantic.usageIndex, e.slot});
      }
    }
    SpirvCodeBuffer code = shader->getRawCode();

    // SPIR-V -> MSL, mirroring the offline flags:
    //   --msl --msl-version 30000 --msl-argument-buffers
    //   --msl-argument-buffer-tier 2
    spirv_cross::CompilerMSL msl(code.data(), code.dwords());
    spirv_cross::CompilerMSL::Options opts;
    opts.set_msl_version(3, 0, 0);
    opts.argument_buffers = true;
    opts.argument_buffers_tier =
        spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
    msl.set_msl_options(opts);

    out.msl = msl.compile();

    // reflection: where did everything land?
    spirv_cross::ShaderResources res = msl.get_shader_resources();

    for (const auto &ub : res.uniform_buffers) {
      uint32_t set = msl.get_decoration(ub.id, spv::DecorationDescriptorSet);
      if (set != 0)
        continue;
      uint32_t id = msl.get_automatic_msl_resource_binding(ub.id);
      track_ab_id(id, out.abEntryCount);
      // for buffer blocks Resource.name is the block (type) name, e.g.
      // "cbuffer_t"; the instance name ("c") is on the variable id
      std::string name = ub.name;
      std::string var = msl.get_name(ub.id);
      if (name.rfind("cbuffer", 0) == 0 || var == "c" || var == "cb")
        out.idCbuffer = static_cast<int>(id);
      else if (name.rfind("clip_info", 0) == 0 || var == "clip_info")
        out.idClipInfo = static_cast<int>(id);
      else if (name.rfind("spec_state", 0) == 0 || var == "spec_state")
        out.idSpecState = static_cast<int>(id);
    }

    for (const auto &img : res.separate_images) {
      uint32_t set = msl.get_decoration(img.id, spv::DecorationDescriptorSet);
      if (set != 0)
        continue;
      uint32_t id = msl.get_automatic_msl_resource_binding(img.id);
      track_ab_id(id, out.abEntryCount);
      // dxso names textures "tN_2d" / "tN_3d" / "tN_cube", with a
      // "..._shadow" depth variant alongside
      const std::string &name = img.name;
      if (name.size() >= 2 && name[0] == 't') {
        uint32_t slot = 0;
        size_t p = 1;
        while (p < name.size() && name[p] >= '0' && name[p] <= '9')
          slot = slot * 10 + (name[p++] - '0');
        bool shadow = name.size() >= 7 &&
                      name.compare(name.size() - 7, 7, "_shadow") == 0;
        out.textures.push_back({slot, id, shadow});
      }
    }

    for (const auto &pc : res.push_constant_buffers)
      out.rsBufferIndex =
          static_cast<int>(msl.get_automatic_msl_resource_binding(pc.id));

    for (const auto &sc : msl.get_specialization_constants()) {
      const auto &cst = msl.get_constant(sc.id);
      out.specConstants.push_back(
          {sc.constant_id, cst.scalar(0, 0)});
    }

    for (const auto &smp : res.separate_samplers) {
      uint32_t set = msl.get_decoration(smp.id, spv::DecorationDescriptorSet);
      if (set == 15)
        out.samplerHeapIndex =
            static_cast<int>(msl.get_automatic_msl_resource_binding(smp.id));
    }

    return true;
  } catch (const DxvkError &e) {
    err = e.message();
    return false;
  } catch (const std::exception &e) {
    err = e.what();
    return false;
  }
}
