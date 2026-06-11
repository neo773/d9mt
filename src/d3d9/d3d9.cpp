// d9mt: minimal native D3D9-on-Metal driver (MVP: colored triangle).
//
//   app (32-bit PE) -> this d3d9.dll (mingw PE) -> winemetal.dll (DXMT's
//   wine-builtin bridge) -> __wine_unix_call -> winemetal.so -> Metal
//
// MVP scope: DrawPrimitiveUP with D3DFVF_XYZRHW|D3DFVF_DIFFUSE triangles,
// windowed BGRA8 swapchain, no depth, no shaders, no textures.

#define INITGUID
#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <deque>
#include <string>
#include <vector>

#include "../winemetal.h"
#include "../d9mtmetal/d9mtmetal.h"
#include "d9mt_translate.h"
#include "shader_metallib.h"

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------

static FILE *g_log = nullptr;

static void log_msg(const char *fmt, ...) {
  if (!g_log) {
    g_log = fopen("d9mt.log", "w");
    if (!g_log)
      return;
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}

static void log_nserror(const char *what, obj_handle_t err) {
  if (!err) {
    log_msg("%s: failed with no NSError", what);
    return;
  }
  obj_handle_t desc = NSObject_description(err);
  char buf[512] = {0};
  if (desc) {
    NSString_getCString(desc, buf, sizeof(buf), WMTUTF8StringEncoding);
    NSObject_release(desc);
  }
  log_msg("%s: %s", what, buf);
  NSObject_release(err);
}

#define STUB_ONCE(name)                                                        \
  do {                                                                         \
    static bool warned_ = false;                                               \
    if (!warned_) {                                                            \
      warned_ = true;                                                          \
      log_msg("stub: %s", name);                                               \
    }                                                                          \
  } while (0)

// ---------------------------------------------------------------------------
// shader path plumbing
// ---------------------------------------------------------------------------

// Metal buffer index plan (vertex stage): the generated shaders own the low
// indices ([[buffer(0)]] = set-0 argument buffer, [[buffer(1)]] =
// render_state, [[buffer(2)]] = sampler heap), so vertex streams bind high.
static constexpr uint32_t STREAM_BUFFER_BASE = 16;
static constexpr UINT MAX_STREAMS = 8;
static constexpr UINT RING_SIZE = 8u << 20; // per-frame upload ring
static constexpr UINT MAX_SAMPLERS = 16;    // PS samplers s0-s15
static constexpr UINT MAX_SAMPLER_STATES = 14; // D3DSAMP_* max + 1
static constexpr UINT SAMPLER_HEAP_SLOTS = 64; // distinct sampler states
static constexpr UINT MAX_RENDER_STATES = 256; // D3DRS_* fit comfortably

// matches the render_state_t push block spirv-cross emits for the DXVK
// dxso shaders (see build/test_*.metal); sampler words pack two u16 heap
// indices per dword
struct D9MTRenderStateBlock {
  float fog_color[3];
  float fog_scale;
  float fog_end;
  float fog_density;
  uint32_t alpha_ref;
  float point_size;
  float point_size_min;
  float point_size_max;
  float point_scale_a;
  float point_scale_b;
  float point_scale_c;
  uint8_t pad[12];
  uint32_t sampler_idx[8];
};
static_assert(sizeof(D9MTRenderStateBlock) == 96, "render_state layout");

// D3DBLEND -> MTLBlendFactor raw value
static uint32_t d3dblend_to_mtl(DWORD b) {
  switch (b) {
  case D3DBLEND_ZERO:           return 0;  // Zero
  case D3DBLEND_ONE:            return 1;  // One
  case D3DBLEND_SRCCOLOR:       return 2;  // SourceColor
  case D3DBLEND_INVSRCCOLOR:    return 3;
  case D3DBLEND_SRCALPHA:       return 4;  // SourceAlpha
  case D3DBLEND_INVSRCALPHA:    return 5;
  case D3DBLEND_DESTCOLOR:      return 6;  // DestinationColor
  case D3DBLEND_INVDESTCOLOR:   return 7;
  case D3DBLEND_DESTALPHA:      return 8;  // DestinationAlpha
  case D3DBLEND_INVDESTALPHA:   return 9;
  case D3DBLEND_SRCALPHASAT:    return 10; // SourceAlphaSaturated
  case D3DBLEND_BLENDFACTOR:    return 11; // BlendColor
  case D3DBLEND_INVBLENDFACTOR: return 12;
  default:                      return 1;
  }
}

// D3DBLENDOP -> MTLBlendOperation (Add/Sub/RevSub/Min/Max, off by one)
static uint32_t d3dblendop_to_mtl(DWORD op) {
  return (op >= D3DBLENDOP_ADD && op <= D3DBLENDOP_MAX) ? op - 1 : 0;
}

// D3DCMPFUNC -> WMTCompareFunction (same order, off by one)
static WMTCompareFunction d3dcmp_to_wmt(DWORD f) {
  return (f >= D3DCMP_NEVER && f <= D3DCMP_ALWAYS)
             ? (WMTCompareFunction)(f - 1)
             : WMTCompareFunctionAlways;
}

// D3DCOLORWRITEENABLE (R=1,G=2,B=4,A=8) -> MTLColorWriteMask (A=1,B=2,G=4,R=8)
static uint32_t d3dwritemask_to_mtl(DWORD m) {
  return ((m & D3DCOLORWRITEENABLE_RED) ? 8u : 0) |
         ((m & D3DCOLORWRITEENABLE_GREEN) ? 4u : 0) |
         ((m & D3DCOLORWRITEENABLE_BLUE) ? 2u : 0) |
         ((m & D3DCOLORWRITEENABLE_ALPHA) ? 1u : 0);
}

// D3DDECLTYPE -> MTLVertexFormat raw value (0 = unsupported)
static uint32_t decltype_to_mtl(BYTE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:    return 28; // MTLVertexFormatFloat
  case D3DDECLTYPE_FLOAT2:    return 29;
  case D3DDECLTYPE_FLOAT3:    return 30;
  case D3DDECLTYPE_FLOAT4:    return 31;
  case D3DDECLTYPE_D3DCOLOR:  return 42; // UChar4Normalized_BGRA
  case D3DDECLTYPE_UBYTE4:    return 3;  // UChar4
  case D3DDECLTYPE_SHORT2:    return 16; // Short2
  case D3DDECLTYPE_SHORT4:    return 18;
  case D3DDECLTYPE_UBYTE4N:   return 9;  // UChar4Normalized
  case D3DDECLTYPE_SHORT2N:   return 22;
  case D3DDECLTYPE_SHORT4N:   return 24;
  case D3DDECLTYPE_USHORT2N:  return 19;
  case D3DDECLTYPE_USHORT4N:  return 21;
  case D3DDECLTYPE_FLOAT16_2: return 25; // Half2
  case D3DDECLTYPE_FLOAT16_4: return 27;
  default:                    return 0;
  }
}

// D3DFORMAT -> WMTPixelFormat plus the layout info LockRect needs.
// blockDim 1 = plain linear formats; 4 = BC (DXT) 4x4 blocks.
// Formats whose D3D9 sample semantics differ from the raw Metal format
// (X* alpha-is-one, luminance replication, A4R4G4B4 component order, ...)
// get a swizzled texture view; the channel mappings mirror DXVK's
// ConvertFormatUnfixed (d3d9_format.cpp) VkComponentMapping table.
struct D9MTFormatInfo {
  uint32_t wmt;
  UINT bytesPerBlock;
  UINT blockDim;
  bool swizzled;
  WMTTextureSwizzleChannels swizzle;
};

static bool d3dfmt_to_wmt(D3DFORMAT fmt, D9MTFormatInfo *out) {
  constexpr WMTTextureSwizzle R = WMTTextureSwizzleRed;
  constexpr WMTTextureSwizzle G = WMTTextureSwizzleGreen;
  constexpr WMTTextureSwizzle B = WMTTextureSwizzleBlue;
  constexpr WMTTextureSwizzle A = WMTTextureSwizzleAlpha;
  constexpr WMTTextureSwizzle ONE = WMTTextureSwizzleOne;
  auto plain = [out](uint32_t w, UINT bpb, UINT bd = 1) {
    *out = {w, bpb, bd, false, {}};
    return true;
  };
  auto swiz = [out](uint32_t w, UINT bpb, WMTTextureSwizzle r,
                    WMTTextureSwizzle g, WMTTextureSwizzle b,
                    WMTTextureSwizzle a) {
    *out = {w, bpb, 1, true, {r, g, b, a}};
    return true;
  };
  switch ((DWORD)fmt) {
  case D3DFMT_A8R8G8B8:      return plain(WMTPixelFormatBGRA8Unorm, 4);
  case D3DFMT_X8R8G8B8:      return swiz(WMTPixelFormatBGRA8Unorm, 4, R, G, B, ONE);
  case D3DFMT_A8B8G8R8:      return plain(WMTPixelFormatRGBA8Unorm, 4);
  case D3DFMT_X8B8G8R8:      return swiz(WMTPixelFormatRGBA8Unorm, 4, R, G, B, ONE);
  case D3DFMT_R5G6B5:        return plain(WMTPixelFormatB5G6R5Unorm, 2);
  case D3DFMT_A1R5G5B5:      return plain(WMTPixelFormatBGR5A1Unorm, 2);
  case D3DFMT_X1R5G5B5:      return swiz(WMTPixelFormatBGR5A1Unorm, 2, R, G, B, ONE);
  // D3D A4R4G4B4 in ABGR4 bit order: components land at (G,B,A,R), the
  // same trick as winemetal's synthetic WMTPixelFormatBGRA4Unorm
  case D3DFMT_A4R4G4B4:      return swiz(WMTPixelFormatABGR4Unorm, 2, G, B, A, R);
  case D3DFMT_X4R4G4B4:      return swiz(WMTPixelFormatABGR4Unorm, 2, G, B, A, ONE);
  case D3DFMT_A8:            return plain(WMTPixelFormatA8Unorm, 1);
  case D3DFMT_L8:            return swiz(WMTPixelFormatR8Unorm, 1, R, R, R, ONE);
  case D3DFMT_A8L8:          return swiz(WMTPixelFormatRG8Unorm, 2, R, R, R, G);
  case D3DFMT_L16:           return swiz(WMTPixelFormatR16Unorm, 2, R, R, R, ONE);
  case D3DFMT_V8U8:          return swiz(WMTPixelFormatRG8Snorm, 2, R, G, ONE, ONE);
  case D3DFMT_Q8W8V8U8:      return plain(WMTPixelFormatRGBA8Snorm, 4);
  case D3DFMT_V16U16:        return swiz(WMTPixelFormatRG16Snorm, 4, R, G, ONE, ONE);
  case D3DFMT_G16R16:        return swiz(WMTPixelFormatRG16Unorm, 4, R, G, ONE, ONE);
  case D3DFMT_A16B16G16R16:  return plain(WMTPixelFormatRGBA16Unorm, 8);
  case D3DFMT_A2B10G10R10:   return plain(WMTPixelFormatRGB10A2Unorm, 4);
  case D3DFMT_A2R10G10B10:   return plain(WMTPixelFormatBGR10A2Unorm, 4);
  case D3DFMT_R16F:          return swiz(WMTPixelFormatR16Float, 2, R, ONE, ONE, ONE);
  case D3DFMT_G16R16F:       return swiz(WMTPixelFormatRG16Float, 4, R, G, ONE, ONE);
  case D3DFMT_A16B16G16R16F: return plain(WMTPixelFormatRGBA16Float, 8);
  case D3DFMT_R32F:          return swiz(WMTPixelFormatR32Float, 4, R, ONE, ONE, ONE);
  case D3DFMT_G32R32F:       return swiz(WMTPixelFormatRG32Float, 8, R, G, ONE, ONE);
  case D3DFMT_A32B32G32R32F: return plain(WMTPixelFormatRGBA32Float, 16);
  case D3DFMT_DXT1:          return plain(WMTPixelFormatBC1_RGBA, 8, 4);
  case D3DFMT_DXT2: // premultiplied variants share the block format
  case D3DFMT_DXT3:          return plain(WMTPixelFormatBC2_RGBA, 16, 4);
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:          return plain(WMTPixelFormatBC3_RGBA, 16, 4);
  // depth formats (Apple silicon has no D24: promote to D32F variants)
  case D3DFMT_D16:           return plain(WMTPixelFormatDepth32Float, 4);
  case D3DFMT_D32:
  case D3DFMT_D32F_LOCKABLE: return plain(WMTPixelFormatDepth32Float, 4);
  case D3DFMT_D24S8:
  case D3DFMT_D24X8:
  case D3DFMT_D24FS8:
    return plain(WMTPixelFormatDepth32Float_Stencil8, 5);
  default:                   return false;
  }
}

static bool d3dfmt_is_depth(D3DFORMAT fmt) {
  switch ((DWORD)fmt) {
  case D3DFMT_D16:
  case D3DFMT_D32:
  case D3DFMT_D32F_LOCKABLE:
  case D3DFMT_D24S8:
  case D3DFMT_D24X8:
  case D3DFMT_D24FS8:
    return true;
  default:
    return false;
  }
}

class D9MTDevice;

class D9MTVertexDeclaration final : public IDirect3DVertexDeclaration9 {
public:
  D9MTVertexDeclaration(IDirect3DDevice9 *dev,
                        const D3DVERTEXELEMENT9 *elems)
      : m_device(dev) {
    for (const D3DVERTEXELEMENT9 *e = elems; e->Stream != 0xFF; e++)
      m_elements.push_back(*e);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DVertexDeclaration9) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_refcount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&m_refcount);
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override {
    if (!ppDevice)
      return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 *pElement,
                                           UINT *pNumElements) override {
    if (!pNumElements)
      return D3DERR_INVALIDCALL;
    UINT n = (UINT)m_elements.size() + 1;
    if (pElement) {
      memcpy(pElement, m_elements.data(),
             m_elements.size() * sizeof(D3DVERTEXELEMENT9));
      static const D3DVERTEXELEMENT9 end = D3DDECL_END();
      pElement[m_elements.size()] = end;
    }
    *pNumElements = n;
    return D3D_OK;
  }

  std::vector<D3DVERTEXELEMENT9> m_elements;

private:
  volatile LONG m_refcount = 1;
  IDirect3DDevice9 *m_device;
};

