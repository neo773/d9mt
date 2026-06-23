// d3d9_device — IDirect3DDevice9(Ex) COM surface for the v2 triangle driver.
//
// Implements only the handful of entry points a 2D triangle needs (Clear,
// BeginScene/EndScene, SetFVF, DrawPrimitiveUP, Present, Reset). Every other
// IDirect3DDevice9Ex method is a generated stub (d3d9_device_stubs.inc) so the
// class is concrete. All rendering is delegated to MetalBackend.

#pragma once

#define COBJMACROS
#include <windows.h>
#include <d3d9.h>

#include "../metal/metal_backend.h"

class D9MTDevice final : public IDirect3DDevice9Ex {
public:
  D9MTDevice(IDirect3D9Ex* parent, HWND window, UINT width, UINT height);

  // Builds the Metal backend (device, swapchain layer, pipeline). Must succeed
  // before the device is handed to the application.
  HRESULT initialize();

  // --- IUnknown ---
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** outInterface) override;
  ULONG   STDMETHODCALLTYPE AddRef() override;
  ULONG   STDMETHODCALLTYPE Release() override;

  // --- IDirect3DDevice9 (implemented subset) ---
  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* presentParameters) override;
  HRESULT STDMETHODCALLTYPE BeginScene() override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE EndScene() override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override { m_fvf = fvf; return D3D_OK; }
  HRESULT STDMETHODCALLTYPE Clear(DWORD rectCount, const D3DRECT* rects, DWORD flags,
                                  D3DCOLOR color, float z, DWORD stencil) override;
  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE primitiveType,
                                            UINT primitiveCount,
                                            const void* vertexData,
                                            UINT vertexStride) override;
  HRESULT STDMETHODCALLTYPE Present(const RECT* sourceRect, const RECT* destRect,
                                    HWND destWindowOverride,
                                    const RGNDATA* dirtyRegion) override;

  // Generated benign stubs for every method not implemented above.
  #include "d3d9_device_stubs.inc"

private:
  // The application's screen-space vertex format for D3DFVF_XYZRHW|D3DFVF_DIFFUSE.
  struct ScreenVertex {
    float x, y, z, rhw;
    DWORD color;
  };

  LONG          m_refCount = 1;
  IDirect3D9Ex* m_parent = nullptr;
  HWND          m_window = nullptr;
  UINT          m_width = 0;
  UINT          m_height = 0;
  DWORD         m_fvf = 0;
  D3DCOLOR      m_clearColor = 0;
  uint32_t      m_vertexCount = 0;

  d9mt::metal::MetalBackend m_backend;
};
