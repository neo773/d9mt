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
// device
// ---------------------------------------------------------------------------

struct D9MTVertex {
  float x, y, z, rhw;
  DWORD color;
};

static constexpr UINT VB_SIZE = 65536; // one VirtualAlloc page region
static constexpr UINT VERTEX_STRIDE = sizeof(D9MTVertex); // 20

class D9MTInterface;

class D9MTDevice final : public IDirect3DDevice9 {
public:
  D9MTDevice(D9MTInterface *parent, HWND hwnd, UINT width, UINT height)
      : m_parent(parent), m_hwnd(hwnd), m_width(width), m_height(height) {}

  HRESULT Init();
  ~D9MTDevice();

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9) {
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
    STUB_ONCE("GetSwapChain");
    return D3DERR_NOTAVAILABLE;
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
    STUB_ONCE("GetBackBuffer");
    if (ppBackBuffer)
      *ppBackBuffer = nullptr;
    return D3DERR_NOTAVAILABLE;
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

  // resource creation: all unsupported in MVP
  HRESULT STDMETHODCALLTYPE CreateTexture(
      UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
      D3DPOOL Pool, IDirect3DTexture9 **ppTexture,
      HANDLE *pSharedHandle) override {
    STUB_ONCE("CreateTexture");
    return D3DERR_NOTAVAILABLE;
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
  HRESULT STDMETHODCALLTYPE CreateRenderTarget(
      UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MS,
      DWORD MSQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface,
      HANDLE *pSharedHandle) override {
    STUB_ONCE("CreateRenderTarget");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(
      UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MS,
      DWORD MSQuality, BOOL Discard, IDirect3DSurface9 **ppSurface,
      HANDLE *pSharedHandle) override {
    STUB_ONCE("CreateDepthStencilSurface");
    return D3DERR_NOTAVAILABLE;
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
    STUB_ONCE("CreateOffscreenPlainSurface");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE
  SetRenderTarget(DWORD RenderTargetIndex,
                  IDirect3DSurface9 *pRenderTarget) override {
    STUB_ONCE("SetRenderTarget");
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetRenderTarget(DWORD RenderTargetIndex,
                  IDirect3DSurface9 **ppRenderTarget) override {
    STUB_ONCE("GetRenderTarget");
    if (ppRenderTarget)
      *ppRenderTarget = nullptr;
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE
  SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) override {
    if (ppZStencilSurface)
      *ppZStencilSurface = nullptr;
    return D3DERR_NOTFOUND;
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
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *pViewport) override {
    if (!pViewport)
      return D3DERR_INVALIDCALL;
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
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State,
                                           DWORD *pValue) override {
    if (pValue)
      *pValue = 0;
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
    if (ppTexture)
      *ppTexture = nullptr;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) override {
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
    if (pValue)
      *pValue = 0;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler,
                                            D3DSAMPLERSTATETYPE Type,
                                            DWORD Value) override {
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
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *pRect) override {
    if (pRect) {
      pRect->left = 0;
      pRect->top = 0;
      pRect->right = (LONG)m_width;
      pRect->bottom = (LONG)m_height;
    }
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
    STUB_ONCE("CreateQuery");
    return D3DERR_NOTAVAILABLE;
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
  };
  std::vector<std::pair<PsoKey, obj_handle_t>> m_psoCache;

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
  psoInfo.depth_pixel_format = WMTPixelFormatInvalid;
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
    m_ringStaticEnd = m_ringOffset;
  }

  NSObject_release(pool);
  log_msg("Init: OK");
  return D3D_OK;
}

D9MTDevice::~D9MTDevice() {
  for (auto &e : m_psoCache)
    NSObject_release(e.second);
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
  PsoKey key{m_vs, m_ps, m_vdecl};
  for (auto &e : m_psoCache)
    if (e.first.vs == key.vs && e.first.ps == key.ps &&
        e.first.decl == key.decl)
      return e.second;

  struct d9mt_pso_info info;
  memset(&info, 0, sizeof(info));
  info.vertex_function = m_vs->m_data.function;
  info.fragment_function = m_ps->m_data.function;
  info.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
  info.colors[0].write_mask = 0xF; // MTLColorWriteMaskAll
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
  // sampler heap index == D3D9 sampler slot; two u16 indices per dword
  for (uint32_t k = 0; k < 8; k++)
    rs->sampler_idx[k] = ((2 * k + 1) << 16) | (2 * k);

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
      // textures stay 0 until SetTexture support lands

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

    if (stage && si.samplerHeapIndex >= 0) {
      // no samplers yet: bind a zeroed heap so the shader has something
      UINT heapOff = 0;
      void *heap = RingAlloc(32 * 8, &heapOff);
      if (!heap)
        return false;
      memset(heap, 0, 32 * 8);
      auto *sb = AppendCmd<wmtcmd_render_setbuffer>(
          WMTRenderCommandSetFragmentBuffer);
      sb->buffer = m_ring.buf;
      sb->offset = heapOff;
      sb->index = (uint8_t)si.samplerHeapIndex;
    }
  }

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
  m_lastPso = 0;

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

class D9MTInterface final : public IDirect3D9 {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3D9) {
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
    HRESULT hr = dev->Init();
    if (FAILED(hr)) {
      dev->Release();
      return hr;
    }
    AddRef(); // device keeps a reference to its parent interface
    *ppReturnedDeviceInterface = dev;
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
  log_msg("Direct3DCreate9Ex: not available");
  if (ppD3D)
    *ppD3D = nullptr;
  return D3DERR_NOTAVAILABLE;
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
