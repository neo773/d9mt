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

#include "../winemetal.h"
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
    STUB_ONCE("CreateVertexBuffer");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(
      UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
      IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) override {
    STUB_ONCE("CreateIndexBuffer");
    return D3DERR_NOTAVAILABLE;
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
                                          UINT PrimitiveCount) override {
    STUB_ONCE("DrawPrimitive");
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(
      D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
      UINT NumVertices, UINT startIndex, UINT primCount) override {
    STUB_ONCE("DrawIndexedPrimitive");
    return D3D_OK;
  }
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
    STUB_ONCE("CreateVertexDeclaration");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE
  SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) override {
    if (ppDecl)
      *ppDecl = nullptr;
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
    STUB_ONCE("CreateVertexShader");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE
  SetVertexShader(IDirect3DVertexShader9 *pShader) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetVertexShader(IDirect3DVertexShader9 **ppShader) override {
    if (ppShader)
      *ppShader = nullptr;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(
      UINT StartRegister, const float *pConstantData,
      UINT Vector4fCount) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(
      UINT StartRegister, float *pConstantData, UINT Vector4fCount) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(
      UINT StartRegister, const int *pConstantData,
      UINT Vector4iCount) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(
      UINT StartRegister, int *pConstantData, UINT Vector4iCount) override {
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
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData,
                  UINT *pOffsetInBytes, UINT *pStride) override {
    if (ppStreamData)
      *ppStreamData = nullptr;
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
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetIndices(IDirect3DIndexBuffer9 **ppIndexData) override {
    if (ppIndexData)
      *ppIndexData = nullptr;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  CreatePixelShader(const DWORD *pFunction,
                    IDirect3DPixelShader9 **ppShader) override {
    STUB_ONCE("CreatePixelShader");
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE
  SetPixelShader(IDirect3DPixelShader9 *pShader) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetPixelShader(IDirect3DPixelShader9 **ppShader) override {
    if (ppShader)
      *ppShader = nullptr;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(
      UINT StartRegister, const float *pConstantData,
      UINT Vector4fCount) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(
      UINT StartRegister, float *pConstantData, UINT Vector4fCount) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(
      UINT StartRegister, const int *pConstantData,
      UINT Vector4iCount) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(
      UINT StartRegister, int *pConstantData, UINT Vector4iCount) override {
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

  NSObject_release(pool);
  log_msg("Init: OK");
  return D3D_OK;
}

D9MTDevice::~D9MTDevice() {
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
    if (m_vertexCount) {
      struct wmtcmd_render_setpso setPso = {};
      struct wmtcmd_render_setbuffer setVb = {};
      struct wmtcmd_render_draw draw = {};

      setPso.type = WMTRenderCommandSetPSO;
      setPso.next.set(&setVb);
      setPso.pso = m_pso;

      setVb.type = WMTRenderCommandSetVertexBuffer;
      setVb.next.set(&draw);
      setVb.buffer = m_vbuf;
      setVb.offset = 0;
      setVb.index = 0;

      draw.type = WMTRenderCommandDraw;
      draw.next.set(nullptr);
      draw.primitive_type = WMTPrimitiveTypeTriangle;
      draw.vertex_start = 0;
      draw.vertex_count = m_vertexCount;
      draw.instance_count = 1;
      draw.base_instance = 0;

      MTLRenderCommandEncoder_encodeCommands(
          enc, (const struct wmtcmd_base *)&setPso);
    }
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
