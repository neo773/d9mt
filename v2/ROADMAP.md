# d9mt v2 — Metal-first Roadmap

Goal: a D3D9→Metal layer that runs real games, built on the **unmodified DXVK D3D9
frontend** but reshaped so the backend is **Metal-resident by design**, not
Vulkan-shaped. Beat v1 on wastage, performance, and maintainability — *provably*,
not by assertion.

**Guiding principle:** never do frontend surgery blind. Get a real workload →
profile → rip Vulkan waste one validated cut at a time, each backed by a measured
delta and a regression target.

---

## Status

- [x] **Triangle milestone** — RGB triangle through the real DXVK frontend +
      SPIR-V→MSL conversion + argument-buffer binding. (commit `0ff82a3`)
- [x] **Stage A** — frame-scoped present: one drawable/encoder per frame,
      present once at `Present()`, per-draw GPU stall gone. (`2aa7fe1`)
- [x] **Stage B** — zero per-draw heap allocations (reused scratch). (`b056014`)
- [x] **Phase 1.1 — programmable SM3 shaders** — ported the real DXVK
      `DxvkIrShader` (dxbc-spirv IR→SPIR-V + binding-model lowering) over the
      stub; `code_shader.exe` renders the RGB triangle. (`2f414cd`)
- [x] **Phase 1.2 — textures + samplers, color-correct** — real MTLTextures +
      upload, bindless sampler heap, residency; and the `depth2d`→`texture2d`
      fix (IR spec-constant fold + a straight-line function inliner + DCE in
      `getCodeSpecialized`) so color samplers return RGBA. `code_texture.exe`
      renders the red/blue checkerboard. (`a03abbd`, `e6db176`)
- [x] **Phase 1.3 — index buffers + drawIndexed** — `code_index.exe`
      renders via `DrawIndexedPrimitive`. (`712c11f`)
- [x] **Phase 1.4 — dynamic state into the PSO** — blend state
      (`code_blend.exe`, olive = SrcAlpha/InvSrcAlpha, `2b49614`) and
      depth/stencil testing (`code_depth.exe`, near-blue occludes far-red,
      `7a901bf`). Minor follow-ups: non-triangle topology classes, RT-format in
      the PSO key (both fixed today: triangle list, BGRA8 backbuffer).
- [x] **Phase 2 — per-frame CPU profiler** — `dxvk_trace.h`, zone breakdown at
      Present when `D9MT_TRACE=1`. (`0d102b8`) Baseline (single-draw test): after
      warmup, per-draw CPU is negligible (resolveShader/buildPipeline/
      bindResources/appendDraw all ~0, caches hit); the Stage-A blocking present
      (GPU wait, ~0.75ms) dominates. Re-measure on a real many-draw frame before
      cutting — that's where bindResources/appendDraw scaling will show.

### Phase 1.2 notes (DONE)

How the texture path works, for reference. The PS samples through a bindless
model: `buffer(0)` = sampler heap (device AB of sampler resource IDs, indexed
by a push-data field); `buffer(2)` id(0) = the texture descriptor (texture
resource ID in the set-2 AB). winemetal: `MTLDevice_newTexture`/`newSamplerState`
(out `gpu_resource_id`), `MTLTexture_replaceRegion` (upload), `useResource`
(residency). The `depth2d`→`texture2d` color fix lives entirely in the IR path
(`DxvkIrShader::getCodeSpecialized`): fold spec constants, inline straight-line
sampler-state helpers, DCE the dead shadow branch — never the convert module.
Follow-up perf: specialize only the sampler-type spec ids (not alpha/fog) to cut
the per-spec-data recompiles the all-spec fold currently causes.

What A+B are NOT: the Vulkan-ectomy. They were the prerequisite + hygiene. The
real Vulkan removal is Phases 3–4 below, deliberately held until there's a real
frame to profile and regression-test against.

---

## Phase 1 — Capability to a real frame *(the gate)*

Can't profile or prove the rip on a 1-draw triangle. Get a real game frame first.
This is the most work but everything downstream depends on it, and it's what makes
"better than v1" provable.

1. **Programmable shaders (sm3)** — wire `d3d9_shader.cpp` vs/ps through the same
   translate→PSO→bind path FF already uses (infra mostly exists; needs bind
   wiring + a programmable-shader test). *Risk: low-med.*
2. **Textures + samplers** — `bindResourceImageView` / `bindResourceSampler` +
   the bindless sampler heap; image AB slots; `useResource` for textures.
   *Risk: med — new resource kind.*
3. **Index buffers + `drawIndexed`** — currently no-op stubs. *Risk: low.*
4. **Dynamic state into the PSO key** — blend, RT format, topology, depth/stencil
   (today the PSO cache keys only on the shader pair). *Risk: med — correctness.*

**Exit:** a real game (L4D2 / GTA IV) renders a frame. Unlocks Phases 2–4.

### Toward the exit — feature coverage beyond items 1–4

A real frame needs more than the four enumerated items. Landed so far:
block-compressed textures (**BC1/2/3 / DXT**, `4231cf4` — the format almost all
game art ships in) + per-mip block-aware upload. Remaining, in rough priority:

