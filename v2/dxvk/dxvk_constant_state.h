#pragma once

#include <cstring>

#include "dxvk_include.h"
#include "dxvk_limits.h"

// Pipeline dynamic/constant state value types — copied verbatim from
// dxvk-ref/dxvk_constant_state.h (minus the dxvk_framebuffer/dxvk_buffer/
// dxvk_shader includes those structs don't actually use). Every type is a packed
// POD over Vulkan TYPES/ENUMS only — no Vulkan runtime. The D3D9 frontend builds
// these by value in EmitCs lambdas (blend/raster/depth-stencil/vertex-input),
// so they MUST be complete, not forward-declared. The shim's DxvkContext setters
// ignore them (the NDC triangle uses fixed pipeline state). See SHIM_SPEC.md.

namespace dxvk {

  struct DxvkBlendConstants {
    float r, g, b, a;
    bool operator == (const DxvkBlendConstants& o) const { return r==o.r && g==o.g && b==o.b && a==o.a; }
    bool operator != (const DxvkBlendConstants& o) const { return !(*this == o); }
  };

  struct DxvkDepthBiasRepresentation {
    VkDepthBiasRepresentationEXT depthBiasRepresentation;
    VkBool32                     depthBiasExact;
    bool operator == (const DxvkDepthBiasRepresentation& o) const {
      return depthBiasRepresentation == o.depthBiasRepresentation && depthBiasExact == o.depthBiasExact; }
    bool operator != (const DxvkDepthBiasRepresentation& o) const { return !(*this == o); }
  };

  struct DxvkDepthBias {
    float depthBiasConstant;
    float depthBiasSlope;
    float depthBiasClamp;
    bool operator == (const DxvkDepthBias& o) const {
      return depthBiasConstant==o.depthBiasConstant && depthBiasSlope==o.depthBiasSlope && depthBiasClamp==o.depthBiasClamp; }
    bool operator != (const DxvkDepthBias& o) const { return !(*this == o); }
  };

  struct DxvkDepthBounds {
    float minDepthBounds;
    float maxDepthBounds;
    bool operator == (const DxvkDepthBounds& o) const { return minDepthBounds==o.minDepthBounds && maxDepthBounds==o.maxDepthBounds; }
    bool operator != (const DxvkDepthBounds& o) const { return !(*this == o); }
  };

  struct DxvkInputAssemblyState {

  public:

    DxvkInputAssemblyState() = default;
    DxvkInputAssemblyState(VkPrimitiveTopology topology, bool restart)
    : m_primitiveTopology(uint16_t(topology)), m_primitiveRestart(uint16_t(restart)),
      m_patchVertexCount(0u), m_reserved(0u) { }

    VkPrimitiveTopology primitiveTopology() const {
      return VkPrimitiveTopology(m_primitiveTopology) <= VK_PRIMITIVE_TOPOLOGY_PATCH_LIST
        ? VkPrimitiveTopology(m_primitiveTopology) : VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    }
    bool primitiveRestart() const { return m_primitiveRestart; }
    uint32_t patchVertexCount() const { return m_patchVertexCount; }
    void setPrimitiveTopology(VkPrimitiveTopology t) { m_primitiveTopology = uint16_t(t); }
    void setPrimitiveRestart(bool e) { m_primitiveRestart = e; }
    void setPatchVertexCount(uint32_t c) { m_patchVertexCount = c; }

  private:

    uint16_t m_primitiveTopology : 4;
    uint16_t m_primitiveRestart  : 1;
    uint16_t m_patchVertexCount  : 6;
    uint16_t m_reserved          : 5;

  };

  struct DxvkRasterizerState {

  public:

    VkPolygonMode polygonMode() const { return VkPolygonMode(m_polygonMode); }
    VkCullModeFlags cullMode() const { return VkCullModeFlags(m_cullMode); }
    VkFrontFace frontFace() const { return VkFrontFace(m_frontFace); }
    bool depthClip() const { return m_depthClipEnable; }
    VkConservativeRasterizationModeEXT conservativeMode() const { return VkConservativeRasterizationModeEXT(m_conservativeMode); }
    VkSampleCountFlags sampleCount() const { return VkSampleCountFlags(m_sampleCount); }
    bool flatShading() const { return m_flatShading; }
    VkLineRasterizationModeEXT lineMode() const { return VkLineRasterizationModeEXT(m_lineMode); }

