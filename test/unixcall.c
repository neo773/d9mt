/* Smoke test for the d9mtmetal companion unixlib: get a Metal device
 * through DXMT's winemetal, then compile MSL source at runtime through
 * d9mtmetal's newLibraryWithSource opcode.
 * d9mtmetal.dll is loaded dynamically so load failures are reportable. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../src/d9mtmetal/d9mtmetal.h"

/* minimal winemetal imports (cdecl, undecorated) */
__declspec(dllimport) uint64_t WMTCopyAllDevices(void);
__declspec(dllimport) uint64_t NSArray_object(uint64_t array, uint64_t index);
__declspec(dllimport) uint64_t NSArray_count(uint64_t array);
__declspec(dllimport) void NSObject_release(uint64_t obj);
__declspec(dllimport) uint64_t NSObject_description(uint64_t obj);
__declspec(dllimport) uint32_t NSString_getCString(uint64_t str, char *buffer,
                                                   uint64_t maxLength,
                                                   uint64_t encoding);
__declspec(dllimport) uint64_t MTLLibrary_newFunction(uint64_t library,
                                                      const char *name);

static const char MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "vertex VOut v_main(uint vid [[vertex_id]],\n"
    "                   constant float4 *verts [[buffer(0)]]) {\n"
    "  VOut o; o.pos = verts[vid]; return o;\n"
    "}\n"
    "fragment float4 f_main() { return float4(1, 0, 1, 1); }\n";

/* second stage: [[stage_in]] shader pair for the vertex-descriptor PSO */
static const char MSL_STAGEIN[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float4 pos [[attribute(0)]]; float4 col [[attribute(1)]];"
    " };\n"
    "struct VOut { float4 pos [[position]]; float4 col; };\n"
    "vertex VOut v_main(VIn in [[stage_in]]) {\n"
    "  VOut o; o.pos = in.pos; o.col = in.col; return o;\n"
    "}\n"
    "fragment float4 f_main(VOut in [[stage_in]]) { return in.col; }\n";

typedef int(__cdecl *PFN_D9MT_UnixCall)(unsigned int code, void *params);

int main(void) {
  /* CX winewrapper swallows console output; write results to a file */
  FILE *out = fopen("unixcall_out.txt", "w");
  if (!out)
    return 1;
#define LOG(...) do { fprintf(out, __VA_ARGS__); fputc('\n', out); fflush(out); } while (0)

  LOG("loading d9mtmetal.dll");
  HMODULE mod = LoadLibraryA("d9mtmetal.dll");
  if (!mod) {
    LOG("FAIL: LoadLibrary error %lu", GetLastError());
    return 1;
  }
  PFN_D9MT_UnixCall call =
      (PFN_D9MT_UnixCall)GetProcAddress(mod, "D9MT_UnixCall");
  if (!call) {
    LOG("FAIL: D9MT_UnixCall not exported");
    return 1;
  }
  LOG("d9mtmetal.dll loaded, D9MT_UnixCall at %p", (void *)call);

  uint64_t devices = WMTCopyAllDevices();
  if (!devices || !NSArray_count(devices)) {
    LOG("FAIL: no Metal devices");
    return 1;
  }
  uint64_t device = NSArray_object(devices, 0);
  LOG("device handle: %llx", (unsigned long long)device);

  struct d9mt_newlibrary_params p;
  memset(&p, 0, sizeof(p));
  p.device = device;
  p.source_ptr = (uint32_t)(uintptr_t)MSL;
  p.source_len = sizeof(MSL) - 1;
  p.fast_math = 1;

  int st = call(D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE, &p);
  LOG("D9MT_UnixCall status: %d", st);
  LOG("library: %llx error: %llx", (unsigned long long)p.ret_library,
      (unsigned long long)p.ret_error);

  if (p.ret_error) {
    char buf[512] = {0};
    uint64_t desc = NSObject_description(p.ret_error);
    if (desc) {
      NSString_getCString(desc, buf, sizeof(buf), 4 /* UTF8 */);
      NSObject_release(desc);
    }
    LOG("compile error: %s", buf);
  }

  if (st != 0 || !p.ret_library) {
    LOG("FAIL");
    return 1;
  }
  LOG("PASS: runtime MSL compilation through d9mtmetal works");

  /* stage 2: vertex-descriptor PSO through d9mtmetal */
  struct d9mt_newlibrary_params p2;
  memset(&p2, 0, sizeof(p2));
  p2.device = device;
  p2.source_ptr = (uint32_t)(uintptr_t)MSL_STAGEIN;
  p2.source_len = sizeof(MSL_STAGEIN) - 1;
  p2.fast_math = 1;
  st = call(D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE, &p2);
  if (st != 0 || !p2.ret_library) {
    LOG("FAIL: stage_in library compile status %d", st);
    return 1;
  }
  uint64_t vf = MTLLibrary_newFunction(p2.ret_library, "v_main");
  uint64_t ff = MTLLibrary_newFunction(p2.ret_library, "f_main");
  LOG("stage_in functions: vf=%llx ff=%llx", (unsigned long long)vf,
      (unsigned long long)ff);
  if (!vf || !ff) {
    LOG("FAIL: newFunction");
    return 1;
  }

  struct d9mt_pso_info info;
  memset(&info, 0, sizeof(info));
  info.vertex_function = vf;
  info.fragment_function = ff;
  info.colors[0].pixel_format = 80; /* BGRA8Unorm */
  info.colors[0].write_mask = 0xF;
  info.raster_sample_count = 1;
  info.num_attributes = 2;
  info.attributes[0].format = 31; /* float4 */
  info.attributes[0].offset = 0;
  info.attributes[0].buffer_index = 16;
  info.attributes[0].location = 0;
  info.attributes[1].format = 31;
  info.attributes[1].offset = 16;
  info.attributes[1].buffer_index = 16;
  info.attributes[1].location = 1;
  info.num_layouts = 1;
  info.layouts[0].buffer_index = 16;
  info.layouts[0].stride = 32;
  info.layouts[0].step_function = 1; /* per-vertex */
  info.layouts[0].step_rate = 1;

  struct d9mt_newpso_params pp;
  memset(&pp, 0, sizeof(pp));
  pp.device = device;
  pp.info_ptr = (uint32_t)(uintptr_t)&info;
  LOG("creating vertex-descriptor PSO...");
  st = call(D9MT_FUNC_NEW_RENDER_PSO, &pp);
  LOG("PSO status: %d pso: %llx error: %llx", st,
      (unsigned long long)pp.ret_pso, (unsigned long long)pp.ret_error);
  if (pp.ret_error) {
    char buf[512] = {0};
    uint64_t desc = NSObject_description(pp.ret_error);
    if (desc) {
      NSString_getCString(desc, buf, sizeof(buf), 4 /* UTF8 */);
      NSObject_release(desc);
    }
    LOG("PSO error: %s", buf);
  }
  if (st == 0 && pp.ret_pso) {
    LOG("PASS: vertex-descriptor PSO through d9mtmetal works");
    return 0;
  }
  LOG("FAIL");
  return 1;
}
