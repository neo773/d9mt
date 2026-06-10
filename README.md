# d9mt

Native D3D9-on-Metal driver for Wine/CrossOver on Apple Silicon. No Vulkan,
no MoltenVK — D3D9 calls go straight to Metal through DXMT's winemetal
bridge.

```
game.exe (32-bit PE)
  -> d3d9.dll        (this project, mingw PE)
  -> winemetal.dll   (DXMT prebuilt, wine builtin)
  -> __wine_unix_call (wow64 thunks)
  -> winemetal.so    (Mach-O) -> Metal -> CAMetalLayer
```

## Status

MVP: colored triangle via `DrawPrimitiveUP` (D3DFVF_XYZRHW|D3DFVF_DIFFUSE),
windowed BGRA8 swapchain. 120 fps, flat frame intervals, verified in a
CrossOver bottle on M1 Max / macOS 26.

Phase B (next): vendor DXVK's dxso translator (SM1-3 -> SPIR-V), then
spirv-cross -> MSL, grafted behind a swappable translate() interface.
Milestone: GTA IV menu.

## Layout

- `src/winemetal.h` — winemetal ABI, copied verbatim from DXMT
- `src/d3d9/d3d9.cpp` — the driver (COM objects, device, present path)
- `src/d3d9/shader.metal` — fixed-function passthrough VS/PS (precompiled
  to metallib at build time, embedded in the dll)
- `test/triangle.c` — win32 + d3d9 test app
- `scripts/fetch-winemetal.sh` — downloads DXMT release binaries into
  `prebuilt/` (gitignored) and generates the import library
- `scripts/build.sh` — builds `build/d3d9.dll` + `build/triangle.exe`
  (needs Xcode CLT for `xcrun metal`, mingw-w64, python3)
- `scripts/run-test.sh` — installs winemetal into CrossOver's wine tree,
  registers the builtin override, launches the triangle test

## Notes

- winemetal exports are cdecl/undecorated; import lib generated from the
  32-bit dll with gendef + dlltool.
- `MTLCommandQueue_commandBuffer` / `MetalLayer_nextDrawable` return
  autoreleased handles — wrap each frame in NSAutoreleasePool.
- `MTLDevice_newLibrary` accepts only precompiled metallib bytes via
  DispatchData (which copies).
- `WMTRenderPipelineInfo.max_tessellation_factor = 0` trips Metal
  validation; use 16.
