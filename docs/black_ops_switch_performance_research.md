# Black Ops (2010) — Switch Homebrew Performance Research

**Target:** Xbox 360 version of *Call of Duty: Black Ops*, Zombies / Kino der Toten  
**Date:** 16 October 2026  
**Priority:** stable frame time first, image quality second  
**Backend:** static PowerPC recompilation to ARM64; Xenos PM4 translation to Vulkan on a Horizon port of Mesa NVK; no official Nintendo SDK or NVN

## Executive summary

The measurements supplied for this project point to a **dual bottleneck**, not a purely GPU-bound game: roughly **20–50 ms/frame** is spent translating and recording **500–1,800 draws**, while GPU time is reported at **25–40 ms/frame**. A 20 FPS target permits 50 ms per frame. These CPU and GPU figures overlap in a pipelined renderer, so do not simply add them; instead, establish which stage is on the critical path during each stutter. The 12–15 FPS scenes correspond to 66.7–83.3 ms per displayed frame and need a significant worst-case reduction, not just a higher average.

There are **no trustworthy public measurements** I could find for Vulkan per-draw cost on a Tegra X1 running an NVK port on Horizon, nor for this game’s PM4 translator. Therefore, gains below are engineering estimates and hypotheses to test—not benchmark claims. Commercial Switch-port analysis is useful for resolution/effects trade-offs, but it does not tell us what this particular homebrew renderer will gain.

### Ranked experiments at a glance

| Rank | Technique | Why it ranks here | Expected result for this port (estimate, not a published benchmark) | Effort / risk |
|---:|---|---|---|---|
| 1 | Cache decoded PM4 state and avoid redundant per-draw Vulkan state work; batch only compatible consecutive draws | Directly attacks the measured 20–50 ms render/translation stage without altering art | Potentially several ms/frame if substantial parsing, state-diff, descriptor, or API work is redundant; measure before assigning a percentage | Medium; medium correctness risk |
| 2 | Fix high-level draw-list expansion—especially shadow-related work—before Vulkan recording | The project reports roughly 2× draw calls when the game shadow system is enabled even when shadow-map drawing is skipped | Can nearly halve shadow-system-generated calls versus that mode if upstream submission is removed; exact current gain depends on which mode is active | Medium–high; high visual/logic risk |
| 3 | Batch / instance genuinely repeated geometry and particles where state and semantics match | Avoids repeated API and state setup; strongest where the stream exposes repeated meshes/materials | Potentially large in suitable clusters; no valid global percentage without a draw-state histogram | Medium–high; high compatibility risk |
| 4 | Add adaptive resolution and lower-resolution effect buffers with a stable controller | GPU time is already 25–40 ms; Switch ports demonstrate adaptive resolution and quarter-resolution alpha/DOF buffers | A move from 960×544 to 800×450 reduces pixels by **31%** (not 50%); 640×360 reduces pixels by **56%**. Actual GPU-time savings depend on pixel-bound work | Low–medium; visible image-quality cost |
| 5 | Optimize render-target resolve/store/barrier behavior and transient attachments | Xenos EDRAM behavior is being mapped to a different memory/resolve model; redundant resolves can add bandwidth and pass boundaries | Unknown until per-pass timings and resolve counts are recorded; could help both bandwidth and frame time if redundant operations exist | Medium–high; high correctness risk |
| 6 | Remove redundant descriptor/UBO updates and use push constants for small per-draw data when appropriate | Mesa already has relevant CB0/push-constant improvements; state updates can be costly at high draw counts | Likely incremental unless current code rewrites the same data repeatedly; no Switch-specific µs/draw figure is public | Low–medium; low–medium risk |
| 7 | Rebalance Horizon scheduling and eliminate avoidable synchronization/oversubscription | Three usable A57 cores are saturated; busy-wait regressions were already found and partly fixed | Could improve worst-case jitter and free CPU time; no credible generic numeric gain | Medium; starvation/deadlock risk if priorities are wrong |
| 8 | Tune static recompilation hot paths and inspect generated AArch64 | Translated code can waste cycles on state materialization, helper dispatch, endian conversion, and scalarized VMX operations | Workload-specific; no comparable public ReXGlue/XenonRecomp benchmark provides a transferable percentage | Medium–high; high correctness risk |
| 9 | Evaluate deko3d with a minimal renderer microbenchmark before considering a port | Its command capture/replay and low-level API are relevant, but there are no comparable published per-draw benchmarks against this NVK build | Unknown; do not assume it will be faster without measuring equivalent workloads | Very high migration cost |
| 10 | Tune frames-in-flight and present timing for latency and consistency | Makes 20 FPS feel less uneven but does not make expensive frames render faster | Improves pacing/latency trade-off, not raw throughput | Low–medium; low risk |

The ranking assumes the reported CPU renderer cost includes the translation/recording work described. It does **not** assume that `vkCmdDrawMultiEXT` or any other single Vulkan extension can erase the guest’s original draw processing cost.

---

## 1. CPU cost per draw: target the translation boundary, not only the Vulkan call

### 1.1 Cache PM4-derived state and only emit state changes that are actually needed

