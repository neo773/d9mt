/* d9mtmetal.so: unix side. Plain Metal calls, no wine APIs needed.
 * Exports __wine_unix_call_funcs / __wine_unix_call_wow64_funcs which the
 * wine loader resolves when the matching builtin PE loads.
 * Param structs are identical for 32/64-bit callers (all-u64 pointers),
 * so both tables point at the same implementations.
 */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "d9mtmetal.h"

typedef int NTSTATUS; /* wine unixlib_entry_t contract */
#define STATUS_SUCCESS 0

static NTSTATUS d9mt_new_library_from_source(void *args) {
  struct d9mt_newlibrary_params *p = args;
  p->ret_library = 0;
  p->ret_error = 0;

  id<MTLDevice> device = (id<MTLDevice>)(uintptr_t)p->device;
  NSString *src =
      [[NSString alloc] initWithBytes:(const void *)(uintptr_t)p->source_ptr
                               length:(NSUInteger)p->source_len
                             encoding:NSUTF8StringEncoding];
  if (!src)
    return STATUS_SUCCESS;

  MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
  opts.languageVersion = MTLLanguageVersion3_0;
  opts.fastMathEnabled = p->fast_math != 0;

  NSError *err = nil;
  id<MTLLibrary> lib = [device newLibraryWithSource:src
                                            options:opts
                                              error:&err];
  [opts release];
  [src release];

  p->ret_library = (uint64_t)(uintptr_t)lib; /* newLibrary* returns +1 */
  p->ret_error = (uint64_t)(uintptr_t)[err retain];
  return STATUS_SUCCESS;
}

static NTSTATUS d9mt_new_render_pso(void *args) {
  struct d9mt_newpso_params *p = args;
  p->ret_pso = 0;
  p->ret_error = 0;

  id<MTLDevice> device = (id<MTLDevice>)(uintptr_t)p->device;
  const struct d9mt_pso_info *info =
      (const struct d9mt_pso_info *)(uintptr_t)p->info_ptr;

  // Metal validation reports descriptor errors as NSExceptions, and
  // [MTLVertexDescriptor vertexDescriptor] is autoreleased: pool + catch
  // or a bad descriptor kills the whole process
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  @try {

  // NOTE: vertex/fragment functions MUST be fully specialized (all their
  // function constants supplied at newFunction time). Building a PSO from
  // an unspecialized function does not return an NSError - it crashes the
  // process inside newRenderPipelineStateWithDescriptor.
  MTLRenderPipelineDescriptor *desc =
      [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = (id<MTLFunction>)(uintptr_t)info->vertex_function;
  desc.fragmentFunction = (id<MTLFunction>)(uintptr_t)info->fragment_function;

  for (unsigned i = 0; i < 8; i++) {
    const struct d9mt_color_attachment *c = &info->colors[i];
    MTLRenderPipelineColorAttachmentDescriptor *att = desc.colorAttachments[i];
    att.pixelFormat = (MTLPixelFormat)c->pixel_format;
    att.blendingEnabled = c->blending_enabled != 0;
    att.rgbBlendOperation = (MTLBlendOperation)c->rgb_blend_op;
    att.alphaBlendOperation = (MTLBlendOperation)c->alpha_blend_op;
    att.sourceRGBBlendFactor = (MTLBlendFactor)c->src_rgb_blend_factor;
    att.destinationRGBBlendFactor = (MTLBlendFactor)c->dst_rgb_blend_factor;
    att.sourceAlphaBlendFactor = (MTLBlendFactor)c->src_alpha_blend_factor;
    att.destinationAlphaBlendFactor =
        (MTLBlendFactor)c->dst_alpha_blend_factor;
    att.writeMask = (MTLColorWriteMask)c->write_mask;
  }

  desc.depthAttachmentPixelFormat = (MTLPixelFormat)info->depth_pixel_format;
  desc.stencilAttachmentPixelFormat =
      (MTLPixelFormat)info->stencil_pixel_format;
  desc.rasterSampleCount =
      info->raster_sample_count ? info->raster_sample_count : 1;
  desc.alphaToCoverageEnabled = info->alpha_to_coverage != 0;

  if (info->num_attributes) {
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (uint32_t i = 0; i < info->num_attributes && i < 18; i++) {
      const struct d9mt_vertex_attribute *a = &info->attributes[i];
      MTLVertexAttributeDescriptor *ad = vd.attributes[a->location];
      ad.format = (MTLVertexFormat)a->format;
      ad.offset = a->offset;
      ad.bufferIndex = a->buffer_index;
    }
    for (uint32_t i = 0; i < info->num_layouts && i < 16; i++) {
      const struct d9mt_vertex_layout *l = &info->layouts[i];
      MTLVertexBufferLayoutDescriptor *ld = vd.layouts[l->buffer_index];
      ld.stride = l->stride;
      ld.stepFunction = (MTLVertexStepFunction)l->step_function;
      ld.stepRate = l->step_rate ? l->step_rate : 1;
    }
    desc.vertexDescriptor = vd;
  }

  NSError *err = nil;
  id<MTLRenderPipelineState> pso =
      [device newRenderPipelineStateWithDescriptor:desc error:&err];
  [desc release];

  p->ret_pso = (uint64_t)(uintptr_t)pso;
  p->ret_error = (uint64_t)(uintptr_t)[err retain];

  } @catch (NSException *ex) {
    NSString *msg = [NSString
        stringWithFormat:@"d9mtmetal PSO exception: %@: %@", ex.name,
                         ex.reason];
    NSError *exErr = [[NSError alloc]
        initWithDomain:@"d9mtmetal"
                  code:1
              userInfo:@{NSLocalizedDescriptionKey : msg}];
    p->ret_pso = 0;
    p->ret_error = (uint64_t)(uintptr_t)exErr;
  }
  [pool release];
  return STATUS_SUCCESS;
}

typedef NTSTATUS (*unixlib_entry_t)(void *args);

__attribute__((visibility("default")))
const unixlib_entry_t __wine_unix_call_funcs[D9MT_FUNC_COUNT] = {
    d9mt_new_library_from_source,
    d9mt_new_render_pso,
};

/* identical param layouts for 32-bit callers (see header ABI rule) */
__attribute__((visibility("default")))
const unixlib_entry_t __wine_unix_call_wow64_funcs[D9MT_FUNC_COUNT] = {
    d9mt_new_library_from_source,
    d9mt_new_render_pso,
};
