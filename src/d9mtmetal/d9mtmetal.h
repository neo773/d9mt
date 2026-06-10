// d9mtmetal: tiny companion wine-unixlib supplying the two Metal entry
// points DXMT's winemetal lacks:
//   0: MTLDevice newLibraryWithSource   (runtime MSL compilation)
//   1: MTLDevice newRenderPipelineState WITH an MTLVertexDescriptor
//      (spirv-cross vertex shaders use [[stage_in]] and need one)
//
// obj_handle_t values interoperate with winemetal's handles: both are raw
// ObjC pointers in the same process.
//
// ABI rule: every field is fixed-width and pointers are zero-extended
// uint64_t, so the 32-bit and 64-bit param layouts are IDENTICAL and the
// wow64 call table can reuse the same entry points.

#ifndef D9MTMETAL_H
#define D9MTMETAL_H

#include <stdint.h>

enum d9mt_unix_func {
  D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE = 0,
  D9MT_FUNC_NEW_RENDER_PSO = 1,
  D9MT_FUNC_COUNT,
};

struct d9mt_newlibrary_params {
  uint64_t device;     /* in:  obj_handle_t MTLDevice */
  uint64_t source_ptr; /* in:  const char* UTF-8 MSL  */
  uint64_t source_len; /* in */
  uint32_t fast_math;  /* in:  bool */
  uint32_t padding;
  uint64_t ret_library; /* out: retained MTLLibrary or 0 */
  uint64_t ret_error;   /* out: retained NSError or 0 (caller releases) */
};

/* matches MTLVertexFormat raw values */
struct d9mt_vertex_attribute {
  uint32_t format;       /* MTLVertexFormat */
  uint32_t offset;
  uint32_t buffer_index;
  uint32_t location;     /* shader [[attribute(location)]] slot */
};

struct d9mt_vertex_layout {
  uint32_t buffer_index;
  uint32_t stride;
  uint32_t step_function; /* MTLVertexStepFunction: 1=per-vertex 2=per-instance */
  uint32_t step_rate;
};

struct d9mt_color_attachment {
  uint32_t pixel_format; /* MTLPixelFormat */
  uint32_t blending_enabled;
  uint32_t rgb_blend_op;       /* MTLBlendOperation */
  uint32_t alpha_blend_op;
  uint32_t src_rgb_blend_factor; /* MTLBlendFactor */
  uint32_t dst_rgb_blend_factor;
  uint32_t src_alpha_blend_factor;
  uint32_t dst_alpha_blend_factor;
  uint32_t write_mask; /* MTLColorWriteMask */
  uint32_t padding;
};

struct d9mt_pso_info {
  uint64_t vertex_function;   /* in: obj_handle_t MTLFunction */
  uint64_t fragment_function; /* in: obj_handle_t MTLFunction or 0 */
  struct d9mt_color_attachment colors[8];
  uint32_t depth_pixel_format;   /* MTLPixelFormat, 0 = none */
  uint32_t stencil_pixel_format;
  uint32_t raster_sample_count;
  uint32_t alpha_to_coverage;
  uint32_t num_attributes;
  uint32_t num_layouts;
  struct d9mt_vertex_attribute attributes[18];
  struct d9mt_vertex_layout layouts[16];
};

struct d9mt_newpso_params {
  uint64_t device;   /* in: obj_handle_t MTLDevice */
  uint64_t info_ptr; /* in: struct d9mt_pso_info* */
  uint64_t ret_pso;  /* out: retained MTLRenderPipelineState or 0 */
  uint64_t ret_error; /* out: retained NSError or 0 */
};

#ifdef _WIN32
/* PE-side entry point exported by d9mtmetal.dll */
#ifdef D9MTMETAL_EXPORTS
#define D9MT_API __declspec(dllexport)
#else
#define D9MT_API __declspec(dllimport)
#endif
#ifdef __cplusplus
extern "C" {
#endif
D9MT_API int __cdecl D9MT_UnixCall(unsigned int code, void *params);
#ifdef __cplusplus
}
#endif
#endif

#endif