// shared MTLBuffer-backed memory for VB/IB (host-visible, page-aligned for
// newBufferWithBytesNoCopy)
struct D9MTBufferStorage {
  void *mem = nullptr;
  obj_handle_t buf = 0;
  uint64_t gpuAddr = 0;
  UINT size = 0;

  bool alloc(obj_handle_t device, UINT length) {
    size = (length + 0xFFFF) & ~0xFFFFu; // VirtualAlloc granularity
    mem = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                       PAGE_READWRITE);
    if (!mem)
      return false;
    WMTBufferInfo info = {};
    info.length = size;
    info.options = WMTResourceStorageModeShared;
    info.memory.set(mem);
    buf = MTLDevice_newBuffer(device, &info);
    gpuAddr = info.gpu_address;
    return buf != 0;
  }
  void free() {
    if (buf)
      NSObject_release(buf);
    if (mem)
      VirtualFree(mem, 0, MEM_RELEASE);
    buf = 0;
    mem = nullptr;
  }
};

template <typename Iface, D3DRESOURCETYPE RType>
class D9MTBufferBase : public Iface {
public:
  D9MTBufferBase(IDirect3DDevice9 *dev) : m_device(dev) {}
  virtual ~D9MTBufferBase() { m_storage.free(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = this;
    this->AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_refcount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&m_refcount);
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override {
    if (!ppDevice)
      return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD,
                                           DWORD) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *,
                                           DWORD *) override {
    return D3DERR_NOTFOUND;
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override {
    return D3D_OK;
  }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return RType; }

  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock,
                                 void **ppbData, DWORD Flags) override {
    if (!ppbData || OffsetToLock >= m_length)
      return D3DERR_INVALIDCALL;
    *ppbData = (uint8_t *)m_storage.mem + OffsetToLock;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE Unlock() override { return D3D_OK; }

  D9MTBufferStorage m_storage;
  UINT m_length = 0;

protected:
  volatile LONG m_refcount = 1;
  IDirect3DDevice9 *m_device;
};

class D9MTVertexBuffer final
    : public D9MTBufferBase<IDirect3DVertexBuffer9, D3DRTYPE_VERTEXBUFFER> {
public:
  using D9MTBufferBase::D9MTBufferBase;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC *pDesc) override {
    if (!pDesc)
      return D3DERR_INVALIDCALL;
    pDesc->Format = D3DFMT_VERTEXDATA;
    pDesc->Type = D3DRTYPE_VERTEXBUFFER;
    pDesc->Usage = m_usage;
    pDesc->Pool = D3DPOOL_DEFAULT;
    pDesc->Size = m_length;
    pDesc->FVF = m_fvf;
    return D3D_OK;
  }
  DWORD m_usage = 0;
  DWORD m_fvf = 0;
};

class D9MTIndexBuffer final
    : public D9MTBufferBase<IDirect3DIndexBuffer9, D3DRTYPE_INDEXBUFFER> {
public:
  using D9MTBufferBase::D9MTBufferBase;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC *pDesc) override {
    if (!pDesc)
      return D3DERR_INVALIDCALL;
    pDesc->Format = m_format;
    pDesc->Type = D3DRTYPE_INDEXBUFFER;
    pDesc->Usage = m_usage;
    pDesc->Pool = D3DPOOL_DEFAULT;
    pDesc->Size = m_length;
    return D3D_OK;
  }
  DWORD m_usage = 0;
  D3DFORMAT m_format = D3DFMT_INDEX16;
};

// translated + Metal-compiled shader (shared shape between VS and PS)
struct D9MTShaderData {
  D9MTShaderInfo info;
  obj_handle_t library = 0;  // retained, from d9mtmetal newLibraryWithSource
  obj_handle_t function = 0; // retained MTLFunction "main0"
  std::vector<DWORD> bytecode;

  void destroy() {
    if (function)
      NSObject_release(function);
    if (library)
      NSObject_release(library);
    function = 0;
    library = 0;
  }
};

template <typename Iface>
class D9MTShaderBase : public Iface {
public:
  D9MTShaderBase(IDirect3DDevice9 *dev) : m_device(dev) {}
  virtual ~D9MTShaderBase() { m_data.destroy(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = this;
    this->AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_refcount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&m_refcount);
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override {
    if (!ppDevice)
      return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFunction(void *pData,
                                        UINT *pSizeOfData) override {
    if (!pSizeOfData)
      return D3DERR_INVALIDCALL;
    UINT size = (UINT)(m_data.bytecode.size() * sizeof(DWORD));
    if (pData)
      memcpy(pData, m_data.bytecode.data(), size);
    *pSizeOfData = size;
    return D3D_OK;
  }

  D9MTShaderData m_data;

private:
  volatile LONG m_refcount = 1;
  IDirect3DDevice9 *m_device;
};

using D9MTVertexShader = D9MTShaderBase<IDirect3DVertexShader9>;
using D9MTPixelShader = D9MTShaderBase<IDirect3DPixelShader9>;

// ---------------------------------------------------------------------------
// textures
// ---------------------------------------------------------------------------

class D9MTSurface;

// 2D texture: shared-storage MTLTexture with the full mip chain, plus a
// sysmem staging copy. LockRect hands out staging memory; UnlockRect
// uploads the level with replaceRegion.
class D9MTTexture final : public IDirect3DTexture9 {
public:
  D9MTTexture(IDirect3DDevice9 *dev, UINT width, UINT height, UINT levels,
              D3DFORMAT format, const D9MTFormatInfo &fi)
      : m_device(dev), m_width(width), m_height(height), m_format(format),
        m_fmtInfo(fi) {
    if (!levels) { // 0 = full chain down to 1x1
      UINT dim = width > height ? width : height;
      for (levels = 1; dim > 1; dim >>= 1)
        levels++;
    }
    m_levels = levels;
    UINT off = 0;
    for (UINT l = 0; l < m_levels; l++) {
      m_levelOffset.push_back(off);
      off += RowBytes(l) * Rows(l);
    }
    m_staging.resize(off);
    m_surfaces.resize(m_levels, nullptr);
  }
  ~D9MTTexture();

  bool Create(obj_handle_t mtlDevice, DWORD d3dUsage = 0) {
    WMTTextureInfo info = {};
    info.pixel_format = (WMTPixelFormat)m_fmtInfo.wmt;
    info.width = m_width;
    info.height = m_height;
    info.depth = 1;
    info.array_length = 1;
    info.type = WMTTextureType2D;
    info.mipmap_level_count = m_levels;
    info.sample_count = 1;
    info.usage = WMTTextureUsageShaderRead;
    if (m_fmtInfo.swizzled)
      info.usage = (WMTTextureUsage)(info.usage | WMTTextureUsagePixelFormatView);
    info.options = WMTResourceStorageModeShared;
    if (d3dUsage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
      info.usage = (WMTTextureUsage)(info.usage | WMTTextureUsageRenderTarget);
    if (d3dfmt_is_depth(m_format)) {
      // depth textures can't be shared-storage; no CPU staging uploads
      info.options = WMTResourceStorageModePrivate;
      m_noUpload = true;
    }
    m_tex = MTLDevice_newTexture(mtlDevice, &info);
    if (!m_tex)
      return false;
    m_gpuId = info.gpu_resource_id;
    if (m_fmtInfo.swizzled) {
      // shaders sample through a view carrying the D3D9 channel mapping;
      // uploads still go to the plain base texture
      m_view = MTLTexture_newTextureView(
          m_tex, (WMTPixelFormat)m_fmtInfo.wmt, WMTTextureType2D, 0, m_levels,
          0, 1, m_fmtInfo.swizzle, &m_gpuId);
      if (!m_view)
        return false;
    }
    return true;
  }

  // what PrepareDraw binds: the swizzled view when there is one
  obj_handle_t SampleHandle() const { return m_view ? m_view : m_tex; }

  UINT LevelWidth(UINT l) const { return m_width >> l ? m_width >> l : 1; }
  UINT LevelHeight(UINT l) const { return m_height >> l ? m_height >> l : 1; }
  UINT RowBytes(UINT l) const {
    UINT w = LevelWidth(l);
    return m_fmtInfo.blockDim == 1
               ? w * m_fmtInfo.bytesPerBlock
               : ((w + 3) / 4) * m_fmtInfo.bytesPerBlock;
  }
  UINT Rows(UINT l) const {
    UINT h = LevelHeight(l);
    return m_fmtInfo.blockDim == 1 ? h : (h + 3) / 4;
  }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_refcount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&m_refcount);
    if (!r)
      delete this;
    return r;
  }

  // IDirect3DResource9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override {
    if (!ppDevice)
      return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD,
                                           DWORD) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) override {
    return D3DERR_NOTFOUND;
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return D3D_OK; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override {
    return D3DRTYPE_TEXTURE;
  }

  // IDirect3DBaseTexture9
  DWORD STDMETHODCALLTYPE SetLOD(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetLOD() override { return 0; }
  DWORD STDMETHODCALLTYPE GetLevelCount() override { return m_levels; }
  HRESULT STDMETHODCALLTYPE
  SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override {
    return D3D_OK;
  }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override {
    return D3DTEXF_LINEAR;
  }
  void STDMETHODCALLTYPE GenerateMipSubLevels() override {
    STUB_ONCE("GenerateMipSubLevels");
  }

  // IDirect3DTexture9
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level,
                                         D3DSURFACE_DESC *pDesc) override {
    if (!pDesc || Level >= m_levels)
      return D3DERR_INVALIDCALL;
    pDesc->Format = m_format;
    pDesc->Type = D3DRTYPE_SURFACE;
    pDesc->Usage = 0;
    pDesc->Pool = D3DPOOL_MANAGED;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    pDesc->MultiSampleQuality = 0;
    pDesc->Width = LevelWidth(Level);
    pDesc->Height = LevelHeight(Level);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(
      UINT Level, IDirect3DSurface9 **ppSurfaceLevel) override;
  HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect,
                                     const RECT *pRect,
                                     DWORD Flags) override {
    if (!pLockedRect || Level >= m_levels)
      return D3DERR_INVALIDCALL;
    uint8_t *base = m_staging.data() + m_levelOffset[Level];
    pLockedRect->Pitch = (INT)RowBytes(Level);
    if (pRect) {
      UINT bx = (pRect->left / m_fmtInfo.blockDim) * m_fmtInfo.bytesPerBlock;
      UINT by = pRect->top / m_fmtInfo.blockDim;
      base += by * RowBytes(Level) + bx;
    }
    pLockedRect->pBits = base;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) override {
    if (Level >= m_levels || !m_tex)
      return D3DERR_INVALIDCALL;
    if (m_noUpload) // private storage (depth/RT): nothing to upload
      return D3D_OK;
    WMTOrigin origin = {0, 0, 0};
    WMTSize size = {LevelWidth(Level), LevelHeight(Level), 1};
    WMTMemoryPointer data;
    data.set(m_staging.data() + m_levelOffset[Level]);
    MTLTexture_replaceRegion(m_tex, origin, size, Level, 0, data,
                             RowBytes(Level), 0);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *) override {
    return D3D_OK;
  }

  obj_handle_t m_tex = 0;
  obj_handle_t m_view = 0; // swizzled view, when the format needs one
  uint64_t m_gpuId = 0;    // of the handle shaders sample (view if present)
  bool m_noUpload = false; // private storage: UnlockRect skips replaceRegion

private:
  volatile LONG m_refcount = 1;
  IDirect3DDevice9 *m_device;
  UINT m_width, m_height, m_levels = 1;
  D3DFORMAT m_format;
  D9MTFormatInfo m_fmtInfo;
  std::vector<uint8_t> m_staging;
  std::vector<UINT> m_levelOffset;
  std::vector<D9MTSurface *> m_surfaces; // lazily created, share our refcount
};

// Mip-level view. Per D3D9 semantics it shares the container's refcount:
// AddRef/Release forward to the texture, which owns and deletes it.
class D9MTSurface final : public IDirect3DSurface9 {
public:
  D9MTSurface(D9MTTexture *parent, UINT level)
      : m_parent(parent), m_level(level) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return m_parent->AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return m_parent->Release(); }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override {
    return m_parent->GetDevice(ppDevice);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD,
                                           DWORD) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) override {
    return D3DERR_NOTFOUND;
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return D3D_OK; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override {
    return D3DRTYPE_SURFACE;
  }

  HRESULT STDMETHODCALLTYPE GetContainer(REFIID, void **ppContainer) override {
    if (!ppContainer)
      return D3DERR_INVALIDCALL;
    m_parent->AddRef();
    *ppContainer = m_parent;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) override {
    return m_parent->GetLevelDesc(m_level, pDesc);
  }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLockedRect,
                                     const RECT *pRect, DWORD Flags) override {
    return m_parent->LockRect(m_level, pLockedRect, pRect, Flags);
  }
  HRESULT STDMETHODCALLTYPE UnlockRect() override {
    return m_parent->UnlockRect(m_level);
  }
  HRESULT STDMETHODCALLTYPE GetDC(HDC *) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override {
    return D3DERR_INVALIDCALL;
  }

private:
  D9MTTexture *m_parent;
  UINT m_level;
};

D9MTTexture::~D9MTTexture() {
  for (D9MTSurface *s : m_surfaces)
    delete s;
  if (m_view)
    NSObject_release(m_view);
  if (m_tex)
    NSObject_release(m_tex);
}

HRESULT D9MTTexture::GetSurfaceLevel(UINT Level,
                                     IDirect3DSurface9 **ppSurfaceLevel) {
  if (!ppSurfaceLevel || Level >= m_levels)
    return D3DERR_INVALIDCALL;
  if (!m_surfaces[Level])
    m_surfaces[Level] = new D9MTSurface(this, Level);
  AddRef(); // surfaces share the container refcount
  *ppSurfaceLevel = m_surfaces[Level];
  return D3D_OK;
}

