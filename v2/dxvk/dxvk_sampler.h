#pragma once

#include "../util/util_bit.h"

#include "dxvk_hash.h"
#include "dxvk_include.h"

// Sampler shim. The Metal shim renders only the FF triangle (no texture
// sampling), so samplers never reach the GPU. We keep DxvkSamplerKey VERBATIM
// from dxvk-ref (a 32-byte Vulkan-types POD the frontend fills in its bind path)
// and provide opaque DxvkSampler / DxvkSamplerPool / DxvkSamplerStats stand-ins
// so DxvkDevice::createSampler()/getSamplerStats() type-check. See SHIM_SPEC.md.

namespace dxvk {

  class DxvkSamplerPool;

  struct DxvkSamplerKey {
    union {
      struct {
        uint32_t minFilter      :  1;
        uint32_t magFilter      :  1;
        uint32_t mipMode        :  1;
        uint32_t anisotropy     :  5;

        uint32_t addressU       :  3;
        uint32_t addressV       :  3;
        uint32_t addressW       :  3;
        uint32_t hasBorder      :  1;

        uint32_t lodBias        : 14;

        uint32_t minLod         : 12;
        uint32_t maxLod         : 12;

        uint32_t compareEnable  :  1;
        uint32_t compareOp      :  3;
        uint32_t reduction      :  2;
        uint32_t pixelCoord     :  1;
        uint32_t legacyCube     :  1;

        uint32_t viewSwizzleR   :  4;
        uint32_t viewSwizzleG   :  4;
        uint32_t viewSwizzleB   :  4;
        uint32_t viewSwizzleA   :  4;
        uint32_t reserved0      : 16;

        uint32_t viewFormat;
      } p;

      uint32_t properties[4] = { 0u, 0u, 0u, 0u };
    } u;

    VkClearColorValue borderColor = { };

    void setFilter(VkFilter min, VkFilter mag, VkSamplerMipmapMode mip) {
      u.p.minFilter = uint32_t(min);
      u.p.magFilter = uint32_t(mag);
      u.p.mipMode = uint32_t(mip);
    }

    void setAniso(uint32_t anisotropy) { u.p.anisotropy = std::min(anisotropy, 16u); }

    void setDepthCompare(bool enable, VkCompareOp op) {
      u.p.compareEnable = uint32_t(enable);
      u.p.compareOp = enable ? uint32_t(op) : 0u;
    }

    void setReduction(VkSamplerReductionMode reduction) { u.p.reduction = uint32_t(reduction); }
    void setUsePixelCoordinates(bool enable) { u.p.pixelCoord = uint32_t(enable); }
    void setLegacyCubeFilter(bool enable) { u.p.legacyCube = uint32_t(enable); }

    void setAddressModes(VkSamplerAddressMode u_, VkSamplerAddressMode v_, VkSamplerAddressMode w_) {
      u.p.addressU = u_;
      u.p.addressV = v_;
      u.p.addressW = w_;
      u.p.hasBorder = uint32_t(u_ == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
                            || v_ == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
                            || w_ == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    }

    void setLodRange(float min, float max, float bias) {
      u.p.minLod = bit::encodeFixed<uint32_t, 4, 8>(min);
      u.p.maxLod = bit::encodeFixed<uint32_t, 4, 8>(std::max(max, min));
      u.p.lodBias = bit::encodeFixed<int32_t, 6, 8>(bias);
    }

    void setBorderColor(VkClearColorValue color) { borderColor = color; }

    void setViewProperties(const VkComponentMapping& mapping, VkFormat format) {
      u.p.viewSwizzleR = uint32_t(mapping.r);
      u.p.viewSwizzleG = uint32_t(mapping.g);
      u.p.viewSwizzleB = uint32_t(mapping.b);
      u.p.viewSwizzleA = uint32_t(mapping.a);
      u.p.viewFormat = uint32_t(format);
    }

    bool eq(const DxvkSamplerKey& other) const {
      bool eq = u.properties[0] == other.u.properties[0]
             && u.properties[1] == other.u.properties[1]
             && u.properties[2] == other.u.properties[2]
             && u.properties[3] == other.u.properties[3];

      if (eq && u.p.hasBorder) {
        eq = borderColor.uint32[0] == other.borderColor.uint32[0]
          && borderColor.uint32[1] == other.borderColor.uint32[1]
          && borderColor.uint32[2] == other.borderColor.uint32[2]
          && borderColor.uint32[3] == other.borderColor.uint32[3];
      }

      return eq;
    }

    size_t hash() const {
      DxvkHashState hash;
      hash.add(u.properties[0]);
      hash.add(u.properties[1]);
      hash.add(u.properties[2]);
      hash.add(u.properties[3]);
      if (u.p.hasBorder) {
        hash.add(borderColor.uint32[0]);
        hash.add(borderColor.uint32[1]);
        hash.add(borderColor.uint32[2]);
        hash.add(borderColor.uint32[3]);
      }
      return hash;
    }
  };

  static_assert(sizeof(DxvkSamplerKey) == 32u);

  // Opaque sampler — holds nothing (no Metal sampler on the FF-triangle path).
  // The frontend keeps it in Rc<> and binds it; the shim's bindResourceSampler
  // ignores it.
  class DxvkSampler : public RcObject {

  public:

    DxvkSampler(const DxvkSamplerKey& key) : m_key(key) { }

    const DxvkSamplerKey& key() const { return m_key; }

  private:

    DxvkSamplerKey m_key = { };

  };

  // Live-sampler stats — the frontend only reads liveCount.
  struct DxvkSamplerStats {
    uint32_t liveCount = 0u;
    uint32_t totalCount = 0u;
  };

  // Pool constant referenced as DxvkSamplerPool::MaxSamplerCount. The shim never
  // pools real samplers; the constant just needs to exist.
  class DxvkSamplerPool {
  public:
    static constexpr uint32_t MaxSamplerCount = 4000u;
  };

}