**Method.** Treat the Xenos command stream translator as a state machine. Keep a canonical shadow of the effective graphics state—pipeline/shader variant, vertex/index-buffer addresses and ranges, viewport/scissor, blend/depth/stencil state, descriptor/resource bindings, render target, constants, and relevant raster state. When a PM4 packet changes one item, mark only that item dirty. Before a draw, compare the desired state with the state already emitted into the current Vulkan command buffer and emit only necessary changes. Keep the packet decoder fast and allocation-free: pre-sized arrays/ring storage, no hash-map lookup in the common path if IDs can be indexed directly, and no per-draw heap allocation.

The important distinction is between **guest draw parsing** and **host Vulkan commands**. Batching one host API call does not remove the need to interpret the original PM4 packets. If the renderer spends much of its 25–40 µs/draw recomputing unchanged state, state-diff caching can target the right cost directly. If the time is mostly command decoding or validation in the Vulkan implementation, a state cache alone will not be enough.

**Measured fact:** this project reports 20–50 ms/frame spent translating/recording 500–1,800 draws, or approximately 25–40 µs/draw. No external benchmark establishes how much of that is PM4 decoding, state derivation, descriptor writes, `vkCmd*` calls, or memory allocation.

**Estimate:** several milliseconds per frame is plausible if the translator repeatedly re-derives state and issues redundant bindings; there is no defensible exact percentage until each stage is timed. Record separate timers for PM4 decode, state diff, descriptor/constant updates, draw-command emission, and command-buffer submission.

**Implementation notes / safety conditions:**

- Start with **consecutive compatible draws** so no reordering changes blending, depth, stencil, query, or render-target semantics.
- Track both logical Xenos state and emitted Vulkan state. Render-target changes, resolves, aliases, and barriers must invalidate the relevant cache.
- Avoid caching host handles by pointer alone if guest virtual addresses can be remapped or object lifetimes change.
- Use counters for “state change requested” versus “host command emitted” to prove the cache is eliminating real work.