// Standalone surface (backbuffer, render target, depth-stencil,
// offscreen-plain). Carries a desc; rendering still goes to the one
// swapchain pass, so non-backbuffer targets are placeholders that let
// apps proceed (logged in SetRenderTarget).
class D9MTPlainSurface final : public IDirect3DSurface9 {
public:
  D9MTPlainSurface(IDirect3DDevice9 *dev, const D3DSURFACE_DESC &desc)
      : m_device(dev), m_desc(desc) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_refcount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&m_refcount);
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override {
    if (!ppDevice)
      return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD,
                                           DWORD) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) override {
    return D3DERR_NOTFOUND;
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return D3D_OK; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override {
    return D3DRTYPE_SURFACE;
  }
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID, void **ppContainer) override {
    if (!ppContainer)
      return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *ppContainer = m_device;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) override {
    if (!pDesc)
      return D3DERR_INVALIDCALL;
    *pDesc = m_desc;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *, const RECT *,
                                     DWORD) override {
    STUB_ONCE("D9MTPlainSurface::LockRect");
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE UnlockRect() override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetDC(HDC *) override {
    return D3DERR_INVALIDCALL;
  }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override {
    return D3DERR_INVALIDCALL;
  }

private:
  volatile LONG m_refcount = 1;
  IDirect3DDevice9 *m_device;
  D3DSURFACE_DESC m_desc;
};

// The implicit swapchain: thin view over the device's single swapchain
// pass. Owned by the device, one app ref per GetSwapChain.
class D9MTSwapChain final : public IDirect3DSwapChain9Ex {
public:
  D9MTSwapChain(IDirect3DDevice9Ex *dev, const D3DPRESENT_PARAMETERS &pp)
      : m_device(dev), m_pp(pp) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_refcount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&m_refcount);
    if (!r)
      delete this;
    return r;
  }

  HRESULT STDMETHODCALLTYPE Present(const RECT *src, const RECT *dst,
                                    HWND hwnd, const RGNDATA *dirty,
                                    DWORD flags) override {
    return m_device->Present(src, dst, hwnd, dirty);
  }
  HRESULT STDMETHODCALLTYPE
  GetFrontBufferData(IDirect3DSurface9 *pDestSurface) override {
    STUB_ONCE("SwapChain::GetFrontBufferData");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE
  GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                IDirect3DSurface9 **ppBackBuffer) override {
    return m_device->GetBackBuffer(0, iBackBuffer, Type, ppBackBuffer);
  }
  HRESULT STDMETHODCALLTYPE
  GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) override {
    return m_device->GetRasterStatus(0, pRasterStatus);
  }
  HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE *pMode) override {
    return m_device->GetDisplayMode(0, pMode);
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override {
    if (!ppDevice)
      return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetPresentParameters(D3DPRESENT_PARAMETERS *pp) override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    *pp = m_pp;
    return D3D_OK;
  }

  // IDirect3DSwapChain9Ex
  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pCount) override {
    if (pCount)
      *pCount = 0;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetPresentStats(D3DPRESENTSTATS *pStats) override {
    if (pStats)
      memset(pStats, 0, sizeof(*pStats));
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetDisplayModeEx(D3DDISPLAYMODEEX *pMode,
                   D3DDISPLAYROTATION *pRotation) override {
    if (pMode) {
      pMode->Size = sizeof(*pMode);
      pMode->Width = m_pp.BackBufferWidth;
      pMode->Height = m_pp.BackBufferHeight;
      pMode->RefreshRate = 60;
      pMode->Format = D3DFMT_X8R8G8B8;
      pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    }
    if (pRotation)
      *pRotation = D3DDISPLAYROTATION_IDENTITY;
    return D3D_OK;
  }

  D3DPRESENT_PARAMETERS m_pp;

private:
  volatile LONG m_refcount = 1;
  IDirect3DDevice9Ex *m_device;
};

// Queries: Present blocks on waitUntilCompleted, so by the time anything
// asks, the GPU is idle - every query completes immediately.
class D9MTQuery final : public IDirect3DQuery9 {
public:
  D9MTQuery(IDirect3DDevice9 *dev, D3DQUERYTYPE type)
      : m_device(dev), m_type(type) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_refcount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&m_refcount);
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override {
    if (!ppDevice)
      return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
  }
  D3DQUERYTYPE STDMETHODCALLTYPE GetType() override { return m_type; }
  DWORD STDMETHODCALLTYPE GetDataSize() override {
    switch (m_type) {
    case D3DQUERYTYPE_EVENT:     return sizeof(BOOL);
    case D3DQUERYTYPE_OCCLUSION: return sizeof(DWORD);
    default:                     return 0;
    }
  }
  HRESULT STDMETHODCALLTYPE Issue(DWORD dwIssueFlags) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetData(void *pData, DWORD dwSize,
                                    DWORD dwGetDataFlags) override {
    if (pData && dwSize) {
      memset(pData, 0, dwSize);
      if (m_type == D3DQUERYTYPE_EVENT && dwSize >= sizeof(BOOL))
        *(BOOL *)pData = TRUE;
      // occlusion: 0 samples passed (no per-draw counters yet)
    }
    return S_OK;
  }

private:
  volatile LONG m_refcount = 1;
  IDirect3DDevice9 *m_device;
  D3DQUERYTYPE m_type;
};

// ---------------------------------------------------------------------------
// device
// ---------------------------------------------------------------------------

struct D9MTVertex {
  float x, y, z, rhw;
  DWORD color;
};

static constexpr UINT VB_SIZE = 65536; // one VirtualAlloc page region
static constexpr UINT VERTEX_STRIDE = sizeof(D9MTVertex); // 20

class D9MTInterface;

class D9MTDevice final : public IDirect3DDevice9Ex {
  friend class D9MTInterface; // CreateDevice seeds m_pp
public:
  D9MTDevice(D9MTInterface *parent, HWND hwnd, UINT width, UINT height)
      : m_parent(parent), m_hwnd(hwnd), m_width(width), m_height(height) {
    for (UINT s = 0; s < MAX_SAMPLERS; s++) {
      m_samplerStates[s][D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
      m_samplerStates[s][D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
      m_samplerStates[s][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
      m_samplerStates[s][D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
      m_samplerStates[s][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
      m_samplerStates[s][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
      m_samplerStates[s][D3DSAMP_MAXANISOTROPY] = 1;
    }
    m_renderStates[D3DRS_ZENABLE] = D3DZB_TRUE;
    m_renderStates[D3DRS_ZWRITEENABLE] = TRUE;
    m_renderStates[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    m_renderStates[D3DRS_CULLMODE] = D3DCULL_CCW;
    m_renderStates[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    m_renderStates[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    m_renderStates[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    m_renderStates[D3DRS_COLORWRITEENABLE] = 0xF;
  }

  HRESULT Init();
  ~D9MTDevice();

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9 ||
        riid == IID_IDirect3DDevice9Ex) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_refcount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&m_refcount);
    if (!r)
      delete this;
    return r;
  }

  // swapchain / lifecycle
  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return D3D_OK; }
  UINT STDMETHODCALLTYPE GetAvailableTextureMem() override {
    return 512u << 20;
  }
  HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9 **ppD3D9) override;
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9 *pCaps) override;
  HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain,
                                           D3DDISPLAYMODE *pMode) override {
    if (!pMode)
      return D3DERR_INVALIDCALL;
    pMode->Width = m_width;
    pMode->Height = m_height;
    pMode->RefreshRate = 60;
    pMode->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *p) override {
    if (!p)
      return D3DERR_INVALIDCALL;
    p->AdapterOrdinal = 0;
    p->DeviceType = D3DDEVTYPE_HAL;
    p->hFocusWindow = m_hwnd;
    p->BehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetCursorProperties(
      UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) override {
    STUB_ONCE("SetCursorProperties");
    return D3D_OK;
  }
  void STDMETHODCALLTYPE SetCursorPosition(int X, int Y,
                                           DWORD Flags) override {}
  BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow) override { return TRUE; }
  HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(
      D3DPRESENT_PARAMETERS *pp, IDirect3DSwapChain9 **ppSwapChain) override {
    STUB_ONCE("CreateAdditionalSwapChain");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE
  GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **ppSwapChain) override {
    if (!ppSwapChain)
      return D3DERR_INVALIDCALL;
    if (iSwapChain != 0) {
      *ppSwapChain = nullptr;
      return D3DERR_INVALIDCALL;
    }
    if (!m_swapChain)
      m_swapChain = new D9MTSwapChain(this, m_pp);
    m_swapChain->AddRef();
    *ppSwapChain = m_swapChain;
    return D3D_OK;
  }
  UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return 1; }
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *pp) override {
    STUB_ONCE("Reset");
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE Present(const RECT *pSourceRect,
                                    const RECT *pDestRect,
                                    HWND hDestWindowOverride,
                                    const RGNDATA *pDirtyRegion) override;
  HRESULT STDMETHODCALLTYPE
  GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                IDirect3DSurface9 **ppBackBuffer) override {
    if (!ppBackBuffer)
      return D3DERR_INVALIDCALL;
    if (!m_backBuffer) {
      D3DSURFACE_DESC d = {};
      d.Format = D3DFMT_A8R8G8B8;
      d.Type = D3DRTYPE_SURFACE;
      d.Usage = D3DUSAGE_RENDERTARGET;
      d.Pool = D3DPOOL_DEFAULT;
      d.MultiSampleType = D3DMULTISAMPLE_NONE;
      d.Width = m_width;
      d.Height = m_height;
      m_backBuffer = new D9MTPlainSurface(this, d);
    }
    m_backBuffer->AddRef();
    *ppBackBuffer = m_backBuffer;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus) override {
    STUB_ONCE("GetRasterStatus");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs) override {
    return D3D_OK;
  }
  void STDMETHODCALLTYPE SetGammaRamp(UINT iSwapChain, DWORD Flags,
                                      const D3DGAMMARAMP *pRamp) override {}
  void STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain,
                                      D3DGAMMARAMP *pRamp) override {}

