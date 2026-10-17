# What other Switch ports did, and what BO1 should take from them (2026-10-15)

Read for this: the Xbox 360 static recompilations that share our toolchain (ReXGlue + NVK), plus the other demanding
ports the user listed and the [SwitchPorts](https://github.com/robzilla10001/SwitchPorts) index. Only documents
and code were read; nothing was copied or run. Each item says where it was measured and what it would mean for BO1.

## The ports, and how close they are to us

| Port | Approach | Relevance to BO1 |
|---|---|---|
| [sa2-nx](https://github.com/ChanseyIsTheBest/sa2-nx) (Sonic Adventure 2) | ReXGlue + native Vulkan renderer (masseffect-nx lineage), 60 FPS at stock clocks | **Highest.** `docs/porting-guide.md` is a complete method; `docs/enhancements.md` and `docs/gpu-optimisation-candidates.md` measure every change |
| [futari-nx](https://github.com/ChanseyIsTheBest/futari-nx) (Mushihimesama Futari) | same lineage | **High.** `xbox360.md` is a notebook shared by five ports (Futari, SA2, Banjo, Peggle 2, Sonic Transformed): general optimisations and the ones that broke things |
| [UnleashedRecomp-NX](https://github.com/ChanseyIsTheBest/UnleashedRecomp-NX) (Sonic Unleashed, the heaviest 360 game on the list) | XenonRecomp + D3D-level renderer (not PM4) | **High for GPU ideas.** `docs/SWITCH-PERFORMANCE.md`: 10 measured rounds, every change image-identical and behind a key |
| [the-simpsons-game-nx](https://github.com/R-drg/the-simpsons-game-nx), [defjam-sw](https://github.com/R-drg/defjam-sw) | R-drg, the author of project8-sw (where our renderer comes from) | **High for the driver:** Mesa patches on **our exact Mesa commit `d4a00ea0ab3`** |
| [RaymanOriginsRecomp](https://github.com/XDanfr/RaymanOriginsRecomp) | ReXGlue, D3D-level native renderer, macOS/Android only | Low: no Switch build; the idea of drawing from the D3D device instead of PM4 |
| Twilight Princess ([dusklight](https://github.com/HayatoG/dusklight), [dusklight-nx](https://github.com/souldbminerr/dusklight-nx)), Melee-NX | decomp + Aurora/Dawn WebGPU on NVK | Low: a WebGPU layer; dusklight-nx says NVK "doesn't perform well" and is moving to deko3d for "2x+" |
| [jak-project-switch](https://github.com/fildicio/jak-project-switch) (OpenGOAL) | source port on OpenGL | Medium as a lesson: CPU-side GL submission was the whole bottleneck (45-67 ms); planning deko3d |
| gta3-nx, 0ad-switch, smashhit_nx, sonic_allstars_nx, DBT-NX | source/Android ports | Low: little performance writing; gta3 has a `full_preload` option (all textures before play, less stutter) |

## The method that made SA2 run at 60 FPS (worth adopting as-is)

1. **One key per optimisation, two master keys** (CPU / non-CPU), a startup line listing what is on, and a use
   counter every 10 s. A missing config key means ON, so an old toml on the SD card cannot silently turn things off.
2. **Every fast path checks itself** against the old path for its first uses (all uses in a verify mode); the
   difference count must stay 0.
3. **Pixel tests in lockstep:** a mode with one vblank per presented frame, so two runs give the same frames; compare
   per-frame hashes against the build before. Gameplay frames are only partly reproducible; judge on menus/fades.
4. **The console decides.** PC/Mac timings mislead on a 1020 MHz A57 with 2 MB L2; compare thread CPU time, not FPS,
   when the frame rate is capped.
5. **Stack-sampling profiler on the console** (every 1 ms, pc + lr + frame chain, symbolized with the build's ELF;
   archive every tested ELF). Use pc-based self time for leaf functions.

## Recommendations for BO1, ranked

Our numbers today (docked, 20 FPS cap): renderer 15-25 ms, GPU 15-20 ms, game thread not measured. **Step 0 is a
console CPU profile**: without it we do not know whether the game thread, the ring thread or the GPU limits 30 FPS.

### A. CPU: the translated game code (all game threads at once)

What our build already has: `skip_msr`, `ctr/xer/reserved/cr_as_local`, `-ffp-contract=off -fno-strict-aliasing`,
`-fno-math-errno -fno-trapping-math`. What it lacks, with the gains others measured:

| # | Change | Measured elsewhere | Notes for BO1 |
|---|---|---|---|
| A1 | **Guest memory without `volatile`** + `REX_LOOP_BARRIER()` at every loop header | masseffect-nx +15-20 % | Mandatory barriers: without them GCC turned 172 polling loops into `b .` (Futari boot hang). Check every ELF for branch-to-self loops |
| A2 | **Direct calls** between translated functions (post-codegen pass) | lets GCC inline; ~178k calls in Futari | Hooked functions must stay indirect; re-run when a hook is added |
| A3 | **No FPCR switches around pure float moves** (`fp_moves_modeless`) | SA2: 20-26 % of the game thread's samples sat right after an `msr fpcr` | FPCR writes are expensive on the A57 |
| A4 | **`elide_lr_stores`** (no `ctx.lr = ...` before every call) | part of the "diet": masseffect-nx −10 % main thread | Our overrides say LR is needed for setjmp/SEH; SA2/Peggle 2 solved it with `lr_keep_callees` and `tools/read_before_write.py` |
| A5 | **`non_volatile_as_local` / `non_argument_as_local`** | masseffect-nx +9 %; Peggle 2 `ctx.r11` uses 549,939 → 13,488 | Needs `share_registers` for the setjmp callers and split SEH fragments (`scan_caller_register_reads.py --with-callers`) |
| A6 | **LTO + `-fipa-pta`**, then **PGO** from one console session | SA2: PGO ≈ +15 % (single player stopped dipping below 60) | PGO needs `-fprofile-info-section` + streaming the counts (games are closed with HOME). With LTO build xxHash with `XXH_FORCE_MEMORY_ACCESS=0` |
| A7 | **Hot functions first** in `.text` (linker order from console samples) | SA2/Unleashed | 55 MB of code vs a 48 KB I-cache; keep `.crt0` first |
| A8 | **Native memcpy/memset** (NEON) for the XDK/CRT versions | Futari: 40 M verified calls | Guarded by a code hash of the guest bytes |
| A9 | **Localized copies / hand natives** of the hottest game functions, each verified against the original while running | SA2: njCalcPoint inline took the largest single cost (8.5 %) away | Only after a profile names them |

### B. CPU: waiting less

| # | Change | Measured | For BO1 |
|---|---|---|---|
| B1 | **D3D's GPU-wait spin → sleep until the ring advances** | Futari: game thread 96 % → 8 % busy (PC), console 96 % → 59 % | Find BO1's XDK wait (8× `db16cyc` then a read-pointer check); the spin steals a core from the other game threads |
| B2 | **The game's own frame-pacing spin loops → sleep** (`while (counter == last) {}` bumped by the vblank callback) | Futari: 78.6 % of the game thread's busy time | Look for the same shape in BO1 |
| B3 | **Relaxed vsync**: give the game its vblank when the flip waits, never sooner than 1/60 s after the last | SA2 PC test: 30 → 54.6 FPS with 18 ms of ring work | Relevant if we go for 30 FPS: a 34 ms frame drops to 20 FPS with a fixed vblank |
| B4 | Log file written by its own thread once a second | Futari: 1 s stutters were the log (SD writes under the sink mutex) | We already have `log_nonblocking`; keep periodic reports off the hot threads |
| B5 | `thread_local` is a function call on the Switch | SA2 | Keep it out of per-draw loops |

### C. Renderer (ring thread)

| # | Change | Measured | For BO1 |
|---|---|---|---|
| C1 | **Fingerprints with the A57's CRC32 instructions** instead of XXH3 | SA2 `fingerprint_mode = 2` | Our texture/vertex hashing is XXH3 |
| C2 | **No per-draw `fmt::format` / string keys** in diagnostics | ~8 % of the ring in masseffect-lineage renderers | Audit our draw path |
| C3 | **Memo of the render-target setup** when nothing changed (EDRAM memo) | ~6-9 % of the ring | We have `setup_cache`; check its hit rate |
| C4 | **Constant uploads only when they changed** / only the registers the shader reads | Unleashed; SA2 (77 % of draws re-uploaded VS constants) | |
| C5 | **Never read back from the upload buffer** (it is uncached NvMap memory) | Futari: 91 % of its ring | Check our vertex conversions |
| C6 | Deferred Vulkan recording on a worker | masseffect-nx: ring 62 → 40 % of a core | **Not pixel-identical** in Futari/SA2 and stalls with read-backs; ours is off (`bo1_native_record_thread`) — keep it off unless lockstep tests pass |
| C7 | Dynamic blend state (EDS) to cut pipeline binds | Futari: 160 binds a frame → 7 | **Not pixel-identical** (212 of 1,098 frames differed); rejected |

### D. GPU and driver (NVK on Maxwell)

Cost model from sa2-nx `gpu-optimisation-candidates.md` §2: **every barrier is a full GPU idle (WFI)**; every legacy
render pass pays one WFI at begin and one plus cache invalidates at end; `vkCmdCopyImage` of colour is a meta draw that
dirties all state; each `vkQueueSubmit` is an IPC (two submits a frame left the GPU idle 3.6 ms in NFSMW).

| # | Change | Measured | For BO1 |
|---|---|---|---|
| D1 | **Mesa: direct draws when the draw parameters are zero + cached memory query** (Simpsons patch 0003, our Mesa commit) | removes a draw macro per draw and a `mallinfo` heap walk under a lock every few dozen allocations | Not in our patch; apply |
| D2 | Mesa: UBO descriptors read at bind time (Simpsons 0001) | project8 Free Skate 2-3 → 7-8 FPS, no device loss | We use `UNIFORM_BUFFER_DYNAMIC`, probably not affected; verify |
| D3 | **ZCULL**: depth images without `TRANSFER_DST` usage get a ZCULL plane | NFSMW ~2 ms | All our targets have `TRANSFER_SRC|DST`; depth targets need another path for their copies |
| D4 | **Fewer passes and barriers**: clears inside the pass (`vkCmdClearAttachments`, no WFI), batch barriers, narrow end dependencies | Unleashed 60 → 37 barrier batches per frame | We run ~16 passes a frame |
| D5 | **Resolves only when read**: drop resolves overwritten before any read, hand a cleared target's image over instead of copying | Unleashed: 24 of 26 copies became hand-overs; resolve copies were 5.8 ms of the frame | |
| D6 | **Depth-only draws without a fragment stage** when the pixel shader has no effect | Unleashed | Our depth prepass (~660 draws) and the shadow passes |
| D7 | **Skip draws that write no colour and no depth** | Unleashed | We already skip them (reason 7) |
| D8 | Vertex outputs the pixel shader never reads not computed; colour channels nothing writes not computed | Unleashed; NVK links varyings already for monolithic pipelines | |
| D9 | One `vkQueueSubmit` per frame | NFSMW | We make ~1.5 a frame |
| D10 | **Render scale** (e.g. 960x544 handheld) | SA2 ships 720p handheld / 1080p docked switching live | Our planned item; GPU-bound scenes |
| D11 | Two or more prewarm threads | Banjo: 23 % of the ring in newlib's malloc lock | Keep one pipeline worker |

### E. Flicker: what the others learned

- **Turning a game system off can leave shaders reading stale data** (our `sm_enable 0` case): shrink it instead.
- **Texture fetch exponent bias** must be applied (FXAA 360 averages two biased fetches; ignored, edges flicker).
  We apply the resolve exponent bias; check the *fetch constant's* bias too.
- A self-check that reads guest memory twice reports false differences on dynamic buffers; snapshot once.
- Deferred recording and dynamic blend state changed pixels in lockstep tests although their own checks passed:
  pixel tests, not only self-checks.

## Second pass (2026-10-15 evening): PC ports and two more 360 recompilations

| Port | Approach | What is worth taking |
|---|---|---|
| [MarathonRecomp-NX](https://github.com/ChanseyIsTheBest/MarathonRecomp-NX) (Sonic 2006, a heavy 360 game) | XenonRecomp + D3D-level renderer; `docs/SWITCH-OPTIMISATIONS.md` catalogues ~100 changes | **Kernel layer:** guest locks that sleep on the owner word (`svcWaitForAddress`) instead of yielding (a yield never lets a lower-priority owner run on that core); events/semaphores that skip their wake-up call when nobody waits; ~2 µs of spinning before sleeping; the game's job workers and sound thread moved **off core 0** where the game thread runs; a frame limiter that sleeps to the deadline instead of yield-spinning. Measured: CPU-bound spot 47 → 60 FPS over four rounds (game thread 20.2 → 16.1 ms) |
| [sh2-nx](https://github.com/cutarev/sh2-nx), sh3-nx, sh4-nx (Silent Hill 2/3/4, PC) | x86 lifted; Direct3D 8 on OpenGL 4.3 core | **Screen-sized render targets at fewer lines:** `handheld=480`, `docked=720` (the game keeps its 720p layout, the targets are drawn smaller and scaled). Shader programs prepared ahead (stutter at first draw otherwise). Guest threads on one core by default because a CRI audio bug loops the music otherwise |
| [outrun2-decomp](https://github.com/r4dius/outrun2-decomp) (OutRun 2006, PC) | decomp | Renders with **deko3d** (Nintendo's native-style API) instead of NVK |
| dusklight-nx (Twilight Princess) | WebGPU on NVK | Moving to deko3d, expecting "2x+"; nfsmw-nx also has a deko3d renderer in a PR |
| openPREYswitch (id Tech 4), openmw-nx, GZDoom-NX-2026, AvP Gold | OpenGL / GLES on Mesa | Little performance writing; GL ports are CPU-bound on driver submission (Jak measured 45-67 ms of GL submission per frame) |
| torchlight-nx (360) | ReXGlue; notes in French | Choosing the interception level: PM4 ring (ours) or the engine's own render system (Ogre's D3D9RenderSystem), where shaders exist as HLSL |

### What the profile of BO1 itself showed (Mac, `sample`, Kino, 2026-10-15)

- The game's **main thread spent ~70 % of its samples inside `NtPulseEvent`**: the SDK implemented a pulse as signal,
  yield, 10 µs sleep, reset, and BO1's job queue (`sub_82545A00` / `sub_82544D40`) pulses an event for each job it
  adds. Fixed exactly (`event_pulse_fast`, on): no waiter → reset only; auto-reset event with waiters → one signal the
  first waiter consumes. After it the main thread waits in a real wait (8,917 of 9,867 samples) and the game runs as
  before. On the Switch this was likely a large part of the main thread's frame.
- The render backend thread polls `WaitForSingleObject` with a zero timeout, and each poll yields
  (`XObject::Wait` → `MaybeYield`). On Horizon a yield only gives way to threads of the same priority. Candidate:
  short sleeps for repeated zero-timeout polls; measure on the console first.
- The job workers now spend their time in the game's own job-queue code (`sub_82545308`, `sub_825450A8`): next
  candidate for a console profile (spinning workers on the main thread's core would cost it time; Marathon moved its
  workers off core 0).

### Prepared in this pass (2026-10-15)

- `event_pulse_fast` (SDK, on by default).
- `tools/codegen_post.py` + CMake option `BO1_GUEST_PLAIN_MEMORY` (A1 above): guest memory without volatile, with a
  compiler barrier at all 13,048 loop headers. Off by default; NRO built in `out/nx-fast`; checked with
  `tools/find_self_loops.sh` (the default ELF has one self-branch in game code, a genuine halt in `sub_82507548`).
- `mesa/mesa-switch-p8-direct-draws.patch` (D1 above, merged by hand onto our Mesa patch), opt-in with
  `P8_DIRECT_DRAWS=1`; not built yet.

## Proposed order for BO1

1. Console CPU profile in Kino (stack sampler): which thread limits, and its top functions.
2. B1/B2 (the waits), if the profile shows the spin.
3. A1-A3 + A2 as one codegen round (with the loop-barrier and branch-to-self checks), then A6 (LTO, then PGO).
4. D1 (Mesa patch), D3 (ZCULL), D5/D6 (resolves, depth-only), D10 (960x544) for the GPU side.
5. C1-C5 on the ring, guided by the profile.

## Third pass (2026-10-16): the console profile, the GTA V toolbox, Mass Effect's recording thread

### What the console stack sampler showed (Kino, shadows off, 20 FPS cap)

The SDK profiler now runs from the command line (`--bo1_profile_stacks=true --bo1_profile_log=true`) and its reports
come back in the Wi-Fi log. Two lessons about the tool: `open_memstream` froze the game on Horizon (the report is
written to the SD file and read back instead), and the sampled call chains are not reliable past the first frame
(the build keeps no frame pointers): use the `pc` distribution and treat `lr` and `stack` lines with care.

- **Job queue spin.** All three cores sat at 300 %. JQ Worker 1/2 (~50-67 % each) re-ran two persistent jobs
  (callbacks `sub_825BBFD0` / `sub_825BC040`, both "not finished" while `sub_825BBB88()` is non-zero): the worker loop
  `sub_82545308` puts an unfinished job back on its list (`sub_82546470` from 0x8254551C) and `sub_825450A8` hands it
  straight out again, ~13,000-16,000 times a second. Throttled (`bo1_job_worker_sleep_us`): workers ~14-20 % each.
  The helpers' "run one job" (`sub_82545618`, used by Backend's wait loop) then took the spin over: Backend went from
  ~20-28 % to ~50-77 %. Its requeue (from 0x8254578C) is now throttled too (console test pending).
- **Renderer thread (`GPU ring native`, ~40-50 % CPU, ~55 % of its samples waiting).** Top functions by `pc`:
  `XXH3_hashLong` 7.6 %, `memcmp` 7.5 %, `memmove` 7.4 %, `Draw` 7.3 %, `Packet` 5.7 %, `nvk_bind_descriptor_sets`
  3.3 %, `Type3Packet` 2.6 %, `RangeHash` 2.4 %, NVK cbuf/dynamic-state work ~6 %. Hashing, compare and copy are
  ~22 % of all its samples (~40 % of its busy time). The callers are not known yet (see the call-chain caveat):
  candidates are texture change detection (dynamic data stays on full hashes every frame since the flicker fix),
  vertex range hashing and the upload copies. Next step: per-site byte counters (cheap, exact) instead of sampling.

### GTA V Switch toolbox (niceboat0/GTA5-Switch-Toolbox, read 2026-10-16)

A mod / settings manager for the GTA V homebrew port (the port's own source is not public). The GTA V port uses the
PC game's `settings.xml` (DX_Version, MSAA, shadow and reflection quality), so it is a PC-build port, not a 360
recompilation. What its tuning presets record (from the header comments, translated):

- Shrinking memory / model-info pools makes it **more** stuttery (full pools force constant unload and reload). Same
  lesson as our texture streaming: cut the work, not the budget.
- What saves CPU: entity density multipliers, entity count limits, population update rates and cull ranges.
  Population density measured as the single biggest knob (~11 % between 0 and 10 of 10).
- BO1's equivalents, present as dvars in the game image: `ai_corpseCount`, `cg_bloodLimit`, `fx_cull_elem_draw`,
  `fx_cull_elem_spawn`, `fx_cull_effect_spawn`, `fx_marks`, `dynEnt_spawnedLimit`, `ragdoll_max_simulating`,
  `r_lodScaleRigid` / `r_lodScaleSkinned`, `r_lodBiasRigid` / `r_lodBiasSkinned`, `r_zfar`. Candidates for the hounds
  round (effects-heavy); defaults still to read from the dvar registration.

### Mass Effect's deferred recording (refs/masseffect-nx/docs/nvk-per-draw.md)

With deferred recording on, every `vkCmd*` on their ring thread only queues the call; a worker (0x2C, core 0) replays
the queue into NVK. NVK's own cost was ~4-5.5 µs per recorded draw (draw + state flush 1.8-2.8, pipeline bind
0.75-1.4, descriptor sets 0.95-1.5); the ring's remaining per-draw work was the renderer's own (~16-25 µs per recorded
draw before their later cuts). Our equivalent, `bo1_native_record_thread`, replays the same draw commands in the same
order (anything else recording drains it first), so the "not pixel-identical" problem of Futari/SA2's approach should
not apply. It was off because the three cores were saturated; with the job queue spin gone there is a core to give
it. Candidate for a console A/B.

### Proposed order (updated)

1. Console test of the open builds: prepass skip + helper requeue throttle (built 2026-10-16).
2. Per-site byte/time counters for hashing, compares and copies on the renderer thread; cut the largest.
3. `bo1_native_record_thread` A/B now that a core is free.
4. Hounds round: effect / corpse / LOD dvars (later, as the user asked).
5. 960x544, ZCULL, then the codegen round (direct calls, FPCR moves, LTO/PGO).

## Fourth pass (2026-10-16): review of the ChatGPT deep-research report

`docs/black_ops_switch_performance_research.md` (ChatGPT, from the prompt in this session). It marks estimates as
estimates and invents no benchmarks, but most of it is general method. Checked against our measurements:

- **Instrument the per-draw path by stage** (its #1): agreed, the most useful item. The profiler's pc/lr pairs are
  the first step; next a per-stage stopwatch on the renderer thread (Mass Effect's `C6 stages` is the model).
- **Dirty-state tracking / skip redundant binds and constant writes** (#2): largely in place (setup cache, bound
  viewport/scissor/blend/bias checks, NVK's "CB0 only when changed" is in our Mesa pin). Add counters of requested
  versus emitted state changes to confirm.
- **Suppress the shadow system's extra draw lists upstream** (#3): right diagnosis (with `sm_enable 1` the game
  issues ~2x draws that are not shadow maps), but it only matters for the `min` preset; the user plays with shadows
  off. Low priority.
- **Dynamic resolution at 960x544 → 640x360** (#4): the premise is wrong (we render 1040x608; 960x544 was a plan
  item), and in a PM4 translator the game's EDRAM-sized targets make dynamic resolution a large job. The cheap GPU
  levers now are `bo1_native_msaa=false` and `bo1_native_zcull=true`.
- **One vs two frames in flight; modest priority/affinity changes** (#5): worth one test for the input lag felt with
  shadows on. The SDK already manages priorities; the busy-wait fixes came from this kind of measurement.
- **VK_EXT_multi_draw, instancing, reused command buffers**: low value until a capture shows long runs of
  state-identical consecutive draws (few in BO1's stream so far).
- **deko3d**: same conclusion as before (benchmark before any rewrite).
- Corrections: "Mesa 26.1 disables large pages" is about NVK GPU pages, not the SDK's CPU-side
  `guest_memory_large_pages`. Two of its links (a "techoverflow" multi-draw article and a personal `Xenon-Recomp`
  repository) could not be verified as primary sources.

Added to the checklist from it: per-stage renderer stopwatch, requested-vs-emitted state counters, frames-in-flight test.

`docs/deep-research-report.md` is a shorter, earlier draft of the same research. Nothing new, and several claims are
wrong for this port: multi-draw "cutting draw overhead 50-75 %" assumes the cost is the Vulkan call (our profile puts
most of it in our own translation work); "update to Mesa 26.x" (already 26.2.3); "800x450 halves the pixels" (31 %
fewer) at an assumed 960x544 (we render 1040x608); "render thread above 0x3B, real-time" misreads Horizon priorities
(lower number = higher; ours is 0x2C); deko3d "avoids validation layers" (none are active). Only the "turn off costly
effects through dvars" point stands, already on the checklist (hounds round, later).

`docs/deep-research-report-2.md` (ChatGPT, dvar documentation for T5). The defaults we could check against the game's
own registration (`sub_8258CDA8`) match: `sm_maxLights` 4, `r_spotLightEntityShadows` / `r_spotLightSModelShadows` 1,
`sm_sunShadowSmallEnable` 0, `sm_strictCull` 1, `sm_fastSunShadow` 1. Its answer to "why ~2x draws with sm_enable 1"
(extra lighting passes per light) is inferred and **does not match a Mac measurement** (Kino, 2026-10-16, prepass not
skipped): with `min` the game issues ~905k draws per 10 s against ~326k with `off`, and ~580k of them are shadow-map
draws that `min` skips (reason 15), so the draws rendered are about the same (~310k vs ~326k). The extra cost of the
shadow system is the game building, and the renderer parsing, shadow-map caster draws that are thrown away. Its levers
for that (fewer shadowed lights, no entity/static-model casters, small sun map) are what `min` already sets;
`sm_sunEnable`, `sm_spotEnable` and `r_spotLightShadows` are documented but did nothing in this build's frame trace.
Useful for the hounds round: `fx_cull_effect_spawn` (default 0 → 1), `fx_drawClouds`, `r_glow`.
New lead: shrink the sun shadow range further for `min` (`sm_sunShadowScale` < 0.25, `sm_sunSampleSizeNear` < 0.25) so
the game stops generating the caster draws `min` discards.

## Fifth pass (2026-10-17): Prototype and Prototype 2 (Xbox 360, ReXGlue)

Sources: [fungalspores/prototype-switch](https://github.com/fungalspores/prototype-switch) (Prototype, and a rebuild
of Prototype 2; created 2026-10-15, GPL-3.0) and [penettrator/prototype2-nx](https://github.com/penettrator/prototype2-nx)
(Prototype 2 0.2.0, GPL-3.0). A Reddit thread about a Prototype free-roam test with an overclock could not be read
(Reddit blocks automated access). Read for ideas only; no code taken.

**Why they matter:** the closest ports to ours so far. Same stack: ReXGlue, the masseffect-nx SDK with its Horizon
layer, a native renderer from the project8-sw lineage, and mesa-switch at the same commit as ours (`d4a00ea0ab3`).
Both play. Reported FPS: Prototype 25-30 in gameplay, Prototype 2 25-30 in the city, but on an OLED (Mariko) overclocked
to CPU 1785 / GPU 768-1152 / RAM 2333 MHz; "lower at stock clocks". Prototype 2 renders at 960x544 (its own release:
800x448) and upscales with FSR 1.

What is new for us, roughly by expected value:

| Idea | What they did | Relevance to BO1 |
|---|---|---|
| **Fourth CPU core** | The homebrew loader's NPDM allows cores 0-2 only. Their HOME-menu NSP forwarder (Forwarder-Mod) sets `highest_cpu_id` 3 in both permission tables (ACID and ACI0); `tools/patch_hbl_core3.py` makes the same 2-byte change to one verified `hbl.nsp`. Thread masks then spread the game: guest threads 0-3, main thread 1-3, audio engine on core 3 only, host threads 1-2. "System services also use core 3; permission does not grant exclusive ownership." | Large potential: our process has 3 cores (profiler: mask 0x7) and they run at 240-298 % of 300 %. Needs either a patched `atmosphere/hbl.nsp` on the user's SD or an NSP forwarder built with the user's own `prod.keys`; both are the user's call. Then put the audio/XMA/background threads on core 3. |
| **Main-thread priority** | Profiling showed the game's main loop got ~70 % of a core while ~16 job workers shared its priority 0x3B. They raise that one thread to 0x3A (below the host present/audio threads at 0x2C/0x2B) and pin it to core 0, away from host threads. | Same shape as BO1 (Main XThread at 0x3B beside the JQ workers and Backend). Risk: at 0x3A the thread is no longer time-sliced, and its job-wait loop yields; a yield only gives way to the same priority, so a spinning wait could starve the 0x3B workers on that core. Test with the job throttle in place. |
| **Immediate guest flip** | The game's D3D scheduled each flip for "last flip + present interval" or the next vblank when late, so 35-60 ms of work became 50/67/83 ms frames. They flip at the current vblank and let the host presenter pace the screen, with a minimum interval of 2 vblanks. | Worth checking: our slow-frame lines report "game's vblank wait", usually 0 ms, so BO1 may not wait there. Look at its swap/flip path before assuming. |
| **Hash less in the renderer** | Changing buffers are copied once per frame with no hash first; ranges that stopped changing (≥16 KB) are checked by samples plus a full hash every 16 frames; small ranges unchanged for 300 frames are hashed only every 4th frame; ranges on pages the skinning code just wrote get a new identity instead of a hash. | Directly aimed at our renderer's hot spot: ~40 % of the ring thread in XXH3/memcmp/memmove. Correctness risk (a change seen up to N-1 frames late), so keep exact-check switches. |
| **PGO that survives HOME exits** | An instrumented build dumps the gcov counters to `sdmc:/pgo` every 3 minutes (`__gcov_dump` + `__gcov_reset` on a helper thread); any 3+ minute session leaves a profile. Arc counts only (`-fno-profile-values`), same GCC (15.2) and build-directory layout for GEN and USE; 222 profiles cover guest and native code. Training builds stall, so never benchmark them. | Solves our noted PGO blocker (the game is closed with HOME, so counters never flush at exit). Goes with guest LTO, which needs more RAM than this Mac's Docker has (~16 GB for LTO links). |
| **Codegen** | `cr/non_volatile/xer/ctr_as_local` with `share_registers` for guest setjmp, the fiber switch and a non-ABI helper pair (found with masseffect-nx's `read_before_write.py`); direct calls, guest LTO, leaf locals, integer VMX on NEON (`P2_ARM_VMX`). Per-address locks for lwarx/stwcx. regions instead of one process-wide mutex. | Known items (checklist "Codegen"). New detail: how they made `non_volatile_as_local` safe with setjmp, our blocker (`Com_Error`). Check how our SDK serializes reservations. |
| **GPU bandwidth** | 32-bit depth instead of D32F_S8 (64 bpp on NVIDIA); HDR scene as R11G11B10; colour resolves and the shadow atlas done with fragment passes instead of compute, because every switch between drawing and compute idles this GPU; far shadow cascades capped or redrawn on alternate frames. | `bo1_native_depth24` exists but was never tested on the console: a cheap A/B. BO1's main colour target is 8_8_8_8, so R11G11B10 does not apply. Check whether our resolves alternate compute and graphics. |
| **Native hot routine with verification** | The job workers' culling routine (5.8 % of a core) rewritten with NEON; the first 256 calls run both versions and compare every result, and a mismatch turns the native path off. | The pattern for any BO1 hot function we replace. |
| **Hangs and saves** | Main thread holding critical sections while waiting for the render thread to drain a full command ring: it now releases them while it waits. `XFile::Write`/`SetLength` returned the size cached at open (0), so saves looked corrupt. Console signature stubs (`XeKeysConsolePrivateKeySign`, `...SignatureVerification`) returned "invalid". | Check our SDK for the same `XFile` size bug and the XeKeys stubs before testing BO1 saves. |
| **Horizon OC crash** | An open temperature-sensor (`ts`) session for the whole game coincided with Horizon OC's `hoc:clk` crashing under load. They stopped polling it. | Check our SDK's clock/temperature code (`switch_sysclk.cpp`). |
| **Release packaging** | NSP forwarders on the HOME menu; `rex-start`, a small libnx launcher that shows the game for 5 s and offers a settings screen writing straight into the game's TOML; release builds without stack/task audits or the SDK profiler. | Later, for a public release. |

Their other settings match ours in spirit: MSAA off, recording thread on, pipeline prewarm, async pipelines, a
depth-prepass skip (LESS/LEQUAL, theirs) and per-pass GPU profiling.