    void setPolygonMode(VkPolygonMode m) { m_polygonMode = uint16_t(m); }
    void setCullMode(VkCullModeFlags m) { m_cullMode = uint16_t(m); }
    void setFrontFace(VkFrontFace f) { m_frontFace = uint16_t(f); }
    void setDepthClip(bool e) { m_depthClipEnable = e; }
    void setConservativeMode(VkConservativeRasterizationModeEXT m) { m_conservativeMode = uint16_t(m); }
    void setSampleCount(VkSampleCountFlags c) { m_sampleCount = uint16_t(c); }
    void setFlatShading(bool e) { m_flatShading = e; }
    void setLineMode(VkLineRasterizationModeEXT m) { m_lineMode = uint16_t(m); }

  private:

    uint16_t m_polygonMode      : 2;
    uint16_t m_cullMode         : 2;
    uint16_t m_frontFace        : 1;
    uint16_t m_depthClipEnable  : 1;
    uint16_t m_conservativeMode : 2;
    uint16_t m_sampleCount      : 5;
    uint16_t m_flatShading      : 1;
    uint16_t m_lineMode         : 2;

  };

  class DxvkMultisampleState {

  public:

    uint16_t sampleMask() const { return m_sampleMask; }
    bool alphaToCoverage() const { return m_enableAlphaToCoverage; }
    void setSampleMask(uint16_t m) { m_sampleMask = m; }
    void setAlphaToCoverage(bool a) { m_enableAlphaToCoverage = a; }

  private:

    uint16_t m_sampleMask;
    uint16_t m_enableAlphaToCoverage : 1;
    uint16_t m_reserved              : 15;

  };

  class DxvkStencilOp {

  public:

    VkStencilOp failOp() const { return VkStencilOp(m_failOp); }
    VkStencilOp passOp() const { return VkStencilOp(m_passOp); }
    VkStencilOp depthFailOp() const { return VkStencilOp(m_depthFailOp); }
    VkCompareOp compareOp() const { return VkCompareOp(m_compareOp); }
    uint8_t compareMask() const { return m_compareMask; }
    uint8_t writeMask() const { return m_writeMask; }

    void setFailOp(VkStencilOp op) { m_failOp = uint16_t(op); }
    void setPassOp(VkStencilOp op) { m_passOp = uint16_t(op); }
    void setDepthFailOp(VkStencilOp op) { m_depthFailOp = uint16_t(op); }
    void setCompareOp(VkCompareOp op) { m_compareOp = uint16_t(op); }
    void setCompareMask(uint8_t m) { m_compareMask = m; }
    void setWriteMask(uint8_t m) { m_writeMask = m; }

    bool eq(const DxvkStencilOp& other) const { return !std::memcmp(this, &other, sizeof(*this)); }
    bool normalize(VkCompareOp) { return false; }

  private:

    uint16_t m_failOp      : 3;
    uint16_t m_passOp      : 3;
    uint16_t m_depthFailOp : 3;
    uint16_t m_compareOp   : 3;
    uint16_t m_reserved    : 4;
    uint8_t  m_compareMask;
    uint8_t  m_writeMask;

  };

  class DxvkDepthStencilState {

  public:

    bool depthTest() const { return m_enableDepthTest; }
    bool depthWrite() const { return m_enableDepthWrite; }
    bool stencilTest() const { return m_enableStencilTest; }
    VkCompareOp depthCompareOp() const { return VkCompareOp(m_depthCompareOp); }
    DxvkStencilOp stencilOpFront() const { return m_stencilOpFront; }
    DxvkStencilOp stencilOpBack() const { return m_stencilOpBack; }

    void setDepthTest(bool e) { m_enableDepthTest = e; }
    void setDepthWrite(bool e) { m_enableDepthWrite = e; }
    void setStencilTest(bool e) { m_enableStencilTest = e; }
    void setDepthCompareOp(VkCompareOp op) { m_depthCompareOp = uint16_t(op); }
    void setStencilOpFront(DxvkStencilOp op) { m_stencilOpFront = op; }
    void setStencilOpBack(DxvkStencilOp op) { m_stencilOpBack = op; }
    void normalize() { }

  private:

    uint16_t m_enableDepthTest   : 1;
    uint16_t m_enableDepthWrite  : 1;
    uint16_t m_enableStencilTest : 1;
    uint16_t m_depthCompareOp    : 3;
    uint16_t m_reserved          : 10;
    DxvkStencilOp m_stencilOpFront;
    DxvkStencilOp m_stencilOpBack;

  };

  class DxvkLogicOpState {

  public:

    bool logicOpEnable() const { return m_logicOpEnable; }
    VkLogicOp logicOp() const { return VkLogicOp(m_logicOp); }
    void setLogicOp(bool enable, VkLogicOp op) { m_logicOpEnable = enable; m_logicOp = uint8_t(op); }

