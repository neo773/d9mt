# dxvk → Metal shim contract

This directory **replaces** DXVK's Vulkan backend (`../dxvk-ref/`, reference only) with a
minimal Metal-backed shim. Goal: render ONE fixed-function triangle through DXVK's real
D3D9 frontend (`../d3d9/`), stubbing everything not on that path.

## Hard rules

1. **The frontend is unmodified.** Match the exact signatures the frontend calls. When in
   doubt, read the real header in `../dxvk-ref/<same-name>` and the call site in `../d3d9/`.
2. **Keep, don't reimplement:** `../util/*` (incl. `rc/util_rc.h` → `RcObject`, `Rc<T>`),
   `../vulkan/.../vulkan.h` **types/enums** (VkFormat, VkBufferUsageFlags, …). The shim only
   replaces the `Dxvk*` classes, never the Vk enums.
3. **No Vulkan runtime.** No `vkCreate*`, no `vk::InstanceFn`/`DeviceFn` bodies — those are
   empty structs. No SPIR-V. No sm3/dxbc-spirv at runtime.
4. **Metal only via winemetal** (`../third_party/winemetal/winemetal.h`, `obj_handle_t` ABI)
   — never metal-cpp (that is host tooling). POD `wmtcmd` packet chains for draws.
5. Classes the frontend holds in `Rc<>` derive `dxvk::RcObject` (from `../util/rc`).
6. **Self-documenting names, why-comments, ≤800 LOC/file** (V2_ARCHITECTURE §3).

## The Metal backend object

A single `dxvk::D9mtBackend` (in `dxvk_backend.h/.cpp`, foundation) owns the winemetal
`obj_handle_t` device, queue, swapchain layer, the **one hardcoded FF MSL pipeline**
(`triangleVertex`/`triangleFragment`, vertex buffer at index 0, BGRA8), and a shared vertex
arena. `DxvkDevice` owns it; `DxvkContext` records into it; the swapchain presents through it.
Reuse the proven logic in `../d9mt/metal/metal_backend.*` and the embedded
`triangle_metallib.h`.

## Module ownership (each = header + cpp, ≤800 LOC)

| Module | Files | Implements (real) | Stubs |
|---|---|---|---|
| foundation | dxvk_include.h, dxvk_backend.h/.cpp, vk fn shims, dxvk_hash.h(keep) | winemetal device/queue/pipeline/present bridge; opaque shader types | — |
| shader | dxvk_shader.h, dxvk_shader_spirv.h, dxvk_shader_ir.h, dxvk_shader_key.h | opaque `DxvkShader`/`DxvkSpirvShader` (RcObject holding nothing; ctor takes SPIR-V array + info, ignores it) | getCode → empty |
| device | dxvk_instance.h, dxvk_adapter.h, dxvk_device.h, dxvk_device_info.h, dxvk_options.h, dxvk_extension_provider.h | Instance(adapterCount→1, enumAdapters), Adapter(createDevice, handle→0), Device(createContext, createBuffer→Metal, createImage→Metal RT, features→all-false, debugFlags→0, waitForIdle, submitCommandList, presentImage) | caps/format queries → canned |
| resource | dxvk_buffer.h, dxvk_staging.h | DxvkBuffer(mapPtr(off)/mapPtr()/allocateStorage/storage/info), DxvkBufferSlice{void* mapPtr; mapPtr(off); ctor(Rc<DxvkBuffer>,off,len)}, DxvkResourceAllocation; backbuffer DxvkImage = real Metal texture | DxvkImage/View for app textures → stub |
| command | dxvk_context.h, dxvk_cs.h | DxvkCsThread/DxvkCsChunk (store + execute lambdas `(DxvkContext*)` — **synchronous, no thread**); DxvkContext: beginRecording, bindRenderTargets, bindVertexBuffer, setViewports, clearRenderTarget, bindShader<Stage>(ignore), draw(count,VkDrawIndirectCommand*), flushCommandList, synchronizeWsi | drawIndexed, bindIndexBuffer, compute, all else → no-op |
| present | dxvk_swapchain_blitter.h, dxvk_format.h, Presenter shim (in ../wsi or here) | Blitter::present → blit backbuffer→drawable (or draw direct); Presenter::acquireNextImage → winemetal nextDrawable | format feature queries → canned |

## Draw → Metal mapping (command module, critical)

- `DxvkContext::draw(count, VkDrawIndirectCommand* draws)`: read `draws[0].vertexCount/firstVertex`,
  emit `wmtcmd_render_setpso`(FF pipeline) → `wmtcmd_render_setbuffer`(bound VB, index 0) →
  `wmtcmd_render_draw`(triangle, vertexCount). Encode via `MTLRenderCommandEncoder_encodeCommands`.
- `clearRenderTarget` / `bindRenderTargets`: fold into the `WMTRenderPassInfo` (clear color +
  drawable/backbuffer texture) opened at `flushCommandList`/present.
- `bindVertexBuffer(binding, DxvkBufferSlice&&, stride)`: remember the slice's Metal buffer
  handle + offset for binding 0.
- The CS lambdas run synchronously when a chunk is dispatched — `DxvkContext*` is the shim.

## Integration target (phase 1)

`v2/scripts/build-dxvk.sh` compiles frontend + shim + winemetal into `d3d9.dll`; `code.exe`
renders the triangle through the REAL frontend. Compile-first (stub bodies), then wire the
real draw/present.