  HRESULT STDMETHODCALLTYPE CreateTexture(
      UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
      D3DPOOL Pool, IDirect3DTexture9 **ppTexture,
      HANDLE *pSharedHandle) override {
    if (!ppTexture || !Width || !Height)
      return D3DERR_INVALIDCALL;
    D9MTFormatInfo fi;
    if (!d3dfmt_to_wmt(Format, &fi)) {
      log_msg("CreateTexture: unsupported format %u", (unsigned)Format);
      return D3DERR_NOTAVAILABLE;
    }
    if (Usage & D3DUSAGE_RENDERTARGET)
      STUB_ONCE("CreateTexture: render target (no draw redirection yet)");
    D9MTTexture *tex = new D9MTTexture(this, Width, Height, Levels, Format, fi);
    if (!tex->Create(m_mtlDevice, Usage)) {
      log_msg("CreateTexture: newTexture failed (%ux%u fmt %u)", Width, Height,
              (unsigned)Format);
      tex->Release();
      return D3DERR_OUTOFVIDEOMEMORY;
    }
    *ppTexture = tex;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateVolumeTexture(
      UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
      D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture,
      HANDLE *pSharedHandle) override {
    STUB_ONCE("CreateVolumeTexture");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE CreateCubeTexture(
      UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
      IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) override {
    STUB_ONCE("CreateCubeTexture");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(
      UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
      IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle) override {
    if (!ppVertexBuffer || !Length)
      return D3DERR_INVALIDCALL;
    D9MTVertexBuffer *vb = new D9MTVertexBuffer(this);
    vb->m_length = Length;
    vb->m_usage = Usage;
    vb->m_fvf = FVF;
    if (!vb->m_storage.alloc(m_mtlDevice, Length)) {
      vb->Release();
      return D3DERR_OUTOFVIDEOMEMORY;
    }
    *ppVertexBuffer = vb;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(
      UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
      IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) override {
    if (!ppIndexBuffer || !Length)
      return D3DERR_INVALIDCALL;
    if (Format != D3DFMT_INDEX16 && Format != D3DFMT_INDEX32)
      return D3DERR_INVALIDCALL;
    D9MTIndexBuffer *ib = new D9MTIndexBuffer(this);
    ib->m_length = Length;
    ib->m_usage = Usage;
    ib->m_format = Format;
    if (!ib->m_storage.alloc(m_mtlDevice, Length)) {
      ib->Release();
      return D3DERR_OUTOFVIDEOMEMORY;
    }
    *ppIndexBuffer = ib;
    return D3D_OK;
  }
  // Placeholder surfaces: created so apps proceed, but rendering still
  // targets the swapchain pass only (real render-to-texture comes later)
  HRESULT STDMETHODCALLTYPE CreateRenderTarget(
      UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MS,
      DWORD MSQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface,
      HANDLE *pSharedHandle) override {
    STUB_ONCE("CreateRenderTarget (placeholder surface)");
    if (!ppSurface || !Width || !Height)
      return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC d = {};
    d.Format = Format;
    d.Type = D3DRTYPE_SURFACE;
    d.Usage = D3DUSAGE_RENDERTARGET;
    d.Pool = D3DPOOL_DEFAULT;
    d.MultiSampleType = MS;
    d.MultiSampleQuality = MSQuality;
    d.Width = Width;
    d.Height = Height;
    *ppSurface = new D9MTPlainSurface(this, d);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(
      UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MS,
      DWORD MSQuality, BOOL Discard, IDirect3DSurface9 **ppSurface,
      HANDLE *pSharedHandle) override {
    if (!ppSurface || !Width || !Height)
      return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC d = {};
    d.Format = Format;
    d.Type = D3DRTYPE_SURFACE;
    d.Usage = D3DUSAGE_DEPTHSTENCIL;
    d.Pool = D3DPOOL_DEFAULT;
    d.MultiSampleType = MS;
    d.MultiSampleQuality = MSQuality;
    d.Width = Width;
    d.Height = Height;
    *ppSurface = new D9MTPlainSurface(this, d);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  UpdateSurface(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect,
                IDirect3DSurface9 *pDestinationSurface,
                const POINT *pDestPoint) override {
    STUB_ONCE("UpdateSurface");
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture,
                IDirect3DBaseTexture9 *pDestinationTexture) override {
    STUB_ONCE("UpdateTexture");
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRenderTargetData(
      IDirect3DSurface9 *pRenderTarget,
      IDirect3DSurface9 *pDestSurface) override {
    STUB_ONCE("GetRenderTargetData");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE
  GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface) override {
    STUB_ONCE("GetFrontBufferData");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *pSourceSurface,
                                        const RECT *pSourceRect,
                                        IDirect3DSurface9 *pDestSurface,
                                        const RECT *pDestRect,
                                        D3DTEXTUREFILTERTYPE Filter) override {
    STUB_ONCE("StretchRect");
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *pSurface,
                                      const RECT *pRect,
                                      D3DCOLOR color) override {
    STUB_ONCE("ColorFill");
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(
      UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
      IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) override {
    STUB_ONCE("CreateOffscreenPlainSurface (placeholder surface)");
    if (!ppSurface || !Width || !Height)
      return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC d = {};
    d.Format = Format;
    d.Type = D3DRTYPE_SURFACE;
    d.Pool = Pool;
    d.Width = Width;
    d.Height = Height;
    *ppSurface = new D9MTPlainSurface(this, d);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  SetRenderTarget(DWORD RenderTargetIndex,
                  IDirect3DSurface9 *pRenderTarget) override {
    if (RenderTargetIndex == 0) {
      if (pRenderTarget && m_backBuffer &&
          pRenderTarget != (IDirect3DSurface9 *)m_backBuffer)
        STUB_ONCE("SetRenderTarget: non-backbuffer target (draws still hit "
                  "the swapchain)");
      m_rt0 = pRenderTarget;
    }
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetRenderTarget(DWORD RenderTargetIndex,
                  IDirect3DSurface9 **ppRenderTarget) override {
    if (!ppRenderTarget)
      return D3DERR_INVALIDCALL;
    if (RenderTargetIndex != 0) {
      *ppRenderTarget = nullptr;
      return D3DERR_NOTFOUND;
    }
    if (m_rt0) {
      m_rt0->AddRef();
      *ppRenderTarget = m_rt0;
      return D3D_OK;
    }
    return GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, ppRenderTarget);
  }
  HRESULT STDMETHODCALLTYPE
  SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) override {
    m_dsSurface = pNewZStencil;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) override {
    if (!ppZStencilSurface)
      return D3DERR_INVALIDCALL;
    if (m_dsSurface) {
      m_dsSurface->AddRef();
      *ppZStencilSurface = m_dsSurface;
      return D3D_OK;
    }
    if (!m_autoDepth) {
      D3DSURFACE_DESC d = {};
      d.Format = D3DFMT_D24S8;
      d.Type = D3DRTYPE_SURFACE;
      d.Usage = D3DUSAGE_DEPTHSTENCIL;
      d.Pool = D3DPOOL_DEFAULT;
      d.Width = m_width;
      d.Height = m_height;
      m_autoDepth = new D9MTPlainSurface(this, d);
    }
    m_autoDepth->AddRef();
    *ppZStencilSurface = m_autoDepth;
    return D3D_OK;
  }

  // frame
  HRESULT STDMETHODCALLTYPE BeginScene() override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE EndScene() override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE Clear(DWORD Count, const D3DRECT *pRects,
                                  DWORD Flags, D3DCOLOR Color, float Z,
                                  DWORD Stencil) override {
    if (Flags & D3DCLEAR_TARGET) {
      m_clearColor = Color;
      m_clearPending = true;
    }
    if (Flags & D3DCLEAR_ZBUFFER)
      m_clearDepth = Z; // the pass always clears depth (single pass/frame)
    return D3D_OK;
  }

  // fixed-function state: accept everything
  HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State,
                                         const D3DMATRIX *pMatrix) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State,
                                         D3DMATRIX *pMatrix) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  MultiplyTransform(D3DTRANSFORMSTATETYPE State,
                    const D3DMATRIX *pMatrix) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *pViewport) override {
    if (!pViewport)
      return D3DERR_INVALIDCALL;
    m_viewport = *pViewport;
    m_viewportSet = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *pViewport) override {
    if (!pViewport)
      return D3DERR_INVALIDCALL;
    if (m_viewportSet) {
      *pViewport = m_viewport;
      return D3D_OK;
    }
    pViewport->X = 0;
    pViewport->Y = 0;
    pViewport->Width = m_width;
    pViewport->Height = m_height;
    pViewport->MinZ = 0.0f;
    pViewport->MaxZ = 1.0f;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *pMaterial) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *pMaterial) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetLight(DWORD Index,
                                     const D3DLIGHT9 *pLight) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9 *pLight) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index,
                                           BOOL *pEnable) override {
    if (pEnable)
      *pEnable = FALSE;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index,
                                         const float *pPlane) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float *pPlane) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State,
                                           DWORD Value) override {
    if ((DWORD)State < MAX_RENDER_STATES)
      m_renderStates[State] = Value;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State,
                                           DWORD *pValue) override {
    if (!pValue)
      return D3DERR_INVALIDCALL;
    *pValue = (DWORD)State < MAX_RENDER_STATES ? m_renderStates[State] : 0;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  CreateStateBlock(D3DSTATEBLOCKTYPE Type,
                   IDirect3DStateBlock9 **ppSB) override {
    STUB_ONCE("CreateStateBlock");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE
  EndStateBlock(IDirect3DStateBlock9 **ppSB) override {
    STUB_ONCE("EndStateBlock");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE
  SetClipStatus(const D3DCLIPSTATUS9 *pClipStatus) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) override {
    if (!ppTexture)
      return D3DERR_INVALIDCALL;
    *ppTexture = Stage < MAX_SAMPLERS ? m_textures[Stage] : nullptr;
    if (*ppTexture)
      (*ppTexture)->AddRef();
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) override {
    if (Stage >= MAX_SAMPLERS) {
      STUB_ONCE("SetTexture: vertex/displacement sampler");
      return D3D_OK;
    }
    if (pTexture && pTexture->GetType() != D3DRTYPE_TEXTURE) {
      STUB_ONCE("SetTexture: non-2D texture");
      m_textures[Stage] = nullptr;
      return D3D_OK;
    }
    m_textures[Stage] = static_cast<D9MTTexture *>(pTexture);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type,
                       DWORD *pValue) override {
    if (pValue)
      *pValue = 0;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage,
                                                 D3DTEXTURESTAGESTATETYPE Type,
                                                 DWORD Value) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler,
                                            D3DSAMPLERSTATETYPE Type,
                                            DWORD *pValue) override {
    if (!pValue)
      return D3DERR_INVALIDCALL;
    *pValue = (Sampler < MAX_SAMPLERS && Type < MAX_SAMPLER_STATES)
                  ? m_samplerStates[Sampler][Type]
                  : 0;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler,
                                            D3DSAMPLERSTATETYPE Type,
                                            DWORD Value) override {
    if (Sampler < MAX_SAMPLERS && Type < MAX_SAMPLER_STATES)
      m_samplerStates[Sampler][Type] = Value;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *pNumPasses) override {
    if (pNumPasses)
      *pNumPasses = 1;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY *pEntries) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber,
                                              PALETTEENTRY *pEntries) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  SetCurrentTexturePalette(UINT PaletteNumber) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetCurrentTexturePalette(UINT *PaletteNumber) override {
    if (PaletteNumber)
      *PaletteNumber = 0;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *pRect) override {
    if (!pRect)
      return D3DERR_INVALIDCALL;
    m_scissor = *pRect;
    m_scissorSet = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *pRect) override {
    if (!pRect)
      return D3DERR_INVALIDCALL;
    if (m_scissorSet) {
      *pRect = m_scissor;
      return D3D_OK;
    }
    pRect->left = 0;
    pRect->top = 0;
    pRect->right = (LONG)m_width;
    pRect->bottom = (LONG)m_height;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  SetSoftwareVertexProcessing(BOOL bSoftware) override {
    return D3D_OK;
  }
  BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override {
    return FALSE;
  }
  HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override {
    return D3D_OK;
  }
  float STDMETHODCALLTYPE GetNPatchMode() override { return 0.0f; }

  // draws
  HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType,
                                          UINT StartVertex,
                                          UINT PrimitiveCount) override;
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(
      D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
      UINT NumVertices, UINT startIndex, UINT primCount) override;
  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(
      D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
      const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(
      D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices,
      UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat,
      const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override {
    STUB_ONCE("DrawIndexedPrimitiveUP");
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE ProcessVertices(
      UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
      IDirect3DVertexBuffer9 *pDestBuffer,
      IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags) override {
    STUB_ONCE("ProcessVertices");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(
      const D3DVERTEXELEMENT9 *pVertexElements,
      IDirect3DVertexDeclaration9 **ppDecl) override {
    if (!pVertexElements || !ppDecl)
      return D3DERR_INVALIDCALL;
    *ppDecl = new D9MTVertexDeclaration(this, pVertexElements);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) override {
    m_vdecl = static_cast<D9MTVertexDeclaration *>(pDecl);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) override {
    if (!ppDecl)
      return D3DERR_INVALIDCALL;
    if (m_vdecl)
      m_vdecl->AddRef();
    *ppDecl = m_vdecl;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override {
    m_fvf = FVF;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFVF(DWORD *pFVF) override {
    if (pFVF)
      *pFVF = m_fvf;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  CreateVertexShader(const DWORD *pFunction,
                     IDirect3DVertexShader9 **ppShader) override {
    if (!pFunction || !ppShader)
      return D3DERR_INVALIDCALL;
    D9MTVertexShader *sh = new D9MTVertexShader(this);
    HRESULT hr = CompileShader(pFunction, sh->m_data);
    if (FAILED(hr)) {
      sh->Release();
      return hr;
    }
    if (!sh->m_data.info.isVertexShader) {
      sh->Release();
      return D3DERR_INVALIDCALL;
    }
    *ppShader = sh;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  SetVertexShader(IDirect3DVertexShader9 *pShader) override {
    m_vs = static_cast<D9MTVertexShader *>(pShader);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetVertexShader(IDirect3DVertexShader9 **ppShader) override {
    if (!ppShader)
      return D3DERR_INVALIDCALL;
    if (m_vs)
      m_vs->AddRef();
    *ppShader = m_vs;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(
      UINT StartRegister, const float *pConstantData,
      UINT Vector4fCount) override {
    if (!pConstantData || StartRegister + Vector4fCount > 256)
      return D3DERR_INVALIDCALL;
    memcpy(m_vsConstF[StartRegister], pConstantData,
           Vector4fCount * 4 * sizeof(float));
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(
      UINT StartRegister, float *pConstantData, UINT Vector4fCount) override {
    if (!pConstantData || StartRegister + Vector4fCount > 256)
      return D3DERR_INVALIDCALL;
    memcpy(pConstantData, m_vsConstF[StartRegister],
           Vector4fCount * 4 * sizeof(float));
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(
      UINT StartRegister, const int *pConstantData,
      UINT Vector4iCount) override {
    if (!pConstantData || StartRegister + Vector4iCount > 16)
      return D3DERR_INVALIDCALL;
    memcpy(m_vsConstI[StartRegister], pConstantData,
           Vector4iCount * 4 * sizeof(int));
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(
      UINT StartRegister, int *pConstantData, UINT Vector4iCount) override {
    if (!pConstantData || StartRegister + Vector4iCount > 16)
      return D3DERR_INVALIDCALL;
    memcpy(pConstantData, m_vsConstI[StartRegister],
           Vector4iCount * 4 * sizeof(int));
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(
      UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(
      UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData,
                  UINT OffsetInBytes, UINT Stride) override {
    if (StreamNumber >= MAX_STREAMS)
      return D3DERR_INVALIDCALL;
    m_streams[StreamNumber].vb = static_cast<D9MTVertexBuffer *>(pStreamData);
    m_streams[StreamNumber].offset = OffsetInBytes;
    m_streams[StreamNumber].stride = Stride;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData,
                  UINT *pOffsetInBytes, UINT *pStride) override {
    if (StreamNumber >= MAX_STREAMS || !ppStreamData)
      return D3DERR_INVALIDCALL;
    if (m_streams[StreamNumber].vb)
      m_streams[StreamNumber].vb->AddRef();
    *ppStreamData = m_streams[StreamNumber].vb;
    if (pOffsetInBytes)
      *pOffsetInBytes = m_streams[StreamNumber].offset;
    if (pStride)
      *pStride = m_streams[StreamNumber].stride;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber,
                                                UINT Setting) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber,
                                                UINT *pSetting) override {
    if (pSetting)
      *pSetting = 1;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  SetIndices(IDirect3DIndexBuffer9 *pIndexData) override {
    m_indices = static_cast<D9MTIndexBuffer *>(pIndexData);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetIndices(IDirect3DIndexBuffer9 **ppIndexData) override {
    if (!ppIndexData)
      return D3DERR_INVALIDCALL;
    if (m_indices)
      m_indices->AddRef();
    *ppIndexData = m_indices;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  CreatePixelShader(const DWORD *pFunction,
                    IDirect3DPixelShader9 **ppShader) override {
    if (!pFunction || !ppShader)
      return D3DERR_INVALIDCALL;
    D9MTPixelShader *sh = new D9MTPixelShader(this);
    HRESULT hr = CompileShader(pFunction, sh->m_data);
    if (FAILED(hr)) {
      sh->Release();
      return hr;
    }
    if (sh->m_data.info.isVertexShader) {
      sh->Release();
      return D3DERR_INVALIDCALL;
    }
    *ppShader = sh;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  SetPixelShader(IDirect3DPixelShader9 *pShader) override {
    m_ps = static_cast<D9MTPixelShader *>(pShader);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetPixelShader(IDirect3DPixelShader9 **ppShader) override {
    if (!ppShader)
      return D3DERR_INVALIDCALL;
    if (m_ps)
      m_ps->AddRef();
    *ppShader = m_ps;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(
      UINT StartRegister, const float *pConstantData,
      UINT Vector4fCount) override {
    if (!pConstantData || StartRegister + Vector4fCount > 224)
      return D3DERR_INVALIDCALL;
    memcpy(m_psConstF[StartRegister], pConstantData,
           Vector4fCount * 4 * sizeof(float));
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(
      UINT StartRegister, float *pConstantData, UINT Vector4fCount) override {
    if (!pConstantData || StartRegister + Vector4fCount > 224)
      return D3DERR_INVALIDCALL;
    memcpy(pConstantData, m_psConstF[StartRegister],
           Vector4fCount * 4 * sizeof(float));
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(
      UINT StartRegister, const int *pConstantData,
      UINT Vector4iCount) override {
    if (!pConstantData || StartRegister + Vector4iCount > 16)
      return D3DERR_INVALIDCALL;
    memcpy(m_psConstI[StartRegister], pConstantData,
           Vector4iCount * 4 * sizeof(int));
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(
      UINT StartRegister, int *pConstantData, UINT Vector4iCount) override {
    if (!pConstantData || StartRegister + Vector4iCount > 16)
      return D3DERR_INVALIDCALL;
    memcpy(pConstantData, m_psConstI[StartRegister],
           Vector4iCount * 4 * sizeof(int));
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(
      UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(
      UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle, const float *pNumSegs,
                                          const D3DRECTPATCH_INFO *pInfo) override {
    STUB_ONCE("DrawRectPatch");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle, const float *pNumSegs,
                                         const D3DTRIPATCH_INFO *pInfo) override {
    STUB_ONCE("DrawTriPatch");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override {
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type,
                                        IDirect3DQuery9 **ppQuery) override {
    if (Type != D3DQUERYTYPE_EVENT && Type != D3DQUERYTYPE_OCCLUSION) {
      STUB_ONCE("CreateQuery: unsupported type");
      return D3DERR_NOTAVAILABLE;
    }
    if (!ppQuery) // capability probe
      return D3D_OK;
    *ppQuery = new D9MTQuery(this, Type);
    return D3D_OK;
  }

  // IDirect3DDevice9Ex
  HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT, UINT, float *,
                                                     float *) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9 *,
                                         IDirect3DSurface9 *,
                                         IDirect3DVertexBuffer9 *, UINT,
                                         IDirect3DVertexBuffer9 *,
                                         D3DCOMPOSERECTSOP, int,
                                         int) override {
    STUB_ONCE("ComposeRects");
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE PresentEx(const RECT *pSourceRect,
                                      const RECT *pDestRect,
                                      HWND hDestWindowOverride,
                                      const RGNDATA *pDirtyRegion,
                                      DWORD dwFlags) override {
    return Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }
  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *pPriority) override {
    if (pPriority)
      *pPriority = 0;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT) override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9 **,
                                                   UINT32) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) override {
    if (pMaxLatency)
      *pMaxLatency = 1;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND hDestinationWindow) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(
      UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MS,
      DWORD MSQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface,
      HANDLE *pSharedHandle, DWORD Usage) override {
    return CreateRenderTarget(Width, Height, Format, MS, MSQuality, Lockable,
                              ppSurface, pSharedHandle);
  }
  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(
      UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
      IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle,
      DWORD Usage) override {
    return CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface,
                                       pSharedHandle);
  }
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(
      UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MS,
      DWORD MSQuality, BOOL Discard, IDirect3DSurface9 **ppSurface,
      HANDLE *pSharedHandle, DWORD Usage) override {
    return CreateDepthStencilSurface(Width, Height, Format, MS, MSQuality,
                                     Discard, ppSurface, pSharedHandle);
  }
  HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS *pp,
                                    D3DDISPLAYMODEEX *) override {
    STUB_ONCE("ResetEx");
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT iSwapChain,
                                             D3DDISPLAYMODEEX *pMode,
                                             D3DDISPLAYROTATION *pRotation)
      override {
    if (pMode) {
      pMode->Size = sizeof(*pMode);
      pMode->Width = (UINT)GetSystemMetrics(SM_CXSCREEN);
      pMode->Height = (UINT)GetSystemMetrics(SM_CYSCREEN);
      pMode->RefreshRate = 60;
      pMode->Format = D3DFMT_X8R8G8B8;
      pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    }
    if (pRotation)
      *pRotation = D3DDISPLAYROTATION_IDENTITY;
    return D3D_OK;
  }

private:
  volatile LONG m_refcount = 1;
  D9MTInterface *m_parent;
  HWND m_hwnd;
  UINT m_width;
  UINT m_height;
  DWORD m_fvf = 0;

  // metal objects (all retained handles unless noted)
  obj_handle_t m_mtlDevice = 0;
  obj_handle_t m_queue = 0;
  obj_handle_t m_library = 0;
  obj_handle_t m_pso = 0;
  obj_handle_t m_view = 0;
  obj_handle_t m_layer = 0;
  obj_handle_t m_vbuf = 0;
  void *m_vbufMem = nullptr;

  // frame state
  UINT m_vertexCount = 0;
  D3DCOLOR m_clearColor = 0xFF000000;
  bool m_clearPending = true;

  // --- programmable pipeline state ---
  D9MTVertexShader *m_vs = nullptr;
  D9MTPixelShader *m_ps = nullptr;
  D9MTVertexDeclaration *m_vdecl = nullptr;
  struct StreamSource {
    D9MTVertexBuffer *vb = nullptr;
    UINT offset = 0;
    UINT stride = 0;
  } m_streams[MAX_STREAMS];
  D9MTIndexBuffer *m_indices = nullptr;

  // shader constants (HWVP layout: i[16] then f[N] in the cbuffer)
  float m_vsConstF[256][4] = {};
  int m_vsConstI[16][4] = {};
  float m_psConstF[224][4] = {};
  int m_psConstI[16][4] = {};

  // per-frame upload ring (cbuffers, argument buffers, render_state)
  D9MTBufferStorage m_ring;
  UINT m_ringOffset = 0;
  UINT m_ringStaticEnd = 0; // static allocations live below this watermark
  UINT m_specStateOffset = 0; // zeroed spec dwords + alpha-func ALWAYS
  UINT m_clipInfoOffset = 0;  // 6 zeroed clip planes

  // recorded render commands for this frame (deque: stable addresses)
  struct CmdSlot {
    alignas(16) uint8_t raw[64];
  };
  std::deque<CmdSlot> m_cmds;
  wmtcmd_base *m_cmdHead = nullptr;
  wmtcmd_base *m_cmdTail = nullptr;
  bool m_ringResident = false; // UseResource emitted this frame
  obj_handle_t m_lastPso = 0;  // dedupe SetPSO commands

  // PSO cache for the programmable path
  struct PsoKey {
    void *vs;
    void *ps;
    void *decl;
    uint32_t blend; // packed blend + color-write state
  };
  std::vector<std::pair<PsoKey, obj_handle_t>> m_psoCache;

  // --- textures + samplers ---
  D9MTTexture *m_textures[MAX_SAMPLERS] = {}; // not ref'd (MVP, like m_vs/m_ps)
  DWORD m_samplerStates[MAX_SAMPLERS][MAX_SAMPLER_STATES] = {};

  // one MTLSamplerState per distinct D3D9 sampler state vector; its
  // gpu_resource_id lives at heapIndex in the static sampler heap region
  // and shaders pick it via render_state.sN_idx
  struct SamplerEntry {
    uint64_t key;
    obj_handle_t handle;
  };
  std::vector<SamplerEntry> m_samplers;
  UINT m_samplerHeapOffset = 0; // static ring region: SAMPLER_HEAP_SLOTS u64s

  // textures made resident this frame (their MTLTextures live outside the
  // ring, so each needs its own UseResource)
  std::vector<obj_handle_t> m_frameResident;

  D3DPRESENT_PARAMETERS m_pp = {}; // as passed to CreateDevice
  D9MTSwapChain *m_swapChain = nullptr; // owned (one app ref per Get)

  // --- surfaces handed to the app ---
  D9MTPlainSurface *m_backBuffer = nullptr; // owned (one app ref each Get)
  D9MTPlainSurface *m_autoDepth = nullptr;  // owned
  IDirect3DSurface9 *m_rt0 = nullptr;       // borrowed, set by the app
  IDirect3DSurface9 *m_dsSurface = nullptr; // borrowed

  // --- render states, depth, viewport/scissor ---
  DWORD m_renderStates[MAX_RENDER_STATES] = {};
  obj_handle_t m_depthTex = 0; // Depth32Float, backbuffer-sized, private
  float m_clearDepth = 1.0f;
  D3DVIEWPORT9 m_viewport = {};
  bool m_viewportSet = false;
  RECT m_scissor = {};
  bool m_scissorSet = false;

  // depth-stencil state objects, deduped on (zenable, zwrite, zfunc)
  struct DssoEntry {
    uint32_t key;
    obj_handle_t handle;
  };
  std::vector<DssoEntry> m_dssoCache;
  obj_handle_t m_lastDsso = 0;       // dedupe SetDSSO per frame
  uint32_t m_lastRaster = ~0u;       // dedupe rasterizer state per frame
  uint64_t m_lastViewScissor = ~0ull; // dedupe viewport+scissor per frame

  template <typename T> T *AppendCmd(uint16_t type) {
    m_cmds.emplace_back();
    T *c = reinterpret_cast<T *>(m_cmds.back().raw);
    static_assert(sizeof(T) <= sizeof(CmdSlot), "cmd slot too small");
    memset(c, 0, sizeof(T));
    memcpy(c, &type, sizeof(type));
    if (m_cmdTail)
      m_cmdTail->next.set(c);
    else
      m_cmdHead = reinterpret_cast<wmtcmd_base *>(c);
    m_cmdTail = reinterpret_cast<wmtcmd_base *>(c);
    return c;
  }

  // bump-allocate from the upload ring; returns host pointer, fills offset
  void *RingAlloc(UINT size, UINT *outOffset) {
    UINT aligned = (m_ringOffset + 255u) & ~255u;
    if (aligned + size > RING_SIZE) {
      log_msg("RingAlloc: ring exhausted (%u + %u)", aligned, size);
      return nullptr;
    }
    *outOffset = aligned;
    m_ringOffset = aligned + size;
    return (uint8_t *)m_ring.mem + aligned;
  }

  HRESULT CompileShader(const DWORD *pFunction, D9MTShaderData &data);
  obj_handle_t GetOrCreatePso();
  obj_handle_t GetOrCreateDsso();
  uint32_t GetOrCreateSampler(UINT slot);
  void MarkResident(obj_handle_t res);
  bool PrepareDraw();
};

HRESULT D9MTDevice::Init() {
  obj_handle_t pool = NSAutoreleasePool_alloc_init();

  obj_handle_t devices = WMTCopyAllDevices();
  if (!devices || NSArray_count(devices) == 0) {
    log_msg("Init: no Metal devices");
    NSObject_release(pool);
    return D3DERR_NOTAVAILABLE;
  }
  m_mtlDevice = NSArray_object(devices, 0);
  NSObject_retain(m_mtlDevice);
  NSObject_release(devices);

  char name[128] = {0};
  obj_handle_t nameStr = MTLDevice_name(m_mtlDevice);
  if (nameStr)
    NSString_getCString(nameStr, name, sizeof(name), WMTUTF8StringEncoding);
  log_msg("Init: Metal device '%s', %ux%u, hwnd=%p", name, m_width, m_height,
          (void *)m_hwnd);

  m_queue = MTLDevice_newCommandQueue(m_mtlDevice, 8);
  if (!m_queue) {
    log_msg("Init: newCommandQueue failed");
    NSObject_release(pool);
    return D3DERR_NOTAVAILABLE;
  }

  // CAMetalLayer attached to the window
  m_view = CreateMetalViewFromHWND((intptr_t)m_hwnd, m_mtlDevice, &m_layer);
  if (!m_view || !m_layer) {
    log_msg("Init: CreateMetalViewFromHWND failed (view=%llx layer=%llx)",
            (unsigned long long)m_view, (unsigned long long)m_layer);
    NSObject_release(pool);
    return D3DERR_NOTAVAILABLE;
  }

  WMTLayerProps props = {};
  MetalLayer_getProps(m_layer, &props);
  props.device = m_mtlDevice;
  props.contents_scale = 1.0;
  props.drawable_width = m_width;
  props.drawable_height = m_height;
  props.opaque = true;
  props.display_sync_enabled = true;
  props.framebuffer_only = true;
  props.pixel_format = WMTPixelFormatBGRA8Unorm;
  MetalLayer_setProps(m_layer, &props);

  // shader library from the embedded precompiled metallib
  obj_handle_t data = DispatchData_alloc_init(
      (uint64_t)(uintptr_t)d9mt_shader_metallib, d9mt_shader_metallib_len);
  obj_handle_t err = 0;
  m_library = MTLDevice_newLibrary(m_mtlDevice, data, &err);
  NSObject_release(data);
  if (!m_library) {
    log_nserror("Init: newLibrary", err);
    NSObject_release(pool);
    return D3DERR_NOTAVAILABLE;
  }

  obj_handle_t vs = MTLLibrary_newFunction(m_library, "vs_main");
  obj_handle_t ps = MTLLibrary_newFunction(m_library, "ps_main");
  if (!vs || !ps) {
    log_msg("Init: newFunction failed (vs=%llx ps=%llx)",
            (unsigned long long)vs, (unsigned long long)ps);
    NSObject_release(pool);
    return D3DERR_NOTAVAILABLE;
  }

  WMTRenderPipelineInfo psoInfo = {};
  psoInfo.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
  psoInfo.colors[0].write_mask = WMTColorWriteMaskAll;
  psoInfo.colors[0].blending_enabled = false;
  psoInfo.rasterization_enabled = true;
  psoInfo.raster_sample_count = 1;
  // the frame pass always attaches the depth buffer, so every PSO
  // (fixed-function included) must declare its format
  psoInfo.depth_pixel_format = WMTPixelFormatDepth32Float;
  psoInfo.stencil_pixel_format = WMTPixelFormatInvalid;
  psoInfo.vertex_function = vs;
  psoInfo.fragment_function = ps;
  psoInfo.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
  psoInfo.max_tessellation_factor = 16; // Metal default; 0 trips validation
  err = 0;
  m_pso = MTLDevice_newRenderPipelineState(m_mtlDevice, &psoInfo, &err);
  NSObject_release(vs);
  NSObject_release(ps);
  if (!m_pso) {
    log_nserror("Init: newRenderPipelineState", err);
    NSObject_release(pool);
    return D3DERR_NOTAVAILABLE;
  }

  // host-visible vertex pool (page-aligned for newBufferWithBytesNoCopy)
  m_vbufMem = VirtualAlloc(nullptr, VB_SIZE, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
  if (!m_vbufMem) {
    log_msg("Init: VirtualAlloc failed");
    NSObject_release(pool);
    return D3DERR_NOTAVAILABLE;
  }
  WMTBufferInfo bufInfo = {};
  bufInfo.length = VB_SIZE;
  bufInfo.options = WMTResourceStorageModeShared;
  bufInfo.memory.set(m_vbufMem);
  m_vbuf = MTLDevice_newBuffer(m_mtlDevice, &bufInfo);
  if (!m_vbuf) {
    log_msg("Init: newBuffer failed");
    NSObject_release(pool);
    return D3DERR_NOTAVAILABLE;
  }

  // backbuffer-sized depth buffer, attached to every frame's render pass
  {
    WMTTextureInfo dti = {};
    dti.pixel_format = WMTPixelFormatDepth32Float;
    dti.width = m_width;
    dti.height = m_height;
    dti.depth = 1;
    dti.array_length = 1;
    dti.type = WMTTextureType2D;
    dti.mipmap_level_count = 1;
    dti.sample_count = 1;
    dti.usage = WMTTextureUsageRenderTarget;
    dti.options = WMTResourceStorageModePrivate;
    m_depthTex = MTLDevice_newTexture(m_mtlDevice, &dti);
    if (!m_depthTex) {
      log_msg("Init: depth texture creation failed");
      NSObject_release(pool);
      return D3DERR_NOTAVAILABLE;
    }
  }

  // upload ring for the programmable path (cbuffers, argument buffers,
  // render_state), plus static blocks the generated shaders always expect
  if (!m_ring.alloc(m_mtlDevice, RING_SIZE)) {
    log_msg("Init: ring alloc failed");
    NSObject_release(pool);
    return D3DERR_NOTAVAILABLE;
  }
  {
    // spec_state: 15 dwords; without function constants the shaders read
    // spec data from this buffer. All-zero except the alpha compare op,
    // which must be ALWAYS (7, dword1 bits [21:24)) or every fragment
    // gets discarded by the alpha-test epilogue.
    uint32_t *spec = (uint32_t *)RingAlloc(64, &m_specStateOffset);
    memset(spec, 0, 64);
    spec[1] = 7u << 21;
    // clip_info: 6 zeroed user clip planes
    float *clip = (float *)RingAlloc(6 * 16, &m_clipInfoOffset);
    memset(clip, 0, 6 * 16);
    // sampler heap: gpu_resource_ids of created MTLSamplerStates, filled
    // by GetOrCreateSampler as states are first used; persists across
    // frames (below the static watermark)
    void *heap = RingAlloc(SAMPLER_HEAP_SLOTS * 8, &m_samplerHeapOffset);
    memset(heap, 0, SAMPLER_HEAP_SLOTS * 8);
    m_ringStaticEnd = m_ringOffset;
  }

  NSObject_release(pool);
  log_msg("Init: OK");
  return D3D_OK;
}

D9MTDevice::~D9MTDevice() {
  for (auto &e : m_psoCache)
    NSObject_release(e.second);
  for (auto &s : m_samplers)
    NSObject_release(s.handle);
  for (auto &d : m_dssoCache)
    NSObject_release(d.handle);
  if (m_swapChain)
    m_swapChain->Release();
  if (m_backBuffer)
    m_backBuffer->Release();
  if (m_autoDepth)
    m_autoDepth->Release();
  if (m_depthTex)
    NSObject_release(m_depthTex);
  m_ring.free();
  if (m_vbuf)
    NSObject_release(m_vbuf);
  if (m_vbufMem)
    VirtualFree(m_vbufMem, 0, MEM_RELEASE);
  if (m_pso)
    NSObject_release(m_pso);
  if (m_library)
    NSObject_release(m_library);
  if (m_view)
    ReleaseMetalView(m_view);
  if (m_queue)
    NSObject_release(m_queue);
  if (m_mtlDevice)
    NSObject_release(m_mtlDevice);
  log_msg("device destroyed");
}

HRESULT D9MTDevice::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,
                                    UINT PrimitiveCount,
                                    const void *pVertexStreamZeroData,
                                    UINT VertexStreamZeroStride) {
  if (PrimitiveType != D3DPT_TRIANGLELIST) {
    STUB_ONCE("DrawPrimitiveUP: non-trianglelist");
    return D3D_OK;
  }
  if (!pVertexStreamZeroData || VertexStreamZeroStride < VERTEX_STRIDE)
    return D3DERR_INVALIDCALL;

  UINT numVerts = PrimitiveCount * 3;
  if ((m_vertexCount + numVerts) * VERTEX_STRIDE > VB_SIZE) {
    STUB_ONCE("DrawPrimitiveUP: vertex pool overflow, dropping");
    return D3D_OK;
  }

  // convert XYZRHW pixel coords -> NDC while copying into the shared buffer
  const uint8_t *src = (const uint8_t *)pVertexStreamZeroData;
  D9MTVertex *dst = (D9MTVertex *)m_vbufMem + m_vertexCount;
  float w = (float)m_width, h = (float)m_height;
  for (UINT i = 0; i < numVerts; i++) {
    const D9MTVertex *v = (const D9MTVertex *)(src + i * VertexStreamZeroStride);
    dst[i].x = v->x / w * 2.0f - 1.0f;
    dst[i].y = 1.0f - v->y / h * 2.0f;
    dst[i].z = v->z;
    dst[i].rhw = 1.0f;
    dst[i].color = v->color;
  }
  m_vertexCount += numVerts;
  return D3D_OK;
}

// ---------------------------------------------------------------------------
// programmable pipeline
// ---------------------------------------------------------------------------

HRESULT D9MTDevice::CompileShader(const DWORD *pFunction,
                                  D9MTShaderData &data) {
  // measure the bytecode: END token (0x0000FFFF) terminates the stream;
  // comment blocks (opcode 0xFFFE, dword length in [30:16]) may contain
  // arbitrary data (CTAB) and must be skipped, not scanned
  const DWORD *p = pFunction + 1; // skip version token
  while (*p != 0x0000FFFFu) {
    if ((*p & 0xFFFFu) == 0xFFFEu)
      p += ((*p >> 16) & 0x7FFFu) + 1;
    else
      p++;
  }
  size_t dwords = (size_t)(p - pFunction) + 1;
  data.bytecode.assign(pFunction, pFunction + dwords);

  std::string err;
  if (!d9mt_translate(data.bytecode.data(), data.info, err)) {
    log_msg("CompileShader: translation failed: %s", err.c_str());
    return D3DERR_INVALIDCALL;
  }

  struct d9mt_newlibrary_params lp;
  memset(&lp, 0, sizeof(lp));
  lp.device = m_mtlDevice;
  lp.source_ptr = (uint64_t)(uintptr_t)data.info.msl.data();
  lp.source_len = data.info.msl.size();
  lp.fast_math = 1;
  int st = D9MT_UnixCall(D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE, &lp);
  if (st != 0 || !lp.ret_library) {
    log_msg("CompileShader: newLibraryWithSource status %d", st);
    if (lp.ret_error)
      log_nserror("CompileShader: MSL compile", lp.ret_error);
    return D3DERR_INVALIDCALL;
  }
  data.library = lp.ret_library;
  if (lp.ret_error)
    NSObject_release(lp.ret_error); // compile warnings

  // supply ALL function constants (PSO creation crashes on unspecialized
  // functions); values are the dxso spec-constant defaults
  const auto &scs = data.info.specConstants;
  if (!scs.empty()) {
    std::vector<uint32_t> values(scs.size());
    std::vector<WMTFunctionConstant> consts(scs.size());
    for (size_t i = 0; i < scs.size(); i++) {
      values[i] = scs[i].second;
      memset(&consts[i], 0, sizeof(consts[i]));
      consts[i].data.set(&values[i]);
      consts[i].type = WMTDataTypeUInt;
      consts[i].index = (uint16_t)scs[i].first;
    }
    obj_handle_t fnErr = 0;
    data.function = MTLLibrary_newFunctionWithConstants(
        data.library, "main0", consts.data(), (uint32_t)consts.size(),
        &fnErr);
    if (!data.function) {
      log_nserror("CompileShader: newFunctionWithConstants", fnErr);
      return D3DERR_INVALIDCALL;
    }
    if (fnErr)
      NSObject_release(fnErr);
  } else {
    data.function = MTLLibrary_newFunction(data.library, "main0");
  }
  if (!data.function) {
    log_msg("CompileShader: main0 not found in compiled library");
    return D3DERR_INVALIDCALL;
  }

  log_msg("CompileShader: %s ok (%zu dwords, %zu bytes MSL, ab=%u rs=%d "
          "heap=%d)",
          data.info.isVertexShader ? "vs" : "ps", dwords,
          data.info.msl.size(), data.info.abEntryCount,
          data.info.rsBufferIndex, data.info.samplerHeapIndex);
  return D3D_OK;
}

obj_handle_t D9MTDevice::GetOrCreatePso() {
  // blend state lives in the Metal PSO, so it is part of the cache key
  bool blendOn = m_renderStates[D3DRS_ALPHABLENDENABLE] != 0;
  uint32_t blendKey =
      (blendOn ? 1u : 0) | (m_renderStates[D3DRS_SRCBLEND] & 31) << 1 |
      (m_renderStates[D3DRS_DESTBLEND] & 31) << 6 |
      (m_renderStates[D3DRS_BLENDOP] & 7) << 11 |
      (m_renderStates[D3DRS_COLORWRITEENABLE] & 0xF) << 14;

  PsoKey key{m_vs, m_ps, m_vdecl, blendKey};
  for (auto &e : m_psoCache)
    if (e.first.vs == key.vs && e.first.ps == key.ps &&
        e.first.decl == key.decl && e.first.blend == key.blend)
      return e.second;

  struct d9mt_pso_info info;
  memset(&info, 0, sizeof(info));
  info.vertex_function = m_vs->m_data.function;
  info.fragment_function = m_ps->m_data.function;
  info.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
  info.colors[0].write_mask =
      d3dwritemask_to_mtl(m_renderStates[D3DRS_COLORWRITEENABLE]);
  info.colors[0].blending_enabled = blendOn;
  if (blendOn) {
    uint32_t src = d3dblend_to_mtl(m_renderStates[D3DRS_SRCBLEND]);
    uint32_t dst = d3dblend_to_mtl(m_renderStates[D3DRS_DESTBLEND]);
    uint32_t op = d3dblendop_to_mtl(m_renderStates[D3DRS_BLENDOP]);
    info.colors[0].src_rgb_blend_factor = src;
    info.colors[0].dst_rgb_blend_factor = dst;
    info.colors[0].rgb_blend_op = op;
    // separate alpha blend (D3DRS_SEPARATEALPHABLENDENABLE) comes later;
    // alpha follows the color factors like D3D9 without it
    info.colors[0].src_alpha_blend_factor = src;
    info.colors[0].dst_alpha_blend_factor = dst;
    info.colors[0].alpha_blend_op = op;
  }
  info.depth_pixel_format = WMTPixelFormatDepth32Float;
  info.raster_sample_count = 1;

  bool streamUsed[MAX_STREAMS] = {};
  uint32_t na = 0;
  for (const auto &in : m_vs->m_data.info.inputs) {
    const D3DVERTEXELEMENT9 *match = nullptr;
    for (const auto &e : m_vdecl->m_elements)
      if (e.Usage == in.usage && e.UsageIndex == in.usageIndex) {
        match = &e;
        break;
      }
    if (!match) {
      log_msg("PSO: vertex decl has no element for shader input usage=%u "
              "index=%u",
              in.usage, in.usageIndex);
      return 0;
    }
    uint32_t fmt = decltype_to_mtl(match->Type);
    if (!fmt || match->Stream >= MAX_STREAMS) {
      log_msg("PSO: unsupported decl type %u / stream %u", match->Type,
              match->Stream);
      return 0;
    }
    info.attributes[na].format = fmt;
    info.attributes[na].offset = match->Offset;
    info.attributes[na].buffer_index = STREAM_BUFFER_BASE + match->Stream;
    info.attributes[na].location = in.location;
    na++;
    streamUsed[match->Stream] = true;
  }
  info.num_attributes = na;

  uint32_t nl = 0;
  for (UINT s = 0; s < MAX_STREAMS; s++) {
    if (!streamUsed[s])
      continue;
    if (!m_streams[s].vb || !m_streams[s].stride) {
      log_msg("PSO: stream %u referenced by decl but not bound", s);
      return 0;
    }
    info.layouts[nl].buffer_index = STREAM_BUFFER_BASE + s;
    info.layouts[nl].stride = m_streams[s].stride;
    info.layouts[nl].step_function = 1; // MTLVertexStepFunctionPerVertex
    info.layouts[nl].step_rate = 1;
    nl++;
  }
  info.num_layouts = nl;

  struct d9mt_newpso_params pp;
  memset(&pp, 0, sizeof(pp));
  pp.device = m_mtlDevice;
  pp.info_ptr = (uint64_t)(uintptr_t)&info;
  log_msg("PSO: creating (%u attributes, %u layouts)", na, nl);
  int st = D9MT_UnixCall(D9MT_FUNC_NEW_RENDER_PSO, &pp);
  if (st != 0 || !pp.ret_pso) {
    log_msg("PSO: creation failed, status %d", st);
    if (pp.ret_error)
      log_nserror("PSO: newRenderPipelineState", pp.ret_error);
    return 0;
  }
  if (pp.ret_error)
    NSObject_release(pp.ret_error);

  log_msg("PSO: created (%u attributes, %u layouts)", na, nl);
  m_psoCache.push_back({key, pp.ret_pso});
  return pp.ret_pso;
}

// D3DTADDRESS_* -> WMTSamplerAddressMode
static WMTSamplerAddressMode d3d_address_to_wmt(DWORD mode) {
  switch (mode) {
  case D3DTADDRESS_WRAP:       return WMTSamplerAddressModeRepeat;
  case D3DTADDRESS_MIRROR:     return WMTSamplerAddressModeMirrorRepeat;
  case D3DTADDRESS_CLAMP:      return WMTSamplerAddressModeClampToEdge;
  case D3DTADDRESS_BORDER:     return WMTSamplerAddressModeClampToBorderColor;
  case D3DTADDRESS_MIRRORONCE: return WMTSamplerAddressModeMirrorClampToEdge;
  default:                     return WMTSamplerAddressModeRepeat;
  }
}

// MTLSamplerState for the slot's current D3D9 sampler states, deduped on
// the packed state vector. Returns the heap index shaders use.
uint32_t D9MTDevice::GetOrCreateSampler(UINT slot) {
  const DWORD *ss = m_samplerStates[slot];
  uint64_t key = (uint64_t)(ss[D3DSAMP_ADDRESSU] & 7) |
                 (uint64_t)(ss[D3DSAMP_ADDRESSV] & 7) << 3 |
                 (uint64_t)(ss[D3DSAMP_ADDRESSW] & 7) << 6 |
                 (uint64_t)(ss[D3DSAMP_MAGFILTER] & 7) << 9 |
                 (uint64_t)(ss[D3DSAMP_MINFILTER] & 7) << 12 |
                 (uint64_t)(ss[D3DSAMP_MIPFILTER] & 7) << 15 |
                 (uint64_t)(ss[D3DSAMP_MAXANISOTROPY] & 31) << 18 |
                 (uint64_t)(ss[D3DSAMP_BORDERCOLOR] ? 1 : 0) << 23;
  for (uint32_t i = 0; i < m_samplers.size(); i++)
    if (m_samplers[i].key == key)
      return i;
  if (m_samplers.size() >= SAMPLER_HEAP_SLOTS) {
    STUB_ONCE("sampler heap full");
    return 0;
  }

  WMTSamplerInfo si = {};
  si.min_filter = ss[D3DSAMP_MINFILTER] >= D3DTEXF_LINEAR
                      ? WMTSamplerMinMagFilterLinear
                      : WMTSamplerMinMagFilterNearest;
  si.mag_filter = ss[D3DSAMP_MAGFILTER] >= D3DTEXF_LINEAR
                      ? WMTSamplerMinMagFilterLinear
                      : WMTSamplerMinMagFilterNearest;
  si.mip_filter = ss[D3DSAMP_MIPFILTER] == D3DTEXF_NONE
                      ? WMTSamplerMipFilterNotMipmapped
                      : (ss[D3DSAMP_MIPFILTER] == D3DTEXF_POINT
                             ? WMTSamplerMipFilterNearest
                             : WMTSamplerMipFilterLinear);
  si.s_address_mode = d3d_address_to_wmt(ss[D3DSAMP_ADDRESSU]);
  si.t_address_mode = d3d_address_to_wmt(ss[D3DSAMP_ADDRESSV]);
  si.r_address_mode = d3d_address_to_wmt(ss[D3DSAMP_ADDRESSW]);
  si.border_color = (ss[D3DSAMP_BORDERCOLOR] >> 24)
                        ? WMTSamplerBorderColorOpaqueBlack
                        : WMTSamplerBorderColorTransparentBlack;
  si.lod_min_clamp = 0.0f;
  si.lod_max_clamp = 1000.0f;
  si.max_anisotroy = ss[D3DSAMP_MINFILTER] == D3DTEXF_ANISOTROPIC
                         ? (ss[D3DSAMP_MAXANISOTROPY] ? ss[D3DSAMP_MAXANISOTROPY] : 1)
                         : 1;
  si.normalized_coords = true;
  si.support_argument_buffers = true;

  obj_handle_t handle = MTLDevice_newSamplerState(m_mtlDevice, &si);
  if (!handle) {
    log_msg("GetOrCreateSampler: newSamplerState failed (key %llx)",
            (unsigned long long)key);
    return 0;
  }
  uint32_t idx = (uint32_t)m_samplers.size();
  ((uint64_t *)((uint8_t *)m_ring.mem + m_samplerHeapOffset))[idx] =
      si.gpu_resource_id;
  m_samplers.push_back({key, handle});
  return idx;
}

obj_handle_t D9MTDevice::GetOrCreateDsso() {
  DWORD zenable = m_renderStates[D3DRS_ZENABLE] != D3DZB_FALSE;
  DWORD zwrite = m_renderStates[D3DRS_ZWRITEENABLE] != 0;
  DWORD zfunc = m_renderStates[D3DRS_ZFUNC];
  uint32_t key = (zenable ? 1u : 0) | (zwrite ? 2u : 0) | (zfunc & 15) << 2;
  for (auto &e : m_dssoCache)
    if (e.key == key)
      return e.handle;

  WMTDepthStencilInfo info = {};
  info.depth_compare_function =
      zenable ? d3dcmp_to_wmt(zfunc) : WMTCompareFunctionAlways;
  info.depth_write_enabled = zenable && zwrite;
  obj_handle_t dsso = MTLDevice_newDepthStencilState(m_mtlDevice, &info);
  if (!dsso) {
    log_msg("GetOrCreateDsso: newDepthStencilState failed (key %x)", key);
    return 0;
  }
  m_dssoCache.push_back({key, dsso});
  return dsso;
}

void D9MTDevice::MarkResident(obj_handle_t res) {
  for (obj_handle_t r : m_frameResident)
    if (r == res)
      return;
  auto *use = AppendCmd<wmtcmd_render_useresource>(WMTRenderCommandUseResource);
  use->resource = res;
  use->usage = WMTResourceUsageRead;
  use->stages =
      (WMTRenderStages)(WMTRenderStageVertex | WMTRenderStageFragment);
  m_frameResident.push_back(res);
}

bool D9MTDevice::PrepareDraw() {
  if (!m_vs || !m_ps || !m_vdecl) {
    STUB_ONCE("draw without vs/ps/vertex declaration (FF draw path)");
    return false;
  }
  obj_handle_t pso = GetOrCreatePso();
  if (!pso)
    return false;

  // everything the argument buffers point at lives in the ring; make it
  // resident once per frame
  if (!m_ringResident) {
    auto *use =
        AppendCmd<wmtcmd_render_useresource>(WMTRenderCommandUseResource);
    use->resource = m_ring.buf;
    use->usage = WMTResourceUsageRead;
    use->stages =
        (WMTRenderStages)(WMTRenderStageVertex | WMTRenderStageFragment);
    m_ringResident = true;
  }

  if (pso != m_lastPso) {
    auto *sp = AppendCmd<wmtcmd_render_setpso>(WMTRenderCommandSetPSO);
    sp->pso = pso;
    m_lastPso = pso;
  }

  obj_handle_t dsso = GetOrCreateDsso();
  if (dsso && dsso != m_lastDsso) {
    auto *sd = AppendCmd<wmtcmd_render_setdsso>(WMTRenderCommandSetDSSO);
    sd->dsso = dsso;
    sd->stencil_ref = 0;
    m_lastDsso = dsso;
  }

  // cull mode (D3D9 front faces wind clockwise; CW culls front, CCW back)
  {
    DWORD cull = m_renderStates[D3DRS_CULLMODE];
    uint32_t rasterKey = cull & 7;
    if (rasterKey != m_lastRaster) {
      auto *rst = AppendCmd<wmtcmd_render_setrasterizerstate>(
          WMTRenderCommandSetRasterizerState);
      rst->fill_mode = WMTTriangleFillModeFill;
      rst->cull_mode = cull == D3DCULL_CW    ? WMTCullModeFront
                       : cull == D3DCULL_CCW ? WMTCullModeBack
                                             : WMTCullModeNone;
      rst->depth_clip_mode = (WMTDepthClipMode)0; // clip
      rst->winding = WMTWindingClockwise;
      m_lastRaster = rasterKey;
    }
  }

  // viewport + scissor (scissor must stay inside the render target;
  // when the test is off Metal still wants a rect, so bind full-RT)
  {
    bool scissorOn =
        m_renderStates[D3DRS_SCISSORTESTENABLE] != 0 && m_scissorSet;
    uint64_t vsKey = ((uint64_t)scissorOn << 63) |
                     ((uint64_t)m_viewportSet << 62) |
                     ((uint64_t)(m_viewport.X ^ m_viewport.Width) << 32) |
                     (scissorOn ? (uint32_t)(m_scissor.left ^ m_scissor.right ^
                                             m_scissor.top ^ m_scissor.bottom)
                                : 0u);
    if (vsKey != m_lastViewScissor) {
      if (m_viewportSet) {
        auto *vp =
            AppendCmd<wmtcmd_render_setviewport>(WMTRenderCommandSetViewport);
        vp->viewport = {(double)m_viewport.X, (double)m_viewport.Y,
                        (double)m_viewport.Width, (double)m_viewport.Height,
                        (double)m_viewport.MinZ, (double)m_viewport.MaxZ};
      }
      auto *sc = AppendCmd<wmtcmd_render_setscissorrect>(
          WMTRenderCommandSetScissorRect);
      if (scissorOn)
        sc->scissor_rect = {(uint64_t)m_scissor.left, (uint64_t)m_scissor.top,
                            (uint64_t)(m_scissor.right - m_scissor.left),
                            (uint64_t)(m_scissor.bottom - m_scissor.top)};
      else
        sc->scissor_rect = {0, 0, m_width, m_height};
      m_lastViewScissor = vsKey;
    }
  }

  // render_state push block, shared by both stages
  UINT rsOff = 0;
  auto *rs = (D9MTRenderStateBlock *)RingAlloc(sizeof(D9MTRenderStateBlock),
                                               &rsOff);
  if (!rs)
    return false;
  memset(rs, 0, sizeof(*rs));
  rs->point_size = 1.0f;
  rs->point_size_min = 1.0f;
  rs->point_size_max = 64.0f;
  // per-slot sampler heap indices, filled from the shaders' texture
  // bindings below and packed two u16 per dword at the end
  uint16_t samplerIdx[MAX_SAMPLERS] = {};

  for (int stage = 0; stage < 2; stage++) {
    const D9MTShaderData &sh = stage ? m_ps->m_data : m_vs->m_data;
    const D9MTShaderInfo &si = sh.info;

    UINT cbOff = 0;
    if (si.idCbuffer >= 0) {
      UINT cbSize = 16 * 16 + si.floatConstCount * 16;
      uint8_t *cb = (uint8_t *)RingAlloc(cbSize, &cbOff);
      if (!cb)
        return false;
      if (stage) {
        memcpy(cb, m_psConstI, sizeof(m_psConstI));
        memcpy(cb + sizeof(m_psConstI), m_psConstF, si.floatConstCount * 16);
      } else {
        memcpy(cb, m_vsConstI, sizeof(m_vsConstI));
        memcpy(cb + sizeof(m_vsConstI), m_vsConstF, si.floatConstCount * 16);
      }
    }

    if (si.abEntryCount) {
      UINT abOff = 0;
      uint64_t *ab = (uint64_t *)RingAlloc(si.abEntryCount * 8, &abOff);
      if (!ab)
        return false;
      memset(ab, 0, si.abEntryCount * 8);
      if (si.idCbuffer >= 0)
        ab[si.idCbuffer] = m_ring.gpuAddr + cbOff;
      if (si.idClipInfo >= 0)
        ab[si.idClipInfo] = m_ring.gpuAddr + m_clipInfoOffset;
      if (si.idSpecState >= 0)
        ab[si.idSpecState] = m_ring.gpuAddr + m_specStateOffset;
      for (const auto &tb : si.textures) {
        if (tb.shadow)
          continue; // depth-compare variant: lands with depth textures
        if (!stage) {
          STUB_ONCE("vertex texture fetch");
          continue;
        }
        D9MTTexture *tex =
            tb.samplerSlot < MAX_SAMPLERS ? m_textures[tb.samplerSlot] : nullptr;
        if (!tex)
          continue; // slot empty: shader reads a null texture handle
        ab[tb.abId] = tex->m_gpuId;
        MarkResident(tex->SampleHandle());
        samplerIdx[tb.samplerSlot] =
            (uint16_t)GetOrCreateSampler(tb.samplerSlot);
      }

      auto *sb = AppendCmd<wmtcmd_render_setbuffer>(
          stage ? WMTRenderCommandSetFragmentBuffer
                : WMTRenderCommandSetVertexBuffer);
      sb->buffer = m_ring.buf;
      sb->offset = abOff;
      sb->index = 0;
    }

    if (si.rsBufferIndex >= 0) {
      auto *sb = AppendCmd<wmtcmd_render_setbuffer>(
          stage ? WMTRenderCommandSetFragmentBuffer
                : WMTRenderCommandSetVertexBuffer);
      sb->buffer = m_ring.buf;
      sb->offset = rsOff;
      sb->index = (uint8_t)si.rsBufferIndex;
    }

    if (si.samplerHeapIndex >= 0) {
      auto *sb = AppendCmd<wmtcmd_render_setbuffer>(
          stage ? WMTRenderCommandSetFragmentBuffer
                : WMTRenderCommandSetVertexBuffer);
      sb->buffer = m_ring.buf;
      sb->offset = m_samplerHeapOffset;
      sb->index = (uint8_t)si.samplerHeapIndex;
    }
  }

  for (uint32_t k = 0; k < 8; k++)
    rs->sampler_idx[k] =
        ((uint32_t)samplerIdx[2 * k + 1] << 16) | samplerIdx[2 * k];

  // vertex streams
  for (UINT s = 0; s < MAX_STREAMS; s++) {
    if (!m_streams[s].vb)
      continue;
    auto *sb =
        AppendCmd<wmtcmd_render_setbuffer>(WMTRenderCommandSetVertexBuffer);
    sb->buffer = m_streams[s].vb->m_storage.buf;
    sb->offset = m_streams[s].offset;
    sb->index = (uint8_t)(STREAM_BUFFER_BASE + s);
  }
  return true;
}

static bool prim_to_mtl(D3DPRIMITIVETYPE t, UINT primCount,
                        WMTPrimitiveType *outType, uint64_t *outCount) {
  switch (t) {
  case D3DPT_POINTLIST:
    *outType = WMTPrimitiveTypePoint;
    *outCount = primCount;
    return true;
  case D3DPT_LINELIST:
    *outType = WMTPrimitiveTypeLine;
    *outCount = (uint64_t)primCount * 2;
    return true;
  case D3DPT_LINESTRIP:
    *outType = WMTPrimitiveTypeLineStrip;
    *outCount = (uint64_t)primCount + 1;
    return true;
  case D3DPT_TRIANGLELIST:
    *outType = WMTPrimitiveTypeTriangle;
    *outCount = (uint64_t)primCount * 3;
    return true;
  case D3DPT_TRIANGLESTRIP:
    *outType = WMTPrimitiveTypeTriangleStrip;
    *outCount = (uint64_t)primCount + 2;
    return true;
  default:
    return false; // triangle fans need emulation
  }
}

HRESULT D9MTDevice::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType,
                                  UINT StartVertex, UINT PrimitiveCount) {
  WMTPrimitiveType pt;
  uint64_t count;
  if (!prim_to_mtl(PrimitiveType, PrimitiveCount, &pt, &count)) {
    STUB_ONCE("DrawPrimitive: unsupported primitive type");
    return D3D_OK;
  }
  if (!PrepareDraw())
    return D3D_OK;
  auto *d = AppendCmd<wmtcmd_render_draw>(WMTRenderCommandDraw);
  d->primitive_type = pt;
  d->vertex_start = StartVertex;
  d->vertex_count = count;
  d->instance_count = 1;
  return D3D_OK;
}

HRESULT D9MTDevice::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType,
                                         INT BaseVertexIndex,
                                         UINT MinVertexIndex,
                                         UINT NumVertices, UINT startIndex,
                                         UINT primCount) {
  if (!m_indices)
    return D3DERR_INVALIDCALL;
  WMTPrimitiveType pt;
  uint64_t count;
  if (!prim_to_mtl(PrimitiveType, primCount, &pt, &count)) {
    STUB_ONCE("DrawIndexedPrimitive: unsupported primitive type");
    return D3D_OK;
  }
  if (!PrepareDraw())
    return D3D_OK;
  bool idx32 = m_indices->m_format == D3DFMT_INDEX32;
  auto *d =
      AppendCmd<wmtcmd_render_draw_indexed>(WMTRenderCommandDrawIndexed);
  d->primitive_type = pt;
  d->index_type = idx32 ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
  d->index_count = count;
  d->index_buffer = m_indices->m_storage.buf;
  d->index_buffer_offset = (uint64_t)startIndex * (idx32 ? 4 : 2);
  d->instance_count = 1;
  d->base_vertex = BaseVertexIndex;
  return D3D_OK;
}

HRESULT D9MTDevice::Present(const RECT *pSourceRect, const RECT *pDestRect,
                            HWND hDestWindowOverride,
                            const RGNDATA *pDirtyRegion) {
  obj_handle_t pool = NSAutoreleasePool_alloc_init();

  obj_handle_t drawable = MetalLayer_nextDrawable(m_layer);
  if (!drawable) {
    log_msg("Present: nextDrawable returned null");
    NSObject_release(pool);
    m_vertexCount = 0;
    return D3D_OK;
  }
  obj_handle_t texture = MetalDrawable_texture(drawable);
  obj_handle_t cmdbuf = MTLCommandQueue_commandBuffer(m_queue);

  WMTRenderPassInfo pass = {};
  pass.colors[0].texture = texture;
  pass.colors[0].load_action =
      m_clearPending ? WMTLoadActionClear : WMTLoadActionDontCare;
  pass.colors[0].store_action = WMTStoreActionStore;
  pass.colors[0].clear_color.r = ((m_clearColor >> 16) & 0xff) / 255.0;
  pass.colors[0].clear_color.g = ((m_clearColor >> 8) & 0xff) / 255.0;
  pass.colors[0].clear_color.b = (m_clearColor & 0xff) / 255.0;
  pass.colors[0].clear_color.a = ((m_clearColor >> 24) & 0xff) / 255.0;
  // single render pass per frame: depth always attached, cleared at load
  pass.depth.texture = m_depthTex;
  pass.depth.load_action = WMTLoadActionClear;
  pass.depth.store_action = WMTStoreActionDontCare;
  pass.depth.clear_depth = m_clearDepth;

  obj_handle_t enc = MTLCommandBuffer_renderCommandEncoder(cmdbuf, &pass);
  if (enc) {
    // fixed-function UP block first (if any), then the recorded
    // programmable-path commands chained after it
    struct wmtcmd_render_setpso setPso = {};
    struct wmtcmd_render_setbuffer setVb = {};
    struct wmtcmd_render_draw draw = {};
    const struct wmtcmd_base *head = nullptr;

    if (m_vertexCount) {
      setPso.type = WMTRenderCommandSetPSO;
      setPso.next.set(&setVb);
      setPso.pso = m_pso;

      setVb.type = WMTRenderCommandSetVertexBuffer;
      setVb.next.set(&draw);
      setVb.buffer = m_vbuf;
      setVb.offset = 0;
      setVb.index = 0;

      draw.type = WMTRenderCommandDraw;
      draw.next.set(m_cmdHead);
      draw.primitive_type = WMTPrimitiveTypeTriangle;
      draw.vertex_start = 0;
      draw.vertex_count = m_vertexCount;
      draw.instance_count = 1;
      draw.base_instance = 0;

      head = (const struct wmtcmd_base *)&setPso;
    } else {
      head = m_cmdHead;
    }

    if (head)
      MTLRenderCommandEncoder_encodeCommands(enc, head);
    MTLCommandEncoder_endEncoding(enc);
  } else {
    log_msg("Present: renderCommandEncoder failed");
  }

  MTLCommandBuffer_presentDrawable(cmdbuf, drawable);
  MTLCommandBuffer_commit(cmdbuf);
  // MVP: block until the GPU is done so the drawable/cmdbuf can be drained
  // with the autorelease pool below. Triple-buffering comes with Phase B.
  MTLCommandBuffer_waitUntilCompleted(cmdbuf);

  NSObject_release(pool);
  m_vertexCount = 0;
  m_clearPending = false;

  // frame done (waitUntilCompleted above): recycle command arena + ring
  m_cmds.clear();
  m_cmdHead = nullptr;
  m_cmdTail = nullptr;
  m_ringOffset = m_ringStaticEnd;
  m_ringResident = false;
  m_frameResident.clear();
  m_lastPso = 0;
  m_lastDsso = 0;
  m_lastRaster = ~0u;
  m_lastViewScissor = ~0ull;

  static int presented = 0;
  if (presented < 3) {
    presented++;
    log_msg("Present: frame %d OK", presented);
  }
  return D3D_OK;
}

// ---------------------------------------------------------------------------
// IDirect3D9
// ---------------------------------------------------------------------------

class D9MTInterface final : public IDirect3D9Ex {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3D9 ||
        riid == IID_IDirect3D9Ex) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_refcount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&m_refcount);
    if (!r)
      delete this;
    return r;
  }

  HRESULT STDMETHODCALLTYPE
  RegisterSoftwareDevice(void *pInitializeFunction) override {
    return D3D_OK;
  }
  UINT STDMETHODCALLTYPE GetAdapterCount() override { return 1; }
  HRESULT STDMETHODCALLTYPE
  GetAdapterIdentifier(UINT Adapter, DWORD Flags,
                       D3DADAPTER_IDENTIFIER9 *pIdentifier) override {
    if (!pIdentifier)
      return D3DERR_INVALIDCALL;
    memset(pIdentifier, 0, sizeof(*pIdentifier));
    strcpy(pIdentifier->Driver, "d9mt.dll");
    strcpy(pIdentifier->Description, "d9mt Metal Adapter");
    strcpy(pIdentifier->DeviceName, "\\\\.\\DISPLAY1");
    pIdentifier->VendorId = 0x106B; // Apple
    pIdentifier->DeviceId = 0x0001;
    pIdentifier->DriverVersion.QuadPart = 1;
    return D3D_OK;
  }
  UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter,
                                             D3DFORMAT Format) override {
    return 1;
  }
  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format,
                                             UINT Mode,
                                             D3DDISPLAYMODE *pMode) override {
    return GetAdapterDisplayMode(Adapter, pMode);
  }
  HRESULT STDMETHODCALLTYPE
  GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) override {
    if (!pMode)
      return D3DERR_INVALIDCALL;
    pMode->Width = (UINT)GetSystemMetrics(SM_CXSCREEN);
    pMode->Height = (UINT)GetSystemMetrics(SM_CYSCREEN);
    pMode->RefreshRate = 60;
    pMode->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType,
                                            D3DFORMAT AdapterFormat,
                                            D3DFORMAT BackBufferFormat,
                                            BOOL bWindowed) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DevType,
                                              D3DFORMAT AdapterFormat,
                                              DWORD Usage, D3DRESOURCETYPE RType,
                                              D3DFORMAT CheckFormat) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat,
      BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType,
      DWORD *pQualityLevels) override {
    if (pQualityLevels)
      *pQualityLevels = 1;
    return MultiSampleType == D3DMULTISAMPLE_NONE ? D3D_OK
                                                  : D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
      D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat,
      D3DFORMAT TargetFormat) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType,
                                          D3DCAPS9 *pCaps) override;
  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override {
    POINT pt = {0, 0};
    return MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
  }
  HRESULT STDMETHODCALLTYPE
  CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
               DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pp,
               IDirect3DDevice9 **ppReturnedDeviceInterface) override {
    if (!ppReturnedDeviceInterface || !pp)
      return D3DERR_INVALIDCALL;
    *ppReturnedDeviceInterface = nullptr;

    HWND hwnd = pp->hDeviceWindow ? pp->hDeviceWindow : hFocusWindow;
    if (!hwnd)
      return D3DERR_INVALIDCALL;

    UINT width = pp->BackBufferWidth;
    UINT height = pp->BackBufferHeight;
    if (!width || !height) {
      RECT rc;
      GetClientRect(hwnd, &rc);
      width = rc.right - rc.left;
      height = rc.bottom - rc.top;
    }
    if (!width)
      width = 640;
    if (!height)
      height = 480;

    D9MTDevice *dev = new D9MTDevice(this, hwnd, width, height);
    dev->m_pp = *pp;
    dev->m_pp.BackBufferWidth = width;
    dev->m_pp.BackBufferHeight = height;
    HRESULT hr = dev->Init();
    if (FAILED(hr)) {
      dev->Release();
      return hr;
    }
    AddRef(); // device keeps a reference to its parent interface
    *ppReturnedDeviceInterface = dev;
    return D3D_OK;
  }

  // IDirect3D9Ex
  UINT STDMETHODCALLTYPE GetAdapterModeCountEx(
      UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter) override {
    return 1;
  }
  HRESULT STDMETHODCALLTYPE
  EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter,
                     UINT Mode, D3DDISPLAYMODEEX *pMode) override {
    return GetAdapterDisplayModeEx(Adapter, pMode, nullptr);
  }
  HRESULT STDMETHODCALLTYPE
  GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX *pMode,
                          D3DDISPLAYROTATION *pRotation) override {
    if (pMode) {
      pMode->Size = sizeof(*pMode);
      pMode->Width = (UINT)GetSystemMetrics(SM_CXSCREEN);
      pMode->Height = (UINT)GetSystemMetrics(SM_CYSCREEN);
      pMode->RefreshRate = 60;
      pMode->Format = D3DFMT_X8R8G8B8;
      pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    }
    if (pRotation)
      *pRotation = D3DDISPLAYROTATION_IDENTITY;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateDeviceEx(
      UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
      DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pp,
      D3DDISPLAYMODEEX *pFullscreenDisplayMode,
      IDirect3DDevice9Ex **ppReturnedDeviceInterface) override {
    // fullscreen mode handled as windowed-sized swapchain for now
    return CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pp,
                        (IDirect3DDevice9 **)ppReturnedDeviceInterface);
  }
  HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT Adapter, LUID *pLUID) override {
    if (!pLUID)
      return D3DERR_INVALIDCALL;
    pLUID->LowPart = 0xD937;
    pLUID->HighPart = 0;
    return D3D_OK;
  }

  volatile LONG m_refcount = 1;
};

static void fill_caps(D3DCAPS9 *caps, UINT adapter) {
  memset(caps, 0, sizeof(*caps));
  caps->DeviceType = D3DDEVTYPE_HAL;
  caps->AdapterOrdinal = adapter;
  caps->Caps2 = D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_FULLSCREENGAMMA;
  caps->PresentationIntervals =
      D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_IMMEDIATE;
  caps->DevCaps = D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_HWRASTERIZATION |
                  D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_PUREDEVICE;
  caps->PrimitiveMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW |
                            D3DPMISCCAPS_CULLCCW | D3DPMISCCAPS_BLENDOP;
  caps->RasterCaps = D3DPRASTERCAPS_SCISSORTEST;
  caps->TextureCaps = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_MIPMAP;
  caps->MaxTextureWidth = 16384;
  caps->MaxTextureHeight = 16384;
  caps->MaxVolumeExtent = 2048;
  caps->MaxTextureRepeat = 8192;
  caps->MaxAnisotropy = 16;
  caps->MaxTextureBlendStages = 8;
  caps->MaxSimultaneousTextures = 8;
  caps->MaxActiveLights = 8;
  caps->MaxUserClipPlanes = 6;
  caps->MaxPrimitiveCount = 0x555555;
  caps->MaxVertexIndex = 0xFFFFFF;
  caps->MaxStreams = 16;
  caps->MaxStreamStride = 508;
  caps->VertexShaderVersion = D3DVS_VERSION(3, 0);
  caps->MaxVertexShaderConst = 256;
  caps->PixelShaderVersion = D3DPS_VERSION(3, 0);
  caps->PixelShader1xMaxValue = 8.0f;
  caps->NumSimultaneousRTs = 4;
  caps->MaxVShaderInstructionsExecuted = 65535;
  caps->MaxPShaderInstructionsExecuted = 65535;
  caps->MaxVertexShader30InstructionSlots = 32768;
  caps->MaxPixelShader30InstructionSlots = 32768;
}

HRESULT D9MTInterface::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType,
                                     D3DCAPS9 *pCaps) {
  if (!pCaps)
    return D3DERR_INVALIDCALL;
  fill_caps(pCaps, Adapter);
  return D3D_OK;
}

HRESULT D9MTDevice::GetDeviceCaps(D3DCAPS9 *pCaps) {
  if (!pCaps)
    return D3DERR_INVALIDCALL;
  fill_caps(pCaps, 0);
  return D3D_OK;
}

HRESULT D9MTDevice::GetDirect3D(IDirect3D9 **ppD3D9) {
  if (!ppD3D9)
    return D3DERR_INVALIDCALL;
  m_parent->AddRef();
  *ppD3D9 = m_parent;
  return D3D_OK;
}

// ---------------------------------------------------------------------------
// exports
// ---------------------------------------------------------------------------

extern "C" {

IDirect3D9 *WINAPI Direct3DCreate9(UINT SDKVersion) {
  log_msg("Direct3DCreate9(SDK %u)", SDKVersion);
  return new D9MTInterface();
}

HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D) {
  log_msg("Direct3DCreate9Ex(SDK %u)", SDKVersion);
  if (!ppD3D)
    return D3DERR_INVALIDCALL;
  *ppD3D = new D9MTInterface();
  return S_OK;
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName) { return 0; }
int WINAPI D3DPERF_EndEvent() { return 0; }
DWORD WINAPI D3DPERF_GetStatus() { return 0; }
BOOL WINAPI D3DPERF_QueryRepeatFrame() { return FALSE; }
void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName) {}
void WINAPI D3DPERF_SetOptions(DWORD dwOptions) {}
void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName) {}

void WINAPI DebugSetMute() {}
int WINAPI DebugSetLevel() { return 0; }

HRESULT WINAPI Direct3DShaderValidatorCreate9() { return E_NOTIMPL; }
void WINAPI PSGPError() {}
void WINAPI PSGPSampleTexture() {}
void WINAPI Direct3D9EnableMaximizedWindowedModeShim() {}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH)
    DisableThreadLibraryCalls(instance);
  return TRUE;
}

} // extern "C"
