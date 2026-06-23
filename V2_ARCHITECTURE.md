# d9mt v2 — Metal-First Architecture Plan

> Status: design proposal. The goal is a **Metal-native** D3D9 translation layer that
> keeps DXVK's hard-won D3D9 **state machine** as a reference but strips every piece of
> **Vulkan plumbing** that costs us CPU (especially under Rosetta) or bends the design
> toward an API we no longer target.

---

## 1. Why v2

v1 is a fork of DXVK's D3D9 front-end with the Vulkan backend swapped for Metal. It
works (GTA IV, 60fps@3K), and this session landed real per-draw wins (argument-buffer
rebuild gate, PSO-lookup memo, dynamic-state early-out, push-block bulk copy). But
profiling (rdtsc micro-probes, `D9MT_TRACE`) showed the remaining per-draw cost is
**structural and inherited from DXVK's Vulkan-shaped plumbing**, not from our Metal code:

- `Rc<DxvkBuffer>` atomic refcount churn (Rosetta amplifies atomics).
- `DxvkObjectTracker::track()` per bound resource — Vulkan lifetime machinery.
- `DxvkStagingBuffer` / `DxvkBufferSlice` Rc-heavy staging.
- A ~300-500 byte Vulkan-shaped pipeline-state key, hashed per dirty draw.
- The deferred command-stream model recording `std::function`/`Rc`-capturing lambdas.
- COM vtable indirection on the draw entry points (Rosetta indirect-branch tax).

None of that is the *D3D9 semantics*. It is the *plumbing wrapped around them*. v2 keeps
the semantics and replaces the plumbing with a Metal-native, allocation-light,
atomic-light design.

### Honest scope and payoff

- The plumbing prize is bounded — estimated **~1-3ms/frame** of CPU. Worth measuring the
  unprofiled app-thread side (`DrawPrimitive → EmitCs → device lock`) before committing.
- The orthogonal **2-3× lever is native arm64** (escaping Rosetta). v2 is a Rosetta-tax
  *trim*; native *removes* Rosetta. They do not conflict — pursue native if/when the wine
  base allows aarch64-unix; pursue v2 for the structural cleanup and maintainability win
  regardless.
- v2 is also a **maintainability** play: v1's `d9mt_context.cpp` is ~5,300 LOC. v2 is a
  chance to enforce real separation of concerns (see §3).

---

## 2. Goals and non-goals

### Goals
1. Keep DXVK's D3D9 state machine as the source of truth for D3D9 semantics.
2. Replace Vulkan plumbing with Metal-native primitives (residency sets, argument
   buffers, gpuAddress, frame-lifetime ownership).
3. Eliminate per-draw atomics, Rc churn, and heap allocation on the hot path.
4. Self-documenting, well-factored code (§3) — no monolith files.
5. Migrate **incrementally and measurably** — each seam is a validated, committed step.

### Non-goals
1. Re-implementing D3D9 state tracking from scratch (months, bug-prone, no payoff).
2. Removing the producer/consumer thread split — it is good design, keep it.
3. Chasing GPU-side optimizations — we are CPU-bound; the GPU is <1ms/frame.
4. Fixing Rosetta — that is the native-arm64 track, separate from v2.

---

## 3. Engineering first principles

These are binding for all v2 code.

### Self-documenting code
- **Descriptive names over comments.** Names state intent: `residencySetForFrame`, not
  `rs`; `argumentBufferSlot`, not `abId`; `pipelineStateKey`, not `key`. A reader should
  understand a function from its signature.
- **Functions describe an action; types describe a thing; booleans read as predicates**
  (`isResident`, `hasDirtyConstants`). No abbreviations unless they are domain-standard
  (`vs`/`fs`/`pso` are fine; invented short forms are not).
- **Comments explain *why*, never *what*.** If a comment restates the code, the code
  needs a better name instead.
- Files are named for their single responsibility (§ separation of concerns).

### Separation of concerns
- **One responsibility per module.** Resource ownership, command recording, command
  execution, pipeline-state caching, residency, shader translation, and the D3D9 API
  surface are *separate* modules with explicit interfaces.
- **No god objects.** v1's `DxvkContext` does state tracking + commit + encode + residency
  + PSO + push data. v2 splits these (see §6).
- **Dependencies point one way:** D3D9 API layer → command recorder → (thread) → command
  executor → Metal backend. No back-references.

### File size and structure
- **Hard ceiling: ~800 LOC per file. Soft target: ~400.** A file over the ceiling is a
  refactor signal, not a style nit. (v1's 5,300-LOC `d9mt_context.cpp` is the anti-pattern
  we are correcting.)
