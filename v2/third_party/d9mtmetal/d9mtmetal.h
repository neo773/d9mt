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
  /* key -> MTLLibrary, with the disk metallib cache + compile backends and
   * all fallback resolved entirely native-side. The PE passes a content key
   * and the source bytes; the source is only read on a cache MISS. The
   * .metallib bytes never cross this ABI — only a handle comes back up. */
  D9MT_FUNC_LIBRARY_FOR_KEY = 2,
  /* macOS 26+ Metal HUD: set a custom label's text color. On macOS 27 the HUD
   * renders custom labels in black by default (invisible on the dark overlay);
   * winemetal exposes addLabel/updateLabel but NOT the color setter, so we add
   * it here. hud + key are raw ObjC handles obtained via winemetal (same
   * process, interoperable per the ABI note above). */
  D9MT_FUNC_HUD_SET_COLOR = 3,
  /* Programmatic Metal frame capture to a .gputrace file (Xcode frame debugger).
   * Wine can't be attached by Xcode live, so we drive MTLCaptureManager
   * ourselves. Requires MTL_CAPTURE_ENABLED=1 at launch. action: 1=begin, 0=end.
   * begin captures all command buffers on `queue` until end is called. */
  D9MT_FUNC_CAPTURE = 4,
  D9MT_FUNC_COUNT,
};

struct d9mt_capture_params {
  uint64_t queue;     /* in: MTLCommandQueue handle (begin only)           */
  uint64_t path_ptr;  /* in: UTF-8 output .gputrace path (begin only)      */
  uint64_t path_len;  /* in */
  uint32_t action;    /* in: 1=begin, 0=end                                */
  uint32_t ret_ok;    /* out: 1 if the action succeeded                    */
};

struct d9mt_hud_color_params {
  uint64_t hud;          /* in: _CADeveloperHUDProperties instance handle */
  uint64_t key;          /* in: NSString label key handle                 */
  uint32_t name_color;   /* in: 0xAARRGGBB (or 0xFFFFFFFF for white)      */
  uint32_t value_color;  /* in */
};

/* source_kind values (InputKind). Today every backend is fed MSL_TEXT;
 * SPIRV/DXBC are reserved for future earlier-input backends (MSC/airconv). */
enum d9mt_source_kind {
  D9MT_SOURCE_MSL_TEXT = 0,
  D9MT_SOURCE_SPIRV = 1,
  D9MT_SOURCE_DXBC = 2,
};

/* target_flags bits: codegen/compile options that affect the artifact bytes.
 * (msl language version is implicit 3.0 today; folded into the key PE-side.) */
enum d9mt_target_flags {
  D9MT_TARGET_FAST_MATH = 1u << 0,
};

/* ret_status values (observable, never branched on for control flow). */
enum d9mt_library_status {
  D9MT_LIBRARY_HIT = 0,       /* served from the disk cache               */
  D9MT_LIBRARY_COMPILED = 1,  /* compiled by a backend, then cached       */
  D9MT_LIBRARY_FELL_BACK = 2, /* degraded to live newLibraryWithSource    */
  D9MT_LIBRARY_FAILED = 3,    /* no library produced                      */
};

struct d9mt_library_params {
  uint64_t device;       /* in:  obj_handle_t MTLDevice                       */
  uint64_t key_ptr;      /* in:  const void* ShaderKey digest bytes           */
  uint64_t key_len;      /* in */
  uint64_t source_ptr;   /* in:  source blob (only read on a cache MISS)      */
  uint64_t source_len;   /* in */
  uint32_t source_kind;  /* in:  enum d9mt_source_kind                        */
  uint32_t target_flags; /* in:  enum d9mt_target_flags bitmask              */
  uint64_t ret_library;  /* out: retained MTLLibrary or 0                     */
  uint64_t ret_status;   /* out: enum d9mt_library_status                     */
  uint64_t ret_error;    /* out: retained NSError or 0 (caller releases)      */
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
