// Frontend-side stubs: definitions for a handful of frontend classes/operators
// whose real .cpp homes were dropped from the Metal shim build (they pull dead
// init/d3d8-interop/shader-IO paths we don't run for the one-triangle target).
// Trivial bodies here keep the linker happy without compiling those subsystems.
//
// Distinct from dxvk_shim_stubs.cpp (which covers com_guid exports): this file
// covers *frontend* symbols (D3D9Initializer, the D3D8 bridges, name printers,
// the sm3 IO map, the win32 monitor helpers).

#include <ostream>

#include "d3d9_device.h"
#include "d3d9_initializer.h"
#include "d3d9_bridge.h"
#include "d3d9_on_12.h"

namespace dxvk {

  // ---- D3D9Initializer ----------------------------------------------------
  // Real impl zero-inits buffers/textures via the CS. The triangle path uploads
  // its own vertex data, so init is a no-op. We still need a live m_csChunk so
  // the inline FlushCsChunk() (header) can deref it safely; back it with a
  // private pool owned by this TU rather than reaching into the device.
  static DxvkCsChunkPool g_initChunkPool;

  D3D9Initializer::D3D9Initializer(D3D9DeviceEx* pParent)
  : m_parent(pParent),
    m_csChunk(g_initChunkPool.allocChunk(DxvkCsChunkFlag::SingleUse), &g_initChunkPool) {
  }

  D3D9Initializer::~D3D9Initializer() {
  }

  void D3D9Initializer::FlushCsChunkLocked() {
    // Chunk never gets commands pushed (Init* are no-ops), so nothing to flush.
    m_transferCommands = 0;
  }

  void D3D9Initializer::NotifyContextFlush() {
  }

  void D3D9Initializer::InitBuffer(D3D9CommonBuffer*) {
  }

  void D3D9Initializer::InitTexture(D3D9CommonTexture*, void*) {
  }

  // ---- D3D8 interop bridges -----------------------------------------------
  // D3D8 layer isn't built; these only need to link. Lifetime is owned by the
  // parent D3D9 object, so AddRef/Release forward nothing and QI rejects.
  DxvkD3D8Bridge::DxvkD3D8Bridge(D3D9DeviceEx* pDevice)
  : m_device(pDevice) {
  }

  DxvkD3D8Bridge::~DxvkD3D8Bridge() {
  }

  ULONG STDMETHODCALLTYPE DxvkD3D8Bridge::AddRef() {
    return 1;
  }

  ULONG STDMETHODCALLTYPE DxvkD3D8Bridge::Release() {
    return 1;
  }

  HRESULT STDMETHODCALLTYPE DxvkD3D8Bridge::QueryInterface(REFIID, void** ppvObject) {
    if (ppvObject)
      *ppvObject = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT DxvkD3D8Bridge::UpdateTextureFromBuffer(
      IDirect3DSurface9*, IDirect3DSurface9*, const RECT*, const POINT*) {
    return E_NOTIMPL;
  }

  bool DxvkD3D8Bridge::IsSupportedSurfaceFormat(D3DFORMAT) {
    return true;
  }

  DxvkD3D8InterfaceBridge::DxvkD3D8InterfaceBridge(D3D9InterfaceEx* pObject)
  : m_interface(pObject) {
  }

  DxvkD3D8InterfaceBridge::~DxvkD3D8InterfaceBridge() {
  }

  ULONG STDMETHODCALLTYPE DxvkD3D8InterfaceBridge::AddRef() {
    return 1;
  }

  ULONG STDMETHODCALLTYPE DxvkD3D8InterfaceBridge::Release() {
    return 1;
  }

  HRESULT STDMETHODCALLTYPE DxvkD3D8InterfaceBridge::QueryInterface(REFIID, void** ppvObject) {
    if (ppvObject)
      *ppvObject = nullptr;
    return E_NOINTERFACE;
  }

  void DxvkD3D8InterfaceBridge::EnableD3D8CompatibilityMode() {
  }

  const Config* DxvkD3D8InterfaceBridge::GetConfig() const {
    static const Config s_config;
    return &s_config;
  }

  // ---- Name printers (generated d3d9_names.cpp dropped) -------------------
  std::ostream& operator << (std::ostream& os, D3D9Format e) {
    return os << uint32_t(e);
  }

  std::ostream& operator << (std::ostream& os, D3DRENDERSTATETYPE e) {
    return os << uint32_t(e);
  }

  // ---- D3D9On12 (d3d9_on_12.cpp dropped; 9-on-12 interop not built) --------
  // Device constructs one unconditionally and QI may hand it out. Lifetime is
  // owned by the parent device, so the COM methods are inert.
  D3D9On12::D3D9On12(D3D9DeviceEx* device)
  : m_device(device) {
  }

  HRESULT STDMETHODCALLTYPE D3D9On12::QueryInterface(REFIID, void** object) {
    if (object)
      *object = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE D3D9On12::AddRef() {
    return 1;
  }

  ULONG STDMETHODCALLTYPE D3D9On12::Release() {
    return 1;
  }

  HRESULT STDMETHODCALLTYPE D3D9On12::GetD3D12Device(REFIID, void** object) {
    if (object)
      *object = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE D3D9On12::UnwrapUnderlyingResource(
      IDirect3DResource9*, ID3D12CommandQueue*, REFIID, void** object) {
    if (object)
      *object = nullptr;
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE D3D9On12::ReturnUnderlyingResource(
      IDirect3DResource9*, UINT, UINT64*, ID3D12Fence**) {
    return E_NOTIMPL;
  }

}
