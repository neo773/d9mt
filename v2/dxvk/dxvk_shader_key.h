#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "dxvk_hash.h"
#include "dxvk_include.h"

namespace dxvk {

  // Shader look-up key. Unlike the rest of the shader module this is NOT a
  // throwaway stub: the frontend stores D3D9 shaders in
  // std::unordered_map<DxvkShaderHash, ..., DxvkHash, DxvkEq>, so eq()/hash()/
  // toString() must behave correctly. The implementation mirrors the real DXVK
  // one (only the digest bytes + stage feed the hash) — no Vulkan runtime, no
  // SPIR-V, just bytes in and a stable identity out.
  class DxvkShaderHash {

  public:

    DxvkShaderHash() = default;

    DxvkShaderHash(
            VkShaderStageFlagBits stage,
            uint32_t              codeSize,
      const uint8_t*              hash,
            size_t                hashSize)
    : m_stage(uint16_t(stage)), m_size(codeSize) {
      for (size_t i = 0; i < m_hash.size(); i++)
        m_hash[i] = getDword(hash, hashSize, i * sizeof(uint32_t));
    }

    // Variant carrying an extra metadata hash (e.g. xfb). Folded into the same
    // four-dword digest so identity stays compact.
    DxvkShaderHash(
            VkShaderStageFlagBits stage,
            uint32_t              codeSize,
      const uint8_t*              hash,
            size_t                hashSize,
      const uint8_t*              metaHash,
            size_t                metaSize)
    : DxvkShaderHash(stage, codeSize, hash, hashSize) {
      m_xfb = 1u;
      for (size_t i = 0; i < m_hash.size(); i++)
        m_hash[i] ^= getDword(metaHash, metaSize, i * sizeof(uint32_t));
    }

    VkShaderStageFlagBits stage() const {
      return VkShaderStageFlagBits(m_stage);
    }

    bool hasXfb() const {
      return m_xfb != 0u;
    }

    // Human-readable shader name used for debug logging + dump filenames.
    std::string toString() const {
      std::string result = "SHADER_";
      appendByte(result, uint8_t(m_stage));
      result += '_';
      for (uint32_t dword : m_hash) {
        for (int byte = 3; byte >= 0; byte--)
          appendByte(result, uint8_t(dword >> (byte * 8)));
      }
      return result;
    }

    bool eq(const DxvkShaderHash& other) const {
      return m_stage == other.m_stage
          && m_xfb   == other.m_xfb
          && m_size  == other.m_size
          && m_hash  == other.m_hash;
    }

    size_t hash() const {
      DxvkHashState state;
      state.add(m_stage);
      state.add(m_xfb);
      state.add(m_size);
      for (uint32_t dword : m_hash)
        state.add(dword);
      return state;
    }

  private:

    uint16_t                  m_stage = uint16_t(-1);
    uint16_t                  m_xfb   = 0u;
    uint32_t                  m_size  = 0u;
    std::array<uint32_t, 4u>  m_hash  = { };

    // Reads a little-endian dword from a hash buffer, tolerating short buffers
    // (SHA-1 is 20 bytes → only the first 5 dwords exist; we read 4).
    static uint32_t getDword(const uint8_t* data, size_t size, size_t offset) {
      uint32_t value = 0u;
      for (size_t i = 0; i < sizeof(uint32_t); i++) {
        if (offset + i < size)
          value |= uint32_t(data[offset + i]) << (i * 8);
      }
      return value;
    }

    // Appends a byte as two lowercase hex chars.
    static void appendByte(std::string& out, uint8_t value) {
      const char* digits = "0123456789abcdef";
      out += digits[(value >> 4) & 0xf];
      out += digits[value & 0xf];
    }

  };

}
