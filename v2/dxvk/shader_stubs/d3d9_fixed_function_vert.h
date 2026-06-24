#pragma once
#include <cstdint>
// STUB SPIR-V for the fixed-function vertex shader. The real header is a
// generated SPIR-V byte array; the Metal shim renders the FF triangle from a
// hardcoded MSL pipeline in D9mtBackend and DxvkSpirvShader ignores this code,
// so a single zero word is enough to satisfy the DxvkSpirvShader(info, code[N])
// constructor. See SHIM_SPEC.md (shader module = opaque, getCode -> empty).
static const uint32_t d3d9_fixed_function_vert[] = { 0u };