  private:

    uint8_t m_logicOpEnable : 1;
    uint8_t m_logicOp       : 4;
    uint8_t m_reserved      : 3;

  };

  class DxvkBlendMode {

  public:

    bool blendEnable() const { return m_enableBlending; }
    VkBlendFactor colorSrcFactor() const { return VkBlendFactor(m_colorSrcFactor); }
    VkBlendFactor colorDstFactor() const { return VkBlendFactor(m_colorDstFactor); }
    VkBlendOp colorBlendOp() const { return VkBlendOp(m_colorBlendOp); }
    VkBlendFactor alphaSrcFactor() const { return VkBlendFactor(m_alphaSrcFactor); }
    VkBlendFactor alphaDstFactor() const { return VkBlendFactor(m_alphaDstFactor); }
    VkBlendOp alphaBlendOp() const { return VkBlendOp(m_alphaBlendOp); }
    VkColorComponentFlags writeMask() const { return VkColorComponentFlags(m_writeMask); }

    void setBlendEnable(bool e) { m_enableBlending = e; }
    void setColorOp(VkBlendFactor s, VkBlendFactor d, VkBlendOp op) {
      m_colorSrcFactor = uint32_t(s); m_colorDstFactor = uint32_t(d); m_colorBlendOp = uint32_t(op); }
    void setAlphaOp(VkBlendFactor s, VkBlendFactor d, VkBlendOp op) {
      m_alphaSrcFactor = uint32_t(s); m_alphaDstFactor = uint32_t(d); m_alphaBlendOp = uint32_t(op); }
    void setWriteMask(VkColorComponentFlags m) { m_writeMask = m; }
    void normalize() { }

  private:

    uint32_t m_enableBlending : 1;
    uint32_t m_colorSrcFactor : 5;
    uint32_t m_colorDstFactor : 5;
    uint32_t m_colorBlendOp   : 3;
    uint32_t m_alphaSrcFactor : 5;
    uint32_t m_alphaDstFactor : 5;
    uint32_t m_alphaBlendOp   : 3;
    uint32_t m_writeMask      : 4;
    uint32_t m_reserved       : 1;

  };

  struct DxvkVertexAttribute {
    uint32_t location;
    uint32_t binding;
    VkFormat format;
    uint32_t offset;
  };

  struct DxvkPackedVertexAttribute {
    DxvkPackedVertexAttribute() = default;
    DxvkPackedVertexAttribute(const DxvkVertexAttribute& a)
    : location(a.location), binding(a.binding), format(uint32_t(a.format)), offset(a.offset), reserved(0u) { }

    uint32_t location : 5;
    uint32_t binding  : 5;
    uint32_t format   : 7;
    uint32_t offset   : 11;
    uint32_t reserved : 4;

    DxvkVertexAttribute unpack() const {
      DxvkVertexAttribute r = { };
      r.location = location; r.binding = binding; r.format = VkFormat(format); r.offset = offset;
      return r;
    }
  };

  struct DxvkVertexBinding {
    uint32_t binding;
    uint32_t extent;
    VkVertexInputRate inputRate;
    uint32_t divisor;
  };

  struct DxvkPackedVertexBinding {
    DxvkPackedVertexBinding() = default;
    DxvkPackedVertexBinding(const DxvkVertexBinding& b)
    : binding(b.binding), extent(b.extent), inputRate(uint32_t(b.inputRate)),
      divisor(b.divisor < (1u << 14) ? b.divisor : 0u) { }

    uint32_t binding   : 5;
    uint32_t extent    : 12;
    uint32_t inputRate : 1;
    uint32_t divisor   : 14;

    DxvkVertexBinding unpack() const {
      DxvkVertexBinding r = { };
      r.binding = binding; r.extent = extent; r.inputRate = VkVertexInputRate(inputRate); r.divisor = divisor;
      return r;
    }
  };

  class DxvkVertexInput {

  public:

    DxvkVertexInput() = default;
    DxvkVertexInput(const DxvkVertexAttribute& attribute) : m_attribute(attribute) { }
    DxvkVertexInput(const DxvkVertexBinding& binding) : m_binding(binding) { }

    DxvkVertexAttribute attribute() const { return m_attribute.unpack(); }
    DxvkVertexBinding binding() const { return m_binding.unpack(); }

  private:

    union {
      DxvkPackedVertexAttribute m_attribute;
      DxvkPackedVertexBinding   m_binding;
    };

  };

  static_assert(sizeof(DxvkVertexInput) == sizeof(uint32_t));

}