- One primary type per file; the file is named after it (`metal_residency_set.cpp/.h`).
- Free helpers live next to what they serve, not in a `utils.cpp` junk drawer.

### Data-oriented hot path
- Per-draw data is **POD** (plain structs, trivially copyable). No `std::function`, no
  `Rc`, no virtual calls on the draw path.
- Ownership is **frame-scoped**: allocate from arenas, free by GPU-completion fence, never
  per-object refcount.
- Hot loops are **branch-light and atomic-free**; cross-thread sync happens at frame/flush
  boundaries, not per draw or per resource.

---

## 4. Keep / rip boundary

### Keep (reference + reuse from DXVK)
- D3D9 device state tracking: render states, texture-stage states, sampler states, FVF /
  vertex declarations, fixed-function emulation, constant register files, SWVP.
- The dirty-flag *set* logic (which D3D9 call dirties what).
- DXBC → DXSO → MSL shader translation.
- Swapchain / present, occlusion & event queries, clear/blit *semantics*, format mapping,
  `D3DPOOL` behavior.

### Rip (Vulkan plumbing → Metal-native)
| Vulkan plumbing (v1) | Metal-native replacement (v2) | Eliminates |
|---|---|---|
| `Rc<DxvkBuffer/Image>` + atomic refcount | arena/pool ownership, freed by frame fence | per-resource atomics |
| `DxvkObjectTracker::track()` + `markResident` | one persistent `MTLResidencySet`/frame | per-bind lifetime traffic |
| `DxvkStagingBuffer` / `DxvkBufferSlice` | bump allocator → POD `{gpuAddress, ptr, offset}` | slice-construct atomics |
| `DxvkGraphicsPipelineStateInfo` (~300-500B) | compact Metal PSO key + flat-array lookup | per-draw hash/key build |
| `DxvkCsCmd` + `EmitCs` lambdas | direct POD `wmtcmd` packets | `std::function`/`Rc` captures, indirect dispatch |
| `m_descriptorState` Vulkan descriptor bitsets | direct argument-buffer writes | descriptor abstraction |
| virtual COM dispatch on internal draw path | direct calls; COM only at the API boundary | Rosetta indirect-branch tax |

---

## 5. Architectural decisions (made up front)

1. **Producer/consumer thread split: KEEP.** The app thread records POD command packets;
   a single command-stream thread executes them into Metal. This hides latency and matches
   D3D9's single-threaded-state reality. v2's change is *what* gets recorded (POD packets,
   not Rc/lambda), not the topology.
2. **COM at the boundary only.** `IDirect3DDevice9` & friends keep their vtables (the app
   calls them), but the internal draw path is non-virtual direct calls.
3. **Frame-scoped ownership.** Transient resources (constants, dynamic VB/IB, argument
   buffers) live in per-frame arenas reclaimed by a completion fence. Persistent resources
   (textures, static geometry) live in pools with explicit, non-atomic lifetime.
4. **Residency via `MTLResidencySet`.** One set per frame (or persistent + per-frame
   delta), committed once per command buffer — not per-resource `useResource`.
5. **Single source of GPU addresses.** Argument buffers are POD structs of `gpuAddress` /
   `MTLResourceID`, written directly; one `setBuffer` binds the table.

---

## 6. Proposed module layout

Directory `src/d9mt/` (new), grouped by concern. Indicative names; each file ≤ ~800 LOC.

```
src/d9mt/
  api/                      # D3D9 COM surface (thin; forwards to core)
    d3d9_device.cpp/.h          # IDirect3DDevice9 — API only, no Metal
    d3d9_resources.cpp/.h       # textures, buffers, surfaces (D3D9 objects)
    d3d9_swapchain.cpp/.h
    d3d9_state_tracker.cpp/.h   # render/sampler/texture-stage state + dirty SET logic
  shader/
    dxbc_to_msl.cpp/.h          # translation (reuse DXVK reference)
    shader_reflection.cpp/.h    # AB layout, push layout, sampler slots
  command/
    command_packet.h            # POD wmtcmd packet definitions
    command_recorder.cpp/.h     # app-thread: builds packets from D3D9 state
    command_stream.cpp/.h       # producer/consumer queue (POD chunks)
    command_executor.cpp/.h     # CS-thread: packets -> Metal encoder
  metal/
    metal_device.cpp/.h         # MTLDevice/queue ownership
    metal_arena_allocator.cpp/.h# bump allocator over shared MTLBuffer (POD slices)
    metal_resource_pool.cpp/.h  # persistent buffer/texture pools, frame-fence reclaim
    metal_residency_set.cpp/.h  # MTLResidencySet management
    metal_pipeline_cache.cpp/.h # compact PSO key + flat lookup + async compile
    metal_argument_buffer.cpp/.h# POD AB assembly (gpuAddress/resourceID)
  frame/
    frame_lifetime.cpp/.h       # completion fences, per-frame arena reset
  profiling/
    trace.h                     # ported zone mask + rdtsc micro-probe tooling
```

