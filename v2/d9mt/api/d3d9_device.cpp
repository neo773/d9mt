#include "d3d9_device.h"

D9MTDevice::D9MTDevice(IDirect3D9Ex* parent, HWND window, UINT width, UINT height)
    : m_parent(parent), m_window(window), m_width(width), m_height(height) {}

HRESULT D9MTDevice::initialize() {
  return m_backend.initialize(m_window, m_width, m_height)
             ? D3D_OK
             : D3DERR_NOTAVAILABLE;
}

HRESULT STDMETHODCALLTYPE D9MTDevice::QueryInterface(REFIID riid, void** outInterface) {
  if (!outInterface)
    return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9 ||
      riid == IID_IDirect3DDevice9Ex) {
    *outInterface = this;
    AddRef();
    return S_OK;
  }
  *outInterface = nullptr;
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9MTDevice::AddRef() {
  return InterlockedIncrement(&m_refCount);
}

ULONG STDMETHODCALLTYPE D9MTDevice::Release() {
  LONG remaining = InterlockedDecrement(&m_refCount);
  if (remaining == 0) {
    if (m_parent)
      m_parent->Release();
    delete this;
  }
  return remaining;
}

HRESULT STDMETHODCALLTYPE D9MTDevice::Reset(D3DPRESENT_PARAMETERS* presentParameters) {
  if (presentParameters) {
    if (presentParameters->BackBufferWidth)
      m_width = presentParameters->BackBufferWidth;
    if (presentParameters->BackBufferHeight)
      m_height = presentParameters->BackBufferHeight;
  }
  m_vertexCount = 0;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9MTDevice::Clear(DWORD, const D3DRECT*, DWORD flags,
                                            D3DCOLOR color, float, DWORD) {
  if (flags & D3DCLEAR_TARGET)
    m_clearColor = color;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9MTDevice::DrawPrimitiveUP(D3DPRIMITIVETYPE primitiveType,
                                                      UINT primitiveCount,
                                                      const void* vertexData,
                                                      UINT vertexStride) {
  if (primitiveType != D3DPT_TRIANGLELIST || !vertexData ||
      vertexStride < sizeof(ScreenVertex))
    return D3DERR_INVALIDCALL;

  const UINT vertexCount = primitiveCount * 3;
  if (m_vertexCount + vertexCount > m_backend.vertexCapacity())
    return D3D_OK;  // milestone: silently drop overflow

  // Convert screen-space XYZRHW pixels to NDC straight into the shared buffer.
  const float halfWidth = m_width * 0.5f;
  const float halfHeight = m_height * 0.5f;
  const auto* source = static_cast<const uint8_t*>(vertexData);
  d9mt::metal::TriangleVertex* destination =
      m_backend.vertexUploadBuffer() + m_vertexCount;

  for (UINT i = 0; i < vertexCount; ++i) {
    const auto* vertex =
        reinterpret_cast<const ScreenVertex*>(source + i * vertexStride);
    destination[i].position[0] = vertex->x / halfWidth - 1.0f;
    destination[i].position[1] = 1.0f - vertex->y / halfHeight;
    destination[i].position[2] = vertex->z;
    destination[i].position[3] = 1.0f;
    destination[i].color = vertex->color;
  }
  m_vertexCount += vertexCount;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9MTDevice::Present(const RECT*, const RECT*, HWND,
                                              const RGNDATA*) {
  m_backend.renderAndPresent(m_vertexCount, m_clearColor);
  m_vertexCount = 0;
  return D3D_OK;
}