**Sources:** [ReXGlue code-generation overview](https://github.com/rexglue/rexglue-sdk/wiki/Codegen-Pipeline-Overview); [Vulkan multi-draw explainer](https://techoverflow.net/2026/09/29/vk-ext-multi-draw-explained-vkcmddrawmultiext-batched-draw-calls/). These are implementation references, not Tegra/NVK benchmarks.

### 1.2 Consider `VK_EXT_multi_draw`, but only after checking extension support and stream compatibility

`VK_EXT_multi_draw` allows a number of logically separate draw operations to be described in one Vulkan command. Its best fit is a run of draws that share compatible pipeline/bindings and can use per-draw indexing such as `gl_DrawID` into a table of transforms/material data. It is **not equivalent to arbitrary mesh merging**: different pipeline state, descriptor sets, index/vertex bindings, resource hazards, blend order, and render-target boundaries can still prevent batching. Nor does the extension guarantee that the driver turns the batch into a single hardware draw; the benefit is primarily avoiding repeated host API/validation overhead.

**Measured fact:** the public TechOverflow article uses an illustrative 500-object example, not an independently controlled benchmark on Switch. No verified published result gives the per-draw CPU savings for NVK on Tegra X1/Horizon. Query the actual device’s extension list and test the exact NVK fork; desktop upstream extension support or Vulkan 1.4 conformance alone does not prove it is exposed in your build.

**Estimate:** the upside is potentially meaningful for batches of many compatible objects, but lower than “1000 calls become one, therefore 99.9% saved” suggests. Your PM4 traversal still sees each original draw unless the translator can also skip repeated parsing and state processing. Prototype the feature behind a runtime switch and compare renderer CPU time, not just `vkCmd*` call counts.

**Effort / risk:** medium. Requires shader/data-path changes and robust equivalence checks. Keep the normal per-draw path as fallback.

**Source:** [VK_EXT_multi_draw explanation and example](https://techoverflow.net/2026/09/29/vk-ext-multi-draw-explained-vkcmddrawmultiext-batched-draw-calls/).

### 1.3 Reuse static command sequences where the stream really is static

Vulkan secondary command buffers or reusable command buffers can help when a group of commands is truly stable across frames and is compatible with the current render-pass/dynamic-rendering context. A likely candidate is immutable environment geometry, but in a PM4-translation renderer a seemingly static draw may still depend on dynamic constants, dynamic descriptors, resource residency, or resolve ordering. Split the recording path into immutable command sequences and per-frame state only after proving the boundary is stable.

Deko3d makes this idea explicit: its [`Primer.md`](https://github.com/devkitPro/deko3d/blob/master/Primer.md) documents reusable command lists and a capture/replay mechanism that records raw GPU commands from supported `dkCmdBuf*` operations. The same file warns that capture has restrictions, that command buffers are externally synchronized, and that replayed commands are version-sensitive. This is a useful architecture reference, **not proof that Vulkan secondary command buffers are automatically faster** on NVK.

**Estimate:** modest-to-large win only if a substantial chunk of the world command stream remains identical across frames. Benchmark CPU recording cost with and without reuse; do not assume GPU execution cost changes.

**Effort / risk:** high; medium–high risk from stale resources, synchronization, and hidden state changes.

---

## 2. Draw-count reduction without changing the art

### 2.1 Stop unwanted draw-list generation before filtering the Vulkan work

The project reports that enabling the game’s shadow system approximately doubles submitted draw calls even when shadow-map rendering itself is skipped. That suggests the shadow path can still generate substantial CPU-side submission/translation work before the renderer discards its output.

**Technique:** identify the guest-side function or PM4 pass boundary responsible for building/submitting shadow-caster work. If shadows are disabled for a run, prevent creation/dispatch of that shadow work as early as safely possible, rather than only suppressing the final map draw. Preserve any shadow-related state required by later game logic. If fully disabling the game path is unsafe, add a pass-level skip before per-draw decoding and maintain the expected guest command-stream state.

**Measured fact:** the approximate 2× draw-call expansion is from this project’s own observations. It is not a published benchmark from another title. Because shadows have already been reduced/disabled in current testing, the incremental benefit depends on whether shadow work is still being generated in the active configuration.

**Expected gain:** relative to a mode where the shadow system doubles all draw calls, removing that extra submission could eliminate a large fraction of CPU work for that mode. Do not claim a universal 50% saving: real call counts and non-shadow costs need a before/after capture.

**Effort / risk:** medium–high. Shadow state may affect lighting or later passes even when maps aren’t rendered; test Kino’s doors, windows, fire, power-ups, and hounds, not only a quiet spawn area.

### 2.2 Merge geometry or instance repeated objects only when draw semantics match

At the renderer translation layer, safe batching candidates are runs with the same render target, shaders/pipeline, descriptor/material state, compatible vertex/index data, and ordering semantics. For repeated static props, an offline mesh merge by material/region can reduce draws further, but that requires asset/object-level information that a raw PM4 stream may not expose. Instancing is especially promising for repeated particle billboards, identical debris, or repeated static models—but only if each instance’s transform and any per-object parameters can be represented in instance data without changing shader behavior.

**Good first targets:** repeated particle billboards and same-material static meshes; next, shadow-caster batches if the game’s shadow path is later re-enabled. **Bad first targets:** blended alpha geometry that depends on exact order, draws around render-target resolves, or objects whose Xbox draw semantics rely on distinct constants or vertex fetch behavior.

**Fact vs estimate:** batching is a standard technique, but no public benchmark gives expected Switch/NVK savings for this Black Ops command stream. Gather a histogram of consecutive runs by pipeline, material/descriptor key, vertex/index buffer, render target, blend mode, and draw parameters. The size of those runs tells you whether batching has enough opportunity to matter.

**Sources:** [Vulkan multi-draw explainer](https://techoverflow.net/2026/09/29/vk-ext-multi-draw-explained-vkcmddrawmultiext-batched-draw-calls/); [deko3d command-buffer/capture documentation](https://github.com/devkitPro/deko3d/blob/master/Primer.md).

### 2.3 Cull only when the translator has enough information to prove a draw invisible

A cheap **conservative CPU cull** can skip draws with zero vertex/index count, empty scissor/viewport, fully masked color writes where depth/stencil also cannot affect later work, or other explicitly proven no-op parameters. Your existing Mesa patch skips the draw macro for zero parameters; the next step is to test whether equivalent no-op cases can be rejected earlier at the translator boundary without violating query, occlusion, or pipeline-statistics semantics.

Frustum/portal culling requires bounds and object identity. Those may not be recoverable from individual PM4 draws, so do not add a generalized CPU culler without a reliable source of per-object bounds. GPU occlusion queries also have costs: query writes/readback, latency, stalls if results are consumed synchronously, and extra pass complexity. If trying occlusion, use delayed results and only test it against a workload that is demonstrably GPU/overdraw bound. It is unlikely to be the first fix for a renderer thread already spending 20–50 ms/frame.

**Effort / risk:** low for provable no-op states; high for recovered bounds or occlusion. Risk can be high if queries, depth/stencil, or blend side effects make an apparently invisible draw observable.

---

## 3. GPU work on Tegra X1: resolution, effects, resolves, and bandwidth

### 3.1 Use a frame-time-driven dynamic-resolution controller

Commercial Switch ports show that dynamic resolution is a practical way to trade pixel count for consistency:

- **DOOM (Switch):** Digital Foundry’s analysis reports adaptive resolution, commonly around 600p docked and roughly 576p or lower handheld; depth-of-field and alpha effects appear to use quarter-resolution buffers. The analysis also notes frame-pacing issues and drops when enemy counts rise. [Summary of Digital Foundry analysis](https://nintendoeverything.com/doom-switch-tech-analysis/).
- **Wolfenstein II (Switch):** reported resolutions can fall as low as 640×360, with common portable samples around 768×432 or 640×360. [Technical analysis summary](https://nintendoeverything.com/wolfenstein-ii-switch-technical-analysis/).
- **Crysis Remastered (Switch):** dynamic resolution plus temporal upscaling; a reported 720p average, with busy scenes reaching 540p or lower and a broad handheld range. [Technical analysis summary](https://nintendoeverything.com/crysis-remastered-switch-tech-analysis/).
- **BioShock: The Collection (Switch):** generally stable 30 FPS with dynamic 1080p docked and 720p portable targets, depending on the game. [Technical analysis summary](https://nintendoeverything.com/bioshock-the-collection-switch-tech-analysis/).
- **The Witcher 3 (Switch):** dynamic 720p docked and 540p portable targets, with documented drops below target during demanding scenes. [Technical analysis summary](https://nintendoeverything.com/the-witcher-3-switch-technical-analysis/).

These are **observed retail-port behaviors**, not measurements of the NVK port. Some ports changed assets, effects, LOD, or rendering architecture; do not assume resolution scaling alone replicates their frame rates.

**Pixel-count math for the current 960×544 target:**

| Internal resolution | Pixels | Change vs. 960×544 |
|---|---:|---:|
| 960×544 | 522,240 | baseline |
| 800×450 | 360,000 | 31.1% fewer pixels |
| 768×432 | 331,776 | 36.5% fewer pixels |
| 640×360 | 230,400 | 55.9% fewer pixels |

The pixel-count reduction is not identical to GPU-time reduction: vertex processing, fixed overhead, bandwidth, resolves, and CPU draw submission remain. The earlier temptation to describe 800×450 as “half the pixels” would be incorrect; it retains about 69% of the baseline pixel count.

**Controller suggestion:** sample GPU frame time, not just average FPS. Use a target budget (for example, leave headroom below the 50 ms deadline for a 20 FPS display cadence), lower resolution in small steps when GPU time exceeds the budget over a short rolling window, and raise it slowly after several stable frames. Clamp minimum/maximum scale, rate-limit changes to reduce shimmer, and avoid oscillating resolution in response to one unusually expensive frame. If a scene is CPU-bound, resolution changes may have little effect; keep CPU and GPU timings separate.

**Effort / risk:** low–medium. Good first experiment because it is easy to A/B; image softness and temporal artifacts are the main cost.

### 3.2 Render expensive alpha effects and selected post effects at reduced resolution

DOOM’s Switch implementation is a useful precedent for rendering some depth-of-field and alpha-effect buffers at quarter resolution. Crysis Remastered also reduces certain shadow-map settings in portable mode. For Kino’s busy hounds/power-up scenes, test quarter- or half-resolution particle/effect rendering, then composite at main resolution. The art remains present, though edges and transparency can appear softer. Start with effect categories that are pixel/overdraw-heavy rather than globally deleting effects.

The *Call of Duty: Ghosts* Wii U coverage reported that more detailed maps and alpha effects/explosions harmed performance more visibly on Wii U than on PS3/Xbox 360, which is consistent with checking these effects in this port; it does not give a transferable millisecond saving. [Report summary](https://www.nintendolife.com/news/2013/11/call_of_duty_ghosts_wii_u_performance_reportedly_drops_below_ps3_and_360_standards).

**Fact vs estimate:** a lower-resolution effect buffer reduces the number of shaded pixels; the actual gain depends on overdraw and effect cost. No public source measures this exact scene on NVK. Keep a per-effect GPU timestamp budget and compare the hounds round and many-zombie cases.

### 3.3 Audit EDRAM resolves, attachment lifetimes, and barriers

The Xbox 360 Xenos render-target/EDRAM model and Switch’s Maxwell memory path are not identical. A translation layer must preserve guest-visible resolves and resource dependencies, but it should not materialize or resolve an intermediate that is never observed later. Audit every resolve/copy and render-target switch:

1. Tag each guest render target with its last writer, later readers, format/sample count, and whether a resolve result is actually consumed.
2. Distinguish required guest-visible resolves from translator-generated copies used only to emulate an internal surface layout.
3. Reuse compatible Vulkan images when lifetimes do not overlap; use appropriate load/store behavior for attachments whose previous contents are irrelevant.
4. Avoid global barriers when a narrower dependency is correct. Combine transitions only where API and guest ordering semantics allow it.
5. Verify MSAA sample counts/resolves against the guest stream before changing them. Disabling MSAA might buy bandwidth but changes image quality and potentially behavior.

**Measured fact:** the user reports 25–40 ms GPU time; no public benchmark establishes how much is memory bandwidth versus fragment shading, pass transitions, or synchronization. Add timestamp queries around each pass and log render-target switch/resolve counts. If supported by the port, use GPU timestamps for elapsed time and separate CPU timers for recording/submission. Mesa 26.2 added non-graphics timestamp support in NVK, but whether that helps this Horizon fork depends on what was ported and exposed. [Mesa 26.2 notes](https://docs.mesa3d.org/relnotes/26.2.0.html).

**Effort / risk:** medium–high. Potentially high payoff if the translator performs redundant copies; high correctness risk because incorrect load/store or barrier decisions create corruption that may show only later in gameplay.

---

## 4. Mesa NVK and descriptor/constant-update considerations

### Relevant Mesa changes to verify in your actual fork

- **Mesa 24.2 release notes:** NVK “use inline constant buffer updates for CB0” and “only write draw parameters to CB0 when they change.” These are relevant to command streams that repeatedly update small draw constants. [Release-note source](https://android.googlesource.com/platform/external/mesa3d/+/refs/heads/main/docs/relnotes/24.2.0.rst).
- **Mesa 25.0:** NVK increased push-constant space to **256 bytes**. [Mesa 25.0 release notes](https://docs.mesa3d.org/relnotes/25.0.0.html).
- **Mesa 26.1:** includes NVK dynamic-descriptor-layout handling changes and notes that NVK disables large pages for now. [Mesa 26.1 release notes](https://docs.mesa3d.org/relnotes/26.1.0.html).
- **Mesa 26.2:** among many changes, includes NVK command/update-buffer, timestamp, image-copy/meta-operation, and mesh/task-shader work. Much of the mesh/task work is aimed at newer GPUs; Tegra X1 Maxwell is pre-Turing and cannot use those mesh-shader capabilities. [Mesa 26.2 release notes](https://docs.mesa3d.org/relnotes/26.2.0.html).

**Important caveat:** upstream Mesa release notes are not proof that a custom NVK-on-Horizon port contains the same patches, supports the same extensions, or gets the same performance. The 26.1–26.2 notes do **not** identify a universal per-draw recording optimization with a published Tegra X1 frame-time gain. Prioritize cherry-picking based on a code-path match and measured behavior, not the version number alone.

### Concrete experiments

- Put tiny, truly per-draw constants in push constants where the limits and shader interface permit; use a persistently mapped, aligned ring/UBO for larger data. Avoid allocating or updating a fresh descriptor set for every draw.
- Use descriptor caching keyed by the full resource/sampler/layout identity. Avoid descriptor churn when the same bindings continue to apply.
- Compare “write CB0/UBO every draw” with “update only when contents differ,” and count bytes written and host commands emitted.
- Inspect the port’s NVK macros/command submission path for locks or allocations on each command, but confirm with tracing or timing before assuming contention. There is no reliable public per-draw NVK lock-cost figure for this platform.
- Ensure validation/debug layers are not accidentally active in performance measurements; retain a separate validation run for correctness.

**Expected gain:** low to moderate, unless the current path performs redundant updates per draw. Treat any “1 µs/draw saved” claim as a hypothesis, not as known Mesa performance data.

---

## 5. Horizon scheduling and Cortex-A57 considerations

### Thread priority and affinity

[Switchbrew’s SVC reference](https://switchbrew.org/wiki/SVC) documents that thread priorities are numbered **0–0x3F, with a lower number meaning a higher priority**, and documents core-mask/affinity operations. Do not interpret `0x3B` as “a high priority” or simply assign the render thread an extreme priority. Over-prioritizing renderer/game threads can starve audio, input, OS work, or other game workers and worsen pacing.

Suggested controlled experiment:

1. Pin the render/translation thread to one suitable application core while leaving the OS-managed system core and other app work alone; verify masks against your libnx/Horizon version.
2. Give the renderer a modest priority advantage over noncritical workers, then measure wake-up delay, execution time, missed deadlines, audio stability, and game update jitter.
3. Keep synchronization event-driven (`svcWaitSynchronization`/events) rather than spinning, except for an intentionally bounded and measured spin where the producer is guaranteed to run promptly.
4. Test priority/affinity choices in the hounds and high-zombie scenes, not just static menu scenes. Keep the previous profile as a rollback option.

**Measured fact:** this port saw three usable A57 cores saturated, partly by game threads busy-waiting for the Xbox 360’s six hardware threads. Some of the worst behavior has already been fixed. No generic public measurement says a particular Switch priority or affinity mask yields a fixed FPS gain.

### Caches, code layout, and 2 MB pages

A57 cache/TLB behavior can matter in a large static recompile, but “the I-cache is small, so make code compact” is only a hypothesis until instruction-cache and branch behavior are measured. Keep common PM4 decoding and state-diff code hot and contiguous; avoid a deep chain of tiny helpers or virtual dispatch in the per-draw path if generated assembly shows it is expensive. Keep large cold translated functions away from the hot renderer helpers, and inspect instruction-cache misses/branch misses if the available profiling environment exposes them.

Do **not** assume 2 MB mappings will automatically help. Page-size support is a kernel/allocator/driver detail as well as a CPU feature, and Mesa 26.1’s release notes specifically list NVK disabling large pages. Large pages are worth an experiment only if the chosen Horizon memory path actually supports them and TLB misses are a demonstrated bottleneck. [Mesa 26.1 notes](https://docs.mesa3d.org/relnotes/26.1.0.html).

**Effort / risk:** medium. Bad affinity or too-high priority can increase jitter or starve essential threads; large-page experiments can be ineffective or unsupported.

---

## 6. Static PowerPC recompilation: what to inspect in generated ARM64 code

The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and [Xenon-Recomp](https://github.com/nimauria/Xenon-Recomp) document ahead-of-time translation of Xbox 360 PPC code to C++ and a shared runtime. [N64Recomp](https://github.com/N64Recomp/N64Recomp) documents a related static-recompilation pipeline, but its MIPS/N64 results are not direct performance predictions for PowerPC on Switch. I found no reliable public benchmark providing a portable “static recompilation is X% faster after optimization Y” figure for this title or for a comparable X360-to-A57 game.

### High-value checks

- **Guest register materialization:** inspect hot generated functions and look for repeated reads/writes through a guest CPU-state struct. Where semantics allow, keep GPR/FPR values in C++ locals across a basic block and materialize them at required boundaries rather than round-tripping every instruction. Verify exception, call, and memory-alias semantics before changing this.
- **Call/branch dispatch:** reduce generic helper calls for common PPC operations and avoid unnecessary indirect dispatch on hot block transitions. But avoid inlining so aggressively that huge generated functions damage I-cache behavior.
- **Big-endian memory access:** map semantically required swaps to efficient AArch64 byte-reverse operations (`REV` forms) and avoid repeated swaps when data is already represented in guest byte order. Do not eliminate swaps across observable guest-memory boundaries without proving correctness.
- **VMX/AltiVec-to-NEON:** identify hot translated vector routines that become long scalar sequences. NEON can handle many packed integer/floating-point operations, but byte permute, saturation, lane ordering, unaligned access, and PPC NaN/FPSCR behavior need instruction-specific validation. Optimize only measured hot instruction patterns.
- **Floating-point modes:** avoid changing host floating-point environment for every arithmetic operation. Emulate rounding/status behavior at the guest control-register boundaries and use specialized helpers for instructions that truly require different semantics. Any fast-math setting must be tested against deterministic gameplay and physics traces.
- **Compiler output:** compile release code for the Switch AArch64 target, inspect disassembly, and run microbenchmarks over actual hot guest functions. The desktop build’s optimization behavior is not a substitute for the final ARM64 build.

**Fact vs estimate:** the recompiler projects demonstrate the approach and its architecture, not the gain from these changes on this game. The likely win varies by what the generated code currently looks like. Build a per-function hotness/time report before broad codegen changes.

**Effort / risk:** medium–high; correctness risk is high, particularly for FP/VMX semantics.

---

## 7. deko3d versus NVK: make it a measured go/no-go decision

[deko3d](https://github.com/devkitPro/deko3d) is a homebrew low-level graphics API for the Switch’s Tegra X1. Its [`Primer.md`](https://github.com/devkitPro/deko3d/blob/master/Primer.md) documents explicit command buffers, independent command lists that can be reused, lazy queue submission, and command capture/replay. These mechanisms may reduce CPU work when many repeated state structures must be converted into hardware commands. The same documentation describes external synchronization, finite command-buffer backing memory, flush thresholds, and capture restrictions.

**What is not known:** I found no apples-to-apples public benchmark of Vulkan/NVK versus deko3d on the same Switch with roughly 1,000 draw calls/frame, similar shaders, comparable resource updates, and equal GPU work. It is not defensible to claim that deko3d will save a particular number of milliseconds. The APIs differ in resource management, shader compilation/binding, synchronization, and how much work is done in the driver versus the application.

**Recommended evaluation:** implement a minimal test scene that reproduces the port’s measured state-change pattern: same draw count, shader switches, buffer/descriptor updates, render-target changes, and upload sizes. Measure CPU command-generation time, submission time, GPU timestamps, and frame-time variance on the actual console. Keep the rendered GPU work as identical as practical. Only consider a full renderer migration if the microbenchmark demonstrates a material advantage that the project cannot obtain by fixing state caching and recording in NVK.

**Homebrew status:** deko3d is an option within the homebrew toolchain. It is not the official NVN API, and using it still means implementing and validating the custom Xenos-to-native-renderer behavior.

**Effort / risk:** very high for a full port; modest for a small benchmark. Best treated as a benchmark project, not a speculative rewrite.

---

## 8. Frame pacing and input latency at 20–30 FPS

The project’s 20 FPS cap maps cleanly to three refresh intervals on a 60 Hz display; 30 FPS maps to two. That is useful for consistent pacing, but a capped renderer still needs its slowest stage to meet the chosen interval frequently enough. A frame that misses the 50 ms budget can still be displayed after a longer interval, which players perceive as a hitch.

**Test one versus two frames in flight.** One queued frame tends to reduce input-to-display latency but can reduce CPU/GPU overlap and hurt throughput when the renderer and GPU both need appreciable time. Two frames in flight may keep the GPU fed but can add latency because the GPU is working on an older input state. With a 25–40 ms GPU and 20–50 ms CPU render stage, neither option is universally best; measure both in the same busy scene. Avoid several frames of queued work unless measurements prove it is necessary.

Specific tests:

- Sample input as late as the game’s simulation architecture safely permits; do not render a frame from unnecessarily stale input.
- Use present/acquire timing and GPU timestamps to determine whether waits happen before recording, during submission, or at present.
- Track frame intervals (not only average FPS): median, 95th/99th percentile, longest hitch, and number of 2×/3× vblank intervals.
- Separate the 20 FPS limiter from GPU/CPU stalls. Ensure a slow frame does not cause catch-up logic or sleep overshoot that makes the next frame even later.
- Compare stable 20 FPS with occasional 30 FPS drops only if the display/present path supports that cadence cleanly. Prefer consistent 20 FPS to a nominally higher average with repeated missed intervals.

**Measured fact:** the port currently targets 20 FPS using three vblanks. No source found provides Switch-homebrew measurements of the precise best frames-in-flight count for this workload. This is a project-specific trade-off, not a universal “always use one” or “always use two” rule.

---

## 9. What the console Call of Duty ports tell us—and do not tell us

- **Black Ops on Wii:** the Wii build is a substantially different port with reduced content/features; secondary coverage notes it had only one Zombies map. It is useful as evidence that Treyarch/n-Space adapted content to the hardware, but it is not a performance benchmark or a source for Xbox 360 renderer behavior. [Review summary](https://gamefaqs.gamespot.com/wii/996443-call-of-duty-black-ops/reviews/153059).
- **DS Call of Duty games:** the DS versions use a very different hardware/software target and are not useful for estimating Vulkan draw costs or Xenos PM4 translation. They may illustrate content adaptation, not a transferable rendering optimization.
- **Call of Duty: Ghosts on Wii U:** contemporary reporting on Digital Foundry’s analysis states that more detailed maps and alpha/explosion effects made performance less consistent than on PS3/360. This supports measuring transparent effects, particles, and busy maps, but the available report does not quantify the benefit of an individual optimization and Ghosts is not Black Ops’ exact engine/configuration. [Nintendo Life report](https://www.nintendolife.com/news/2013/11/call_of_duty_ghosts_wii_u_performance_reportedly_drops_below_ps3_and_360_standards).
- **Dvars:** I did not find a reliable public, title-specific table of Xbox 360 Black Ops dvars that quantifies draw-call or CPU savings for Kino der Toten. Do not blindly transplant PC dvars or names from other IW-engine games; names, build availability, and behavior differ. If the game build exposes the relevant dvars, log the effective values and validate their influence in a controlled A/B run. The strongest known title-specific evidence in this project is its own observation that the shadow system expands draw submission even when map rendering is skipped.
- **Wolfenstein II, DOOM, Crysis Remastered, BioShock, Witcher 3, Borderlands, and Dark Souls retail Switch ports:** these are useful as examples of adaptive resolution, lower-resolution effect buffers, LOD/asset compromises, and performance targets. Retail ports use proprietary APIs and custom engine work; their frame-rate numbers do not measure NVK, Horizon scheduling, or this command translator. In particular, **do not infer per-draw Vulkan CPU cost from a retail game’s FPS.**

---

## 10. Homebrew-compatible options versus official SDK background

All immediate recommendations in this report—PM4-state caching, batch/instance compatible draws, upstream shadow-submission suppression, resolution scaling, effect-resolution changes, resolve/barrier auditing, descriptor/push-constant tuning, Horizon SVC thread tuning, recompiler codegen work, and a deko3d prototype—are **potentially applicable to homebrew**. Actual availability depends on the APIs and features exposed by the ported runtime/driver.

Official Nintendo SDK/NVN measurements can be useful as background about how a commercial Switch renderer behaves, but they are not directly actionable in a libnx/NVK build. No recommendation here requires NVN. Conversely, the fact that NVK supports an upstream Vulkan feature does not prove the Horizon port exposes or implements it in the same way.

---

## 11. First five actions I would try

1. **Instrument the per-draw path and identify the largest slice of the 25–40 µs.** Add counters/timers for PM4 decode, state diff, resource/descriptor updates, Vulkan state commands, actual draw commands, and submit/present waits. This decides whether the first fix is decoder optimization, state caching, descriptor churn, or driver work.
2. **Add dirty-state tracking and skip redundant binds/constant writes.** It is a lower-risk way to attack the measured CPU bottleneck and will also expose whether descriptor/UBO updates are repeated unnecessarily. Prove the win with emitted-command counters and busy-scene timing.
3. **Move shadow-system suppression upstream, if the current build still generates its draw lists.** The observed 2× expansion is unusually concrete evidence of avoidable work, but test correctness and whether the current shadow-off configuration already removes it.
4. **Run controlled dynamic-resolution A/Bs at 960×544, 800×450, 768×432, and 640×360, then test reduced-resolution alpha/effect buffers.** Log GPU time and worst-case frame intervals in the hounds round, power-up effects, and large zombie waves. Keep whichever scale actually reduces GPU time enough to improve consistency.
5. **Compare one versus two frames in flight while applying modest, measured Horizon priority/affinity changes.** Track missed 50 ms deadlines, input-to-present latency, and audio/input stability. Avoid extreme priorities and do not assume a single queueing policy is best for all scenes.

After those experiments, pursue multi-draw/instancing only where the capture shows long compatible runs; audit resolves if GPU timings suggest pass/bandwidth costs; optimize PPC-to-AArch64 code only after function-level profiling; and evaluate deko3d with a microbenchmark before investing in a rewrite.

---

## Source links

### Primary / project documentation

- [ReXGlue SDK — Xbox 360 recompilation runtime/toolkit](https://github.com/rexglue/rexglue-sdk)
- [ReXGlue code-generation pipeline](https://github.com/rexglue/rexglue-sdk/wiki/Codegen-Pipeline-Overview)
- [Xenon-Recomp](https://github.com/nimauria/Xenon-Recomp)
- [N64Recomp](https://github.com/N64Recomp/N64Recomp)
- [deko3d repository](https://github.com/devkitPro/deko3d)
- [deko3d Primer: command buffers, queue behavior, capture/replay](https://github.com/devkitPro/deko3d/blob/master/Primer.md)
- [Switchbrew SVC reference](https://switchbrew.org/wiki/SVC)
- [Mesa NVK driver documentation](https://docs.mesa3d.org/drivers/nvk.html)
- [Mesa 24.2 release notes](https://android.googlesource.com/platform/external/mesa3d/+/refs/heads/main/docs/relnotes/24.2.0.rst)
- [Mesa 25.0 release notes](https://docs.mesa3d.org/relnotes/25.0.0.html)
- [Mesa 26.1 release notes](https://docs.mesa3d.org/relnotes/26.1.0.html)
- [Mesa 26.2 release notes](https://docs.mesa3d.org/relnotes/26.2.0.html)
- [Vulkan multi-draw explainer](https://techoverflow.net/2026/09/29/vk-ext-multi-draw-explained-vkcmddrawmultiext-batched-draw-calls/)

### Retail-port technical analyses / supporting coverage

- [DOOM Switch technical analysis summary (Digital Foundry)](https://nintendoeverything.com/doom-switch-tech-analysis/)
- [Wolfenstein II Switch technical analysis summary](https://nintendoeverything.com/wolfenstein-ii-switch-technical-analysis/)
- [Crysis Remastered Switch technical analysis summary](https://nintendoeverything.com/crysis-remastered-switch-tech-analysis/)
- [BioShock: The Collection Switch technical analysis summary](https://nintendoeverything.com/bioshock-the-collection-switch-tech-analysis/)
- [The Witcher 3 Switch technical analysis summary](https://nintendoeverything.com/the-witcher-3-switch-technical-analysis/)
- [Borderlands: Legendary Collection Digital Foundry analysis](https://www.youtube.com/watch?v=-FuSiIdcZRI)
- [Call of Duty: Ghosts Wii U performance report summarizing Digital Foundry observations](https://www.nintendolife.com/news/2013/11/call_of_duty_ghosts_wii_u_performance_reportedly_drops_below_ps3_and_360_standards)
- [Call of Duty: Black Ops Wii review (context on changed content)](https://gamefaqs.gamespot.com/wii/996443-call-of-duty-black-ops/reviews/153059)

## Confidence and limitations

- **High confidence:** the arithmetic for the current frame budget and pixel-count reductions; priority-number direction in Switchbrew; the cited Mesa release-note entries; deko3d’s documented command-list/capture concepts; published Switch-port resolution observations.
- **Medium confidence:** that state caching, upstream draw-list suppression, effect-resolution reduction, and adaptive resolution are the most promising engineering targets, given the measurements in the project description.
- **Low confidence / not publicly measured:** exact milliseconds saved by multi-draw, NVK per-draw locking/UBO/descriptor updates on Horizon, best thread priority/core mask, 2 MB-page benefit, translated PPC optimization gains, and NVK-versus-deko3d performance for an equivalent Black Ops workload. These require testing on the actual port; this report intentionally does not invent benchmarks.

---

## Review notes (bo1-nx, 2026-10-16/17)

Checked against the port's own measurements and console tests. The report marks estimates as estimates and invents no
benchmarks; most of it is general method. Item by item:

- **Instrument the per-draw path by stage (first action 1):** agreed, the most useful item. The console profiler now
  records pc/lr caller pairs; a per-stage stopwatch on the renderer thread is the next step.
- **Dirty-state tracking / skip redundant binds and constant writes (action 2):** largely in place already (setup cache,
  viewport/scissor/blend/bias change checks; NVK's "CB0 only when changed" is in our Mesa pin, d4a00ea0ab3 = Mesa
  26.2.3). Still worth adding requested-versus-emitted counters to prove it.
- **Suppress the shadow system's extra draw lists upstream (action 3):** right diagnosis. With `sm_enable 1` the game
  issues ~2.8x the draws (Mac, Kino: ~905k vs ~326k per 10 s), and ~580k of them are shadow-map caster draws that the
  `min` preset skips, so the draws actually rendered are about the same. It only matters for `min`; the user plays with
  shadows off.
- **Dynamic resolution from 960x544 (action 4):** the premise is wrong. The port renders 1040x608 (960x544 was only a
  plan item), and in a PM4 translator the game's EDRAM-sized targets make dynamic resolution a large job.
- **Frames in flight and priority/affinity (action 5):** frames in flight is still on the checklist, untested. Horizon
  priorities are handled by the SDK (renderer ring thread at 0x2C); the real CPU wins so far came from removing busy
  waits, found with the stack sampler (job workers 50-67 % → 14-20 % of a core each).
- **ZCULL / prepass skipping:** both tried on the console after this report. Skipping the depth prepass cut renderer
  draws from 987 to 559 per frame but raised GPU time from 36 to 42 ms per frame (hidden pixels no longer rejected
  early), so it is off. ZCULL-capable depth targets worked (NVK gave every depth target a plane) but showed no
  measurable gain, and held ZL behaved like rapid tapping while it was on, so it is off too.
- **Heavy effects in busy scenes (section 3.2):** confirmed as the source of the remaining stutters. Frames of 70-199 ms
  average ~1,370 draws against ~860 for an average frame, with no shader, pipeline or texture creation in them. The
  effect-culling dvars are being tested (`bo1_fx_lite`).
- **VK_EXT_multi_draw, instancing, reused command buffers:** low value until a capture shows long runs of
  state-identical consecutive draws, which BO1's stream has shown few of.
- **deko3d:** agreed: benchmark before any rewrite.
- **Corrections:** "Mesa 26.1 disables large pages" concerns NVK's GPU pages, not CPU-side guest memory mapping. Two
  links (the techoverflow multi-draw article and the personal `nimauria/Xenon-Recomp` repository) could not be
  confirmed as primary sources; the main Xbox 360 recompiler families are ReXGlue (used here) and hedge-dev's
  XenonRecomp.