Boundaries: `api/` knows D3D9, not Metal. `command/` knows packets, not D3D9 or Metal
internals. `metal/` knows Metal, not D3D9. Shader translation is isolated. The only place
that touches all three is the thin wiring in `command_executor`.

---

## 7. Phased migration (in-place, validated, committed)

Each phase is independently shippable, in-game validated (boot + a D3D `Reset`), and
measured against the rdtsc baseline. **Do not big-bang.** The phases can land on the
existing fork (de-Vulkanizing in place) and converge toward the §6 layout.

0. **Measure the app-thread side.** Instrument `DrawPrimitive → PrepareDraw → EmitCs →
   device lock` with the rdtsc micro-probe. Confirm the plumbing prize before refactoring.
1. **POD staging.** Replace `DxvkBufferSlice` returns with a POD `{gpuAddress, ptr,
   offset}`; bump allocator stops copying an `Rc`. Kills the `abAlc` atomic.
2. **Residency set.** Replace `track()` + per-bind `useResource` with one
   `MTLResidencySet`/frame. Kills per-bind lifetime traffic.
3. **Frame-scoped resource ownership.** Move transient resources off `Rc` onto arena +
   completion-fence reclaim. (Largest phase; do behind a flag.)
4. **Compact PSO key.** Replace the Vulkan state-info key with a Metal-shaped key + flat
   lookup. Folds in the v1 PSO-memo.
5. **POD command packets.** Replace `EmitCs` lambdas with POD packets end to end; drop
   `std::function`/`Rc` captures and the descriptor-state abstraction.
6. **Devirtualize the internal draw path.** Direct calls; COM only at the API surface.
7. **File split / rename pass.** Land the §6 layout and §3 naming. Retire the monolith.

Phases 1, 2, 4 are low-risk and capture most of the per-draw win. Phases 3, 5, 6 are the
structural heart and the most code. Phase 7 is the maintainability payoff.

---

## 8. Validation discipline (carried over from v1)

The hard lesson from v1: redundancy/dirty-skip changes crash GTA IV's **startup + D3D
`Reset`** path even when they pass static review (see `memory/perf-guards-break-startup-
reset.md`). For every phase:

1. **Profile first** (`D9MT_TRACE` / rdtsc micro-probe) — confirm the seam is worth it.
2. **One seam at a time**, build, measure before/after.
3. **Validate in-game**: clean RELEASE build, full boot (~2 min), AND a resolution/settings
   change to force a `Reset`. Boot-to-menu is insufficient.
4. Prefer **behavior-preserving** changes (identical output, less work) — like the
   PSO-memo and dynState early-out — over changes that alter dirty/Reset logic.

---

## 9. Open questions

- **Does the app-thread D3D9 path actually cost 1-3ms?** Unmeasured. Decides whether v2 is
  worth it for perf (it is worth it for maintainability regardless).
- **Frame-scoped ownership vs DXVK's lifetime guarantees.** Phase 3 must preserve the "in
  flight until GPU done" guarantee `track()` provides — via completion fences. Needs care.
- **Threading model under residency sets.** A persistent `MTLResidencySet` mutated on the
  app thread and committed on the CS thread needs a defined ownership/sync rule.
- **How much DXVK reference code can be copied vs re-derived?** The state machine is huge;
  prefer copying proven logic and de-plumbing it in place over re-writing.
- **Native arm64 interaction.** If native lands, the Rosetta-atomic tax vanishes and some
  v2 phases lose their perf justification (but keep their maintainability value).

---

## 10. Definition of done (per phase) and overall

- **Per phase:** builds clean (RELEASE), in-game validated incl. Reset, rdtsc delta
  recorded in the commit message, file(s) under the LOC ceiling, names self-documenting.
- **Overall v2:** no `Rc`/atomic on the per-draw path; one residency commit/frame; POD
  command packets; compact PSO key; modules per §6; no file over ~800 LOC; the D3D9 state
  machine unchanged in behavior; GTA IV at parity-or-better FPS with v1.