- **Render-to-texture** — DONE (`c85b8b7`). One command buffer per frame, a
  render pass per bound target; backbuffers registered via `initImage` route to
  the drawable, others render offscreen; RT textures are Private storage; a
  pending clear is flushed to the current target before a switch. `code_rtt.exe`
  samples a red offscreen RT onto a quad. Current cut assumes full-screen RTs
  (reuse `m_depthTexture`, so the Depth32Float PSO matches); arbitrary RT sizes
  need per-size depth or a target-keyed PSO depth format.
- **Cube textures** (skyboxes/reflections): texture type Cube + 6 faces; the
  IR already bakes the sampler dimensionality from the DCL.
- **Occlusion queries**, multi-RT, more uncompressed formats (with channel
  swizzle for L8/A8/…).

---

## Phase 2 — Instrument *(cheap, mandatory before cutting)*

5. **Per-frame CPU profiler** — port v1's `D9MT_TRACE` zone concept; capture a
   real frame's draw-path CPU breakdown. Without numbers, the Vulkan rip is
   guesswork. *Risk: low (additive).*

---

## Phase 3 — The Vulkan rip *(X′; profile → cut → re-profile → commit)*

Order by measured cost. Stop a cut that doesn't move the needle.

> **Status / gate.** Item 6 (below) is **done** (`59c9e09`). Items 7, 9, 10 are
> the dirty-skip / EmitCs-devirtualize / descriptor-collapse cuts — exactly the
> class of change that broke GTA IV's startup + D3D `Reset` in v1 (see
> `memory/perf-guards-break-startup-reset.md`). They **must** regression-test
> against a real game (Phase 1's exit), and the profiler shows steady-state
> per-draw CPU is already ~0 on the simple tests, so there is nothing to
> measure a win against yet. Holding 7/9/10 until a real game frame exists —
> doing them blind would violate this phase's own discipline. Item 8 (compact
> PSO key) is effectively already in place: the PSO is cached and keyed on the
> shader pair + blend bits, and `buildPipeline` is ~0 steady-state.

6. **C′ — residency once per frame** — winemetal has no `MTLResidencySet`, so
   dedupe `useResource` manually: issue per resource per *frame*, not per draw.
   *Risk: low, backend-only.* **DONE** (`59c9e09`).
7. **D — dirty-gate AB/push + cache vertex layout** — skip rebuilds when bound
   state is unchanged since the last draw (v1 had this gate; v2 dropped it).
   Validatable only against multi-state frames. *Risk: med.*
8. **PSO/state cache hardening** — proper key; stop per-draw PSO churn. *Low-med.*
9. **E — devirtualize the command path** *(big structural win)* — replace the
   `CsThread`/`CsChunk` type-erased lambda recording with POD `wmtcmd` packets +
   direct calls. **Edits the frontend `EmitCs` templates** → highest blast
   radius; behind a flag; last in this phase. *Risk: high, highest leverage.*
10. **Collapse descriptor-set / pipeline-layout indirection → Metal-native bind**
    — kill the Vulkan binding-map dance between frontend slots and Metal buffer
    indices; cut per-draw AB assembly + `Rc<>` atomic churn. *Risk: high.*

---

## Phase 4 — Frontend de-plumb in place *(V2_ARCHITECTURE §7)*

11. Rip dead Vulkan paths out of the frontend's **hot** functions (the ones that
    actually run per-draw — NOT the gc-dropped no-ops), reshape Metal-first.
    Profile-guided, surgical, backed by a working game to regression-test.
    *Risk: high — but now numbers + regression target exist.*

---

## Phase 5 — Maintainability *(V2_ARCHITECTURE §6 / Stage F)*

12. File split, retire the no-op Vulkan stubs (barriers / layout transitions /
    sync — currently empty inlines, gc-dropped, ~0 runtime), rename to the §6
    module layout. Pure cleanliness; do last.

---

## Critical path

`1 → 2 → 3 → 4 → 5`. Phase 1 is the gate. Phases 3–4 are the actual Vulkan
removal and are deliberately *after* instrumentation (so every cut is justified by
a measured delta) and *after* a working game (so there's a regression target).

The compiled-but-dead Vulkan stubs stay untouched until Phase 5 — ripping them
earlier looks productive but buys ~0 runtime and risks link breakage.

---

## Working notes (for the loop)

- Build: `bash v2/scripts/build-dxvk.sh` (parallel + sccache; ~3s warm). Deploy +
  launch: `bash v2/scripts/run.sh`. Self-verify by screenshot (`screencapture -x`)
  + Read the PNG — the triangle window is titled "d9mt triangle".
- Diagnostics land in the bottle: `…/Rockstar Games Launcher/drive_c/d9mt-test/v2.log`.
- Keep the triangle rendering at every step; commit per validated step; push to
  `origin/main` (private repo `neo773/d9mt-v2`).
- Divide work with sub-agents on **disjoint files**; define the header/API seam
  first, then fan out (the Stage A pattern worked well).
- Do NOT touch the SPIR-V→MSL shader conversion (cached after warmup, low
  priority, swappable later).
