// d3d9_entry — the DLL entry point (Direct3DCreate9) and the IDirect3D9(Ex)
// factory. The factory implements only CreateDevice (the rest are generated
// stubs); it constructs and initializes a D9MTDevice. No Metal knowledge here.

// INITGUID (in exactly this one TU) emits the definitions for IID_IDirect3D9,
// IID_IDirect3DDevice9, etc. that the QueryInterface implementations reference.
#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>

#include "d3d9_device.h"

namespace {

  class D9MTInterface final : public IDirect3D9Ex {
  public:
    // --- IUnknown ---
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** outInterface) override {
      if (!outInterface)
        return E_POINTER;
      if (riid == IID_IUnknown || riid == IID_IDirect3D9 ||
          riid == IID_IDirect3D9Ex) {
        *outInterface = this;
        AddRef();
        return S_OK;
      }
      *outInterface = nullptr;
      return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
      return InterlockedIncrement(&m_refCount);
    }
    ULONG STDMETHODCALLTYPE Release() override {
      LONG remaining = InterlockedDecrement(&m_refCount);
      if (remaining == 0)
        delete this;
      return remaining;
    }

    // --- IDirect3D9: the one method a device needs ---
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE deviceType,
                                           HWND focusWindow, DWORD behaviorFlags,
                                           D3DPRESENT_PARAMETERS* presentParameters,
                                           IDirect3DDevice9** returnedDevice) override {
      if (!returnedDevice || !presentParameters)
        return D3DERR_INVALIDCALL;
      *returnedDevice = nullptr;

      HWND window = presentParameters->hDeviceWindow ? presentParameters->hDeviceWindow
                                                     : focusWindow;
      if (!window)
        return D3DERR_INVALIDCALL;

      UINT width = presentParameters->BackBufferWidth;
      UINT height = presentParameters->BackBufferHeight;
      if (!width || !height) {
        RECT clientRect = {};
        GetClientRect(window, &clientRect);
        width = clientRect.right - clientRect.left;
        height = clientRect.bottom - clientRect.top;
      }
      if (!width)  width = 640;
      if (!height) height = 480;

      auto* device = new D9MTDevice(this, window, width, height);
      HRESULT result = device->initialize();
      if (FAILED(result)) {
        device->Release();
        return result;
      }
      AddRef();  // the device holds a reference to its parent
      *returnedDevice = device;
      return D3D_OK;
    }

    // Generated benign stubs for every other IDirect3D9Ex method.
    #include "d3d9_interface_stubs.inc"

  private:
    LONG m_refCount = 1;
  };

}

extern "C" __declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT) {
  return new D9MTInterface();
}
